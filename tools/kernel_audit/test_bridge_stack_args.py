"""
A bridge may not read more stack than its arg-size entry says the caller pushed.

Run: py -3 -m pytest tools/kernel_audit/test_bridge_stack_args.py

Each bridge_<Name> reads its arguments with STACK_ARG(n), and
stdcall_args_for_ordinal() separately declares how many bytes the caller pushed
-- the count kernel_thunk_dispatch adds back to g_esp so the simulated stack
stays balanced. Nothing tied the two together, so a bridge could read stack the
caller never pushed and get whatever residue was sitting there.

That is not hypothetical. The Xbox kernel's "f"-suffixed exports are __fastcall:
KfRaiseIrql, KfLowerIrql, IofCallDriver, IofCompleteRequest,
ObfDereferenceObject. Their arguments arrive in ecx/edx and never touch the
stack, which is why their arg-size entries are 0 -- and the table said exactly
that, in a comment naming KfRaiseIrql/KfLowerIrql specifically. Both bridges
nevertheless read STACK_ARG(0).

Measured on JSRF: "KfLowerIrql: attempt to raise IRQL from 2 to 144" logged
every ~24ms. 144 is not a valid IRQL -- HIGH_LEVEL is 31 -- it was stack
residue, and each call drove the kernel's tracked IRQL to garbage.
bridge_ObfDereferenceObject had already been fixed for this exact reason and
carries a comment saying so; the Kf pair was simply missed.

The rule is mechanical, so assert it rather than relying on the next reader
noticing the comment.
"""

import os
import re

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")
BRIDGE_C = os.path.join(ROOT, "src", "kernel", "kernel_bridge.c")


def _source():
    with open(BRIDGE_C, encoding="utf-8", errors="replace") as fh:
        return fh.read()


def _bridge_bodies(src):
    """bridge_<Name> -> function body text, by brace matching."""
    bodies = {}
    for m in re.finditer(r"static void (bridge_\w+)\(void\)\s*\n\{", src):
        name = m.group(1)
        start = m.end() - 1
        depth = 0
        for i in range(start, len(src)):
            if src[i] == "{":
                depth += 1
            elif src[i] == "}":
                depth -= 1
                if depth == 0:
                    bodies[name] = src[start:i]
                    break
    return bodies


def _dispatch(src):
    """ordinal -> bridge_<Name>, from bridge_for_ordinal's switch."""
    return {int(o): b for o, b in
            re.findall(r"case\s+(\d+):\s*return\s+(bridge_\w+);", src)}


def _arg_bytes(src):
    """ordinal -> declared stack bytes, from stdcall_args_for_ordinal."""
    fn = src[src.index("static int stdcall_args_for_ordinal"):]
    fn = fn[:fn.index("\n}\n")]
    return {int(o): int(n) for o, n in
            re.findall(r"case\s+(\d+):\s*return\s+(-?\d+);", fn)}


def _violations():
    src = _source()
    bodies, disp, argb = _bridge_bodies(src), _dispatch(src), _arg_bytes(src)
    out = []
    for ordinal, bridge in sorted(disp.items()):
        body = bodies.get(bridge)
        if body is None or ordinal not in argb:
            continue
        idxs = [int(i) for i in re.findall(r"STACK_ARG\((\d+)\)", body)]
        if not idxs:
            continue
        declared = argb[ordinal]
        if declared < 0:          # caller-cleans (cdecl varargs, e.g. DbgPrint)
            continue
        needed = (max(idxs) + 1) * 4
        if declared < needed:
            out.append((ordinal, bridge, max(idxs), declared, needed))
    return out


def test_no_bridge_reads_past_its_declared_stack_frame():
    bad = _violations()
    assert not bad, "\n".join(
        f"ordinal {o} {b}: reads STACK_ARG({i}) "
        f"(needs {need} bytes) but stdcall_args_for_ordinal says {have}. "
        f"If this export is __fastcall, read g_ecx/g_edx instead -- see "
        f"bridge_ObfDereferenceObject."
        for o, b, i, have, need in bad)


def test_the_known_fastcall_bridges_use_registers_not_the_stack():
    """
    Named explicitly so a future edit cannot quietly reintroduce the JSRF bug
    by reverting one of these to STACK_ARG while its arg-size entry stays 0.
    """
    bodies = _bridge_bodies(_source())
    for name in ("bridge_KfRaiseIrql", "bridge_KfLowerIrql",
                 "bridge_ObfDereferenceObject"):
        body = bodies.get(name)
        assert body is not None, f"{name} disappeared"
        assert "STACK_ARG" not in body, (
            f"{name} is __fastcall: its argument arrives in ecx and never "
            f"reaches the stack, so STACK_ARG reads caller residue")
        assert "g_ecx" in body, f"{name} should read its argument from g_ecx"


def test_the_checker_itself_finds_a_planted_fault():
    """A checker that can never fail is not a check."""
    src = _source().replace("(UCHAR)g_ecx", "(UCHAR)STACK_ARG(0)")
    bodies, disp, argb = _bridge_bodies(src), _dispatch(src), _arg_bytes(src)
    hits = [o for o, b in disp.items()
            if b in bodies and "STACK_ARG" in bodies[b] and argb.get(o) == 0]
    assert hits, "planting STACK_ARG(0) in a 0-byte-frame bridge went unnoticed"
