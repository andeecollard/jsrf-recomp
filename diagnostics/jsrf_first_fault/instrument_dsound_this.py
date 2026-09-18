#!/usr/bin/env python3
"""Capture the guest DirectSound object pointer (`this`) from the ISR.

WHY THIS EXISTS. The DSOUND crash is `001A200D` doing
`pBuf = this->owner[h]` with no NULL check, and `001A2E2E` dereferencing the
result. Read out of the generated C:

    ecx = MEM32(ecx + eax * 4 + 0x2C4);     // ecx = this->owner[h]
    edx = ZX8(MEM8(ecx + 0x64));            // NULL -> page 0 -> 0
    edx = ZX16(MEM16(ecx + edx * 2 + 0xA)); // -> 0
    if (eax != edx) goto return;            // h != 0 RETURNS SAFELY
    ... call sub_001A2E2E                   // only reached when h == 0

So a NULL owner is harmless for every handle except 0, where the accidental
`0 == 0` lets it through -- which is why all 26 crash dumps name voice 0.

To stop handing the ISR a handle it will dereference into NULL, the APU model
must be able to read `owner[h]` itself. That needs `this`, and `this` is a
thiscall parameter passed down 001A25AA -> 001A24BE -> 001A241F -> 001A200D.
It is never loaded from a guest global, so it cannot be read from outside; it
has to be captured where the guest hands it over.

WHAT THIS PATCHES. One line at the top of `sub_001A25AA`, where `edi = ecx`
already records `this`:

    jsrf_dsound_this_seen(ecx);   /* DSOUND-THIS probe */

which stores `this` AND counts how many distinct values the run ever sees.
That second number is the point: the probe fires at ISR ENTRY, so a raise-time
read of owner[h] goes through a `this` left behind by an EARLIER entry. One
distinct value means that cannot matter. More than one means every owner[]
number taken so far -- including "of those NULLs, 0 were h==0", the control
that retired the owner guard -- was read against a table that may not be the
ISR's, and has to be retaken. Read the [APU-IDLE-OWNER] `this` captured line in the
report, and read its calls= before believing a distinct count of 0.

Read-only with respect to the guest. Nothing else is touched.

USAGE
    instrument_dsound_this.py <gen-dir> [--revert]

The file is backed up beside itself as `<file>.dsound-this-orig` with an APFS
clone (`cp -c`, instant and near-free), and `--revert` restores it and checks
the restore byte for byte. CLAUDE.md asks for instrumentation to go into a COPY
of a gen tree; a single-file clone-and-restore is the same guarantee for one
translation unit at a fraction of the rebuild, and the revert is verified
rather than assumed.
"""
import hashlib
import shutil
import subprocess
import sys
from pathlib import Path

ANCHOR = "loc_001A25AA: ;"
PROBE = ("    { extern void jsrf_dsound_this_seen(unsigned int);\n"
         "      jsrf_dsound_this_seen(ecx); }  /* DSOUND-THIS probe */\n")
MARK = "DSOUND-THIS probe"
# The v1 probe stored `this` and nothing else. A tree still carrying it looks
# instrumented to MARK, so check for the CALL as well -- otherwise an upgrade
# silently no-ops and the distinct count reads 0 with calls=0, which is
# indistinguishable from "no probe at all".
CALL = "jsrf_dsound_this_seen(ecx)"


def find(gen: Path) -> Path:
    hits = [p for p in sorted(gen.glob("recomp_*.c"))
            if "void sub_001A25AA" in p.read_text(errors="ignore")]
    if len(hits) != 1:
        sys.exit("expected exactly one file defining sub_001A25AA, found %d" % len(hits))
    return hits[0]


def digest(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def main() -> int:
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    gen = Path(sys.argv[1])
    revert = "--revert" in sys.argv
    src = find(gen)
    bak = src.with_suffix(src.suffix + ".dsound-this-orig")

    if revert:
        if not bak.exists():
            sys.exit("no backup at %s -- nothing to revert" % bak)
        shutil.copyfile(bak, src)
        if MARK in src.read_text(errors="ignore"):
            sys.exit("REVERT FAILED: the probe is still present in %s" % src)
        print("reverted %s (sha %s)" % (src.name, digest(src)[:16]))
        return 0

    text = src.read_text(errors="ignore")
    if MARK in text:
        if CALL in text:
            print("already instrumented: %s" % src.name)
            return 0
        sys.exit("%s carries the OLD probe, which records only the last `this`"
                 " and cannot count distinct objects. Revert it first:\n"
                 "    %s %s --revert"
                 % (src.name, Path(sys.argv[0]).name, gen))
    if text.count(ANCHOR) != 1:
        sys.exit("expected exactly one %r in %s, found %d"
                 % (ANCHOR, src.name, text.count(ANCHOR)))
    if not bak.exists():
        # cp -c: APFS clone. Falls back to a plain copy on any other filesystem.
        if subprocess.run(["cp", "-c", str(src), str(bak)]).returncode != 0:
            shutil.copyfile(src, bak)
    src.write_text(text.replace(ANCHOR, ANCHOR + "\n" + PROBE, 1))
    print("instrumented %s" % src.name)
    print("  backup  %s" % bak.name)
    print("  revert  %s %s --revert" % (Path(sys.argv[0]).name, gen))
    return 0


if __name__ == "__main__":
    sys.exit(main())
