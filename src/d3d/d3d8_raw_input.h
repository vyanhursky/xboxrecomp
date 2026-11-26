/* Native IA formats for the raw vertex shader adapter. Integer formats keep
 * NV2A normalization/sign extraction in our shader rather than SNORM clamps. */
#ifndef XBOXRECOMP_D3D8_RAW_INPUT_H
#define XBOXRECOMP_D3D8_RAW_INPUT_H
#include "../kernel/nv2a_raw_draw.h"
#ifdef _WIN32
#include <dxgiformat.h>
static NV2A_RAW_INLINE DXGI_FORMAT d3d8_raw_format(uint32_t type, uint32_t size)
{
    static const DXGI_FORMAT f[] = { DXGI_FORMAT_UNKNOWN, DXGI_FORMAT_R32_FLOAT,
        DXGI_FORMAT_R32G32_FLOAT, DXGI_FORMAT_R32G32B32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT };
    if (!size || size > 4) return DXGI_FORMAT_UNKNOWN;
    if (type == 2) return f[size];
    if (type == 0 || type == 4) return DXGI_FORMAT_R8G8B8A8_UINT;
    if (type == 6) return DXGI_FORMAT_R32_UINT;
    if (type == 1 || type == 5)
        return size == 1 ? DXGI_FORMAT_R16_SINT : size == 2 ? DXGI_FORMAT_R16G16_SINT : DXGI_FORMAT_R16G16B16A16_SINT;
    return DXGI_FORMAT_UNKNOWN;
}
#else
#include <vulkan/vulkan.h>
static NV2A_RAW_INLINE VkFormat d3d8_raw_format(uint32_t type, uint32_t size)
{
    static const VkFormat f[] = { VK_FORMAT_UNDEFINED, VK_FORMAT_R32_SFLOAT,
        VK_FORMAT_R32G32_SFLOAT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT };
    if (!size || size > 4) return VK_FORMAT_UNDEFINED;
    if (type == 2) return f[size];
    if (type == 0 || type == 4) return VK_FORMAT_R8G8B8A8_UINT;
    if (type == 6) return VK_FORMAT_R32_UINT;
    if (type == 1 || type == 5)
        return size == 1 ? VK_FORMAT_R16_SINT : size == 2 ? VK_FORMAT_R16G16_SINT : VK_FORMAT_R16G16B16A16_SINT;
    return VK_FORMAT_UNDEFINED;
}
#endif
#endif
