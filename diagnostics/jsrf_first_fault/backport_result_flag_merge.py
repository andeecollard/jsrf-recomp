"""Backport the result-setter flag merge into the preserved JSRF tree.

``_merge_predecessor_flag_states`` used to give up whenever the incoming edges
set flags with different instructions, and the lifter then emitted its dead
``_flags`` fallback -- a local initialised to zero and never written, so the
branch could only ever go one way.  It now merges edges that all wrote the same
destination, because those agree on ZF and SF whatever the instruction was.

MSVC's signed remainder by a power of two produces exactly that join:

    and eax, 0x800007FF
    jns  L
    dec  eax ; or eax, 0xFFFFF800 ; inc eax
L:  jne  ...

JSRF runs it twice inside sub_001403B0 -- wxCiReqRd, the WXCI/XB DVD
sector-read request -- on the read length at 0x001404DB and the seek position
at 0x001404F1.  With the second ``je`` nailed not-taken every read request the
title issued fell straight into "E0109152:illegal seek position." and returned
zero.

A full regeneration is avoided here for the same reason
``backport_flag_merge.py`` avoids it: this generated checkout carries recovered
mid-function entries, shared epilogues and startup entries that regeneration
does not reproduce on its own.  So rather than hand-writing C, this runs the
fixed translator over each affected function and copies its statement across,
keyed on the guest block label, the mnemonic and the branch target.  It is
idempotent; ``--check`` is the build-tree gate.
"""

import argparse
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from tools.recomp import config                      # noqa: E402
from tools.recomp import translator as translator_module  # noqa: E402
from tools.recomp.translator import BatchTranslator  # noqa: E402

FUNC = re.compile(r'^void (sub_([0-9A-Fa-f]+))\(void\)')
LABEL = re.compile(r'^\s*loc_([0-9A-Fa-f]{8}):')
# Both shapes the lifter emits for a conditional transfer: a local goto, and
# the frame-bridged tail call it uses when the target is another function.
GOTO = re.compile(r'goto loc_([0-9A-Fa-f]{8});')
TAIL = re.compile(r'\{ g_seh_ebp = ebp; (\w+)\(\); return; \}')
MNEMONIC = re.compile(r'/\* (\w+):')


def sites(lines):
    """Map every conditional transfer to (block, mnemonic, target, nth)."""
    found = {}
    block = None
    seen = {}
    for index, line in enumerate(lines):
        label = LABEL.match(line)
        if label:
            block = label.group(1)
            continue
        if 'if (' not in line:
            continue
        target = GOTO.search(line) or TAIL.search(line)
        mnemonic = MNEMONIC.search(line)
        if not target or not mnemonic:
            continue
        key = (block, mnemonic.group(1), target.group(1))
        key += (seen.get(key, 0),)
        seen[key[:3]] = key[3] + 1
        found[key] = index
    return found


def bodies(text):
    """Split a generated file into {function address: (first, last)} lines."""
    lines = text.splitlines()
    spans, name, start = {}, None, None
    for index, line in enumerate(lines):
        match = FUNC.match(line)
        if match:
            if name is not None:
                spans[name] = (start, index)
            name, start = int(match.group(2), 16), index
        elif line == '}' and name is not None:
            spans[name] = (start, index)
            name = None
    if name is not None:
        spans[name] = (start, len(lines))
    return lines, spans


def dead(line):
    return 'if (_flags' in line


