"""Pair two runs' frames and say how far apart the pictures are.

    /usr/bin/python3 frame_match.py <run A dir> <run B dir> [--threshold 32]

Each directory is searched (recursively) for frame-*.bmp, the flight
recorder's presented frames. Every frame of A is paired with the frame of B
it differs from least, so two arms of the same scene line up even when one
runs a frame or two ahead; the score is the percentage of pixels whose
largest channel difference exceeds the threshold.

Why this exists: on 25 Sep 2026 VERIFY (per draw) and the glitch watch (one-
frame transients) both passed a build whose window showed a frozen,
half-drawn frame for seconds at a time. A whole-frame comparison against the
executor arm caught it at once. Motion (poses, spinning pickups, traffic)
scores a few percent; a broken picture scores far more, or pairs every frame
of A with the same frame of B.
"""
import argparse
import glob
import os

import numpy as np
from PIL import Image


def frames(root):
    paths = sorted(glob.glob(os.path.join(root, "**", "frame-*.bmp"),
                             recursive=True))
    return paths, [np.asarray(Image.open(p).convert("RGB")).astype(np.int16)
                   for p in paths]


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("a")
    ap.add_argument("b")
    ap.add_argument("--threshold", type=int, default=32)
    args = ap.parse_args()
    pa, fa = frames(args.a)
    pb, fb = frames(args.b)
    if not fa or not fb:
        raise SystemExit("no frame-*.bmp in %s" % (args.a if not fa else args.b))
    scores = []
    for i, a in enumerate(fa):
        row = [100.0 * (np.abs(a - b).max(axis=2) > args.threshold).mean()
               if a.shape == b.shape else float("inf") for b in fb]
        j = int(np.argmin(row))
        scores.append(row[j])
        print("%-40s -> %-40s %6.2f%%" % (os.path.relpath(pa[i], args.a),
                                          os.path.relpath(pb[j], args.b), row[j]))
    s = sorted(scores)
    print("frames %d, best %.2f%%, median %.2f%%, worst %.2f%% over %d"
          % (len(s), s[0], s[len(s) // 2], s[-1], args.threshold))


if __name__ == "__main__":
    main()
