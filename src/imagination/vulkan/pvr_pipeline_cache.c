/*
 * SPDX-License-Identifier: MIT
 */

#include "pvr_pipeline_cache.h"

#include <string.h>

#include "pvr_descriptor_set.h"
#include "pvr_device.h"
#include "pds/pvr_pds.h"
#include "util/blob.h"
#include "vk_pipeline_layout.h"
#include "vk_util.h"
#include "util/crc32.h"
#include "util/macros.h"
#include "util/ralloc.h"
#include "vk_alloc.h"
#include "vk_device.h"

#define PVR_CACHED_MAGIC 0x32435650u /* "PVC2" */

static void pvr_cached_shaders_destroy(struct vk_device *vk_device,
                                       struct vk_pipeline_cache_object *base)
{
   struct pvr_cached_shaders *obj =
      container_of(base, struct pvr_cached_shaders, base);

   vk_pipeline_cache_object_finish(&obj->base);
   ralloc_free(obj->mem_ctx);
   vk_free(&vk_device->alloc, obj);
}

struct pvr_cached_shaders *
pvr_cached_shaders_create(struct pvr_device *device,
                          const uint8_t key[PVR_CACHED_KEY_SIZE],
                          uint32_t flags)
{
   struct pvr_cached_shaders *obj = vk_zalloc(&device->vk.alloc,
                                              sizeof(*obj),
                                              8,
                                              VK_SYSTEM_ALLOCATION_SCOPE_DEVICE);
   if (!obj)
      return NULL;

   obj->mem_ctx = ralloc_context(NULL);
   if (!obj->mem_ctx) {
      vk_free(&device->vk.alloc, obj);
      return NULL;
   }

   memcpy(obj->key, key, sizeof(obj->key));
   obj->flags = flags;
   vk_pipeline_cache_object_init(&device->vk,
                                 &obj->base,
                                 &pvr_cached_shaders_ops,
                                 obj->key,
                                 sizeof(obj->key));
   return obj;
}

/* Deep-copies the pointers of src into mem_ctx. */
static bool pvr_cached_stage_own_data(void *mem_ctx,
                                      bool is_vs,
                                      pco_data *data)
{
   for (unsigned u = 0; u < ARRAY_SIZE(data->common.desc_sets); ++u) {
      pco_descriptor_set_data *set = &data->common.desc_sets[u];

      if (!set->bindings)
         continue;

      size_t size = (size_t)set->binding_count * sizeof(*set->bindings);
      void *copy = ralloc_size(mem_ctx, size);
      if (!copy)
         return false;

      memcpy(copy, set->bindings, size);
      set->bindings = copy;
   }

   if (is_vs && data->vs.xfb_info) {
      size_t size = nir_xfb_info_size(data->vs.xfb_info->output_count);
      void *copy = ralloc_size(mem_ctx, size);
      if (!copy)
         return false;

      memcpy(copy, data->vs.xfb_info, size);
      data->vs.xfb_info = copy;
   }

   return true;
}

bool pvr_cached_shaders_set_stage(struct pvr_cached_shaders *obj,
                                  unsigned slot,
                                  const pco_data *data,
                                  const void *binary,
                                  uint32_t binary_size,
                                  const struct pvr_stats *stats)
{
   struct pvr_cached_stage *stage = &obj->stage[slot];
   const bool is_vs = slot == 0 && (obj->flags & PVR_CACHED_STAGE0_IS_VS);

   stage->data = *data;
   if (!pvr_cached_stage_own_data(obj->mem_ctx, is_vs, &stage->data))
      return false;

   if (is_vs && stage->data.vs.xfb_info)
      obj->flags |= PVR_CACHED_HAS_XFB;

   stage->binary = ralloc_size(obj->mem_ctx, binary_size);
   if (!stage->binary)
      return false;

   memcpy(stage->binary, binary, binary_size);
   stage->binary_size = binary_size;
   stage->stats = *stats;
   return true;
}

bool pvr_cached_stage_clone_data(const struct pvr_cached_stage *stage,
                                 bool is_vs,
                                 pco_data *out)
{
   *out = stage->data;

   for (unsigned u = 0; u < ARRAY_SIZE(out->common.desc_sets); ++u) {
      pco_descriptor_set_data *set = &out->common.desc_sets[u];

      if (!set->bindings)
         continue;

      size_t size = (size_t)set->binding_count * sizeof(*set->bindings);
      void *copy = ralloc_size(NULL, size);
      if (!copy)
         goto fail;

      memcpy(copy, set->bindings, size);
      set->bindings = copy;
   }

   if (is_vs && out->vs.xfb_info) {
      size_t size = nir_xfb_info_size(out->vs.xfb_info->output_count);
      void *copy = ralloc_size(NULL, size);
      if (!copy)
         goto fail;

      memcpy(copy, out->vs.xfb_info, size);
      out->vs.xfb_info = copy;
   }

   return true;

fail:
   for (unsigned u = 0; u < ARRAY_SIZE(out->common.desc_sets); ++u) {
      if (out->common.desc_sets[u].bindings !=
          stage->data.common.desc_sets[u].bindings)
         ralloc_free(out->common.desc_sets[u].bindings);
   }
   memset(out, 0, sizeof(*out));
   return false;
}

