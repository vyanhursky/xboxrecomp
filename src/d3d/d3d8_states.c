/**
 * D3D8 Render State → D3D11 State Object Translation
 *
 * Converts D3D8 render state values into D3D11 state objects:
 *   - Blend state (alpha blending, color write mask)
 *   - Depth-stencil state (z-test, z-write, stencil)
 *   - Rasterizer state (cull mode, fill mode)
 *   - Sampler state (texture filtering, addressing)
 *
 * State objects are cached and recreated only when dirty.
 */

#include "d3d8_internal.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ================================================================
 * Cached D3D11 state objects
 * ================================================================ */

static ID3D11BlendState        *g_blend_state = NULL;
static ID3D11DepthStencilState *g_ds_state = NULL;
static ID3D11RasterizerState   *g_raster_state = NULL;
static ID3D11SamplerState      *g_sampler_states[4] = { NULL, NULL, NULL, NULL };

/* Last known render state identity for dirty detection */
static DWORD g_last_blend_hash = 0;
static DWORD g_last_raster_hash = 0;
static D3D11_DEPTH_STENCIL_DESC g_last_ds_desc;
static D3D11_BLEND_DESC g_last_blend_desc;
static D3D11_RASTERIZER_DESC g_last_raster_desc;
static D3D11_SAMPLER_DESC g_last_sampler_desc[4];
static int g_sampler_valid[4];

/* A cache owns one reference, independent of the active state and context.
 * Full zero-initialized descriptors are keys; dynamic bind values stay out. */
#define STATE_SLOTS 32
typedef struct { unsigned char desc[512]; IUnknown *object; uint64_t used; } StateEntry;
static StateEntry g_state_cache[4][STATE_SLOTS];
static uint64_t g_state_tick;
static D3D8StateStats g_state_stats;
static int g_reuse = -1, g_profile = -1;

static int reuse_states(void)
{
    if (g_reuse < 0) {
        const char *v = getenv("RECOMP_D3D11_STATE_CACHE");
        g_reuse = v && *v == '1';
    }
    return g_reuse;
}

static void *find_state(unsigned kind, const void *desc, size_t size)
{
    unsigned i;
    for (i = 0; i < STATE_SLOTS; i++) {
        StateEntry *e = &g_state_cache[kind][i];
        if (e->object && !memcmp(e->desc, desc, size)) {
            e->used = ++g_state_tick;
            IUnknown_AddRef(e->object);
            g_state_stats.hits[kind]++;
            return e->object;
        }
    }
    return NULL;
}

static void keep_state(unsigned kind, const void *desc, size_t size, void *object)
{
    unsigned i, slot = 0;
    StateEntry *e;
    if (size > sizeof g_state_cache[0][0].desc) return;
    for (i = 0; i < STATE_SLOTS; i++) {
        if (!g_state_cache[kind][i].object) { slot = i; break; }
        if (g_state_cache[kind][i].used < g_state_cache[kind][slot].used) slot = i;
    }
    e = &g_state_cache[kind][slot];
    if (e->object) { IUnknown_Release(e->object); g_state_stats.evictions[kind]++; }
    memcpy(e->desc, desc, size);
    e->object = (IUnknown *)object;
    IUnknown_AddRef(e->object);
    e->used = ++g_state_tick;
}

void d3d8_states_get_stats(D3D8StateStats *out) { if (out) *out = g_state_stats; }

static void state_report(void)
{
    static ULONGLONG last;
    ULONGLONG now;
    if (g_profile < 0) {
        const char *v = getenv("RECOMP_STREAM2_STATS");
        g_profile = v && *v == '1';
    }
    if (!g_profile) return;
    now = GetTickCount64();
    if (!last) last = now;
    if (now - last < 2000) return;
    last = now;
    fprintf(stderr, "[S2STATE] enabled=%d draws=%llu creates=%llu,%llu,%llu,%llu hits=%llu,%llu,%llu,%llu fast=%llu,%llu,%llu,%llu\n",
        reuse_states(), g_state_stats.draws,
        g_state_stats.creates[0], g_state_stats.creates[1], g_state_stats.creates[2], g_state_stats.creates[3],
        g_state_stats.hits[0], g_state_stats.hits[1], g_state_stats.hits[2], g_state_stats.hits[3],
        g_state_stats.fast[0], g_state_stats.fast[1], g_state_stats.fast[2], g_state_stats.fast[3]);
}

/* ================================================================
 * D3D8 → D3D11 enum translation
 * ================================================================ */

