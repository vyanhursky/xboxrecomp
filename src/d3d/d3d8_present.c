/* Scaled presentation. See d3d8_present.h. */

#include "d3d8_internal.h"
#include "d3d8_present.h"
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static BOOL g_scaling;
static UINT g_scale_override;
static UINT g_canvas_num, g_canvas_den;

/* Written from any thread, read by the presenting thread. */
static volatile LONG g_keep_aspect = 1;
static volatile LONG g_aspect_num = 4, g_aspect_den = 3;
static volatile LONG g_vsync = -1;
static volatile LONG g_linear = 1;          /* d3d8_present_set_filter_mode: 0, 1 or 2 */
static UINT g_game_width, g_game_height;

static ID3D11Texture2D *g_game_texture;
static ID3D11ShaderResourceView *g_game_srv;
static ID3D11RenderTargetView *g_window_rtv;
static UINT g_window_width, g_window_height;
static ID3D11DeviceContext *g_deferred;
static ID3D11VertexShader *g_vs;
static ID3D11PixelShader *g_ps;
static ID3D11SamplerState *g_sampler_linear, *g_sampler_nearest;

/* MSAA ([display] msaa, d3d8_set_msaa): the title draws into g_game_ms, and
 * d3d8_present_resolve averages it into g_game_texture, which the gamma pass,
 * the blit to the window and every capture read as before. */
static UINT g_msaa_want = 1, g_msaa = 1;
static ID3D11Texture2D *g_game_ms;
static ID3D11RenderTargetView *g_game_rtv;     /* g_game_texture's own view, with MSAA */
void d3d8_set_msaa(unsigned samples) { g_msaa_want = samples ? samples : 1; }
UINT d3d8_present_msaa(void) { return g_msaa; }

ID3D11RenderTargetView *d3d8_present_resolve(ID3D11RenderTargetView *drawn)
{
    if (!g_game_ms || !g_game_texture)
        return drawn;
    ID3D11DeviceContext_ResolveSubresource(d3d8_GetD3D11Context(), (ID3D11Resource *)g_game_texture, 0,
                                           (ID3D11Resource *)g_game_ms, 0, DXGI_FORMAT_R8G8B8A8_UNORM);
    return g_game_rtv;
}

/* Widescreen side fill: the game target has a mip chain when it is a wide canvas;
 * on a frame that asked (d3d8_RequestSideFill), the areas beside the 4:3 box get
 * a blurred, darkened stretch of the picture instead of black. */
static ID3D11Buffer *g_fill_cb;
static BOOL g_game_mips;
static volatile LONG g_fill_request;
void d3d8_RequestSideFill(void) { InterlockedExchange(&g_fill_request, 1); }

void d3d8_present_enable_scaling(int enable) { g_scaling = enable ? TRUE : FALSE; }
void d3d8_present_set_render_scale(unsigned scale) { g_scale_override = scale > 4 ? 4 : scale; }
void d3d8_present_set_canvas_aspect(unsigned num, unsigned den) { g_canvas_num = num; g_canvas_den = den; }

void d3d8_present_set_aspect(int keep, unsigned num, unsigned den)
{
    if (num && den) {
        InterlockedExchange(&g_aspect_num, (LONG)num);
        InterlockedExchange(&g_aspect_den, (LONG)den);
    }
    InterlockedExchange(&g_keep_aspect, keep ? 1 : 0);
}

void d3d8_present_set_vsync(int vsync)
{
    InterlockedExchange(&g_vsync, vsync < 0 ? -1 : vsync ? 1 : 0);
}

void d3d8_present_set_linear_filter(int linear)
{
    InterlockedExchange(&g_linear, linear ? 1 : 0);
}

void d3d8_present_set_filter_mode(int mode)
{
    InterlockedExchange(&g_linear, mode < 0 ? 0 : mode > 2 ? 2 : mode);
}

BOOL d3d8_present_scaling(void) { return g_scaling; }
UINT d3d8_present_render_scale(void) { return g_scale_override; }
void d3d8_present_canvas_aspect(UINT *num, UINT *den)
{
    *num = g_scaling ? g_canvas_num : 0;
    *den = g_scaling ? g_canvas_den : 0;
}

/* The display the window is on, re-read about once a second: the window can
 * be dragged to another monitor and the mode can change. */