/* XCLIPSE: serialization.
 *
 * Every field of the compiler data is written one by one (never the raw
 * struct), so no struct padding or pointer reaches the disk cache and the
 * reader decides itself what goes into each field. The same walker runs for
 * writing and for reading (io->w set or not), so the two cannot disagree. On
 * read every value is checked against a bound that all compiler output
 * satisfies (register spaces, counts, enum ranges, bools are 0 or 1) and the
 * first violation makes the whole object a miss. Pointers (descriptor
 * bindings, xfb_info) are rebuilt from explicitly serialized data only.
 * Checks that depend on the pipeline being created (layout, fragment shader
 * presence) are in pvr_cached_shaders_check_*(), run before a hit is used.
 */

/* Bound on any register number or count in a range. The largest register
 * spaces (shared, coefficient, unified store) are a few thousand dwords.
 */
#define PVR_CACHED_MAX_REG 0x10000u

/* Sanity bound on a descriptor set's binding count when reading cache data. */
#define PVR_CACHED_MAX_BINDINGS 4096u

static_assert(sizeof(unsigned) == sizeof(uint32_t), "unsigned is 32 bits");
static_assert(PVR_MAX_DESCRIPTOR_SETS <= 8, "binding presence mask is 8 bits");

struct pvr_cache_io {
   struct blob *w; /* Writing when set, reading from r otherwise. */
   struct blob_reader *r;
   bool ok;
};

static void io_val(struct pvr_cache_io *io, uint32_t *v, uint32_t max)
{
   if (io->w) {
      if (*v > max)
         io->ok = false;
      blob_write_uint32(io->w, *v);
      return;
   }

   *v = blob_read_uint32(io->r);
   if (*v > max) {
      io->ok = false;
      *v = 0;
   }
}

/* Any integer field (also bit-fields); max must fit the field. */
#define IO_INT(f, max)                      \
   do {                                     \
      uint32_t v_ = (f);                    \
      io_val(io, &v_, (max));               \
      (f) = v_;                             \
   } while (0)

/* A bool is read as a byte first: any pattern other than 0 or 1 is bad. */
#define IO_BOOL(f)                                      \
   do {                                                 \
      uint32_t v_ = ((const uint8_t *)&(f))[0] != 0;    \
      io_val(io, &v_, 1);                               \
      (f) = v_;                                         \
   } while (0)

/* Enums may be 1, 2 or 4 bytes (pipe_format is packed). */
static void io_enum(struct pvr_cache_io *io, void *p, size_t size, uint32_t max)
{
   uint32_t v = 0;

   switch (size) {
   case 1: {
      uint8_t t;
      memcpy(&t, p, sizeof(t));
      v = t;
      break;
   }
   case 2: {
      uint16_t t;
      memcpy(&t, p, sizeof(t));
      v = t;
      break;
   }
   default: {
      assert(size == sizeof(v));
      memcpy(&v, p, sizeof(v));
      break;
   }
   }

   io_val(io, &v, max);

   switch (size) {
   case 1: {
      uint8_t t = v;
      memcpy(p, &t, sizeof(t));
      break;
   }
   case 2: {
      uint16_t t = v;
      memcpy(p, &t, sizeof(t));
      break;
   }
   default:
      memcpy(p, &v, sizeof(v));
      break;
   }
}

#define IO_ENUM(f, max) io_enum(io, &(f), sizeof(f), (max))

#define IO_RANGE_MAX(r, max)     \
   do {                          \
      IO_INT((r).start, (max));  \
      IO_INT((r).count, (max));  \
      IO_INT((r).stride, (max)); \
   } while (0)

#define IO_RANGE(r) IO_RANGE_MAX(r, PVR_CACHED_MAX_REG)

#define IO_RANGES(arr)                              \
   for (unsigned i_ = 0; i_ < ARRAY_SIZE(arr); i_++) \
      IO_RANGE((arr)[i_])

