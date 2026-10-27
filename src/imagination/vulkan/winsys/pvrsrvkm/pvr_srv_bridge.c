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

/*
 * XCLIPSE: this file speaks services DDK 1.11 (1.11ED5425693, the PowerVR GE8320 of the Redmi
 * 9C), not 1.17. The bridge structs come from pvr_srv_bridge_ddk111.h, generated from that
 * DDK's own headers. Handles and user pointers are 8 bytes on the wire whatever the client's
 * word size (the kernel is 64-bit), so arrays of handles or pointers are widened to uint64_t
 * before a call; that is what lets the same code run as an armv7 and an arm64 client. Where
 * the Mesa interface asks for something 1.11 does not have, the function says so.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <xf86drm.h>

#include "fw-api/pvr_rogue_fwif_ddk111.h"
#include "fw-api/pvr_rogue_fwif_shared.h"
#include "pvr_device_info.h"
#include "pvr_macros.h"
#include "pvr_srv.h"
#include "pvr_srv_bridge.h"
#include "pvr_srv_bridge_ddk111.h"
#include "pvr_types.h"
#include "util/log.h"
#include "util/macros.h"
#include "vk_log.h"

#define vk_bridge_err(vk_err, bridge_func, bridge_ret)  \
   vk_errorf(NULL,                                      \
             vk_err,                                    \
             "%s failed, PVR_SRV_ERROR: %u, Errno: %s", \
             bridge_func,                               \
             (bridge_ret).eError,                       \
             strerror(errno))

/* A kernel handle as Mesa keeps it. Handles are small, but on a 32-bit client one that does
 * not fit would be silently cut, so say it once. */
static inline void *h_ptr(uint64_t h)
{
   if (sizeof(void *) < sizeof(uint64_t) && (h >> 32)) {
      static bool warned;
      if (!warned) {
         warned = true;
         mesa_loge("pvrsrvkm: kernel handle 0x%" PRIx64 " does not fit a pointer", h);
      }
   }
   return (void *)(uintptr_t)h;
}

static inline uint64_t h64(const void *p)
{
   return (uint64_t)(uintptr_t)p;
}

/* An array of n handles or pointers as the kernel reads it: n uint64_t. */
static uint64_t *widen(void *const *array, uint32_t n)
{
   if (!n || !array)
      return NULL;
   uint64_t *out = malloc(n * sizeof(uint64_t));
   if (!out)
      return NULL;
   for (uint32_t i = 0; i < n; i++)
      out[i] = h64(array[i]);
   return out;
}

/* Packed client CCB sizes come as (initial log2, max log2) byte pairs. A max of 0 means "the
 * kernel's default", and on 1.11 that default (from the vendor build config, 2^15 for transfer
 * and compute) can be below Mesa's initial size, which the kernel rejects. Ask for at least the
 * initial size instead. */
static uint32_t ddk111_ccb_sizes(uint32_t packed, unsigned pairs)
{
   for (unsigned i = 0; i < pairs; i++) {
      const unsigned shift = 16 * i;
      const uint32_t size = (packed >> shift) & 0xff;
      const uint32_t max = (packed >> (shift + 8)) & 0xff;

      if (size && max < size)
         packed = (packed & ~(0xffu << (shift + 8))) | (size << (shift + 8));
   }
   return packed;
}

/* 1.17's reset framework command has no flags word, so Mesa passes it with size 0. 1.11's
 * (RGXFWIF_RF_CMD) starts with ui32Flags and the kernel cannot allocate a 0 byte framework
 * buffer, so pass the flags word, 0: framework disabled. */
static const uint32_t ddk111_rf_cmd_disabled;

#define RF_CMD_SIZE(size) ((size) ? (size) : sizeof(ddk111_rf_cmd_disabled))
#define RF_CMD(size, cmd) h64((size) ? (const void *)(cmd) : &ddk111_rf_cmd_disabled)

static int pvr_srv_bridge_call(int fd,
                               uint8_t bridge_id,
                               uint32_t function_id,
                               void *input,
                               uint32_t input_buffer_size,
                               void *output,
                               uint32_t output_buffer_size)
{
   struct drm_srvkm_cmd cmd = {
      .bridge_id = bridge_id,
      .bridge_func_id = function_id,
      .in_data_ptr = (uint64_t)(uintptr_t)input,
      .out_data_ptr = (uint64_t)(uintptr_t)output,
      .in_data_size = input_buffer_size,
      .out_data_size = output_buffer_size,
   };

   int ret = drmIoctl(fd, DRM_IOCTL_SRVKM_CMD, &cmd);
   if (unlikely(ret))
      return ret;

   VG(VALGRIND_MAKE_MEM_DEFINED(output, output_buffer_size));

   return 0U;
}

#define CALL(fd, group, func, in, out) \
   pvr_srv_bridge_call(fd, group, func, in, sizeof(*(in)), out, sizeof(*(out)))

/* XCLIPSE: the RGX destroy calls answer RETRY in eError (not as the ioctl result) while the
 * firmware still holds the object, and then the object stays alive. Giving up there is not
 * safe on 1.11: its freelist keeps a bare pointer to our freelist PMR (no reference), so once
 * we free that BO the next freelist reconstruction after a GPU recovery maps freed pages and
 * the kernel panics. Retry until the kernel lets go; about 5 s, then report the error. */
#define PVR_SRV_DESTROY_RETRIES 5000U

#define CALL_DESTROY(fd, group, func, in, out)                          \
   ({                                                                   \
      int _result;                                                      \
      uint32_t _tries = 0;                                              \
      while (true) {                                                    \
         (out)->eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED;              \
         _result = CALL(fd, group, func, in, out);                      \
         if (_result || (out)->eError != PVR_SRV_ERROR_RETRY ||         \
             ++_tries >= PVR_SRV_DESTROY_RETRIES)                       \
            break;                                                      \
         usleep(1000);                                                  \
      }                                                                 \
      _result;                                                          \
   })

/* 1.11 has no services init command on the DRM node: a connection is all it takes. */
VkResult pvr_srv_init_module(int fd, enum pvr_srvkm_module_type module)
{
   return VK_SUCCESS;
}

/* 1.11's timelines live on /dev/pvr_sync, which can force a timeline to software-only but
 * cannot create or advance software fences (pvr_fd_sync_kernel.h: RENAME and FORCE_SW_ONLY
 * only). The software fence calls fail; pvr_srv_sync exports a signaled sync as -1 instead. */
VkResult pvr_srv_set_timeline_sw_only(int sw_timeline_fd)
{
   if (ioctl(sw_timeline_fd, _IO('W', 5)) < 0) {
      return vk_errorf(NULL,
                       VK_ERROR_OUT_OF_HOST_MEMORY,
                       "PVR_SYNC_IOC_FORCE_SW_ONLY failed, Errno: %s",
                       strerror(errno));
   }

   return VK_SUCCESS;
}

VkResult pvr_srv_create_sw_fence(int sw_timeline_fd,
                                 int *new_fence_fd,
                                 uint64_t *sync_pt_idx)
{
   return vk_errorf(NULL,
                    VK_ERROR_FEATURE_NOT_PRESENT,
                    "services 1.11 has no software fences");
}

VkResult pvr_srv_sw_sync_timeline_increment(int sw_timeline_fd,
                                            uint64_t *sync_pt_idx)
{
   return vk_errorf(NULL,
                    VK_ERROR_FEATURE_NOT_PRESENT,
                    "services 1.11 has no software fences");
}

