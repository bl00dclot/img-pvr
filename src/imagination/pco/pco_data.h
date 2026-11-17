/*
 * Copyright © 2024 Imagination Technologies Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

#ifndef PCO_DATA_H
#define PCO_DATA_H

/**
 * \file pco_data.h
 *
 * \brief PCO shader-specific data/compiler-driver interface.
 */

#include "common/pvr_limits.h"
#include "compiler/shader_enums.h"
#include "nir/nir_lower_blend.h"
#include "nir/nir_xfb_info.h"
#include "util/format/u_format.h"

#include <stdbool.h>

#define PCO_POINT_SAMPLER 0xffff
#define PCO_IA_SAMPLER 0xfffe

/* Compiler-specific forward-declarations. */
typedef struct _pco_shader pco_shader;

/** Generic range struct. */
typedef struct _pco_range {
   unsigned start;
   unsigned count;
   union {
      unsigned stride;
      unsigned offset;
   };
} pco_range;

/** PCO vertex shader-specific data. */
typedef struct _pco_vs_data {
   /** Attributes/input mappings. */
   pco_range attribs[VERT_ATTRIB_MAX];

   enum pipe_format attrib_formats[VERT_ATTRIB_MAX];

   /** Varyings/output mappings. */
   pco_range varyings[VARYING_SLOT_MAX];

   unsigned clip_count;
   unsigned cull_count;

   nir_xfb_info *xfb_info;

   unsigned f32_smooth; /** Number of F32 linear varyings. */
   unsigned f32_flat; /** Number of F32 flat varyings. */
   unsigned f32_npc; /** Number of F32 NPC varyings. */

   unsigned f16_smooth; /** Number of F16 linear varyings. */
   unsigned f16_flat; /** Number of F16 flat varyings. */
   unsigned f16_npc; /** Number of F16 NPC varyings. */

   unsigned vtxouts; /** How many vertex outputs are written to. */
} pco_vs_data;

enum pco_front_face_op {
   PCO_FRONT_FACE_OP_NOP,
   PCO_FRONT_FACE_OP_SWAP,
   PCO_FRONT_FACE_OP_TRUE,
};

/** PCO fragment shader-specific data. */
typedef struct _pco_fs_data {
   /** Varyings/input mappings. */
   pco_range varyings[VARYING_SLOT_MAX];

   /** Results/output mappings. */
   pco_range outputs[FRAG_RESULT_MAX];

   /** If outputs are to be placed in tile buffers. */
   uint8_t output_tile_buffers;

   /** Fragment output formats. */
   enum pipe_format output_formats[FRAG_RESULT_MAX];

   /** On-chip input attachment mappings. */
   pco_range ias_onchip[8];

   /** On-chip input attachment formats. */
   enum pipe_format ia_formats[8];

   uint8_t ia_tile_buffers;
   uint8_t ia_has_stencil;

   unsigned num_tile_buffers;

   pco_range meta;

   /* XCLIPSE: set when the pipeline fixes the whole fs meta word (PVR_FS_META_*: no dynamic
    * multisample or colour-write state, one sample per pixel). The shader then uses meta_static as
    * a constant instead of testing the shared register per fragment, and with no discard left it
    * needs no ISP feedback, so it is not a punch-through object. */
   bool meta_is_static;
   uint32_t meta_static;

   /* XCLIPSE: internal shader that never runs per sample (the attachment load of a single-sampled
    * attachment): fragment x/y come from the pixel registers without the per-fragment test of the
    * coverage mask. */
   bool single_sampled;

   gl_frag_result z_replicate;

   /* Blend options. */
   nir_lower_blend_options blend_opts;
   pco_range blend_consts;
   uint8_t blend_consts_needed;

   uint16_t rasterization_samples;

   pco_range tile_buffers;

   pco_range sample_locations;

   gl_varying_slot view_index_slot;

   struct {
      bool w; /** Whether the shader uses pos.w. */
      bool z; /** Whether the shader uses pos.z */
      bool pntc; /** Whether the shader uses point coord. */
      bool phase_change; /** Whether the shader does a phase change. */
      bool fbfetch; /** Whether the shader fetches from the framebuffer. */
      bool depth_feedback;
      bool discard;
      bool sample_shading;
      bool sample_locations;
      bool alpha_to_coverage;
      bool olchk_skip;
   } uses;

   struct {
      bool color_write_enable;
   } meta_present;
} pco_fs_data;

