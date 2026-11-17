/*
 * Copyright © 2022 Imagination Technologies Ltd.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "pvr_srv_job_transfer.h"

#include "util/os_file.h"

#include "util/os_time.h"
#include "vk_log.h"
#include "vk_util.h"

#include "fw-api/pvr_rogue_fwif.h"
#include "fw-api/pvr_rogue_fwif_ddk111.h"
#include "fw-api/pvr_rogue_fwif_ddk113.h"

#include "pvr_csb.h"
#include "pvr_device_info.h"
#include "pvr_srv.h"
#include "pvr_srv_bridge.h"

#if PVR_SRV_DDK == 113
/* docs/ddk113/CAPTURE.md: 1.17's register order with 6 PBE words, in 152 bytes. */
typedef struct ddk113_fwif_cmd_transfer srv_fwif_cmd_transfer;
typedef struct ddk113_fwif_transfer_regs srv_fwif_transfer_regs;
#else
typedef struct ddk111_fwif_cmd_transfer srv_fwif_cmd_transfer;
typedef struct ddk111_fwif_transfer_regs srv_fwif_transfer_regs;
#endif

/* XCLIPSE: TQ_PREP_FLAGS_* of DDK 1.11 (rgx_tq_shared.h), unchanged in 1.13. They only steer server syncs and pdump
 * in the kernel; set them as the vendor does. */
#define DDK111_TQ_PREP_FLAGS_PDUMPCONTINUOUS (1U << 4)
#define DDK111_TQ_PREP_FLAGS_START (1U << 5)
#define DDK111_TQ_PREP_FLAGS_END (1U << 6)

/* XCLIPSE: DDK 1.11 or 1.13 command (fw-api/pvr_rogue_fwif_ddk11[13].h). isp_rgn has a slot
 * only in 1.13; the GE8320 has neither SIPF v2 nor ipf_creq_pf, so Mesa always packs it as 0. */