VkResult pvr_srv_connection_create(int fd, uint64_t *const bvnc_out)
{
   struct ddk111_in_connect in = {
      .ui32Flags = PVR_SRV_FLAGS_CLIENT_64BIT_COMPAT,
      .ui32ClientBuildOptions = RGX_BUILD_OPTIONS,
      .ui32ClientDDKVersion = PVR_SRV_VERSION,
      .ui32ClientDDKBuild = PVR_SRV_VERSION_BUILD,
   };
   struct ddk111_out_connect out = { .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED };

   int result = CALL(fd,
                     PVR_SRV_BRIDGE_SRVCORE,
                     DDK111_BRIDGE_SRVCORE_CONNECT,
                     &in,
                     &out);
   if (result || out.eError != PVR_SRV_OK) {
      return vk_bridge_err(VK_ERROR_INITIALIZATION_FAILED,
                           "PVR_SRV_BRIDGE_SRVCORE_CONNECT",
                           out);
   }

   /* 1.11's CONNECT does not return the BVNC. The HWPerf bridge has it, as the string
    * "B.V.N.C" (RGX_HWPERF_BVNC in rgx_hwperf.h). */
   struct {
      uint32_t placeholder;
   } PACKED bvnc_in = { 0 };
   struct {
      char bvnc_string[24]; /* RGX_HWPERF_MAX_BVNC_LEN */
      uint32_t km_feature_flags;
      uint8_t units[4];
      uint32_t eError;
   } PACKED bvnc_out_struct = { .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED };

   result = CALL(fd,
                 PVR_SRV_BRIDGE_RGXHWPERF,
                 PVR_SRV_BRIDGE_RGXHWPERF_RGXGETHWPERFBVNCFEATUREFLAGS,
                 &bvnc_in,
                 &bvnc_out_struct);
   if (result || bvnc_out_struct.eError != PVR_SRV_OK) {
      return vk_bridge_err(VK_ERROR_INITIALIZATION_FAILED,
                           "PVR_SRV_BRIDGE_RGXHWPERF_RGXGETHWPERFBVNCFEATUREFLAGS",
                           bvnc_out_struct);
   }

   unsigned b, v, n, c;
   bvnc_out_struct.bvnc_string[sizeof(bvnc_out_struct.bvnc_string) - 1] = '\0';
   if (sscanf(bvnc_out_struct.bvnc_string, "%u.%u.%u.%u", &b, &v, &n, &c) != 4) {
      return vk_errorf(NULL,
                       VK_ERROR_INITIALIZATION_FAILED,
                       "unexpected BVNC string \"%s\"",
                       bvnc_out_struct.bvnc_string);
   }

   *bvnc_out = PVR_BVNC_PACK(b, v, n, c);

   return VK_SUCCESS;
}

void pvr_srv_connection_destroy(int fd)
{
   struct ddk111_in_disconnect in = { 0 };
   struct ddk111_out_disconnect out = { .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED };

   int result = CALL(fd,
                     PVR_SRV_BRIDGE_SRVCORE,
                     DDK111_BRIDGE_SRVCORE_DISCONNECT,
                     &in,
                     &out);
   if (result || out.eError != PVR_SRV_OK)
      vk_bridge_err(VK_ERROR_UNKNOWN, "PVR_SRV_BRIDGE_SRVCORE_DISCONNECT", out);
}

/* 1.11 has no multicore query: the GE8320 is one core with no multicore capabilities. */
VkResult pvr_srv_get_multicore_info(int fd,
                                    uint32_t caps_size,
                                    uint64_t *caps,
                                    uint32_t *num_cores)
{
   if (caps && caps_size)
      memset(caps, 0, caps_size * sizeof(*caps));
   if (num_cores)
      *num_cores = 1;

   return VK_SUCCESS;
}

VkResult pvr_srv_alloc_sync_primitive_block(int fd,
                                            void **const handle_out,
                                            void **const pmr_out,
                                            uint32_t *const size_out,
                                            uint32_t *const addr_out)
{
   struct ddk111_in_allocsyncprimitiveblock in = { 0 };
   struct ddk111_out_allocsyncprimitiveblock out = {
      .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED,
   };

   int result = CALL(fd,
                     PVR_SRV_BRIDGE_SYNC,
                     DDK111_BRIDGE_SYNC_ALLOCSYNCPRIMITIVEBLOCK,
                     &in,
                     &out);
   if (result || out.eError != PVR_SRV_OK) {
      return vk_bridge_err(VK_ERROR_INITIALIZATION_FAILED,
                           "PVR_SRV_BRIDGE_SYNC_ALLOCSYNCPRIMITIVEBLOCK",
                           out);
   }

   *handle_out = h_ptr(out.hSyncHandle);
   *pmr_out = h_ptr(out.hhSyncPMR);
   *size_out = out.ui32SyncPrimBlockSize;
   *addr_out = out.ui32SyncPrimVAddr;

   return VK_SUCCESS;
}

void pvr_srv_free_sync_primitive_block(int fd, void *handle)
{
   struct ddk111_in_freesyncprimitiveblock in = { .hSyncHandle = h64(handle) };
   struct ddk111_out_freesyncprimitiveblock out = {
      .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED,
   };

   int result = CALL(fd,
                     PVR_SRV_BRIDGE_SYNC,
                     DDK111_BRIDGE_SYNC_FREESYNCPRIMITIVEBLOCK,
                     &in,
                     &out);
   if (result || out.eError != PVR_SRV_OK) {
      vk_bridge_err(VK_ERROR_UNKNOWN,
                    "PVR_SRV_BRIDGE_SYNC_FREESYNCPRIMITIVEBLOCK",
                    out);
   }
}

VkResult
pvr_srv_set_sync_primitive(int fd, void *handle, uint32_t index, uint32_t value)
{
   struct ddk111_in_syncprimset in = {
      .hSyncHandle = h64(handle),
      .ui32Index = index,
      .ui32Value = value,
   };
   struct ddk111_out_syncprimset out = { .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED };

   int result =
      CALL(fd, PVR_SRV_BRIDGE_SYNC, DDK111_BRIDGE_SYNC_SYNCPRIMSET, &in, &out);
   if (result || out.eError != PVR_SRV_OK)
      return vk_bridge_err(VK_ERROR_UNKNOWN, "PVR_SRV_BRIDGE_SYNC_SYNCPRIMSET", out);

   return VK_SUCCESS;
}

VkResult pvr_srv_get_heap_count(int fd, uint32_t *const heap_count_out)
{
   struct ddk111_in_heapcfgheapcount in = { .ui32HeapConfigIndex = 0 };
   struct ddk111_out_heapcfgheapcount out = { .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED };

   int result =
      CALL(fd, PVR_SRV_BRIDGE_MM, DDK111_BRIDGE_MM_HEAPCFGHEAPCOUNT, &in, &out);
   if (result || out.eError != PVR_SRV_OK) {
      return vk_bridge_err(VK_ERROR_INITIALIZATION_FAILED,
                           "PVR_SRV_BRIDGE_MM_HEAPCFGHEAPCOUNT",
                           out);
   }

   *heap_count_out = out.ui32NumHeaps;

   return VK_SUCCESS;
}

VkResult pvr_srv_int_heap_create(int fd,
                                 pvr_dev_addr_t base_address,
                                 uint64_t size,
                                 uint32_t log2_page_size,
                                 void *server_memctx,
                                 void **const server_heap_out)
{
   struct ddk111_in_devmemintheapcreate in = {
      .hDevmemCtx = h64(server_memctx),
      .sHeapBaseAddr = base_address.addr,
      .uiHeapLength = size,
      .ui32Log2DataPageSize = log2_page_size,
   };
   struct ddk111_out_devmemintheapcreate out = {
      .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED,
   };

   int result =
      CALL(fd, PVR_SRV_BRIDGE_MM, DDK111_BRIDGE_MM_DEVMEMINTHEAPCREATE, &in, &out);
   if (result || out.eError != PVR_SRV_OK) {
      return vk_bridge_err(VK_ERROR_INITIALIZATION_FAILED,
                           "PVR_SRV_BRIDGE_MM_DEVMEMINTHEAPCREATE",
                           out);
   }

   *server_heap_out = h_ptr(out.hDevmemHeapPtr);

   return VK_SUCCESS;
}

void pvr_srv_int_heap_destroy(int fd, void *server_heap)
{
   struct ddk111_in_devmemintheapdestroy in = { .hDevmemHeap = h64(server_heap) };
   struct ddk111_out_devmemintheapdestroy out = {
      .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED,
   };

   int result =
      CALL(fd, PVR_SRV_BRIDGE_MM, DDK111_BRIDGE_MM_DEVMEMINTHEAPDESTROY, &in, &out);
   if (result || out.eError != PVR_SRV_OK) {
      vk_bridge_err(VK_ERROR_INITIALIZATION_FAILED,
                    "PVR_SRV_BRIDGE_MM_DEVMEMINTHEAPDESTROY",
                    out);
   }
}

/* This bridge function allows to independently query heap name and heap
 * details, i-e buffer/base_address/size/reserved_size/log2_page_size pointers
 * are allowed to be NULL.
 */