static D3D11_BLEND d3d8_to_d3d11_blend(DWORD d3d8blend)
{
    switch (d3d8blend) {
    case D3DBLEND_ZERO:         return D3D11_BLEND_ZERO;
    case D3DBLEND_ONE:          return D3D11_BLEND_ONE;
    case D3DBLEND_SRCCOLOR:     return D3D11_BLEND_SRC_COLOR;
    case D3DBLEND_INVSRCCOLOR:  return D3D11_BLEND_INV_SRC_COLOR;
    case D3DBLEND_SRCALPHA:     return D3D11_BLEND_SRC_ALPHA;
    case D3DBLEND_INVSRCALPHA:  return D3D11_BLEND_INV_SRC_ALPHA;
    case D3DBLEND_DESTALPHA:    return D3D11_BLEND_DEST_ALPHA;
    case D3DBLEND_INVDESTALPHA: return D3D11_BLEND_INV_DEST_ALPHA;
    case D3DBLEND_DESTCOLOR:    return D3D11_BLEND_DEST_COLOR;
    case D3DBLEND_INVDESTCOLOR: return D3D11_BLEND_INV_DEST_COLOR;
    case D3DBLEND_SRCALPHASAT:  return D3D11_BLEND_SRC_ALPHA_SAT;
    case D3DBLEND_CONSTANTCOLOR:
    case D3DBLEND_CONSTANTALPHA:    return D3D11_BLEND_BLEND_FACTOR;
    case D3DBLEND_INVCONSTANTCOLOR:
    case D3DBLEND_INVCONSTANTALPHA: return D3D11_BLEND_INV_BLEND_FACTOR;
    default:                    return D3D11_BLEND_ONE;
    }
}

/* D3D11 forbids color factors in the alpha channel. Use their alpha components. */
static D3D11_BLEND blend_alpha_factor(D3D11_BLEND blend)
{
    switch (blend) {
    case D3D11_BLEND_SRC_COLOR:      return D3D11_BLEND_SRC_ALPHA;
    case D3D11_BLEND_INV_SRC_COLOR:  return D3D11_BLEND_INV_SRC_ALPHA;
    case D3D11_BLEND_DEST_COLOR:     return D3D11_BLEND_DEST_ALPHA;
    case D3D11_BLEND_INV_DEST_COLOR: return D3D11_BLEND_INV_DEST_ALPHA;
    default:                       return blend;
    }
}

static D3D11_COMPARISON_FUNC d3d8_to_d3d11_cmp(DWORD d3d8cmp)
{
    switch (d3d8cmp) {
    case D3DCMP_NEVER:        return D3D11_COMPARISON_NEVER;
    case D3DCMP_LESS:         return D3D11_COMPARISON_LESS;
    case D3DCMP_EQUAL:        return D3D11_COMPARISON_EQUAL;
    case D3DCMP_LESSEQUAL:    return D3D11_COMPARISON_LESS_EQUAL;
    case D3DCMP_GREATER:      return D3D11_COMPARISON_GREATER;
    case D3DCMP_NOTEQUAL:     return D3D11_COMPARISON_NOT_EQUAL;
    case D3DCMP_GREATEREQUAL: return D3D11_COMPARISON_GREATER_EQUAL;
    case D3DCMP_ALWAYS:       return D3D11_COMPARISON_ALWAYS;
    default:                  return D3D11_COMPARISON_LESS_EQUAL;
    }
}

static D3D11_STENCIL_OP d3d8_to_d3d11_stencilop(DWORD op)
{
    switch (op) {
    case 1: return D3D11_STENCIL_OP_KEEP;
    case 2: return D3D11_STENCIL_OP_ZERO;
    case 3: return D3D11_STENCIL_OP_REPLACE;
    case 4: return D3D11_STENCIL_OP_INCR_SAT;
    case 5: return D3D11_STENCIL_OP_DECR_SAT;
    case 6: return D3D11_STENCIL_OP_INVERT;
    case 7: return D3D11_STENCIL_OP_INCR;
    case 8: return D3D11_STENCIL_OP_DECR;
    default: return D3D11_STENCIL_OP_KEEP;
    }
}

/* ARGB; see D3DBLEND_CONSTANTCOLOR. */
static DWORD g_blend_color = 0;

void d3d8_SetBlendColor(DWORD argb)
{
    g_blend_color = argb;
}

