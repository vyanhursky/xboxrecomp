"""`void sub_XXXXXXXX_enter(void)` in the manual file is an entry hook: found by
the scan, and neither an override nor a reference that pins a name."""
import os
import tempfile

from tools.recomp.manual_scan import entry_hooks, scan

SRC = """
void sub_0021EB90_enter(void)
{
}
#if 0
void sub_00111111_enter(void) { }
#endif
void sub_00216410(void) { }
"""


def _write(text):
    d = tempfile.mkdtemp()
    p = os.path.join(d, "recomp_manual.c")
    with open(p, "w", encoding="utf-8") as f:
        f.write(text)
    return p


def test_hook_is_found_and_disabled_ones_are_not():
    assert entry_hooks(_write(SRC)) == {0x0021EB90}


def test_hook_is_not_an_override():
    skip, wrap, referenced = scan(_write(SRC))
    assert skip == {0x00216410}
    assert not wrap
    assert 0x0021EB90 not in referenced


def test_hook_start_is_protected_before_boundary_repair():
    from tools.recomp.__main__ import _load_manual_protection
    protected, (skip, wrap, referenced) = _load_manual_protection(None, _write(SRC))
    assert 0x0021EB90 in protected
    assert 0x0021EB90 not in skip | wrap | referenced


def test_generated_hook_precedes_first_instruction(monkeypatch):
    from tools.recomp import config, translator
    base = 0x10000
    image = bytes.fromhex('b8 78 56 34 12 c3')
    config._install([config.Section('.text', base, len(image), 0, len(image), True)],
                    entry_point=base, kernel_thunk_addr=base, origin='hook-test')
    monkeypatch.setattr(translator, 'ENTRY_HOOKS', {base})
    db = {base: {'end': base + len(image), 'size': len(image)}}
    code = translator.FunctionTranslator(image, db).translate_function(base, db[base])
    assert code.count('sub_00010000_enter();') == 1
    assert code.index('sub_00010000_enter();') < code.index('eax = 0x12345678;')
