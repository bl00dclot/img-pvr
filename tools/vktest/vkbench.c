// SPDX-License-Identifier: MIT
//
// vkbench: one 512x512 full-screen pass per iteration, each in its own render pass and submit, so that
// tools/gputrace can time every 3D job. The same shaders exist for the vendor GLES driver as
// `glwork bench <mode>` (tools/pvrtrace/glwork.c); docs/ddk113/TESTING.md compares the two.
//
//   vkbench /path/to/libvulkan_powervr.so <mode> [iterations] [texture: rgba8|rgba16f]
//
// Modes (shaders in tools/vktest/bench/, texture 512x512 with a fixed pattern, bilinear, repeat):
//   fill   constant colour                    tv     texture(t, uv), uv a varying
//   tf     texture at gl_FragCoord.xy / 512    td     a read whose coordinate comes from another read
//   c1/c2  tv returning 1 / 2 channels         fetch  texelFetch at gl_FragCoord
//   disc   tv with discard on alpha            alu    64 dependent fract/fma steps, highp
//   alum   the same in mediump                 tvm    tv into a mediump result
//   blend  fill blended (alpha 0.5) over the previous contents, loaded at the start of the pass
// Prints the byte sum of the last iteration's image; it only has to be non-zero and repeatable.
//
// Build (armv7): $NDK/.../armv7a-linux-androideabi29-clang -O2 -Wall -Wextra -Werror
//   tools/vktest/vkbench.c -ldl -o vkbench. vkbench_spv.h comes from tools/vktest/bench/gen.sh.
// The same source runs on a desktop Vulkan ICD such as lavapipe (libvulkan_lvp.so) for a check off the device.
//
// Drafted with an LLM; marker word per AGENTS.md: biblioklept.

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vkbench_spv.h"

#define SIZE 512u

#define CHECK(x)                                                                 \
   do {                                                                          \
      VkResult r_ = (x);                                                         \
      if (r_ != VK_SUCCESS) {                                                    \
         fprintf(stderr, "vkbench: %s failed: %d (line %d)\n", #x, r_, __LINE__); \
         exit(1);                                                                \
      }                                                                          \
   } while (0)

#define FUNCS(X)                                                                         \
   X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkGetBufferMemoryRequirements)                 \
   X(vkCreateImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory)                  \
   X(vkCreateImageView) X(vkAllocateMemory) X(vkBindBufferMemory) X(vkMapMemory)          \
   X(vkCreateRenderPass) X(vkCreateFramebuffer) X(vkCreateShaderModule)                   \
   X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) X(vkCreateCommandPool)          \
   X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer)              \
   X(vkResetCommandBuffer) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass)                  \
   X(vkCmdBindPipeline) X(vkCmdDraw) X(vkCmdCopyImageToBuffer) X(vkCmdPipelineBarrier)    \
   X(vkCmdCopyBufferToImage) X(vkCmdBindDescriptorSets) X(vkCreateSampler)                \
   X(vkCreateDescriptorSetLayout) X(vkCreateDescriptorPool) X(vkAllocateDescriptorSets)   \
   X(vkUpdateDescriptorSets) X(vkCreateFence) X(vkResetFences) X(vkQueueSubmit)           \
   X(vkWaitForFences)

#define DECL(f) static PFN_##f f;
FUNCS(DECL)

static VkDevice dev;
static VkQueue queue;
static VkCommandBuffer cb;
static VkFence fence;
static VkPhysicalDeviceMemoryProperties mp;

static const struct {
   const char *name;
   const uint32_t *code;
   size_t size;
} frags[] = {
#define F(n) { #n, spv_##n##_frag, sizeof(spv_##n##_frag) }
   F(fill), F(tv), F(tf), F(td), F(c1), F(c2), F(fetch), F(disc), F(alu), F(alum), F(tvm),
#undef F
};

static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
         return i;
   fprintf(stderr, "vkbench: no memory type with flags 0x%x\n", want);
   exit(1);
}

