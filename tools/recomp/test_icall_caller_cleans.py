"""Classify decoded caller cleanup and execute failed/guarded stack behavior."""
import pytest
from tools.recomp import config
from tools.recomp.translator import FunctionTranslator
from tools.recomp.test_icall_guarded_runtime import _macro, HARNESS, A, B
from tools.recomp.test_lifter_result_clobber import _build_and_run

BASE = 0x10000
BODY = '6a01 6a02 ffd0 83c408 c3'


def translate(body=BODY, guarded=False):
    image = bytes.fromhex(body)
    config._install([config.Section('.text', BASE, 0x2000, 0, len(image), True)],
                    entry_point=BASE, kernel_thunk_addr=BASE, origin='cleanup-test')
    db = {BASE: {'end': BASE + len(image), 'size': len(image)},
          A: {'end': A+1, 'size': 1, 'name': 'sub_A'},
          B: {'end': B+1, 'size': 1, 'name': 'sub_B'}}
    return FunctionTranslator(image, db, icall_sites={BASE+4: [A, B]} if guarded else {}).translate_function(BASE, db[BASE])


@pytest.mark.parametrize('guarded', [False, True])
def test_actual_add_snapshots_do_not_hide_cleanup(guarded):
    code = translate(guarded=guarded)
    assert '/* add source, before the write */' in code
    assert 'RECOMP_ICALL_SAFE_AT_CC(_icall_target' in code
    assert '_icall_esp = g_esp' in code
    assert code.count('{') == code.count('}')


@pytest.mark.parametrize('body', ['ffd0 31c9 83c408 c3', 'ffd0 c20400', 'ffd0 eb00 83c408 c3'])
def test_non_immediate_cleanup_is_not_assumed(body):
    code = translate(body)
    assert 'RECOMP_ICALL_SAFE_AT(' in code
    assert 'RECOMP_ICALL_SAFE_AT_CC(' not in code


@pytest.mark.parametrize('guarded', [False, True])
def test_actual_translated_call_restores_stack_for_every_target(guarded):
    source = HARNESS.split('IS_CODE')[0]
    source += _macro('RECOMP_ICALL_IS_CODE') + '\n'
    source += _macro('RECOMP_ICALL_SAFE_AT_CC') + '\n'
    source += translate(guarded=guarded)
    source += '''
int main(void) {
    const uint32_t targets[] = {0x11000, 0x11100, 0x11200, 0x11300, 0xF00000};
    for(unsigned i=0;i<5;i++) {
        g_esp=0x1000; eax=targets[i];
        hits=misses=lookups=fails=called_a=called_b=called_c=0;
        sub_00010000();
        if(g_esp!=0x1004) return 1;
        if(i<3 && called_a+called_b+called_c!=1) return 2;
        if(i>=3 && fails!=1) return 3;
        if(GUARDED && i<2 && (hits!=1 || lookups!=0)) return 4;
        if(GUARDED && i>=2 && misses!=1) return 5;
    }
    return 0;
}
'''.replace('GUARDED', str(int(guarded)))
    ran = _build_and_run(source)
    assert ran.returncode == 0, ran.stdout + ran.stderr
