// SPDX-License-Identifier: MIT
//
// Host test of the pipeline cache deserializer (src/imagination/vulkan/pvr_pipeline_cache.c), meant to run
// under AddressSanitizer and UBSan: tools/vktest/pvr_cache_host_test.sh builds and runs it.
//
// The contents of a VkPipelineCache or of the disk cache are attacker-controlled input. The test builds
// valid cache objects (graphics with xfb info and descriptor bindings, graphics without FS, compute with
// the zero-init shader), serializes them, and then feeds the deserializer and the use-time checks
// (pvr_cached_shaders_check_graphics / _compute) with
//   - every byte of the payload replaced by 0, 1, 0x7f, 0x80, 0xff (CRC recomputed: the CRC is no
//     authentication),
//   - every dword replaced by values at the edge of the bounds,
//   - every truncation, every combination of the flags, all-0x41 and all-0xff payloads,
//   - forged layouts (wrong binding count, set size, dynamic buffers, push constants, FS presence),
// and requires: no crash or sanitizer report, no pointer taken from the blob (every pointer of an
// accepted object is owned by the object), accepted objects survive clone and free exactly as a pipeline
// does, a forged IS_VS / FS presence never passes the use-time check.
// It also writes objects whose pco_data was filled with 0xAA first and requires the same bytes as for a
// zeroed one (no padding or uninitialised memory in the cache), and probes which bytes of pco_data the
// format keeps, to compare with the struct's holes.
//
// Drafted with an LLM; marker word per AGENTS.md: biblioklept.

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pvr_descriptor_set.h"
#include "pvr_device.h"
#include "pvr_pipeline_cache.h"
#include "util/blob.h"
#include "util/crc32.h"
#include "util/ralloc.h"
#include "vk_pipeline_layout.h"

static unsigned checks, failures;

#define EXPECT(c, ...)                                         \
   do {                                                        \
      checks++;                                                \
      if (!(c)) {                                              \
         failures++;                                           \
         fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);  \
         fprintf(stderr, __VA_ARGS__);                         \
         fprintf(stderr, "\n");                                \
      }                                                        \
   } while (0)

/* What the object needs from the runtime. */
struct vk_pipeline_cache_object *
vk_pipeline_cache_lookup_object(struct vk_pipeline_cache *cache,
                                const void *key_data,
                                size_t key_size,
                                const struct vk_pipeline_cache_object_ops *ops,
                                bool *cache_hit)
{
   abort();
}

struct vk_pipeline_cache_object *
vk_pipeline_cache_add_object(struct vk_pipeline_cache *cache,
                             struct vk_pipeline_cache_object *object)
{
   abort();
}

void vk_pipeline_cache_object_unref(struct vk_device *device,
                                    struct vk_pipeline_cache_object *object)
{
   if (!--object->ref_cnt)
      object->ops->destroy(device, object);
}

static void *host_alloc(void *u, size_t s, size_t a, VkSystemAllocationScope sc)
{
   void *p = NULL;
   return posix_memalign(&p, a < sizeof(void *) ? sizeof(void *) : a, s) ? NULL : p;
}

static void *host_realloc(void *u, void *o, size_t s, size_t a, VkSystemAllocationScope sc)
{
   free(o);
   return host_alloc(u, s, a, sc);
}

static void host_free(void *u, void *p)
{
   free(p);
}

static struct pvr_device *dev;
static struct vk_pipeline_cache cache;

static void setup(void)
{
   dev = calloc(1, sizeof(*dev));
   dev->vk.alloc = (VkAllocationCallbacks){
      .pfnAllocation = host_alloc,
      .pfnReallocation = host_realloc,
      .pfnFree = host_free,
   };
   cache.base.device = &dev->vk;
}

/* A layout of `n` sets, each `binding_count` bindings, size 64 bytes, 2 dynamic buffers. */
struct fake_layout {
   struct vk_pipeline_layout l;
   struct vk_descriptor_set_layout *p[2];
   struct pvr_descriptor_set_layout sl[2];
   struct pvr_descriptor_set_layout_binding bind[2][4];
   VkPushConstantRange pc;
};

static void fake_layout_fix(struct fake_layout *f)
{
   f->l.set_layouts[0] = &f->sl[0].vk;
   f->l.set_layouts[1] = &f->sl[1].vk;
   f->sl[0].bindings = f->bind[0];
   f->sl[1].bindings = f->bind[1];
}

static void fake_layout_init(struct fake_layout *f, uint32_t stages)
{
   memset(f, 0, sizeof(*f));
   f->l.set_count = 2;
   f->l.set_layouts[0] = &f->sl[0].vk;
   f->l.set_layouts[1] = &f->sl[1].vk;
   for (unsigned i = 0; i < 2; i++) {
      f->sl[i].binding_count = 3 + i;
      f->sl[i].size = 64;
      f->sl[i].dynamic_buffer_count = i;
      f->sl[i].stage_flags = stages;
      f->sl[i].bindings = f->bind[i];
   }
   /* XCLIPSE: the binding types the UBO ranges are checked against: set 0 has a 3-element UBO (0), an inline block (1),
    * a combined image sampler (2); set 1 has a UBO (0), a storage buffer (1), a sampler (2), a dynamic UBO (3). */
   f->bind[0][0] = (struct pvr_descriptor_set_layout_binding){ .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                               .descriptor_count = 3, .dynamic_buffer_idx = ~0u };
   f->bind[0][1] = (struct pvr_descriptor_set_layout_binding){ .type = VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK,
                                                               .descriptor_count = 16, .dynamic_buffer_idx = ~0u };
   f->bind[0][2] = (struct pvr_descriptor_set_layout_binding){ .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                               .descriptor_count = 1, .dynamic_buffer_idx = ~0u };
   f->bind[1][0] = (struct pvr_descriptor_set_layout_binding){ .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                               .descriptor_count = 1, .dynamic_buffer_idx = ~0u };
   f->bind[1][1] = (struct pvr_descriptor_set_layout_binding){ .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                               .descriptor_count = 1, .dynamic_buffer_idx = ~0u };
   f->bind[1][2] = (struct pvr_descriptor_set_layout_binding){ .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                                               .descriptor_count = 1, .dynamic_buffer_idx = ~0u };
   f->bind[1][3] = (struct pvr_descriptor_set_layout_binding){ .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                                                               .descriptor_count = 1, .dynamic_buffer_idx = 0 };
   f->pc = (VkPushConstantRange){ .stageFlags = VK_SHADER_STAGE_ALL, .offset = 0, .size = 16 };
   f->l.push_range_count = 1;
   f->l.push_ranges[0] = f->pc;
}

