// SPDX-License-Identifier: MIT
//
// vkshim: a stand-in for the Vulkan loader, for test programs that dlopen a libvulkan and look up
// vkGetInstanceProcAddr (dEQP-VK with --deqp-vk-library-path, docs/ddk113/TESTING.md, L7). It loads the
// ICD named by VKSHIM_ICD (default: libvulkan_powervr.so next to this library on the search path),
// negotiates interface version 5 and forwards vkGetInstanceProcAddr to vk_icdGetInstanceProcAddr.
// No layers, no other ICDs.
//
// Build (armv7): $NDK/.../armv7a-linux-androideabi29-clang -O2 -Wall -Wextra -Werror -shared -fPIC \
//   tools/vktest/vkshim.c -ldl -o libvkshim.so

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

static PFN_vkGetInstanceProcAddr icd_gipa;
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void load(void)
{
   const char *path = getenv("VKSHIM_ICD");
   void *lib = dlopen(path ? path : "libvulkan_powervr.so", RTLD_NOW | RTLD_LOCAL);
   if (!lib) {
      fprintf(stderr, "vkshim: dlopen: %s\n", dlerror());
      return;
   }
   VkResult (*negotiate)(uint32_t *) =
      (VkResult(*)(uint32_t *))dlsym(lib, "vk_icdNegotiateLoaderICDInterfaceVersion");
   PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vk_icdGetInstanceProcAddr");
   uint32_t version = 5;
   if (!negotiate || !gipa || negotiate(&version) != VK_SUCCESS) {
      fprintf(stderr, "vkshim: not a Vulkan ICD\n");
      return;
   }
   icd_gipa = gipa;
}

__attribute__((visibility("default"))) PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance,
                                                                                const char *name)
{
   pthread_once(&once, load);
   return icd_gipa ? icd_gipa(instance, name) : NULL;
}
