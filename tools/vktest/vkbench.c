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
//   ubo    tv plus 12 flag-controlled branches reading 28 scalar uniforms from one UBO (set 0 binding 1)
//   load   fill, no blending, previous contents loaded   blendc  blend over a cleared target
//   ubod   ubo with the 512 B UBO in a device-local, non-host-visible memory type (staging copy)
//   ubo128 shader (ubo shape, 12 flag branches, one 128 B std140 block: vec4 v[5], ivec4 m[3]) run as
//          u128h UBO host-visible+coherent   u128d UBO device-local (staging copy)
//          (ubogap: u128k / u128k0 = u128p with the flags as shader constants, default pattern / all 0)
//          u128i inline uniform block (VK_EXT_inline_uniform_block)   u128p push constants
//          All four use identical data, so their pixel sums are equal. UBO_FLAGS=0|1 sets all 12 flags to 0 or 1
//          (default: the pattern of ubo); the sum then differs, the four variants still agree.
//
// Pipeline cache test: with VKBENCH_CACHE=<file> the pipelines are created through a VkPipelineCache
// that starts from <file> (if it exists) and is saved back to it (written atomically) at the end;
// VKBENCH_CACHE_ALL=1 also creates every mode's pipeline, with and without blending. It prints, per run,
// how many pipelines the driver reports as found in the cache (VK_EXT_pipeline_creation_feedback) and
// the time vkCreateGraphicsPipelines took. Run it twice (new process each time): the second run should
// report every pipeline as found in the cache, and the image sum must not change. VKBENCH_FEEDBACK=1 does the
// same without a VkPipelineCache (the driver's device cache and, if MESA_SHADER_CACHE_DIR is set, its disk cache).
// The feedback only says "found" for a pipeline found in the application's VkPipelineCache (as the spec has it), not
// for one served from the device or disk cache: there the vkCreateGraphicsPipelines time is the evidence.
// CPU wall-time mode: BENCH_WALL=<ms> (e.g. 300) runs passes (one submit each) until the GED level reads 0 (680 MHz,
// top), then <ms> more, capped at 5 s, then times the <iterations> passes recorded in ONE command buffer, one submit, one wait, with
// CLOCK_MONOTONIC, then one more pass with the readback for the sum. Prints
// "wall <mode> <ms>/pass freq <before> -> <after>": the GED clock samples (read-only, /sys/kernel/ged/hal/
// current_freqency) taken just before and just after the timed block tag the result with its clock level.
// Firmware job times are only valid inside one clock level (the kernel converts them with a stale clock
// estimate); wall time is not affected. XCLIPSE: 680 MHz vk/gl ratios from this mode: see docs/ddk113/TESTING.md.
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
#include <time.h>

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
   X(vkWaitForFences) X(vkCreatePipelineCache) X(vkGetPipelineCacheData)                     \
   X(vkCmdCopyBuffer) X(vkCmdPushConstants)

#define DECL(f) static PFN_##f f;
FUNCS(DECL)

static VkDevice dev;
static VkQueue queue;
static VkCommandBuffer cb;
static VkFence fence;
static VkPhysicalDeviceMemoryProperties mp;

