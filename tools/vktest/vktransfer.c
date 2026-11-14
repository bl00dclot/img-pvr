// SPDX-License-Identifier: MIT
//
// vktransfer: transfer-queue bring-up (docs/ddk113/TESTING.md, L3). Loads the ICD directly (no
// loader) and runs three jobs, each in its own submit, checking every byte on the host:
//   fill   vkCmdFillBuffer of 16 KiB with 0xa5c3e10f
//   copy   vkCmdCopyBuffer of a 16 KiB host-written pattern
//   clear  vkCmdClearColorImage of a 64x64 RGBA8 image to (51, 102, 153, 255), then
//          vkCmdCopyImageToBuffer
//
//   vktransfer /path/to/libvulkan_powervr.so [fill|copy|clear|all]
//
// Build (armv7): $NDK/.../armv7a-linux-androideabi29-clang -O2 -Wall tools/vktest/vktransfer.c -ldl \
//   -o vktransfer

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SIZE 4096u
#define W 64u
#define H 64u

#define CHECK(x)                                                                    \
   do {                                                                             \
      VkResult r_ = (x);                                                            \
      if (r_ != VK_SUCCESS) {                                                       \
         fprintf(stderr, "vktransfer: %s failed: %d (line %d)\n", #x, r_, __LINE__); \
         exit(1);                                                                   \
      }                                                                             \
   } while (0)

#define FUNCS(X)                                                                    \
   X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkGetBufferMemoryRequirements)            \
   X(vkCreateImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory)             \
   X(vkAllocateMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateCommandPool)   \
   X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer)         \
   X(vkResetCommandBuffer) X(vkCmdFillBuffer) X(vkCmdCopyBuffer)                     \
   X(vkCmdClearColorImage) X(vkCmdCopyImageToBuffer) X(vkCmdPipelineBarrier)         \
   X(vkCreateFence) X(vkResetFences) X(vkQueueSubmit) X(vkWaitForFences)

#define DECL(f) static PFN_##f f;
FUNCS(DECL)

static VkDevice dev;
static VkQueue queue;
static VkCommandBuffer cb;
static VkFence fence;
static VkPhysicalDeviceMemoryProperties mp;

static void *bo(VkBufferUsageFlags usage, VkBuffer *buf)
{
   VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = SIZE * 4, .usage = usage };
   CHECK(vkCreateBuffer(dev, &bci, NULL, buf));
   VkMemoryRequirements mr;
   vkGetBufferMemoryRequirements(dev, *buf, &mr);
   const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   uint32_t mt = UINT32_MAX;
   for (uint32_t i = 0; i < mp.memoryTypeCount && mt == UINT32_MAX; i++)
      if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
         mt = i;
   if (mt == UINT32_MAX) {
      fprintf(stderr, "vktransfer: no host-visible coherent memory type\n");
      exit(1);
   }
   VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = mt };
   VkDeviceMemory mem;
   CHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
   CHECK(vkBindBufferMemory(dev, *buf, mem, 0));
   void *p;
   CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &p));
   memset(p, 0, SIZE * 4);
   return p;
}

static void begin(void)
{
   CHECK(vkResetCommandBuffer(cb, 0));
   VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
   CHECK(vkBeginCommandBuffer(cb, &cbi));
}

static void submit(void)
{
   VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
   CHECK(vkEndCommandBuffer(cb));
   VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
   CHECK(vkResetFences(dev, 1, &fence));
   CHECK(vkQueueSubmit(queue, 1, &si, fence));
   // 2 s: far beyond these jobs; a timeout means the GPU did not complete the work.
   CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 2000000000ull));
}

static int report(const char *name, const uint32_t *got, uint32_t n, uint32_t (*want)(uint32_t))
{
   uint32_t bad = 0, first = UINT32_MAX;
   for (uint32_t i = 0; i < n; i++)
      if (got[i] != want(i) && bad++ == 0)
         first = i;
   printf("vktransfer %s: %u/%u words wrong", name, bad, n);
   if (bad)
      printf(", first at %u: got 0x%08x want 0x%08x", first, got[first], want(first));
   printf(" -> %s\n", bad ? "FAIL" : "ok");
   return bad != 0;
}

static uint32_t want_fill(uint32_t i) { (void)i; return 0xa5c3e10fu; }
static uint32_t want_copy(uint32_t i) { return i * 2654435761u; }
static uint32_t want_clear(uint32_t i) { (void)i; return 0xff996633u; } /* R=51 G=102 B=153 A=255 */