/** PCO compute shader-specific data. */
typedef struct _pco_cs_data {
   unsigned workgroup_size[3]; /** Workgroup size. */

   pco_range shmem;
   bool zero_shmem;
   bool global_shmem;
} pco_cs_data;

/** PCO image descriptor metadata. */
enum pco_image_meta {
   PCO_IMAGE_META_LAYER_SIZE,
   PCO_IMAGE_META_BUFFER_ELEMS,
   PCO_IMAGE_META_Z_SLICE,
   PCO_IMAGE_META_PCK_INFO,

   PCO_IMAGE_META_COUNT,
};

/** PCO sampler descriptor metadata. */
enum pco_sampler_meta {
   PCO_SAMPLER_META_COMPARE_OP,
   PCO_SAMPLER_META_RSVD0,
   PCO_SAMPLER_META_RSVD1,
   PCO_SAMPLER_META_RSVD2,

   PCO_SAMPLER_META_COUNT,
};

/** PCO descriptor binding data. */
typedef struct _pco_binding_data {
   pco_range range; /** Descriptor location range. */
   bool used; /** Whether the descriptor binding is used by the shader. */

   /** Whether the descriptor binding is a combined image sampler. */
   bool is_img_smp;

   /** Whether the descriptor binding is an inline uniform block. */
   bool is_inline_ubo;

   /** XCLIPSE: whether the binding is a dynamic uniform/storage buffer. The fourth dword of every other buffer
    * descriptor (the dynamic offset) is always zero, so reads of those skip adding it. */
   bool is_dynamic_buffer;
} pco_binding_data;

/** PCO descriptor set data. */
typedef struct _pco_descriptor_set_data {
   pco_range range; /** Descriptor location range. */
   pco_range dynamic_range; /** Dynamic descriptor location range. */

   unsigned binding_count; /** Number of bindings. */
   pco_binding_data *bindings; /** Descriptor set bindings. */

   bool used; /** Whether the descriptor set is used by the shader. */
} pco_descriptor_set_data;

/** PCO push constant data. */
typedef struct _pco_push_const_data {
   pco_range range; /** Push constant range. */

   unsigned used;
} pco_push_const_data;

/* XCLIPSE: uniform-buffer ranges promoted to shared registers (pco_nir_ubo_push.c).
 * The compiler picks, per stage, the byte ranges of non-inline UBOs that are read at constant
 * offsets; the driver allocates shared registers for them and the descriptor PDS program DMAs
 * them there when the draw executes (the same way as inline uniform blocks and push constants).
 * Budget: at most PCO_UBO_PUSH_MAX_RANGES ranges (one DMA each) and PCO_UBO_PUSH_BUDGET dwords
 * per stage. 64 dwords is twice what the game's UBO reads (31); tiles in flight stay at 7 up to
 * about 570 shared dwords on a GE8320 (lanes/ubopush.md). */
#define PCO_UBO_PUSH_MAX_RANGES 8

/* Hard limits, whatever the budget: reads at byte offsets of 1024 or more are never promoted, and the ranges of a stage
 * never total more than this (the PDS DMA size field is 12 bits, the destination offset 13). */
#define PCO_UBO_PUSH_MAX_DW 256
#define PCO_UBO_PUSH_BUDGET 64

typedef struct _pco_ubo_range {
   unsigned desc_set;
   unsigned binding;
   unsigned elem; /** Array element of the binding. */
   unsigned offset; /** Dword offset into the buffer. */
   unsigned count; /** Dwords. */
   unsigned start; /** First shared register, set by the driver. */
   unsigned uses; /** Reads of the range's dwords in the shader (the driver drops the least dense range first). */
} pco_ubo_range;

