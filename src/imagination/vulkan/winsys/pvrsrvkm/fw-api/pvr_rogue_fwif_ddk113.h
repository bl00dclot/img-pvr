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
 * The client kick commands (TA, 3D, transfer, compute) are not defined here yet: the kernel never
 * sees their layout, and the 1.13 one is still being established (docs/ddk113/CAPTURE.md). Their
 * sizes from the vendor alignment array are recorded below so the eventual definitions are
 * checked against them.
 */

#ifndef PVR_ROGUE_FWIF_DDK113_H
#define PVR_ROGUE_FWIF_DDK113_H

#include <assert.h>
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

#endif /* PVR_ROGUE_FWIF_DDK113_H */