static D3D11_BLEND_OP d3d8_to_d3d11_blendop(DWORD op)
{
    switch (op) {
    case 1: return D3D11_BLEND_OP_ADD;
    case 2: return D3D11_BLEND_OP_SUBTRACT;
    case 3: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 4: return D3D11_BLEND_OP_MIN;
    case 5: return D3D11_BLEND_OP_MAX;
    default: return D3D11_BLEND_OP_ADD;
    }
}

/* Simple hash of relevant render state values for dirty detection */
static DWORD hash_blend_states(const DWORD *rs)
{
    return rs[D3DRS_ALPHABLENDENABLE] ^
           (rs[D3DRS_SRCBLEND] << 4) ^
           (rs[D3DRS_DESTBLEND] << 8) ^
           (rs[D3DRS_BLENDOP] << 12) ^
           (rs[D3DRS_COLORWRITEENABLE] << 16);
}

static DWORD hash_raster_states(const DWORD *rs)
{
    return rs[D3DRS_CULLMODE] ^
           (rs[D3DRS_FILLMODE] << 4);
}

/* ================================================================
 * State object creation
 * ================================================================ */

static void update_blend_state(const DWORD *rs)
{
    DWORD hash = hash_blend_states(rs);
    D3D11_BLEND_DESC bd;
    HRESULT hr;

    if (!reuse_states() && hash == g_last_blend_hash && g_blend_state) return;
    g_last_blend_hash = hash;

    memset(&bd, 0, sizeof(bd));
    bd.RenderTarget[0].BlendEnable = rs[D3DRS_ALPHABLENDENABLE] ? TRUE : FALSE;
    bd.RenderTarget[0].SrcBlend = d3d8_to_d3d11_blend(rs[D3DRS_SRCBLEND]);
    bd.RenderTarget[0].DestBlend = d3d8_to_d3d11_blend(rs[D3DRS_DESTBLEND]);
    bd.RenderTarget[0].BlendOp = d3d8_to_d3d11_blendop(rs[D3DRS_BLENDOP] ? rs[D3DRS_BLENDOP] : 1);
    bd.RenderTarget[0].SrcBlendAlpha = blend_alpha_factor(bd.RenderTarget[0].SrcBlend);
    bd.RenderTarget[0].DestBlendAlpha = blend_alpha_factor(bd.RenderTarget[0].DestBlend);
    bd.RenderTarget[0].BlendOpAlpha = bd.RenderTarget[0].BlendOp;
    bd.RenderTarget[0].RenderTargetWriteMask = (UINT8)(rs[D3DRS_COLORWRITEENABLE] & 0x0F);

    if (reuse_states() && g_blend_state && !memcmp(&bd, &g_last_blend_desc, sizeof bd)) {
        g_state_stats.fast[0]++; return;
    }
    if (g_blend_state) { ID3D11BlendState_Release(g_blend_state); g_blend_state = NULL; }
    if (reuse_states()) g_blend_state = (ID3D11BlendState *)find_state(0, &bd, sizeof bd);
    if (g_blend_state) { g_last_blend_desc = bd; return; }
    g_state_stats.creates[0]++;
    hr = ID3D11Device_CreateBlendState(d3d8_GetD3D11Device(), &bd, &g_blend_state);
    if (SUCCEEDED(hr)) {
        g_last_blend_desc = bd;
        if (reuse_states()) keep_state(0, &bd, sizeof bd, g_blend_state);
    }
    if (FAILED(hr))
        fprintf(stderr, "D3D8: CreateBlendState failed: 0x%08lX\n", hr);
}