VkResult pvr_srv_get_heap_details(int fd,
                                  uint32_t heap_index,
                                  uint32_t buffer_size,
                                  char *const buffer_out,
                                  pvr_dev_addr_t *const base_address_out,
                                  uint64_t *const size_out,
                                  uint64_t *const reserved_size_out,
                                  uint32_t *const log2_page_size_out)
{
   char name[PVR_SRV_DEVMEM_HEAPNAME_MAXLENGTH];
   struct ddk111_in_heapcfgheapdetails in = {
      .ui32HeapConfigIndex = 0,
      .ui32HeapIndex = heap_index,
      .ui32HeapNameBufSz = buffer_out ? buffer_size : sizeof(name),
      .puiHeapNameOut = h64(buffer_out ? buffer_out : name),
   };
   struct ddk111_out_heapcfgheapdetails out = {
      .puiHeapNameOut = in.puiHeapNameOut,
      .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED,
   };

   int result =
      CALL(fd, PVR_SRV_BRIDGE_MM, DDK111_BRIDGE_MM_HEAPCFGHEAPDETAILS, &in, &out);
   if (result || out.eError != PVR_SRV_OK) {
      return vk_bridge_err(VK_ERROR_INITIALIZATION_FAILED,
                           "PVR_SRV_BRIDGE_MM_HEAPCFGHEAPDETAILS",
                           out);
   }

   if (buffer_out)
      VG(VALGRIND_MAKE_MEM_DEFINED(buffer_out, buffer_size));

   if (base_address_out)
      *base_address_out = PVR_DEV_ADDR(out.sDevVAddrBase);

   if (size_out)
      *size_out = out.uiHeapLength;

   /* 1.11 reserves nothing at the heap base, but Mesa places its static data (VDM sync and
    * EOT programs, YUV CSC tables) in a carveout there. Every client heap is private to our
    * memory context, so the first 64 KiB are simply claimed for it. */
   if (reserved_size_out)
      *reserved_size_out = PVR_SRV_CARVEOUT_SIZE_GRANULARITY;

   if (log2_page_size_out)
      *log2_page_size_out = out.ui32Log2DataPageSizeOut;

   return VK_SUCCESS;
}

void pvr_srv_int_ctx_destroy(int fd, void *server_memctx)
{
   struct ddk111_in_devmemintctxdestroy in = {
      .hDevmemServerContext = h64(server_memctx),
   };
   struct ddk111_out_devmemintctxdestroy out = {
      .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED,
   };

   int result =
      CALL(fd, PVR_SRV_BRIDGE_MM, DDK111_BRIDGE_MM_DEVMEMINTCTXDESTROY, &in, &out);
   if (result || out.eError != PVR_SRV_OK) {
      vk_bridge_err(VK_ERROR_INITIALIZATION_FAILED,
                    "PVR_SRV_BRIDGE_MM_DEVMEMINTCTXDESTROY",
                    out);
   }
}

VkResult pvr_srv_int_ctx_create(int fd,
                                void **const server_memctx_out,
                                void **const server_memctx_data_out)
{
   struct ddk111_in_devmemintctxcreate in = { .bbKernelMemoryCtx = false };
   struct ddk111_out_devmemintctxcreate out = {
      .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED,
   };

   int result =
      CALL(fd, PVR_SRV_BRIDGE_MM, DDK111_BRIDGE_MM_DEVMEMINTCTXCREATE, &in, &out);
   if (result || out.eError != PVR_SRV_OK) {
      return vk_bridge_err(VK_ERROR_INITIALIZATION_FAILED,
                           "PVR_SRV_BRIDGE_MM_DEVMEMINTCTXCREATE",
                           out);
   }

   *server_memctx_out = h_ptr(out.hDevMemServerContext);
   *server_memctx_data_out = h_ptr(out.hPrivData);

   return VK_SUCCESS;
}

VkResult pvr_srv_int_reserve_addr(int fd,
                                  void *server_heap,
                                  pvr_dev_addr_t addr,
                                  uint64_t size,
                                  void **const reservation_out)
{
   struct ddk111_in_devmemintreserverange in = {
      .hDevmemServerHeap = h64(server_heap),
      .sAddress = addr.addr,
      .uiLength = size,
   };
   struct ddk111_out_devmemintreserverange out = {
      .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED,
   };

   int result = CALL(fd,
                     PVR_SRV_BRIDGE_MM,
                     DDK111_BRIDGE_MM_DEVMEMINTRESERVERANGE,
                     &in,
                     &out);
   if (result || out.eError != PVR_SRV_OK) {
      return vk_bridge_err(VK_ERROR_INITIALIZATION_FAILED,
                           "PVR_SRV_BRIDGE_MM_DEVMEMINTRESERVERANGE",
                           out);
   }

   *reservation_out = h_ptr(out.hReservation);

   return VK_SUCCESS;
}

void pvr_srv_int_unreserve_addr(int fd, void *reservation)
{
   struct ddk111_in_devmemintunreserverange in = { .hReservation = h64(reservation) };
   struct ddk111_out_devmemintunreserverange out = {
      .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED,
   };

   int result = CALL(fd,
                     PVR_SRV_BRIDGE_MM,
                     DDK111_BRIDGE_MM_DEVMEMINTUNRESERVERANGE,
                     &in,
                     &out);
   if (result || out.eError != PVR_SRV_OK) {
      vk_bridge_err(VK_ERROR_INITIALIZATION_FAILED,
                    "PVR_SRV_BRIDGE_MM_DEVMEMINTUNRESERVERANGE",
                    out);
   }
}

VkResult pvr_srv_alloc_pmr(int fd,
                           uint64_t size,
                           uint64_t block_size,
                           uint32_t phy_blocks,
                           uint32_t virt_blocks,
                           uint32_t log2_page_size,
                           uint64_t flags,
                           uint32_t pid,
                           void **const pmr_out)
{
   const char *annotation = "VK PHYSICAL ALLOCATION";
   const uint32_t annotation_size =
      strnlen(annotation, DEVMEM_ANNOTATION_MAX_LEN - 1) + 1;
   uint32_t mapping_table = 0;

   struct ddk111_in_physmemnewrambackedlockedpmr in = {
      .uiSize = size,
      .uiChunkSize = block_size,
      .ui32NumPhysChunks = phy_blocks,
      .ui32NumVirtChunks = virt_blocks,
      .pui32MappingTable = h64(&mapping_table),
      .ui32Log2PageSize = log2_page_size,
      .uiFlags = (uint32_t)flags,
      .ui32AnnotationLength = annotation_size,
      .puiAnnotation = h64(annotation),
      .ui32PID = pid,
   };
   struct ddk111_out_physmemnewrambackedlockedpmr out = {
      .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED,
   };

   int result = CALL(fd,
                     PVR_SRV_BRIDGE_MM,
                     DDK111_BRIDGE_MM_PHYSMEMNEWRAMBACKEDLOCKEDPMR,
                     &in,
                     &out);
   if (result || out.eError != PVR_SRV_OK) {
      return vk_bridge_err(VK_ERROR_MEMORY_MAP_FAILED,
                           "PVR_SRV_BRIDGE_MM_PHYSMEMNEWRAMBACKEDLOCKEDPMR",
                           out);
   }

   *pmr_out = h_ptr(out.hPMRPtr);

   return VK_SUCCESS;
}

void pvr_srv_free_pmr(int fd, void *pmr)
{
   struct ddk111_in_pmrunrefunlockpmr in = { .hPMR = h64(pmr) };
   struct ddk111_out_pmrunrefunlockpmr out = {
      .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED,
   };

   int result =
      CALL(fd, PVR_SRV_BRIDGE_MM, DDK111_BRIDGE_MM_PMRUNREFUNLOCKPMR, &in, &out);
   if (result || out.eError != PVR_SRV_OK)
      vk_bridge_err(VK_ERROR_UNKNOWN, "PVR_SRV_BRIDGE_MM_PMRUNREFUNLOCKPMR", out);
}

VkResult pvr_srv_int_map_pages(int fd,
                               void *reservation,
                               void *pmr,
                               uint32_t page_count,
                               uint32_t page_offset,
                               uint64_t flags,
                               pvr_dev_addr_t addr)
{
   struct ddk111_in_devmemintmappages in = {
      .hReservation = h64(reservation),
      .hPMR = h64(pmr),
      .ui32PageCount = page_count,
      .ui32PhysicalPgOffset = page_offset,
      .uiFlags = (uint32_t)flags,
      .sDevVAddr = addr.addr,
   };
   struct ddk111_out_devmemintmappages out = {
      .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED,
   };

   int result =
      CALL(fd, PVR_SRV_BRIDGE_MM, DDK111_BRIDGE_MM_DEVMEMINTMAPPAGES, &in, &out);
   if (result || out.eError != PVR_SRV_OK) {
      return vk_bridge_err(VK_ERROR_MEMORY_MAP_FAILED,
                           "PVR_SRV_BRIDGE_MM_DEVMEMINTMAPPAGES",
                           out);
   }

   return VK_SUCCESS;
}

void pvr_srv_int_unmap_pages(int fd,
                             void *reservation,
                             pvr_dev_addr_t dev_addr,
                             uint32_t page_count)
{
   struct ddk111_in_devmemintunmappages in = {
      .hReservation = h64(reservation),
      .sDevVAddr = dev_addr.addr,
      .ui32PageCount = page_count,
   };
   struct ddk111_out_devmemintunmappages out = {
      .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED,
   };

   int result =
      CALL(fd, PVR_SRV_BRIDGE_MM, DDK111_BRIDGE_MM_DEVMEMINTUNMAPPAGES, &in, &out);
   if (result || out.eError != PVR_SRV_OK)
      vk_bridge_err(VK_ERROR_UNKNOWN, "PVR_SRV_BRIDGE_MM_DEVMEMINTUNMAPPAGES", out);
}

