/* Thor port: the volk and VMA stand-ins declared in volk.h and vk_mem_alloc.h. */
#include "volk.h"
#include "vk_mem_alloc.h"
#include <stdlib.h>
#include <string.h>

PFN_vkCreateDebugUtilsMessengerEXT shim_vkCreateDebugUtilsMessengerEXT;
PFN_vkGetMemoryHostPointerPropertiesEXT shim_vkGetMemoryHostPointerPropertiesEXT;
void volkLoadInstance(VkInstance instance) {
    shim_vkCreateDebugUtilsMessengerEXT = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT");
}
void volkLoadDevice(VkDevice device) {
    shim_vkGetMemoryHostPointerPropertiesEXT = (PFN_vkGetMemoryHostPointerPropertiesEXT)vkGetDeviceProcAddr(device, "vkGetMemoryHostPointerPropertiesEXT");
}

struct VmaAllocator_T { VkDevice dev; VkPhysicalDeviceMemoryProperties mp; VkDeviceSize atom; };
struct VmaAllocation_T { VkDeviceMemory mem; VkDeviceSize size; uint32_t type; void *map; };

VkResult vmaCreateAllocator(const VmaAllocatorCreateInfo *info, VmaAllocator *out) {
    VmaAllocator a = calloc(1, sizeof *a); if (!a) return VK_ERROR_OUT_OF_HOST_MEMORY;
    a->dev = info->device; vkGetPhysicalDeviceMemoryProperties(info->physicalDevice, &a->mp);
    VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(info->physicalDevice, &p); a->atom = p.limits.nonCoherentAtomSize ? p.limits.nonCoherentAtomSize : 1;
    *out = a; return VK_SUCCESS;
}
void vmaDestroyAllocator(VmaAllocator a) { free(a); }

/* VMA's choice, reduced to what the backend asks for: host access wanted -> host visible required (coherent and, for
   random access, cached preferred); AUTO_PREFER_DEVICE or no host access -> device local preferred. The first type that
   meets the required flags with the most preferred flags wins. */