/* The first NFRAGS_CACHE entries are the modes VKBENCH_CACHE_ALL creates (unchanged by the ubo128 shaders). */
#define NFRAGS_CACHE 12
static const struct {
   const char *name;
   const uint32_t *code;
   size_t size;
} frags[] = {
#define F(n) { #n, spv_##n##_frag, sizeof(spv_##n##_frag) }
   F(fill), F(tv), F(tf), F(td), F(c1), F(c2), F(fetch), F(disc), F(alu), F(alum), F(tvm), F(ubo), F(ubo128), F(ubo128p), F(ubo128k), F(ubo128k0), F(ubox),
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

static double now_ms(void)
{
   struct timespec t;
   clock_gettime(CLOCK_MONOTONIC, &t);
   return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static double cpu_ms(void)
{
   struct timespec t;
   clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t);
   return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void ged_freq(char *out, size_t n)
{
   FILE *f = fopen("/sys/kernel/ged/hal/current_freqency", "r");
   out[0] = 0;
   if (f) {
      if (fgets(out, (int)n, f))
         out[strcspn(out, "\n")] = 0;
      fclose(f);
   }
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
   /* blend: blending over a loaded target. load: the load without blending. blendc: blending over a cleared
    * target. Together they split the cost of the blend mode into its two parts. */
   const int blend = !strcmp(mode, "blend") || !strcmp(mode, "blendc");
   const int load = !strcmp(mode, "blend") || !strcmp(mode, "load");
   const int u128k = !strcmp(mode, "u128k"), u128k0 = !strcmp(mode, "u128k0"); /* XCLIPSE: ubogap: flags as shader constants */
   const int u128p = !strcmp(mode, "u128p") || u128k || u128k0, u128i = !strcmp(mode, "u128i");
   const int u128d = !strcmp(mode, "u128d"), u128h = !strcmp(mode, "u128h");
   const int ubo = !strcmp(mode, "ubo"), ubod = !strcmp(mode, "ubod");
   /* XCLIPSE: ubofix. UBO_DYN=whole|range (mode ubo): binding 1 is a UNIFORM_BUFFER_DYNAMIC in a 4096 B host-visible buffer.
    * whole: the descriptor range is VK_WHOLE_SIZE and the dynamic offset puts the window at the end of the buffer with only
    * UBO_DYN_VALID bytes (default 256) of the data in it (the same sum as UBO_TAILZERO=<valid> without dynamic offsets: the
    * rest of the block must read as zero, the driver must not read past the buffer). range: an explicit range of 512 B at
    * dynamic offset 1024 (the same sum as plain ubo: an explicit range is not reduced by the dynamic offset). */
   const char *ubo_dyn = getenv("UBO_DYN");
   const int udyn = ubo && ubo_dyn && (!strcmp(ubo_dyn, "whole") || !strcmp(ubo_dyn, "range"));
   const int udyn_whole = udyn && !strcmp(ubo_dyn, "whole");
   uint32_t dyn_off = 0;
   const int ubo_buf = ubo || ubod || u128h || u128d; /* binding 1 is a UBO */
   /* XCLIPSE: ubofix. ubox: two UBOs (bindings 1, 2) whose promoted ranges are back to back, and UBO_PAD=<n> extra unused UBO
    * descriptors (binding 3, 4 dwords each) that move the ranges' first shared register in steps of 4: the vec4 read of the second
    * range starts at an odd register, so some n makes it a repeat across shared register 255 -> 256. */
   const int ubox = !strcmp(mode, "ubox");
   const int ubox_pad = ubox && getenv("UBO_PAD") ? atoi(getenv("UBO_PAD")) : 0;
   const int has_b1 = ubo_buf || u128i;
   const char *frag_name = ubox ? "ubox" : (blend || load) ? "fill" : u128k ? "ubo128k" : u128k0 ? "ubo128k0" : u128p ? "ubo128p" : (u128h || u128d || u128i) ? "ubo128"
                           : ubod ? "ubo" : mode;
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
   const char *dev_exts[2];
   uint32_t n_exts = 0;
   if (getenv("VKBENCH_CACHE") || getenv("VKBENCH_FEEDBACK"))
      dev_exts[n_exts++] = "VK_EXT_pipeline_creation_feedback";
   VkPhysicalDeviceInlineUniformBlockFeaturesEXT iubf = {
      .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES_EXT, .inlineUniformBlock = VK_TRUE };
   if (u128i)
      dev_exts[n_exts++] = "VK_EXT_inline_uniform_block";
   VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = u128i ? &iubf : NULL,
                              .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = n_exts,
                              .ppEnabledExtensionNames = dev_exts };
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
   VkDescriptorSetLayoutBinding dlb[4] = { { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                             VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
                                           { 1, u128i ? VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT
                                                      : udyn ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                                                             : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                             u128i ? 128 : 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
                                           { 2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
                                           { 3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, (uint32_t)(ubox_pad > 0 ? ubox_pad : 1),
                                             VK_SHADER_STAGE_FRAGMENT_BIT, NULL } };
   VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = ubox ? (ubox_pad > 0 ? 4 : 3) : has_b1 ? 2 : 1, .pBindings = dlb };
   VkDescriptorSetLayout dsl;
   CHECK(vkCreateDescriptorSetLayout(dev, &dli, NULL, &dsl));
   VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 },
                                  { u128i ? VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT
                                          : udyn ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                    u128i ? 128 : ubox ? 2 + ubox_pad : 1 } };
   VkDescriptorPoolInlineUniformBlockCreateInfoEXT dpiub = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_INLINE_UNIFORM_BLOCK_CREATE_INFO_EXT,
      .maxInlineUniformBlockBindings = 1 };
   VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                      .pNext = u128i ? &dpiub : NULL, .maxSets = 1,
                                      .poolSizeCount = has_b1 || ubox ? 2 : 1, .pPoolSizes = ps };
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
   float u128[32]; /* 128 B std140: vec4 v[5] at 0, ivec4 m[3] at 80, no pad (also the push constants). XCLIPSE: board 4 lane fold: an
                   * earlier version zeroed floats 28..31 as a "pad", which are m[8..11]: those runs had 6 flags on, not 9. */
   if (has_b1 || u128p) {
      const char *uf = getenv("UBO_FLAGS");
      if (u128h || u128d || u128i || u128p) {
         for (unsigned i = 0; i < 32; i++)
            u128[i] = 0.05f + 0.03f * (float)(i % 11);
         int32_t *m = (int32_t *)u128 + 20;
         for (unsigned i = 0; i < 12; i++)
            m[i] = uf ? atoi(uf) != 0 : ((i * 7u + 3u) % 5u) != 0;
      }
   }
   VkBuffer ubo_vkbuf = VK_NULL_HANDLE;
   /* XCLIPSE: ubofix. UBO_SAMECB=1 (mode ubod): the last submit copies zeros into the device-local UBO, draws, copies the
    * data in, draws again and reads that second draw back, all in one command buffer: its sum equals plain ubod only if
    * the PDS DMA of the promoted ranges sees the copy that a barrier ordered before it. */
   VkBuffer ubo_dev = VK_NULL_HANDLE, ubo_stage_data = VK_NULL_HANDLE, ubo_stage_zero = VK_NULL_HANDLE;
   VkDeviceSize ubo_dev_size = 0;
   const int ubo_samecb = ubod && getenv("UBO_SAMECB");
   if (has_b1) {
      VkBuffer ub = VK_NULL_HANDLE;
      float u512[32 * 4];
      const void *udata = u128;
      VkDeviceSize usize = 128;
      if (ubo || ubod) {
         /* 8 vec4, 8 ivec4 (alternating flags), 16 floats at a 16-byte stride (std140). */
         for (unsigned i = 0; i < 32 * 4; i++)
            u512[i] = 0.05f + 0.03f * (float)(i % 11);
         int32_t *m = (int32_t *)u512 + 8 * 4;
         for (unsigned i = 0; i < 8 * 4; i++)
            m[i] = ((i * 7u + 3u) % 5u) != 0;
         udata = u512;
         usize = sizeof(u512);
      }
      if (u128i) {
         /* the data goes into the descriptor set itself */
      } else if (ubod || u128d) {
         /* device-local, not host-visible if the device has such a type; filled by a staging copy */
         VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = usize,
                                    .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
         CHECK(vkCreateBuffer(dev, &bci, NULL, &ub));
         VkMemoryRequirements mr;
         vkGetBufferMemoryRequirements(dev, ub, &mr);
         int ti = -1, ti_vis = -1;
         for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
            const VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
            if (!(mr.memoryTypeBits & (1u << i)) || !(f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
               continue;
            if (!(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && ti < 0)
               ti = (int)i;
            if (ti_vis < 0)
               ti_vis = (int)i;
         }
         if (ti < 0) {
            printf("vkbench: NOTE no DEVICE_LOCAL memory type without HOST_VISIBLE for a UBO: the device-local run uses "
                   "type %d (host-visible too), so the test is moot\n", ti_vis);
            ti = ti_vis;
         }
         if (ti < 0) {
            fprintf(stderr, "vkbench: no DEVICE_LOCAL memory type for a UBO\n");
            return 1;
         }
         printf("vkbench: UBO memory type %d flags 0x%x\n", ti, mp.memoryTypes[ti].propertyFlags);
         VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                      .memoryTypeIndex = (uint32_t)ti };
         VkDeviceMemory umem;
         CHECK(vkAllocateMemory(dev, &mai, NULL, &umem));
         CHECK(vkBindBufferMemory(dev, ub, umem, 0));
         VkBuffer ustage;
         void *sp = host_buffer(usize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &ustage);
         memcpy(sp, udata, usize);
         ubo_dev = ub;
         ubo_stage_data = ustage;
         ubo_dev_size = usize;
         if (ubo_samecb) {
            void *zp = host_buffer(usize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &ubo_stage_zero);
            memset(zp, 0, usize);
         }
         begin();
         VkBufferCopy bc = { 0, 0, usize };
         vkCmdCopyBuffer(cb, ustage, ub, 1, &bc);
         VkMemoryBarrier umb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                                 .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                 .dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT };
         vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &umb,
                              0, NULL, 0, NULL);
         submit_wait();
      } else {
         void *hp;
         if (udyn) {
            hp = host_buffer(4096, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &ub);
            memset(hp, 0, 4096);
            const uint32_t valid = getenv("UBO_DYN_VALID") ? (uint32_t)atoi(getenv("UBO_DYN_VALID")) : 256;
            if (udyn_whole) {
               dyn_off = 4096 - valid;
               memcpy((uint8_t *)hp + dyn_off, udata, valid);
            } else {
               dyn_off = 1024;
               memcpy((uint8_t *)hp + dyn_off, udata, usize);
            }
         } else {
            hp = host_buffer(usize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &ub);
            memcpy(hp, udata, usize);
         }
         if (ubo) {
            ubo_vkbuf = ub;
            /* UBO_TAILZERO=<bytes>: zero the buffer from this offset on (the reference of UBO_RANGE). */
            if (getenv("UBO_TAILZERO")) {
               /* clamped: an offset past the buffer would make the length negative (a huge size_t) */
               long tz = atol(getenv("UBO_TAILZERO"));
               tz = tz < 0 ? 0 : tz > (long)usize ? (long)usize : tz;
               memset((uint8_t *)hp + tz, 0, usize - tz);
            }
         }
      }
      VkDescriptorBufferInfo dbi = { ub, 0, udyn && !udyn_whole ? 512 : VK_WHOLE_SIZE };
      VkWriteDescriptorSetInlineUniformBlockEXT iub = {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK_EXT, .dataSize = 128, .pData = u128 };
      VkWriteDescriptorSet wds2[2] = { wds, { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set,
                                               .pNext = u128i ? &iub : NULL, .dstBinding = 1,
                                               .descriptorCount = u128i ? 128 : 1,
                                               .descriptorType = u128i ? VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK_EXT
                                                                       : udyn ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC
                                                                              : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                               .pBufferInfo = u128i ? NULL : &dbi } };
      vkUpdateDescriptorSets(dev, 2, wds2, 0, NULL);
   } else if (ubox) {
      static const float da[4] = { 0.30f, 0.20f, 0.10f, 0.9f }, db[6] = { 0.11f, 0.22f, 0.33f, 0.44f, 0.5f, 0.6f };
      VkBuffer ba, bb;
      memcpy(host_buffer(16, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &ba), da, sizeof(da));
      memcpy(host_buffer(32, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &bb), db, sizeof(db));
      VkDescriptorBufferInfo ia = { ba, 0, VK_WHOLE_SIZE }, ib = { bb, 0, VK_WHOLE_SIZE };
      VkWriteDescriptorSet wx[3] = { wds, { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1,
                                            .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                            .pBufferInfo = &ia },
                                     { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 2,
                                       .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                       .pBufferInfo = &ib } };
      vkUpdateDescriptorSets(dev, 3, wx, 0, NULL);
   } else {
      vkUpdateDescriptorSets(dev, 1, &wds, 0, NULL);
   }

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
                                  .loadOp = load ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR,
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
   /* BENCH_FBS=<n>: the timed passes cycle over n framebuffers (n render targets, so n of the driver's RT datasets). */
   VkFramebuffer fbs[8] = { fb };
   int nfb = getenv("BENCH_FBS") ? atoi(getenv("BENCH_FBS")) : 1;
   if (nfb < 1 || nfb > 8)
      nfb = 1;
   for (int k = 1; k < nfb; k++) {
      VkImage rtk;
      CHECK(vkCreateImage(dev, &rti, NULL, &rtk));
      bind_image(rtk);
      VkImageView vk_ = view_of(rtk, VK_FORMAT_R8G8B8A8_UNORM);
      begin();
      barrier(rtk, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0, 0,
              VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
      submit_wait();
      VkFramebufferCreateInfo fbk = fbi;
      fbk.pAttachments = &vk_;
      CHECK(vkCreateFramebuffer(dev, &fbk, NULL, &fbs[k]));
   }
   unsigned pass_no = 0;
   /* BENCH_NODRAW=1: render passes without a draw (clear only), the case the driver's dummy object exists for. */
   const int nodraw = getenv("BENCH_NODRAW") != NULL;

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
   /* BENCH_RA=<n>: render area, viewport and scissor n x n (a multiple of 32), the rest of the target untouched. */
   const uint32_t ra = getenv("BENCH_RA") ? (uint32_t)atoi(getenv("BENCH_RA")) : SIZE;
   VkViewport vp = { 0, 0, ra, ra, 0, 1 };
   VkRect2D sc = { { 0, 0 }, { ra, ra } };
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
   VkPushConstantRange pcr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128 };
   VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                      .pSetLayouts = &dsl, .pushConstantRangeCount = u128p ? 1 : 0,
                                      .pPushConstantRanges = &pcr };
   VkPipelineLayout pl;
   CHECK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));
   VkGraphicsPipelineCreateInfo gpi = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .stageCount = 2,
                                        .pStages = stages, .pVertexInputState = &vis, .pInputAssemblyState = &ias,
                                        .pViewportState = &vps, .pRasterizationState = &rs,
                                        .pMultisampleState = &ms, .pColorBlendState = &cbs, .layout = pl,
                                        .renderPass = rp, .subpass = 0 };
   VkPipeline pipe;
   const char *cache_file = getenv("VKBENCH_CACHE");
   /* VKBENCH_FEEDBACK=1: report cache hits without a VkPipelineCache (device cache and disk cache only). */
   const int use_fb = cache_file || getenv("VKBENCH_FEEDBACK");
   VkPipelineCache pcache = VK_NULL_HANDLE;
   unsigned n_pipes = 0, n_hit = 0;
   double create_ms = 0;
   if (cache_file) {
      size_t init_size = 0;
      void *init = NULL;
      FILE *cf = fopen(cache_file, "rb");
      if (cf) {
         fseek(cf, 0, SEEK_END);
         init_size = (size_t)ftell(cf);
         fseek(cf, 0, SEEK_SET);
         init = malloc(init_size ? init_size : 1);
         if (!init || fread(init, 1, init_size, cf) != init_size)
            init_size = 0;
         fclose(cf);
      }
      VkPipelineCacheCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
                                        .initialDataSize = init_size, .pInitialData = init };
      CHECK(vkCreatePipelineCache(dev, &pci, NULL, &pcache));
      printf("vkbench cache: initial data %zu bytes\n", init_size);
      free(init);
   }
   /* One pipeline per call so that the feedback is per pipeline. */
   VkPipelineCreationFeedback fb_pipe, fb_stages[2];
   VkPipelineCreationFeedbackCreateInfo fbi_ext = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO,
                                                    .pPipelineCreationFeedback = &fb_pipe,
                                                    .pipelineStageCreationFeedbackCount = 2,
                                                    .pPipelineStageCreationFeedbacks = fb_stages };
   if (use_fb)
      gpi.pNext = &fbi_ext;
