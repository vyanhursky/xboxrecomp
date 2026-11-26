/* Synthetic executor-to-native bridge ordering/topology tests. */
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_vsh_cpu.h"
#include "nv2a_vsh_interp.h"
#include "nv2a_combiner.h"
#define MEM_BYTES 0x10000u
static uint8_t s_mem[MEM_BYTES];

/* ---- what the executor expects the runtime to provide ------------------ */
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)s_mem; }
uint32_t g_xbox_image_lo, g_xbox_image_hi;
uint32_t g_xbox_stack_base, g_xbox_stack_size;   /* #189 made the stack placement runtime state */
uint32_t xbox_ContiguousAllocatedBytes(void) { return 0; }
void xbox_FramebufferWindowSet(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }
static uint32_t s_fence_va;                           /* the mirrored fence word, if any */
uint32_t xbox_Nv2aFenceWordVa(void) { return s_fence_va; }
int nv2a_pb_dma_walk(void) { return 1; }             /* RECOMP_PB_WALK=dma: 0x0310 is latched */
void xbox_FramebufferWindowStart(void) {}
void xbox_FramebufferWindowFrameStats(uint32_t draws) { (void)draws; }
void xbox_FramebufferWindowPresent(uint32_t va, uint32_t pitch) { (void)va; (void)pitch; }
void xbox_Nv2aFrameCounterFlip(void) {}
/* The flip hold (nv2a_flip_hold.c, linked in). No vblank moves here, so the
 * executor never holds a flip; the walk is never inside a pushbuffer CALL. */
int nv2a_pb_scan_in_call(void) { return 0; }
uint32_t xbox_VblankCount(void) { return 0; }
uint64_t xbox_HostNowNs(void) { return 0; }
uint32_t xbox_Nv2aFrameCountersOwned(void) { return 0; }

static volatile uint32_t s_pgraph_400b10;
volatile uint32_t *xbox_Nv2aRegPtr(uint32_t offset)
{
    return offset == 0x400B10u ? &s_pgraph_400b10 : NULL;
}

