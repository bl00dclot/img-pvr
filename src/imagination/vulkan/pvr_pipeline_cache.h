/*
 * SPDX-License-Identifier: MIT
 *
 * XCLIPSE: compiled shaders kept in the common vk_pipeline_cache (memory,
 * VkPipelineCache data and the on-disk shader cache). One object holds the
 * compiler output of a whole pipeline: the VS and FS (or the CS and its
 * zero-init shader) with the pco_data the driver needs to build the PDS
 * programs, and the few things taken from the NIR after linking.
 */

#ifndef PVR_PIPELINE_CACHE_H
#define PVR_PIPELINE_CACHE_H

#include <stdbool.h>
#include <stdint.h>

#include "compiler/shader_enums.h"
#include "pco/pco_data.h"
#include "util/mesa-blake3.h"
#include "util/shader_stats.h"
#include "vk_pipeline_cache.h"

struct pvr_device;

#define PVR_CACHED_KEY_SIZE BLAKE3_OUT_LEN

/* Slot 0 is the VS (graphics) or the CS (compute); slot 1 is the FS
 * (graphics) or the coefficient-update shader of a compute pipeline.
 */
#define PVR_CACHED_HAS_STAGE0 BITFIELD_BIT(0)
#define PVR_CACHED_HAS_STAGE1 BITFIELD_BIT(1)
#define PVR_CACHED_STAGE0_IS_VS BITFIELD_BIT(2)
#define PVR_CACHED_FS_PASSTHROUGH BITFIELD_BIT(3)

struct pvr_cached_stage {
   pco_data data; /* Pointers inside point into the owning object. */
   void *binary;
   uint32_t binary_size;
   struct pvr_stats stats;
};

struct pvr_cached_shaders {
   struct vk_pipeline_cache_object base;
   uint8_t key[PVR_CACHED_KEY_SIZE];
   void *mem_ctx;
   uint32_t flags;
   struct pvr_cached_stage stage[2];

   /* Fragment inputs, from the linked FS NIR (coefficient PDS program). */
   uint64_t fs_varyings_used;
   uint8_t fs_interp[MAX_VARYING];
};

extern const struct vk_pipeline_cache_object_ops pvr_cached_shaders_ops;

struct pvr_cached_shaders *
pvr_cached_shaders_create(struct pvr_device *device,
                          const uint8_t key[PVR_CACHED_KEY_SIZE],
                          uint32_t flags);

/* Copies everything the stage needs out of the compiler's output. */
bool pvr_cached_shaders_set_stage(struct pvr_cached_shaders *obj,
                                  unsigned slot,
                                  const pco_data *data,
                                  const void *binary,
                                  uint32_t binary_size,
                                  const struct pvr_stats *stats);

/* Copy of the stage data whose ralloc'd members the caller owns and frees. */
bool pvr_cached_stage_clone_data(const struct pvr_cached_stage *stage,
                                 bool is_vs,
                                 pco_data *out);

struct pvr_cached_shaders *
pvr_cached_shaders_lookup(struct vk_pipeline_cache *cache,
                          const uint8_t key[PVR_CACHED_KEY_SIZE],
                          bool *cache_hit);

/* Consumes the caller's reference, returns the object now in the cache. */
struct pvr_cached_shaders *
pvr_cached_shaders_add(struct vk_pipeline_cache *cache,
                       struct pvr_cached_shaders *obj);

void pvr_cached_shaders_unref(struct pvr_device *device,
                              struct pvr_cached_shaders *obj);

#endif /* PVR_PIPELINE_CACHE_H */