int main(int argc, char **argv)
{
   if (argc < 2) {
      fprintf(stderr, "usage: vktransfer <libvulkan_powervr.so> [fill|copy|clear|all]\n");
      return 2;
   }
   const char *mode = argc > 2 ? argv[2] : "all";
   int all = !strcmp(mode, "all");
   void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
   if (!lib) {
      fprintf(stderr, "vktransfer: dlopen: %s\n", dlerror());
      return 1;
   }
   VkResult (*negotiate)(uint32_t *) = (VkResult(*)(uint32_t *))dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   if (!negotiate || !gipa) {
      fprintf(stderr, "vktransfer: not a Vulkan ICD\n");
      return 1;
   }
   uint32_t icd_version = 5;
   negotiate(&icd_version);

   PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
   VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
   VkInstance inst;
   CHECK(vkCreateInstance(&ici, NULL, &inst));
   PFN_vkEnumeratePhysicalDevices vkEnumeratePhysicalDevices =
      (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
   PFN_vkGetPhysicalDeviceMemoryProperties vkGetPhysicalDeviceMemoryProperties =
      (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
   PFN_vkCreateDevice vkCreateDevice = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
   PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
   if (!vkEnumeratePhysicalDevices || !vkGetPhysicalDeviceMemoryProperties || !vkCreateDevice ||
       !vkGetDeviceProcAddr) {
      fprintf(stderr, "vktransfer: missing instance entry points\n");
      return 1;
   }

   uint32_t n = 1;
   VkPhysicalDevice pd;
   VkResult er = vkEnumeratePhysicalDevices(inst, &n, &pd);
   if (!n || (er != VK_SUCCESS && er != VK_INCOMPLETE)) {
      fprintf(stderr, "vktransfer: no physical device (%d)\n", er);
      return 1;
   }
   vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   // Queue family 0 is the only family on this driver and supports graphics, compute and transfer.
   float prio = 1;
   VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
                                   .queueCount = 1, .pQueuePriorities = &prio };
   VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci };
   CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
#define LOAD(f)                                                   \
   if (!(f = (PFN_##f)vkGetDeviceProcAddr(dev, #f))) {            \
      fprintf(stderr, "vktransfer: no %s\n", #f);                 \
      return 1;                                                   \
   }
   FUNCS(LOAD)
   vkGetDeviceQueue(dev, 0, 0, &queue);

   VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
   VkCommandPool pool;
   CHECK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
   VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                       .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                       .commandBufferCount = 1 };
   CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
   VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   CHECK(vkCreateFence(dev, &fci, NULL, &fence));

   const VkBufferUsageFlags tu = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
   VkBuffer src, dst;
   uint32_t *s = bo(tu, &src), *d = bo(tu, &dst);
   int fails = 0;

   if (all || !strcmp(mode, "fill")) {
      begin();
      vkCmdFillBuffer(cb, dst, 0, SIZE * 4, 0xa5c3e10fu);
      submit();
      fails += report("fill", d, SIZE, want_fill);
   }

   if (all || !strcmp(mode, "copy")) {
      for (uint32_t i = 0; i < SIZE; i++) {
         s[i] = want_copy(i);
         d[i] = 0;
      }
      begin();
      VkBufferCopy r = { 0, 0, SIZE * 4 };
      vkCmdCopyBuffer(cb, src, dst, 1, &r);
      submit();
      fails += report("copy", d, SIZE, want_copy);
   }

   if (all || !strcmp(mode, "clear")) {
      VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                               .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { W, H, 1 }, .mipLevels = 1,
                               .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
                               .tiling = VK_IMAGE_TILING_OPTIMAL,
                               .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
      VkImage img;
      CHECK(vkCreateImage(dev, &ii, NULL, &img));
      VkMemoryRequirements mr;
      vkGetImageMemoryRequirements(dev, img, &mr);
      uint32_t mt = UINT32_MAX;
      for (uint32_t i = 0; i < mp.memoryTypeCount && mt == UINT32_MAX; i++)
         if (mr.memoryTypeBits & (1u << i))
            mt = i;
      VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                   .memoryTypeIndex = mt };
      VkDeviceMemory mem;
      CHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
      CHECK(vkBindImageMemory(dev, img, mem, 0));
      memset(d, 0, SIZE * 4);

      VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
      begin();
      VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                  .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                  .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                  .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                  .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                                  .subresourceRange = range };
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                           NULL, 1, &ib);
      VkClearColorValue cc = { .float32 = { 51 / 255.0f, 102 / 255.0f, 153 / 255.0f, 1.0f } };
      vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &range);
      ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL,
                           1, &ib);
      VkBufferImageCopy r = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                              .imageExtent = { W, H, 1 } };
      vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &r);
      submit();
      fails += report("clear", d, W * H, want_clear);
   }

   return fails ? 3 : 0;
}
