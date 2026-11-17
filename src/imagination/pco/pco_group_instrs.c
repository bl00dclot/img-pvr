/*
 * Copyright © 2024 Imagination Technologies Ltd.
 *
 * SPDX-License-Identifier: MIT
 */

/**
 * \file pco_group_instrs.c
 *
 * \brief PCO instruction grouping pass.
 */

#include "hwdef/rogue_hw_defs.h"
#include "pco.h"
#include "pco_builder.h"
#include "pco_internal.h"
#include "pco_map.h"
#include "util/macros.h"

#include <stdbool.h>

/**
 * \brief Calculates the decode-assist value for an instruction group.
 *
 * \param[in] igrp PCO instruction group.
 * \return The decode-assist value.
 */
static inline unsigned calc_da(pco_igrp *igrp)
{
   unsigned da = igrp->enc.len.hdr;
   bool no_srcs_dests = !igrp->enc.len.lower_srcs &&
                        !igrp->enc.len.upper_srcs && !igrp->enc.len.dests;

   switch (igrp->hdr.alutype) {
   case PCO_ALUTYPE_MAIN:
   case PCO_ALUTYPE_BITWISE: {
      pco_foreach_phase_rev (p) {
         if (igrp->hdr.alutype == PCO_ALUTYPE_BITWISE || p > PCO_OP_PHASE_1)
            da += igrp->enc.len.instrs[p];
      }
      break;
   }

   case PCO_ALUTYPE_CONTROL:
      if (no_srcs_dests)
         return 0;

      da += igrp->enc.len.instrs[PCO_OP_PHASE_CTRL];
      break;

   default:
      UNREACHABLE("");
   }

   return da;
}

/**
 * \brief Calculates the lengths for an instruction group.
 *
 * \param[in,out] igrp PCO instruction group.
 * \param[in,out] offset_bytes The cumulative shader offset (in bytes).
 */
static inline void calc_lengths(pco_igrp *igrp, unsigned *offset_bytes)
{
   unsigned total_length = 0;

   igrp->enc.len.hdr = pco_igrp_hdr_bytes(igrp->variant.hdr);
   total_length += igrp->enc.len.hdr;

   igrp->enc.len.lower_srcs = pco_src_bytes(igrp->variant.lower_src);
   total_length += igrp->enc.len.lower_srcs;

   igrp->enc.len.upper_srcs = pco_src_bytes(igrp->variant.upper_src);
   total_length += igrp->enc.len.upper_srcs;

   igrp->enc.len.iss = pco_iss_bytes(igrp->variant.iss);
   total_length += igrp->enc.len.iss;

   igrp->enc.len.dests = pco_dst_bytes(igrp->variant.dest);
   total_length += igrp->enc.len.dests;

   pco_foreach_phase_in_igrp (igrp, phase) {
      switch (igrp->hdr.alutype) {
      case PCO_ALUTYPE_MAIN:
         if (phase == PCO_OP_PHASE_BACKEND) {
            igrp->enc.len.instrs[phase] =
               pco_backend_bytes(igrp->variant.instr[phase].backend);
         } else {
            igrp->enc.len.instrs[phase] =
               pco_main_bytes(igrp->variant.instr[phase].main);
         }
         break;

      case PCO_ALUTYPE_BITWISE:
         igrp->enc.len.instrs[phase] =
            pco_bitwise_bytes(igrp->variant.instr[phase].bitwise);
         break;

      case PCO_ALUTYPE_CONTROL:
         igrp->enc.len.instrs[phase] =
            pco_ctrl_bytes(igrp->variant.instr[phase].control);
         break;

      default:
         UNREACHABLE("");
      }

      total_length += igrp->enc.len.instrs[phase];
   }

   igrp->enc.len.word_padding = total_length % 2;
   total_length += igrp->enc.len.word_padding;

   igrp->enc.len.total = total_length;

   /* Set igrp header length and decode-assist. */
   igrp->hdr.length = igrp->enc.len.total / 2;
   igrp->hdr.da = calc_da(igrp);

   /* Set offset and update running offset byte count. */
   igrp->enc.offset = *offset_bytes;
   *offset_bytes += igrp->enc.len.total;
}

