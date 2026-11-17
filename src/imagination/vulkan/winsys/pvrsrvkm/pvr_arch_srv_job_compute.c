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

#include "pvr_srv_job_compute.h"

#include "util/os_file.h"

#include "util/os_time.h"
#include "vk_log.h"

#include "fw-api/pvr_rogue_fwif.h"
#include "fw-api/pvr_rogue_fwif_ddk111.h"
#include "fw-api/pvr_rogue_fwif_ddk113.h"

#include "pvr_csb.h"
#include "pvr_device_info.h"
#include "pvr_srv.h"
#include "pvr_srv_bridge.h"

#if PVR_SRV_DDK == 113
/* docs/ddk113/CAPTURE.md: the same fields as 1.11 without tpu_yuv_csc_table, in 32 bytes. */
typedef struct ddk113_fwif_cmd_compute srv_fwif_cmd_compute;
typedef struct ddk113_fwif_cdm_regs srv_fwif_cdm_regs;
#else
typedef struct ddk111_fwif_cmd_compute srv_fwif_cmd_compute;
typedef struct ddk111_fwif_cdm_regs srv_fwif_cdm_regs;
#endif

/* XCLIPSE: DDK 1.11 command (fw-api/pvr_rogue_fwif_ddk111.h). The context state base is in the
 * context switch registers given at context creation, and the GE8320 has none of the optional
 * registers below, so they are only skipped. */
static uint32_t
pvr_srv_compute_cmd_stream_load(srv_fwif_cmd_compute *const cmd,
                                const uint8_t *const stream,
                                const uint32_t stream_len,
                                const struct pvr_device_info *const dev_info)
{
   const uint32_t *stream_ptr = (const uint32_t *)stream;
   srv_fwif_cdm_regs *const regs = &cmd->regs;
   uint32_t main_stream_len =
      pvr_csb_unpack((uint64_t *)stream_ptr, KMD_STREAM_HDR).length;

   stream_ptr += pvr_cmd_length(KMD_STREAM_HDR);

   memcpy(&regs->tpu_border_colour_table, stream_ptr, sizeof(uint64_t));
   stream_ptr += pvr_cmd_length(CR_TPU_BORDER_COLOUR_TABLE_CDM);

   memcpy(&regs->cdm_ctrl_stream_base, stream_ptr, sizeof(uint64_t));
   stream_ptr += pvr_cmd_length(CR_CDM_CTRL_STREAM_BASE);

   stream_ptr += pvr_cmd_length(CR_CDM_CONTEXT_STATE_BASE);

   cmd->cdm_resume_pds1 = *stream_ptr;
   stream_ptr += pvr_cmd_length(CR_CDM_CONTEXT_PDS1);

   if (PVR_HAS_FEATURE(dev_info, compute_morton_capable))
      stream_ptr += pvr_cmd_length(CR_CDM_ITEM);

   if (PVR_HAS_FEATURE(dev_info, cluster_grouping))
      stream_ptr += pvr_cmd_length(CR_COMPUTE_CLUSTER);

   if (PVR_HAS_FEATURE(dev_info, tpu_dm_global_registers))
      stream_ptr++;

   if (PVR_HAS_FEATURE(dev_info, gpu_multicore_support))
      stream_ptr++;

   assert((const uint8_t *)stream_ptr - stream <= stream_len);
   assert((const uint8_t *)stream_ptr - stream == main_stream_len);

   return main_stream_len;
}

static void pvr_srv_compute_cmd_ext_stream_load(
   srv_fwif_cmd_compute *const cmd,
   const uint8_t *const stream,
   const uint32_t stream_len,
   const uint32_t ext_stream_offset,
   const struct pvr_device_info *const dev_info)
{
   const uint32_t *ext_stream_ptr =
      (const uint32_t *)((uint8_t *)stream + ext_stream_offset);
   srv_fwif_cdm_regs *const regs = &cmd->regs;

   struct ROGUE_KMD_STREAM_EXTHDR_COMPUTE0 header0;

   header0 = pvr_csb_unpack(ext_stream_ptr, KMD_STREAM_EXTHDR_COMPUTE0);
   ext_stream_ptr += pvr_cmd_length(KMD_STREAM_EXTHDR_COMPUTE0);

   assert(PVR_HAS_QUIRK(dev_info, 49927) == header0.has_brn49927);
   if (header0.has_brn49927) {
      regs->tpu = *ext_stream_ptr;
      ext_stream_ptr += pvr_cmd_length(CR_TPU);
   }

   assert((const uint8_t *)ext_stream_ptr - stream == stream_len);
}

static void
srv_compute_cmd_init(const struct pvr_winsys_compute_submit_info *submit_info,
                     srv_fwif_cmd_compute *cmd,
                     const struct pvr_srv_winsys *srv_ws,
                     const struct pvr_device_info *const dev_info)
{
   uint32_t ext_stream_offset;

   memset(cmd, 0, sizeof(*cmd));

#if PVR_SRV_DDK != 113
   cmd->regs.tpu_yuv_csc_table = pvr_srv_ddk111_yuv_csc_table(srv_ws);
#endif

   ext_stream_offset =
      pvr_srv_compute_cmd_stream_load(cmd,
                                      submit_info->fw_stream,
                                      submit_info->fw_stream_len,
                                      dev_info);

   if (ext_stream_offset < submit_info->fw_stream_len) {
      pvr_srv_compute_cmd_ext_stream_load(cmd,
                                          submit_info->fw_stream,
                                          submit_info->fw_stream_len,
                                          ext_stream_offset,
                                          dev_info);
   }

   /* The 1.11 compute flags are unknown and the vendor sends 0, so prevent_all_overlap and
    * use_single_core (meaningless on one core) are not passed on. */

#if PVR_SRV_DDK != 113
   /* How the vendor derives its value is unknown; send the one it sends. */
   cmd->num_temp_regions = 0x27;
#endif
}

VkResult PVR_PER_ARCH(srv_winsys_compute_submit)(
   const struct pvr_winsys_compute_ctx *ctx,
   const struct pvr_winsys_compute_submit_info *submit_info,
   const struct pvr_device_info *const dev_info,
   struct vk_sync *signal_sync)
{
   const struct pvr_srv_winsys_compute_ctx *srv_ctx =
      to_pvr_srv_winsys_compute_ctx(ctx);
   const struct pvr_srv_winsys *srv_ws = to_pvr_srv_winsys(ctx->ws);
   srv_fwif_cmd_compute compute_cmd;
   struct pvr_srv_sync *srv_signal_sync;
   VkResult result;
   int in_fd = -1;
   int fence;

   srv_compute_cmd_init(submit_info, &compute_cmd, srv_ws, dev_info);

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

   /* XCLIPSE: same bound as the render kick: the queue drains at the GPU's pace, so 10 s of RETRY
    * answers means a hung GPU or firmware; report device loss instead of spinning for ever. */
   const int64_t retry_deadline = os_time_get_nano() + 10000000000ll;

   do {
      result = pvr_srv_rgx_kick_compute2(srv_ws->base.render_fd,
                                         srv_ctx->handle,
                                         0U,
                                         NULL,
                                         NULL,
                                         NULL,
                                         in_fd,
                                         srv_ctx->timeline,
                                         sizeof(compute_cmd),
                                         (uint8_t *)&compute_cmd,
                                         submit_info->job_num,
                                         0,
                                         NULL,
                                         NULL,
                                         0U,
                                         0U,
                                         0U,
                                         0U,
                                         "COMPUTE",
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

   return result;
}
