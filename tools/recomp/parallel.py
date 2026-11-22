"""Spawn-safe body lifting from the parent's finalized analysis snapshot.

Only independent body emission runs here. Boundary recovery, manual-name
binding, entry stubs and output publication remain in BatchTranslator.
"""
from collections import deque
from concurrent.futures import ProcessPoolExecutor
from contextlib import redirect_stderr
import io
import multiprocessing
import sys

_translator = None
_warning_seen = None


def _initialize(state, config_state, hooks):
    from . import config
    from . import translator as module
    from .disasm import Disassembler
    vars(config).update(config_state)
    module.ENTRY_HOOKS = set(hooks)
    global _translator, _warning_seen
    _translator = module.FunctionTranslator.__new__(module.FunctionTranslator)
    _translator.__dict__.update(state)
    # Capstone contains a native handle. Construct it in the child, never pickle it.
    _translator.disasm = Disassembler()
    _warning_seen = {name: set(getattr(_translator, name))
                     for name in ('_unreadable_reported', '_teardown_reported')}


def _lift_group(items):
    records = []
    for address, info in items:
        diagnostics = io.StringIO()
        with redirect_stderr(diagnostics):
            lifter = _translator.lifter
            # These are reporting accumulators, never inputs to body emission.
            # Per-function tallies also bound the fixed-point probe's copies.
            lifter.referenced_calls = {}
            lifter.unimplemented = {}
            _translator._warning_events = []
            for name, seen in _warning_seen.items():
                setattr(_translator, name, set(seen))
            code = _translator.translate_function(address, info)
        records.append((address, code, lifter.referenced_calls, lifter.unimplemented,
                        _translator._warning_events, diagnostics.getvalue()))
    return records


def body_results(translator, items, workers, hooks):
    """Yield results in input order, with at most 2*workers groups in flight.

    Use spawn explicitly on every host so Linux tests exercise the same state
    transfer as Windows, rather than accidentally inheriting process globals.
    Exceptions propagate; never silently publish a partially parallel result.
    """
    from . import config
    state = {name: value for name, value in vars(translator).items() if name != 'disasm'}
    config_state = {name: value for name, value in vars(config).items()
                    if name.isupper() or name in {'_SECTIONS', '_configured_from', '_game_name_explicit'}}
    # Contiguous batches amortize IPC while preserving all aggregate order.
    groups = iter([items[i:i + 32] for i in range(0, len(items), 32)])
    workers = min(workers, max(1, (len(items) + 31) // 32))
    with ProcessPoolExecutor(max_workers=workers, mp_context=multiprocessing.get_context('spawn'),
                             initializer=_initialize, initargs=(state, config_state, hooks)) as pool:
        pending = deque()
        for _ in range(workers * 2):
            group = next(groups, None)
            if group is None:
                break
            pending.append(pool.submit(_lift_group, group))
        while pending:
            records = pending.popleft().result()
            for address, code, references, unimplemented, warnings, diagnostics in records:
                for seen_name, key, message in warnings:
                    translator._warn_once(seen_name, key, message)
                if diagnostics:
                    sys.stderr.write(diagnostics)
                translator.lifter.referenced_calls.update(references)
                for mnemonic, addresses in unimplemented.items():
                    translator.lifter.unimplemented.setdefault(mnemonic, []).extend(addresses)
                yield address, code
            group = next(groups, None)
            if group is not None:
                pending.append(pool.submit(_lift_group, group))