static void
srv_transfer_cmd_stream_load(srv_fwif_cmd_transfer *const cmd,
                             const uint8_t *const stream,
                             const uint32_t stream_len,
                             const struct pvr_device_info *const dev_info)
{
   const uint32_t *stream_ptr = (const uint32_t *)stream;
   srv_fwif_transfer_regs *const regs = &cmd->regs;
   uint32_t main_stream_len =
      pvr_csb_unpack((uint64_t *)stream_ptr, KMD_STREAM_HDR).length;

   stream_ptr += pvr_cmd_length(KMD_STREAM_HDR);

   memcpy(&regs->pds_bgnd0_base, stream_ptr, sizeof(regs->pds_bgnd0_base));
   stream_ptr += pvr_cmd_length(CR_PDS_BGRND0_BASE);

   memcpy(&regs->pds_bgnd1_base, stream_ptr, sizeof(regs->pds_bgnd1_base));
   stream_ptr += pvr_cmd_length(CR_PDS_BGRND1_BASE);

   memcpy(&regs->pds_bgnd3_sizeinfo,
          stream_ptr,
          sizeof(regs->pds_bgnd3_sizeinfo));
   stream_ptr += pvr_cmd_length(CR_PDS_BGRND3_SIZEINFO);

   memcpy(&regs->isp_mtile_base, stream_ptr, sizeof(regs->isp_mtile_base));
   stream_ptr += pvr_cmd_length(CR_ISP_MTILE_BASE);

   STATIC_ASSERT(sizeof(regs->pbe_wordx_mrty[0]) == sizeof(uint64_t));
#if PVR_SRV_DDK == 113
   /* Mesa streams 9 words but writes render target i at [2i, 2i + 1] (pvr_arch_job_transfer.c),
    * so with at most 3 targets the last 3 are never written and stay zero. A debug-build check. */
   STATIC_ASSERT(ARRAY_SIZE(regs->pbe_wordx_mrty) ==
                 PVR_TRANSFER_MAX_RENDER_TARGETS *
                    ROGUE_NUM_PBESTATE_REG_WORDS_FOR_TRANSFER);
   memcpy(regs->pbe_wordx_mrty, stream_ptr, sizeof(regs->pbe_wordx_mrty));
   for (uint32_t i = ARRAY_SIZE(regs->pbe_wordx_mrty) * 2U; i < 9U * 2U; i++)
      assert(stream_ptr[i] == 0);
#else
   STATIC_ASSERT(ARRAY_SIZE(regs->pbe_wordx_mrty) == 9U);
   memcpy(regs->pbe_wordx_mrty, stream_ptr, sizeof(regs->pbe_wordx_mrty));
#endif
   stream_ptr += 9U * 2U;

   regs->isp_bgobjvals = *stream_ptr;
   stream_ptr += pvr_cmd_length(CR_ISP_BGOBJVALS);

   regs->usc_pixel_output_ctrl = *stream_ptr;
   stream_ptr += pvr_cmd_length(CR_USC_PIXEL_OUTPUT_CTRL);

   regs->usc_clear_register0 = *stream_ptr;
   stream_ptr += pvr_cmd_length(CR_USC_CLEAR_REGISTER);

   regs->usc_clear_register1 = *stream_ptr;
   stream_ptr += pvr_cmd_length(CR_USC_CLEAR_REGISTER);

   regs->usc_clear_register2 = *stream_ptr;
   stream_ptr += pvr_cmd_length(CR_USC_CLEAR_REGISTER);

   regs->usc_clear_register3 = *stream_ptr;
   stream_ptr += pvr_cmd_length(CR_USC_CLEAR_REGISTER);

   regs->isp_mtile_size = *stream_ptr;
   stream_ptr += pvr_cmd_length(CR_ISP_MTILE_SIZE);

   regs->isp_render_origin = *stream_ptr;
   stream_ptr += pvr_cmd_length(CR_ISP_RENDER_ORIGIN);

   regs->isp_ctl = *stream_ptr;
   stream_ptr += pvr_cmd_length(CR_ISP_CTL);

   regs->isp_aa = *stream_ptr;
   stream_ptr += pvr_cmd_length(CR_ISP_AA);

   regs->event_pixel_pds_info = *stream_ptr;
   stream_ptr += pvr_cmd_length(CR_EVENT_PIXEL_PDS_INFO);

   regs->event_pixel_pds_code = *stream_ptr;
   stream_ptr += pvr_cmd_length(CR_EVENT_PIXEL_PDS_CODE);

   regs->event_pixel_pds_data = *stream_ptr;
   stream_ptr += pvr_cmd_length(CR_EVENT_PIXEL_PDS_DATA);

   regs->isp_render = *stream_ptr;
   stream_ptr += pvr_cmd_length(CR_ISP_RENDER);

   assert(*stream_ptr == 0);
#if PVR_SRV_DDK == 113
   regs->isp_rgn = *stream_ptr;
#endif
   stream_ptr++;

   if (PVR_HAS_FEATURE(dev_info, gpu_multicore_support))
      stream_ptr++;

   assert((const uint8_t *)stream_ptr - stream == stream_len);
   assert((const uint8_t *)stream_ptr - stream == main_stream_len);
}

static void pvr_srv_transfer_cmds_init(
   const struct pvr_winsys_transfer_submit_info *submit_info,
   srv_fwif_cmd_transfer *cmds,
   uint32_t cmd_count,
   const struct pvr_device_info *const dev_info)
{
   memset(cmds, 0, sizeof(*cmds) * submit_info->cmd_count);

   for (uint32_t i = 0; i < cmd_count; i++) {
      const struct pvr_winsys_transfer_cmd *submit_cmd = &submit_info->cmds[i];
      srv_fwif_cmd_transfer *cmd = &cmds[i];

      srv_transfer_cmd_stream_load(cmd,
                                   submit_cmd->fw_stream,
                                   submit_cmd->fw_stream_len,
                                   dev_info);
   }
}

VkResult PVR_PER_ARCH(srv_winsys_transfer_submit)(
   const struct pvr_winsys_transfer_ctx *ctx,
   const struct pvr_winsys_transfer_submit_info *submit_info,
   const struct pvr_device_info *const dev_info,
   struct vk_sync *signal_sync)
{
   const struct pvr_srv_winsys_transfer_ctx *srv_ctx =
      to_pvr_srv_winsys_transfer_ctx(ctx);
   const struct pvr_srv_winsys *srv_ws = to_pvr_srv_winsys(ctx->ws);

