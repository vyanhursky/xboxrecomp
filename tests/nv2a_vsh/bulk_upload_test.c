/* Scalar/bulk differential test: raw float bits, exact revisions, cursor,
 * short vectors, register boundaries, invalid loads and uint32 wrap. */
#include "nv2a_vsh_interp.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void reset(uint32_t load, unsigned prefix)
{
    unsigned i;
    const uint32_t pattern[4] = {0x3f800000,0x80000000,0x7fc12345,0xff800000};
    float value[4];
    memcpy(value,pattern,sizeof value);
    for(i=0;i<NV2A_VSH_CONSTANTS;i++) nv2a_vsh_set_constant(i,value);
    nv2a_vsh_set_constant_load(load);
    for(i=0;i<prefix;i++) nv2a_vsh_constant_word(0x12345678+i);
}
int main(void)
{
    const uint32_t loads[]={0,1,63,191,192,0xffffffffu};
    const unsigned sizes[]={0,1,3,4,15,16,31,32,33,767,768,769};
    uint32_t input[769],expected[NV2A_VSH_CONSTANTS*4],v0,v1,v2,v3;
    unsigned l,p,n,i,cases=0;
    for(i=0;i<769;i++) input[i]=i*0x1020304u+0x7fc12345u;
    for(l=0;l<sizeof loads/sizeof loads[0];l++) for(p=0;p<4;p++)
    for(n=0;n<sizeof sizes/sizeof sizes[0];n++) {
        reset(loads[l],p);
        nv2a_vsh_constants(&v0);
        for(i=0;i<sizes[n];i++) nv2a_vsh_constant_word(input[i]);
        /* A following scalar word makes a wrong upload cursor visible. */
        nv2a_vsh_constant_word(0x89abcdefu);
        memcpy(expected,nv2a_vsh_constants(&v1),sizeof expected);
        reset(loads[l],p);
        nv2a_vsh_constants(&v2);
        nv2a_vsh_constant_words(input,sizes[n]);
        nv2a_vsh_constant_word(0x89abcdefu);
        if(memcmp(expected,nv2a_vsh_constants(&v3),sizeof expected) || v1-v0!=v3-v2) {
            fprintf(stderr,"FAIL load=%u prefix=%u count=%u revisions=%u/%u\n",loads[l],p,sizes[n],v1-v0,v3-v2);
            return 1;
        }
        cases++;
    }
    printf("constant upload scalar/bulk: %u cases passed\n",cases);
    return 0;
}