static unsigned present_refresh_hz(void)
{
    static unsigned hz, countdown;
    IDXGIOutput *output = NULL;
    if (countdown) { countdown--; return hz; }
    countdown = 60;
    if (SUCCEEDED(IDXGISwapChain_GetContainingOutput(d3d8_GetSwapChain(), &output)) && output) {
        DXGI_OUTPUT_DESC desc;
        DEVMODEW mode;
        memset(&mode, 0, sizeof(mode));
        mode.dmSize = sizeof(mode);
        if (SUCCEEDED(IDXGIOutput_GetDesc(output, &desc)) &&
            EnumDisplaySettingsW(desc.DeviceName, ENUM_CURRENT_SETTINGS, &mode))
            hz = mode.dmDisplayFrequency;
        IDXGIOutput_Release(output);
    }
    return hz;
}

int d3d8_present_sync_interval(void)
{
    LONG vsync = g_vsync;
    if (vsync <= 0) return (int)vsync;
    return (int)d3d8_present_interval_for(present_refresh_hz());
}

int d3d8_present_display_paced(void)
{
    return g_scaling && g_window_rtv && d3d8_present_sync_interval() > 0;
}

/* RECOMP_PRESENT_PACING=1, after each present: how long each frame actually
 * stayed on screen, from DXGI's frame statistics (the vertical blank at which
 * each present was first shown). A frame that stays up the same time as the
 * others is even motion; 2 then 3 refreshes of a 144 Hz display is judder. With
 * variable refresh working, a 60 fps title's frames each stay up about 16.7 ms. */
static double g_block_ms[512];
static unsigned g_nblock;
void d3d8_present_note_block(double ms)
{
    if (g_nblock < 512)
        g_block_ms[g_nblock++] = ms;
}

void d3d8_present_trace_display(void *swap_chain)
{
    static int on = -1;
    static LARGE_INTEGER freq, window_start;
    static UINT last_present, last_refresh;
    static LONGLONG last_qpc;
    static double ms[512], sorted[512];
    static unsigned refreshes[5], n, misses;
    DXGI_FRAME_STATISTICS st;
    LARGE_INTEGER now;
    if (on < 0) {
        const char *e = getenv("RECOMP_PRESENT_PACING");
        on = e && *e == '1';
        QueryPerformanceFrequency(&freq);
    }
    if (!on || !swap_chain) return;
    QueryPerformanceCounter(&now);
    if (FAILED(IDXGISwapChain_GetFrameStatistics((IDXGISwapChain *)swap_chain, &st))) {
        misses++;
    } else if (st.PresentCount != last_present) {
        if (last_qpc && st.PresentCount == last_present + 1 && n < 512) {
            UINT r = st.SyncRefreshCount - last_refresh;
            refreshes[r >= 4 ? 4 : r]++;
            ms[n++] = (double)(st.SyncQPCTime.QuadPart - last_qpc) * 1000.0 / (double)freq.QuadPart;
        }
        last_present = st.PresentCount;
        last_refresh = st.SyncRefreshCount;
        last_qpc = st.SyncQPCTime.QuadPart;
    }
    if (!window_start.QuadPart) window_start = now;
    if (now.QuadPart - window_start.QuadPart >= 2 * freq.QuadPart) {
        if (g_nblock) {
            unsigned i, j, over = 0;
            for (i = 0; i < g_nblock; i++) {
                for (j = i; j > 0 && sorted[j - 1] > g_block_ms[i]; j--) sorted[j] = sorted[j - 1];
                sorted[j] = g_block_ms[i];
                if (g_block_ms[i] > 2.0) over++;
            }
            fprintf(stderr, "[ONSCREEN] Present call %.2f / %.2f / %.2f / %.2f ms (min/median/90%%/max), %u of %u over 2 ms\n",
                    sorted[0], sorted[g_nblock / 2], sorted[g_nblock * 9 / 10], sorted[g_nblock - 1], over, g_nblock);
            g_nblock = 0;
        }
        if (n) {
            unsigned i, j, off = 0;
            double median;
            for (i = 0; i < n; i++) {
                for (j = i; j > 0 && sorted[j - 1] > ms[i]; j--) sorted[j] = sorted[j - 1];
                sorted[j] = ms[i];
            }
            median = sorted[n / 2];
            for (i = 0; i < n; i++) if (ms[i] < median - 2.0 || ms[i] > median + 2.0) off++;
            fprintf(stderr, "[ONSCREEN] %u frames: refreshes each 1:%u 2:%u 3:%u 4+:%u; on screen "
                            "%.2f / %.2f / %.2f ms (min/median/max), %u more than 2 ms off the median\n",
                    n, refreshes[1], refreshes[2], refreshes[3], refreshes[4],
                    sorted[0], median, sorted[n - 1], off);
        } else {
            fprintf(stderr, "[ONSCREEN] no frame statistics (%u calls failed)\n", misses);
        }
        memset(refreshes, 0, sizeof refreshes);
        n = misses = 0;
        window_start = now;
    }
}