static void update_depth_stencil_state(const DWORD *rs)
{
    D3D11_DEPTH_STENCIL_DESC dsd;
    HRESULT hr;

    memset(&dsd, 0, sizeof(dsd));
    dsd.DepthEnable = rs[D3DRS_ZENABLE] ? TRUE : FALSE;
    dsd.DepthWriteMask = rs[D3DRS_ZWRITEENABLE] ? D3D11_DEPTH_WRITE_MASK_ALL : D3D11_DEPTH_WRITE_MASK_ZERO;
    dsd.DepthFunc = d3d8_to_d3d11_cmp(rs[D3DRS_ZFUNC]);

    dsd.StencilEnable = rs[D3DRS_STENCILENABLE] ? TRUE : FALSE;
    dsd.StencilReadMask = (UINT8)(rs[D3DRS_STENCILMASK] & 0xFF);
    dsd.StencilWriteMask = (UINT8)(rs[D3DRS_STENCILWRITEMASK] & 0xFF);

    dsd.FrontFace.StencilFunc = d3d8_to_d3d11_cmp(rs[D3DRS_STENCILFUNC]);
    dsd.FrontFace.StencilFailOp = d3d8_to_d3d11_stencilop(rs[D3DRS_STENCILFAIL]);
    dsd.FrontFace.StencilDepthFailOp = d3d8_to_d3d11_stencilop(rs[D3DRS_STENCILZFAIL]);
    dsd.FrontFace.StencilPassOp = d3d8_to_d3d11_stencilop(rs[D3DRS_STENCILPASS]);
    dsd.BackFace = dsd.FrontFace;

    /* Zeroed padding makes the complete descriptor comparable; the reference
     * is bound separately. */
    if (g_ds_state && memcmp(&dsd, &g_last_ds_desc, sizeof(dsd)) == 0) {
        g_state_stats.fast[1]++; return;
    }
    if (g_ds_state) {
        ID3D11DepthStencilState_Release(g_ds_state);
        g_ds_state = NULL;
    }

    if (reuse_states()) g_ds_state = (ID3D11DepthStencilState *)find_state(1, &dsd, sizeof dsd);
    if (g_ds_state) { g_last_ds_desc = dsd; return; }
    g_state_stats.creates[1]++;
    hr = ID3D11Device_CreateDepthStencilState(d3d8_GetD3D11Device(), &dsd, &g_ds_state);
    if (FAILED(hr))
        fprintf(stderr, "D3D8: CreateDepthStencilState failed: 0x%08lX\n", hr);
    else {
        memcpy(&g_last_ds_desc, &dsd, sizeof(dsd));
        if (reuse_states()) keep_state(1, &dsd, sizeof dsd, g_ds_state);
    }
}

static void update_rasterizer_state(const DWORD *rs)
{
    DWORD hash = hash_raster_states(rs);
    D3D11_RASTERIZER_DESC rd;
    HRESULT hr;

    if (!reuse_states() && hash == g_last_raster_hash && g_raster_state) return;
    g_last_raster_hash = hash;

    memset(&rd, 0, sizeof(rd));

    switch (rs[D3DRS_FILLMODE]) {
    case D3DFILL_POINT:     rd.FillMode = D3D11_FILL_WIREFRAME; break;  /* D3D11 has no point fill */
    case D3DFILL_WIREFRAME: rd.FillMode = D3D11_FILL_WIREFRAME; break;
    default:                rd.FillMode = D3D11_FILL_SOLID; break;
    }

    switch (rs[D3DRS_CULLMODE]) {
    case D3DCULL_NONE: rd.CullMode = D3D11_CULL_NONE; break;
    case D3DCULL_CW:   rd.CullMode = D3D11_CULL_FRONT; break;  /* D3D8 CW = cull front in D3D11 convention */
    case D3DCULL_CCW:  rd.CullMode = D3D11_CULL_BACK; break;
    default:           rd.CullMode = D3D11_CULL_BACK; break;
    }

    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable = TRUE;
    rd.ScissorEnable = TRUE;               /* d3d8_SetScissorRect() */
    rd.MultisampleEnable = FALSE;
    rd.AntialiasedLineEnable = FALSE;

    if (reuse_states() && g_raster_state && !memcmp(&rd, &g_last_raster_desc, sizeof rd)) {
        g_state_stats.fast[2]++; return;
    }
    if (g_raster_state) { ID3D11RasterizerState_Release(g_raster_state); g_raster_state = NULL; }
    if (reuse_states()) g_raster_state = (ID3D11RasterizerState *)find_state(2, &rd, sizeof rd);
    if (g_raster_state) { g_last_raster_desc = rd; return; }
    g_state_stats.creates[2]++;
    hr = ID3D11Device_CreateRasterizerState(d3d8_GetD3D11Device(), &rd, &g_raster_state);
    if (SUCCEEDED(hr)) {
        g_last_raster_desc = rd;
        if (reuse_states()) keep_state(2, &rd, sizeof rd, g_raster_state);
    }
    if (FAILED(hr))
        fprintf(stderr, "D3D8: CreateRasterizerState failed: 0x%08lX\n", hr);
}

/* ================================================================
 * Sampler state
 * ================================================================ */

