/* Synthetic spans only. Bounds and eligibility, independent of a GPU/backend. */
#include "nv2a_raw_draw.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
static Nv2aRawAttr make_attr(uint32_t o,uint32_t t,uint32_t n,uint32_t stride)
{ Nv2aRawAttr a={o,t,n,stride};return a; }
int main(void)
{
    Nv2aRawAttr a[16] = {{0}};
    Nv2aRawPlan p, before;
    uint32_t idx[] = {100,102,101,102,103,101};
    a[0] = make_attr(0x10000,2,3,32);
    a[3] = make_attr(0x1000c,0,4,32);
    a[9] = make_attr(0x10010,2,2,32);
    CHECK(nv2a_raw_plan(a,521,idx,6,&p));
    CHECK(p.first==100 && p.nverts==4 && p.bytes==128 && p.source[0]==0x10000+3200);
    CHECK(p.layout.stride[0]==32 && p.layout.used==521 && p.layout.offset[9]==16);
    before=p;
    CHECK(!nv2a_raw_plan(a,0,idx,6,&p)); CHECK(!memcmp(&p,&before,sizeof p));
    CHECK(!nv2a_raw_plan(a,0x10000,idx,6,&p));
    CHECK(!nv2a_raw_plan(a,521,idx,0,&p)); CHECK(!nv2a_raw_plan(a,521,idx,65537,&p));
    a[3].stride=36; CHECK(nv2a_raw_plan(a,521,idx,6,&p) && p.layout.streams==2); a[3].stride=32;
    a[3].size=0; CHECK(!nv2a_raw_plan(a,521,idx,6,&p)); a[3].size=4;
    a[3].offset+=32; CHECK(nv2a_raw_plan(a,521,idx,6,&p) && p.layout.streams==2); a[3].offset-=32;
    a[9].offset++; CHECK(nv2a_raw_plan(a,521,idx,6,&p) && p.layout.streams==2); a[9].offset--;
    idx[1]=UINT32_MAX; CHECK(!nv2a_raw_plan(a,521,idx,6,&p)); idx[1]=102;
    idx[1]=1000; CHECK(!nv2a_raw_plan(a,521,idx,6,&p)); idx[1]=102;
    a[0].offset=UINT32_MAX-31; CHECK(!nv2a_raw_plan(a,1,idx,6,&p));
    a[0].offset=0;a[0].stride=16; CHECK(nv2a_raw_plan(a,1,idx,6,&p)); /* legal inline base */
    /* Packed/overlapping source rows get safe padded native records. */
    a[0]=make_attr(0x1000,1,3,4); CHECK(nv2a_raw_plan(a,1,idx,6,&p));
    CHECK(p.source_stride[0]==4 && p.source_extent[0]==6 && p.layout.stride[0]==8);
    a[0].stride=6; CHECK(nv2a_raw_plan(a,1,idx,6,&p) && p.layout.stride[0]==8);
    {
        unsigned char source[18],target[24];
        for(unsigned i=0;i<18;++i)source[i]=(unsigned char)(i+1);
        memset(target,0xcd,sizeof target);nv2a_raw_copy_rows(target,source,3,8,6,8,6);
        for(unsigned i=0;i<3;++i) {
            CHECK(!memcmp(target+i*8,source+i*6,6));CHECK(!target[i*8+6] && !target[i*8+7]);
        }
    }
    for (uint32_t t=0;t<8;++t) for(uint32_t s=0;s<6;++s) {
        a[0]=make_attr(0x1000,t,s,16);
        int valid=s>=1 && s<=4 && t!=3 && t<=6;
        CHECK(!!nv2a_raw_plan(a,1,idx,6,&p)==valid);
    }
    /* Huge inline records with few used attributes would waste bandwidth. */
    a[0]=make_attr(0,2,4,256); CHECK(!nv2a_raw_plan(a,1,idx,6,&p));
    memset(a,0,sizeof a);
    a[0]=make_attr(0x1000,2,3,12);a[2]=make_attr(0x3000,6,3,4);a[9]=make_attr(0x4000,2,2,8);
    CHECK(nv2a_raw_plan(a,517,idx,6,&p) && p.layout.streams==3 && p.bytes==96);
    uint32_t at[16];CHECK(nv2a_raw_offsets(&p.layout,p.nverts,at)==96);
    CHECK(at[0]==0 && at[1]==48 && at[2]==64);
    CHECK(p.source[0]==0x1000+100*12 && p.source[1]==0x3000+100*4 && p.source[2]==0x4000+100*8);
    /* All sixteen native bindings at the exact combined upload/range cap. */
    uint32_t *large=malloc(65536u*sizeof *large);CHECK(large);
    for(uint32_t i=0;i<65536u;++i)large[i]=i;
    for(uint32_t i=0;i<16;++i)a[i]=make_attr(0x1000+i*0x100000,2,1,4);
    CHECK(nv2a_raw_plan(a,65535,large,65536,&p) && p.layout.streams==16 && p.bytes==4u*1024u*1024u);
    for(uint32_t i=0;i<16;++i)a[i].stride=8;
    CHECK(!nv2a_raw_plan(a,65535,large,65536,&p));
    large[65535]=65536;CHECK(!nv2a_raw_plan(a,65535,large,65536,&p));free(large);
    puts("raw planner: formats, indices, padding, overflow, sparse/bandwidth guards passed");
    return 0;
}
