/*
 * Copyright 2026 JimVulkan
 * SPDX-License-Identifier: MIT
 *
 * The firmware structures that services DDK 1.11 (1.11ED5425693, the GE8320 of the Redmi 9C)
 * shares with userspace, where they differ from the 1.17 ones in pvr_rogue_fwif_shared.h:
 * the context switch registers and the static context states the kernel copies into the
 * firmware context. Layouts from services/include/rgx_fwif_shared.h of that DDK (Imagination
 * Technologies Ltd, Dual MIT/GPLv2).
 */

#ifndef PVR_ROGUE_FWIF_DDK111_H
#define PVR_ROGUE_FWIF_DDK111_H

#include <assert.h>
#include <stddef.h>
#include <stdint.h>

/* RGXFWIF_TAREGISTERS_CSWITCH: passed on its own (ui32CtxSwitchSize, pRegs) when a render
 * context is created, not inside the static state as in 1.17. */
struct ddk111_fwif_ta_regs_cswitch {
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

/* RGXFWIF_CDM_REGISTERS_CSWITCH: the context state base comes first, and two registers for
 * GPUs with two phantoms come last. */
struct ddk111_fwif_cdm_regs_cswitch {
   uint64_t cdm_context_state_base_addr;
   uint64_t cdm_context_pds0;
   uint64_t cdm_context_pds1;
   uint64_t cdm_terminate_pds;
   uint64_t cdm_terminate_pds1;
   uint64_t cdm_resume_pds0;
   uint64_t cdm_context_pds0_b;
   uint64_t cdm_resume_pds0_b;
   uint64_t cdm_terminate_pds_ph2;
   uint64_t cdm_terminate_pds1_ph2;
};

/* RGXFWIF_STATIC_RENDERCONTEXT_STATE */
struct ddk111_fwif_static_rendercontext_state {
   uint64_t robustness_address;
   uint32_t max_ta_deadline_ms;
   uint32_t max_3d_deadline_ms;
};

/* RGXFWIF_STATIC_COMPUTECONTEXT_STATE */
struct ddk111_fwif_static_computecontext_state {
   uint64_t robustness_address;
   struct ddk111_fwif_cdm_regs_cswitch ctx_switch_regs;
};

/*
 * The client kick commands. Their definitions (rgx_fwif_client.h) were never published for
 * 1.11, so these layouts are recovered from the vendor driver of the Redmi 9C:
 *
 * - The sizes and the register block offsets come from the align-check table in the vendor's
 *   libsrv_um.so, which the firmware checks at connect: RGXFWIF_CMDTA 128 bytes, RGXFWIF_CMD3D
 *   328, RGXFWIF_CMDTRANSFER 208, RGXFWIF_CMD_COMPUTE 64, each with its registers at offset 0.
 *   There is no cmd_common/cmd_ta_3d_shared header in front as in 1.17.
 * - The fields come from the vendor's own kicks (probe/pvrspy.c, captures in cap/vendor), matched
 *   by value against Mesa's 1.17 commands for the same jobs and against the GPU memory each
 *   address points at (PVRSPY_DUMP). A 3-target draw fixed the PBE stride of the 3D command.
 *
 * Where 1.17 has firmware-known fixed offsets in the heaps (YUV CSC table, pixel event PDS code,
 * VDM sync program), 1.11 passes the addresses in every command. The kernel does not patch these
 * commands either: the HWRT data and PR buffer firmware addresses are written by the client, from
 * the outputs of RGXCreateHWRTData and RGXCreateZSBuffer.
 *
 * Fields named unknown_* were zero or constant in every capture and have not been identified.
 * PDS addresses are offsets into the PDS code/data heap, USC addresses into the USC code heap,
 * and the TPU table registers hold the address >> 2, as in the hardware registers.
 */

#define DDK111_FWIF_PRBUFFER_ZBUFFER 0U
#define DDK111_FWIF_PRBUFFER_SBUFFER 1U
#define DDK111_FWIF_PRBUFFER_MSAABUFFER 2U
#define DDK111_FWIF_PRBUFFER_MAXSUPPORTED 3U

#define DDK111_FWIF_TA_PDS_CTRL 0x9300U

/* RGXFWIF_TAREGISTERS: every register takes a 64-bit slot. */
struct ddk111_fwif_ta_regs {
   uint64_t ppp_ctrl;
   uint64_t te_psg;
   uint64_t vdm_ctrl_stream_base;
   uint64_t tpu_border_colour_table;
   /* The YUV CSC coefficient table, ROGUE_FWIF_HEAP_FIXED_OFFSET_GENERAL_HEAP_YUV_CSC in 1.17. */
   uint64_t tpu_yuv_csc_table;
   uint64_t tpu;
   uint64_t vdm_context_resume_task0_size;
   /* CR_PDS_CTRL, loaded by the firmware on every TA kick. Bits 15:8 cap the vertex tasks per
    * USC: with a cap of 1, a draw that needs a second VS task deadlocks the TA. The vendor
    * driver sends DDK111_FWIF_TA_PDS_CTRL on every kick. */
   uint64_t pds_ctrl;
   /* The VDM sync PDS program: data segment here (one DOUTU constant pointing at a USC NOP
    * shader), code segment 32 bytes after it. 1.17: FWIF_PDS_HEAP_VDM_SYNC_OFFSET_BYTES. */
   uint64_t vdm_sync_pds_data;
};

/* RGXFWIF_CMDTA */
struct ddk111_fwif_cmd_ta {
   struct ddk111_fwif_ta_regs regs;
   /* FIRSTKICK | LASTKICK, and bit 20 set by the vendor on every kick. */
   uint32_t flags;
   uint32_t unknown_4c[3];
   uint32_t hw_rt_data;
   uint32_t pr_buffers[DDK111_FWIF_PRBUFFER_MAXSUPPORTED];
   struct {
      uint32_t ufo_addr;
      uint32_t value;
   } partial_render_ta_3d_fence;
   uint32_t unknown_70[4];
};

/* RGXFWIF_3DREGISTERS */
struct ddk111_fwif_3d_regs {
   uint32_t usc_pixel_output_ctrl;
   uint32_t usc_clear_register[4];
   uint32_t isp_bgobjdepth;
   uint32_t isp_bgobjvals;
   uint32_t isp_aa;
   uint32_t isp_ctl;
   uint32_t tpu;
   uint32_t event_pixel_pds_info;
   /* pixel_phantom or view_idx: zero for single-view renders. */
   uint32_t unknown_2c;
   /* The static pixel event PDS code (DOUTU; HALT), the same for the PR and the 3D command.
    * 1.17: FWIF_PDS_HEAP_EOT_OFFSET_BYTES. */
   uint32_t event_pixel_pds_code;
   uint32_t event_pixel_pds_data;