static D3D11_TEXTURE_ADDRESS_MODE d3d8_to_d3d11_address(DWORD mode)
{
    switch (mode) {
    case D3DTADDRESS_WRAP:       return D3D11_TEXTURE_ADDRESS_WRAP;
    case D3DTADDRESS_MIRROR:     return D3D11_TEXTURE_ADDRESS_MIRROR;
    case D3DTADDRESS_CLAMP:      return D3D11_TEXTURE_ADDRESS_CLAMP;
    case D3DTADDRESS_BORDER:     return D3D11_TEXTURE_ADDRESS_BORDER;
    case D3DTADDRESS_MIRRORONCE: return D3D11_TEXTURE_ADDRESS_MIRROR_ONCE;
    default:                     return D3D11_TEXTURE_ADDRESS_WRAP;
    }
}

/* [display] anisotropy (d3d8_set_texture_anisotropy): 1 off, else 2-16. */
static volatile LONG g_anisotropy = 1;

void d3d8_set_texture_anisotropy(unsigned max)
{
    LONG v = max < 2 ? 1 : max > D3D11_REQ_MAXANISOTROPY ? D3D11_REQ_MAXANISOTROPY : (LONG)max;
    InterlockedExchange(&g_anisotropy, v);
}

static D3D11_FILTER d3d8_to_d3d11_filter(DWORD mag, DWORD min, DWORD mip)
{
    /* Simplified filter mapping */
    BOOL mag_linear = (mag == D3DTEXF_LINEAR || mag == D3DTEXF_ANISOTROPIC);
    BOOL min_linear = (min == D3DTEXF_LINEAR || min == D3DTEXF_ANISOTROPIC);
    BOOL mip_linear = (mip == D3DTEXF_LINEAR);

    if (mag == D3DTEXF_ANISOTROPIC || min == D3DTEXF_ANISOTROPIC)
        return D3D11_FILTER_ANISOTROPIC;

    if (min_linear && mag_linear && mip_linear)
        return D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    if (min_linear && mag_linear)
        return D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    if (min_linear)
        return D3D11_FILTER_MIN_LINEAR_MAG_MIP_POINT;
    if (mag_linear)
        return D3D11_FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT;

    return D3D11_FILTER_MIN_MAG_MIP_POINT;
}

void d3d8_states_apply_sampler(DWORD stage)
{
    const DWORD *tss;
    D3D11_SAMPLER_DESC sd;
    HRESULT hr;
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();

    if (stage >= 4) return;
    tss = d3d8_GetTSS(stage);
    if (!tss) return;

    memset(&sd, 0, sizeof(sd));
    sd.Filter = d3d8_to_d3d11_filter(
        tss[D3DTSS_MAGFILTER],
        tss[D3DTSS_MINFILTER],
        tss[D3DTSS_MIPFILTER]);
    sd.AddressU = d3d8_to_d3d11_address(tss[D3DTSS_ADDRESSU] ? tss[D3DTSS_ADDRESSU] : D3DTADDRESS_WRAP);
    sd.AddressV = d3d8_to_d3d11_address(tss[D3DTSS_ADDRESSV] ? tss[D3DTSS_ADDRESSV] : D3DTADDRESS_WRAP);
    sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.MaxAnisotropy = tss[D3DTSS_MAXANISOTROPY] ? tss[D3DTSS_MAXANISOTROPY] : 1;
    /* The player's anisotropic filtering, for a texture the title minifies
     * linearly; a nearest one is a cut-out and stays exactly as asked. */
    {
        LONG aniso = g_anisotropy;
        DWORD min = tss[D3DTSS_MINFILTER];
        if (aniso > 1 && (min == D3DTEXF_LINEAR || min == D3DTEXF_ANISOTROPIC)) {
            sd.Filter = D3D11_FILTER_ANISOTROPIC;
            if ((LONG)sd.MaxAnisotropy < aniso)
                sd.MaxAnisotropy = (UINT)aniso;
        }
    }
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    /* MIPFILTER NONE samples level 0 only, as the Vulkan device does; D3D11
     * has no "no mip" filter, so the LOD range says it. */
    sd.MaxLOD = tss[D3DTSS_MIPFILTER] == D3DTEXF_NONE ? 0.0f : D3D11_FLOAT32_MAX;

    if (reuse_states() && g_sampler_states[stage] && g_sampler_valid[stage] &&
        !memcmp(&sd, &g_last_sampler_desc[stage], sizeof sd)) {
        g_state_stats.fast[3]++;
        ID3D11DeviceContext_PSSetSamplers(ctx, stage, 1, &g_sampler_states[stage]);
        return;
    }
    if (g_sampler_states[stage]) { ID3D11SamplerState_Release(g_sampler_states[stage]); g_sampler_states[stage] = NULL; }
    g_sampler_valid[stage] = 0;
    if (reuse_states()) g_sampler_states[stage] = (ID3D11SamplerState *)find_state(3, &sd, sizeof sd);
    if (g_sampler_states[stage]) hr = S_OK;
    else {
        g_state_stats.creates[3]++;
        hr = ID3D11Device_CreateSamplerState(d3d8_GetD3D11Device(), &sd, &g_sampler_states[stage]);
        if (SUCCEEDED(hr) && reuse_states()) keep_state(3, &sd, sizeof sd, g_sampler_states[stage]);
    }
    if (SUCCEEDED(hr)) {
        g_last_sampler_desc[stage] = sd;
        g_sampler_valid[stage] = 1;
        ID3D11DeviceContext_PSSetSamplers(ctx, stage, 1, &g_sampler_states[stage]);
    }
}

