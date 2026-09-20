#!/usr/bin/env python3
"""Install an opt-in removal probe into a COPY of a generated tree.

Usage: instrument_audio_remove.py SOURCE_GEN NEW_DEST_GEN
Build NEW_DEST_GEN, then set RECOMP_AUDIO_REMOVE=1. Records the guest's
removal inputs, not a proposed repair. Neither source nor guest state changes.
"""
import argparse
from pathlib import Path
import shutil


PROBE = r'''
    /* AUDIO-REMOVE probe: guest registers and memory are read-only. */
    {
        static int enabled = -1;
        if (enabled < 0) enabled = getenv("RECOMP_AUDIO_REMOVE") != NULL;
        if (enabled && (uint32_t)(edx - 25165824u) == 0xFE802054u) {
            extern unsigned long long g_apu_out_frames;
            fprintf(stderr, "[AUDIO-REMOVE] audio_frames=%llu pc=001A2EDC"
                    " object=%08X caller=%08X flags=%04X count=%u list=%u"
                    " first=%u successor=%u saved_top=%u saved_cvl=%u"
                    " saved_nvl=%u fatal=%08X\n",
                    g_apu_out_frames, esi, MEM32(ebp + 4),
                    (unsigned)MEM16(esi + 0x12), (unsigned)MEM8(esi + 0x64),
                    (unsigned)MEM8(esi + 0x65), (unsigned)MEM16(esi + 0xC),
                    edi, MEM32(ebp - 4), MEM32(ebp - 28), MEM32(ebp - 32),
                    MEM32(0x1BA04C));
        }
    }
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    anchor = "loc_001A2ED9: ;"
    matches = [(p, p.read_text()) for p in args.source.glob("recomp_*.c")
               if anchor in p.read_text()]
    if len(matches) != 1 or matches[0][1].count(anchor) != 1:
        parser.error("expected exactly one removal-site label")
    src, text = matches[0]
    if "AUDIO-REMOVE probe" in text:
        parser.error("source is already instrumented")
    # These are the precise locals the probe interprets; fail on gen drift.
    required = (
        "RECOMP_MEM_WRITE32(0x001A2EDCu, 0x001A2E2Eu, edx + -25165824, edi);",
        "RECOMP_MEM_WRITE32(0x001A2E4Eu, 0x001A2E2Eu, ebp + -4, edi);",
        "RECOMP_MEM_WRITE32(0x001A2E7Bu, 0x001A2E2Eu, ebp + -28, edi);",
        "RECOMP_MEM_WRITE32(0x001A2E69u, 0x001A2E2Eu, ebp + -32, eax);",
    )
    if not all(s in text for s in required):
        parser.error("generated removal locals do not match the probe")
    if args.destination.exists():
        parser.error("destination must not exist; use a fresh scratch directory")
    shutil.copytree(args.source, args.destination)
    text = text.replace('#include <math.h>',
                        '#include <math.h>\n#include <stdio.h>\n#include <stdlib.h>', 1)
    (args.destination / src.name).write_text(text.replace(anchor, anchor + PROBE, 1))
    print(f"Instrumented {args.destination / src.name}; source untouched")


if __name__ == "__main__":
    main()