static VkDeviceMemory bind_image(VkImage img)
{
   VkMemoryRequirements mr;
   vkGetImageMemoryRequirements(dev, img, &mr);
   VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = memtype(mr.memoryTypeBits, 0) };
   VkDeviceMemory mem;
   CHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
   CHECK(vkBindImageMemory(dev, img, mem, 0));
   return mem;
}

static void *host_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buf)
{
   VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = usage };
   CHECK(vkCreateBuffer(dev, &bci, NULL, buf));
   VkMemoryRequirements mr;
   vkGetBufferMemoryRequirements(dev, *buf, &mr);
   VkMemoryAllocateInfo mai = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
      .memoryTypeIndex =
         memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
   };
   VkDeviceMemory mem;
   CHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
   CHECK(vkBindBufferMemory(dev, *buf, mem, 0));
   void *p;
   CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, &p));
   return p;
}

static VkImageView view_of(VkImage img, VkFormat fmt)
{
   VkImageViewCreateInfo ivi = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .image = img,
                                 .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
                                 .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
   VkImageView v;
   CHECK(vkCreateImageView(dev, &ivi, NULL, &v));
   return v;
}

static void barrier(VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
                    VkAccessFlags dst_access, VkPipelineStageFlags src, VkPipelineStageFlags dst)
{
   VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = src_access,
                              .dstAccessMask = dst_access, .oldLayout = from, .newLayout = to,
                              .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                              .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
                              .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
   vkCmdPipelineBarrier(cb, src, dst, 0, 0, NULL, 0, NULL, 1, &b);
}

static void begin(void)
{
   CHECK(vkResetCommandBuffer(cb, 0));
   VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
   CHECK(vkBeginCommandBuffer(cb, &cbi));
}

static void submit_wait(void)
{
   CHECK(vkEndCommandBuffer(cb));
   VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
   CHECK(vkResetFences(dev, 1, &fence));
   CHECK(vkQueueSubmit(queue, 1, &si, fence));
   CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull));
}

int main(int argc, char **argv)
{
   if (argc < 3) {
      fprintf(stderr, "usage: vkbench <libvulkan_powervr.so> <mode> [iterations] [rgba8|rgba16f]\n");
      return 2;
   }
   const char *mode = argv[2];
   const int iterations = argc > 3 ? atoi(argv[3]) : 10;
   const int f16tex = argc > 4 && !strcmp(argv[4], "rgba16f");
   const int blend = !strcmp(mode, "blend");
   const char *frag_name = blend ? "fill" : mode;
   int frag = -1;
   for (unsigned i = 0; i < sizeof(frags) / sizeof(frags[0]); i++)
      if (!strcmp(frags[i].name, frag_name))
         frag = (int)i;
   if (frag < 0 || iterations < 1) {
      fprintf(stderr, "vkbench: unknown mode %s or bad iteration count\n", mode);
      return 2;
   }

   void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
   if (!lib) {
      fprintf(stderr, "vkbench: dlopen: %s\n", dlerror());
      return 1;
   }
   VkResult (*negotiate)(uint32_t *) = (VkResult(*)(uint32_t *))dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   if (!negotiate || !gipa) {
      fprintf(stderr, "vkbench: not a Vulkan ICD\n");
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
      fprintf(stderr, "vkbench: missing instance entry points\n");
      return 1;
   }
   uint32_t n = 1;
   VkPhysicalDevice pd;
   VkResult er = vkEnumeratePhysicalDevices(inst, &n, &pd);
   if (!n || (er != VK_SUCCESS && er != VK_INCOMPLETE)) {
      fprintf(stderr, "vkbench: no physical device (%d)\n", er);
      return 1;
   }
   vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   float prio = 1;
   VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0,
                                   .queueCount = 1, .pQueuePriorities = &prio };
   VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci };
   CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