static void io_common(struct pvr_cache_io *io, pco_common_data *c)
{
   IO_RANGES(c->sys_vals);

   for (unsigned u = 0; u < ARRAY_SIZE(c->desc_sets); u++) {
      pco_descriptor_set_data *set = &c->desc_sets[u];

      IO_RANGE(set->range);
      IO_RANGE(set->dynamic_range);
      IO_INT(set->binding_count, PVR_CACHED_MAX_BINDINGS);
      IO_BOOL(set->used);
   }

   IO_RANGE(c->push_consts.range);
   IO_INT(c->push_consts.used, ~0u);

   IO_RANGE(c->point_sampler);
   IO_RANGE(c->ia_sampler);

   IO_INT(c->temps, PVR_CACHED_MAX_REG);
   IO_INT(c->vtxins, PVR_CACHED_MAX_REG);
   IO_INT(c->interns, PVR_CACHED_MAX_REG);

   /* Both multiplied by the GPU's instance count to size a buffer. */
   IO_INT(c->spilled_temps, BITFIELD_BIT(16));
   IO_RANGE(c->spill_info);
   IO_INT(c->scratch, BITFIELD_BIT(20));
   IO_RANGE(c->scratch_info);

   IO_INT(c->coeffs, PVR_CACHED_MAX_REG);
   IO_INT(c->shareds, PVR_CACHED_MAX_REG);

   /* Checked against the binary size by the reader of the stage. */
   IO_INT(c->entry_offset, ~0u);

   IO_BOOL(c->uses.atomics);
   IO_BOOL(c->uses.barriers);
   IO_BOOL(c->uses.side_effects);
   IO_BOOL(c->uses.empty);
   IO_BOOL(c->uses.point_sampler);
   IO_BOOL(c->uses.ia_sampler);
   IO_BOOL(c->uses.usclib);

   IO_BOOL(c->robust_buffer_access);
   IO_BOOL(c->null_descriptor);
   IO_BOOL(c->image_2d_view_of_3d);
   IO_BOOL(c->image_sliced_view_of_3d);
   IO_BOOL(c->multiview);
}

static void io_vs(struct pvr_cache_io *io, pco_vs_data *vs)
{
   IO_RANGES(vs->attribs);
   for (unsigned i = 0; i < ARRAY_SIZE(vs->attrib_formats); i++)
      IO_ENUM(vs->attrib_formats[i], PIPE_FORMAT_COUNT - 1);
   IO_RANGES(vs->varyings);

   IO_INT(vs->clip_count, 8);
   IO_INT(vs->cull_count, 8);

   IO_INT(vs->f32_smooth, 1024);
   IO_INT(vs->f32_flat, 1024);
   IO_INT(vs->f32_npc, 1024);
   IO_INT(vs->f16_smooth, 1024);
   IO_INT(vs->f16_flat, 1024);
   IO_INT(vs->f16_npc, 1024);
   IO_INT(vs->vtxouts, 1024);
}

static void io_blend_channel(struct pvr_cache_io *io,
                             nir_lower_blend_channel *ch)
{
   /* Compile-time input only (nothing reads it after the shader is built). */
   IO_ENUM(ch->func, 0xffff);
   IO_ENUM(ch->src_factor, 0xffff);
   IO_ENUM(ch->dst_factor, 0xffff);
}

static void io_fs(struct pvr_cache_io *io, pco_fs_data *fs)
{
   IO_RANGES(fs->varyings);
   IO_RANGES(fs->outputs);
   IO_INT(fs->output_tile_buffers, 0xff);
   for (unsigned i = 0; i < ARRAY_SIZE(fs->output_formats); i++)
      IO_ENUM(fs->output_formats[i], PIPE_FORMAT_COUNT - 1);

   IO_RANGES(fs->ias_onchip);
   for (unsigned i = 0; i < ARRAY_SIZE(fs->ia_formats); i++)
      IO_ENUM(fs->ia_formats[i], PIPE_FORMAT_COUNT - 1);
   IO_INT(fs->ia_tile_buffers, 0xff);
   IO_INT(fs->ia_has_stencil, 0xff);

   IO_INT(fs->num_tile_buffers, PVR_MAX_TILE_BUFFER_COUNT);
   IO_RANGE(fs->meta);
   IO_BOOL(fs->meta_is_static);
   IO_INT(fs->meta_static, ~0u);

   /* ~0 when there is no Z replicate. */
   IO_ENUM(fs->z_replicate, ~0u);

   for (unsigned i = 0; i < ARRAY_SIZE(fs->blend_opts.rt); i++) {
      nir_lower_blend_rt *rt = &fs->blend_opts.rt[i];

      IO_ENUM(rt->format, PIPE_FORMAT_COUNT - 1);
      io_blend_channel(io, &rt->rgb);
      io_blend_channel(io, &rt->alpha);
      IO_INT(rt->colormask, 0xf);
      IO_INT(rt->advanced_blend, 1);
      IO_ENUM(rt->blend_mode, 0xffff);
      IO_BOOL(rt->src_premultiplied);
      IO_BOOL(rt->dst_premultiplied);
      IO_ENUM(rt->overlap, 0xffff);
   }
   IO_BOOL(fs->blend_opts.logicop_enable);
   IO_ENUM(fs->blend_opts.logicop_func, 0xffff);
   IO_BOOL(fs->blend_opts.scalar_blend_const);

   IO_RANGE(fs->blend_consts);
   IO_INT(fs->blend_consts_needed, 0xff);
   IO_INT(fs->rasterization_samples, 64);
   IO_RANGE(fs->tile_buffers);
   IO_RANGE(fs->sample_locations);
   IO_ENUM(fs->view_index_slot, VARYING_SLOT_MAX);

   IO_BOOL(fs->uses.w);
   IO_BOOL(fs->uses.z);
   IO_BOOL(fs->uses.pntc);
   IO_BOOL(fs->uses.phase_change);
   IO_BOOL(fs->uses.fbfetch);
   IO_BOOL(fs->uses.depth_feedback);
   IO_BOOL(fs->uses.discard);
   IO_BOOL(fs->uses.sample_shading);
   IO_BOOL(fs->uses.sample_locations);
   IO_BOOL(fs->uses.alpha_to_coverage);
   IO_BOOL(fs->uses.olchk_skip);
   IO_BOOL(fs->meta_present.color_write_enable);
}

