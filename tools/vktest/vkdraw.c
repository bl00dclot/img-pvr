// SPDX-License-Identifier: MIT
//
// vkdraw: render bring-up (docs/ddk113/TESTING.md, L5). Loads the ICD directly (no loader), renders
// into a 64x64 RGBA8 colour attachment, copies it to a host-visible buffer and prints its byte sum.
// Each iteration is its own submit:
//   clear  render pass with load-op clear to (0.25, 0.5, 0.75, 1), nothing drawn
//   draw   load-op clear to 0, then vkdraw.vert/.frag: a full-screen triangle in that colour
// Both should give 2613248, the checksum of `glwork draw` on the vendor driver.
//   big    glwork big in Vulkan: 2^21 small triangles into 512x512, enough geometry to run the
//          parameter buffer out and force partial renders. Should give 173277184, the checksum of
//          `glwork big` on the vendor driver (not part of all).
//
//   vkdraw /path/to/libvulkan_powervr.so [clear|draw|all|big] [iterations]
//
// Build (armv7): $NDK/.../armv7a-linux-androideabi29-clang -O2 -Wall -Wextra -Werror \
//   tools/vktest/vkdraw.c -ldl -o vkdraw.  vkdraw_vert_spv.h: glslangValidator -V --vn vkdraw_vert_spv \
//   -o vkdraw_vert_spv.h vkdraw.vert (vkdraw_frag_spv.h, vkdraw_big_*_spv.h likewise)
//
// Drafted with an LLM; marker word per AGENTS.md: biblioklept.

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vkdraw_big_frag_spv.h"
#include "vkdraw_big_vert_spv.h"
#include "vkdraw_frag_spv.h"
#include "vkdraw_vert_spv.h"

#define W 64u
#define H 64u
/* The image is BIG x BIG; clear and draw use its top-left W x H corner. */
#define BIG 512u
#define BIG_TRIS (1u << 21)

#define CHECK(x)                                                                \
   do {                                                                         \
      VkResult r_ = (x);                                                        \
      if (r_ != VK_SUCCESS) {                                                   \
         fprintf(stderr, "vkdraw: %s failed: %d (line %d)\n", #x, r_, __LINE__); \
         exit(1);                                                               \
      }                                                                         \
   } while (0)

#define FUNCS(X)                                                                      \
   X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkGetBufferMemoryRequirements)              \
   X(vkCreateImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory)               \
   X(vkCreateImageView) X(vkAllocateMemory) X(vkBindBufferMemory) X(vkMapMemory)       \
   X(vkCreateRenderPass) X(vkCreateFramebuffer) X(vkCreateShaderModule)                \
   X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) X(vkCreateCommandPool)       \
   X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer)           \
   X(vkResetCommandBuffer) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass)               \
   X(vkCmdBindPipeline) X(vkCmdDraw) X(vkCmdCopyImageToBuffer) X(vkCmdPipelineBarrier) \
   X(vkCreateFence) X(vkResetFences) X(vkQueueSubmit) X(vkWaitForFences)

#define DECL(f) static PFN_##f f;
FUNCS(DECL)

static VkDevice dev;
static VkPhysicalDeviceMemoryProperties mp;

static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
         return i;
   fprintf(stderr, "vkdraw: no memory type with flags 0x%x\n", want);
   exit(1);
}

static VkShaderModule shader(const uint32_t *code, size_t size)
{
   VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size,
                                    .pCode = code };
   VkShaderModule sm;
   CHECK(vkCreateShaderModule(dev, &smi, NULL, &sm));
   return sm;
}

/* Every pixel must equal the first, with R 63 or 64, G 128, B 191 or 192, A 255. */
static int report(const char *name, int it, const volatile uint32_t *got)
{
   uint32_t sum = 0, first = got[0];
   int uniform = 1;
   for (uint32_t i = 0; i < W * H; i++) {
      uint32_t p = got[i];
      sum += (p & 0xff) + (p >> 8 & 0xff) + (p >> 16 & 0xff) + (p >> 24);
      uniform &= p == first;
   }
   uint32_t r = first & 0xff, g = first >> 8 & 0xff, b = first >> 16 & 0xff, a = first >> 24;
   int ok = uniform && (r == 63 || r == 64) && g == 128 && (b == 191 || b == 192) && a == 255;
   printf("vkdraw %s iteration %d: sum %u (expected 2613248), first pixel 0x%08x -> %s\n", name, it, sum, first,
          ok ? "ok" : "FAIL");
   return !ok;
}