VkResult pvr_srv_int_map_pmr(int fd,
                             void *server_heap,
                             void *reservation,
                             void *pmr,
                             uint64_t flags,
                             void **const mapping_out)
{
   struct ddk111_in_devmemintmappmr in = {
      .hDevmemServerHeap = h64(server_heap),
      .hReservation = h64(reservation),
      .hPMR = h64(pmr),
      .uiMapFlags = (uint32_t)flags,
   };
   struct ddk111_out_devmemintmappmr out = {
      .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED,
   };

   int result =
      CALL(fd, PVR_SRV_BRIDGE_MM, DDK111_BRIDGE_MM_DEVMEMINTMAPPMR, &in, &out);
   if (result || out.eError != PVR_SRV_OK) {
      return vk_bridge_err(VK_ERROR_MEMORY_MAP_FAILED,
                           "PVR_SRV_BRIDGE_MM_DEVMEMINTMAPPMR",
                           out);
   }

   *mapping_out = h_ptr(out.hMapping);

   return VK_SUCCESS;
}

void pvr_srv_int_unmap_pmr(int fd, void *mapping)
{
   struct ddk111_in_devmemintunmappmr in = { .hMapping = h64(mapping) };
   struct ddk111_out_devmemintunmappmr out = {
      .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED,
   };

   int result =
      CALL(fd, PVR_SRV_BRIDGE_MM, DDK111_BRIDGE_MM_DEVMEMINTUNMAPPMR, &in, &out);
   if (result || out.eError != PVR_SRV_OK)
      vk_bridge_err(VK_ERROR_UNKNOWN, "PVR_SRV_BRIDGE_MM_DEVMEMINTUNMAPPMR", out);
}

VkResult pvr_srv_physmem_import_dmabuf(int fd,
                                       int buffer_fd,
                                       uint64_t flags,
                                       void **const pmr_out,
                                       uint64_t *const size_out,
                                       uint64_t *const align_out)
{
   struct ddk111_in_physmemimportdmabuf in = {
      .ifd = buffer_fd,
      .uiFlags = (uint32_t)flags,
      .ui32NameSize = 0,
      .puiName = 0,
   };
   struct ddk111_out_physmemimportdmabuf out = {
      .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED,
   };

   int result = CALL(fd,
                     PVR_SRV_BRIDGE_DMABUF,
                     DDK111_BRIDGE_DMABUF_PHYSMEMIMPORTDMABUF,
                     &in,
                     &out);
   if (result || out.eError != PVR_SRV_OK) {
      return vk_bridge_err(VK_ERROR_INVALID_EXTERNAL_HANDLE,
                           "PVR_SRV_BRIDGE_DMABUF_PHYSMEMIMPORTDMABUF",
                           out);
   }

   *pmr_out = h_ptr(out.hPMRPtr);
   *size_out = out.uiSize;
   *align_out = out.sAlign;

   return VK_SUCCESS;
}

VkResult pvr_srv_physmem_export_dmabuf(int fd, void *pmr, int *const fd_out)
{
   struct ddk111_in_physmemexportdmabuf in = { .hPMR = h64(pmr) };
   struct ddk111_out_physmemexportdmabuf out = {
      .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED,
   };

   int result = CALL(fd,
                     PVR_SRV_BRIDGE_DMABUF,
                     DDK111_BRIDGE_DMABUF_PHYSMEMEXPORTDMABUF,
                     &in,
                     &out);
   if (result || out.eError != PVR_SRV_OK) {
      return vk_bridge_err(VK_ERROR_OUT_OF_HOST_MEMORY,
                           "PVR_SRV_BRIDGE_DMABUF_PHYSMEMEXPORTDMABUF",
                           out);
   }

   *fd_out = out.iFd;

   return VK_SUCCESS;
}

/* 1.11 has no transfer context flags or robustness address, and does not hand back the
 * client and USC PMRs. */
VkResult pvr_srv_rgx_create_transfer_context(int fd,
                                             uint32_t priority,
                                             uint32_t reset_framework_cmd_size,
                                             uint8_t *reset_framework_cmd,
                                             void *priv_data,
                                             uint32_t packed_ccb_size_u8888,
                                             uint32_t context_flags,
                                             uint64_t robustness_address,
                                             void **const cli_pmr_out,
                                             void **const usc_pmr_out,
                                             void **const transfer_context_out)
{
   struct ddk111_in_rgxcreatetransfercontext in = {
      .ui32Priority = priority,
      .ui32FrameworkCmdize = RF_CMD_SIZE(reset_framework_cmd_size),
      .psFrameworkCmd = RF_CMD(reset_framework_cmd_size, reset_framework_cmd),
      .hPrivData = h64(priv_data),
      .ui32PackedCCBSizeU8888 = ddk111_ccb_sizes(packed_ccb_size_u8888, 2),
   };
   struct ddk111_out_rgxcreatetransfercontext out = {
      .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED,
   };

   int result = CALL(fd,
                     PVR_SRV_BRIDGE_RGXTQ,
                     DDK111_BRIDGE_RGXTQ_RGXCREATETRANSFERCONTEXT,
                     &in,
                     &out);
   if (result || out.eError != PVR_SRV_OK) {
      return vk_bridge_err(VK_ERROR_INITIALIZATION_FAILED,
                           "PVR_SRV_BRIDGE_RGXTQ_RGXCREATETRANSFERCONTEXT",
                           out);
   }

   if (cli_pmr_out)
      *cli_pmr_out = NULL;

   if (usc_pmr_out)
      *usc_pmr_out = NULL;

   *transfer_context_out = h_ptr(out.hTransferContext);

   return VK_SUCCESS;
}

void pvr_srv_rgx_destroy_transfer_context(int fd, void *transfer_context)
{
   struct ddk111_in_rgxdestroytransfercontext in = {
      .hTransferContext = h64(transfer_context),
   };
   struct ddk111_out_rgxdestroytransfercontext out = {
      .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED,
   };

   int result = CALL_DESTROY(fd,
                     PVR_SRV_BRIDGE_RGXTQ,
                     DDK111_BRIDGE_RGXTQ_RGXDESTROYTRANSFERCONTEXT,
                     &in,
                     &out);
   if (result || out.eError != PVR_SRV_OK) {
      vk_bridge_err(VK_ERROR_UNKNOWN,
                    "PVR_SRV_BRIDGE_RGXTQ_RGXDESTROYTRANSFERCONTEXT",
                    out);
   }
}