static void io_cs(struct pvr_cache_io *io, pco_cs_data *cs)
{
   /* Product of the three is multiplied by per-cluster counts. */
   for (unsigned i = 0; i < ARRAY_SIZE(cs->workgroup_size); i++)
      IO_INT(cs->workgroup_size[i], 1024);
   IO_RANGE(cs->shmem);
   IO_BOOL(cs->zero_shmem);
   IO_BOOL(cs->global_shmem);
}

/* Which union member of pco_data a stage uses. */
enum pvr_cached_kind {
   PVR_CACHED_KIND_VS,
   PVR_CACHED_KIND_FS,
   PVR_CACHED_KIND_CS,
};

static enum pvr_cached_kind pvr_cached_kind(uint32_t flags, unsigned slot)
{
   if (flags & PVR_CACHED_STAGE0_IS_VS)
      return slot ? PVR_CACHED_KIND_FS : PVR_CACHED_KIND_VS;

   /* The zero-init shader of a compute pipeline is built as a CS too. */
   return PVR_CACHED_KIND_CS;
}

static void io_data(struct pvr_cache_io *io,
                    enum pvr_cached_kind kind,
                    pco_data *d)
{
   switch (kind) {
   case PVR_CACHED_KIND_VS:
      io_vs(io, &d->vs);
      break;
   case PVR_CACHED_KIND_FS:
      io_fs(io, &d->fs);
      break;
   case PVR_CACHED_KIND_CS:
      io_cs(io, &d->cs);
      break;
   }

   io_common(io, &d->common);
}

static void io_binding(struct pvr_cache_io *io, pco_binding_data *b)
{
   /* Only the compiler reads the ranges (they are not register numbers the
    * driver uses), so they are not bounded.
    */
   IO_RANGE_MAX(b->range, ~0u);
   IO_BOOL(b->used);
   IO_BOOL(b->is_img_smp);
   IO_BOOL(b->is_inline_ubo);
}

/* Hard sizes: the xfb info is two dozen bytes plus 16 per output. */
static_assert(PVR_XFB_MAX_BUFFERS == NIR_MAX_XFB_BUFFERS, "xfb buffers");

static void io_xfb_head(struct pvr_cache_io *io, nir_xfb_info *x)
{
   IO_INT(x->buffers_written, BITFIELD_MASK(NIR_MAX_XFB_BUFFERS));
   IO_INT(x->streams_written, BITFIELD_MASK(NIR_MAX_XFB_STREAMS));
   for (unsigned i = 0; i < NIR_MAX_XFB_BUFFERS; i++) {
      IO_INT(x->buffers[i].stride, PVR_XFB_MAX_DATA_STRIDE);
      IO_INT(x->buffers[i].varying_count, PVR_XFB_MAX_TOTAL_VARYINGS);
      IO_INT(x->buffer_to_stream[i], NIR_MAX_XFB_STREAMS - 1);
   }
   IO_INT(x->output_count, PVR_XFB_MAX_TOTAL_VARYINGS);
}

static void io_xfb_output(struct pvr_cache_io *io, nir_xfb_output_info *o)
{
   IO_INT(o->buffer, NIR_MAX_XFB_BUFFERS - 1);
   IO_INT(o->offset, PVR_XFB_MAX_DATA_STRIDE);
   IO_INT(o->location, VARYING_SLOT_MAX - 1);
   IO_BOOL(o->data_is_16bit);
   IO_BOOL(o->high_16bits);
   IO_BOOL(o->mediump);
   IO_ENUM(o->mediump_upconvert_type, 0xff);
   IO_INT(o->component_mask, 0xf);
   IO_INT(o->component_offset, 3);
}