/**
 * \brief Calculates the alignment padding to be applied to
 *        the last instruction group in the shader.
 *
 * \param[in,out] last_igrp The last instruction group.
 * \param[in,out] offset_bytes The cumulative shader offset (in bytes).
 */
static inline void calc_align_padding(pco_igrp *last_igrp,
                                      unsigned *offset_bytes)
{
   /* We should never end up with a completely empty shader. */
   assert(last_igrp);

   unsigned total_align = last_igrp->enc.len.total % ROGUE_ICACHE_ALIGN;
   unsigned offset_align = last_igrp->enc.offset % ROGUE_ICACHE_ALIGN;

   if (total_align) {
      unsigned padding = ROGUE_ICACHE_ALIGN - total_align;
      *offset_bytes += padding;

      /* Pad the size of the last igrp. */
      last_igrp->enc.len.align_padding += padding;
      last_igrp->enc.len.total += padding;

      /* Update the last igrp header length. */
      last_igrp->hdr.length = last_igrp->enc.len.total / 2;
   }

   if (offset_align) {
      unsigned padding = ROGUE_ICACHE_ALIGN - offset_align;
      *offset_bytes += padding;

      /* Pad the size of the penultimate igrp. */
      pco_igrp *penultimate_igrp = pco_prev_igrp(last_igrp);

      /* If we only have one igrp then its offset will be zero. */
      assert(penultimate_igrp);

      penultimate_igrp->enc.len.align_padding += padding;
      penultimate_igrp->enc.len.total += padding;

      /* Update the penultimate igrp header length. */
      penultimate_igrp->hdr.length = penultimate_igrp->enc.len.total / 2;

      /* Update the offset of the last igrp. */
      last_igrp->enc.offset += padding;
   }
}

/**
 * \brief Converts a PCO instruction to an instruction group.
 *
 * \param[in] b PCO builder.
 * \param[in] instr PCO instruction.
 * \param[out] igrp PCO instruction group.
 * \param[in,out] offset_bytes The cumulative shader offset (in bytes).
 */
static void pco_instr_to_igrp(pco_builder *b,
                              pco_instr *instr,
                              pco_igrp *igrp,
                              unsigned *offset_bytes)
{
   pco_map_igrp(igrp, instr);
   calc_lengths(igrp, offset_bytes);
   pco_builder_insert_igrp(b, igrp);
}


/* XCLIPSE: late co-issue. A main instruction group has phase 0 and phase 1
 * slots (ISA reference, "Main Instruction Group"): FMAD, FADD, FMUL and MBYP
 * exist in both, on separate source sets (S0-S2 and S3-S5) and with separate
 * feedthrough registers (FT0, FT1) that drive W0 and W1. Two such instructions
 * that do not depend on each other therefore execute in one group, as
 * pco_mbyp2 already does for two bypasses.
 *
 * This runs after register allocation (so it cannot change the register
 * count): the pair is accepted only if the registers are free of RAW/WAW
 * hazards, the second instruction can be moved up across everything between
 * the two, and the group's source and destination encodings exist. */

#define PCO_COISSUE_WINDOW 6

static inline bool coissue_ref_ok(pco_ref ref, bool is_dest, bool is_p1)
{
   if (!pco_ref_is_hwreg(ref) || pco_ref_get_chans(ref) != 1 ||
       pco_ref_get_bits(ref) != 32)
      return false;

   switch (pco_ref_get_reg_class(ref)) {
   case PCO_REG_CLASS_TEMP:
      return true;

   case PCO_REG_CLASS_COEFF:
      /* XCLIPSE: not as a phase 1 (S3-S5) source. Bisected on the device with a
       * pair counter: dEQP-VK draw.*.linear_interpolation.* fail (3 of 3 runs)
       * with a pair that reads cf5/cf6 as S3/S5, and pass with every other pair
       * formed. The upper source group's coefficient bank encoding is not
       * established, so only S0-S2 take coefficients. */
      return !is_dest && !is_p1;

   case PCO_REG_CLASS_VTXIN:
   case PCO_REG_CLASS_SHARED:
      /* XCLIPSE: the same restriction, because no shader run on the device has
       * read these as S3-S5 (the bench and probe shaders, whose pixel sums are
       * checked, only use constants there); the bank encoding for them is
       * unproven. */
      return !is_dest && !is_p1;

   case PCO_REG_CLASS_CONST:
      /* XCLIPSE: constants are proven in S3-S5: the vkbench blend/alu/tv/c1/c2
       * and probe shaders read them there (sc0, sc64, sc75-77, sc84, sc141) and
       * their pixel sums are unchanged. */
      return !is_dest;

   default:
      return false;
   }
}

