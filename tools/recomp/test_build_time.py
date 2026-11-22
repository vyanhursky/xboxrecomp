"""Independent x86 fixtures for spawn parity and bounded source chunking."""
import copy
import sys

import pytest

from . import config
from . import translator as module
from .translator import BatchTranslator, FunctionTranslator, split_translations


def _subject(monkeypatch):
    base, stride, count = 0x500000, 32, 100
    monkeypatch.setattr(config, '_SECTIONS', [config.Section('.text', base, 0x4000, 0, 0x4000, True)])
    # These deliberately differ from the fallback globals a spawned import sees.
    monkeypatch.setattr(config, 'TEXT_VA_START', base)
    monkeypatch.setattr(config, 'TEXT_VA_END', base + 0x4000)
    monkeypatch.setattr(module, 'ENTRY_HOOKS', {base})
    image = bytearray(b'\xcc' * 0x4000)
    db = {}
    for index in range(count):
        address = base + index * stride
        target = base + ((index + 1) % count) * stride
        # call next; dec ecx; jnz dec; arpl ax,ax (unsupported); ret.
        body = b'\xe8' + (target - address - 5).to_bytes(4, 'little', signed=True)
        body += bytes.fromhex('4975fd63c0c3')
        image[index * stride:index * stride + len(body)] = body
        db[address] = {'start': hex(address), 'end': address + len(body),
                       'size': len(body), '_addr': address, 'name': f'sub_{address:08X}'}
    db[base + stride * 2]['name'] += '_gen'
    db[base + stride * 2]['wrapper_name'] = f'sub_{base + stride * 2:08X}'
    # Referenced code with no declared function exercises parent entry stubs.
    unknown = base + 0x2000
    image[0x2000] = 0xc3
    image[5 * stride + 1:5 * stride + 5] = (unknown - (base + 5 * stride) - 5).to_bytes(4, 'little', signed=True)
    batch = BatchTranslator.__new__(BatchTranslator)
    batch.func_db = db
    batch.abi_db = {}
    batch.title = 'Independent build fixture'
    batch.translator = FunctionTranslator(bytes(image), db, trace_functions={base + stride})
    batch.translator.back_edge_yield = True
    batch.entry_stubs = True
    return batch, list(db.items()), {base + stride * 3}


@pytest.mark.parametrize('workers', [2, 3])
def test_spawn_output_and_reports_match_serial(tmp_path, monkeypatch, workers):
    batch, functions, manual = _subject(monkeypatch)
    serial = copy.deepcopy({k: v for k, v in vars(batch.translator).items() if k != 'disasm'})
    first = batch.translate_batch_split(functions, tmp_path / 'serial', chunk_size=17, manual=manual)
    serial_calls = dict(batch.translator.lifter.referenced_calls)
    batch.translator = FunctionTranslator.__new__(FunctionTranslator)
    batch.translator.__dict__.update(serial)
    from .disasm import Disassembler
    batch.translator.disasm = Disassembler()
    second = batch.translate_batch_split(functions, tmp_path / 'parallel', chunk_size=17,
                                         manual=manual, workers=workers)
    files = lambda root: {p.name: p.read_bytes() for p in root.iterdir()}
    assert files(tmp_path / 'serial') == files(tmp_path / 'parallel')
    for key in ('total', 'translated', 'failed', 'total_lines', 'unresolved_stubs', 'manual_functions', 'unimplemented'):
        assert first[key] == second[key]
    assert first['unimplemented']  # The test actually exercises tally transfer.
    assert serial_calls == batch.translator.lifter.referenced_calls
    chunk = (tmp_path / 'parallel/recomp_0000.c').read_text(encoding='utf-8')
    assert 'sub_00500000_enter();' in chunk
    assert 'RECOMP_BACK_EDGE' in chunk
    assert 'sub_00500040_gen' in chunk


def test_line_budget_keeps_order_and_never_splits_a_function():
    entries = [(index, str(index), 'x\n' * lines) for index, lines in enumerate([2, 3, 40, 1, 2])]
    chunks = split_translations(entries, 3, 10)
    assert [entry for chunk in chunks for entry in chunk] == entries
    assert [len(chunk) for chunk in chunks] == [2, 1, 2]
    assert split_translations(entries, 2) == [entries[:2], entries[2:4], entries[4:]]
    assert split_translations([], 2, 10) == []
    with pytest.raises(ValueError):
        split_translations(entries, 0)


def test_line_rechunking_preserves_bodies_and_removes_stale_chunks(tmp_path, monkeypatch):
    batch, functions, manual = _subject(monkeypatch)
    old = batch.translate_batch_split(functions, tmp_path, chunk_size=2, manual=manual)
    (tmp_path / 'recomp_10000.c').write_text('stale duplicate definition', encoding='utf-8')
    (tmp_path / 'recomp_other.c').write_text('unrelated', encoding='utf-8')
    new_batch, functions, manual = _subject(monkeypatch)
    new = new_batch.translate_batch_split(functions, tmp_path, chunk_size=1000,
                                          chunk_lines=1000, manual=manual)
    assert new['num_chunks'] < old['num_chunks']
    assert len(list(tmp_path.glob('recomp_[0-9][0-9][0-9][0-9].c'))) == new['num_chunks']
    assert not (tmp_path / 'recomp_10000.c').exists()
    assert (tmp_path / 'recomp_other.c').read_text() == 'unrelated'


def test_cli_refuses_unused_parallel_options():
    import subprocess
    result = subprocess.run([sys.executable, '-m', 'tools.recomp', 'unused.xbe', '--workers', '2'],
                            capture_output=True, text=True)
    assert result.returncode == 2
    assert 'require --split' in result.stderr
