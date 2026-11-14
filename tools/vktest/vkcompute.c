// SPDX-License-Identifier: MIT
//
// vkcompute: the smallest Vulkan compute job for bring-up (docs/ddk113/TESTING.md, L4). It loads the
// ICD directly (no loader), runs vkcompute.comp (one 64-invocation workgroup writing v[i] = 3i + 1)
// a few times and prints the same checksum as `glwork compute` (6112 when correct).
//
//   vkcompute /path/to/libvulkan_powervr.so [iterations]
//
// Build (armv7): $NDK/.../armv7a-linux-androideabi29-clang -O2 -Wall tools/vktest/vkcompute.c -ldl \
//   -o vkcompute.  vkcompute_spv.h: glslangValidator -V --vn vkcompute_spv -o vkcompute_spv.h \
//   vkcompute.comp

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vkcompute_spv.h"

#define N 64

#define CHECK(x)                                                              \
   do {                                                                       \
      VkResult r_ = (x);                                                      \
      if (r_ != VK_SUCCESS) {                                                 \
         fprintf(stderr, "vkcompute: %s failed: %d (line %d)\n", #x, r_, __LINE__); \
         exit(1);                                                             \
      }                                                                       \
   } while (0)

#define LOAD(get, obj, f)                                                     \
   PFN_##f f = (PFN_##f)get(obj, #f);                                         \
   if (!f) {                                                                  \
      fprintf(stderr, "vkcompute: no %s\n", #f);                             \
      return 1;                                                               \
   }

int main(int argc, char **argv)
{
   if (argc < 2) {
      fprintf(stderr, "usage: vkcompute <libvulkan_powervr.so> [iterations]\n");
      return 2;
   }
   int iterations = argc > 2 ? atoi(argv[2]) : 3;
   void *lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
   if (!lib) {
      fprintf(stderr, "vkcompute: dlopen: %s\n", dlerror());
      return 1;
   }
   VkResult (*negotiate)(uint32_t *) = (VkResult(*)(uint32_t *))dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   if (!negotiate || !gipa) {
      fprintf(stderr, "vkcompute: not a Vulkan ICD\n");
      return 1;
   }
   uint32_t icd_version = 5;
   negotiate(&icd_version);

   LOAD(gipa, NULL, vkCreateInstance);
   VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_1 };
   VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
   VkInstance inst;
   CHECK(vkCreateInstance(&ici, NULL, &inst));
   LOAD(gipa, inst, vkEnumeratePhysicalDevices);
   LOAD(gipa, inst, vkGetPhysicalDeviceQueueFamilyProperties);
   LOAD(gipa, inst, vkGetPhysicalDeviceMemoryProperties);
   LOAD(gipa, inst, vkCreateDevice);
   LOAD(gipa, inst, vkGetDeviceProcAddr);

   uint32_t n = 1;
   VkPhysicalDevice pd;
   VkResult er = vkEnumeratePhysicalDevices(inst, &n, &pd);
   if (!n || (er != VK_SUCCESS && er != VK_INCOMPLETE)) {
      fprintf(stderr, "vkcompute: no physical device (%d)\n", er);
      return 1;
   }
   VkQueueFamilyProperties qf[8];
   n = 8;
   vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, qf);
   uint32_t qfam = UINT32_MAX;
   for (uint32_t i = 0; i < n && qfam == UINT32_MAX; i++)
      if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
         qfam = i;
   if (qfam == UINT32_MAX) {
      fprintf(stderr, "vkcompute: no compute queue\n");
      return 1;
   }

   float prio = 1;
   VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                   .queueFamilyIndex = qfam, .queueCount = 1, .pQueuePriorities = &prio };
   VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                              .pQueueCreateInfos = &qci };
   VkDevice dev;
   CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
   LOAD(vkGetDeviceProcAddr, dev, vkGetDeviceQueue);
   LOAD(vkGetDeviceProcAddr, dev, vkCreateBuffer);
   LOAD(vkGetDeviceProcAddr, dev, vkGetBufferMemoryRequirements);
   LOAD(vkGetDeviceProcAddr, dev, vkAllocateMemory);
   LOAD(vkGetDeviceProcAddr, dev, vkBindBufferMemory);
   LOAD(vkGetDeviceProcAddr, dev, vkMapMemory);
   LOAD(vkGetDeviceProcAddr, dev, vkCreateShaderModule);
   LOAD(vkGetDeviceProcAddr, dev, vkCreateDescriptorSetLayout);
   LOAD(vkGetDeviceProcAddr, dev, vkCreatePipelineLayout);
   LOAD(vkGetDeviceProcAddr, dev, vkCreateComputePipelines);
   LOAD(vkGetDeviceProcAddr, dev, vkCreateDescriptorPool);
   LOAD(vkGetDeviceProcAddr, dev, vkAllocateDescriptorSets);
   LOAD(vkGetDeviceProcAddr, dev, vkUpdateDescriptorSets);
   LOAD(vkGetDeviceProcAddr, dev, vkCreateCommandPool);
   LOAD(vkGetDeviceProcAddr, dev, vkAllocateCommandBuffers);
   LOAD(vkGetDeviceProcAddr, dev, vkBeginCommandBuffer);
   LOAD(vkGetDeviceProcAddr, dev, vkCmdBindPipeline);
   LOAD(vkGetDeviceProcAddr, dev, vkCmdBindDescriptorSets);
   LOAD(vkGetDeviceProcAddr, dev, vkCmdDispatch);
   LOAD(vkGetDeviceProcAddr, dev, vkCmdPipelineBarrier);
   LOAD(vkGetDeviceProcAddr, dev, vkEndCommandBuffer);
   LOAD(vkGetDeviceProcAddr, dev, vkCreateFence);
   LOAD(vkGetDeviceProcAddr, dev, vkResetFences);
   LOAD(vkGetDeviceProcAddr, dev, vkQueueSubmit);
   LOAD(vkGetDeviceProcAddr, dev, vkWaitForFences);
   VkQueue queue;
   vkGetDeviceQueue(dev, qfam, 0, &queue);

   // Host-visible, coherent storage buffer, zeroed through the mapping (no transfer job).
   VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = N * 4,
                              .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
   VkBuffer buf;
   CHECK(vkCreateBuffer(dev, &bci, NULL, &buf));
   VkMemoryRequirements mr;
   vkGetBufferMemoryRequirements(dev, buf, &mr);
   VkPhysicalDeviceMemoryProperties mp;
   vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
   uint32_t mt = UINT32_MAX;
   for (uint32_t i = 0; i < mp.memoryTypeCount && mt == UINT32_MAX; i++)
      if ((mr.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
         mt = i;
   if (mt == UINT32_MAX) {
      fprintf(stderr, "vkcompute: no host-visible coherent memory type\n");
      return 1;
   }
   VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr.size,
                                .memoryTypeIndex = mt };
   VkDeviceMemory mem;
   CHECK(vkAllocateMemory(dev, &mai, NULL, &mem));
   CHECK(vkBindBufferMemory(dev, buf, mem, 0));
   volatile uint32_t *v;
   CHECK(vkMapMemory(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&v));

   VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = sizeof(vkcompute_spv), .pCode = vkcompute_spv };
   VkShaderModule sm;
   CHECK(vkCreateShaderModule(dev, &smi, NULL, &sm));
   VkDescriptorSetLayoutBinding b = { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                      .descriptorCount = 1, .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
   VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = 1, .pBindings = &b };
   VkDescriptorSetLayout dsl;
   CHECK(vkCreateDescriptorSetLayout(dev, &dli, NULL, &dsl));
   VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                      .setLayoutCount = 1, .pSetLayouts = &dsl };
   VkPipelineLayout pl;
   CHECK(vkCreatePipelineLayout(dev, &pli, NULL, &pl));
   VkComputePipelineCreateInfo cpi = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                 .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = sm, .pName = "main" },
      .layout = pl,
   };
   VkPipeline pipe;
   CHECK(vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &pipe));

   VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
   VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 1,
                                      .poolSizeCount = 1, .pPoolSizes = &ps };
   VkDescriptorPool dp;
   CHECK(vkCreateDescriptorPool(dev, &dpi, NULL, &dp));
   VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                       .descriptorPool = dp, .descriptorSetCount = 1, .pSetLayouts = &dsl };
   VkDescriptorSet ds;
   CHECK(vkAllocateDescriptorSets(dev, &dai, &ds));
   VkDescriptorBufferInfo dbi = { buf, 0, VK_WHOLE_SIZE };
   VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0,
                              .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                              .pBufferInfo = &dbi };
   vkUpdateDescriptorSets(dev, 1, &w, 0, NULL);

   VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfam };
   VkCommandPool pool;
   CHECK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
   VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                       .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                       .commandBufferCount = 1 };
   VkCommandBuffer cb;
   CHECK(vkAllocateCommandBuffers(dev, &cai, &cb));
   VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   CHECK(vkBeginCommandBuffer(cb, &cbi));
   vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
   vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
   vkCmdDispatch(cb, 1, 1, 1);
   VkMemoryBarrier mb = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_HOST_READ_BIT };
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL,
                        0, NULL);
   CHECK(vkEndCommandBuffer(cb));

   VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   VkFence fence;
   CHECK(vkCreateFence(dev, &fci, NULL, &fence));
   VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };

   uint32_t check = 0;
   for (int it = 0; it < iterations; it++) {
      for (int i = 0; i < N; i++)
         v[i] = 0;
      CHECK(vkResetFences(dev, 1, &fence));
      CHECK(vkQueueSubmit(queue, 1, &si, fence));
      // 2 s: far beyond a 64-invocation job; a timeout means the GPU did not complete it.
      CHECK(vkWaitForFences(dev, 1, &fence, VK_TRUE, 2000000000ull));
      check = 0;
      for (int i = 0; i < N; i++)
         check += v[i];
      printf("vkcompute iteration %d: checksum %u (expected 6112), v[0..3] = %u %u %u %u\n", it, check, v[0], v[1],
             v[2], v[3]);
   }
   return check == 6112 ? 0 : 3;
}
