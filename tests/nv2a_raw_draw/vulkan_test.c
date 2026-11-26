/* Game-free, offscreen native Vulkan differential and upload-ring stress.
 * Run with Lavapipe when no hardware ICD is available. No SDL/window/assets. */
#include "d3d8_xbox.h"
#include "d3d8_vk.h"
#include "nv2a_vsh_cpu.h"
#include "nv2a_vsh_interp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
extern int d3d8_Nv2aProgramDraw(uint32_t,const char *,uint32_t,const float *,uint32_t,
    float,const float[2],const float *,uint32_t,const uint16_t *,uint32_t,const Nv2aRawLayout *);
static IDirect3DDevice8 *dev;
static IDirect3DSurface8 *surface;
static float consts[192*4],texscale[2]={1,1};
static float gathered[3][8];
static unsigned char reference[32*32*4];
static uint16_t indices[196608];
static char normal_hlsl[65536],raw_hlsl[65536];
static uint32_t hash,version=1;
static const float positions[3][4]={{4,4,.5f,1},{28,4,.5f,1},{4,28,.5f,1}};

static void capture(unsigned char *out)
{
    D3DLOCKED_RECT lr;
    CHECK(SUCCEEDED(surface->lpVtbl->LockRect(surface,&lr,NULL,0)));
    for(unsigned y=0;y<32;++y)memcpy(out+y*32*4,(const unsigned char *)lr.pBits+y*lr.Pitch,32*4);
    CHECK(SUCCEEDED(surface->lpVtbl->UnlockRect(surface)));
}
static void clear(void) { CHECK(SUCCEEDED(dev->lpVtbl->Clear(dev,0,NULL,D3DCLEAR_TARGET,0xff000000u,1,0))); }
static void compare(void)
{
    unsigned char actual[sizeof reference];capture(actual);
    CHECK(!memcmp(reference,actual,sizeof reference));
}
static void start(void)
{
    D3DPRESENT_PARAMETERS pp={0};
    IDirect3D8 *d=xbox_Direct3DCreate8(0);
    pp.BackBufferWidth=32;pp.BackBufferHeight=32;pp.BackBufferFormat=D3DFMT_A8R8G8B8;pp.Windowed=1;
    CHECK(SUCCEEDED(d->lpVtbl->CreateDevice(d,0,1,NULL,0,&pp,&dev)));
    CHECK(SUCCEEDED(dev->lpVtbl->GetBackBuffer(dev,0,0,&surface)));
    dev->lpVtbl->SetRenderState(dev,D3DRS_ZENABLE,0);
    /* MOV oD0,v3; MOV oPos,v0, final. */
    uint32_t color[4]={0,(1u<<21)|(3u<<9)|0x1bu,2u<<26,(15u<<12)|(1u<<11)|(3u<<3)};
    uint32_t pos[4]={0,(1u<<21)|0x1bu,2u<<26,(15u<<12)|(1u<<11)|1u};
    nv2a_vsh_set_instruction(0,color);nv2a_vsh_set_instruction(1,pos);
    uint32_t used;CHECK(nv2a_vsh_emit_hlsl(normal_hlsl,sizeof normal_hlsl,&used)>0 && used==9);
    hash=nv2a_vsh_program_hash();
    for(unsigned i=0;i<sizeof indices/sizeof indices[0];++i)indices[i]=i<3?(uint16_t)i:0;
    for(unsigned v=0;v<3;++v) {
        memcpy(gathered[v],positions[v],16);
        gathered[v][4]=0x85/255.0f;gathered[v][5]=0x33/255.0f;gathered[v][6]=0xa9/255.0f;gathered[v][7]=1;
    }
    clear();CHECK(d3d8_Nv2aProgramDraw(hash,normal_hlsl,9,consts,version,1,texscale,
        &gathered[0][0],3,indices,3,NULL)==1);
    capture(reference);
    unsigned lit=0;for(unsigned i=0;i<sizeof reference;i+=4)if(reference[i]||reference[i+1]||reference[i+2])++lit;
    CHECK(lit>100 && lit<400); /* compare cannot pass with two blank targets */
}
static Nv2aRawLayout layout(unsigned stride,unsigned offset,unsigned size)
{
    Nv2aRawLayout r={0};r.used=9;r.streams=1;r.stride[0]=stride;r.type[0]=2;r.size[0]=size;
    r.type[3]=0;r.size[3]=4;r.offset[3]=offset;return r;
}
static unsigned char *vertices(const Nv2aRawLayout *r,unsigned n)
{
    uint32_t offsets[16],bytes=nv2a_raw_offsets(r,n,offsets);
    unsigned char *p=calloc(1,bytes);CHECK(p && bytes);
    for(unsigned v=0;v<3;++v) {
        uint32_t color=0xff8533a9u;
        memcpy(p+offsets[r->binding[0]]+v*r->stride[r->binding[0]]+r->offset[0],positions[v],r->size[0]*4);
        memcpy(p+offsets[r->binding[3]]+v*r->stride[r->binding[3]]+r->offset[3],&color,4);
    }
    return p;
}
static int draw_raw(const Nv2aRawLayout *r,const void *v,unsigned n,unsigned ni)
{
    uint32_t used;CHECK(nv2a_vsh_emit_hlsl_raw(raw_hlsl,sizeof raw_hlsl,&used,r)>0 && used==9);
    return d3d8_Nv2aProgramDraw(hash,raw_hlsl,9,consts,++version,1,texscale,v,n,indices,ni,r);
}
static void fallback_index_wrap(void)
{
    /* Start with fresh rings, then leave less than one draw's index space.
     * The fallback must reserve ALL rings before copying its vertex data:
     * an index-triggered frame switch would otherwise lose its vertex offset.
     * Clear immediately before the final draw so earlier triangles cannot
     * conceal a dropped/corrupt fallback. */
    unsigned char scratch[sizeof reference];capture(scratch);
    Nv2aRawLayout raw=layout(20,16,4),rejected=raw;
    unsigned char *v=vertices(&raw,3);
    clear();for(unsigned i=0;i<10;++i)CHECK(draw_raw(&raw,v,3,196608)==1);
    clear();rejected.stride[0]=0;
    CHECK(d3d8_Nv2aProgramDraw(hash,NULL,9,consts,++version,1,texscale,
        (const float *)v,3,indices,196608,&rejected)==-1);
    CHECK(d3d8_Nv2aProgramDraw(hash,normal_hlsl,9,consts,++version,1,texscale,
        &gathered[0][0],3,indices,196608,NULL)==1);
    compare();free(v);
}
static void format_cases(void)
{
    float saved[3][8];unsigned char saved_pixels[sizeof reference];
    memcpy(saved,gathered,sizeof saved);memcpy(saved_pixels,reference,sizeof reference);
    for(unsigned t=0;t<=6;++t) if(t!=3) for(unsigned n=1;n<=4;++n) for(unsigned trial=0;trial<2;++trial) {
        Nv2aRawLayout r=layout(16,0,4);
        r.streams=2;r.binding[3]=1;r.type[3]=t;r.size[3]=n;
        r.stride[1]=(nv2a_raw_fetch_bytes(t,n)+3u)&~3u;
        unsigned char *v=vertices(&r,3);uint32_t offsets[16];
        CHECK(nv2a_raw_offsets(&r,3,offsets));
        for(unsigned vi=0;vi<3;++vi) {
            unsigned char *at=v+offsets[1]+vi*r.stride[1];
            if(t==0 || t==4) { uint32_t w=trial?0x77d2a907u:0xff8533a9u;memcpy(at,&w,4); }
            else if(t==2) { float f[4]={trial?-.5f:.17f,.59f,.88f,1};memcpy(at,f,n*4); }
            else if(t==1 || t==5) {
                int16_t s[4]={trial?-32768:16384,8192,32767,16384};
                if(t==5) { s[0]=trial?-1:1;s[1]=0;s[2]=1;s[3]=1; }
                memcpy(at,s,n*2);
            } else {
                int x=trial?-1024:256,y=512,z=trial?-512:128;
                uint32_t w=((uint32_t)x&2047u)|(((uint32_t)y&2047u)<<11)|(((uint32_t)z&1023u)<<22);
                memcpy(at,&w,4);
            }
        }
        Nv2aRawDraw d={0};d.layout=r;d.vertices=v;d.nverts=3;nv2a_raw_gather(&d,&gathered[0][0]);
        clear();CHECK(d3d8_Nv2aProgramDraw(hash,normal_hlsl,9,consts,++version,1,texscale,
            &gathered[0][0],3,indices,3,NULL)==1);capture(reference);
        clear();CHECK(draw_raw(&r,v,3,3)==1);compare();free(v);
    }
    memcpy(gathered,saved,sizeof saved);memcpy(reference,saved_pixels,sizeof reference);
}
static void point_ring_wrap(void)
{
    extern void d3d8_Nv2aPointMode(int,float);
    unsigned char triangle[sizeof reference];memcpy(triangle,reference,sizeof reference);
    d3d8_Nv2aPointMode(1,2);
    clear();CHECK(d3d8_Nv2aProgramDraw(hash,normal_hlsl,9,consts,++version,1,texscale,
        &gathered[0][0],3,indices,3,NULL)==1);capture(reference);
    unsigned lit=0;for(unsigned i=0;i<sizeof reference;i+=4)if(reference[i]||reference[i+1]||reference[i+2])++lit;
    CHECK(lit>4 && lit<64);
    /* Point instances upload one record PER INDEX, not per unique vertex.
     * This crosses the vertex ring with only three source vertices. */
    clear();for(unsigned i=0;i<26;++i)
        CHECK(d3d8_Nv2aProgramDraw(hash,normal_hlsl,9,consts,++version,1,texscale,
            &gathered[0][0],3,indices,65536,NULL)==1);
    compare();d3d8_Nv2aPointMode(0,0);memcpy(reference,triangle,sizeof reference);
}
int main(int argc,char **argv)
{
    if(argc>1 && !strcmp(argv[1],"--msaa")) {
        extern void d3d8_set_msaa(unsigned);
        d3d8_set_msaa(4);
    }
    start();
    fallback_index_wrap();
    Nv2aRawLayout raw=layout(20,16,4);
    unsigned char *v=vertices(&raw,3);
    clear();CHECK(draw_raw(&raw,v,3,3)==1);compare();free(v);
    raw=layout(16,12,3);v=vertices(&raw,3);
    clear();CHECK(draw_raw(&raw,v,3,3)==1);compare();free(v);
    /* The same shader/program hash with a different byte layout, then back. */
    raw=layout(32,28,4);v=vertices(&raw,3);
    clear();CHECK(draw_raw(&raw,v,3,3)==1);compare();free(v);
    format_cases(); /* separate bindings, all packed/float sizes and signs */
    point_ring_wrap();
    raw=layout(20,16,4);v=vertices(&raw,3);
    clear();CHECK(draw_raw(&raw,v,3,3)==1);compare();
    /* Uniform ring wrap alone: no vertex/index ring is close to full. */
    clear();for(unsigned i=0;i<6000;++i)CHECK(draw_raw(&raw,v,3,3)==1);compare();
    /* Index ring wrap alone, mostly degenerate triangles to avoid overdraw. */
    clear();for(unsigned i=0;i<12;++i)CHECK(draw_raw(&raw,v,3,196608)==1);compare();free(v);
    /* Vertex ring wrap alone: copied 4 MiB spans, three actual vertices. */
    raw=layout(64,16,4);v=vertices(&raw,65536);
    clear();for(unsigned i=0;i<14;++i)CHECK(draw_raw(&raw,v,65536,3)==1);compare();free(v);
    /* Real default capacity exhaustion must preserve the normal path. */
    unsigned rejected=0;
    for(unsigned i=0;i<260;++i) {
        raw=layout(24+i*4,20+i*4,4);v=vertices(&raw,3);
        clear();int result=draw_raw(&raw,v,3,3);
        if(result<0) {
            ++rejected;
            CHECK(d3d8_Nv2aProgramDraw(hash,normal_hlsl,9,consts,++version,1,texscale,
                &gathered[0][0],3,indices,3,NULL)==1);
        } else CHECK(result==1);
        compare();free(v);
    }
    CHECK(rejected>0);
    /* A new normal pipeline remains available after all raw slots filled. */
    dev->lpVtbl->SetRenderState(dev,D3DRS_ALPHABLENDENABLE,1);
    dev->lpVtbl->SetRenderState(dev,D3DRS_SRCBLEND,D3DBLEND_ONE);
    dev->lpVtbl->SetRenderState(dev,D3DRS_DESTBLEND,D3DBLEND_ZERO);
    clear();CHECK(d3d8_Nv2aProgramDraw(hash,normal_hlsl,9,consts,++version,1,texscale,
        &gathered[0][0],3,indices,3,NULL)==1);compare();
    dev->lpVtbl->SetRenderState(dev,D3DRS_ALPHABLENDENABLE,0);
    fallback_index_wrap(); /* also after raw caches have been exhausted */
    puts("native Vulkan raw draw: pixels, layout identity, all three ring wraps and capacity fallback passed");
    return 0;
}
