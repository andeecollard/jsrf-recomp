#!/usr/bin/env python3
"""Score dumped framebuffers for renderer artefacts, because no other gate can.

WHY THIS EXISTS. Every gate in this tree scores a synthetic surface:
metal_batch_test draws a few hundred triangles into one retained surface and
reads it straight back, metal_copy_test compares single draws byte for byte,
and metal_hw_check.sh reads their numbers. All of them passed, for weeks, on a
renderer that a person could see was wrong -- because the defect needed tens of
thousands of draws before the GPU fell behind the producer. The gate for that
is a real frame, and there was no tool that could score one.

It was also written after doing the same analysis five times in throwaway
scripts, one of which searched for seams ONLY at multiples of 64 and then
reported that seams were at multiples of 64. A committed tool with a chance
baseline and a self-test does not make that mistake twice.

WHAT IT MEASURES

  tile     Strong edges that land on a --grid boundary, against the count
           chance would give. This is what found the tile mosaic: one render
           pass per draw let command buffers overlap, so each 64x64 tile of the
           frame kept whichever buffer stored it last.

               hardware, per-draw   248 seams,  66 on a 64 boundary   17.0x
               hardware, batched     29 seams,   0                     0.0x
               software control     227 seams,   3                     0.8x

           Reported as enrichment, not a raw count, because a detailed scene
           has more edges everywhere and a raw count would track scene content
           rather than the artefact.

  speckle  Isolated pixels that disagree with ALL of their neighbours. Sampling
           or texture-fetch corruption shows up as this granular noise, which
           carries no straight edges and so is invisible to `tile`.

WHAT IS VALIDATED, AND WHAT IS NOT. `tile` is: it separates the three arms
cleanly (17.0x per-draw, 0.0x batched, 0.8x software control) and its self-test
detects an injected seam. `speckle` and `temporal` are NOT. Both detect their
injected artefacts, and neither has been shown to measure the granular noise a
person can see:

  speckle   ranked the KNOWN-GOOD software path as the noisiest of three arms
            in all three of its versions -- disagree-with-all-neighbours, then
            require a flat neighbourhood, then normalise per flat opportunity.
            The cause is not the formula. The arms never reach the same scene,
            and a single-frame noise rate tracks how much flat area a scene
            has. No normalisation fixes comparing different pictures.
  temporal  reads 0.5 per million on a gameplay recording where the mosaic is
            plainly visible, because it needs the surroundings to hold still
            and the camera is moving.

`tile` works because it keys on a STRUCTURAL property only the artefact has --
alignment to a grid -- measured as a ratio inside one frame, which is why it
needs no matched scene. That is the bar a metric here has to clear. Until one
does, do not compare speckle or temporal numbers across arms; a passing
--selftest is not evidence that a metric measures the real defect.

NEITHER IS A PASS/FAIL ON ITS OWN. Both are scene dependent, so the unit of
judgement is two arms of the same scene: --compare takes a baseline directory
and a candidate one and reports both metrics side by side.

    fb_score.py dumps/*.bmp
    fb_score.py --compare good/ bad/
    fb_score.py --selftest          # injects known artefacts; must detect them
"""
import argparse, glob, os, struct, sys

def read_bmp(path):
    d = open(path, 'rb').read()
    if d[:2] != b'BM':
        raise ValueError(f"{path}: not a BMP")
    off = struct.unpack_from('<I', d, 10)[0]
    w, h = struct.unpack_from('<ii', d, 18)
    bits = struct.unpack_from('<H', d, 28)[0]
    if bits != 24:
        raise ValueError(f"{path}: expected 24bpp, got {bits}")
    stride = (w * 3 + 3) & ~3
    if off + stride * h > len(d):
        raise ValueError(f"{path}: truncated")
    rows = []
    for r in range(h):
        base = off + r * stride
        rows.append(d[base:base + w * 3])
    rows.reverse()                       # BMP is bottom-up
    return w, h, rows

def luma(rows, w, h):
    return [[(rows[y][x*3] + rows[y][x*3+1] + rows[y][x*3+2]) // 3
             for x in range(w)] for y in range(h)]