/* RECOMP_PRESENT_PACING=1: every two seconds, how evenly frames reached the
 * display -- the count, the shortest, mean and longest interval, and how many
 * were more than half as long again as the median. */
void d3d8_present_trace_pacing(void)
{
    static int on = -1;
    static LARGE_INTEGER freq, last, window_start;
    static double ms[512], sorted[512];
    static unsigned n;
    LARGE_INTEGER now;
    if (on < 0) {
        const char *e = getenv("RECOMP_PRESENT_PACING");
        on = e && *e == '1';
        QueryPerformanceFrequency(&freq);
    }
    if (!on) return;
    QueryPerformanceCounter(&now);
    if (last.QuadPart && n < 512)
        ms[n++] = (double)(now.QuadPart - last.QuadPart) * 1000.0 / (double)freq.QuadPart;
    last = now;
    if (!window_start.QuadPart) window_start = now;
    if (now.QuadPart - window_start.QuadPart >= 2 * freq.QuadPart && n) {
        double lo = ms[0], hi = ms[0], sum = 0, median;
        unsigned i, j, late = 0;
        for (i = 0; i < n; i++) {
            if (ms[i] < lo) lo = ms[i];
            if (ms[i] > hi) hi = ms[i];
            sum += ms[i];
            for (j = i; j > 0 && sorted[j - 1] > ms[i]; j--) sorted[j] = sorted[j - 1];
            sorted[j] = ms[i];
        }
        median = sorted[n / 2];
        for (i = 0; i < n; i++) if (ms[i] > median * 1.5) late++;
        fprintf(stderr, "[PACING] %u presents, interval %.2f / %.2f / %.2f ms (min/mean/max), "
                        "median %.2f, %u late, sync interval %d\n",
                n, lo, sum / n, hi, median, late, d3d8_present_sync_interval());
        n = 0;
        window_start = now;
    }
}
ID3D11Texture2D *d3d8_present_game_texture(void) { return g_game_texture; }

void d3d8_present_shutdown(void)
{
    if (g_sampler_nearest) { ID3D11SamplerState_Release(g_sampler_nearest); g_sampler_nearest = NULL; }
    if (g_sampler_linear) { ID3D11SamplerState_Release(g_sampler_linear); g_sampler_linear = NULL; }
    if (g_ps) { ID3D11PixelShader_Release(g_ps); g_ps = NULL; }
    if (g_fill_cb) { ID3D11Buffer_Release(g_fill_cb); g_fill_cb = NULL; }
    if (g_vs) { ID3D11VertexShader_Release(g_vs); g_vs = NULL; }
    if (g_deferred) { ID3D11DeviceContext_Release(g_deferred); g_deferred = NULL; }
    if (g_window_rtv) { ID3D11RenderTargetView_Release(g_window_rtv); g_window_rtv = NULL; }
    if (g_game_srv) { ID3D11ShaderResourceView_Release(g_game_srv); g_game_srv = NULL; }
    if (g_game_rtv) { ID3D11RenderTargetView_Release(g_game_rtv); g_game_rtv = NULL; }
    if (g_game_ms) { ID3D11Texture2D_Release(g_game_ms); g_game_ms = NULL; }
    if (g_game_texture) { ID3D11Texture2D_Release(g_game_texture); g_game_texture = NULL; }
    g_msaa = 1;
    g_window_width = g_window_height = 0;
}