/* Writes *xfb, or reads a new one (zeroed, owned by mem_ctx) into *xfb. */
static bool io_xfb(struct pvr_cache_io *io, void *mem_ctx, nir_xfb_info **xfb)
{
   nir_xfb_info head;

   if (io->w) {
      head = **xfb;
      io_xfb_head(io, &head);
      for (unsigned i = 0; i < head.output_count; i++) {
         nir_xfb_output_info o = (*xfb)->outputs[i];
         io_xfb_output(io, &o);
      }
      return io->ok;
   }

   memset(&head, 0, sizeof(head));
   io_xfb_head(io, &head);
   if (!io->ok || io->r->overrun)
      return false;

   nir_xfb_info *x = rzalloc_size(mem_ctx, nir_xfb_info_size(head.output_count));
   if (!x)
      return false;

   *x = head;
   for (unsigned i = 0; i < head.output_count; i++)
      io_xfb_output(io, &x->outputs[i]);

   if (!io->ok || io->r->overrun)
      return false;

   *xfb = x;
   return true;
}

/* XCLIPSE: every field of pco_data and its parts is listed above by hand. A
 * field added to the compiler's data that is not added there would silently
 * not be cached (and read back as zero), so a size change stops the build
 * until the walkers are looked at.
 */
#if UINTPTR_MAX == 0xffffffffu
static_assert(sizeof(pco_data) == 3096, "pco_data changed: update io_*()");
#else
static_assert(sizeof(pco_data) == 3152, "pco_data changed: update io_*()");
#endif
static_assert(sizeof(nir_xfb_info) == 24, "nir_xfb_info changed: update io_xfb()");

/* Written after the fields of each stage. */
static_assert(sizeof(struct pvr_stats) == sizeof(uint32_t) + sizeof(struct rogue_stats),
              "pvr_stats has padding");

static bool pvr_cached_shaders_serialize(struct vk_pipeline_cache_object *base,
                                         struct blob *blob)
{
   const struct pvr_cached_shaders *obj =
      container_of(base, struct pvr_cached_shaders, base);
   struct pvr_cache_io io = { .w = blob, .ok = true };

   const size_t start = blob->size;

   blob_write_uint32(blob, PVR_CACHED_MAGIC);
   blob_write_uint32(blob, obj->flags);

   for (unsigned s = 0; s < 2; ++s) {
      if (!(obj->flags & (PVR_CACHED_HAS_STAGE0 << s)))
         continue;

      const struct pvr_cached_stage *stage = &obj->stage[s];
      pco_data raw = stage->data;
      uint32_t bindings_present = 0;

      io_data(&io, pvr_cached_kind(obj->flags, s), &raw);

      for (unsigned u = 0; u < ARRAY_SIZE(raw.common.desc_sets); ++u) {
         if (raw.common.desc_sets[u].bindings)
            bindings_present |= BITFIELD_BIT(u);
      }

      blob_write_uint32(blob, bindings_present);
      for (unsigned u = 0; u < ARRAY_SIZE(raw.common.desc_sets); ++u) {
         const pco_descriptor_set_data *set = &raw.common.desc_sets[u];

         if (!(bindings_present & BITFIELD_BIT(u)))
            continue;

         for (unsigned b = 0; b < set->binding_count; b++) {
            pco_binding_data binding = set->bindings[b];
            io_binding(&io, &binding);
         }
      }

      if (s == 0 && (obj->flags & PVR_CACHED_HAS_XFB)) {
         nir_xfb_info *xfb = raw.vs.xfb_info;

         if (!xfb || !io_xfb(&io, NULL, &xfb))
            return false;
      }

      blob_write_uint32(blob, stage->binary_size);
      blob_write_bytes(blob, stage->binary, stage->binary_size);
      blob_write_bytes(blob, &stage->stats, sizeof(stage->stats));
   }

   blob_write_uint64(blob, obj->fs_varyings_used);
   blob_write_bytes(blob, obj->fs_interp, sizeof(obj->fs_interp));

   /* A value above its bound would be refused on read: do not write it. */
   if (!io.ok)
      return false;

   /* XCLIPSE: the VkPipelineCache data has no checksum of its own. The
    * payload is raw compiler data (register ranges, formats), so a flipped
    * bit must be rejected here and not turn into a bad shader. This catches
    * accidents only: the data is still treated as untrusted on read. Absent
    * when only the size is queried (no buffer).
    */
   blob_write_uint32(blob,
                     blob->data ? util_hash_crc32(blob->data + start,
                                                  blob->size - start)
                                : 0);

