"""Observe every D3D render state the title sets, to find the intro fade's.

G1's remaining question is guest-side: xemu's combiner factor ramps through
107 distinct values across the intro cards and ours holds one, and nothing in
the Opening object could produce a ramp -- its fields are a card index, a frame
timer, a skip flag, two FileGet handles, a language id and a chapter number.

CMGameGL::setRenderState is the funnel, and it is reached only through a
vtable, so the call graph cannot say who calls it. This records the state, the
value and the RETURN ADDRESS, which is what names the caller at runtime. Pipe
the log through symbolize.py to read those addresses.

Read-only, and it does not regenerate: probe calls are inserted after the
entry label of an existing generated checkout, the way instrument_startup.py
does it.

    python3 instrument_render_state.py            # install
    python3 instrument_render_state.py --remove   # take it back out

    RECOMP_RSTATE_TRACE=1 [RECOMP_RSTATE_REPORT_MS=5000] <binary>
"""
import argparse
import re
from pathlib import Path

p = argparse.ArgumentParser(description=__doc__,
                            formatter_class=argparse.RawDescriptionHelpFormatter)
p.add_argument('--remove', action='store_true')
p.add_argument('--gen', type=Path,
               default=Path(__file__).resolve().parents[2]
               / 'build-macos/jsrf-first-fault/gen')
a = p.parse_args()

# At the entry label nothing has been pushed yet, so the __stdcall frame is
# still intact: esp+0 return address, esp+4 this, esp+8 state, esp+0xC value.
# Reading it here rather than after the prologue is what keeps the offsets
# stable if the generated prologue changes.
points = {
    'recomp_0007.c': {
        # CMGameGL::setRenderState. Measured NEVER CALLED across a 45 s run
        # that drew 102000 batches, so CMGameGL is not the retail render path
        # -- CMGameGLFont::draw(x, y, str) sits beside it, which reads as a
        # debug overlay. Kept installed deliberately: it is the negative half
        # of the pair, and a negative is only worth having next to a positive.
        '001504D0': ('jsrf_render_state_probe',
                     'MEM32(esp + 8), MEM32(esp + 0xC), MEM32(esp)'),
    },
    'recomp_0002.c': {
        # Opening::Exec0Default and Opening::drawDefault, both __thiscall, so
        # ecx is `this` at the entry label before any push.
        '0007E360': ('jsrf_opening_probe',
                     'ecx, MEM32(ecx + 0x98), MEM32(ecx + 0x9C), '
                     'MEM32(ecx + 0xA0)'),
        '0007E550': ('jsrf_opening_probe',
                     'ecx, MEM32(ecx + 0x98), MEM32(ecx + 0x9C), '
                     'MEM32(ecx + 0xA0)'),
    },
}

MARKER = '/* RENDER_STATE_OBSERVATION */'
changed = 0
for filename, file_points in points.items():
    f = a.gen / filename
    if not f.exists():
        raise SystemExit(f'no {f} -- point --gen at a generated checkout')
    s = f.read_text()
    for pc, (func, args) in file_points.items():
        label = f'loc_{pc}: ;'
        call = f'\n    {func}(0x{pc}u, {args}); {MARKER}'
        count = s.count(label)
        if count != 1:
            raise SystemExit(f'expected 1 copy of {label} in {f}, found {count}')
        # Remove first so installing twice is a no-op rather than a double call.
        s = s.replace(label + call, label)
        if not a.remove:
            s = s.replace(label, label + call)
        changed += 1
    f.write_text(s)

print(f'{"Removed" if a.remove else "Installed"} {changed} render-state '
      f'observation site(s) in {a.gen}')