/** Whether an instruction is a plain main-ALU op with no modifiers that need the group header. */
static inline bool coissue_instr_ok(const pco_instr *instr, bool allow_end, bool is_p1)
{
   switch (instr->op) {
   case PCO_OP_FMAD:
   case PCO_OP_FADD:
   case PCO_OP_FMUL:
   case PCO_OP_MBYP:
      break;

   default:
      return false;
   }

   if (pco_instr_has_olchk(instr) && pco_instr_get_olchk(instr))
      return false;
   if (pco_instr_has_atom(instr) && pco_instr_get_atom(instr))
      return false;
   if (pco_instr_has_rpt(instr) && pco_instr_get_rpt(instr) != 1)
      return false;
   if (pco_instr_has_exec_cnd(instr) &&
       pco_instr_get_exec_cnd(instr) != PCO_EXEC_CND_E1_ZX)
      return false;
   if (!allow_end && pco_instr_has_end(instr) && pco_instr_get_end(instr))
      return false;

   if (instr->num_dests != 1 || !coissue_ref_ok(instr->dest[0], true, is_p1))
      return false;

   for (unsigned s = 0; s < instr->num_srcs; ++s)
      if (!coissue_ref_ok(instr->src[s], false, is_p1))
         return false;

   return true;
}

/** Whether an instruction may be moved across (pure register-to-register work). */
static inline bool coissue_passable(const pco_instr *instr)
{
   switch (instr->op) {
   case PCO_OP_FMAD:
   case PCO_OP_FADD:
   case PCO_OP_FMUL:
   case PCO_OP_MBYP:
   case PCO_OP_MOVI32:
      break;

   default:
      return false;
   }

   if (pco_instr_has_olchk(instr) && pco_instr_get_olchk(instr))
      return false;
   if (pco_instr_has_atom(instr) && pco_instr_get_atom(instr))
      return false;
   if (pco_instr_has_end(instr) && pco_instr_get_end(instr))
      return false;
   if (pco_instr_has_exec_cnd(instr) &&
       pco_instr_get_exec_cnd(instr) != PCO_EXEC_CND_E1_ZX)
      return false;

   for (unsigned d = 0; d < instr->num_dests; ++d)
      if (!pco_ref_is_hwreg(instr->dest[d]) ||
          pco_ref_get_reg_class(instr->dest[d]) != PCO_REG_CLASS_TEMP)
         return false;

   return true;
}

static inline bool coissue_reg_overlap(pco_ref a, pco_ref b)
{
   if (!pco_ref_is_hwreg(a) || !pco_ref_is_hwreg(b))
      return false;

   if (pco_ref_get_reg_class(a) != pco_ref_get_reg_class(b))
      return false;

   unsigned a0 = pco_ref_get_reg_index(a);
   unsigned a1 = a0 + pco_ref_get_chans(a) * (pco_ref_get_bits(a) == 64 ? 2 : 1);
   unsigned b0 = pco_ref_get_reg_index(b);
   unsigned b1 = b0 + pco_ref_get_chans(b) * (pco_ref_get_bits(b) == 64 ? 2 : 1);

   return a0 < b1 && b0 < a1;
}

/** Whether instr writes something that other reads. */
static inline bool coissue_writes_read(const pco_instr *w, const pco_instr *r)
{
   for (unsigned d = 0; d < w->num_dests; ++d)
      for (unsigned s = 0; s < r->num_srcs; ++s)
         if (coissue_reg_overlap(w->dest[d], r->src[s]))
            return true;

   return false;
}

static inline bool coissue_writes_write(const pco_instr *a, const pco_instr *b)
{
   for (unsigned x = 0; x < a->num_dests; ++x)
      for (unsigned y = 0; y < b->num_dests; ++y)
         if (coissue_reg_overlap(a->dest[x], b->dest[y]))
            return true;

   return false;
}