   return !blob->out_of_memory;
}

static bool pvr_cached_stage_read(struct pvr_cached_shaders *obj,
                                  unsigned slot,
                                  struct blob_reader *r)
{
   struct pvr_cached_stage *stage = &obj->stage[slot];
   struct pvr_cache_io io = { .r = r, .ok = true };
   pco_data *data = &stage->data;

   /* Everything not read stays zero: no pointer from the blob is ever kept. */
   memset(data, 0, sizeof(*data));
   io_data(&io, pvr_cached_kind(obj->flags, slot), data);

   uint32_t bindings_present = blob_read_uint32(r);
   if (!io.ok || r->overrun ||
       (bindings_present & ~BITFIELD_MASK(PVR_MAX_DESCRIPTOR_SETS)))
      return false;

   for (unsigned u = 0; u < ARRAY_SIZE(data->common.desc_sets); ++u) {
      pco_descriptor_set_data *set = &data->common.desc_sets[u];

      if (!(bindings_present & BITFIELD_BIT(u))) {
         if (set->binding_count)
            return false;
         continue;
      }

      /* binding_count was bounded above. */
      set->bindings = rzalloc_array_size(obj->mem_ctx,
                                         sizeof(*set->bindings),
                                         set->binding_count);
      if (!set->bindings)
         return false;

      for (unsigned b = 0; b < set->binding_count; b++)
         io_binding(&io, &set->bindings[b]);
   }

   if (!io.ok || r->overrun)
      return false;

   if (slot == 0 && (obj->flags & PVR_CACHED_HAS_XFB) &&
       !io_xfb(&io, obj->mem_ctx, &data->vs.xfb_info))
      return false;

   stage->binary_size = blob_read_uint32(r);
   if (r->overrun || !stage->binary_size ||
       stage->binary_size > (size_t)(r->end - r->current) ||
       data->common.entry_offset > stage->binary_size)
      return false;

   stage->binary = ralloc_size(obj->mem_ctx, stage->binary_size);
   if (!stage->binary)
      return false;

   blob_copy_bytes(r, stage->binary, stage->binary_size);
   blob_copy_bytes(r, &stage->stats, sizeof(stage->stats));

   return !r->overrun && stage->stats.isa == PVR_STAT_ROGUE;
}

/* The flags say which shapes an object can have: a graphics one is a VS and
 * optionally an FS (plain or passthrough) and xfb info on the VS, a compute
 * one a CS and optionally its zero-init shader.
 */
static bool pvr_cached_flags_valid(uint32_t flags)
{
   if ((flags & ~PVR_CACHED_ALL_FLAGS) || !(flags & PVR_CACHED_HAS_STAGE0))
      return false;

   if (!(flags & PVR_CACHED_STAGE0_IS_VS))
      return !(flags & (PVR_CACHED_FS_PASSTHROUGH | PVR_CACHED_HAS_XFB));

   return !(flags & PVR_CACHED_FS_PASSTHROUGH) ||
          (flags & PVR_CACHED_HAS_STAGE1);
}

static bool pvr_cached_fs_inputs_valid(const struct pvr_cached_shaders *obj)
{
   if (obj->fs_varyings_used &
       ~BITFIELD64_RANGE(VARYING_SLOT_VAR0, MAX_VARYING))
      return false;

   if (obj->fs_varyings_used && !(obj->flags & PVR_CACHED_HAS_STAGE1))
      return false;

   for (unsigned i = 0; i < MAX_VARYING; i++) {
      const bool used = obj->fs_varyings_used & BITFIELD64_BIT(VARYING_SLOT_VAR0 + i);

      if (!used) {
         if (obj->fs_interp[i])
            return false;
         continue;
      }

      switch (obj->fs_interp[i]) {
      case INTERP_MODE_SMOOTH:
      case INTERP_MODE_NOPERSPECTIVE:
      case INTERP_MODE_FLAT:
         break;
      default:
         return false;
      }
   }

   return true;
}

static struct vk_pipeline_cache_object *
pvr_cached_shaders_deserialize(struct vk_pipeline_cache *cache,
                               const void *key_data,
                               size_t key_size,
                               struct blob_reader *r)
{
   struct pvr_device *device =
      container_of(cache->base.device, struct pvr_device, vk);

   if (key_size != PVR_CACHED_KEY_SIZE)
      return NULL;

   /* The last dword is the CRC of everything before it. */
   const size_t total = r->end - r->current;
   if (total < sizeof(uint32_t))
      return NULL;

   uint32_t crc;
   memcpy(&crc, r->end - sizeof(crc), sizeof(crc));
   if (util_hash_crc32(r->current, total - sizeof(crc)) != crc)
      return NULL;

