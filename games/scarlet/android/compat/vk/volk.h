/* Thor port: the volk entry points the Vulkan NVN backend calls, without volk. Android's libvulkan exports every core
   function up to Vulkan 1.3, so the backend links against it directly; the two extension functions it calls are loaded
   by volkLoadInstance / volkLoadDevice (android/compat/vk/vk_shims.c) into pointers of the same name. */
#pragma once
#include <vulkan/vulkan.h>

static inline VkResult volkInitialize(void) { return VK_SUCCESS; }
static inline uint32_t volkGetInstanceVersion(void) { uint32_t v = VK_API_VERSION_1_0; vkEnumerateInstanceVersion(&v); return v; }
void volkLoadInstance(VkInstance instance);
void volkLoadDevice(VkDevice device);

extern PFN_vkCreateDebugUtilsMessengerEXT shim_vkCreateDebugUtilsMessengerEXT;
extern PFN_vkGetMemoryHostPointerPropertiesEXT shim_vkGetMemoryHostPointerPropertiesEXT;
#define vkCreateDebugUtilsMessengerEXT shim_vkCreateDebugUtilsMessengerEXT
#define vkGetMemoryHostPointerPropertiesEXT shim_vkGetMemoryHostPointerPropertiesEXT
