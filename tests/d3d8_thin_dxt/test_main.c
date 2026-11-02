/* Synthetic BC blocks only: preserve guest dimensions, mip levels, lock data,
 * alpha and surface lifetime while D3D11 stores unaligned base levels in BGRA. */
#include "d3d8_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ID3D11Device *device;
static ID3D11DeviceContext *context;
ID3D11Device *d3d8_GetD3D11Device(void) { return device; }
ID3D11DeviceContext *d3d8_GetD3D11Context(void) { return context; }
IDirect3DDevice8 *xbox_GetD3DDevice(void) { return NULL; }
const DWORD *d3d8_GetPalette(DWORD stage) { (void)stage; return NULL; }
#define CHECK(c) do { HRESULT h_ = (c); if (FAILED(h_)) { fprintf(stderr, "%s: %08lx at %d\n", #c, h_, __LINE__); exit(1); } } while (0)
#define REQUIRE(c) do { if (!(c)) { fprintf(stderr, "%s at %d\n", #c, __LINE__); exit(1); } } while (0)

static unsigned alpha(D3DFORMAT fmt) { return fmt == D3DFMT_DXT1 ? 255 : fmt == D3DFMT_DXT3 ? 136 : 127; }
static void fill_blocks(void *ptr, UINT w, UINT h, D3DFORMAT fmt, UINT level)
{
    UINT blocks = ((w + 3) / 4) * ((h + 3) / 4), i;
    UINT bytes = fmt == D3DFMT_DXT1 ? 8 : 16;
    BYTE *data = (BYTE *)ptr;
    for (i = 0; i < blocks; i++) {
        BYTE *b = data + i * bytes;
        unsigned color = ((i + level) & 1) ? 0x001f : 0xf800;
        memset(b, 0, bytes);
        if (fmt == D3DFMT_DXT3) memset(b, 0x88, 8);
        if (fmt == D3DFMT_DXT5) b[0] = 127;
        if (bytes == 16) b += 8;
        b[0] = (BYTE)color; b[1] = (BYTE)(color >> 8);
    }
}

static void check_pixels(D3D8Texture *texture, UINT level, UINT w, UINT h, D3DFORMAT fmt, UINT pattern, int red_block)
{
    ID3D11Texture2D *staging;
    D3D11_TEXTURE2D_DESC td = {0};
    D3D11_MAPPED_SUBRESOURCE m;
    UINT x, y;
    td.Width = w; td.Height = h; td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    CHECK(ID3D11Device_CreateTexture2D(device, &td, NULL, &staging));
    ID3D11DeviceContext_CopySubresourceRegion(context, (ID3D11Resource *)staging, 0, 0, 0, 0,
                                               (ID3D11Resource *)texture->d3d11_texture, level, NULL);
    CHECK(ID3D11DeviceContext_Map(context, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &m));
    for (y = 0; y < h; y++) for (x = 0; x < w; x++) {
        UINT block = (y / 4) * ((w + 3) / 4) + x / 4;
        unsigned expected = (alpha(fmt) << 24) |
            (((int)block != red_block && ((block + pattern) & 1)) ? 0xff : 0xff0000);
        unsigned actual = *(unsigned *)((BYTE *)m.pData + y * m.RowPitch + x * 4);
        REQUIRE(actual == expected);
    }
    ID3D11DeviceContext_Unmap(context, (ID3D11Resource *)staging, 0);
    ID3D11Texture2D_Release(staging);
}