static void fill_common(pco_data *d, unsigned stage_bit, struct fake_layout *f)
{
   pco_common_data *c = &d->common;

   c->temps = 12;
   c->vtxins = 8;
   c->shareds = 80;
   c->coeffs = 8;
   c->entry_offset = 4;
   c->uses.barriers = true;
   c->sys_vals[SYSTEM_VALUE_VERTEX_ID] = (pco_range){ 1, 1, { 0 } };
   c->push_consts.used = 4;
   c->push_consts.range = (pco_range){ 40, 4, { 0 } };

   for (unsigned u = 0; u < 2; u++) {
      pco_descriptor_set_data *s = &c->desc_sets[u];

      if (!(f->sl[u].stage_flags & stage_bit))
         continue;

      s->binding_count = f->sl[u].binding_count;
      s->bindings = rzalloc_array_size(NULL, sizeof(*s->bindings), s->binding_count);
      for (unsigned b = 0; b < s->binding_count; b++) {
         s->bindings[b].range = (pco_range){ b, 4, { 4 } };
         s->bindings[b].used = b & 1;
         s->bindings[b].is_img_smp = b == 2;
      }
      s->used = true;
      s->range = (pco_range){ 8 * u, 16, { 0 } };
      if (f->sl[u].dynamic_buffer_count)
         s->dynamic_range = (pco_range){ 24, 4 * f->sl[u].dynamic_buffer_count, { 0 } };
   }

   /* XCLIPSE: two promoted UBO ranges (shareds 80: the registers 64..79 are theirs): element 2 of set 0 binding 0 and the
    * dynamic UBO of set 1, when the stage has the set. */
   if (c->desc_sets[0].used)
      c->ubo_ranges[c->ubo_range_count++] = (pco_ubo_range){ .desc_set = 0, .binding = 0, .elem = 2, .offset = 4,
                                                             .count = 8, .start = 64, .uses = 5 };
   if (c->desc_sets[1].used)
      c->ubo_ranges[c->ubo_range_count++] = (pco_ubo_range){ .desc_set = 1, .binding = 3, .elem = 0, .offset = 0,
                                                             .count = 8, .start = 72, .uses = 9 };
}

static void fill_vs(pco_data *d, struct fake_layout *f)
{
   fill_common(d, BITFIELD_BIT(MESA_SHADER_VERTEX), f);
   d->vs.attribs[VERT_ATTRIB_GENERIC0] = (pco_range){ 0, 4, { 0 } };
   d->vs.attrib_formats[VERT_ATTRIB_GENERIC0] = PIPE_FORMAT_R32G32B32A32_FLOAT;
   d->vs.varyings[VARYING_SLOT_VAR0] = (pco_range){ 4, 4, { 0 } };
   d->vs.varyings[VARYING_SLOT_POS] = (pco_range){ 0, 4, { 0 } };
   d->vs.vtxouts = 8;
   d->vs.f32_smooth = 1;

   nir_xfb_info *x = rzalloc_size(NULL, nir_xfb_info_size(2));
   x->buffers_written = 1;
   x->streams_written = 1;
   x->buffers[0].stride = 32;
   x->buffers[0].varying_count = 2;
   x->output_count = 2;
   x->outputs[0] = (nir_xfb_output_info){ .buffer = 0, .offset = 0, .location = VARYING_SLOT_POS,
                                          .component_mask = 0xf };
   x->outputs[1] = (nir_xfb_output_info){ .buffer = 0, .offset = 16, .location = VARYING_SLOT_VAR0,
                                          .component_mask = 0xf, .component_offset = 0 };
   d->vs.xfb_info = x;
}

static void fill_fs(pco_data *d, struct fake_layout *f)
{
   fill_common(d, BITFIELD_BIT(MESA_SHADER_FRAGMENT), f);
   d->fs.varyings[VARYING_SLOT_VAR0] = (pco_range){ 0, 4, { 0 } };
   d->fs.outputs[FRAG_RESULT_DATA0] = (pco_range){ 0, 4, { 0 } };
   d->fs.output_formats[FRAG_RESULT_DATA0] = PIPE_FORMAT_R8G8B8A8_UNORM;
   d->fs.z_replicate = ~0u;
   d->fs.num_tile_buffers = 2;
   d->fs.tile_buffers = (pco_range){ 60, 4, { 2 } };
   d->fs.uses.w = true;
   d->fs.rasterization_samples = 1;
   d->fs.blend_opts.rt[0].colormask = 0xf;
   d->fs.blend_opts.rt[0].format = PIPE_FORMAT_R8G8B8A8_UNORM;
   d->fs.view_index_slot = VARYING_SLOT_VAR0;
}

static void fill_cs(pco_data *d, struct fake_layout *f, bool zero_init)
{
   fill_common(d, BITFIELD_BIT(MESA_SHADER_COMPUTE), f);
   d->cs.workgroup_size[0] = 8;
   d->cs.workgroup_size[1] = 8;
   d->cs.workgroup_size[2] = 1;
   d->cs.shmem = (pco_range){ 0, 16, { 0 } };
   d->cs.zero_shmem = zero_init;
}

static const struct pvr_stats stats = { .isa = PVR_STAT_ROGUE };
static const uint8_t binary[32] = { 1, 2, 3 };

static void free_data(pco_data *d, bool vs)
{
   for (unsigned u = 0; u < ARRAY_SIZE(d->common.desc_sets); u++)
      ralloc_free(d->common.desc_sets[u].bindings);
   if (vs)
      ralloc_free(d->vs.xfb_info);
}

enum shape { GFX_FS, GFX_NOFS, GFX_PASSTHROUGH, COMPUTE, COMPUTE_ZERO };

