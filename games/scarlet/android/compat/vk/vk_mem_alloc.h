/* Thor port: the part of VulkanMemoryAllocator's interface the Vulkan NVN backend uses (create / destroy buffers and
   images, flush), implemented in android/compat/vk/vk_shims.c over plain vkAllocateMemory: one dedicated allocation per
   resource, persistently mapped when the memory is host visible. The memory type follows VMA's rules for the usage and
   host-access flags given. Names and values match VMA 3.1 for the members used, so the backend source is unchanged. */
#pragma once
#include <vulkan/vulkan.h>

typedef struct VmaAllocator_T *VmaAllocator;
typedef struct VmaAllocation_T *VmaAllocation;

typedef enum VmaMemoryUsage {
    VMA_MEMORY_USAGE_UNKNOWN = 0,
    VMA_MEMORY_USAGE_AUTO = 7,
    VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE = 8,
    VMA_MEMORY_USAGE_AUTO_PREFER_HOST = 9,
} VmaMemoryUsage;
typedef enum VmaAllocationCreateFlagBits {
    VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT = 0x1,
    VMA_ALLOCATION_CREATE_MAPPED_BIT = 0x4,
    VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT = 0x400,
    VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT = 0x800,
} VmaAllocationCreateFlagBits;
typedef VkFlags VmaAllocationCreateFlags;

typedef struct VmaAllocationCreateInfo {
    VmaAllocationCreateFlags flags;
    VmaMemoryUsage usage;
    VkMemoryPropertyFlags requiredFlags, preferredFlags;
    uint32_t memoryTypeBits;
    void *pool, *pUserData;
    float priority;
} VmaAllocationCreateInfo;
typedef struct VmaAllocationInfo {
    uint32_t memoryType;
    VkDeviceMemory deviceMemory;
    VkDeviceSize offset, size;
    void *pMappedData, *pUserData;
    const char *pName;
} VmaAllocationInfo;
typedef struct VmaVulkanFunctions {
    PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr vkGetDeviceProcAddr;
} VmaVulkanFunctions;
typedef struct VmaAllocatorCreateInfo {
    VkFlags flags;
    VkPhysicalDevice physicalDevice;
    VkDevice device;
    VkDeviceSize preferredLargeHeapBlockSize;
    const VkAllocationCallbacks *pAllocationCallbacks;
    const void *pDeviceMemoryCallbacks;
    const VkDeviceSize *pHeapSizeLimit;
    const VmaVulkanFunctions *pVulkanFunctions;
    VkInstance instance;
    uint32_t vulkanApiVersion;
    const void *pTypeExternalMemoryHandleTypes;
} VmaAllocatorCreateInfo;

VkResult vmaCreateAllocator(const VmaAllocatorCreateInfo *info, VmaAllocator *out);
void vmaDestroyAllocator(VmaAllocator a);
VkResult vmaCreateBuffer(VmaAllocator a, const VkBufferCreateInfo *bi, const VmaAllocationCreateInfo *ai, VkBuffer *buf, VmaAllocation *al, VmaAllocationInfo *info);
void vmaDestroyBuffer(VmaAllocator a, VkBuffer buf, VmaAllocation al);
VkResult vmaCreateImage(VmaAllocator a, const VkImageCreateInfo *ii, const VmaAllocationCreateInfo *ai, VkImage *img, VmaAllocation *al, VmaAllocationInfo *info);
void vmaDestroyImage(VmaAllocator a, VkImage img, VmaAllocation al);
VkResult vmaFlushAllocation(VmaAllocator a, VmaAllocation al, VkDeviceSize offset, VkDeviceSize size);
VkResult vmaInvalidateAllocation(VmaAllocator a, VmaAllocation al, VkDeviceSize offset, VkDeviceSize size);
void vmaGetAllocationInfo(VmaAllocator a, VmaAllocation al, VmaAllocationInfo *info);