   uint64_t isp_scissor_base;
   uint64_t isp_dbias_base;
   uint64_t isp_oclqry_base;
   uint64_t isp_zlsctl;
   uint64_t isp_zload_store_base;
   uint64_t isp_stencil_load_store_base;
   /* Two PBE state words per render target (three in 1.17). */
   uint64_t pbe_word[8][2];
   uint64_t tpu_border_colour_table;
   uint64_t tpu_yuv_csc_table;
   uint64_t pds_bgnd[3];
   uint64_t pds_pr_bgnd[3];
};

/* RGXFWIF_CMD3D */
struct ddk111_fwif_cmd_3d {
   struct ddk111_fwif_3d_regs regs;
   /* 0x80000 (SPM scratch buffer) when pr_buffers[MSAABUFFER] is set. */
   uint32_t flags;
   uint32_t unknown_12c;
   uint32_t hw_rt_data;
   uint32_t pr_buffers[DDK111_FWIF_PRBUFFER_MAXSUPPORTED];
   /* Probably zls_stride and sls_stride (only non-zero for render target arrays). */
   uint32_t unknown_140[2];
};

/* RGXFWIF_TRANSFERREGISTERS */
struct ddk111_fwif_transfer_regs {
   uint32_t isp_bgobjvals;
   uint32_t usc_pixel_output_ctrl;
   uint32_t usc_clear_register0;
   uint32_t usc_clear_register1;
   uint32_t usc_clear_register2;
   uint32_t usc_clear_register3;
   uint32_t isp_mtile_size;
   uint32_t isp_render_origin;
   uint32_t isp_ctl;
   uint32_t isp_aa;
   uint32_t event_pixel_pds_info;
   uint32_t event_pixel_pds_code;
   uint32_t event_pixel_pds_data;
   uint32_t unknown_34;