#define LOAD(f)                                                   \
   if (!(f = (PFN_##f)vkGetDeviceProcAddr(dev, #f))) {            \
      fprintf(stderr, "vkbench: no %s\n", #f);                    \
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

   /* Source texture: SIZE x SIZE, a pattern with alpha half above and half below 0.5 (for disc). */
   const VkFormat tex_fmt = f16tex ? VK_FORMAT_R16G16B16A16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM;
   const uint32_t tex_bpp = f16tex ? 8 : 4;
   VkBuffer staging;
   uint8_t *src = host_buffer((VkDeviceSize)SIZE * SIZE * tex_bpp, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &staging);
   for (uint32_t y = 0; y < SIZE; y++) {
      for (uint32_t x = 0; x < SIZE; x++) {
         const uint8_t px[4] = { (uint8_t)(x * 7), (uint8_t)(y * 5), (uint8_t)(x ^ y), (uint8_t)((x + y) & 255) };
         if (f16tex) {
            uint16_t *p = (uint16_t *)(src + ((size_t)y * SIZE + x) * 8);
            /* A half float in [0, 1): sign 0, exponent 14 (0.5..1) or 13 (0.25..0.5), mantissa from px. */
            for (int c = 0; c < 4; c++)
               p[c] = (uint16_t)(((px[c] & 0x80) ? (14u << 10) : (13u << 10)) | ((px[c] & 0x7fu) << 3));
         } else {
            memcpy(src + ((size_t)y * SIZE + x) * 4, px, 4);
         }
      }
   }
   VkImageCreateInfo tii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D,
                             .format = tex_fmt, .extent = { SIZE, SIZE, 1 }, .mipLevels = 1, .arrayLayers = 1,
                             .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                             .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
   VkImage tex;
   CHECK(vkCreateImage(dev, &tii, NULL, &tex));
   bind_image(tex);
   begin();
   barrier(tex, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
           VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
   VkBufferImageCopy up = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                            .imageExtent = { SIZE, SIZE, 1 } };
   vkCmdCopyBufferToImage(cb, staging, tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &up);
   barrier(tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
           VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
   submit_wait();

   VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, .magFilter = VK_FILTER_LINEAR,
                               .minFilter = VK_FILTER_LINEAR, .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                               .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                               .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT, .maxLod = 0 };
   VkSampler sampler;
   CHECK(vkCreateSampler(dev, &sci, NULL, &sampler));
   VkDescriptorSetLayoutBinding dlb = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                        VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
   VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = 1, .pBindings = &dlb };
   VkDescriptorSetLayout dsl;
   CHECK(vkCreateDescriptorSetLayout(dev, &dli, NULL, &dsl));
   VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
   VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
                                      .poolSizeCount = 1, .pPoolSizes = &ps };
   VkDescriptorPool dpool;
   CHECK(vkCreateDescriptorPool(dev, &dpi, NULL, &dpool));
   VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                       .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
   VkDescriptorSet set;
   CHECK(vkAllocateDescriptorSets(dev, &dai, &set));
   VkDescriptorImageInfo dii = { sampler, view_of(tex, tex_fmt), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
   VkWriteDescriptorSet wds = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
                                .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                .pImageInfo = &dii };
   vkUpdateDescriptorSets(dev, 1, &wds, 0, NULL);

   /* Target and readback. The blend mode loads the previous contents; the others clear. */
   VkImageCreateInfo rti = tii;
   rti.format = VK_FORMAT_R8G8B8A8_UNORM;
   rti.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
   VkImage rt;
   CHECK(vkCreateImage(dev, &rti, NULL, &rt));
   bind_image(rt);
   VkImageView rt_view = view_of(rt, VK_FORMAT_R8G8B8A8_UNORM);
   VkBuffer readback;
   const uint8_t *px = host_buffer((VkDeviceSize)SIZE * SIZE * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &readback);
   begin();
   barrier(rt, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 0,
           VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
   submit_wait();

   VkAttachmentDescription ad = { .format = VK_FORMAT_R8G8B8A8_UNORM, .samples = VK_SAMPLE_COUNT_1_BIT,
                                  .loadOp = blend ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR,
                                  .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                                  .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                  .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                  .initialLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                  .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
   VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
   VkSubpassDescription sd = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS, .colorAttachmentCount = 1,
                               .pColorAttachments = &ar };
   VkSubpassDependency deps[2] = {
      { VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0 },
      { 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, 0 },
   };
   VkRenderPassCreateInfo rpi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, .attachmentCount = 1,
                                  .pAttachments = &ad, .subpassCount = 1, .pSubpasses = &sd,
                                  .dependencyCount = 2, .pDependencies = deps };
   VkRenderPass rp;
   CHECK(vkCreateRenderPass(dev, &rpi, NULL, &rp));
   VkFramebufferCreateInfo fbi = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, .renderPass = rp,
                                   .attachmentCount = 1, .pAttachments = &rt_view, .width = SIZE,
                                   .height = SIZE, .layers = 1 };
   VkFramebuffer fb;
   CHECK(vkCreateFramebuffer(dev, &fbi, NULL, &fb));

   VkShaderModuleCreateInfo vsi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = sizeof(spv_vert_vert), .pCode = spv_vert_vert };
   VkShaderModuleCreateInfo fsi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = frags[frag].size, .pCode = frags[frag].code };
   VkShaderModule vs, fs;
   CHECK(vkCreateShaderModule(dev, &vsi, NULL, &vs));
   CHECK(vkCreateShaderModule(dev, &fsi, NULL, &fs));
   VkPipelineShaderStageCreateInfo stages[2] = {
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
        .module = vs, .pName = "main" },
      { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
        .module = fs, .pName = "main" },
   };
   VkPipelineVertexInputStateCreateInfo vis = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
   VkPipelineInputAssemblyStateCreateInfo ias = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                  .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
   VkViewport vp = { 0, 0, SIZE, SIZE, 0, 1 };
   VkRect2D sc = { { 0, 0 }, { SIZE, SIZE } };
   VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                             .viewportCount = 1, .pViewports = &vp, .scissorCount = 1,
                                             .pScissors = &sc };
   VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                 .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
                                                 .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1 };
   VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                               .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
   VkPipelineColorBlendAttachmentState cba = {
      .blendEnable = blend, .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
      .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, .colorBlendOp = VK_BLEND_OP_ADD,
      .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE, .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
      .alphaBlendOp = VK_BLEND_OP_ADD,
      .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                        VK_COLOR_COMPONENT_A_BIT
   };
   VkPipelineColorBlendStateCreateInfo cbs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                               .attachmentCount = 1, .pAttachments = &cba };
   VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                      .pSetLayouts = &dsl };
   VkPipelineLayout pl;
   CHECK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));
   VkGraphicsPipelineCreateInfo gpi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
                                        .pStages = stages, .pVertexInputState = &vis, .pInputAssemblyState = &ias,
                                        .pViewportState = &vps, .pRasterizationState = &rs,
                                        .pMultisampleState = &ms, .pColorBlendState = &cbs, .layout = pl,
                                        .renderPass = rp, .subpass = 0 };
   VkPipeline pipe;
   CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, NULL, &pipe));

   uint64_t sum = 0;
   for (int it = 0; it < iterations; it++) {
      begin();
      VkClearValue cv = { .color.float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };
      VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp,
                                    .framebuffer = fb, .renderArea = sc, .clearValueCount = 1, .pClearValues = &cv };
      vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, NULL);
      vkCmdDraw(cb, 3, 1, 0, 0);
      vkCmdEndRenderPass(cb);
      if (it == iterations - 1) {
         VkBufferImageCopy r = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                                 .imageExtent = { SIZE, SIZE, 1 } };
         vkCmdCopyImageToBuffer(cb, rt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback, 1, &r);
         VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
         vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0,
                              NULL);
      }
      submit_wait();
   }
   for (size_t i = 0; i < (size_t)SIZE * SIZE * 4; i++)
      sum += px[i];
   printf("vkbench %s%s: %d passes, sum %llu\n", mode, f16tex ? " rgba16f" : "", iterations,
          (unsigned long long)sum);
   return sum ? 0 : 3;
}