/* 1.11 takes client fences per prepare too (none here) and a cache-op sequence number. */
VkResult pvr_srv_rgx_submit_transfer2(int fd,
                                      void *transfer_context,
                                      uint32_t prepare_count,
                                      uint32_t *client_update_count,
                                      void ***update_ufo_sync_prim_block,
                                      uint32_t **update_sync_offset,
                                      uint32_t **update_value,
                                      int32_t check_fence,
                                      int32_t update_timeline_2d,
                                      int32_t update_timeline_3d,
                                      char *update_fence_name,
                                      uint32_t *cmd_size,
                                      uint8_t **fw_command,
                                      uint32_t *tq_prepare_flags,
                                      uint32_t ext_job_ref,
                                      uint32_t sync_pmr_count,
                                      uint32_t *sync_pmr_flags,
                                      void **sync_pmrs,
                                      int32_t *const update_fence_2d_out,
                                      int32_t *const update_fence_3d_out)
{
   VkResult vk_result = VK_SUCCESS;
   uint32_t *fence_counts = calloc(prepare_count, sizeof(uint32_t));
   uint64_t *null_ptrs = calloc(prepare_count, sizeof(uint64_t));
   uint64_t *update_blocks = calloc(prepare_count, sizeof(uint64_t));
   uint64_t *update_offsets = calloc(prepare_count, sizeof(uint64_t));
   uint64_t *update_values = calloc(prepare_count, sizeof(uint64_t));
   uint64_t *commands = calloc(prepare_count, sizeof(uint64_t));
   uint64_t **blocks = calloc(prepare_count, sizeof(uint64_t *));
   uint64_t *pmrs = widen(sync_pmrs, sync_pmr_count);

   if (!fence_counts || !null_ptrs || !update_blocks || !update_offsets ||
       !update_values || !commands || !blocks || (sync_pmr_count && !pmrs)) {
      vk_result = vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto out_free;
   }

   for (uint32_t i = 0; i < prepare_count; i++) {
      blocks[i] = widen(update_ufo_sync_prim_block[i], client_update_count[i]);
      if (client_update_count[i] && !blocks[i]) {
         vk_result = vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);
         goto out_free;
      }
      update_blocks[i] = h64(blocks[i]);
      update_offsets[i] = h64(update_sync_offset[i]);
      update_values[i] = h64(update_value[i]);
      commands[i] = h64(fw_command[i]);
   }

   /* The v1 bridge: the "2" variants are only built without SUPPORT_SERVER_SYNC_IMPL, which
    * this kernel has. No server syncs are passed (all counts 0). */
   struct ddk111_in_rgxsubmittransfer in = {
      .hTransferContext = h64(transfer_context),
      .ui32ClientCacheOpSeqNum = 0,
      .ui32PrepareCount = prepare_count,
      .pui32ClientFenceCount = h64(fence_counts),
      .phFenceUFOSyncPrimBlock = h64(null_ptrs),
      .pui32FenceSyncOffset = h64(null_ptrs),
      .pui32FenceValue = h64(null_ptrs),
      .pui32ClientUpdateCount = h64(client_update_count),
      .phUpdateUFOSyncPrimBlock = h64(update_blocks),
      .pui32UpdateSyncOffset = h64(update_offsets),
      .pui32UpdateValue = h64(update_values),
      .pui32ServerSyncCount = h64(fence_counts),
      .pui32ServerSyncFlags = h64(null_ptrs),
      .phServerSync = h64(null_ptrs),
      .hCheckFenceFD = check_fence,
      .h2DUpdateTimeline = update_timeline_2d,
      .h3DUpdateTimeline = update_timeline_3d,
      .puiUpdateFenceName = h64(update_fence_name),
      .pui32CommandSize = h64(cmd_size),
      .pui8FWCommand = h64(commands),
      .pui32TQPrepareFlags = h64(tq_prepare_flags),
      .ui32ExtJobRef = ext_job_ref,
      .ui32SyncPMRCount = sync_pmr_count,
      .pui32SyncPMRFlags = h64(sync_pmr_flags),
      .phSyncPMRs = h64(pmrs),
   };
   struct ddk111_out_rgxsubmittransfer out = {
      .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED,
   };

   int result = CALL(fd,
                     PVR_SRV_BRIDGE_RGXTQ,
                     DDK111_BRIDGE_RGXTQ_RGXSUBMITTRANSFER,
                     &in,
                     &out);
   if (result || out.eError != PVR_SRV_OK) {
      /* There is no 'retry' VkResult, so treat it as VK_NOT_READY instead. */
      if (result == PVR_SRV_ERROR_RETRY || out.eError == PVR_SRV_ERROR_RETRY)
         vk_result = VK_NOT_READY;
      else
         vk_result = vk_bridge_err(VK_ERROR_OUT_OF_DEVICE_MEMORY,
                                   "PVR_SRV_BRIDGE_RGXTQ_RGXSUBMITTRANSFER",
                                   out);
      goto out_free;
   }

   if (update_fence_2d_out)
      *update_fence_2d_out = out.h2DUpdateFence;

   if (update_fence_3d_out)
      *update_fence_3d_out = out.h3DUpdateFence;

out_free:
   for (uint32_t i = 0; blocks && i < prepare_count; i++)
      free(blocks[i]);
   free(blocks);
   free(commands);
   free(update_values);
   free(update_offsets);
   free(update_blocks);
   free(null_ptrs);
   free(fence_counts);
   free(pmrs);
   return vk_result;
}

/* The caller passes Mesa's 1.17 static compute state (rogue_fwif_static_computecontext_state);
 * 1.11 wants its own, with the context state base and robustness address in front. 1.11 has
 * no context flags or deadline here. */
VkResult
pvr_srv_rgx_create_compute_context(int fd,
                                   uint32_t priority,
                                   uint32_t reset_framework_cmd_size,
                                   uint8_t *reset_framework_cmd,
                                   void *priv_data,
                                   uint32_t static_compute_context_state_size,
                                   uint8_t *static_compute_context_state,
                                   uint32_t packed_ccb_size,
                                   uint32_t context_flags,
                                   uint64_t robustness_address,
                                   uint32_t max_deadline_ms,
                                   void **const compute_context_out)
{
   const struct rogue_fwif_static_computecontext_state *state117 =
      (const void *)static_compute_context_state;
   struct ddk111_fwif_static_computecontext_state state = {
      .robustness_address = robustness_address,
   };

   assert(static_compute_context_state_size == sizeof(*state117));
   state.ctx_switch_regs.cdm_context_pds0 = state117->ctx_switch_regs.cdm_context_pds0;
   state.ctx_switch_regs.cdm_context_pds1 = state117->ctx_switch_regs.cdm_context_pds1;
   state.ctx_switch_regs.cdm_terminate_pds = state117->ctx_switch_regs.cdm_terminate_pds;
   state.ctx_switch_regs.cdm_terminate_pds1 = state117->ctx_switch_regs.cdm_terminate_pds1;
   state.ctx_switch_regs.cdm_resume_pds0 = state117->ctx_switch_regs.cdm_resume_pds0;
   state.ctx_switch_regs.cdm_context_pds0_b = state117->ctx_switch_regs.cdm_context_pds0_b;
   state.ctx_switch_regs.cdm_resume_pds0_b = state117->ctx_switch_regs.cdm_resume_pds0_b;

   struct ddk111_in_rgxcreatecomputecontext in = {
      .ui32Priority = priority,
      .ui32FrameworkCmdize = RF_CMD_SIZE(reset_framework_cmd_size),
      .psFrameworkCmd = RF_CMD(reset_framework_cmd_size, reset_framework_cmd),
      .hPrivData = h64(priv_data),
      .sResumeSignalAddr = 0,
      .ui32StaticComputecontextStateSize = sizeof(state),
      .psStaticComputecontextState = h64(&state),
      .ui32PackedCCBSizeU88 = ddk111_ccb_sizes(packed_ccb_size, 1),
   };
   struct ddk111_out_rgxcreatecomputecontext out = {
      .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED,
   };

   int result = CALL(fd,
                     PVR_SRV_BRIDGE_RGXCMP,
                     DDK111_BRIDGE_RGXCMP_RGXCREATECOMPUTECONTEXT,
                     &in,
                     &out);
   if (result || out.eError != PVR_SRV_OK) {
      return vk_bridge_err(VK_ERROR_INITIALIZATION_FAILED,
                           "PVR_SRV_BRIDGE_RGXCMP_RGXCREATECOMPUTECONTEXT",
                           out);
   }

   *compute_context_out = h_ptr(out.hComputeContext);

   return VK_SUCCESS;
}

void pvr_srv_rgx_destroy_compute_context(int fd, void *compute_context)
{
   struct ddk111_in_rgxdestroycomputecontext in = {
      .hComputeContext = h64(compute_context),
   };
   struct ddk111_out_rgxdestroycomputecontext out = {
      .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED,
   };

   int result = CALL_DESTROY(fd,
                     PVR_SRV_BRIDGE_RGXCMP,
                     DDK111_BRIDGE_RGXCMP_RGXDESTROYCOMPUTECONTEXT,
                     &in,
                     &out);
   if (result || out.eError != PVR_SRV_OK) {
      vk_bridge_err(VK_ERROR_UNKNOWN,
                    "PVR_SRV_BRIDGE_RGXCMP_RGXDESTROYCOMPUTECONTEXT",
                    out);
   }
}

