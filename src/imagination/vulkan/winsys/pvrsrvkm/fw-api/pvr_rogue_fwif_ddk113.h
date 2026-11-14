/*
 * SPDX-License-Identifier: MIT
 *
 * The firmware structures that services DDK 1.13 (1.13ED5776728, the GE8320 of the Redmi A2) shares
 * with userspace through the kernel: the context switch registers and static context states that
 * the kernel copies verbatim into the firmware contexts. Layouts from include/rogue/rgx_fwif_shared.h
 * of that DDK (Imagination Technologies Ltd, Dual MIT/GPLv2, used here under MIT), see
 * docs/ddk113/ABI.md section 4.
 *
 * Copyright (c) Imagination Technologies Ltd. All Rights Reserved.
 *
 * The contents of the source files these definitions come from are dual licensed (MIT or GPLv2) and
 * are used here under the MIT license:
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
 * associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
 * NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES
 * OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The client kick commands (TA, 3D, transfer, compute) are never seen by the kernel: their 1.13
 * layouts come from captures of the vendor driver (docs/ddk113/CAPTURE.md), checked against the
 * sizes in the vendor alignment array. Only the compute command is defined so far.
 */

#ifndef PVR_ROGUE_FWIF_DDK113_H
#define PVR_ROGUE_FWIF_DDK113_H

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

/* RGXFWIF_TAREGISTERS_CSWITCH */
struct ddk113_fwif_ta_regs_cswitch {
   uint64_t vdm_context_state_base_addr;
   uint64_t vdm_context_state_resume_addr;
   uint64_t ta_context_state_base_addr;
   struct {
      uint64_t vdm_context_store_task0;
      uint64_t vdm_context_store_task1;
      uint64_t vdm_context_store_task2;
      uint64_t vdm_context_resume_task0;
      uint64_t vdm_context_resume_task1;
      uint64_t vdm_context_resume_task2;
      uint64_t vdm_context_store_task3;
      uint64_t vdm_context_store_task4;
      uint64_t vdm_context_resume_task3;
      uint64_t vdm_context_resume_task4;
   } ta_state[2];
};

/* RGXFWIF_CDM_REGISTERS_CSWITCH: the context state base first, then the registers 1.17 has. */
struct ddk113_fwif_cdm_regs_cswitch {
   uint64_t cdm_context_state_base_addr;
   uint64_t cdm_context_pds0;
   uint64_t cdm_context_pds1;
   uint64_t cdm_terminate_pds;
   uint64_t cdm_terminate_pds1;
   uint64_t cdm_resume_pds0;
   uint64_t cdm_context_pds0_b;
   uint64_t cdm_resume_pds0_b;
};

/* RGXFWIF_STATIC_RENDERCONTEXT_STATE: the bridge rejects anything larger
 * (KM: generated/rogue/rgxta3d_bridge/server_rgxta3d_bridge.c). */
struct ddk113_fwif_static_rendercontext_state {
   struct ddk113_fwif_ta_regs_cswitch ctx_switch_regs;
};

/* RGXFWIF_STATIC_COMPUTECONTEXT_STATE */
struct ddk113_fwif_static_computecontext_state {
   struct ddk113_fwif_cdm_regs_cswitch ctx_switch_regs;
};

static_assert(sizeof(struct ddk113_fwif_ta_regs_cswitch) == 184, "RGXFWIF_TAREGISTERS_CSWITCH size");
static_assert(sizeof(struct ddk113_fwif_static_rendercontext_state) == 184,
              "RGXFWIF_STATIC_RENDERCONTEXT_STATE size");
static_assert(sizeof(struct ddk113_fwif_cdm_regs_cswitch) == 64, "RGXFWIF_CDM_REGISTERS_CSWITCH size");
static_assert(sizeof(struct ddk113_fwif_static_computecontext_state) == 64,
              "RGXFWIF_STATIC_COMPUTECONTEXT_STATE size");

/* Client command sizes from the vendor 1.13 alignment array (unconfirmed against the firmware,
 * docs/ddk113/CAPTURE.md). TA and 3D start with the 12-byte CMDTA3D_SHARED block the kernel
 * patches (HWRT data, ZS and MSAA PR buffers), padded to 16. */
#define DDK113_FWIF_CMDTA_SIZE 80U
#define DDK113_FWIF_CMDTA_REGS_OFFSET 16U
#define DDK113_FWIF_CMD3D_SIZE 320U
#define DDK113_FWIF_CMD3D_REGS_OFFSET 16U
#define DDK113_FWIF_CMDTRANSFER_SIZE 152U
#define DDK113_FWIF_CMD_COMPUTE_SIZE 32U

/* RGXFWIF_CDM_REGS as the vendor fills it (docs/ddk113/CAPTURE.md, compute): 1.17's register set
 * without the user mode queue registers, the context state base (in the static state, see
 * ddk113_fwif_cdm_regs_cswitch) and the optional registers the GE8320 lacks. Its last register,
 * cdm_resume_pds1, is kept in the command (as in ddk111_fwif_cmd_compute) so the shared stream
 * loader fills both layouts; packed to 4 so it follows tpu directly, as in the capture. */
struct ddk113_fwif_cdm_regs {
   uint64_t tpu_border_colour_table;
   uint64_t cdm_ctrl_stream_base;
   /* CR_TPU, BRN 49927: the vendor sends tag_cem_4k_face_packing (0x20), as Mesa does. */
   uint32_t tpu;
} __attribute__((packed, aligned(4)));

/* RGXFWIF_CMD_COMPUTE. The last two words are zero in every vendor compute kick captured. 1.17
 * has flags then num_temp_regions after the registers; the 1.13 names are not confirmed. */
struct ddk113_fwif_cmd_compute {
   struct ddk113_fwif_cdm_regs regs;
   uint32_t cdm_resume_pds1;
   uint32_t flags;
   uint32_t unknown_1c;
};

static_assert(sizeof(struct ddk113_fwif_cmd_compute) == DDK113_FWIF_CMD_COMPUTE_SIZE,
              "RGXFWIF_CMD_COMPUTE size");
static_assert(offsetof(struct ddk113_fwif_cmd_compute, regs) == 0, "CMD_COMPUTE sCDMRegs");
static_assert(offsetof(struct ddk113_fwif_cmd_compute, regs.tpu) == 0x10, "CMD_COMPUTE tpu");
static_assert(offsetof(struct ddk113_fwif_cmd_compute, cdm_resume_pds1) == 0x14,
              "CMD_COMPUTE cdm_resume_pds1");

#endif /* PVR_ROGUE_FWIF_DDK113_H */
