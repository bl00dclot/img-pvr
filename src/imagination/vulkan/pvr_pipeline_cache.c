/*
 * SPDX-License-Identifier: MIT
 */

#include "pvr_pipeline_cache.h"

#include <string.h>

#include "pvr_device.h"
#include "util/blob.h"
#include "util/crc32.h"
#include "util/macros.h"
#include "util/ralloc.h"
#include "vk_alloc.h"
#include "vk_device.h"

#define PVR_CACHED_MAGIC 0x31435650u /* "PVC1" */

/* Sanity bound on a descriptor set's binding count when reading cache data. */
#define PVR_CACHED_MAX_BINDINGS 4096u

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

static bool pvr_cached_shaders_serialize(struct vk_pipeline_cache_object *base,
                                         struct blob *blob)
{
   const struct pvr_cached_shaders *obj =
      container_of(base, struct pvr_cached_shaders, base);

   const size_t start = blob->size;

   blob_write_uint32(blob, PVR_CACHED_MAGIC);
   blob_write_uint32(blob, obj->flags);
   blob_write_uint32(blob, sizeof(pco_data));

   for (unsigned s = 0; s < 2; ++s) {
      if (!(obj->flags & (PVR_CACHED_HAS_STAGE0 << s)))
         continue;

      const struct pvr_cached_stage *stage = &obj->stage[s];
      const bool is_vs = s == 0 && (obj->flags & PVR_CACHED_STAGE0_IS_VS);
      pco_data raw = stage->data;
      uint32_t bindings_present = 0;
      uint32_t xfb_size = 0;

      for (unsigned u = 0; u < ARRAY_SIZE(raw.common.desc_sets); ++u) {
         if (raw.common.desc_sets[u].bindings)
            bindings_present |= BITFIELD_BIT(u);
         raw.common.desc_sets[u].bindings = NULL;
      }

      if (is_vs && raw.vs.xfb_info) {
         xfb_size = nir_xfb_info_size(raw.vs.xfb_info->output_count);
         raw.vs.xfb_info = NULL;
      }

      blob_write_bytes(blob, &raw, sizeof(raw));
      blob_write_uint32(blob, bindings_present);
      for (unsigned u = 0; u < ARRAY_SIZE(raw.common.desc_sets); ++u) {
         if (bindings_present & BITFIELD_BIT(u)) {
            const pco_descriptor_set_data *set =
               &stage->data.common.desc_sets[u];
            blob_write_bytes(blob,
                             set->bindings,
                             set->binding_count * sizeof(*set->bindings));
         }
      }

      blob_write_uint32(blob, xfb_size);
      if (xfb_size)
         blob_write_bytes(blob, stage->data.vs.xfb_info, xfb_size);

      blob_write_uint32(blob, stage->binary_size);
      blob_write_bytes(blob, stage->binary, stage->binary_size);
      blob_write_bytes(blob, &stage->stats, sizeof(stage->stats));
   }

   blob_write_uint64(blob, obj->fs_varyings_used);
   blob_write_bytes(blob, obj->fs_interp, sizeof(obj->fs_interp));

   /* XCLIPSE: the VkPipelineCache data has no checksum of its own. The
    * payload is raw compiler data (register ranges, formats), so a flipped
    * bit must be rejected here and not turn into a bad shader. Absent when
    * only the size is queried (no buffer).
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
   const bool is_vs = slot == 0 && (obj->flags & PVR_CACHED_STAGE0_IS_VS);

   blob_copy_bytes(r, &stage->data, sizeof(stage->data));
   uint32_t bindings_present = blob_read_uint32(r);
   if (r->overrun)
      return false;

   /* Everything read from the blob is untrusted: pointers were written as
    * NULL and sizes are checked against what is left. */
   for (unsigned u = 0; u < ARRAY_SIZE(stage->data.common.desc_sets); ++u) {
      pco_descriptor_set_data *set = &stage->data.common.desc_sets[u];

      set->bindings = NULL;
      if (!(bindings_present & BITFIELD_BIT(u)))
         continue;

      if (set->binding_count > PVR_CACHED_MAX_BINDINGS)
         return false;

      size_t size = (size_t)set->binding_count * sizeof(*set->bindings);
      if (size > (size_t)(r->end - r->current))
         return false;

      set->bindings = ralloc_size(obj->mem_ctx, size);
      if (!set->bindings)
         return false;

      blob_copy_bytes(r, set->bindings, size);
   }

   uint32_t xfb_size = blob_read_uint32(r);
   if (is_vs)
      stage->data.vs.xfb_info = NULL;

   if (xfb_size) {
      if (!is_vs || xfb_size < sizeof(nir_xfb_info) ||
          xfb_size > (size_t)(r->end - r->current))
         return false;

      nir_xfb_info *xfb = ralloc_size(obj->mem_ctx, xfb_size);
      if (!xfb)
         return false;

      blob_copy_bytes(r, xfb, xfb_size);
      if (r->overrun || nir_xfb_info_size(xfb->output_count) != xfb_size)
         return false;

      stage->data.vs.xfb_info = xfb;
   }

   stage->binary_size = blob_read_uint32(r);
   if (r->overrun || !stage->binary_size ||
       stage->binary_size > (size_t)(r->end - r->current))
      return false;

   stage->binary = ralloc_size(obj->mem_ctx, stage->binary_size);
   if (!stage->binary)
      return false;

   blob_copy_bytes(r, stage->binary, stage->binary_size);
   blob_copy_bytes(r, &stage->stats, sizeof(stage->stats));

   return !r->overrun;
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
   if (blob_read_uint32(r) != sizeof(pco_data) || r->overrun ||
       !(flags & PVR_CACHED_HAS_STAGE0))
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
   if (r->overrun || r->current != r->end)
      goto fail;

   return &obj->base;

fail:
   vk_pipeline_cache_object_unref(&device->vk, &obj->base);
   return NULL;
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