   srv_fwif_cmd_transfer
      *cmds_ptr_arr[PVR_TRANSFER_MAX_PREPARES_PER_SUBMIT];
   uint32_t *update_sync_offsets[PVR_TRANSFER_MAX_PREPARES_PER_SUBMIT] = { 0 };
   uint32_t client_update_count[PVR_TRANSFER_MAX_PREPARES_PER_SUBMIT] = { 0 };
   void **update_ufo_syc_prims[PVR_TRANSFER_MAX_PREPARES_PER_SUBMIT] = { 0 };
   uint32_t *update_values[PVR_TRANSFER_MAX_PREPARES_PER_SUBMIT] = { 0 };
   uint32_t cmd_flags[PVR_TRANSFER_MAX_PREPARES_PER_SUBMIT] = { 0 };
   uint32_t cmd_sizes[PVR_TRANSFER_MAX_PREPARES_PER_SUBMIT];

   struct pvr_srv_sync *srv_signal_sync;
   uint32_t job_num;
   VkResult result;
   int in_fd = -1;
   int fence;

   STACK_ARRAY(srv_fwif_cmd_transfer,
               transfer_cmds,
               submit_info->cmd_count);
   if (!transfer_cmds)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   pvr_srv_transfer_cmds_init(submit_info,
                              transfer_cmds,
                              submit_info->cmd_count,
                              dev_info);

   for (uint32_t i = 0U; i < submit_info->cmd_count; i++) {
      cmd_sizes[i] = sizeof(**cmds_ptr_arr);
      cmds_ptr_arr[i] = &transfer_cmds[i];
      cmd_flags[i] = DDK111_TQ_PREP_FLAGS_PDUMPCONTINUOUS;
   }
   cmd_flags[0] |= DDK111_TQ_PREP_FLAGS_START;
   cmd_flags[submit_info->cmd_count - 1] |= DDK111_TQ_PREP_FLAGS_END;

   if (submit_info->wait) {
      struct pvr_srv_sync *srv_wait_sync = to_srv_sync(submit_info->wait);

      if (srv_wait_sync->fd >= 0) {
         in_fd = os_dupfd_cloexec(srv_wait_sync->fd);
         if (in_fd == -1) {
            return vk_errorf(NULL,
                             VK_ERROR_OUT_OF_HOST_MEMORY,
                             "dup called on wait sync failed, Errno: %s",
                             strerror(errno));
         }
      }
   }

   job_num = submit_info->job_num;

   /* XCLIPSE: same bound as the render kick: the queue drains at the GPU's pace, so 10 s of RETRY
    * answers means a hung GPU or firmware; report device loss instead of spinning for ever. */
   const int64_t retry_deadline = os_time_get_nano() + 10000000000ll;

   do {
      result = pvr_srv_rgx_submit_transfer2(srv_ws->base.render_fd,
                                            srv_ctx->handle,
                                            submit_info->cmd_count,
                                            client_update_count,
                                            update_ufo_syc_prims,
                                            update_sync_offsets,
                                            update_values,
                                            in_fd,
                                            -1,
                                            srv_ctx->timeline_3d,
                                            "TRANSFER",
                                            cmd_sizes,
                                            (uint8_t **)cmds_ptr_arr,
                                            cmd_flags,
                                            job_num,
                                            0U,
                                            NULL,
                                            NULL,
                                            NULL,
                                            &fence);
      if (result == VK_NOT_READY) {
         if (os_time_get_nano() > retry_deadline) {
            result = VK_ERROR_DEVICE_LOST;
            break;
         }
         os_time_sleep(100);
      }
   } while (result == VK_NOT_READY);

   if (result != VK_SUCCESS)
      goto end_close_in_fd;

   if (signal_sync) {
      srv_signal_sync = to_srv_sync(signal_sync);
      pvr_srv_set_sync_payload(srv_signal_sync, fence);
   } else if (fence != -1) {
      close(fence);
   }

end_close_in_fd:
   if (in_fd >= 0)
      close(in_fd);

   STACK_ARRAY_FINISH(transfer_cmds);

   return result;
}