   r->end -= sizeof(crc);

   if (blob_read_uint32(r) != PVR_CACHED_MAGIC)
      return NULL;

   uint32_t flags = blob_read_uint32(r);
   if (r->overrun || !pvr_cached_flags_valid(flags))
      return NULL;

   struct pvr_cached_shaders *obj =
      pvr_cached_shaders_create(device, key_data, flags);
   if (!obj)
      return NULL;

   for (unsigned s = 0; s < 2; ++s) {
      if ((flags & (PVR_CACHED_HAS_STAGE0 << s)) &&
          !pvr_cached_stage_read(obj, s, r))
         goto fail;
   }

   obj->fs_varyings_used = blob_read_uint64(r);
   blob_copy_bytes(r, obj->fs_interp, sizeof(obj->fs_interp));
   if (r->overrun || r->current != r->end || !pvr_cached_fs_inputs_valid(obj))
      goto fail;

   return &obj->base;

fail:
   vk_pipeline_cache_object_unref(&device->vk, &obj->base);
   return NULL;
}

/* XCLIPSE: checks of a hit against the pipeline that wants to use it. What
 * the compiler derived from the pipeline layout (descriptor set ranges, push
 * constant size, binding counts) must be what this layout gives, because the
 * draw code copies that many dwords from descriptor sets and push constants.
 * A hit that fails is not used.
 */
static bool pvr_range_is_zero(const pco_range *r)
{
   return !r->start && !r->count && !r->stride;
}

static bool pvr_cached_desc_sets_match(const pco_data *d,
                                       int stage, /* mesa_shader_stage or -1 */
                                       const struct vk_pipeline_layout *layout)
{
   for (unsigned u = 0; u < ARRAY_SIZE(d->common.desc_sets); u++) {
      const pco_descriptor_set_data *set = &d->common.desc_sets[u];
      const struct pvr_descriptor_set_layout *sl = NULL;

      if (stage >= 0 && u < layout->set_count && layout->set_layouts[u])
         sl = vk_to_pvr_descriptor_set_layout(layout->set_layouts[u]);

      if (!sl || !(sl->stage_flags & BITFIELD_BIT(stage))) {
         /* Not used by this stage: the compiler leaves it all zero. */
         if (set->bindings || set->binding_count || set->used ||
             !pvr_range_is_zero(&set->range) ||
             !pvr_range_is_zero(&set->dynamic_range))
            return false;
         continue;
      }

      if (!set->bindings || set->binding_count != sl->binding_count)
         return false;

      if (!set->used) {
         if (!pvr_range_is_zero(&set->range) ||
             !pvr_range_is_zero(&set->dynamic_range))
            return false;
         continue;
      }

      const unsigned dynamic_dw =
         (sl->dynamic_buffer_count * sizeof(struct pvr_buffer_descriptor)) /
         sizeof(uint32_t);

      if (set->range.count != sl->size / sizeof(uint32_t))
         return false;

      if (dynamic_dw ? set->dynamic_range.count != dynamic_dw
                     : !pvr_range_is_zero(&set->dynamic_range))
         return false;
   }

   return true;
}

static bool pvr_cached_push_consts_match(const pco_data *d,
                                         int stage,
                                         const struct vk_pipeline_layout *layout)
{
   const pco_push_const_data *pc = &d->common.push_consts;
   unsigned count = pc->used;

   if (!count || stage < 0)
      return !count && pvr_range_is_zero(&pc->range);

   if (count == ~0U) {
      count = 0;
      for (unsigned u = 0; u < layout->push_range_count; ++u) {
         const VkPushConstantRange *range = &layout->push_ranges[u];

         if (!(mesa_to_vk_shader_stage(stage) & range->stageFlags))
            continue;

         count = MAX2(count, range->offset + range->size);
      }

      count /= 4;
   }

   return pc->range.count == count &&
          count <= PVR_MAX_PUSH_CONSTANTS_SIZE / sizeof(uint32_t);
}

static bool pvr_cached_stage_matches(const struct pvr_cached_stage *stage,
                                     int mesa_stage,
                                     const struct vk_pipeline_layout *layout)
{
   return pvr_cached_desc_sets_match(&stage->data, mesa_stage, layout) &&
          pvr_cached_push_consts_match(&stage->data, mesa_stage, layout);
}

/* Tile buffer addresses are looked up by count / stride at draw time. */
static bool pvr_cached_tile_buffers_match(const pco_fs_data *fs)
{
   const pco_range *tb = &fs->tile_buffers;

   if (!tb->count)
      return !fs->num_tile_buffers && !tb->stride;

   return fs->num_tile_buffers <= PVR_MAX_TILE_BUFFER_COUNT &&
          tb->stride == sizeof(uint64_t) / sizeof(uint32_t) &&
          tb->count == fs->num_tile_buffers * tb->stride;
}

