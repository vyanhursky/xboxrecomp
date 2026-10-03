"""Run real copy emission through an access-counting guest-memory fixture."""
from tools.recomp.lifter import Lifter
from tools.recomp.test_lifter_result_clobber import _build_and_run


def test_mmio_overlap_and_direction_at_every_element_width():
    prelude=r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
uint32_t esi,edi,ecx;
int g_df;
unsigned accesses;
uint8_t mem[131072],refmem[131072];
unsigned index(uint32_t a){return (a&0xffffu)+(a>=0xFD000000u?65536u:0u);}
void* access(uint32_t a){accesses++;return mem+index(a);}
#define XBOX_PTR(a) (mem+index(a))
#define MEM8(a) (*(volatile uint8_t*)access(a))
#define MEM16(a) (*(volatile uint16_t*)access(a))
#define MEM32(a) (*(volatile uint32_t*)access(a))
#define RECOMP_DF_STEP(s) (g_df?-(s):(s))
'''
    functions=[]
    for size in (1,2,4):
        functions.append(f'void copy_{size}(void){{'+''.join(Lifter()._lift_rep_movs(size,'fixture'))+'}')
    main=r'''
int main(void){
    void(*copies[])(void)={copy_1,copy_2,copy_4};
    const unsigned sizes[]={1,2,4};
    for(unsigned w=0;w<3;w++) for(unsigned mode=0;mode<8;mode++) for(unsigned n=0;n<=8;n++){
        unsigned z=sizes[w];
        uint32_t s=0x100,d=0x400;
        int df=0;
        if(mode==1)d=s+z;                    /* forward propagation */
        if(mode==2){s+=8*z;d=s-z;df=1;}     /* backward overlap */
        if(mode==3)s=0xFD000100;
        if(mode==4)d=0xFD000400;
        if(mode==5){s=0xFD000100;d=0xFD000400;}
        if(mode==6)s=0xFD000000-2*z;        /* range crosses into hardware */
        if(mode==7)d=0xFD000000-2*z;
        for(unsigned i=0;i<sizeof mem;i++)mem[i]=refmem[i]=(uint8_t)(i*37+13);
        uint32_t rs=s,rd=d;
        for(unsigned i=0;i<n;i++){
            uint8_t element[4];
            memcpy(element,refmem+index(rs),z);memcpy(refmem+index(rd),element,z);
            rs+=df?-(int)z:z;rd+=df?-(int)z:z;
        }
        esi=s;edi=d;ecx=n;g_df=df;accesses=0;copies[w]();
        int fast=!df && s<0xFD000000u && d<0xFD000000u
                && (uint64_t)s+n*z<=0xFD000000u && (uint64_t)d+n*z<=0xFD000000u
                && ((uint64_t)d+n*z<=s || (uint64_t)s+n*z<=d);
        if(memcmp(mem,refmem,sizeof mem) || esi!=rs || edi!=rd || ecx!=0 || accesses!=(fast?0:2*n)){
            printf("size %u mode %u count %u access %u\n",z,mode,n,accesses);return 1;
        }
    }
    return 0;
}
'''
    ran=_build_and_run(prelude+'\n'.join(functions)+main)
    assert ran.returncode == 0,ran.stdout+ran.stderr