/* The title's render target: what the swap chain's buffer used to be. */
HRESULT d3d8_present_create_game_target(UINT width, UINT height, ID3D11RenderTargetView **rtv)
{
    ID3D11Device *device = d3d8_GetD3D11Device();
    D3D11_TEXTURE2D_DESC desc;
    HRESULT hr;

    memset(&desc, 0, sizeof(desc));
    desc.Width = width;
    desc.Height = height;
    g_game_width = width;
    g_game_height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    g_game_mips = (UINT64)width * 3 > (UINT64)height * 4 + height / 8;   /* a widescreen canvas */
    if (g_game_mips) {
        desc.MipLevels = 0;                         /* the full chain, for the side fill */
        desc.MiscFlags = D3D11_RESOURCE_MISC_GENERATE_MIPS;
    }

    hr = ID3D11Device_CreateTexture2D(device, &desc, NULL, &g_game_texture);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateShaderResourceView(device,
        (ID3D11Resource *)g_game_texture, NULL, &g_game_srv);
    {
        /* The most samples up to the ones asked for that both the colour and
         * the depth format support. */
        UINT n = g_msaa_want > 8 ? 8 : g_msaa_want, qc = 0, qd = 0;
        g_msaa = 1;
        for (; SUCCEEDED(hr) && n >= 2; n >>= 1) {
            if (SUCCEEDED(ID3D11Device_CheckMultisampleQualityLevels(device, DXGI_FORMAT_R8G8B8A8_UNORM, n, &qc))
                    && SUCCEEDED(ID3D11Device_CheckMultisampleQualityLevels(device, DXGI_FORMAT_D24_UNORM_S8_UINT, n, &qd))
                    && qc && qd) {
                g_msaa = n;
                break;
            }
        }
        if (g_msaa_want > 1)
            fprintf(stderr, "D3D8: MSAA %ux asked, %ux used\n", g_msaa_want, g_msaa);
    }
    if (SUCCEEDED(hr) && g_msaa > 1) {
        D3D11_TEXTURE2D_DESC ms = desc;
        ms.MipLevels = 1;
        ms.MiscFlags = 0;
        ms.BindFlags = D3D11_BIND_RENDER_TARGET;
        ms.SampleDesc.Count = g_msaa;
        ms.SampleDesc.Quality = 0;
        hr = ID3D11Device_CreateTexture2D(device, &ms, NULL, &g_game_ms);
        if (SUCCEEDED(hr)) hr = ID3D11Device_CreateRenderTargetView(device,
            (ID3D11Resource *)g_game_ms, NULL, rtv);
        if (SUCCEEDED(hr)) hr = ID3D11Device_CreateRenderTargetView(device,
            (ID3D11Resource *)g_game_texture, NULL, &g_game_rtv);
    } else if (SUCCEEDED(hr)) {
        hr = ID3D11Device_CreateRenderTargetView(device,
            (ID3D11Resource *)g_game_texture, NULL, rtv);
    }
    if (FAILED(hr)) {
        d3d8_present_shutdown();
        return hr;
    }
    /* Alt+Enter is the host's to handle: DXGI's own handler would switch this
     * swap chain to exclusive full screen behind the host's back. */
    {
        IDXGIFactory *factory = NULL;
        if (SUCCEEDED(IDXGISwapChain_GetParent(d3d8_GetSwapChain(), &IID_IDXGIFactory,
                                               (void **)&factory)) && factory) {
            IDXGIFactory_MakeWindowAssociation(factory, d3d8_GetHWND(), DXGI_MWA_NO_ALT_ENTER);
            IDXGIFactory_Release(factory);
        }
    }
    return S_OK;
}

