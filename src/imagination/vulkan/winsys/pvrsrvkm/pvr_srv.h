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

#ifndef PVR_SRV_H
#define PVR_SRV_H

#include "util/log.h"

#include <stdint.h>
#include <pthread.h>
#include <vulkan/vulkan.h>

#include "pvr_srv_sync.h"
#include "pvr_srv_sync_prim.h"
#include "pvr_winsys.h"
#include "util/macros.h"
#include "util/vma.h"

/*******************************************
   Misc defines
 *******************************************/

/* 64KB is MAX anticipated OS page size */
#define PVR_SRV_CARVEOUT_SIZE_GRANULARITY 0x10000

#define PVR_SRV_DEVMEM_HEAPNAME_MAXLENGTH 160

#define PVR_SRV_GENERAL_HEAP_IDENT "General"
#define PVR_SRV_PDSCODEDATA_HEAP_IDENT "PDS Code and Data"
#define PVR_SRV_RGNHDR_BRN_63142_HEAP_IDENT "RgnHdr BRN63142"
#define PVR_SRV_TRANSFER_3D_HEAP_IDENT "TQ3DParameters"
#define PVR_SRV_USCCODE_HEAP_IDENT "USC Code"
/* XCLIPSE: DDK 1.11 calls it "VisTest" (RGX_VISTEST_HEAP_IDENT in rgx_heaps.h). */
#define PVR_SRV_VISIBILITY_TEST_HEAP_IDENT "VisTest"

/* XCLIPSE: DDK 1.11 takes these addresses in every command instead of at firmware-known offsets,
 * and a PDS or USC address of 0 reads as "no program", so nothing sits at offset 0. */
#define FWIF_PDS_HEAP_TOTAL_BYTES 4096
#define FWIF_PDS_HEAP_VDM_SYNC_OFFSET_BYTES 256
#define FWIF_PDS_HEAP_EOT_OFFSET_BYTES 128
#define FWIF_GENERAL_HEAP_TOTAL_BYTES 4096
#define FWIF_USC_HEAP_TOTAL_BYTES 4096
#define FWIF_USC_HEAP_VDM_SYNC_OFFSET_BYTES 256
#define FWIF_GENERAL_HEAP_YUV_CSC_OFFSET_BYTES 128U

/*******************************************
    structure definitions
 *******************************************/
struct pvr_srv_winsys_heap {
   struct pvr_winsys_heap base;

   void *server_heap;
};

struct pvr_srv_winsys {
   struct pvr_winsys base;

   struct pvr_device *presignaled_sync_device;
   struct pvr_srv_sync *presignaled_sync;

   struct pvr_device_info dev_info;

   void *server_memctx;
   void *server_memctx_data;

   /* Required heaps */
   struct pvr_srv_winsys_heap general_heap;
   struct pvr_srv_winsys_heap pds_heap;
   struct pvr_srv_winsys_heap transfer_3d_heap;
   struct pvr_srv_winsys_heap usc_heap;
   struct pvr_srv_winsys_heap vis_test_heap;

   /* Optional heaps */
   bool rgn_hdr_heap_present;
   struct pvr_srv_winsys_heap rgn_hdr_heap;

   /* vma's for carveout memory regions */
   struct pvr_winsys_vma *pds_vma;
   struct pvr_winsys_vma *usc_vma;
   struct pvr_winsys_vma *general_vma;

   struct pvr_srv_sync_prim_ctx sync_prim_ctx;
};

/*******************************************
    helper macros
 *******************************************/

#define to_pvr_srv_winsys(ws) container_of((ws), struct pvr_srv_winsys, base)
#define to_pvr_srv_winsys_heap(heap) \
   container_of((heap), struct pvr_srv_winsys_heap, base)

/* XCLIPSE: the static data addresses that the DDK 1.11 commands carry (fw-api/
 * pvr_rogue_fwif_ddk111.h). PDS and USC addresses are offsets into their heap, the TPU YUV CSC
 * table register holds the address >> 2. */
static inline uint32_t
pvr_srv_static_heap_offset(const struct pvr_srv_winsys_heap *heap, uint64_t offset)
{
   return (uint32_t)(heap->base.static_data_carveout_addr.addr - heap->base.base_addr.addr +
                     offset);
}

static inline uint32_t pvr_srv_ddk111_eot_pds_code(const struct pvr_srv_winsys *ws)
{
   return pvr_srv_static_heap_offset(&ws->pds_heap, ws->pds_heap.base.static_data_offsets.eot);
}

static inline uint32_t pvr_srv_ddk111_vdm_sync_pds_data(const struct pvr_srv_winsys *ws)
{
   return pvr_srv_static_heap_offset(&ws->pds_heap,
                                     ws->pds_heap.base.static_data_offsets.vdm_sync);
}

static inline uint64_t pvr_srv_ddk111_yuv_csc_table(const struct pvr_srv_winsys *ws)
{
   return (ws->general_heap.base.static_data_carveout_addr.addr +
           ws->general_heap.base.static_data_offsets.yuv_csc) >> 2;
}

/*******************************************
    functions
 *******************************************/

VkResult pvr_srv_sync_get_presignaled_sync(struct pvr_device *device,
                                           struct pvr_srv_sync **out_sync);

/* DDK 1.13: the client kick command layouts (TA, 3D, transfer, compute) are not confirmed yet
 * (docs/ddk113/CAPTURE.md). Until they are, a 1.13 build refuses to submit GPU work instead of
 * sending the firmware a guessed command. Expands to an early return in the submit functions. */
#if !defined(PVR_SRV_DDK) || (PVR_SRV_DDK != 111 && PVR_SRV_DDK != 113)
#error "PVR_SRV_DDK must be 111 or 113 (meson option imagination-srv-ddk)"
#endif

#if PVR_SRV_DDK == 113
#define PVR_SRV_DDK113_REFUSE_UNCONFIRMED_SUBMIT(what)                                    \
   do {                                                                                  \
      mesa_loge("pvrsrvkm DDK 1.13: the %s command layout is not confirmed yet, "        \
                "nothing submitted (docs/ddk113/CAPTURE.md)",                            \
                what);                                                                   \
      return vk_error(NULL, VK_ERROR_DEVICE_LOST);                                       \
   } while (0)
#else
#define PVR_SRV_DDK113_REFUSE_UNCONFIRMED_SUBMIT(what) (void)0
#endif

#endif /* PVR_SRV_H */