   uint64_t pds_bgnd0_base;
   uint64_t pds_bgnd1_base;
   uint64_t pds_bgnd2_base;
   uint64_t pds_bgnd3_sizeinfo;
   uint64_t isp_mtile_base;
   /* 3 in every capture, as Mesa's isp_render. */
   uint64_t isp_render;
   /* isp_rgn is probably among these. */
   uint64_t unknown_68[3];
   uint64_t pbe_wordx_mrty[3 * 3];
};

/* RGXFWIF_CMDTRANSFER. The prepare flags the vendor passes with it are 0x70. */
struct ddk111_fwif_cmd_transfer {
   struct ddk111_fwif_transfer_regs regs;
   uint32_t flags;
   uint32_t padding;
};

/* RGXFWIF_CDM_REGS, without its last register: cdm_resume_pds1 is a 32-bit field at 0x20 and a
 * 64-bit aligned struct would pad it. */
struct ddk111_fwif_cdm_regs {
   uint64_t tpu_border_colour_table;
   uint64_t tpu_yuv_csc_table;
   uint64_t tpu;
   uint64_t cdm_ctrl_stream_base;
};

/* RGXFWIF_CMD_COMPUTE. The vendor leaves the last 20 bytes unwritten (stack garbage). */
struct ddk111_fwif_cmd_compute {
   struct ddk111_fwif_cdm_regs regs;
   uint32_t cdm_resume_pds1;
   /* Zero in the capture; taken to be the flags. */
   uint32_t flags;
   /* Taken to be num_temp_regions (1.17: "only used when feature
    * UNIFIED_STORE_VIRTUAL_PARTITIONING present"). The vendor sends 0x27 for a 64-invocation
    * workgroup writing one SSBO. With 0 the 1.11 firmware runs no CDM work at all (the kick
    * completes, nothing is written); 1 and 0x27 both work. */
   uint32_t num_temp_regions;
   uint32_t unknown_2c[5];
};

static_assert(sizeof(struct ddk111_fwif_cmd_ta) == 128, "RGXFWIF_CMDTA size");
static_assert(offsetof(struct ddk111_fwif_cmd_ta, hw_rt_data) == 0x58, "CMDTA hw_rt_data");
static_assert(sizeof(struct ddk111_fwif_cmd_3d) == 328, "RGXFWIF_CMD3D size");
static_assert(offsetof(struct ddk111_fwif_cmd_3d, hw_rt_data) == 0x130, "CMD3D hw_rt_data");
static_assert(offsetof(struct ddk111_fwif_3d_regs, pbe_word) == 0x68, "3D pbe_word");
static_assert(offsetof(struct ddk111_fwif_3d_regs, tpu_border_colour_table) == 0xe8,
              "3D tpu_border_colour_table");
static_assert(sizeof(struct ddk111_fwif_cmd_transfer) == 208, "RGXFWIF_CMDTRANSFER size");
static_assert(offsetof(struct ddk111_fwif_transfer_regs, pbe_wordx_mrty) == 0x80,
              "TQ pbe_wordx_mrty");
static_assert(sizeof(struct ddk111_fwif_cmd_compute) == 64, "RGXFWIF_CMD_COMPUTE size");
static_assert(offsetof(struct ddk111_fwif_cmd_compute, flags) == 0x24, "CMD_COMPUTE flags");

#endif /* PVR_ROGUE_FWIF_DDK111_H */