def tile_score(g, w, h, grid, edge, span):
    """Strong seams on a grid boundary, against the chance expectation."""
    vs = [x for x in range(1, w)
          if sum(1 for y in range(h) if abs(g[y][x] - g[y][x-1]) > edge) > h * span]
    hs = [y for y in range(1, h)
          if sum(1 for x in range(w) if abs(g[y][x] - g[y-1][x]) > edge) > w * span]
    on = sum(1 for x in vs if x % grid == 0) + sum(1 for y in hs if y % grid == 0)
    tot = len(vs) + len(hs)
    exp = tot / float(grid)              # a seam at a uniformly random position
    return tot, on, exp, (on / exp if exp > 0 else 0.0)

def speckle_score(g, w, h, thresh, smooth=20):
    """A speck in a FLAT region. Both halves of that matter.

    The first version of this only asked whether a pixel disagreed with all
    four neighbours, and it ranked the known-good software path as the NOISIEST
    of the three arms -- which is the metric telling you it is measuring the
    wrong thing. Two things it was actually counting:

      the title's own ordered dither, which by construction makes every other
      pixel disagree with its neighbours by about one 565 step, everywhere; and

      cel-shaded single-pixel detail, where a pixel legitimately differs from
      all of its neighbours because the texture does.

    Requiring the NEIGHBOURHOOD to be mutually consistent removes both: dither
    and texture detail live in busy neighbourhoods, a corrupted texel sits in a
    flat one. `smooth` is how flat the neighbours must be, and `thresh` is how
    far the centre must stand out -- well above the ~8 luma a one-step dither
    can produce.

    A metric that ranks the known-good arm worst is not measuring the artefact.
    That is the check to apply before believing any number this returns.
    """
    n = 0
    flat = 0
    for y in range(1, h - 1):
        gy, gu, gd = g[y], g[y-1], g[y+1]
        for x in range(1, w - 1):
            a, b, c, d = gy[x-1], gy[x+1], gu[x], gd[x]
            lo = a if a < b else b
            if c < lo: lo = c
            if d < lo: lo = d
            hi = a if a > b else b
            if c > hi: hi = c
            if d > hi: hi = d
            if hi - lo > smooth:            # busy neighbourhood: detail, not noise
                continue
            flat += 1
            if abs(gy[x] - (a + b + c + d) // 4) > thresh:
                n += 1
    # PER FLAT OPPORTUNITY, not per pixel.
    #
    # A rate over all pixels measures how much flat area the scene happens to
    # have, not how noisy it is, and two arms never reach the same scene -- so
    # the first two versions of this ranked the known-good software path as the
    # noisiest of three arms twice over. The tile metric above survived that
    # confound because enrichment is a ratio taken inside the frame; this is
    # the same idea. Denominator is the number of flat neighbourhoods actually
    # examined, so a bare scene and a busy one are comparable.
    return n, (1e6 * n / float(flat)) if flat else 0.0

def load_any(path):
    """BMP from RECOMP_FB_DUMP, or anything Pillow reads (a recording's frames)."""
    if path.lower().endswith('.bmp'):
        return read_bmp(path)
    from PIL import Image                      # only needed for recordings
    im = Image.open(path).convert('RGB')
    w, h = im.size
    d = im.tobytes()
    return w, h, [d[y*w*3:(y+1)*w*3] for y in range(h)]

def temporal_score(frames, thresh, still):
    """Pixels that FLICKER while their surroundings hold still.

    The one property that separates this artefact from the two things every
    single-frame metric here kept measuring instead. The title's ordered dither
    is deterministic per screen position, so a static area dithers to the SAME
    value every frame; cel-shaded detail is likewise fixed to the geometry. A
    corrupted fetch is not -- it changes while nothing around it does.

    It is also the only formulation that does not need the two arms to reach
    the same scene, which single-frame noise comparison does and which no
    amount of normalising fixes. Three versions of speckle_score ranked the
    known-good path worst before that was the conclusion rather than the
    metric.

    `still` is how quiet the neighbourhood must be between the two frames, so
    ordinary motion is not counted; `thresh` is how far the centre must move.
    Reported per still opportunity, for the same reason speckle is.
    """
    flick = 0; opp = 0
    # EXCLUDE PIXELS THAT NEVER MOVE AT ALL. Letterbox bars and static HUD are
    # perfectly still in every frame, so they are "still opportunities" by the
    # thousand and drown the rate in pixels that could never have flickered.
    # A pixel has to be somewhere the picture is actually live.
    w0, h0, _ = frames[0]
    gs = [luma(f[2], f[0], f[1]) for f in frames]
    live = [[any(gs[k][y][x] != gs[0][y][x] for k in range(1, len(gs)))
             for x in range(w0)] for y in range(h0)]
    for (w, h, a), (_, _, b) in zip(frames, frames[1:]):
        ga = luma(a, w, h); gb = luma(b, w, h)
        for y in range(1, h - 1):
            for x in range(1, w - 1):
                n = (abs(ga[y][x-1]-gb[y][x-1]) + abs(ga[y][x+1]-gb[y][x+1])
                     + abs(ga[y-1][x]-gb[y-1][x]) + abs(ga[y+1][x]-gb[y+1][x]))
                if n > still * 4:             # the area is moving: not flicker
                    continue
                if not live[y][x]:            # letterbox, static HUD: not live
                    continue
                opp += 1
                if abs(ga[y][x] - gb[y][x]) > thresh:
                    flick += 1
    return flick, (1e6 * flick / float(opp)) if opp else 0.0

def distinct_frames(files, limit):
    """A recording runs faster than the game, so most frames are duplicates."""
    out = []; prev = None
    for f in files:
        w, h, rows = load_any(f)
        g = luma(rows, w, h)
        if prev is not None:
            diff = sum(abs(g[y][x]-prev[y][x])
                       for y in range(0, h, 4) for x in range(0, w, 4))
            if diff < (w//4)*(h//4)*2:
                continue
        prev = g; out.append((w, h, rows))
        if len(out) >= limit:
            break
    return out

def score_files(files, args):
    tot = on = exp = 0.0; sp = 0.0; used = 0
    for f in files:
        try:
            w, h, rows = read_bmp(f)
        except ValueError as e:
            print(f"  skipped {os.path.basename(f)}: {e}", file=sys.stderr); continue
        g = luma(rows, w, h)
        t, o, e, _ = tile_score(g, w, h, args.grid, args.edge, args.span)
        _, rate = speckle_score(g, w, h, args.speckle, args.smooth)
        tot += t; on += o; exp += e; sp += rate; used += 1
        if args.per_frame:
            enr = o / e if e > 0 else 0.0
            print(f"  {os.path.basename(f):>16}  seams {t:4d}  on-grid {o:3d}"
                  f"  enrichment {enr:5.1f}x   speckle {rate:7.1f}/Mflat")
    if not used:
        return None
    return dict(frames=used, seams=tot, on=on, exp=exp,
                enrich=(on / exp if exp > 0 else 0.0), speckle=sp / used)

def report(tag, s):
    if not s:
        print(f"{tag}: no readable frames"); return
    print(f"{tag}: {s['frames']} frames, {int(s['seams'])} strong seams, "
          f"{int(s['on'])} on a grid boundary (chance {s['exp']:.1f}) "
          f"-> enrichment {s['enrich']:.1f}x; speckle {s['speckle']:.1f}/Mflat")

def selftest(args):
    """A tool that cannot fail is not a gate. Inject, then require detection."""
    import random
    w = h = 128
    random.seed(11)
    def blank():
        return [[bytearray(b''.join(bytes((60, 90, 70)) for _ in range(w)))
                 for _ in range(1)][0] for _ in range(h)]
    def scene():                       # smooth content, no straight edges
        rows = []
        for y in range(h):
            r = bytearray()
            for x in range(w):
                v = 60 + (x // 17) * 3 + (y // 23) * 2
                r += bytes((v, v + 20, v + 5))
            rows.append(r)
        return rows
    def measure(rows):
        g = luma(rows, w, h)
        t, o, e, enr = tile_score(g, w, h, args.grid, args.edge, args.span)
        _, sp = speckle_score(g, w, h, args.speckle, args.smooth)
        return enr, sp
    base_enr, base_sp = measure(scene())
    # inject a tile seam every `grid` columns
    rows = scene()
    for y in range(h):
        for x in range(args.grid, w, args.grid):
            for k in range(x, min(w, x + 3)):
                rows[y][k*3:k*3+3] = bytes((250, 250, 250))
    tile_enr, _ = measure(rows)
    # inject isolated speckle
    rows = scene()
    for _ in range(400):
        x = random.randrange(1, w-1); y = random.randrange(1, h-1)
        rows[y][x*3:x*3+3] = bytes((255, 0, 255))
    _, spk = measure(rows)
    ok = True
    print(f"clean scene       enrichment {base_enr:5.1f}x   speckle {base_sp:7.1f}/Mflat")
    print(f"seams injected    enrichment {tile_enr:5.1f}x   (must exceed 5x)")
    print(f"speckle injected  speckle    {spk:7.1f}/Mflat (must exceed 10x clean)")
    if tile_enr <= 5.0:
        print("FAIL: injected grid seams were not detected", file=sys.stderr); ok = False
    if spk <= max(10.0 * base_sp, 10.0):
        print("FAIL: injected speckle was not detected", file=sys.stderr); ok = False
    print("PASS: both injected artefacts are detected" if ok else "SELFTEST FAILED")
    return 0 if ok else 1

def main():
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('paths', nargs='*', help='BMP files or directories')
    p.add_argument('--compare', nargs=2, metavar=('BASELINE', 'CANDIDATE'))
    p.add_argument('--grid', type=int, default=64, help='tile grid (default 64)')
    p.add_argument('--edge', type=int, default=40, help='luma step that counts as an edge')
    p.add_argument('--span', type=float, default=0.33, help='fraction of the axis a seam must cross')
    p.add_argument('--speckle', type=int, default=35, help='how far a speck must stand out')
    p.add_argument('--smooth', type=int, default=20, help='how flat its neighbourhood must be')
    p.add_argument('--per-frame', action='store_true')
    p.add_argument('--selftest', action='store_true')
    p.add_argument('--temporal', metavar='DIR',
                   help='consecutive frames (or a recording\'s frames): score flicker')
    p.add_argument('--still', type=int, default=6, help='how quiet the surroundings must be')
    p.add_argument('--frames', type=int, default=14, help='distinct frames to use')
    p.add_argument('--skip', type=int, default=0, help='ignore the first N frames (intro)')
    args = p.parse_args()
    if args.selftest:
        return selftest(args)
    def expand(items):
        out = []
        for it in items:
            out += sorted(glob.glob(os.path.join(it, '*.bmp'))) if os.path.isdir(it) else sorted(glob.glob(it))
        return out[args.skip:]
    if args.temporal:
        files = sorted(glob.glob(os.path.join(args.temporal, '*')))
        fr = distinct_frames([f for f in files if os.path.isfile(f)], args.frames)
        if len(fr) < 2:
            print('need at least two distinct frames', file=sys.stderr); return 2
        n, rate = temporal_score(fr, args.speckle, args.still)
        print(f"{args.temporal}: {len(fr)} distinct frames, "
              f"flicker {rate:.1f} per million still pixels ({n} flagged)")
        return 0
    if args.compare:
        a, b = expand([args.compare[0]]), expand([args.compare[1]])
        sa, sb = score_files(a, args), score_files(b, args)
        report('baseline ', sa); report('candidate', sb)
        if sa and sb:
            print(f"\nenrichment {sa['enrich']:.1f}x -> {sb['enrich']:.1f}x;"
                  f"  speckle {sa['speckle']:.1f} -> {sb['speckle']:.1f}/Mflat")
        return 0
    if not args.paths:
        p.print_help(); return 2
    report('frames', score_files(expand(args.paths), args))
    return 0

if __name__ == '__main__':
    sys.exit(main())