#define CREATE(out)                                                                            \
   do {                                                                                        \
      struct timespec t0_, t1_;                                                                \
      memset(&fb_pipe, 0, sizeof(fb_pipe));                                                    \
      clock_gettime(CLOCK_MONOTONIC, &t0_);                                                    \
      CHECK(vkCreateGraphicsPipelines(dev, pcache, 1, &gpi, NULL, out));                       \
      clock_gettime(CLOCK_MONOTONIC, &t1_);                                                    \
      create_ms += (t1_.tv_sec - t0_.tv_sec) * 1e3 + (t1_.tv_nsec - t0_.tv_nsec) / 1e6;       \
      n_pipes++;                                                                               \
      if (use_fb && (fb_pipe.flags & VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT) &&           \
          (fb_pipe.flags & VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT))  \
         n_hit++;                                                                              \
   } while (0)
   if (use_fb && getenv("VKBENCH_CACHE_ALL")) {
      for (unsigned k = 0; k < 2 * NFRAGS_CACHE; k++) {
         VkShaderModuleCreateInfo ksi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                          .codeSize = frags[k / 2].size, .pCode = frags[k / 2].code };
         VkShaderModule kfs;
         CHECK(vkCreateShaderModule(dev, &ksi, NULL, &kfs));
         VkPipelineShaderStageCreateInfo kst[2] = { stages[0], stages[1] };
         kst[1].module = kfs;
         VkPipelineColorBlendAttachmentState kcba = cba;
         kcba.blendEnable = k & 1;
         VkPipelineColorBlendStateCreateInfo kcbs = cbs;
         kcbs.pAttachments = &kcba;
         VkGraphicsPipelineCreateInfo kgpi = gpi;
         kgpi.pStages = kst;
         kgpi.pColorBlendState = &kcbs;
         VkPipeline kp;
         VkGraphicsPipelineCreateInfo save = gpi;
         gpi = kgpi;
         CREATE(&kp);
         gpi = save;
      }
   }
   CREATE(&pipe);
   size_t sz = 0;
   if (cache_file) {
      CHECK(vkGetPipelineCacheData(dev, pcache, &sz, NULL));
      void *data = malloc(sz ? sz : 1);
      CHECK(vkGetPipelineCacheData(dev, pcache, &sz, data));
      char tmp[512];
      snprintf(tmp, sizeof(tmp), "%s.tmp", cache_file);
      FILE *cf = fopen(tmp, "wb");
      if (!cf || fwrite(data, 1, sz, cf) != sz || fclose(cf) || rename(tmp, cache_file)) {
         fprintf(stderr, "vkbench: cannot save %s\n", cache_file);
         return 1;
      }
      free(data);
   }
   if (use_fb)
      printf("vkbench cache: %u pipelines, %u reported found in the cache, vkCreateGraphicsPipelines %.2f ms, "
             "saved %zu bytes\n", n_pipes, n_hit, create_ms, sz);

   uint64_t sum = 0;
   const char *wall_env = getenv("BENCH_WALL");
   const int wall = wall_env != NULL;
   const int warmup = wall ? atoi(wall_env) : 0;
   VkClearValue cv = { .color.float32 = { 0.0f, 0.0f, 0.0f, 1.0f } };
   VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, .renderPass = rp,
                                 .framebuffer = fb, .renderArea = sc, .clearValueCount = 1, .pClearValues = &cv };