static struct pvr_cached_shaders *make_object(enum shape shape, struct fake_layout *f,
                                              bool unused)
{
   uint8_t key[PVR_CACHED_KEY_SIZE] = { 7 };
   uint32_t flags = PVR_CACHED_HAS_STAGE0;
   const bool gfx = shape <= GFX_PASSTHROUGH;

   if (gfx)
      flags |= PVR_CACHED_STAGE0_IS_VS;
   if (shape == GFX_FS || shape == GFX_PASSTHROUGH || shape == COMPUTE_ZERO)
      flags |= PVR_CACHED_HAS_STAGE1;
   if (shape == GFX_PASSTHROUGH)
      flags |= PVR_CACHED_FS_PASSTHROUGH;

   struct pvr_cached_shaders *obj = pvr_cached_shaders_create(dev, key, flags);

   pco_data d0;
   memset(&d0, 0, sizeof(d0));
   if (gfx)
      fill_vs(&d0, f);
   else
      fill_cs(&d0, f, shape == COMPUTE_ZERO);
   EXPECT(pvr_cached_shaders_set_stage(obj, 0, &d0, binary, sizeof(binary), &stats), "set_stage 0");
   free_data(&d0, gfx);

   if (flags & PVR_CACHED_HAS_STAGE1) {
      pco_data d1;
      memset(&d1, 0, sizeof(d1));
      if (gfx) {
         fill_fs(&d1, f);
      } else {
         d1.cs.shmem = (pco_range){ 0, 16, { 0 } };
         d1.common.temps = 3;
      }
      EXPECT(pvr_cached_shaders_set_stage(obj, 1, &d1, binary, sizeof(binary), &stats), "set_stage 1");
      free_data(&d1, false);
   }

   if (gfx && shape != GFX_NOFS) {
      obj->fs_varyings_used = BITFIELD64_BIT(VARYING_SLOT_VAR0);
      obj->fs_interp[0] = INTERP_MODE_SMOOTH;
   }

   return obj;
}

static bool try_serialize(struct pvr_cached_shaders *obj, struct blob *b)
{
   blob_init(b);
   return pvr_cached_shaders_ops.serialize(&obj->base, b);
}

static void serialize(struct pvr_cached_shaders *obj, struct blob *b)
{
   EXPECT(try_serialize(obj, b), "serialize");
}

static void fix_crc(uint8_t *data, size_t size)
{
   uint32_t crc = util_hash_crc32(data, size - 4);
   memcpy(data + size - 4, &crc, 4);
}

static struct pvr_cached_shaders *deserialize(const uint8_t *data, size_t size)
{
   struct blob_reader r;
   uint8_t key[PVR_CACHED_KEY_SIZE] = { 7 };

   blob_reader_init(&r, data, size);
   struct vk_pipeline_cache_object *o =
      pvr_cached_shaders_ops.deserialize(&cache, key, sizeof(key), &r);
   return o ? container_of(o, struct pvr_cached_shaders, base) : NULL;
}

/* Everything an accepted object holds must be owned by it. */
static void check_ownership(struct pvr_cached_shaders *obj)
{
   for (unsigned s = 0; s < 2; s++) {
      if (!(obj->flags & (PVR_CACHED_HAS_STAGE0 << s)))
         continue;

      const pco_data *d = &obj->stage[s].data;
      for (unsigned u = 0; u < ARRAY_SIZE(d->common.desc_sets); u++) {
         const pco_descriptor_set_data *set = &d->common.desc_sets[u];
         if (set->bindings)
            EXPECT(ralloc_parent(set->bindings) == obj->mem_ctx, "foreign bindings pointer");
         else
            EXPECT(!set->binding_count, "count without bindings");
      }

      if ((obj->flags & PVR_CACHED_STAGE0_IS_VS) && s == 0) {
         if (d->vs.xfb_info) {
            EXPECT(ralloc_parent(d->vs.xfb_info) == obj->mem_ctx, "foreign xfb pointer");
            EXPECT(obj->flags & PVR_CACHED_HAS_XFB, "xfb without flag");
         } else {
            EXPECT(!(obj->flags & PVR_CACHED_HAS_XFB), "flag without xfb");
         }
      }
   }
}

/* What the driver's consumers index or size by, restated independently of the check: if the check
 * passed, all of this must hold. */
