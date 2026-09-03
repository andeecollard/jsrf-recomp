"""Make the remaining placeholder stubs in an existing checkout announce themselves.

tools/recomp emits this shape for freshly generated code. This applies the same
change to a generated checkout that must not be regenerated, so a run names the
unresolved return paths it actually executes instead of leaving them silent.

--remove puts the plain stubs back.
"""
import argparse
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]

HELPER = '''
/* A stub that runs is a return path this build got wrong, and it used to run
 * in silence -- the damage showed up frames later as a caller with rotated
 * callee-saved registers, with nothing naming the address responsible. Say it
 * once per address: the first line is the one that matters, and a title that
 * reaches a stub in its main loop reaches it constantly. */
#include <stdio.h>
static void recomp_stub_ran(unsigned address, const char *note)
{
    fprintf(stderr, "  [STUB] unresolved target 0x%08X ran (%s); "
                    "its caller's stack is off\\n", address, note);
    fflush(stderr);
}
'''

PLAIN = re.compile(
    r"^void (sub_([0-9A-F]{8}))\(void\) \{ g_esp \+= (\d+); /\* 0x[0-9A-F]{8}: ([^\n]*) \*/ \}$",
    re.MULTILINE)
ANNOUNCING = re.compile(
    r"^void (sub_([0-9A-F]{8}))\(void\) \{ static int _seen; if \(!_seen\) \{ _seen = 1; "
    r"recomp_stub_ran\(0x[0-9A-F]{8}u, \"[^\"]*\"\); \} g_esp \+= (\d+); "
    r"/\* 0x[0-9A-F]{8}: ([^\n]*) \*/ \}$",
    re.MULTILINE)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path,
                        default=ROOT / "build-macos/jsrf-first-fault")
    parser.add_argument("--remove", action="store_true")
    args = parser.parse_args()

    path = args.output / "gen/recomp_stubs_unresolved.c"
    text = path.read_text()

    if args.remove:
        patched = ANNOUNCING.sub(
            lambda m: f"void {m.group(1)}(void) {{ g_esp += {m.group(3)}; "
                      f"/* 0x{m.group(2)}: {m.group(4)} */ }}", text)
        patched = patched.replace(HELPER, "")
        count = len(ANNOUNCING.findall(text))
    else:
        if "recomp_stub_ran" not in text:
            anchor = " * esp off by N on every call. */\n"
            assert anchor in text
            text = text.replace(anchor, anchor + HELPER, 1)
        patched = PLAIN.sub(
            lambda m: f"void {m.group(1)}(void) {{ static int _seen; "
                      f"if (!_seen) {{ _seen = 1; "
                      f'recomp_stub_ran(0x{m.group(2)}u, "{m.group(4)}"); }} '
                      f"g_esp += {m.group(3)}; "
                      f"/* 0x{m.group(2)}: {m.group(4)} */ }}", text)
        count = len(PLAIN.findall(text))

    path.write_text(patched)
    print(f"{'Reverted' if args.remove else 'Announcing'} {count} stubs in {path}")


if __name__ == "__main__":
    main()