/* 1.11's compute kick takes no sync PMRs, work group counts or deadline. */
VkResult pvr_srv_rgx_kick_compute2(int fd,
                                   void *compute_context,
                                   uint32_t client_update_count,
                                   void **client_update_ufo_sync_prim_block,
                                   uint32_t *client_update_offset,
                                   uint32_t *client_update_value,
                                   int32_t check_fence,
                                   int32_t update_timeline,
                                   uint32_t cmd_size,
                                   uint8_t *cdm_cmd,
                                   uint32_t ext_job_ref,
                                   uint32_t sync_pmr_count,
                                   uint32_t *sync_pmr_flags,
                                   void **sync_pmrs,
                                   uint32_t num_work_groups,
                                   uint32_t num_work_items,
                                   uint32_t pdump_flags,
                                   uint64_t max_deadline_us,
                                   char *update_fence_name,
                                   int32_t *const update_fence_out)
{
   uint64_t *blocks =
      widen(client_update_ufo_sync_prim_block, client_update_count);
   if (client_update_count && !blocks)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   /* The v1 bridge, see pvr_srv_rgx_submit_transfer2(). */
   struct ddk111_in_rgxkickcdm in = {
      .hComputeContext = h64(compute_context),
      .ui32ClientCacheOpSeqNum = 0,
      .ui32ClientFenceCount = 0,
      .ui32ClientUpdateCount = client_update_count,
      .phClientUpdateUFOSyncPrimBlock = h64(blocks),
      .pui32ClientUpdateOffset = h64(client_update_offset),
      .pui32ClientUpdateValue = h64(client_update_value),
      .ui32ServerSyncCount = 0,
      .hCheckFenceFd = check_fence,
      .hUpdateTimeline = update_timeline,
      .puiUpdateFenceName = h64(update_fence_name),
      .ui32CmdSize = cmd_size,
      .psDMCmd = h64(cdm_cmd),
      .ui32PDumpFlags = pdump_flags,
      .ui32ExtJobRef = ext_job_ref,
   };
   struct ddk111_out_rgxkickcdm out = {
      .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED,
   };

   int result = CALL(fd,
                     PVR_SRV_BRIDGE_RGXCMP,
                     DDK111_BRIDGE_RGXCMP_RGXKICKCDM,
                     &in,
                     &out);
   free(blocks);
   if (result || out.eError != PVR_SRV_OK) {
      /* There is no 'retry' VkResult, so treat it as VK_NOT_READY instead. */
      if (result == PVR_SRV_ERROR_RETRY || out.eError == PVR_SRV_ERROR_RETRY)
         return VK_NOT_READY;

      return vk_bridge_err(VK_ERROR_OUT_OF_DEVICE_MEMORY,
                           "PVR_SRV_BRIDGE_RGXCMP_RGXKICKCDM",
                           out);
   }

   *update_fence_out = out.hUpdateFence;

   return VK_SUCCESS;
}

/* 1.17 creates a dataset of ROGUE_FWIF_NUM_RTDATAS render target datas in one call; 1.11
 * creates a render target, then each HWRT data on its own, referring to the render target by
 * firmware address. Each handle Mesa gets back is one of these; the kick passes its cleanup
 * cookie, and the TA and 3D commands need its firmware address
 * (pvr_srv_rgx_hwrt_data_fw_addr). */
struct pvr_srv_ddk111_render_target {
   uint64_t mem_desc;
   uint32_t fw_addr;
   uint32_t ref_count;
};

struct pvr_srv_ddk111_hwrt_data {
   uint64_t cleanup_cookie;
   uint32_t fw_addr;
   struct pvr_srv_ddk111_render_target *rt;
};

static void pvr_srv_ddk111_render_target_unref(int fd,
                                               struct pvr_srv_ddk111_render_target *rt)
{
   if (--rt->ref_count)
      return;

   struct ddk111_in_rgxdestroyrendertarget in = { .hsRenderTargetMemDesc = rt->mem_desc };
   struct ddk111_out_rgxdestroyrendertarget out = {
      .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED,
   };
   int result = CALL(fd,
                     PVR_SRV_BRIDGE_RGXTA3D,
                     DDK111_BRIDGE_RGXTA3D_RGXDESTROYRENDERTARGET,
                     &in,
                     &out);
   if (result || out.eError != PVR_SRV_OK) {
      vk_bridge_err(VK_ERROR_UNKNOWN,
                    "PVR_SRV_BRIDGE_RGXTA3D_RGXDESTROYRENDERTARGET",
                    out);
   }
   free(rt);
}

uint32_t pvr_srv_rgx_hwrt_data_fw_addr(void *hwrt_data)
{
   return ((struct pvr_srv_ddk111_hwrt_data *)hwrt_data)->fw_addr;
}

VkResult
pvr_srv_rgx_create_hwrt_dataset(int fd,
                                uint64_t flipped_multi_sample_ctl,
                                uint64_t multi_sample_ctl,
                                const pvr_dev_addr_t *macrotile_array_dev_addrs,
                                const pvr_dev_addr_t *pm_mlist_dev_addrs,
                                const pvr_dev_addr_t *rtc_dev_addrs,
                                const pvr_dev_addr_t *rgn_header_dev_addrs,
                                const pvr_dev_addr_t *tail_ptrs_dev_addrs,
                                const pvr_dev_addr_t *vheap_table_dev_adds,
                                void **free_lists,
                                uint32_t isp_merge_lower_x,
                                uint32_t isp_merge_lower_y,
                                uint32_t isp_merge_scale_x,
                                uint32_t isp_merge_scale_y,
                                uint32_t isp_merge_upper_x,
                                uint32_t isp_merge_upper_y,
                                uint32_t isp_mtile_size,
                                uint32_t mtile_stride,
                                uint32_t ppp_screen,
                                uint32_t rgn_header_size,
                                uint32_t te_aa,
                                uint32_t te_mtile1,
                                uint32_t te_mtile2,
                                uint32_t te_screen,
                                uint32_t tpc_size,
                                uint32_t tpc_stride,
                                uint16_t max_rts,
                                void **hwrt_dataset_out)
{
   struct pvr_srv_ddk111_render_target *rt = calloc(1, sizeof(*rt));
   if (!rt)
      return vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);

   struct ddk111_in_rgxcreaterendertarget rt_in = {
      .spsVHeapTableDevVAddr = vheap_table_dev_adds[0].addr,
   };
   struct ddk111_out_rgxcreaterendertarget rt_out = {
      .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED,
   };
   int result = CALL(fd,
                     PVR_SRV_BRIDGE_RGXTA3D,
                     DDK111_BRIDGE_RGXTA3D_RGXCREATERENDERTARGET,
                     &rt_in,
                     &rt_out);
   if (result || rt_out.eError != PVR_SRV_OK) {
      free(rt);
      return vk_bridge_err(VK_ERROR_INITIALIZATION_FAILED,
                           "PVR_SRV_BRIDGE_RGXTA3D_RGXCREATERENDERTARGET",
                           rt_out);
   }
   rt->mem_desc = rt_out.hsRenderTargetMemDesc;
   rt->fw_addr = rt_out.ui32sRenderTargetFWDevVAddr;
   rt->ref_count = 1;

   /* RGXFW_MAX_FREELISTS = 2 in 1.11: local, then global. */
   uint64_t fl[2] = { h64(free_lists[0]), h64(free_lists[1]) };
   uint32_t created = 0;
   VkResult vk_result = VK_SUCCESS;

   for (uint32_t i = 0; i < ROGUE_FWIF_NUM_RTDATAS; i++) {
      struct pvr_srv_ddk111_hwrt_data *data = calloc(1, sizeof(*data));
      if (!data) {
         vk_result = vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);
         break;
      }

      struct ddk111_in_rgxcreatehwrtdata in = {
         .ui32RenderTarget = rt->fw_addr,
         .sPMMlistDevVAddr = pm_mlist_dev_addrs[i].addr,
         .phapsFreeLists = h64(fl),
         .ui32PPPScreen = ppp_screen,
         .ui64MultiSampleCtl = multi_sample_ctl,
         .ui64FlippedMultiSampleCtl = flipped_multi_sample_ctl,
         .ui32TPCStride = tpc_stride,
         .sTailPtrsDevVAddr = tail_ptrs_dev_addrs[0].addr,
         .ui32TPCSize = tpc_size,
         .ui32TEScreen = te_screen,
         .ui32TEAA = te_aa,
         .ui32TEMTILE1 = te_mtile1,
         .ui32TEMTILE2 = te_mtile2,
         .ui32MTileStride = mtile_stride,
         .ui32ui32ISPMergeLowerX = isp_merge_lower_x,
         .ui32ui32ISPMergeLowerY = isp_merge_lower_y,
         .ui32ui32ISPMergeUpperX = isp_merge_upper_x,
         .ui32ui32ISPMergeUpperY = isp_merge_upper_y,
         .ui32ui32ISPMergeScaleX = isp_merge_scale_x,
         .ui32ui32ISPMergeScaleY = isp_merge_scale_y,
         .ssMacrotileArrayDevVAddr = macrotile_array_dev_addrs[i].addr,
         .ssRgnHeaderDevVAddr = rgn_header_dev_addrs[i].addr,
         .ssRTCDevVAddr = rtc_dev_addrs[0].addr,
         .ui64uiRgnHeaderSize = rgn_header_size,
         .ui32ui32ISPMtileSize = isp_mtile_size,
         .ui16MaxRTs = max_rts,
      };
      struct ddk111_out_rgxcreatehwrtdata out = {
         .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED,
      };

      result = CALL(fd,
                    PVR_SRV_BRIDGE_RGXTA3D,
                    DDK111_BRIDGE_RGXTA3D_RGXCREATEHWRTDATA,
                    &in,
                    &out);
      if (result || out.eError != PVR_SRV_OK) {
         free(data);
         vk_result = vk_bridge_err(VK_ERROR_INITIALIZATION_FAILED,
                                   "PVR_SRV_BRIDGE_RGXTA3D_RGXCREATEHWRTDATA",
                                   out);
         break;
      }

      data->cleanup_cookie = out.hCleanupCookie;
      data->fw_addr = out.ui32FWHWRTData;
      data->rt = rt;
      rt->ref_count++;
      hwrt_dataset_out[i] = data;
      created++;
   }

   if (vk_result != VK_SUCCESS) {
      for (uint32_t i = 0; i < created; i++)
         pvr_srv_rgx_destroy_hwrt_dataset(fd, hwrt_dataset_out[i]);
   }

   /* The datas hold the render target now. */
   pvr_srv_ddk111_render_target_unref(fd, rt);

   return vk_result;
}

