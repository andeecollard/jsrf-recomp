"""Add/remove the opt-in sub_001A3E58 voice-submission probe in a generated
checkout.

The APU model never sees NV1BA0_PIO_VOICE_ON (on=0 for a whole gameplay run),
yet the store that issues it is present in the generated code. This installs
read-only observation points along the one path that reaches it, so a run says
which of the path's four stopping points is the one being hit.

Nothing here changes a branch, a register or a byte of guest memory: each site
is a call inserted immediately after an existing generated label, taking
register values and read-only guest memory as arguments.

Unlike instrument_startup.py the generated partition is not hardcoded. The
recompiler moves functions between recomp_NNNN.c between generations, and this
script is meant to survive that: it finds whichever file holds the label.
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

# guest pc -> argument expression, in the order the path executes them.
#
# sub_001A3E58 keeps the voice object in esi (from ecx at entry) for its whole
# body, edi is the descending voice index and ebx walks the handle array at
# esi+0xC. 0xFE820010 is NV1BA0_PIO_FREE, which both spins compare against.
POINTS = {
    # Entry. Nothing is pushed yet, so [esp] is the return address, and the
    # entry gate's own input -- word [ecx+0x12] -- is still untouched.
    '001A3E58': 'ecx, MEM32(esp), 0',
    # Past the gate and back from sub_001A308E (a manual override).
    '001A3E74': 'esi, 0, 0',
    # First PIO_FREE spin: waits for (FREE & ~3) >= 0x80.
    '001A3EB3': 'esi, MEM32(0xFE820010u), 0x80',
    '001A3EC2': 'esi, MEM32(0xFE820010u), 0x80',
    # The voice count at esi+0x64 is read in this block; eax becomes count * 7.
    '001A3F17': 'esi, 0, 0',
    # Second PIO_FREE spin: waits for (FREE >> 2) >= count * 7, which eax holds.
    '001A3F24': 'esi, MEM32(0xFE820010u), eax',
    # Count was nonzero, so the submission loop runs at least once.
    '001A3F36': 'esi, edi, 0',
    '001A3F3B': 'esi, edi, ebx',
    # The last label before the store at 0x001A3F8D. ecx already holds the
    # VOICE_ON argument and [ebp-8] the SET_ANTECEDENT_VOICE one.
    '001A3F7A': 'ecx, MEM32(ebp + -8), edi',
    # Loop finished, or was skipped because the count was zero.
    '001A3FA9': 'esi, 0, 0',
    # A third PIO_FREE spin, after the loop -- a thread wedged here has already
    # issued its voices.
    '001A3FDB': 'esi, MEM32(0xFE820010u), 0x80',
    '001A3FEA': 'esi, MEM32(0xFE820010u), 0x80',
    # Shared epilogue: reached by the early-out and by the normal path alike.
    '001A4000': 'esi, 0, 0',
}
MARKER = '/* VOICE_SUBMIT_OBSERVATION */'

installed = removed = 0
for pc, args in POINTS.items():
    label = f'loc_{pc}: ;'
    call = f'\n    jsrf_voice_submit_probe(0x{pc}u, {args}); {MARKER}'
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
    print(f'{action} {len(POINTS)} voice-submission observation sites '
          f'({removed} pre-existing removed)')
