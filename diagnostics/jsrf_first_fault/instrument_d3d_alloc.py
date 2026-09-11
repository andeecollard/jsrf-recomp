"""Add/remove the opt-in D3D contiguous-allocation path probe.

MmAllocateContiguousMemoryEx (ordinal 166) is called 59 times on macOS and 0 on
Windows, and every one of those calls comes from sub_0018E670 or sub_00199760.
Both were read in full -- see
docs/jsrf/progress/CLAUDE_PROGRESS_2026-09-11_TWO_FUNCTIONS_READ.md -- and both
have the same shape, with exactly one branch ahead of the allocation:

    descriptor = sub_0014A83E(0x40, tag)    # the title's own heap
    if (!descriptor) return E_OUTOFMEMORY   # <-- allocation never attempted
    mem = (*(void**)0x001C40F8)(size, ...)  # thunk slot 102 = ordinal 166
    if (!mem) { free(descriptor); ... }

"Windows never allocates" therefore has two shapes, and they are separable at
one site each. Entry counters cannot separate them: sub_0014A83E and the free
are general-purpose and are called from all over the title, so their totals say
nothing about these two callers. Hence a path probe rather than another --va
list.

Read-only. Every site is a call inserted immediately after an existing
generated label, taking register values and guest memory as arguments; no
branch, register or byte of guest memory changes. Install into a COPY of a gen
tree, never the tree a preserved result came from.

Like instrument_voice_submit.py, the generated partition is not hardcoded --
functions move between recomp_NNNN.c across generations, so each label is
located wherever it currently lives, and every anchor is validated before any
file is written.
"""
import argparse
import sys
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--remove', action='store_true')
p.add_argument('--check', action='store_true',
               help='report what is installed and exit without writing')
p.add_argument('--gen', type=Path, required=True,
               help='generated source directory (RECOMP_GEN_DIR)')
a = p.parse_args()

# guest pc -> the two dwords worth recording there, in execution order.
#
# The register holding each value is read from the generated code at that
# label, BEFORE the label's own first statement runs -- so at loc_0018E6CB eax
# is still sub_0014A83E's return value and has not yet been copied to esi.
POINTS = {
    # --- sub_0018E670: descriptor tag 0x14, size computed by sub_00193380 ---
    '0018E670': '0, 0',
    # eax = the heap descriptor. ZERO HERE MEANS THE ALLOCATION IS NEVER TRIED.
    '0018E6CB': 'eax, 0',
    # About to call ordinal 166. edi = NumberOfBytes; the thunk word says
    # whether the slot resolves to a synthetic kernel VA at all.
    '0018E6D1': 'edi, MEM32(0x1C40F8u)',
    # eax = what ordinal 166 returned. Zero means allocation attempted, failed.
    '0018E6E9': 'eax, 0',
    '0018E6F3': '0, 0',
    '0018E6FE': 'eax, 0',
    # --- sub_00199760: descriptor tag 0x0C, size passed on the stack ---
    '00199760': '0, 0',
    '0019976A': 'eax, 0',
    # esp has four pushes on it here; [esp+8] is NumberOfBytes, which the
    # label's own first statement is about to load into eax.
    '00199770': 'MEM32(esp + 8), MEM32(0x1C40F8u)',
    '00199789': 'eax, 0',
    '00199793': '0, 0',
    '0019979C': 'eax, 0',
}
MARKER = '/* D3D_ALLOC_OBSERVATION */'

installed = removed = 0
for pc, args in POINTS.items():
    label = f'loc_{pc}: ;'
    call = f'\n    jsrf_d3d_alloc_probe(0x{pc}u, {args}); {MARKER}'
    hits = [f for f in sorted(a.gen.glob('recomp_*.c'))
            if label in f.read_text()]
    if len(hits) != 1:
        sys.exit(f'expected exactly one generated file with {label}, '
                 f'found {len(hits)}: {[h.name for h in hits]}')
    f = hits[0]
    s = f.read_text()
    if s.count(label) != 1:
        sys.exit(f'expected one copy of {label} in {f}, found {s.count(label)}')
    present = (label + call) in s
    if a.check:
        print(f'{pc} {f.name} {"installed" if present else "absent"}')
        continue
    if present:
        s = s.replace(label + call, label)
        removed += 1
    if not a.remove:
        s = s.replace(label, label + call)
        installed += 1
    f.write_text(s)

if not a.check:
    action = 'Removed' if a.remove else 'Installed'
    print(f'{action} {len(POINTS)} D3D allocation observation sites '
          f'({removed} pre-existing removed)')
