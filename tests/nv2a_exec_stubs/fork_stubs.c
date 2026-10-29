/*
 * What nv2a_pb_exec.c calls outside itself in the Def Jam fork, for the
 * fixtures that compile the executor on its own (nv2a_raster_batch,
 * nv2a_ffp_backend). The fork's executor forwards decoded batches to the
 * D3D11/Vulkan translator (nv2a_pgraph_d3d11.h), consults texture packs, and
 * stops at software methods (the kernel's interrupt model); none of that is on
 * these fixtures' path, which exercise upstream's software rasteriser and back
 * ends with RECOMP_PB_D3D11 unset. Every stub here is inert.
 */
#include <stdint.h>
#include <string.h>
#if !defined(_WIN32)
#include <time.h>
#endif

#include "../../src/nv2a/nv2a_pgraph_d3d11.h"
#include "../../src/kernel/nv2a_vsh_cpu.h"

typedef uint32_t stub_format;

/* src/d3d */
void d3d8_RequestSideFill(void) {}
void d3d8_FlushGPU(void) {}
unsigned int d3d8_format_bpp(stub_format fmt) { (void)fmt; return 4; }

/* kernel: software methods and the executor's stall */
void kernel_nv2a_software_method(uint32_t subch, uint32_t method, uint32_t data)
{ (void)subch; (void)method; (void)data; }
void nv2a_pb_stall(void) {}

/* the vertex-program HLSL adapter */
uint32_t nv2a_vsh_inputs_used(int *writes_consts) { if (writes_consts) *writes_consts = 0; return 0; }
int nv2a_vsh_run_ready(const float in[16][4], NvVshOutput *out) { (void)in; (void)out; return 0; }

/* the translator */
void pgraph_d3d11_bind_stage(int stage) { (void)stage; }
void pgraph_d3d11_unbind_stage(int stage) { (void)stage; }
int pgraph_d3d11_draw_program(const float *verts, uint32_t nverts, uint32_t used,
                              const uint16_t *idx, uint32_t nidx,
                              float tex_u_scale, float tex_v_scale)
{ (void)verts; (void)nverts; (void)used; (void)idx; (void)nidx; (void)tex_u_scale; (void)tex_v_scale; return 0; }
#ifndef NV2A_HOST_CLIP_TEST
void pgraph_d3d11_draw_ready(uint32_t nv2a_prim_mode, const PgraphReadyVertex *verts, uint32_t count)
{ (void)nv2a_prim_mode; (void)verts; (void)count; }
#endif
void pgraph_d3d11_frame_end(void) {}
int pgraph_d3d11_method(int subchannel, uint32_t method, uint32_t param)
{ (void)subchannel; (void)method; (void)param; return 0; }
void pgraph_d3d11_count_methods(uint32_t n) { (void)n; }
int pgraph_d3d11_program_usable(void) { return 0; }
int pgraph_d3d11_replace_texture_ready(uint32_t guest_addr, const void *data, uint32_t width,
                                       uint32_t height, uint32_t nv2a_format)
{ (void)guest_addr; (void)data; (void)width; (void)height; (void)nv2a_format; return 0; }
int pgraph_d3d11_set_texture_ready(uint32_t guest_addr, const void *data, uint32_t width,
                                   uint32_t height, uint32_t nv2a_format)
{ (void)guest_addr; (void)data; (void)width; (void)height; (void)nv2a_format; return 0; }
int pgraph_d3d11_set_texture_ready_mips(uint32_t guest_addr, const void *data, uint32_t width,
                                        uint32_t height, uint32_t nv2a_format, uint32_t levels)
{ (void)guest_addr; (void)data; (void)width; (void)height; (void)nv2a_format; (void)levels; return 0; }
void pgraph_d3d11_set_combiner_state(const Nv2aCombiner *state) { (void)state; }
void pgraph_d3d11_set_placement(float kx, float bx) { (void)kx; (void)bx; }
void pgraph_d3d11_set_point_mode(int on, float size) { (void)on; (void)size; }
#ifndef NV2A_HOST_CLIP_TEST
void pgraph_d3d11_set_surface_clip(uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{ (void)x; (void)y; (void)w; (void)h; }
#endif
void pgraph_d3d11_set_texture_address(uint32_t nv2a_u, uint32_t nv2a_v) { (void)nv2a_u; (void)nv2a_v; }
void pgraph_d3d11_set_texture_filter(uint32_t nv2a_filter) { (void)nv2a_filter; }
const char *pgraph_d3d11_stage0_id(void) { return ""; }
int pgraph_d3d11_stage0_stretch(void) { return 0; }
int pgraph_d3d11_try_texture_pack(const RecompTextureSource *source, RecompTextureSample sample, void *user)
{ (void)source; (void)sample; (void)user; return 0; }
int pgraph_d3d11_wide_bind(unsigned slab) { (void)slab; return 0; }
void pgraph_d3d11_wide_end(void) {}
int pgraph_d3d11_wide_slabs(int *dt, unsigned max) { (void)dt; (void)max; return 0; }

/* texture packs */
int texture_pack_active(void) { return 0; }
void texture_pack_set_widescreen(int on) { (void)on; }

/* the register file's accessors and the fence */
/* As the real accessor does for a page no device model owns: straight into
 * the register file, which the fixture provides. */
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset);
void xbox_Nv2aRegWrite(uint32_t va, uint32_t value)
{
    volatile uint32_t *r = xbox_Nv2aRegPtr(va - 0xFD000000u);
    if (r) *r = value;
}
void xbox_Nv2aSemaphoreRelease(uint32_t value) { (void)value; }