void pvr_srv_rgx_destroy_hwrt_dataset(int fd, void *hwrt_dataset)
{
   struct pvr_srv_ddk111_hwrt_data *data = hwrt_dataset;
   struct ddk111_in_rgxdestroyhwrtdata in = { .hCleanupCookie = data->cleanup_cookie };
   struct ddk111_out_rgxdestroyhwrtdata out = {
      .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED,
   };

   int result = CALL_DESTROY(fd,
                     PVR_SRV_BRIDGE_RGXTA3D,
                     DDK111_BRIDGE_RGXTA3D_RGXDESTROYHWRTDATA,
                     &in,
                     &out);
   if (result || out.eError != PVR_SRV_OK) {
      vk_bridge_err(VK_ERROR_INITIALIZATION_FAILED,
                    "PVR_SRV_BRIDGE_RGXTA3D_RGXDESTROYHWRTDATA",
                    out);
   }

   pvr_srv_ddk111_render_target_unref(fd, data->rt);
   free(data);
}

/* 1.11 has no memory context private data argument here. */
VkResult pvr_srv_rgx_create_free_list(int fd,
                                      void *mem_ctx_priv_data,
                                      uint32_t max_free_list_pages,
                                      uint32_t init_free_list_pages,
                                      uint32_t grow_free_list_pages,
                                      uint32_t grow_param_threshold,
                                      void *global_free_list,
                                      enum pvr_srv_bool free_list_check,
                                      pvr_dev_addr_t free_list_dev_addr,
                                      void *free_list_pmr,
                                      uint64_t pmr_offset,
                                      void **const cleanup_cookie_out)
{
   struct ddk111_in_rgxcreatefreelist in = {
      .ui32ui32MaxFLPages = max_free_list_pages,
      .ui32ui32InitFLPages = init_free_list_pages,
      .ui32ui32GrowFLPages = grow_free_list_pages,
      .ui32ui32GrowParamThreshold = grow_param_threshold,
      .hsGlobalFreeList = h64(global_free_list),
      .bbFreeListCheck = free_list_check,
      .spsFreeListDevVAddr = free_list_dev_addr.addr,
      .hsFreeListPMR = h64(free_list_pmr),
      .uiPMROffset = pmr_offset,
   };
   struct ddk111_out_rgxcreatefreelist out = {
      .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED,
   };

   int result = CALL(fd,
                     PVR_SRV_BRIDGE_RGXTA3D,
                     DDK111_BRIDGE_RGXTA3D_RGXCREATEFREELIST,
                     &in,
                     &out);
   if (result || out.eError != PVR_SRV_OK) {
      return vk_bridge_err(VK_ERROR_INITIALIZATION_FAILED,
                           "PVR_SRV_BRIDGE_RGXTA3D_RGXCREATEFREELIST",
                           out);
   }

   *cleanup_cookie_out = h_ptr(out.hCleanupCookie);

   return VK_SUCCESS;
}

void pvr_srv_rgx_destroy_free_list(int fd, void *cleanup_cookie)
{
   struct ddk111_in_rgxdestroyfreelist in = { .hCleanupCookie = h64(cleanup_cookie) };
   struct ddk111_out_rgxdestroyfreelist out = {
      .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED,
   };

   int result = CALL_DESTROY(fd,
                             PVR_SRV_BRIDGE_RGXTA3D,
                             DDK111_BRIDGE_RGXTA3D_RGXDESTROYFREELIST,
                             &in,
                             &out);

   if (result || out.eError != PVR_SRV_OK) {
      vk_bridge_err(VK_ERROR_UNKNOWN,
                    "PVR_SRV_BRIDGE_RGXTA3D_RGXDESTROYFREELIST",
                    out);
   }
}

/* The caller passes Mesa's 1.17 static render state, which carries the TA context switch
 * registers; 1.11 takes those on their own (pRegs) and a static state with the robustness
 * address and the two deadlines. 1.11 has no call stack depth or context flags. */
VkResult
pvr_srv_rgx_create_render_context(int fd,
                                  uint32_t priority,
                                  pvr_dev_addr_t vdm_callstack_addr,
                                  uint32_t call_stack_depth,
                                  uint32_t reset_framework_cmd_size,
                                  uint8_t *reset_framework_cmd,
                                  void *priv_data,
                                  uint32_t static_render_context_state_size,
                                  uint8_t *static_render_context_state,
                                  uint32_t packed_ccb_size,
                                  uint32_t context_flags,
                                  uint64_t robustness_address,
                                  uint32_t max_geom_deadline_ms,
                                  uint32_t max_frag_deadline_ms,
                                  void **const render_context_out)
{
   const struct rogue_fwif_static_rendercontext_state *state117 =
      (const void *)static_render_context_state;
   const struct rogue_fwif_ta_regs_cswitch *regs117 = &state117->ctx_switch_geom_regs[0];
   struct ddk111_fwif_ta_regs_cswitch regs = {
      .vdm_context_state_base_addr = regs117->vdm_context_state_base_addr,
      .vdm_context_state_resume_addr = regs117->vdm_context_state_resume_addr,
      .ta_context_state_base_addr = regs117->ta_context_state_base_addr,
   };
   /* Mesa asks for no limit with UINT_MAX. The 1.11 firmware does not take that as unlimited:
    * it kills any render that runs longer than a few ms (reset reason GUILTY_OVERRUNING), and
    * the 3D job stops partway through its tiles, leaving the rest of the frame unwritten.
    * 0 means no deadline there (a 2.4 s render completes; with 2000 it is killed at ~1.5 s). */
   struct ddk111_fwif_static_rendercontext_state state = {
      .robustness_address = robustness_address,
      .max_ta_deadline_ms = max_geom_deadline_ms == UINT32_MAX ? 0 : max_geom_deadline_ms,
      .max_3d_deadline_ms = max_frag_deadline_ms == UINT32_MAX ? 0 : max_frag_deadline_ms,
   };

   assert(static_render_context_state_size == sizeof(*state117));
   for (uint32_t i = 0; i < ARRAY_SIZE(regs.ta_state); i++) {
      regs.ta_state[i].vdm_context_store_task0 = regs117->ta_state[i].vdm_context_store_task0;
      regs.ta_state[i].vdm_context_store_task1 = regs117->ta_state[i].vdm_context_store_task1;
      regs.ta_state[i].vdm_context_store_task2 = regs117->ta_state[i].vdm_context_store_task2;
      regs.ta_state[i].vdm_context_resume_task0 =
         regs117->ta_state[i].vdm_context_resume_task0;
      regs.ta_state[i].vdm_context_resume_task1 =
         regs117->ta_state[i].vdm_context_resume_task1;
      regs.ta_state[i].vdm_context_resume_task2 =
         regs117->ta_state[i].vdm_context_resume_task2;
   }

   struct ddk111_in_rgxcreaterendercontext in = {
      .ui32Priority = priority,
      .sVDMCallStackAddr = vdm_callstack_addr.addr,
      .ui32FrameworkCmdize = RF_CMD_SIZE(reset_framework_cmd_size),
      .psFrameworkCmd = RF_CMD(reset_framework_cmd_size, reset_framework_cmd),
      .hPrivData = h64(priv_data),
      .ui32CtxSwitchSize = sizeof(regs),
      .psRegs = h64(&regs),
      .ui32StaticRendercontextStateSize = sizeof(state),
      .psStaticRendercontextState = h64(&state),
      .ui32PackedCCBSizeU8888 = ddk111_ccb_sizes(packed_ccb_size, 2),
   };
   struct ddk111_out_rgxcreaterendercontext out = {
      .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED,
   };

   int result = CALL(fd,
                     PVR_SRV_BRIDGE_RGXTA3D,
                     DDK111_BRIDGE_RGXTA3D_RGXCREATERENDERCONTEXT,
                     &in,
                     &out);
   if (result || out.eError != PVR_SRV_OK) {
      return vk_bridge_err(VK_ERROR_INITIALIZATION_FAILED,
                           "PVR_SRV_BRIDGE_RGXTA3D_RGXCREATERENDERCONTEXT",
                           out);
   }

   *render_context_out = h_ptr(out.hRenderContext);

   return VK_SUCCESS;
}

