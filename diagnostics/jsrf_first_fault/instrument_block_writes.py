"""Make `rep movs` / `rep stos` visible to RECOMP_MEM_WATCH.

The memory watch only sees stores routed through the RECOMP_MEM_WRITE* macros.
String operations are lifted to memcpy and to raw MEM32/byte loops that bypass
them, so every block copy and fill in the title is invisible to it -- which is
why a watch on the XPP list head at 0x002648D4 reported nothing while the value
demonstrably changed underneath it (its own old= fields proved it).

This inserts one call before each lifted string operation, naming the enclosing
guest function and the destination range. The runtime side
(recomp_mem_watch_guest_block) prints only when the range meets the watched
window, so with RECOMP_MEM_WATCH unset the cost is a load and a branch.

It does NOT report old/new values -- the point is to name the writer, and the
ordinary per-store lines already carry values.

Read-only, and installed into a COPY of a gen tree; no regeneration, which
matters because regeneration is not bit-stable across translator changes and
every preserved result here came from a specific tree.

Two shapes are lifted, both matched on their distinctive text rather than on
line position:

  stos   { uint32_t _i; int32_t _st = RECOMP_DF_STEP(W); for (...) MEM..(edi ...
  movs   if (!g_df) { uint8_t *_d = ... XBOX_PTR(edi) ...; uint32_t _n = ecx * W;

The length is recomputed from ecx at the call rather than read from _n, because
_n is not in scope until after the line being annotated.
"""
import argparse
import re
import sys
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--remove', action='store_true')
p.add_argument('--check', action='store_true')
p.add_argument('--gen', type=Path, required=True)
a = p.parse_args()

MARKER = '/* BLOCK_WRITE_OBSERVATION */'
FUNC_RE = re.compile(r'^void (sub_[0-9A-Fa-f]{8})\(void\)')
STOS_RE = re.compile(r'RECOMP_DF_STEP\((\d)\)')
MOVS_RE = re.compile(r'uint32_t _n = ecx(?: \* (\d))?;')

installed = removed = files = 0
for f in sorted(a.gen.glob('recomp_*.c')):
    src = f.read_text()
    if a.remove or a.check:
        n = src.count(MARKER)
        if a.check:
            if n:
                print(f'{f.name}: {n} sites')
            removed += n
            continue
        if not n:
            continue
        src = re.sub(r'^ *recomp_mem_watch_guest_block\([^;]*\); '
                     + re.escape(MARKER) + r'\n', '', src, flags=re.M)
        f.write_text(src)
        removed += n
        files += 1
        continue

    out, func, changed = [], '0x00000000', 0
    for line in src.split('\n'):
        m = FUNC_RE.match(line)
        if m:
            func = '0x' + m.group(1)[4:]
        if MARKER not in line:
            width = None
            ms = STOS_RE.search(line)
            if ms and 'for (_i = 0; _i < ecx; _i++)' in line:
                width = int(ms.group(1))
            else:
                mm = MOVS_RE.search(line)
                if mm and 'XBOX_PTR(edi)' in line:
                    width = int(mm.group(1)) if mm.group(1) else 1
            if width is not None:
                indent = line[:len(line) - len(line.lstrip())]
                out.append(f'{indent}recomp_mem_watch_guest_block({func}u, edi,'
                           f' ecx * {width}u); {MARKER}')
                changed += 1
        out.append(line)
    if changed:
        f.write_text('\n'.join(out))
        installed += changed
        files += 1

if a.check:
    print(f'{removed} observation sites installed')
elif a.remove:
    print(f'Removed {removed} block-write observation sites from {files} files')
else:
    if not installed:
        sys.exit('no string operations matched -- the lifter output has changed '
                 'shape; update the two patterns in this script')
    print(f'Installed {installed} block-write observation sites in {files} files')
