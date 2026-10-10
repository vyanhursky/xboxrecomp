/*
 * d3d8_vk.h -- what the Vulkan backend needs from the program that hosts it.
 *
 * The backend creates the Vulkan instance and device and draws; it does not
 * create a window. A host that has one says which instance extensions its
 * window system needs and how to make a surface for it, before the device is
 * created. With no host set (or a host that returns no surface) the backend
 * renders off screen: every frame is drawn and can be captured, and nothing
 * is presented. That is what an unattended test run on a machine without a
 * display uses.
 */
#ifndef XBOXRECOMP_D3D8_VK_H
#define XBOXRECOMP_D3D8_VK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct D3D8VkHost {
    /* Instance extensions the window system needs (VK_KHR_surface and its
     * platform one), as SDL_Vulkan_GetInstanceExtensions returns them. */
    const char *const *instance_extensions;
    uint32_t           instance_extension_count;
    /* Make a VkSurfaceKHR for the window: instance is a VkInstance, surface
     * points at a VkSurfaceKHR. Returns non-zero on success. Called once,
     * from the thread that creates the device. */
    int  (*create_surface)(void *instance, void *surface, void *user);
    /* The window's size in pixels, asked whenever the swap chain is built. */
    void (*drawable_size)(int *width, int *height, void *user);
    void *user;
    /* The refresh rate of the display the window is on, in whole Hz (0 when
     * unknown), asked whenever the swap chain is built. Optional. */
    unsigned (*refresh_hz)(void *user);
} D3D8VkHost;

void d3d8_vk_set_host(const D3D8VkHost *host);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_D3D8_VK_H */
