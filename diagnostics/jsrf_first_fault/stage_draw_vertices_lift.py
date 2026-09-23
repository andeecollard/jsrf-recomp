#!/usr/bin/env python3
"""Create a separate opt-in DrawVertices gen tree; never edit the source tree."""
import argparse
import hashlib
from pathlib import Path
import shutil

BODY_SHA = "f173e6acbee1fa5dde27e20124ebd66184a784a39522de4ce82d861ec43a4ad7"


def extract(source):
    start = source.index("void sub_00199300(void)\n{")
    end = source.index("\n}\n", start) + 3
    return start, end, source[start:end]


def stage(source, destination):
    source = source.resolve()
    destination = destination.resolve()
    if destination == source or source in destination.parents:
        raise ValueError("destination must be separate from source")
    if destination.exists():
        raise FileExistsError(destination)
    candidates = []
    for path in source.glob("recomp_*.c"):
        content = path.read_text()
        if "void sub_00199300(void)\n{" in content:
            candidates.append((path, content))
    if len(candidates) != 1:
        raise ValueError("expected exactly one DrawVertices body")
    path, content = candidates[0]
    start, end, body = extract(content)
    if hashlib.sha256(body.encode()).hexdigest() != BODY_SHA:
        raise ValueError("unverified generated body; rerun equivalence checks before supporting it")
    shutil.copytree(source, destination)
    renamed = body.replace("void sub_00199300(void)",
                           "static void jsrf_draw_vertices_original(void)", 1)
    (destination / path.name).write_text(content[:start] + renamed + content[end:] +
                                       '\n#include "draw_vertices_lift.h"\n')
    shutil.copyfile(Path(__file__).with_name("draw_vertices_lift.h"),
                    destination / "draw_vertices_lift.h")
    (destination / "DRAW_LIFT_MANIFEST.txt").write_text(
        f"source={source}\noriginal_body_sha256={BODY_SHA}\n"
        "switch=RECOMP_JSRF_DRAW_LIFT=1\n"
        "scope=DrawVertices packet emitter; existing renderer retained\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    args = parser.parse_args()
    stage(args.source, args.destination)
    print(args.destination)