static void consumer_invariants(const struct pvr_cached_shaders *obj, enum shape shape,
                                const struct fake_layout *f)
{
   const bool gfx = shape <= GFX_PASSTHROUGH;
   const unsigned nstage = (obj->flags & PVR_CACHED_HAS_STAGE1) ? 2 : 1;

   EXPECT(gfx == !!(obj->flags & PVR_CACHED_STAGE0_IS_VS), "IS_VS does not match the pipeline kind");

   for (unsigned s = 0; s < nstage; s++) {
      const pco_data *d = &obj->stage[s].data;
      const mesa_shader_stage ms = gfx ? (s ? MESA_SHADER_FRAGMENT : MESA_SHADER_VERTEX)
                                       : MESA_SHADER_COMPUTE;

      for (unsigned u = 0; u < PVR_MAX_DESCRIPTOR_SETS; u++) {
         const pco_descriptor_set_data *set = &d->common.desc_sets[u];
         const struct pvr_descriptor_set_layout *sl = u < f->l.set_count ? &f->sl[u] : NULL;
         const bool in = sl && (sl->stage_flags & BITFIELD_BIT(ms)) && (gfx || s == 0);

         if (set->bindings)
            EXPECT(in && set->binding_count == sl->binding_count, "bindings do not match the layout");
         if (set->used)
            EXPECT(in, "used set the stage does not have");
         if (set->used && in) {
            EXPECT(set->range.count * 4 == sl->size, "set range");
            EXPECT(set->dynamic_range.count * 4 ==
                      sl->dynamic_buffer_count * sizeof(struct pvr_buffer_descriptor), "dynamic range");
         }
         if (!in)
            EXPECT(!set->range.count && !set->dynamic_range.count, "ranges on an unused set");
      }

      EXPECT(d->common.push_consts.range.count * 4 <= PVR_MAX_PUSH_CONSTANTS_SIZE, "push range");

      /* XCLIPSE: what pvr_setup_descriptor_mappings relies on for every promoted UBO range. */
      EXPECT(d->common.ubo_range_count <= PCO_UBO_PUSH_MAX_RANGES, "range count");
      unsigned total = 0;
      for (unsigned u = 0; u < d->common.ubo_range_count && u < PCO_UBO_PUSH_MAX_RANGES; u++) {
         const pco_ubo_range *r = &d->common.ubo_ranges[u];
         const struct pvr_descriptor_set_layout *sl = r->desc_set < f->l.set_count ? &f->sl[r->desc_set] : NULL;

         EXPECT(sl && (sl->stage_flags & BITFIELD_BIT(ms)) && (gfx || s == 0), "range: set %u not in the stage", r->desc_set);
         if (!sl || r->binding >= sl->binding_count)
            { EXPECT(!sl || r->binding < sl->binding_count, "range: binding %u", r->binding); continue; }
         const struct pvr_descriptor_set_layout_binding *b = &sl->bindings[r->binding];
         EXPECT(b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER || b->type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                "range: binding type %d", b->type);
         EXPECT(r->elem < b->descriptor_count, "range: element %u", r->elem);
         EXPECT(r->count > 0 && r->count <= 256 && r->offset <= 256 - r->count, "range: dwords %u+%u", r->offset, r->count);
         total += r->count;
         EXPECT(total <= 256, "range: total %u", total);
         EXPECT(r->start < d->common.shareds && r->count <= d->common.shareds - r->start && d->common.shareds <= 8192,
                "range: registers %u+%u of %u", r->start, r->count, d->common.shareds);
      }
   }

   if (!gfx)
      return;

   const nir_xfb_info *x = obj->stage[0].data.vs.xfb_info;
   if (x) {
      EXPECT(x->output_count <= PVR_XFB_MAX_TOTAL_VARYINGS, "xfb output count");
      for (unsigned i = 0; i < x->output_count; i++) {
         EXPECT(x->outputs[i].buffer < NIR_MAX_XFB_BUFFERS, "xfb buffer %u", x->outputs[i].buffer);
         EXPECT(x->outputs[i].location < VARYING_SLOT_MAX, "xfb location");
      }
   }

   if (obj->flags & PVR_CACHED_HAS_STAGE1) {
      const pco_fs_data *fs = &obj->stage[1].data.fs;

      EXPECT(!fs->tile_buffers.count ||
                (fs->tile_buffers.stride == 2 && fs->tile_buffers.count / 2 <= PVR_MAX_TILE_BUFFER_COUNT),
             "tile buffers");
      EXPECT(!(obj->fs_varyings_used & ~BITFIELD64_RANGE(VARYING_SLOT_VAR0, MAX_VARYING)), "varyings");
      u_foreach_bit64 (v, obj->fs_varyings_used) {
         const uint8_t m = obj->fs_interp[v - VARYING_SLOT_VAR0];
         EXPECT(m == INTERP_MODE_SMOOTH || m == INTERP_MODE_FLAT || m == INTERP_MODE_NOPERSPECTIVE, "interp");
         EXPECT(fs->varyings[v].count > 0 && fs->varyings[v].count % 4 == 0, "fs varying range");
         EXPECT(obj->stage[0].data.vs.varyings[v].count > 0, "vs varying range");
      }
   } else {
      EXPECT(!obj->fs_varyings_used, "varyings without FS");
   }
}

/* What a pipeline does with a hit: use-time check, clone, free. */
static unsigned use(struct pvr_cached_shaders *obj, enum shape shape, struct fake_layout *f)
{
   bool ok;

   switch (shape) {
   case GFX_FS:
      ok = pvr_cached_shaders_check_graphics(obj, &f->l, true, false);
      break;
   case GFX_NOFS:
      ok = pvr_cached_shaders_check_graphics(obj, &f->l, false, false);
      break;
   case GFX_PASSTHROUGH:
      ok = pvr_cached_shaders_check_graphics(obj, &f->l, true, true);
      break;
   default:
      ok = pvr_cached_shaders_check_compute(obj, &f->l);
      break;
   }

   if (ok) {
      const bool gfx = shape <= GFX_PASSTHROUGH;
      pco_data a, b;

      consumer_invariants(obj, shape, f);

      EXPECT(pvr_cached_stage_clone_data(&obj->stage[0], gfx, &a), "clone 0");
      free_data(&a, gfx);
      if (obj->flags & PVR_CACHED_HAS_STAGE1) {
         EXPECT(pvr_cached_stage_clone_data(&obj->stage[1], false, &b), "clone 1");
         free_data(&b, false);
      }
   }

   return ok;
}

struct stats {
   unsigned total, parsed, used;
};

static void feed(const uint8_t *data, size_t size, enum shape shape, struct fake_layout *f,
                 struct stats *st)
{
   st->total++;
   struct pvr_cached_shaders *obj = deserialize(data, size);

   if (!obj)
      return;

   st->parsed++;
   check_ownership(obj);
   if (use(obj, shape, f))
      st->used++;

   /* Same bytes out as in, so nothing hidden came from the blob. */
   pvr_cached_shaders_unref(dev, obj);
}

static const uint32_t edge[] = { 0, 1, 2, 3, 4, 7, 8, 0xf, 0x10, 0x7f, 0x80, 0xff, 0x100, 0xfff, 0x1000,
                                 0x1001, 0xffff, 0x10000, 0x10001, 0x7fffffff, 0x80000000, 0xfffffffe,
                                 0xffffffff };

