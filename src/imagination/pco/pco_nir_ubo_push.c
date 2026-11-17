/*
 * Copyright © 2026 Imagination Technologies Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * \file pco_nir_ubo_push.c
 *
 * \brief Picks the uniform-buffer ranges that are read at constant offsets.
 *
 * XCLIPSE: like Intel's brw_nir_analyze_ubo_ranges and Freedreno's ir3_nir_analyze_ubo_ranges. A
 * regular UBO read costs a 64-bit address, an ld and a wdf per use and per pixel. The ranges
 * chosen here are loaded into shared registers by the descriptor PDS program before the draw
 * runs (pvr_pds_generate_descriptor_upload_program), and the translation reads them from there
 * (trans_load_buffer), as it already does for inline uniform blocks and push constants. Reads at
 * run-time offsets, of run-time indexed array elements and of ranges that do not fit the budget
 * stay ld.
 *
 * The pass only records the ranges: the shared registers are not known yet, the driver assigns
 * them (pvr_setup_descriptors) and the translation rewrites each covered load.
 */

#include "nir.h"
#include "pco.h"
#include "pco_internal.h"
#include "util/macros.h"
#include "util/u_debug.h"

#include <assert.h>
#include <stdbool.h>
#include <string.h>

/* Constant-offset reads further into a block than this are not considered. */
#define MAX_WINDOW_DW PCO_UBO_PUSH_MAX_DW

/* Holes of up to this many unread dwords are bridged: a range costs one DMA, a hole a register. */
#define MAX_GAP_DW 3

#define MAX_KEYS 16

struct block_use {
   unsigned desc_set;
   unsigned binding;
   unsigned elem;
   uint8_t uses[MAX_WINDOW_DW]; /* loads covering each dword, saturating */
};

struct candidate {
   struct block_use *block;
   unsigned offset;
   unsigned count;
   unsigned uses;
};

static struct block_use *
find_block(struct block_use *blocks, unsigned *count, unsigned set, unsigned binding, unsigned elem)
{
   for (unsigned u = 0; u < *count; ++u)
      if (blocks[u].desc_set == set && blocks[u].binding == binding && blocks[u].elem == elem)
         return &blocks[u];

   if (*count == MAX_KEYS)
      return NULL;

   struct block_use *b = &blocks[(*count)++];
   memset(b, 0, sizeof(*b));
   b->desc_set = set;
   b->binding = binding;
   b->elem = elem;
   return b;
}

/**
 * \brief Records the ranges of non-inline UBOs read at constant offsets in a shader.
 *
 * Does nothing with PCO_DEBUG=no_ubo_push, for internal shaders without a descriptor layout, and
 * with nullDescriptor (a null buffer has no address to DMA from).
 *
 * \param[in] nir NIR shader, at the end of the NIR pipeline (nothing may change its UBO reads
 *                afterwards).
 * \param[in,out] data Shader data; common.ubo_ranges is filled in.
 */