#define RECORD_PASS()                                                                    \
   do {                                                                                  \
      rbi.framebuffer = fbs[pass_no++ % nfb];                                            \
      vkCmdBeginRenderPass(cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);                        \
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);                      \
      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, udyn ? 1 : 0, &dyn_off); \
      if (u128p)                                                                         \
         vkCmdPushConstants(cb, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128, u128);         \
      if (!nodraw)                                                                       \
         vkCmdDraw(cb, 3, 1, 0, 0);                                                      \
      vkCmdEndRenderPass(cb);                                                            \
   } while (0)
   if (wall) {
      char f0[32], f1[32];
      struct timespec t0, t1;
      /* XCLIPSE: warm up on the clock, not the pass count: 400 passes (150 ms) left GED at 400/500 MHz. */
      double start = now_ms(), top = 0;
      int warm = 0;
      for (;;) {
         begin(); /* batches of 64: one pass per submit leaves the GPU mostly idle and GED stays low */
         for (int k = 0; k < 64; k++)
            RECORD_PASS();
         submit_wait();
         warm += 64;
         double t = now_ms();
         if (!top &&  t - start > 200) { /* a 0 read earlier is the previous process's */
            ged_freq(f0, sizeof(f0));
            if (!strncmp(f0, "0 ", 2))
               top = t;
         }
         if ((top && t - top >= warmup) || t - start > 5000)
            break;
      }
      begin();
      for (int it = 0; it < iterations; it++)
         RECORD_PASS();
      CHECK(vkEndCommandBuffer(cb));
      VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
      CHECK(vkResetFences(dev, 1, &fence));
      ged_freq(f0, sizeof(f0));
      const double c0 = cpu_ms();
      clock_gettime(CLOCK_MONOTONIC, &t0);
      CHECK(vkQueueSubmit(queue, 1, &si, fence));
      struct timespec ts;
      clock_gettime(CLOCK_MONOTONIC, &ts);
      CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 60000000000ull));
      clock_gettime(CLOCK_MONOTONIC, &t1);
      const double c1 = cpu_ms();
      ged_freq(f1, sizeof(f1));
      printf("wall vk %s %.4f ms/pass (%d passes, %d warm-up passes) freq %s -> %s, process cpu %.4f ms/pass\n", mode,
             ((t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6) / iterations, iterations, warm,
             f0, f1, (c1 - c0) / iterations);
      printf("submit vk %s %.4f ms/pass (vkQueueSubmit alone, CLOCK_MONOTONIC; the rest of the wall is the wait)\n", mode,
             ((ts.tv_sec - t0.tv_sec) * 1e3 + (ts.tv_nsec - t0.tv_nsec) / 1e6) / iterations);
   }
   /* UBO_RANGE=<bytes>: a UBO range smaller than the block (the shader reads past it: robustBufferAccess is off, so
    * that is undefined in Vulkan; this driver promotes the reads to shared registers and defines the tail as zero,
    * which UBO_TAILZERO=<same bytes> on a full-range run must reproduce). A full-range pass first leaves the
    * shared registers holding the buffer's tail, then the descriptor is rewritten with the short range. */
   if (ubo_vkbuf && getenv("UBO_RANGE")) {
      begin();
      RECORD_PASS();
      submit_wait();
      VkDescriptorBufferInfo sdbi = { ubo_vkbuf, 0, (VkDeviceSize)atoi(getenv("UBO_RANGE")) };
      VkWriteDescriptorSet sw = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = set, .dstBinding = 1,
                                  .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                  .pBufferInfo = &sdbi };
      vkUpdateDescriptorSets(dev, 1, &sw, 0, NULL);
   }
   pass_no = 0;
   for (int it = wall ? iterations - 1 : 0; it < iterations; it++) {
      begin();
      if (ubo_samecb && it == iterations - 1) {
#define UBO_COPY(src)                                                                                          \
   do {                                                                                                        \
      VkMemoryBarrier b1 = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_UNIFORM_READ_BIT, \
                             .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };                                  \
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &b1, \
                           0, NULL, 0, NULL);                                                                  \
      VkBufferCopy bc2 = { 0, 0, ubo_dev_size };                                                               \
      vkCmdCopyBuffer(cb, src, ubo_dev, 1, &bc2);                                                              \
      VkMemoryBarrier b2 = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, \
                             .dstAccessMask = VK_ACCESS_UNIFORM_READ_BIT };                                    \
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 1, &b2, \
                           0, NULL, 0, NULL);                                                                  \
   } while (0)
         UBO_COPY(ubo_stage_zero);
         RECORD_PASS(); /* reads zeros */
         UBO_COPY(ubo_stage_data);
      }
      RECORD_PASS();
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