/** Whether `j` can be moved up to just after `i` across the instructions between. */
static bool coissue_can_move_across(pco_block *block,
                                    const pco_instr *i,
                                    const pco_instr *j)
{
   for (struct list_head *pos = i->link.next; pos != &j->link; pos = pos->next) {
      const pco_instr *k = list_entry(pos, pco_instr, link);

      if (!coissue_passable(k))
         return false;

      /* RAW (j reads k's result), WAR (j overwrites k's source), WAW. */
      if (coissue_writes_read(k, j) || coissue_writes_read(j, k) ||
          coissue_writes_write(k, j))
         return false;
   }

   return true;
}

/** Whether `i` (phase 0) and `j` (phase 1) form an encodable group. */
static bool coissue_encodable(const pco_instr *i, const pco_instr *j)
{
   pco_igrp tmp = { 0 };
   tmp.hdr.alutype = PCO_ALUTYPE_MAIN;

   for (unsigned s = 0; s < i->num_srcs; ++s)
      tmp.srcs.s[0 + s] = i->src[s];
   for (unsigned s = 0; s < j->num_srcs; ++s)
      tmp.srcs.s[3 + s] = j->src[s];

   tmp.dests.w[0] = i->dest[0];
   tmp.dests.w[1] = j->dest[0];

   enum pco_src_variant sv;
   enum pco_dst_variant dv;
   return pco_igrp_src_variant_fits(&tmp, false, &sv) &&
          pco_igrp_src_variant_fits(&tmp, true, &sv) &&
          pco_igrp_dest_variant_fits(&tmp, &dv);
}

static pco_instr *coissue_find_mate(pco_block *block, pco_instr *i)
{
   if (!coissue_instr_ok(i, false, false))
      return NULL;

   unsigned n = 0;
   for (struct list_head *pos = i->link.next;
        pos != &block->instrs && n < PCO_COISSUE_WINDOW;
        pos = pos->next, ++n) {
      pco_instr *j = list_entry(pos, pco_instr, link);

      /* The end flag may only sit on a group that stays last. */
      bool adjacent = pos == i->link.next;
      if (coissue_instr_ok(j, adjacent, true) &&
          !coissue_writes_read(i, j) && !coissue_writes_write(i, j) &&
          coissue_can_move_across(block, i, j) && coissue_encodable(i, j))
         return j;

      /* Anything that is not plain ALU work stops the search. */
      if (!coissue_passable(j))
         return NULL;
   }

   return NULL;
}

/**
 * \brief Hoists constant loads to the front of the run of plain ALU work they
 *        sit in, so that the instructions around them can pair up.
 *
 * Only moves a movi32 across instructions it has no register hazard with, so
 * the allocation stays valid.
 */
static void coissue_hoist_imms(pco_block *block)
{
   pco_foreach_instr_in_block_safe (m, block) {
      if (m->op != PCO_OP_MOVI32 || !coissue_passable(m))
         continue;

      struct list_head *before = &m->link;
      for (struct list_head *pos = m->link.prev; pos != &block->instrs;
           pos = pos->prev) {
         const pco_instr *k = list_entry(pos, pco_instr, link);

         if (!coissue_passable(k) || coissue_writes_read(k, m) ||
             coissue_writes_read(m, k) || coissue_writes_write(k, m))
            break;

         before = pos;
      }

      if (before != &m->link) {
         list_del(&m->link);
         list_addtail(&m->link, before);
      }
   }
}

static inline unsigned coissue_main_variant(pco_instr *instr)
{
   switch (instr->op) {
   case PCO_OP_FMAD:
      return pco_fmad_variant(instr);
   case PCO_OP_FADD:
      return pco_fadd_variant(instr);
   case PCO_OP_FMUL:
      return pco_fmul_variant(instr);
   case PCO_OP_MBYP:
      return pco_mbyp_variant(instr);
   default:
      break;
   }

   UNREACHABLE("");
}