static int pick_type(VmaAllocator a, uint32_t bits, const VmaAllocationCreateInfo *ai) {
    VkMemoryPropertyFlags req = ai->requiredFlags, pref = ai->preferredFlags;
    int host = (ai->flags & (VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT)) != 0;
    if (host) { req |= VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT; pref |= VK_MEMORY_PROPERTY_HOST_COHERENT_BIT; }
    if (ai->flags & VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT) pref |= VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    if (!host || ai->usage == VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE) pref |= VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    if (ai->memoryTypeBits) bits &= ai->memoryTypeBits;
    int best = -1, best_score = -1;
    for (uint32_t i = 0; i < a->mp.memoryTypeCount; i++) {
        if (!(bits & (1u << i))) continue;
        VkMemoryPropertyFlags f = a->mp.memoryTypes[i].propertyFlags;
        if ((f & req) != req) continue;
        if (f & VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT) continue;
        int score = __builtin_popcount(f & pref) * 2 - (!host && (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? 1 : 0);
        if (score > best_score) { best = (int)i; best_score = score; }
    }
    return best;
}
static VkResult alloc_for(VmaAllocator a, VkMemoryRequirements rq, const VmaAllocationCreateInfo *ai, VkBuffer buf, VkImage img, VmaAllocation *out, VmaAllocationInfo *info) {
    int t = pick_type(a, rq.memoryTypeBits, ai); if (t < 0) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkMemoryDedicatedAllocateInfo ded = {VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, NULL, img, buf};
    VkMemoryAllocateInfo mi = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &ded, rq.size, (uint32_t)t};
    VmaAllocation al = calloc(1, sizeof *al); if (!al) return VK_ERROR_OUT_OF_HOST_MEMORY;
    VkResult r = vkAllocateMemory(a->dev, &mi, NULL, &al->mem); if (r != VK_SUCCESS) { free(al); return r; }
    al->size = rq.size; al->type = (uint32_t)t;
    if ((a->mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && (ai->flags & VMA_ALLOCATION_CREATE_MAPPED_BIT)) {
        r = vkMapMemory(a->dev, al->mem, 0, VK_WHOLE_SIZE, 0, &al->map);
        if (r != VK_SUCCESS) { vkFreeMemory(a->dev, al->mem, NULL); free(al); return r; }
    }
    *out = al; if (info) vmaGetAllocationInfo(a, al, info);
    return VK_SUCCESS;
}
VkResult vmaCreateBuffer(VmaAllocator a, const VkBufferCreateInfo *bi, const VmaAllocationCreateInfo *ai, VkBuffer *buf, VmaAllocation *al, VmaAllocationInfo *info) {
    VkResult r = vkCreateBuffer(a->dev, bi, NULL, buf); if (r != VK_SUCCESS) return r;
    VkMemoryRequirements rq; vkGetBufferMemoryRequirements(a->dev, *buf, &rq);
    if ((r = alloc_for(a, rq, ai, *buf, VK_NULL_HANDLE, al, info)) != VK_SUCCESS) { vkDestroyBuffer(a->dev, *buf, NULL); *buf = VK_NULL_HANDLE; return r; }
    if ((r = vkBindBufferMemory(a->dev, *buf, (*al)->mem, 0)) != VK_SUCCESS) { vmaDestroyBuffer(a, *buf, *al); *buf = VK_NULL_HANDLE; *al = NULL; }
    return r;
}
VkResult vmaCreateImage(VmaAllocator a, const VkImageCreateInfo *ii, const VmaAllocationCreateInfo *ai, VkImage *img, VmaAllocation *al, VmaAllocationInfo *info) {
    VkResult r = vkCreateImage(a->dev, ii, NULL, img); if (r != VK_SUCCESS) return r;
    VkMemoryRequirements rq; vkGetImageMemoryRequirements(a->dev, *img, &rq);
    if ((r = alloc_for(a, rq, ai, VK_NULL_HANDLE, *img, al, info)) != VK_SUCCESS) { vkDestroyImage(a->dev, *img, NULL); *img = VK_NULL_HANDLE; return r; }
    if ((r = vkBindImageMemory(a->dev, *img, (*al)->mem, 0)) != VK_SUCCESS) { vmaDestroyImage(a, *img, *al); *img = VK_NULL_HANDLE; *al = NULL; }
    return r;
}
static void free_alloc(VmaAllocator a, VmaAllocation al) {
    if (!al) return;
    if (al->map) vkUnmapMemory(a->dev, al->mem);
    vkFreeMemory(a->dev, al->mem, NULL); free(al);
}
void vmaDestroyBuffer(VmaAllocator a, VkBuffer buf, VmaAllocation al) { if (buf) vkDestroyBuffer(a->dev, buf, NULL); free_alloc(a, al); }
void vmaDestroyImage(VmaAllocator a, VkImage img, VmaAllocation al) { if (img) vkDestroyImage(a->dev, img, NULL); free_alloc(a, al); }
static VkResult flush_or_invalidate(VmaAllocator a, VmaAllocation al, VkDeviceSize off, VkDeviceSize n, int flush) {
    if (!al || (a->mp.memoryTypes[al->type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) return VK_SUCCESS;
    VkDeviceSize lo = off / a->atom * a->atom, hi = n == VK_WHOLE_SIZE ? al->size : (off + n + a->atom - 1) / a->atom * a->atom;
    if (hi > al->size) hi = al->size;
    VkMappedMemoryRange r = {VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, al->mem, lo, n == VK_WHOLE_SIZE ? VK_WHOLE_SIZE : hi - lo};
    return flush ? vkFlushMappedMemoryRanges(a->dev, 1, &r) : vkInvalidateMappedMemoryRanges(a->dev, 1, &r);
}
VkResult vmaFlushAllocation(VmaAllocator a, VmaAllocation al, VkDeviceSize off, VkDeviceSize n) { return flush_or_invalidate(a, al, off, n, 1); }
VkResult vmaInvalidateAllocation(VmaAllocator a, VmaAllocation al, VkDeviceSize off, VkDeviceSize n) { return flush_or_invalidate(a, al, off, n, 0); }
void vmaGetAllocationInfo(VmaAllocator a, VmaAllocation al, VmaAllocationInfo *info) {
    (void)a; memset(info, 0, sizeof *info);
    info->memoryType = al->type; info->deviceMemory = al->mem; info->offset = 0; info->size = al->size; info->pMappedData = al->map;
}