bool pvr_cached_shaders_check_graphics(const struct pvr_cached_shaders *obj,
                                       const struct vk_pipeline_layout *layout,
                                       bool has_fs,
                                       bool fs_passthrough)
{
   const uint32_t flags = obj->flags;

   if (!(flags & PVR_CACHED_STAGE0_IS_VS) ||
       !!(flags & PVR_CACHED_HAS_STAGE1) != has_fs ||
       !!(flags & PVR_CACHED_FS_PASSTHROUGH) != fs_passthrough)
      return false;

   const pco_data *vs = &obj->stage[0].data;
   const pco_data *fs = &obj->stage[1].data;

   if (!pvr_cached_stage_matches(&obj->stage[0], MESA_SHADER_VERTEX, layout))
      return false;

   if (vs->vs.xfb_info && !(flags & PVR_CACHED_HAS_XFB))
      return false;

   if (!has_fs)
      return !obj->fs_varyings_used;

   if (!pvr_cached_stage_matches(&obj->stage[1], MESA_SHADER_FRAGMENT, layout) ||
       !pvr_cached_tile_buffers_match(&fs->fs))
      return false;

   /* The coefficient program pairs each FS input with a VS output. */
   u_foreach_bit64 (varying, obj->fs_varyings_used) {
      const pco_range *cf = &fs->fs.varyings[varying];
      const pco_range *out = &vs->vs.varyings[varying];

      if (!cf->count || cf->count % ROGUE_USC_COEFFICIENT_SET_SIZE ||
          cf->start % ROGUE_USC_COEFFICIENT_SET_SIZE || !out->count ||
          out->start < 4)
         return false;
   }

   /* Two FPU iterators of position, point coord, then one per varying. */
   return 3 + util_bitcount64(obj->fs_varyings_used) <= PVR_MAXIMUM_ITERATIONS;
}

bool pvr_cached_shaders_check_compute(const struct pvr_cached_shaders *obj,
                                      const struct vk_pipeline_layout *layout)
{
   const uint32_t flags = obj->flags;

   if ((flags & ~(PVR_CACHED_HAS_STAGE0 | PVR_CACHED_HAS_STAGE1)) ||
       !(flags & PVR_CACHED_HAS_STAGE0))
      return false;

   const pco_cs_data *cs = &obj->stage[0].data.cs;

   /* The compile builds the zero-init shader exactly then. */
   if (!!(flags & PVR_CACHED_HAS_STAGE1) != (cs->zero_shmem && !cs->global_shmem))
      return false;

   if (!pvr_cached_stage_matches(&obj->stage[0], MESA_SHADER_COMPUTE, layout))
      return false;

   return !(flags & PVR_CACHED_HAS_STAGE1) ||
          pvr_cached_stage_matches(&obj->stage[1], -1, layout);
}

bool pvr_cache_dir_name_valid(const char *name)
{
   if (!name[0] || name[0] == '.' || !strchr(name, '.'))
      return false;

   for (const char *c = name; *c; c++) {
      if (!(*c >= 'a' && *c <= 'z') && !(*c >= 'A' && *c <= 'Z') &&
          !(*c >= '0' && *c <= '9') && *c != '.' && *c != '_')
         return false;
   }

   return true;
}

const struct vk_pipeline_cache_object_ops pvr_cached_shaders_ops = {
   .serialize = pvr_cached_shaders_serialize,
   .deserialize = pvr_cached_shaders_deserialize,
   .destroy = pvr_cached_shaders_destroy,
};

struct pvr_cached_shaders *
pvr_cached_shaders_lookup(struct vk_pipeline_cache *cache,
                          const uint8_t key[PVR_CACHED_KEY_SIZE],
                          bool *cache_hit)
{
   struct vk_pipeline_cache_object *base =
      vk_pipeline_cache_lookup_object(cache,
                                      key,
                                      PVR_CACHED_KEY_SIZE,
                                      &pvr_cached_shaders_ops,
                                      cache_hit);
   return base ? container_of(base, struct pvr_cached_shaders, base) : NULL;
}

struct pvr_cached_shaders *
pvr_cached_shaders_add(struct vk_pipeline_cache *cache,
                       struct pvr_cached_shaders *obj)
{
   struct vk_pipeline_cache_object *base =
      vk_pipeline_cache_add_object(cache, &obj->base);
   return container_of(base, struct pvr_cached_shaders, base);
}

void pvr_cached_shaders_unref(struct pvr_device *device,
                              struct pvr_cached_shaders *obj)
{
   vk_pipeline_cache_object_unref(&device->vk, &obj->base);
}