int main(int argc, char **argv)
{
   if (argc < 2) {
      fprintf(stderr, "usage: vkdraw <libvulkan_powervr.so> [clear|draw|all] [iterations]\n");
      return 2;
   }
   const char *mode = argc > 2 ? argv[2] : "all";
   int iterations = argc > 3 ? atoi(argv[3]) : 1;
   int all = !strcmp(mode, "all");
   int big = !strcmp(mode, "big");
   if ((!all && !big && strcmp(mode, "clear") && strcmp(mode, "draw")) || iterations < 1) {
      fprintf(stderr, "vkdraw: bad mode %s or iteration count\n", mode);
      return 2;
   }
   void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
   if (!lib) {
      fprintf(stderr, "vkdraw: dlopen: %s\n", dlerror());
      return 1;
   }
   VkResult (*negotiate)(uint32_t *) = (VkResult(*)(uint32_t *))dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   if (!negotiate || !gipa) {
      fprintf(stderr, "vkdraw: not a Vulkan ICD\n");
      return 1;
   }
   uint32_t icd_version = 5;
   negotiate(&icd_version);

   PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   if (!vkCreateInstance) {
      fprintf(stderr, "vkdraw: no vkCreateInstance\n");
      return 1;
   }
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
      fprintf(stderr, "vkdraw: missing instance entry points\n");
      return 1;
   }

   uint32_t n = 1;
   VkPhysicalDevice pd;
   VkResult er = vkEnumeratePhysicalDevices(inst, &n, &pd);
   if (!n || (er != VK_SUCCESS && er != VK_INCOMPLETE)) {
      fprintf(stderr, "vkdraw: no physical device (%d)\n", er);
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
      fprintf(stderr, "vkdraw: no %s\n", #f);                     \
      return 1;                                                   \
   }
   FUNCS(LOAD)
   VkQueue queue;
   vkGetDeviceQueue(dev, 0, 0, &queue);

   VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { BIG, BIG, 1 }, .mipLevels = 1,
                            .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                            .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
   VkImage img;
   CHECK(vkCreateImage(dev, &ii, NULL, &img));
   VkMemoryRequirements mr;
   vkGetImageMemoryRequirements(dev, img, &mr);
   VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = memtype(mr.memoryTypeBits, 0) };
   VkDeviceMemory imem;
   CHECK(vkAllocateMemory(dev, &mai, NULL, &imem));
   CHECK(vkBindImageMemory(dev, img, imem, 0));
   VkImageViewCreateInfo ivi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
                                 .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
                                 .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
   VkImageView view;
   CHECK(vkCreateImageView(dev, &ivi, NULL, &view));

   VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = BIG * BIG * 4,
                              .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
   VkBuffer buf;
   CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
   vkGetBufferMemoryRequirements(dev, buf, &mr);
   mai.allocationSize = mr.size;
   mai.memoryTypeIndex =
      memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
   VkDeviceMemory bmem;
   CHECK(vkAllocateMemory(dev, &mai, NULL, &bmem));
   CHECK(vkBindBufferMemory(dev, buf, bmem, 0));
   volatile uint32_t *px;
   CHECK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, (void **)&px));

   // The attachment ends in TRANSFER_SRC_OPTIMAL; the dependency orders its write before the copy.
   VkAttachmentDescription ad = { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
                                  .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                  .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                  .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                  .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                  .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
   VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
   VkSubpassDescription sd = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
                               .pColorAttachments = &ar };
   VkSubpassDependency dep = { .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
                               .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                               .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
                               .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                               .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT };
   VkRenderPassCreateInfo rpi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
                                  .pAttachments = &ad, .subpassCount = 1, .pSubpasses = &sd,
                                  .dependencyCount = 1, .pDependencies = &dep };
   VkRenderPass rp;
   CHECK(vkCreateRenderPass(dev, &rpi, NULL, &rp));
   VkFramebufferCreateInfo fbi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp,
                                   .attachmentCount = 1, .pAttachments = &view, .width = BIG, .height = BIG,
                                   .layers = 1 };
   VkFramebuffer fb;
   CHECK(vkCreateFramebuffer(dev, &fbi, NULL, &fb));

   VkPipelineShaderStageCreateInfo stages[2] = {
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
        .module = shader(vkdraw_vert_spv, sizeof(vkdraw_vert_spv)), .pName = "main" },
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
        .module = shader(vkdraw_frag_spv, sizeof(vkdraw_frag_spv)), .pName = "main" },
   };
   VkPipelineVertexInputStateCreateInfo vis = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
   VkPipelineInputAssemblyStateCreateInfo ias = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                  .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
   VkViewport vp = { 0, 0, W, H, 0, 1 };
   VkRect2D sc = { { 0, 0 }, { W, H } };
   VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                             .viewportCount = 1, .pViewports = &vp, .scissorCount = 1,
                                             .pScissors = &sc };
   VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL,
                                                 .cullMode = VK_CULL_MODE_NONE,
                                                 .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1 };
   VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
   VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                                                 VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT };
   VkPipelineColorBlendStateCreateInfo cbs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                               .attachmentCount = 1, .pAttachments = &cba };
   VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
   VkPipelineLayout pl;
   CHECK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));
   VkGraphicsPipelineCreateInfo gpi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
                                        .pStages = stages, .pVertexInputState = &vis, .pInputAssemblyState = &ias,
                                        .pViewportState = &vps, .pRasterizationState = &rs,
                                        .pMultisampleState = &ms, .pColorBlendState = &cbs, .layout = pl,
                                        .renderPass = rp, .subpass = 0 };
   VkPipeline pipe, big_pipe;
   CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, NULL, &pipe));
   stages[0].module = shader(vkdraw_big_vert_spv, sizeof(vkdraw_big_vert_spv));
   stages[1].module = shader(vkdraw_big_frag_spv, sizeof(vkdraw_big_frag_spv));
   vp = (VkViewport){ 0, 0, BIG, BIG, 0, 1 };
   VkRect2D big_sc = { { 0, 0 }, { BIG, BIG } };
   vps.pScissors = &big_sc;
   CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, NULL, &big_pipe));

   VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
   VkCommandPool pool;
   CHECK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
   VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                       .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                       .commandBufferCount = 1 };
   VkCommandBuffer cb;
   CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
   VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   VkFence fence;
   CHECK(vkCreateFence(dev, &fci, NULL, &fence));

   int fails = 0;
   for (int m = 0; m < 3; m++) {
      const char *name = m == 2 ? "big" : m ? "draw" : "clear";
      if ((!all && strcmp(mode, name)) || (all && m == 2))
         continue;
      const uint32_t w = m == 2 ? BIG : W, h = m == 2 ? BIG : H;
      for (int it = 0; it < iterations; it++) {
         for (uint32_t i = 0; i < w * h; i++)
            px[i] = 0;
         CHECK(vkResetCommandBuffer(cb, 0));
         VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                          .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
         CHECK(vkBeginCommandBuffer(cb, &cbi));
         VkClearValue cv = { .color.float32 = { 0.25f, 0.5f, 0.75f, 1.0f } };
         if (m)
            cv = (VkClearValue){ .color.float32 = { 0, 0, 0, 0 } };
         VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp,
                                       .framebuffer = fb, .renderArea = m == 2 ? big_sc : sc,
                                       .clearValueCount = 1,
                                       .pClearValues = &cv };
         vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
         if (m) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, m == 2 ? big_pipe : pipe);
            vkCmdDraw(cb, m == 2 ? BIG_TRIS * 3 : 3, 1, 0, 0);
         }
         vkCmdEndRenderPass(cb);
         VkBufferImageCopy r = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                 .imageExtent = { w, h, 1 } };
         vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &r);
         VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
         vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0,
                              NULL);
         CHECK(vkEndCommandBuffer(cb));
         VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
                             .pCommandBuffers = &cb };
         CHECK(vkResetFences(dev, 1, &fence));
         CHECK(vkQueueSubmit(queue, 1, &si, fence));
         // 2 s (20 s for big): far beyond these renders; a timeout means the GPU did not complete it.
         CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, m == 2 ? 20000000000ull : 2000000000ull));
         if (m == 2) {
            uint32_t sum = 0;
            for (uint32_t i = 0; i < w * h; i++)
               sum += (px[i] & 0xff) + (px[i] >> 8 & 0xff) + (px[i] >> 16 & 0xff) + (px[i] >> 24);
            printf("vkdraw big iteration %d: sum %u (expected 173277184) -> %s\n", it, sum,
                   sum == 173277184u ? "ok" : "FAIL");
            fails += sum != 173277184u;
         } else {
            fails += report(name, it, px);
         }
      }
   }
   return fails ? 3 : 0;
}