void pvr_srv_rgx_destroy_render_context(int fd, void *render_context)
{
   struct ddk111_in_rgxdestroyrendercontext in = { .hCleanupCookie = h64(render_context) };
   struct ddk111_out_rgxdestroyrendercontext out = {
      .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED,
   };

   int result = CALL_DESTROY(fd,
                     PVR_SRV_BRIDGE_RGXTA3D,
                     DDK111_BRIDGE_RGXTA3D_RGXDESTROYRENDERCONTEXT,
                     &in,
                     &out);
   if (result || out.eError != PVR_SRV_OK) {
      vk_bridge_err(VK_ERROR_UNKNOWN,
                    "PVR_SRV_BRIDGE_RGXTA3D_RGXDESTROYRENDERCONTEXT",
                    out);
   }
}

/* 1.11 names the depth and stencil buffers apart (the combined ZS buffer goes in as Z) and
 * the HWRT data by its cleanup cookie; it also takes 3D fences (none here), a cache-op
 * sequence number and whether this TA is the last of the scene. */
VkResult pvr_srv_rgx_kick_render2(int fd,
                                  void *render_ctx,
                                  uint32_t client_geom_fence_count,
                                  void **client_geom_fence_sync_prim_block,
                                  uint32_t *client_geom_fence_sync_offset,
                                  uint32_t *client_geom_fence_value,
                                  uint32_t client_geom_update_count,
                                  void **client_geom_update_sync_prim_block,
                                  uint32_t *client_geom_update_sync_offset,
                                  uint32_t *client_geom_update_value,
                                  uint32_t client_frag_update_count,
                                  void **client_frag_update_sync_prim_block,
                                  uint32_t *client_frag_update_sync_offset,
                                  uint32_t *client_frag_update_value,
                                  void *pr_fence_ufo_sync_prim_block,
                                  uint32_t client_pr_fence_ufo_sync_offset,
                                  uint32_t client_pr_fence_value,
                                  int32_t check_fence,
                                  int32_t update_timeline,
                                  int32_t *const update_fence_out,
                                  char *update_fence_name,
                                  int32_t check_fence_frag,
                                  int32_t update_timeline_frag,
                                  int32_t *const update_fence_frag_out,
                                  char *update_fence_name_frag,
                                  uint32_t cmd_geom_size,
                                  uint8_t *cmd_geom,
                                  uint32_t cmd_frag_pr_size,
                                  uint8_t *cmd_frag_pr,
                                  uint32_t cmd_frag_size,
                                  uint8_t *cmd_frag,
                                  uint32_t ext_job_ref,
                                  bool kick_geom,
                                  bool kick_pr,
                                  bool kick_frag,
                                  bool abort,
                                  uint32_t pdump_flags,
                                  void *hw_rt_dataset,
                                  void *zs_buffer,
                                  void *msaa_scratch_buffer,
                                  uint32_t sync_pmr_count,
                                  uint32_t *sync_pmr_flags,
                                  void **sync_pmrs,
                                  uint32_t render_target_size,
                                  uint32_t num_draw_calls,
                                  uint32_t num_indices,
                                  uint32_t num_mrts,
                                  uint64_t deadline)
{
   VkResult vk_result = VK_SUCCESS;
   uint64_t *ta_fence_blocks =
      widen(client_geom_fence_sync_prim_block, client_geom_fence_count);
   uint64_t *ta_update_blocks =
      widen(client_geom_update_sync_prim_block, client_geom_update_count);
   uint64_t *frag_update_blocks =
      widen(client_frag_update_sync_prim_block, client_frag_update_count);
   uint64_t *pmrs = widen(sync_pmrs, sync_pmr_count);

   if ((client_geom_fence_count && !ta_fence_blocks) ||
       (client_geom_update_count && !ta_update_blocks) ||
       (client_frag_update_count && !frag_update_blocks) ||
       (sync_pmr_count && !pmrs)) {
      vk_result = vk_error(NULL, VK_ERROR_OUT_OF_HOST_MEMORY);
      goto out_free;
   }

   struct pvr_srv_ddk111_hwrt_data *hwrt = hw_rt_dataset;

   /* The v1 bridge, see pvr_srv_rgx_submit_transfer2(). */
   struct ddk111_in_rgxkickta3d in = {
      .hRenderContext = h64(render_ctx),
      .ui32ClientCacheOpSeqNum = 0,
      .ui32ServerTASyncPrims = 0,
      .ui32Server3DSyncPrims = 0,
      .ui32ClientTAFenceCount = client_geom_fence_count,
      .phClientTAFenceSyncPrimBlock = h64(ta_fence_blocks),
      .pui32ClientTAFenceSyncOffset = h64(client_geom_fence_sync_offset),
      .pui32ClientTAFenceValue = h64(client_geom_fence_value),
      .ui32ClientTAUpdateCount = client_geom_update_count,
      .phClientTAUpdateSyncPrimBlock = h64(ta_update_blocks),
      .pui32ClientTAUpdateSyncOffset = h64(client_geom_update_sync_offset),
      .pui32ClientTAUpdateValue = h64(client_geom_update_value),
      .ui32Client3DFenceCount = 0,
      .ui32Client3DUpdateCount = client_frag_update_count,
      .phClient3DUpdateSyncPrimBlock = h64(frag_update_blocks),
      .pui32Client3DUpdateSyncOffset = h64(client_frag_update_sync_offset),
      .pui32Client3DUpdateValue = h64(client_frag_update_value),
      .hPRFenceUFOSyncPrimBlock = h64(pr_fence_ufo_sync_prim_block),
      .ui32FRFenceUFOSyncOffset = client_pr_fence_ufo_sync_offset,
      .ui32FRFenceValue = client_pr_fence_value,
      .hCheckFence = check_fence,
      .hUpdateTimeline = update_timeline,
      .puiUpdateFenceName = h64(update_fence_name),
      .hCheckFence3D = check_fence_frag,
      .hUpdateTimeline3D = update_timeline_frag,
      .puiUpdateFenceName3D = h64(update_fence_name_frag),
      .ui32TACmdSize = cmd_geom_size,
      .psTACmd = h64(cmd_geom),
      .ui323DPRCmdSize = cmd_frag_pr_size,
      .ps3DPRCmd = h64(cmd_frag_pr),
      .ui323DCmdSize = cmd_frag_size,
      .ps3DCmd = h64(cmd_frag),
      .ui32ExtJobRef = ext_job_ref,
      .bbLastTAInScene = kick_geom,
      .bbKickTA = kick_geom,
      .bbKickPR = kick_pr,
      .bbKick3D = kick_frag,
      .bbAbort = abort,
      .ui32PDumpFlags = pdump_flags,
      .hRTDataCleanup = hwrt ? hwrt->cleanup_cookie : 0,
      .hZBuffer = h64(zs_buffer),
      .hSBuffer = 0,
      .hMSAAScratchBuffer = h64(msaa_scratch_buffer),
      .ui32SyncPMRCount = sync_pmr_count,
      .pui32SyncPMRFlags = h64(sync_pmr_flags),
      .phSyncPMRs = h64(pmrs),
      .ui32RenderTargetSize = render_target_size,
      .ui32NumberOfDrawCalls = num_draw_calls,
      .ui32NumberOfIndices = num_indices,
      .ui32NumberOfMRTs = num_mrts,
      .ui64Deadline = deadline,
   };
   struct ddk111_out_rgxkickta3d out = {
      .hUpdateFence = -1,
      .hUpdateFence3D = -1,
      .eError = PVR_SRV_ERROR_BRIDGE_CALL_FAILED,
   };

   int result = CALL(fd,
                     PVR_SRV_BRIDGE_RGXTA3D,
                     DDK111_BRIDGE_RGXTA3D_RGXKICKTA3D,
                     &in,
                     &out);
   if (result || out.eError != PVR_SRV_OK) {
      /* There is no 'retry' VkResult, so treat it as VK_NOT_READY instead. */
      if (result == PVR_SRV_ERROR_RETRY || out.eError == PVR_SRV_ERROR_RETRY)
         vk_result = VK_NOT_READY;
      else
         vk_result = vk_bridge_err(VK_ERROR_OUT_OF_DEVICE_MEMORY,
                                   "PVR_SRV_BRIDGE_RGXTA3D_RGXKICKTA3D",
                                   out);
      goto out_free;
   }

   *update_fence_out = out.hUpdateFence;
   *update_fence_frag_out = out.hUpdateFence3D;

out_free:
   free(pmrs);
   free(frag_update_blocks);
   free(ta_update_blocks);
   free(ta_fence_blocks);
   return vk_result;
}