static HRESULT present_create_pipeline(void)
{
    static const char shader[] =
        "Texture2D pixels : register(t0);\n"
        "SamplerState smp : register(s0);\n"
        "struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
        "V vs(uint id : SV_VertexID) {\n"
        " V o; o.p = float4(id==2?3:-1, id==1?3:-1, 0, 1);\n"
        " o.uv = float2(id==2?2:0, id==1?-1:1); return o; }\n"
        "cbuffer Fill : register(b0) { float4 Box; float4 Look; float4 Area; };\n"
        "float4 ps(V i) : SV_Target {\n"
        " if (Look.x > 0.5 && (i.uv.x < Box.x || i.uv.x > Box.y)) {\n"
        "  float2 s = float2(Box.x + i.uv.x * (Box.y - Box.x), i.uv.y);\n"
        "  float3 c = 0; int k, l;\n"
        "  [unroll] for (k = -2; k <= 2; k++) [unroll] for (l = -2; l <= 2; l++)\n"
        "   c += pixels.SampleLevel(smp, s + float2(k, l) * Box.zw, Look.y).rgb;\n"
        "  c /= 25.0; c = lerp(dot(c, float3(0.299, 0.587, 0.114)).xxx, c, 0.8) * Look.z;\n"
        "  return float4(c, 1); }\n"
        /* Area: smooth (high quality) downscaling, Area.z bilinear taps a side
         * spread Area.xy apart over the window pixel's footprint. */
        " if (Area.z > 1.5) {\n"
        "  float3 c = 0; int k, l, n = (int)Area.z;\n"
        "  [loop] for (k = 0; k < n; k++) [loop] for (l = 0; l < n; l++)\n"
        "   c += pixels.SampleLevel(smp, i.uv + (float2(k, l) + 0.5 - 0.5 * Area.z) * Area.xy, 0).rgb;\n"
        "  return float4(c / (Area.z * Area.z), 1); }\n"
        " return float4(pixels.SampleLevel(smp, i.uv, 0).rgb, 1); }\n";
    ID3D11Device *device = d3d8_GetD3D11Device();
    ID3DBlob *vs = NULL, *ps = NULL;
    D3D11_SAMPLER_DESC sd;
    HRESULT hr;

    if (g_deferred) return S_OK;
    hr = D3DCompile(shader, sizeof(shader) - 1, NULL, NULL, NULL, "vs", "vs_4_0", 0, 0, &vs, NULL);
    if (SUCCEEDED(hr)) hr = D3DCompile(shader, sizeof(shader) - 1, NULL, NULL, NULL, "ps", "ps_4_0", 0, 0, &ps, NULL);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateVertexShader(device,
        ID3D10Blob_GetBufferPointer(vs), ID3D10Blob_GetBufferSize(vs), NULL, &g_vs);
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreatePixelShader(device,
        ID3D10Blob_GetBufferPointer(ps), ID3D10Blob_GetBufferSize(ps), NULL, &g_ps);
    memset(&sd, 0, sizeof(sd));
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateSamplerState(device, &sd, &g_sampler_linear);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateSamplerState(device, &sd, &g_sampler_nearest);
    if (SUCCEEDED(hr)) {
        D3D11_BUFFER_DESC bd;
        memset(&bd, 0, sizeof(bd));
        bd.ByteWidth = 48;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = ID3D11Device_CreateBuffer(device, &bd, NULL, &g_fill_cb);
    }
    if (SUCCEEDED(hr)) hr = ID3D11Device_CreateDeferredContext(device, 0, &g_deferred);
    if (vs) ID3D10Blob_Release(vs);
    if (ps) ID3D10Blob_Release(ps);
    return hr;
}

/* Make the swap chain's buffer the size of the window's client area. Returns
 * S_FALSE while there is nothing to draw into (a minimised window). */
static HRESULT present_follow_window(void)
{
    IDXGISwapChain *swap_chain = d3d8_GetSwapChain();
    ID3D11Texture2D *buffer = NULL;
    RECT client;
    UINT width, height;
    HRESULT hr;

    if (!GetClientRect(d3d8_GetHWND(), &client))
        return S_FALSE;
    width = (UINT)(client.right - client.left);
    height = (UINT)(client.bottom - client.top);
    if (!width || !height)
        return S_FALSE;
    if (g_window_rtv && width == g_window_width && height == g_window_height)
        return S_OK;

    if (g_window_rtv) {
        /* The only reference to the old buffer: the blit's commands restore
         * the context they ran on, so nothing else holds it. */
        ID3D11RenderTargetView_Release(g_window_rtv);
        g_window_rtv = NULL;
        hr = IDXGISwapChain_ResizeBuffers(swap_chain, 0, width, height, DXGI_FORMAT_UNKNOWN, 0);
        if (FAILED(hr)) {
            fprintf(stderr, "D3D8: ResizeBuffers(%ux%u) failed: 0x%08lX\n", width, height, hr);
            return hr;
        }
    }
    hr = IDXGISwapChain_GetBuffer(swap_chain, 0, &IID_ID3D11Texture2D, (void **)&buffer);
    if (FAILED(hr)) return hr;
    {
        D3D11_TEXTURE2D_DESC desc;
        ID3D11Texture2D_GetDesc(buffer, &desc);
        g_window_width = desc.Width;
        g_window_height = desc.Height;
    }
    hr = ID3D11Device_CreateRenderTargetView(d3d8_GetD3D11Device(), (ID3D11Resource *)buffer,
                                             NULL, &g_window_rtv);
    ID3D11Texture2D_Release(buffer);
    return hr;
}

static D3D8PresentOverlay g_overlay;
static void *g_overlay_user;

void d3d8_present_set_overlay(D3D8PresentOverlay overlay, void *user)
{
    g_overlay_user = user;
    g_overlay = overlay;
}

