"""Only original, proven table ordinals may rescue a corrupted runtime slot."""
from .test_switch_table_offset_index import _image, _translate, BASE, TABLE, ARMS
from .test_lifter_result_clobber import _build_and_run
from .lifter import Lifter
from .disasm import Operand
from .test_icall_guarded_runtime import _macro, HARNESS


def test_compiled_original_positive_and_negative_slots():
    prelude=r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
uint32_t eax,ecx,edx,esp,g_seh_ebp,ebp,g_ebp;
uint8_t image[512];
unsigned failed,site;
#define MEM32(a) (*(uint32_t*)(image+(uint32_t)(a)-0x10000u))
#define MEM16(a) (*(uint16_t*)(image+(uint32_t)(a)-0x10000u))
#define MEM8(a) (*(uint8_t*)(image+(uint32_t)(a)-0x10000u))
#define LO8(r) ((uint8_t)(r))
#define HI8(r) ((uint8_t)((r)>>8))
#define LO16(r) ((uint16_t)(r))
#define SET_LO8(r,v) ((r)=((r)&0xffffff00u)|(uint8_t)(v))
#define SET_LO16(r,v) ((r)=((r)&0xffff0000u)|(uint16_t)(v))
#define RECOMP_ITAIL_AT(a,s) do{failed++;site=s;}while(0)
'''
    functions, checks=[],[]
    cases=(('normal',bytes.fromhex('83e003'),TABLE,0),
           ('positive',bytes.fromhex('83e003'),TABLE-4,1),
           ('negative',bytes.fromhex('83e904'),TABLE+12,-3))
    for tag,prefix,disp,first in cases:
        body=_image(prefix,disp)
        functions.append(_translate(body).replace('sub_00010000','switch_'+tag))
        initializer=','.join(str(b) for b in body)
        setup=f'{{const uint8_t original[]={{{initializer}}};memcpy(image,original,sizeof original);}}'
        for ordinal in range(3):
            index=first+ordinal
            incoming=index+4 if tag=='negative' else index
            # A corrupted target may use a proven index. A valid runtime arm wins.
            for runtime,want in ((0xdeadbeef,ordinal+1),(ARMS[2],3)):
                checks.append(setup+f'MEM32(0x{TABLE+ordinal*4:X})=0x{runtime:X}u;'
                    +f'eax=ecx={incoming}u;esp=0x1000;failed=0;switch_{tag}();'
                    +f'if(eax!={want}||failed||esp!=0x1004)return 1;')
        # Outside the proven ordinal set must retain the tail diagnostic path.
        outside=3 if tag=='normal' else 0
        checks.append(setup+f'eax=ecx={outside};esp=0x1000;failed=0;switch_{tag}();'
            +f'if(failed!=1||site!=0x{BASE+3:X})return 2;')
    ran=_build_and_run(prelude+'\n'.join(functions)+'int main(void){'+''.join(checks)+'return 0;}')
    assert ran.returncode==0,ran.stdout+ran.stderr


def test_ambiguous_shapes_and_changed_census_refuse_index_rescue():
    lifter=Lifter()
    for op in (Operand(type='mem',mem_index='eax',mem_scale=2,mem_disp=TABLE,mem_size=4),
               Operand(type='mem',mem_base='eax',mem_disp=TABLE,mem_size=4),
               Operand(type='mem',mem_index='eax',mem_scale=4,mem_disp=TABLE,mem_size=2)):
        assert lifter._switch_index_pairs(op,ARMS)==[]
    op=Operand(type='mem',mem_index='eax',mem_scale=4,mem_disp=TABLE,mem_size=4)
    assert lifter._switch_index_pairs(op,ARMS*22)==[]


def test_tail_site_shared_across_translation_units_and_cleared_after_dispatch():
    # Compile the actual macros in one unit and observe the TLS variable from another.
    source=HARNESS.split('IS_CODE')[0].replace('uint32_t g_esp, eax;', 'uint32_t g_esp, eax;\n#define g_eax eax')
    source+='\nextern uint32_t read_site(void);\n'
    source+=_macro('RECOMP_ITAIL')+'\n'+_macro('RECOMP_ITAIL_AT')+'\n'
    source=source.replace('void recomp_icall_fail_log(uint32_t va) { (void)va; fails++; }',
        'void recomp_icall_fail_log(uint32_t va) { (void)va; fails++; if(read_site()!=0x10040) fails+=100; }')
    source='''#if defined(_MSC_VER)
#define TLS __declspec(thread)
#else
#define TLS __thread
#endif
'''+source
    source=source.replace('extern uint32_t read_site(void);','extern uint32_t read_site(void);\nextern TLS uint32_t g_itail_site;')
    # The earlier callback also needs the accessor prototype.
    source=source.replace('#include <stdio.h>','#include <stdio.h>\nuint32_t read_site(void);')
    source+='''
int main(void){g_esp=0x1000; RECOMP_ITAIL_AT(0x11000,0x10040);
if(g_esp!=0x1004||called_a!=1||read_site())return 1;
g_esp=0x1000; RECOMP_ITAIL_AT(0x11300,0x10040);
return g_esp!=0x1004||fails!=1||read_site()!=0;}
'''
    other='''#include <stdint.h>
#if defined(_MSC_VER)
#define TLS __declspec(thread)
#else
#define TLS __thread
#endif
TLS uint32_t g_itail_site;
uint32_t read_site(void){return g_itail_site;}
'''
    ran=_build_and_run(source,{'site.c':other})
    assert ran.returncode==0,ran.stdout+ran.stderr