static void exercise(UINT w, UINT h, D3DFORMAT fmt)
{
    IDirect3DTexture8 *texture = NULL;
    IDirect3DSurface8 *surface = NULL;
    D3D8Texture *impl;
    D3D11_TEXTURE2D_DESC td;
    D3DSURFACE_DESC guest;
    D3DLOCKED_RECT lock;
    UINT level, levels, lw = w, lh = h;
    BOOL fallback = (w & 3) || (h & 3);
    CHECK(d3d8_CreateTextureImpl(w, h, 0, 0, fmt, &texture));
    impl = (D3D8Texture *)texture;
    ID3D11Texture2D_GetDesc(impl->d3d11_texture, &td);
    REQUIRE(td.Width == w && td.Height == h);
    REQUIRE(td.Format == (fallback ? DXGI_FORMAT_B8G8R8A8_UNORM : d3d8_to_dxgi_format(fmt)));
    levels = texture->lpVtbl->GetLevelCount(texture);
    REQUIRE(levels == td.MipLevels);
    for (level = 0; level < levels; level++) {
        CHECK(texture->lpVtbl->GetLevelDesc(texture, level, &guest));
        REQUIRE(guest.Width == lw && guest.Height == lh && guest.Format == fmt);
        CHECK(texture->lpVtbl->LockRect(texture, level, &lock, NULL, 0));
        REQUIRE(lock.Pitch == (INT)d3d8_row_pitch(fmt, lw));
        fill_blocks(lock.pBits, lw, lh, fmt, level);
        CHECK(texture->lpVtbl->UnlockRect(texture, level));
        if (fallback) check_pixels(impl, level, lw, lh, fmt, level, -1);
        lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1;
    }
    if (fallback) {
        CHECK(texture->lpVtbl->GetSurfaceLevel(texture, 0, &surface));
        CHECK(surface->lpVtbl->GetDesc(surface, &guest));
        REQUIRE(guest.Width == w && guest.Height == h && guest.Format == fmt);
        CHECK(surface->lpVtbl->LockRect(surface, &lock, NULL, 0));
        REQUIRE(lock.Pitch == (INT)d3d8_row_pitch(fmt, w));
        fill_blocks(lock.pBits, w, h, fmt, 1);
        CHECK(surface->lpVtbl->UnlockRect(surface));
        check_pixels(impl, 0, w, h, fmt, 1, -1);
        /* Restore the level-zero test pattern, then release the parent first. */
        CHECK(surface->lpVtbl->LockRect(surface, &lock, NULL, 0));
        fill_blocks(lock.pBits, w, h, fmt, 0);
        CHECK(surface->lpVtbl->UnlockRect(surface));
        if (w >= 8) {
            RECT rect = {4, 0, 8, h < 4 ? (LONG)h : 4};
            const BYTE *base = impl->sys_mem;
            CHECK(surface->lpVtbl->LockRect(surface, &lock, &rect, 0));
            REQUIRE(lock.Pitch == (INT)d3d8_row_pitch(fmt, w));
            REQUIRE(lock.pBits == base + (fmt == D3DFMT_DXT1 ? 8 : 16));
            fill_blocks(lock.pBits, 4, rect.bottom, fmt, 0);
            CHECK(surface->lpVtbl->UnlockRect(surface));
            check_pixels(impl, 0, w, h, fmt, 0, 1);
            rect.left = 1;
            REQUIRE(surface->lpVtbl->LockRect(surface, &lock, &rect, 0) == E_INVALIDARG);
        }
        texture->lpVtbl->Release(texture);
        CHECK(surface->lpVtbl->LockRect(surface, &lock, NULL, D3DLOCK_READONLY));
        REQUIRE(lock.Pitch == (INT)d3d8_row_pitch(fmt, w));
        CHECK(surface->lpVtbl->UnlockRect(surface));
        surface->lpVtbl->Release(surface);
    } else texture->lpVtbl->Release(texture);
    printf("DXT format %u: %ux%u, %u mips, %s passed\n", fmt, w, h, levels, fallback ? "BGRA fallback/guest locks" : "native BC control");
}

int main(void)
{
    const D3DFORMAT formats[] = {D3DFMT_DXT1, D3DFMT_DXT3, D3DFMT_DXT5};
    const UINT shapes[][2] = {{256, 2}, {2, 256}, {1, 1}, {2, 2}, {6, 5}, {8, 8}};
    UINT i, j;
    D3D_FEATURE_LEVEL level;
    CHECK(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_WARP, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &device, &level, &context));
    for (i = 0; i < sizeof(formats)/sizeof(formats[0]); i++)
        for (j = 0; j < sizeof(shapes)/sizeof(shapes[0]); j++) exercise(shapes[j][0], shapes[j][1], formats[i]);
    ID3D11DeviceContext_ClearState(context);
    ID3D11DeviceContext_Release(context); ID3D11Device_Release(device);
    return 0;
}
