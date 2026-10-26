#pragma once
#include <stdint.h>
void *front_metal_layer(void);
void front_frame_presented(void);
uint64_t front_game_res(int docked);
#if defined(_WIN32) || defined(__ANDROID__)
/* Vulkan front ends (Windows: runtime/frontend_win.c in the TotK tree; Android: runtime/frontend_android.c, Thor port):
   what the Vulkan backend needs in place of front_metal_layer(). Handles are passed as void * / uint64_t so this header
   needs no Vulkan headers. */
/* Instance extensions the window's surface needs; NULL with *count 0 when there is no window (offscreen rendering). */
const char *const *front_vk_instance_extensions(uint32_t *count);
/* A VkSurfaceKHR (as uint64_t) for the game window on vk_instance (a VkInstance); 0 on failure. */
uint64_t front_vk_surface(void *vk_instance);
#endif
