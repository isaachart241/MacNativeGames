// NVNformat (nvn.h 0x01-0x9C) -> VkFormat, the Vulkan counterpart of nvn_fmt_mtl.c (same table order and the same
// substitutions where Vulkan desktop drivers lack a format). Pure C: VkFormat values come from vulkan_core.h.
#pragma once
#include <stdint.h>
#include "nvn_fmt_mtl.h"   // NVN_SWZ_* (= NVNtextureSwizzle values)

enum {
    NVN_VKCONV_NONE = 0,
    NVN_VKCONV_RGB_EXPAND,   // 3-component storage expanded to 4 components on upload (a = 1 / integer 1)
    NVN_VKCONV_BC,           // BC1-7: native if the device has textureCompressionBC, else CPU-decoded (nvn_bcdec.c)
    NVN_VKCONV_ASTC,         // ASTC LDR: native if textureCompressionASTC_LDR, else CPU-decoded to RGBA8 (nvn_astc_dec.c)
    NVN_VKCONV_D24,          // 24-bit depth data uploaded into a 32-bit float depth format
};
typedef struct {
    uint32_t vk;             // VkFormat (0 = no counterpart)
    uint8_t swz[4];          // NVN_SWZ_* per output component (format substitution)
    uint8_t conv;            // NVN_VKCONV_*
    uint8_t kind;            // 1 exact, 2 substitute (as nvn_fmt_mtl.c)
    uint32_t vk_decoded;     // format after CPU decoding (BC / ASTC fallback, RGB expansion)
} nvn_vkfmt;
int nvn_fmt_vk(int nvn_format, nvn_vkfmt *out);          // returns kind (0 = unsupported)
uint32_t nvn_vertex_fmt_vk(int nvn_format);              // vertex attribute format (VkFormat), 0 = unsupported
int nvn_fmt_is_depth(int f);                             // depth and / or stencil format
int nvn_fmt_has_stencil(int f);
int nvn_fmt_is_astc(int f, int *bw, int *bh, int *srgb);