p = argparse.ArgumentParser(
    description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument('--check', action='store_true',
               help='report without writing; exit 1 if any site is unfixed')
p.add_argument('--scope', choices=('result-merge', 'all'),
               default='result-merge',
               help='result-merge (default) applies only the sites the new '
                    'destination-writing join resolves, so a run tests one '
                    'hypothesis; all also applies sites the older cmp/test '
                    'snapshot merge resolves but that were never backported')
p.add_argument('--gen', type=Path,
               default=Path(__file__).resolve().parents[2]
               / 'build-macos/jsrf-first-fault/gen')
p.add_argument('--xbe', type=Path,
               default=Path(__file__).resolve().parents[3]
               / 'Jet Set Radio Future (US)/default.xbe')
p.add_argument('--disasm-dir', type=Path,
               default=Path(__file__).resolve().parents[2]
               / 'build-macos/jsrf-first-fault/disasm')
p.add_argument('--func-id-dir', type=Path,
               default=Path(__file__).resolve().parents[2]
               / 'build-macos/jsrf-first-fault/func-id')
a = p.parse_args()

# Which functions still carry a dead fallback. Only those are re-translated:
# loading the database and rediscovering CFG ownership costs half a minute, and
# translating all ten thousand functions would defeat the point of a backport.
targets = {}
for path in sorted(a.gen.glob('recomp_*.c')):
    lines, spans = bodies(path.read_text())
    for addr, (first, last) in spans.items():
        if any(dead(line) for line in lines[first:last]):
            targets.setdefault(addr, []).append(path)

if not targets:
    print('no dead _flags fallbacks remain')
    sys.exit(0)

config.configure_from_xbe(str(a.xbe))
translator = BatchTranslator(
    xbe_path=str(a.xbe),
    func_json_path=str(a.disasm_dir / 'functions.json'),
    labels_json_path=str(a.disasm_dir / 'labels.json'),
    identified_json_path=str(a.func_id_dir / 'identified_functions.json'),
    output_dir=None,
)

RESULT_SETTERS = translator_module._RESULT_ZF_SF_SETTERS


def translate(addr, result_merge=True):
    """Translate one function, optionally with the new join suppressed.

    Emptying the setter class is exactly and only what disables the
    destination-writing join; every other merge rule is untouched, so a
    statement that differs between the two passes differs because of it.
    """
    translator_module._RESULT_ZF_SF_SETTERS = (
        RESULT_SETTERS if result_merge else frozenset())
    try:
        return translator.translate_single(addr)
    except Exception as error:                        # noqa: BLE001
        print(f'  0x{addr:08X}: re-translation failed: {error}',
              file=sys.stderr)
        return None
    finally:
        translator_module._RESULT_ZF_SF_SETTERS = RESULT_SETTERS


fixed = unresolved = unmatched = skipped = 0
for path in sorted({p for paths in targets.values() for p in paths}):
    lines, spans = bodies(path.read_text())
    changed = False
    for addr, (first, last) in sorted(spans.items()):
        old = lines[first:last]
        if not any(dead(line) for line in old):
            continue
        fresh = translate(addr)
        new = fresh.splitlines() if fresh else []
        new_sites = sites(new)
        if a.scope == 'result-merge':
            baseline = translate(addr, result_merge=False)
            base = baseline.splitlines() if baseline else []
            base_sites = sites(base)
        else:
            base, base_sites = [], {}
        for key, index in sites(old).items():
            if not dead(old[index]):
                continue
            match = new_sites.get(key)
            if match is None:
                unmatched += 1
                print(f'  0x{addr:08X} {key[0]} {key[1]} -> {key[2]}: '
                      f'no matching statement in the re-translation')
                continue
            if dead(new[match]):
                unresolved += 1
                continue
            if a.scope == 'result-merge':
                other = base_sites.get(key)
                if other is not None and not dead(base[other]):
                    # The older cmp/test merge already resolved this one; it is
                    # a separate, already-regressed fix and does not belong in
                    # a run meant to test the new join.
                    skipped += 1
                    continue
            indent = old[index][:len(old[index]) - len(old[index].lstrip())]
            lines[first + index] = indent + new[match].strip()
            fixed += 1
            changed = True
            print(f'  0x{addr:08X} loc_{key[0]} {key[1]} -> loc_{key[2]}: '
                  f'{new[match].strip()}')
    if changed and not a.check:
        path.write_text('\n'.join(lines) + '\n')

print(f'{fixed} site(s) resolved, {skipped} left to the older merge, '
      f'{unresolved} still unresolvable by the lifter, {unmatched} unmatched')
if a.check and fixed:
    print(f'FAIL: {fixed} site(s) the fixed lifter resolves are still dead '
          f'in {a.gen}', file=sys.stderr)
    sys.exit(1)