#define CHECK(x) do { if(!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static int raw_calls, normal_calls, force_fallback, mutate_during_prepare, mutate_packed;
static uint32_t active_used=1;
static float captured[24][4], captured_color[24][4];
static uint32_t captured_n;
void pgraph_d3d11_set_combiner_state(const Nv2aCombiner *state)
{
    (void)state;
    if (mutate_during_prepare) {
        float changed = -9999;
        for (unsigned vi=0;vi<5;++vi) memcpy(s_mem+0x2000+vi*16,&changed,4);
    }
    if (mutate_packed) {
        memset(s_mem+0x5000,0,30);memset(s_mem+0x6000,0,20);
    }
}
int pgraph_d3d11_program_usable(void) { return 1; }
int pgraph_d3d11_draw_program(const float *v,uint32_t nv,uint32_t used,
                              const uint16_t *idx,uint32_t n,float su,float sv)
{
    (void)su;(void)sv;CHECK(used==active_used && n<=24);++normal_calls;
    captured_n=n;
    for(uint32_t i=0;i<n;++i) {
        CHECK(idx[i]<nv);const float *p=v+idx[i]*(used==9?8:4);memcpy(captured[i],p,16);
        if(used==9)memcpy(captured_color[i],p+4,16);
    }
    return 1;
}
int pgraph_d3d11_draw_program_raw(const Nv2aRawDraw *v,uint32_t used,
                                  const uint16_t *idx,uint32_t n,float su,float sv)
{
    (void)su;(void)sv;CHECK(used==active_used && n<=24);++raw_calls;
    if(force_fallback) return -1;
    CHECK(v->layout.used==used && v->nverts<=5);
    float packed[5*8];nv2a_raw_gather(v,packed);
    captured_n=n;
    for(uint32_t i=0;i<n;++i) {
        CHECK(idx[i]<v->nverts);
        const float *p=packed+idx[i]*(used==9?8:4);memcpy(captured[i],p,16);
        if(used==9)memcpy(captured_color[i],p+4,16);
    }
    return 1;
}
extern void nv2a_pb_exec_method(uint32_t subch,uint32_t method,uint32_t param);
extern int nv2a_pb_exec_elements_block(const uint32_t *,uint32_t);
static void m(uint32_t method,uint32_t value) { nv2a_pb_exec_method(0,method,value); }
static void env(const char *key,const char *value)
{
#ifdef _WIN32
    _putenv_s(key,value);
#else
    setenv(key,value,1);
#endif
}
static void draw(uint32_t prim,const uint32_t *indices,uint32_t n)
{
    m(0x17fc,prim);
    for(uint32_t i=0;i<n;i+=2) {
        uint32_t pair=indices[i]|(indices[i+1]<<16);
        CHECK(nv2a_pb_exec_elements_block(&pair,1));
    }
    m(0x17fc,0);
}
int main(int argc,char **argv)
{
    uint32_t ins[4]={0,(1u<<21)|0x1bu,2u<<26,(15u<<12)|(1u<<11)|1u};
    uint32_t indices[8]={3,1,2,2,4,1,0,4};
    const uint32_t expected[7][12]={
        {3,1,2,2,4,1}, /* points */
        {3,1,2,2,4,1}, /* triangle list */
        {3,1,2,1,2,2,2,2,4,2,4,1}, /* strip */
        {3,1,2,3,2,2,3,2,4,3,4,1}, /* fan */
        {3,1,2,3,2,2,4,1,0,4,0,4}, /* quads */
        {3,1,2,3,2,2,2,2,1,2,1,4}, /* quad strip */
        {3,1,2,3,2,2,3,2,4,3,4,1}, /* polygon */
    };
    int mode=argc>1?atoi(argv[1]):0;
    force_fallback=mode==2;
    env("RECOMP_GPU_DRAW_BRIDGE",mode?"1":"0");env("RECOMP_PB_D3D11","1");
    env("RECOMP_D3D_HLE_BULK","3");env("RECOMP_FLIP_HZ","0");
    nv2a_vsh_set_instruction(0,ins);m(0x1e94,2);
    m(0x1720,0x2000);m(0x1760,2|(3<<4)|(16<<8));
    for(uint32_t test=0;test<7;++test) {
        uint32_t prim=test?test+4:1,n=test<2?6:12;
        /* Same address, new contents: no guest-VB content/version cache. */
        for(uint32_t vi=0;vi<5;++vi) {
            float v[4]={(float)(vi*10+test),(float)(vi*20+2),(float)(vi*3+3),99};
            memcpy(s_mem+0x2000+vi*16,v,16);
        }
        int r0=raw_calls,g0=normal_calls;
        mutate_during_prepare=1;
        draw(prim,indices,prim==8?8:6);CHECK(captured_n==n);
        mutate_during_prepare=0;
        for(uint32_t i=0;i<n;++i) {
            uint32_t vi=expected[test][i];
            CHECK(captured[i][0]==(float)(vi*10+test));
            CHECK(captured[i][1]==(float)(vi*20+2));
            CHECK(captured[i][2]==(float)(vi*3+3));CHECK(captured[i][3]==1);
        }
        CHECK(raw_calls-r0==(mode && prim!=1));
        CHECK(normal_calls-g0==(!mode || force_fallback || prim==1));
    }
    /* A missing/unsupported record layout must retain the ordinary gather. */
    int r0=raw_calls,g0=normal_calls;
    m(0x1760,3|(3<<4)|(8<<8));draw(5,indices,6);
    CHECK(raw_calls==r0 && normal_calls==g0+1);
    /* Whole raw span beyond guest memory: safe absent-attribute fallback. */
    m(0x1760,2|(3<<4)|(16<<8));m(0x1720,0x03fffffcu);
    draw(5,indices,6);CHECK(raw_calls==r0 && normal_calls==g0+2);
    /* A null array is absent even when index*stride exceeds the null page. */
    m(0x1720,0);
    uint32_t high[6]={300,301,302,302,301,300};draw(5,high,6);
    CHECK(raw_calls==r0 && normal_calls==g0+3);
    for(unsigned i=0;i<6;++i)CHECK(captured[i][0]==0 && captured[i][1]==0 && captured[i][2]==0 && captured[i][3]==1);
    /* Packed SHORT3 positions and a separate byte stream, snapshotted before
     * a host preparation callback mutates BOTH guest buffers. */
    uint32_t color_ins[4]={0,(1u<<21)|(3u<<9)|0x1bu,2u<<26,(15u<<12)|(1u<<11)|(3u<<3)};
    nv2a_vsh_set_instruction(0,color_ins);nv2a_vsh_set_instruction(1,ins);active_used=9;
    m(0x1720,0x5000);m(0x1760,5|(3<<4)|(6<<8));
    m(0x172c,0x6000);m(0x176c,4|(4<<4)|(4<<8));
    for(unsigned vi=0;vi<5;++vi) {
        int16_t xyz[3]={(int16_t)(vi*7+1),(int16_t)(vi*3+2),(int16_t)(vi*5+3)};
        memcpy(s_mem+0x5000+vi*6,xyz,6);
        for(unsigned c=0;c<4;++c)s_mem[0x6000+vi*4+c]=(unsigned char)(vi*10+c+1);
    }
    r0=raw_calls;g0=normal_calls;mutate_packed=1;draw(5,indices,6);mutate_packed=0;
    CHECK(raw_calls-r0==!!mode && normal_calls-g0==(!mode || force_fallback));
    for(unsigned i=0;i<6;++i) {
        unsigned vi=indices[i];CHECK(captured[i][0]==(float)(vi*7+1));
        CHECK(captured[i][1]==(float)(vi*3+2) && captured[i][2]==(float)(vi*5+3) && captured[i][3]==1);
        for(unsigned c=0;c<4;++c)CHECK(captured_color[i][c]==(float)(vi*10+c+1)/255.0f);
    }
    printf("raw executor mode %d: primitive/index order, bulk delivery, mutation and fallback passed\n",mode);
    return 0;
}