/** PCO common data. */
typedef struct _pco_common_data {
   /** System value mappings. */
   pco_range sys_vals[SYSTEM_VALUE_MAX];

   /** Descriptor set data. */
   pco_descriptor_set_data desc_sets[PVR_MAX_DESCRIPTOR_SETS];

   /** Push constant data. */
   pco_push_const_data push_consts;

   /** UBO ranges read from shared registers (XCLIPSE, see PCO_UBO_PUSH_BUDGET). */
   unsigned ubo_range_count;
   pco_ubo_range ubo_ranges[PCO_UBO_PUSH_MAX_RANGES];

   pco_range point_sampler;
   pco_range ia_sampler;

   unsigned temps; /** Number of allocated temp registers. */
   unsigned vtxins; /** Number of allocated vertex input registers. */
   unsigned interns; /** Number of allocated internal registers. */

   unsigned spilled_temps;
   pco_range spill_info; /* addr_lo, addr_hi, block_size */

   unsigned scratch;
   pco_range scratch_info; /* addr_lo, addr_hi, block_size */

   unsigned coeffs; /** Number of allocated coefficient registers. */
   unsigned shareds; /** Number of allocated shared registers. */

   unsigned entry_offset; /** Offset of the shader entrypoint. */

   struct {
      bool atomics; /** Whether the shader uses atomics. */
      bool barriers; /** Whether the shader uses barriers. */
      bool side_effects; /** Whether the shader has side effects. */
      bool empty; /** Whether the shader is empty. */
      bool point_sampler; /** Whether the shader uses a point sampler. */
      bool ia_sampler; /** Does the shader use an input attachment sampler? */
      bool usclib; /** Does the shader use usclib functions? */
   } uses;

   bool robust_buffer_access;
   bool null_descriptor;
   bool image_2d_view_of_3d;
   bool image_sliced_view_of_3d;
   bool multiview;
} pco_common_data;

/** The promoted range holding dwords [offset, offset + count) of a UBO element, or NULL. */
static inline const pco_ubo_range *pco_find_ubo_range(const pco_common_data *common,
                                                      unsigned desc_set,
                                                      unsigned binding,
                                                      unsigned elem,
                                                      unsigned offset,
                                                      unsigned count)
{
   for (unsigned u = 0; u < common->ubo_range_count; ++u) {
      const pco_ubo_range *r = &common->ubo_ranges[u];
      if (r->desc_set == desc_set && r->binding == binding && r->elem == elem && offset >= r->offset &&
          offset + count <= r->offset + r->count)
         return r;
   }

   return NULL;
}

/**
 * XCLIPSE: gives the promoted ranges their shared registers after everything else a stage allocated, and drops ranges
 * (least reads per dword first) until the stage's shared registers fit in `limit`. A dropped range's reads stay `ld`:
 * the translation (pco_trans_nir.c) runs after this and only sees the ranges that are left. Needs common->shareds to be
 * the allocation without the ranges. A stage without a range is left untouched.
 *
 * \param[in,out] common Common data; ubo_ranges, ubo_range_count and shareds change.
 * \param[in] limit Most shared registers the stage may use.
 */
static inline void pco_fit_ubo_ranges(pco_common_data *common, unsigned limit)
{
   for (;;) {
      unsigned start = common->ubo_range_count ? (common->shareds + 3) & ~3u : common->shareds;
      unsigned total = start;
      for (unsigned u = 0; u < common->ubo_range_count; ++u)
         total += common->ubo_ranges[u].count;

      if (total <= limit || !common->ubo_range_count) {
         if (common->ubo_range_count) {
            common->shareds = start;
            for (unsigned u = 0; u < common->ubo_range_count; ++u) {
               common->ubo_ranges[u].start = common->shareds;
               common->shareds += common->ubo_ranges[u].count;
            }
         }

         return;
      }

      /* Lowest uses / count; on a tie the larger one (frees more). */
      unsigned worst = 0;
      for (unsigned u = 1; u < common->ubo_range_count; ++u) {
         const pco_ubo_range *a = &common->ubo_ranges[u], *w = &common->ubo_ranges[worst];
         const unsigned long long ad = (unsigned long long)a->uses * w->count, wd = (unsigned long long)w->uses * a->count;
         if (ad < wd || (ad == wd && a->count > w->count))
            worst = u;
      }

      for (unsigned u = worst; u + 1 < common->ubo_range_count; ++u)
         common->ubo_ranges[u] = common->ubo_ranges[u + 1];
      --common->ubo_range_count;
   }
}

/** PCO shader data. */
typedef struct _pco_data {
   union {
      pco_vs_data vs;
      pco_fs_data fs;
      pco_cs_data cs;
   };

   pco_common_data common;
} pco_data;

/** PCO precompiled shader data. */
typedef struct PACKED _pco_precomp_data {
   uint8_t temps;
   uint8_t vtxins;

   uint16_t coeffs : 12;
   uint16_t shareds : 12;

   uint16_t size_dwords;

   uint8_t pad[1];

   uint32_t binary[];
} pco_precomp_data;
static_assert(sizeof(pco_precomp_data) == 8, "sizeof(pco_precomp_data) != 8");

pco_precomp_data pco_get_precomp_data(pco_shader *shader);

struct pvr_stats pco_get_pvr_stats(pco_shader *shader);
#endif /* PCO_DATA_H */