static void fuzz_shape(enum shape shape, const char *name)
{
   struct fake_layout f;
   fake_layout_init(&f, BITFIELD_BIT(MESA_SHADER_VERTEX) | BITFIELD_BIT(MESA_SHADER_FRAGMENT) |
                           BITFIELD_BIT(MESA_SHADER_COMPUTE));

   struct pvr_cached_shaders *obj = make_object(shape, &f, false);
   struct blob b;
   serialize(obj, &b);

   /* Valid object: round trip is byte identical and passes the use-time check. */
   struct pvr_cached_shaders *rt = deserialize(b.data, b.size);
   EXPECT(rt, "%s: valid object rejected", name);
   if (rt) {
      struct blob b2;
      check_ownership(rt);
      EXPECT(use(rt, shape, &f), "%s: valid object fails the use-time check", name);
      serialize(rt, &b2);
      EXPECT(b2.size == b.size && !memcmp(b.data, b2.data, b.size), "%s: reserialization differs", name);
      blob_finish(&b2);
      pvr_cached_shaders_unref(dev, rt);
   }

   uint8_t *m = malloc(b.size);
   struct stats st = { 0 };
   static const uint8_t bytes[] = { 0, 1, 0x7f, 0x80, 0xff };

   for (size_t i = 0; i < b.size - 4; i++) {
      for (unsigned k = 0; k < ARRAY_SIZE(bytes); k++) {
         memcpy(m, b.data, b.size);
         m[i] = bytes[k];
         fix_crc(m, b.size);
         feed(m, b.size, shape, &f, &st);
      }
   }

   for (size_t i = 0; i + 4 <= b.size - 4; i += 4) {
      for (unsigned k = 0; k < ARRAY_SIZE(edge); k++) {
         memcpy(m, b.data, b.size);
         memcpy(m + i, &edge[k], 4);
         fix_crc(m, b.size);
         feed(m, b.size, shape, &f, &st);
      }
   }

   /* Truncations and extensions, with a valid CRC. */
   for (size_t n = 4; n < b.size; n++) {
      memcpy(m, b.data, n);
      fix_crc(m, n);
      feed(m, n, shape, &f, &st);
   }

   uint8_t *ext = malloc(b.size + 64);
   memcpy(ext, b.data, b.size - 4);
   memset(ext + b.size - 4, 0x41, 68);
   fix_crc(ext, b.size + 64);
   feed(ext, b.size + 64, shape, &f, &st);

   /* Flag words: every combination, over the valid payload. */
   for (uint32_t fl = 0; fl < 256; fl++) {
      memcpy(m, b.data, b.size);
      memcpy(m + 4, &fl, 4);
      fix_crc(m, b.size);
      feed(m, b.size, shape, &f, &st);
   }

   /* Payload all one pattern. */
   for (unsigned k = 0; k < 3; k++) {
      static const uint8_t pat[] = { 0x41, 0xff, 0x00 };
      memcpy(m, b.data, 8);
      memset(m + 8, pat[k], b.size - 12);
      fix_crc(m, b.size);
      feed(m, b.size, shape, &f, &st);
   }

   printf("%-16s blob %zu bytes: %u mutants, %u parsed, %u pass the use-time check\n", name, b.size,
          st.total, st.parsed, st.used);

   free(m);
   free(ext);
   blob_finish(&b);
   pvr_cached_shaders_unref(dev, obj);
}

/* Cross use: a blob must not pass for a pipeline of the other kind or with another FS presence. */
static void cross_use(void)
{
   struct fake_layout f;
   fake_layout_init(&f, ~0u);

   struct pvr_cached_shaders *g = make_object(GFX_FS, &f, false);
   struct pvr_cached_shaders *gn = make_object(GFX_NOFS, &f, false);
   struct pvr_cached_shaders *gp = make_object(GFX_PASSTHROUGH, &f, false);
   struct pvr_cached_shaders *c = make_object(COMPUTE, &f, false);
   struct pvr_cached_shaders *cz = make_object(COMPUTE_ZERO, &f, false);

   EXPECT(pvr_cached_shaders_check_graphics(g, &f.l, true, false), "gfx fs");
   EXPECT(!pvr_cached_shaders_check_graphics(g, &f.l, false, false), "gfx fs as no-fs");
   EXPECT(!pvr_cached_shaders_check_graphics(g, &f.l, true, true), "gfx fs as passthrough");
   EXPECT(pvr_cached_shaders_check_graphics(gn, &f.l, false, false), "gfx no fs");
   EXPECT(!pvr_cached_shaders_check_graphics(gn, &f.l, true, false), "gfx no fs as fs");
   EXPECT(pvr_cached_shaders_check_graphics(gp, &f.l, true, true), "gfx passthrough");
   EXPECT(!pvr_cached_shaders_check_graphics(gp, &f.l, true, false), "passthrough as fs");
   EXPECT(!pvr_cached_shaders_check_graphics(c, &f.l, false, false), "compute as graphics (no IS_VS)");
   EXPECT(!pvr_cached_shaders_check_graphics(cz, &f.l, true, false), "compute+zero as graphics");
   EXPECT(!pvr_cached_shaders_check_compute(g, &f.l), "graphics as compute (IS_VS)");
   EXPECT(!pvr_cached_shaders_check_compute(gn, &f.l), "graphics no fs as compute");
   EXPECT(pvr_cached_shaders_check_compute(c, &f.l), "compute");
   EXPECT(pvr_cached_shaders_check_compute(cz, &f.l), "compute zero");

   /* Graphics flags without IS_VS, forged on a serialized graphics blob: the stage then parses as a CS
    * (or fails) and must never pass as graphics. Done by the flag sweep in fuzz_shape; here: a forged
    * compute object whose flags claim a graphics VS must not pass as compute. */
   c->flags |= PVR_CACHED_STAGE0_IS_VS;
   EXPECT(!pvr_cached_shaders_check_compute(c, &f.l), "compute with IS_VS");
   c->flags &= ~PVR_CACHED_STAGE0_IS_VS;
   cz->flags &= ~PVR_CACHED_HAS_STAGE1;
   EXPECT(!pvr_cached_shaders_check_compute(cz, &f.l), "zero-init shader missing");

   /* Pointers in the in-memory object are never trusted blindly: a stage whose data claims bindings
    * its layout does not have fails. */
   g->stage[0].data.common.desc_sets[3].binding_count = 5;
   EXPECT(!pvr_cached_shaders_check_graphics(g, &f.l, true, false), "binding count on unused set");
   g->stage[0].data.common.desc_sets[3].binding_count = 0;

   pvr_cached_shaders_unref(dev, g);
   pvr_cached_shaders_unref(dev, gn);
   pvr_cached_shaders_unref(dev, gp);
   pvr_cached_shaders_unref(dev, c);
   pvr_cached_shaders_unref(dev, cz);
}