/* Draw the title's target into the window's buffer. */
HRESULT d3d8_present_blit(void)
{
    static const float black[4] = { 0.0f, 0.0f, 0.0f, 1.0f };
    ID3D11CommandList *commands = NULL;
    ID3D11SamplerState *sampler;
    D3D11_VIEWPORT viewport;
    D3D8PresentRect fit;
    HRESULT hr;

    if (!g_scaling || !g_game_srv)
        return S_FALSE;
    hr = present_create_pipeline();
    if (FAILED(hr)) return hr;
    hr = present_follow_window();
    if (hr != S_OK) return hr;

    fit = d3d8_present_fit((unsigned)g_aspect_num, (unsigned)g_aspect_den,
                           g_window_width, g_window_height, (int)g_keep_aspect);
    memset(&viewport, 0, sizeof(viewport));
    viewport.TopLeftX = (float)fit.x;
    viewport.TopLeftY = (float)fit.y;
    viewport.Width = (float)fit.width;
    viewport.Height = (float)fit.height;
    viewport.MaxDepth = 1.0f;
    sampler = g_linear ? g_sampler_linear : g_sampler_nearest;

    ID3D11DeviceContext_ClearRenderTargetView(g_deferred, g_window_rtv, black);
    ID3D11DeviceContext_RSSetViewports(g_deferred, 1, &viewport);
    ID3D11DeviceContext_OMSetRenderTargets(g_deferred, 1, &g_window_rtv, NULL);
    ID3D11DeviceContext_IASetPrimitiveTopology(g_deferred, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(g_deferred, g_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(g_deferred, g_ps, NULL, 0);
    {
        /* Box: where the 4:3 picture sits in the canvas (u), and the blur tap step. */
        D3D11_MAPPED_SUBRESOURCE m;
        float k = 1.0f, fill[12];
        int on = g_game_mips && InterlockedExchange(&g_fill_request, 0);
        if (on) {
            /* The title's aspect over the canvas's: one title pixel across over one down. */
            k = d3d8_GetCanvasScaleY() / d3d8_GetCanvasScaleX();
            ID3D11DeviceContext_GenerateMips(g_deferred, g_game_srv);
        }
        fill[0] = 0.5f - 0.5f * k; fill[1] = 0.5f + 0.5f * k;
        fill[2] = 0.025f; fill[3] = 0.035f;
        fill[4] = (on && k < 0.999f) ? 1.0f : 0.0f; fill[5] = 5.0f; fill[6] = 0.55f; fill[7] = 0.0f;
        /* Area taps: one per texel a window pixel covers, up to four a side, when
         * the picture is larger than its place in the window. */
        fill[8] = fill[9] = fill[10] = fill[11] = 0.0f;
        if (g_linear == 2 && fit.width && fit.height && g_game_width && g_game_height) {
            float rx = (float)g_game_width / (float)fit.width, ry = (float)g_game_height / (float)fit.height;
            float r = rx > ry ? rx : ry;
            float n = r > 1.01f ? (float)(int)(r + 0.999f) : 1.0f;
            if (n > 4.0f) n = 4.0f;
            fill[8] = 1.0f / ((float)fit.width * n);
            fill[9] = 1.0f / ((float)fit.height * n);
            fill[10] = n;
        }
        if (SUCCEEDED(ID3D11DeviceContext_Map(g_deferred, (ID3D11Resource *)g_fill_cb, 0,
                                               D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memcpy(m.pData, fill, sizeof(fill));
            ID3D11DeviceContext_Unmap(g_deferred, (ID3D11Resource *)g_fill_cb, 0);
        }
        ID3D11DeviceContext_PSSetConstantBuffers(g_deferred, 0, 1, &g_fill_cb);
    }
    ID3D11DeviceContext_PSSetShaderResources(g_deferred, 0, 1, &g_game_srv);
    ID3D11DeviceContext_PSSetSamplers(g_deferred, 0, 1, &sampler);
    ID3D11DeviceContext_Draw(g_deferred, 3, 0);
    hr = ID3D11DeviceContext_FinishCommandList(g_deferred, FALSE, &commands);
    if (FAILED(hr)) return hr;
    /* TRUE: the title's pipeline state, including its bound target, is put
     * back exactly as it was. */
    ID3D11DeviceContext_ExecuteCommandList(d3d8_GetD3D11Context(), commands, TRUE);
    ID3D11CommandList_Release(commands);
    if (g_overlay)
        g_overlay(d3d8_GetD3D11Device(), d3d8_GetD3D11Context(), g_window_rtv,
                  g_window_width, g_window_height, g_overlay_user);
    return S_OK;
}