/* ================================================================
 * Apply all states before draw call
 * ================================================================ */

HRESULT d3d8_states_init(void)
{
    memset(&g_state_stats, 0, sizeof g_state_stats);
    /* States are created on first apply */
    return S_OK;
}

void d3d8_states_shutdown(void)
{
    int i;
    if (g_blend_state)  { ID3D11BlendState_Release(g_blend_state); g_blend_state = NULL; }
    if (g_ds_state)     { ID3D11DepthStencilState_Release(g_ds_state); g_ds_state = NULL; }
    if (g_raster_state) { ID3D11RasterizerState_Release(g_raster_state); g_raster_state = NULL; }
    for (i = 0; i < 4; i++) {
        if (g_sampler_states[i]) {
            ID3D11SamplerState_Release(g_sampler_states[i]);
            g_sampler_states[i] = NULL;
        }
    }
    g_last_blend_hash = 0;
    g_last_raster_hash = 0;
    memset(g_sampler_valid, 0, sizeof g_sampler_valid);
    {
        unsigned kind, slot;
        for (kind = 0; kind < 4; kind++) for (slot = 0; slot < STATE_SLOTS; slot++) {
            StateEntry *e = &g_state_cache[kind][slot];
            if (e->object) IUnknown_Release(e->object);
        }
        memset(g_state_cache, 0, sizeof g_state_cache);
        g_state_tick = 0;
    }
}

void d3d8_states_apply(void)
{
    const DWORD *rs = d3d8_GetRenderStates();
    ID3D11DeviceContext *ctx = d3d8_GetD3D11Context();
    float blend_factor[4] = { 1, 1, 1, 1 };

    if (!rs || !ctx) return;
    g_state_stats.draws++;
    state_report();

    /* The blend colour, for the constant factors. D3D11 has one factor and
     * applies its alpha to the alpha channel only, so a constant-ALPHA
     * factor on colour is done by putting the alpha in all four. */
    {
        DWORD c = g_blend_color, sb = rs[D3DRS_SRCBLEND], db = rs[D3DRS_DESTBLEND];
        float a = ((c >> 24) & 0xFF) / 255.0f;
        if (sb == D3DBLEND_CONSTANTALPHA || sb == D3DBLEND_INVCONSTANTALPHA
                || db == D3DBLEND_CONSTANTALPHA || db == D3DBLEND_INVCONSTANTALPHA) {
            blend_factor[0] = blend_factor[1] = blend_factor[2] = blend_factor[3] = a;
        } else {
            blend_factor[0] = ((c >> 16) & 0xFF) / 255.0f;
            blend_factor[1] = ((c >> 8) & 0xFF) / 255.0f;
            blend_factor[2] = (c & 0xFF) / 255.0f;
            blend_factor[3] = a;
        }
    }

    update_blend_state(rs);
    update_depth_stencil_state(rs);
    update_rasterizer_state(rs);

    if (g_blend_state)
        ID3D11DeviceContext_OMSetBlendState(ctx, g_blend_state, blend_factor, 0xFFFFFFFF);
    if (g_ds_state)
        ID3D11DeviceContext_OMSetDepthStencilState(ctx, g_ds_state, rs[D3DRS_STENCILREF]);
    if (g_raster_state)
        ID3D11DeviceContext_RSSetState(ctx, g_raster_state);

    /* Apply samplers for all 4 texture stages */
    {
        DWORD s;
        for (s = 0; s < 4; s++)
            d3d8_states_apply_sampler(s);
    }
}