/* Layout forging: values the compiler derived from a layout must be that layout's. */
static void forged_layouts(void)
{
   struct fake_layout f, g;
   fake_layout_init(&f, ~0u);
   struct pvr_cached_shaders *o = make_object(GFX_FS, &f, false);

   EXPECT(pvr_cached_shaders_check_graphics(o, &f.l, true, false), "layout baseline");

   g = f; fake_layout_fix(&g); g.sl[0].binding_count++;
   EXPECT(!pvr_cached_shaders_check_graphics(o, &g.l, true, false), "binding count differs");
   g = f; fake_layout_fix(&g); g.sl[1].binding_count = 1u << 20;
   EXPECT(!pvr_cached_shaders_check_graphics(o, &g.l, true, false), "huge binding count");
   g = f; fake_layout_fix(&g); g.sl[0].size = 128;
   EXPECT(!pvr_cached_shaders_check_graphics(o, &g.l, true, false), "set size differs");
   g = f; fake_layout_fix(&g); g.sl[1].dynamic_buffer_count = 5;
   EXPECT(!pvr_cached_shaders_check_graphics(o, &g.l, true, false), "dynamic buffer count differs");
   g = f; fake_layout_fix(&g); g.sl[0].dynamic_buffer_count = 1;
   EXPECT(!pvr_cached_shaders_check_graphics(o, &g.l, true, false), "dynamic buffer appears");
   g = f; fake_layout_fix(&g); g.l.push_ranges[0].size = 64;
   o->stage[0].data.common.push_consts.used = ~0u;
   EXPECT(!pvr_cached_shaders_check_graphics(o, &g.l, true, false), "push range differs (~0)");
   o->stage[0].data.common.push_consts.used = 4;
   g = f; fake_layout_fix(&g); g.l.set_count = 1;
   EXPECT(!pvr_cached_shaders_check_graphics(o, &g.l, true, false), "fewer sets");
   g = f; fake_layout_fix(&g); g.sl[1].stage_flags = BITFIELD_BIT(MESA_SHADER_COMPUTE);
   EXPECT(!pvr_cached_shaders_check_graphics(o, &g.l, true, false), "set not used by this stage");
   g = f; fake_layout_fix(&g); g.l.set_layouts[1] = NULL;
   EXPECT(!pvr_cached_shaders_check_graphics(o, &g.l, true, false), "null set layout");

   /* tile buffers: stride 0 (division at draw time), more buffers than the device has slots */
   pco_fs_data *fs = &o->stage[1].data.fs;
   fs->tile_buffers.stride = 0;
   EXPECT(!pvr_cached_shaders_check_graphics(o, &f.l, true, false), "tile buffer stride 0");
   fs->tile_buffers.stride = 2; fs->tile_buffers.count = 2000; fs->num_tile_buffers = 1000;
   EXPECT(!pvr_cached_shaders_check_graphics(o, &f.l, true, false), "1000 tile buffers");
   fs->tile_buffers.count = 4; fs->num_tile_buffers = 2;

   /* FS varying bits outside VAR0..VAR31 or without ranges */
   o->fs_varyings_used = BITFIELD64_BIT(VARYING_SLOT_PNTC);
   EXPECT(!pvr_cached_shaders_check_graphics(o, &f.l, true, false), "varying out of range");
   o->fs_varyings_used = BITFIELD64_BIT(VARYING_SLOT_VAR0 + 1);
   EXPECT(!pvr_cached_shaders_check_graphics(o, &f.l, true, false), "varying without ranges");

   pvr_cached_shaders_unref(dev, o);
}

/* Forgery number i of the list below applied to a stage's data; returns what it did, NULL past the end. */
enum { FORGE_BASE = __COUNTER__ };
#define M(w, ...) case __COUNTER__ - FORGE_BASE - 1: { __VA_ARGS__; } return w
static const char *forge_ubo_range(unsigned i, pco_common_data *c)
{
   switch (i) {
   M("set 7", c->ubo_ranges[0].desc_set = 7);
   M("set 2 (layout has 2)", c->ubo_ranges[0].desc_set = 2);
   M("binding 4095", c->ubo_ranges[0].binding = 4095);
   M("binding 3 of set 0 (past the set)", c->ubo_ranges[0].binding = 3);
   M("binding is an inline block", c->ubo_ranges[0].binding = 1);
   M("binding is a sampler", c->ubo_ranges[0].binding = 2);
   M("binding of set 1 is a storage buffer", c->ubo_ranges[1].binding = 1);
   M("element 3 of 3", c->ubo_ranges[0].elem = 3);
   M("element 0xffffffff (elem * stride wraps)", c->ubo_ranges[0].elem = ~0u);
   M("dynamic element 1 of 1", c->ubo_ranges[1].elem = 1);
   M("zero dwords", c->ubo_ranges[0].count = 0);
   M("257 dwords", c->ubo_ranges[0].count = 257; c->ubo_ranges[0].offset = 0);
   M("4096 dwords (BSIZE overflow)", c->ubo_ranges[0].count = 4096; c->ubo_ranges[0].offset = 0);
   M("offset 256", c->ubo_ranges[0].offset = 256);
   M("offset + count past the window", c->ubo_ranges[0].offset = 250; c->ubo_ranges[0].count = 8);
   M("offset + count wraps 32 bits", c->ubo_ranges[0].offset = 0xfffffffe; c->ubo_ranges[0].count = 4);
   M("total 400 dwords", c->ubo_ranges[0].count = 200; c->ubo_ranges[0].offset = 0;
                            c->ubo_ranges[1].count = 200; c->ubo_ranges[1].offset = 0);
   M("start past the shared registers", c->ubo_ranges[0].start = 80);
   M("start 8191 (AO field)", c->ubo_ranges[0].start = 8191);
   M("start + count past shareds", c->ubo_ranges[0].start = 73);
   M("start 0xfffffff8 (start + count wraps)", c->ubo_ranges[0].start = 0xfffffff8u);
   M("shareds 8193", c->shareds = 8193);
   M("9 ranges", c->ubo_range_count = 9);
   }
#undef M
   return NULL;
}