void pco_nir_analyze_ubo_ranges(nir_shader *nir, pco_data *data)
{
   pco_common_data *common = &data->common;

   common->ubo_range_count = 0;

   if (PCO_DEBUG(NO_UBO_PUSH) || common->null_descriptor)
      return;

   struct block_use blocks[MAX_KEYS];
   unsigned block_count = 0;

   nir_foreach_function_impl (impl, nir) {
      nir_foreach_block (block, impl) {
         nir_foreach_instr (instr, block) {
            if (instr->type != nir_instr_type_intrinsic)
               continue;

            nir_intrinsic_instr *intr = nir_instr_as_intrinsic(instr);
            if (intr->intrinsic != nir_intrinsic_load_ubo)
               continue;

            /* Both the descriptor (set, binding, element) and the byte offset must be constants. */
            if (!nir_src_is_const(intr->src[0]) || !nir_src_is_const(intr->src[1]))
               continue;

            if (intr->def.bit_size != 32)
               continue;

            uint32_t packed_desc = nir_src_comp_as_uint(intr->src[0], 0);
            unsigned elem = nir_src_comp_as_uint(intr->src[0], 1);
            uint64_t byte_offset = nir_src_as_uint(intr->src[1]);
            unsigned desc_set, binding;
            pco_unpack_desc(packed_desc, &desc_set, &binding);

            if (byte_offset % 4 || byte_offset / 4 + intr->def.num_components > MAX_WINDOW_DW)
               continue;

            if (desc_set >= ARRAY_SIZE(common->desc_sets))
               continue;

            const pco_descriptor_set_data *set = &common->desc_sets[desc_set];
            if (!set->bindings || binding >= set->binding_count)
               continue;

            /* Inline uniform blocks are in shared registers already. */
            if (set->bindings[binding].is_inline_ubo)
               continue;

            struct block_use *b = find_block(blocks, &block_count, desc_set, binding, elem);
            if (!b)
               continue;

            for (unsigned c = 0; c < intr->def.num_components; ++c) {
               uint8_t *u = &b->uses[byte_offset / 4 + c];
               *u = MIN2(*u + 1, UINT8_MAX);
            }
         }
      }
   }

   /* Runs of read dwords (bridging small holes) are the candidates. */
   struct candidate cands[MAX_KEYS * 8];
   unsigned cand_count = 0;

   for (unsigned k = 0; k < block_count; ++k) {
      struct block_use *b = &blocks[k];

      for (unsigned d = 0; d < MAX_WINDOW_DW;) {
         if (!b->uses[d]) {
            ++d;
            continue;
         }

         unsigned start = d;
         unsigned end = d; /* last read dword */
         unsigned uses = 0;
         for (unsigned e = d; e < MAX_WINDOW_DW && e <= end + 1 + MAX_GAP_DW; ++e) {
            if (b->uses[e]) {
               end = e;
               uses += b->uses[e];
            }
         }

         d = end + 1;

         if (cand_count == ARRAY_SIZE(cands))
            continue;

         cands[cand_count++] = (struct candidate){
            .block = b,
            .offset = start,
            .count = end - start + 1,
            .uses = uses,
         };
      }
   }

   /* Densest first (reads per register), then the most read. */
   for (unsigned i = 1; i < cand_count; ++i) {
      struct candidate c = cands[i];
      unsigned j = i;
      while (j > 0) {
         const struct candidate *p = &cands[j - 1];
         /* p->uses / p->count < c.uses / c.count */
         bool before = (uint64_t)c.uses * p->count > (uint64_t)p->uses * c.count ||
                       ((uint64_t)c.uses * p->count == (uint64_t)p->uses * c.count && c.uses > p->uses);
         if (!before)
            break;
         cands[j] = cands[j - 1];
         --j;
      }
      cands[j] = c;
   }

   /* PCO_UBO_PUSH_BUDGET=<dwords> overrides the budget (the board-4 measurement at 32/64/128; the driver default is 64).
    * A negative value is not a budget (it would wrap to a huge unsigned one): the default applies. */
   long env_budget = debug_get_num_option("PCO_UBO_PUSH_BUDGET", PCO_UBO_PUSH_BUDGET);
   unsigned budget = env_budget < 0 ? PCO_UBO_PUSH_BUDGET : MIN2((unsigned long)env_budget, MAX_WINDOW_DW);
   for (unsigned i = 0; i < cand_count && budget && common->ubo_range_count < PCO_UBO_PUSH_MAX_RANGES; ++i) {
      struct candidate c = cands[i];

      /* A run larger than what is left: the window of that size with the most reads, cut to its first and
       * last read dword (the game's std shader reads 76 dwords of one block). */
      if (c.count > budget) {
         unsigned best = 0, best_sum = 0, sum = 0;
         for (unsigned e = 0; e < c.count; ++e) {
            sum += c.block->uses[c.offset + e];
            if (e >= budget)
               sum -= c.block->uses[c.offset + e - budget];
            if (e + 1 >= budget && sum > best_sum) {
               best_sum = sum;
               best = e + 1 - budget;
            }
         }

         unsigned first = c.offset + best, last = first + budget - 1;
         while (!c.block->uses[first])
            ++first;
         while (!c.block->uses[last])
            --last;
         c.offset = first;
         c.count = last - first + 1;
         c.uses = best_sum;
      }

      budget -= c.count;
      common->ubo_ranges[common->ubo_range_count++] = (pco_ubo_range){
         .desc_set = c.block->desc_set,
         .binding = c.block->binding,
         .elem = c.block->elem,
         .offset = c.offset,
         .count = c.count,
         .start = ~0u, /* the driver allocates the shared registers */
         .uses = c.uses,
      };
   }

   /* Stable order for the allocation and the PDS program: set, binding, element, offset. */
   for (unsigned i = 1; i < common->ubo_range_count; ++i) {
      pco_ubo_range r = common->ubo_ranges[i];
      unsigned j = i;
      while (j > 0) {
         const pco_ubo_range *p = &common->ubo_ranges[j - 1];
         bool before = r.desc_set != p->desc_set   ? r.desc_set < p->desc_set
                       : r.binding != p->binding   ? r.binding < p->binding
                       : r.elem != p->elem         ? r.elem < p->elem
                                                   : r.offset < p->offset;
         if (!before)
            break;
         common->ubo_ranges[j] = common->ubo_ranges[j - 1];
         --j;
      }
      common->ubo_ranges[j] = r;
   }
}