/** Moves a pair of independent main-ALU instructions into one phase 0/1 group. */
static void pco_pair_to_igrp(pco_builder *b,
                             pco_instr *i,
                             pco_instr *j,
                             pco_igrp *igrp,
                             unsigned *offset_bytes)
{
   bool end = pco_instr_has_end(j) && pco_instr_get_end(j);
   if (end)
      pco_instr_set_mod(j, PCO_OP_MOD_END, 0);

   igrp->hdr.oporg = PCO_OPORG_P0_P1;
   igrp->hdr.olchk = false;
   igrp->hdr.w1p = true;
   igrp->hdr.w0p = true;
   igrp->hdr.cc = pco_map_exec_cnd_to_cc(PCO_EXEC_CND_E1_ZX);
   igrp->hdr.end = end;
   igrp->hdr.atom = false;
   igrp->hdr.rpt = 1;
   igrp->hdr.alutype = PCO_ALUTYPE_MAIN;

   pco_instr *instrs[2] = { i, j };
   for (unsigned p = 0; p < 2; ++p) {
      pco_instr *instr = instrs[p];

      list_del(&instr->link);
      igrp->instrs[p] = instr;
      instr->phase = p;
      instr->parent_igrp = igrp;
      ralloc_steal(igrp, instr);

      for (unsigned s = 0; s < instr->num_srcs; ++s) {
         igrp->srcs.s[3 * p + s] = instr->src[s];
         instr->src[s] = pco_ref_io(PCO_IO_S0 + 3 * p + s);
         pco_ref_xfer_mods(&instr->src[s], &igrp->srcs.s[3 * p + s], true);
      }

      igrp->iss.is[4 + p] = pco_ref_io(PCO_IO_FT0 + p);

      igrp->dests.w[p] = instr->dest[0];
      instr->dest[0] = pco_ref_io(PCO_IO_FT0 + p);
      pco_ref_xfer_mods(&instr->dest[0], &igrp->dests.w[p], true);
   }

   igrp->variant.instr[PCO_OP_PHASE_0].main = coissue_main_variant(i);
   igrp->variant.instr[PCO_OP_PHASE_1].main = coissue_main_variant(j);

   igrp->variant.hdr = pco_igrp_hdr_variant(igrp);
   igrp->variant.lower_src = pco_igrp_src_variant(igrp, false);
   igrp->variant.upper_src = pco_igrp_src_variant(igrp, true);
   igrp->variant.iss = pco_igrp_iss_variant(igrp);
   igrp->variant.dest = pco_igrp_dest_variant(igrp);

   calc_lengths(igrp, offset_bytes);
   pco_builder_insert_igrp(b, igrp);
}

/**
 * \brief Groups PCO instructions into instruction groups.
 *
 * \param[in,out] shader PCO shader.
 * \return True if the pass made progress.
 */
bool pco_group_instrs(pco_shader *shader)
{
   pco_builder b;
   pco_igrp *igrp = NULL;
   unsigned offset_bytes = 0;

   assert(!shader->is_grouped);

   pco_foreach_func_in_shader (func, shader) {
      /* TODO: double check that *start* alignment is satisfied by
       * calc_align_padding when having multiple functions?
       */
      pco_foreach_block_in_func (block, func) {
         b = pco_builder_create(func, pco_cursor_before_block(block));

         /* XCLIPSE: PCO_DEBUG=no_coissue turns the pairing off (A/B). */
         bool coissue = !PCO_DEBUG(NO_COISSUE);
         if (coissue)
            coissue_hoist_imms(block);

         /* Only instructions are in the list here; groups go in front. */
         struct list_head *pos = block->instrs.next;
         while (pos != &block->instrs) {
            pco_instr *instr = list_entry(pos, pco_instr, link);
            pos = pos->next;

            pco_instr *mate = coissue ? coissue_find_mate(block, instr) : NULL;
            igrp = pco_igrp_create(func);
            if (mate) {
               if (&mate->link == pos)
                  pos = pos->next;
               pco_pair_to_igrp(&b, instr, mate, igrp, &offset_bytes);
            } else {
               pco_instr_to_igrp(&b, instr, igrp, &offset_bytes);
            }
         }
      }

      /* Ensure the final instruction group has a total size and offset
       * that are a multiple of the icache alignment.
       */
      calc_align_padding(igrp, &offset_bytes);
   }

   shader->is_grouped = true;
   return true;
}