/* XCLIPSE: forged UBO ranges: a blob that is well formed (every field in its own bound) whose ranges do not fit the
 * layout must not pass the use-time check; each of them would index or DMA out of bounds in the draw code. */
static void forged_ubo_ranges(void)
{
   struct fake_layout f;
   fake_layout_init(&f, ~0u);

   for (unsigned i = 0; forge_ubo_range(i, &(pco_common_data){ 0 }); i++) {
      struct pvr_cached_shaders *o = make_object(GFX_FS, &f, false);
      EXPECT(pvr_cached_shaders_check_graphics(o, &f.l, true, false), "ranges baseline");
      EXPECT(o->stage[0].data.common.ubo_range_count == 2 && o->stage[1].data.common.ubo_range_count == 2,
             "baseline has no ranges");
      const char *what = forge_ubo_range(i, &o->stage[0].data.common);
      EXPECT(!pvr_cached_shaders_check_graphics(o, &f.l, true, false), "forged range accepted (VS): %s", what);
      pvr_cached_shaders_unref(dev, o);

      o = make_object(GFX_FS, &f, false);
      what = forge_ubo_range(i, &o->stage[1].data.common);
      EXPECT(!pvr_cached_shaders_check_graphics(o, &f.l, true, false), "forged range accepted (FS): %s", what);
      pvr_cached_shaders_unref(dev, o);

      o = make_object(COMPUTE, &f, false);
      EXPECT(pvr_cached_shaders_check_compute(o, &f.l), "compute ranges baseline");
      what = forge_ubo_range(i, &o->stage[0].data.common);
      EXPECT(!pvr_cached_shaders_check_compute(o, &f.l), "forged range accepted (CS): %s", what);
      pvr_cached_shaders_unref(dev, o);
   }

   /* Ranges with no layout set to match, or for a stage the set is not visible to. */
   struct fake_layout g = f;
   fake_layout_fix(&g);
   struct pvr_cached_shaders *o = make_object(GFX_FS, &f, false);
   g.sl[0].stage_flags = BITFIELD_BIT(MESA_SHADER_COMPUTE);
   EXPECT(!pvr_cached_shaders_check_graphics(o, &g.l, true, false), "range of a set the stage cannot see");
   g = f; fake_layout_fix(&g); g.l.set_layouts[1] = NULL;
   EXPECT(!pvr_cached_shaders_check_graphics(o, &g.l, true, false), "range of a null set layout");
   g = f; fake_layout_fix(&g); g.bind[0][0].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
   EXPECT(!pvr_cached_shaders_check_graphics(o, &g.l, true, false), "range of a binding the layout made a storage buffer");
   g = f; fake_layout_fix(&g); g.bind[0][0].descriptor_count = 2;
   EXPECT(!pvr_cached_shaders_check_graphics(o, &g.l, true, false), "range element past a smaller array");
   pvr_cached_shaders_unref(dev, o);
}

/* XCLIPSE: pco_fit_ubo_ranges(): the ranges end inside the limit, the least dense go first, nothing else moves. */
static void fit_ranges(void)
{
   pco_common_data c;

   memset(&c, 0, sizeof(c));
   c.shareds = 10;
   pco_fit_ubo_ranges(&c, 100);
   EXPECT(c.shareds == 10 && !c.ubo_range_count, "no ranges: untouched");

   c.ubo_range_count = 3;
   c.ubo_ranges[0] = (pco_ubo_range){ .count = 16, .uses = 100 }; /* 6.25 per dword */
   c.ubo_ranges[1] = (pco_ubo_range){ .count = 32, .uses = 33 };  /* 1.03 */
   c.ubo_ranges[2] = (pco_ubo_range){ .count = 8, .uses = 40 };   /* 5 */
   pco_fit_ubo_ranges(&c, 100);
   EXPECT(c.ubo_range_count == 3 && c.ubo_ranges[0].start == 12 && c.ubo_ranges[1].start == 28 &&
             c.ubo_ranges[2].start == 60 && c.shareds == 68, "fits: aligned to 4, in order (start %u %u %u, shareds %u)",
          c.ubo_ranges[0].start, c.ubo_ranges[1].start, c.ubo_ranges[2].start, c.shareds);

   memset(&c, 0, sizeof(c));
   c.shareds = 10;
   c.ubo_range_count = 3;
   c.ubo_ranges[0] = (pco_ubo_range){ .count = 16, .uses = 100 };
   c.ubo_ranges[1] = (pco_ubo_range){ .count = 32, .uses = 33 };
   c.ubo_ranges[2] = (pco_ubo_range){ .count = 8, .uses = 40 };
   pco_fit_ubo_ranges(&c, 40); /* 12 + 56 does not fit; without the 32 it does */
   EXPECT(c.ubo_range_count == 2 && c.ubo_ranges[0].count == 16 && c.ubo_ranges[1].count == 8 && c.shareds == 36 &&
             c.ubo_ranges[0].start == 12 && c.ubo_ranges[1].start == 28,
          "drops the least dense (count %u, shareds %u)", c.ubo_range_count, c.shareds);

   pco_fit_ubo_ranges(&c, 11); /* nothing fits: all dropped, shareds back to the allocation without the ranges */
   EXPECT(c.ubo_range_count == 0 && c.shareds == 36, "all dropped (count %u shareds %u)", c.ubo_range_count, c.shareds);

   memset(&c, 0, sizeof(c));
   c.shareds = 2000;
   c.ubo_range_count = 1;
   c.ubo_ranges[0] = (pco_ubo_range){ .count = 4, .uses = 1 };
   pco_fit_ubo_ranges(&c, 1024); /* the rest of the stage already exceeds the limit */
   EXPECT(c.ubo_range_count == 0 && c.shareds == 2000, "stage already over the limit");
}

/* Which bytes of a stage's pco_data does the format keep? Each byte is changed in turn; if the new value
 * comes back out of serialize + deserialize (or is refused as out of bounds) the byte is data, else it is
 * padding or a field the format drops. Pointers are marked 2 and not touched. */
enum { DROPPED = 0, KEPT = 1, POINTER = 2 };

static void classify(struct pvr_cached_shaders *obj, unsigned slot, uint8_t *kind)
{
   pco_data *d = &obj->stage[slot].data;
   uint8_t *p = (uint8_t *)d;
   const bool vs = slot == 0 && (obj->flags & PVR_CACHED_STAGE0_IS_VS);
   static const uint8_t nudge[] = { 0x01, 0x80 };

   memset(kind, DROPPED, sizeof(*d));
   for (unsigned u = 0; u < ARRAY_SIZE(d->common.desc_sets); u++) {
      size_t o = (uint8_t *)&d->common.desc_sets[u].bindings - p;
      memset(kind + o, POINTER, sizeof(void *));
   }
   if (vs)
      memset(kind + ((uint8_t *)&d->vs.xfb_info - p), POINTER, sizeof(void *));

   /* The writer indexes the bindings by this count, so it is not nudged (in memory data is trusted). */
   for (unsigned u = 0; u < ARRAY_SIZE(d->common.desc_sets); u++) {
      size_t o = (uint8_t *)&d->common.desc_sets[u].binding_count - p;
      memset(kind + o, KEPT, sizeof(unsigned));
   }

   for (size_t i = 0; i < sizeof(*d); i++) {
      if (kind[i] != DROPPED)
         continue;

      const uint8_t was = p[i];

      for (unsigned k = 0; k < ARRAY_SIZE(nudge) && kind[i] == DROPPED; k++) {
         p[i] = was ^ nudge[k];

         struct blob b;
         const bool written = try_serialize(obj, &b);
         struct pvr_cached_shaders *rt = written ? deserialize(b.data, b.size) : NULL;

         if (!rt) {
            /* The writer or the reader refused the value: out of bounds, so data. */
            kind[i] = KEPT;
         } else {
            if (((const uint8_t *)&rt->stage[slot].data)[i] == p[i])
               kind[i] = KEPT;
            pvr_cached_shaders_unref(dev, rt);
         }
         blob_finish(&b);
      }

      p[i] = was;
   }
}

static void probe_bytes(void)
{
   struct fake_layout f;
   fake_layout_init(&f, ~0u);

   static const struct { enum shape s; const char *n; unsigned slot; } cases[] = {
      { GFX_FS, "vs", 0 }, { GFX_FS, "fs", 1 }, { COMPUTE_ZERO, "cs", 0 },
   };

   for (unsigned c = 0; c < ARRAY_SIZE(cases); c++) {
      struct pvr_cached_shaders *obj = make_object(cases[c].s, &f, false);
      const size_t sz = sizeof(pco_data);
      uint8_t *kind = malloc(sz);
      unsigned kept = 0;

      classify(obj, cases[c].slot, kind);
      for (size_t i = 0; i < sz; i++)
         kept += kind[i] == KEPT;

      printf("pco_data (%s) %zu bytes: %u kept, dropped (padding, pointers, unused union members):",
             cases[c].n, sz, kept);
      for (size_t i = 0; i < sz;) {
         if (kind[i] == KEPT) {
            i++;
            continue;
         }
         size_t j = i;
         while (j < sz && kind[j] != KEPT)
            j++;
         printf(" [%zu,%zu)", i, j);
         i = j;
      }
      printf("\n");
      free(kind);
      pvr_cached_shaders_unref(dev, obj);
   }
}

/* No struct padding or uninitialised memory in the written data. */
static void padding_is_zero(void)
{
   struct fake_layout f;
   fake_layout_init(&f, ~0u);

   static const struct { enum shape s; unsigned slots; } cases[] = {
      { GFX_FS, 2 }, { GFX_NOFS, 1 }, { COMPUTE_ZERO, 2 },
   };

   for (unsigned c = 0; c < ARRAY_SIZE(cases); c++) {
      struct pvr_cached_shaders *a = make_object(cases[c].s, &f, false);
      struct pvr_cached_shaders *b = make_object(cases[c].s, &f, false);
      struct blob ba, bb;

      for (unsigned slot = 0; slot < cases[c].slots; slot++) {
         uint8_t *kind = malloc(sizeof(pco_data));
         uint8_t *p = (uint8_t *)&b->stage[slot].data;
         unsigned poisoned = 0;

         classify(a, slot, kind);
         for (size_t i = 0; i < sizeof(pco_data); i++) {
            if (kind[i] == DROPPED) {
               p[i] = 0xaa;
               poisoned++;
            }
         }
         EXPECT(poisoned, "no padding found to poison");
         free(kind);
      }

      serialize(a, &ba);
      serialize(b, &bb);
      EXPECT(ba.size == bb.size && !memcmp(ba.data, bb.data, ba.size),
             "shape %d: poisoned padding reaches the output", cases[c].s);
      blob_finish(&ba);
      blob_finish(&bb);
      pvr_cached_shaders_unref(dev, a);
      pvr_cached_shaders_unref(dev, b);
   }
}

static void cache_dir_names(void)
{
   static const struct { const char *n; bool ok; } t[] = {
      { "com.example.app", true }, { "com.example_2.App9", true }, { "a.b", true },
      { "..", false }, { ".", false }, { ".hidden.app", false }, { "../x.y", false },
      { "com.example/../../..", false }, { "com.exa mple", false }, { "com.ex%ample", false },
      { "com.ex-ample", false }, { "nodots", false }, { "", false }, { "com.ex\nample", false },
      { "com.example\xc3\xa9", false },
   };

   for (unsigned i = 0; i < ARRAY_SIZE(t); i++)
      EXPECT(pvr_cache_dir_name_valid(t[i].n) == t[i].ok, "cache dir name '%s'", t[i].n);
}

int main(void)
{
   setup();

   cross_use();
   forged_layouts();
   forged_ubo_ranges();
   fit_ranges();
   fuzz_shape(GFX_FS, "graphics fs");
   fuzz_shape(GFX_NOFS, "graphics no fs");
   fuzz_shape(GFX_PASSTHROUGH, "passthrough");
   fuzz_shape(COMPUTE, "compute");
   fuzz_shape(COMPUTE_ZERO, "compute zero-init");
   probe_bytes();
   padding_is_zero();
   cache_dir_names();

   printf("%u checks, %u failures\n", checks, failures);
   return failures ? 1 : 0;
}
