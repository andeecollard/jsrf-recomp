# JSRF: combiner classification and the missing physical-memory alias

Historical checkpoint: the black-screen status below was superseded later
the same day by `CODEX_PROGRESS_2026-09-03_FIRST_GRAPHICS.md`. The genuine
SEGA startup logo now renders; this file retains the earlier evidence.

Continues `CLAUDE_TO_CODEX_HANDOVER_2026-09-03.txt` from clean commit
`5d95272`. Original handovers are preserved. No generated source was edited
or regenerated; no commit or push was made.

## Outcome

The dominant combiner is identified, narrowly implemented and pixel-tested.
More importantly, the title's supposedly empty vertex buffers were found in
the CPU physical window. The JSRF harness now makes that heap view share
storage with the GPU's low-memory view, and prevents a legacy scanner from
executing the same pushbuffer a second time.

**The framebuffer is still black. G6 and G7 remain open.** Real draws now
have usable geometry, but alpha testing, DXT1 sampling, blending, culling,
depth and dithering still block their fragment path. No unsupported state
was bypassed to manufacture a picture.

## 1. Exactly two configurations, not a general-combiner problem

The five-minute fresh-HDD measurement `codex-combiners-01` counted:

| Configuration | Draws | Rejected | Meaning |
| --- | ---: | ---: | --- |
| 0, first draw 1 | 806,831 | 0 | Existing RGB565 copy |
| 1, first draw 11 | 1,613,642 | 1,613,642 | Texture × diffuse, with three pass-through stages |

These are last emitted report counts, not exact termination counts or frame
rates. All 56 combiner/texture-program words are compared directly, including
inactive stages and constants. No hash-only equality and no table overflow.

Configuration 1:

| Register | Stage 0 | Stages 1–3 |
| --- | --- | --- |
| COLOR_ICW | `08040000` | `0C200000` |
| ALPHA_ICW | `18140000` | `1C200000` |
| COLOR_OCW | `00000C00` | `00000C00` |
| ALPHA_OCW | `00000C00` | `00000C00` |

`COMBINER_CONTROL=4`, `SHADER_STAGE_PROGRAM=1`, final CW0=`0000000C`,
CW1=`00001C80`. Stage 0 computes T0 × V0 in RGB and alpha; stages 1–3
copy R0. The final combiner selects R0. Field decoding and unsigned-input /
output clamping were checked against the local register definitions and
[xemu's combiner parser and evaluation](https://github.com/xemu-project/xemu/blob/master/hw/xbox/nv2a/pgraph/glsl/psh.c).

`nv2a_texture_copy` recognizes exactly those active-stage words. It now
interpolates diffuse RGB with reciprocal W and modulates the sampled RGB565
colour. RGB565 texture alpha is one, so output alpha remains interpolated
diffuse alpha. The byte-copy shortcut is disabled for modulation. Unknown
stage words, DXT1, blend/depth states and unverified dithered modulation remain
rejections. This is not a general register-combiner interpreter.

On `codex-modulate-02`, 1,591,368 draws still rejected, now at `alpha test`.
This is a changed first failure, **not** a successful reduction in dropped
draws. That run also classified every rejected draw as array-backed with
invalid W and collapsed XY; every valid inline draw was the existing copy.
The old batch-120,000 observation did not establish post-cache scene geometry.

## 2. The title did write the vertices

`codex-physical-04`, sampled at VSH batch 20,000, found:

| View | First position | Diffuse |
| --- | --- | --- |
| Low `01BA2000`, read by the renderer | `(0, 0, 0, 0)` | zero |
| CPU physical `81BA2000`, written by the title | `(160, 165, 0.500125, 0.652994)` | white |
| CPU physical `81BA2020` | `(478, 165, 0.500125, 0.652994)` | white |
| CPU physical `81BA2040` | `(160, 311, 0.500125, 0.652994)` | white |

Guest helper `sub_00192820` reads the resource's data offset and ORs
`80000000` into it. `xbox_ContiguousAlloc` on POSIX returns low heap addresses,
but the high contiguous window had independent backing. Therefore heap-poison
and no-free tests against the low view could not establish that the title
never filled the buffer. The DMA base of zero was correct; storage aliasing
was wrong.

New `xbox_EnablePhysicalHeapAlias()` explicitly maps only
`80F80000..84000000` onto low heap `00F80000..04000000` for this 64 MB layout.
It checks the mapping lies inside the already-owned high window, uses the
same backing file, and preserves low-heap bytes. The JSRF harness enables it
after memory initialisation, before guest code. `RECOMP_PHYSICAL_HEAP_ALIAS=0`
restores separate backing for an A/B test.

This is deliberately opt-in: other titles, Windows, low pinned pools, the
image and fake kernel keep their existing layouts. Expanded mappings and
pinned pools overlapping the heap require a fuller physical allocator and
are not supported by this opt-in. Tests check both write directions,
reallocation zeroing, idempotence, the initial disabled state, and preservation
of the low image, high fake kernel and separate low-address pinned storage.

## 3. There was also a duplicate executor

With shared bytes, the background `nv2a_pb_scan` began seeing the same commands
as the harness's validated `nv2a_pusher_run_segment`. Both called
`nv2a_pb_exec_method`, concurrently mutating one global renderer. Previously
the scanner's high-memory input was empty, hiding the duplicate ownership.

`codex-alias-05` exposed mixed partial configurations and is **not valid
rendering evidence**. Do not treat its extra configurations as title programs.

The harness now calls `nv2a_pb_scan_set_external_executor(1)` before memory
initialisation starts the scanner thread. The scanner may still survey when
requested, but cannot execute or report the owner's mutable renderer state.
Legacy standalone execution remains available to other callers. A dedicated
ownership regression test checks both behaviours.

The final single-owner run returns to the two original configurations, with
zero invalid-position and collapsed-XY draws for the array-backed program.
Generic busy-bit/DMA acknowledgements remain bring-up behaviour; this does
not establish hardware-accurate GPU scheduling.

Final run `codex-alias-owner-06` completed the 300-second bound:

| Measure | Last report / verified output |
| --- | --- |
| Vertex batches executed / rejected | 2,373,127 / 0 |
| Existing copies prepared | 791,049 |
| Modulated draws rejected at alpha test | 1,582,078 |
| Invalid positions / collapsed XY, either configuration | 0 / 0 |
| Combiner configurations / overflow | 2 / 0 |
| Pusher bad headers | 0 |
| HDD files / bytes | 259 / 122,467,906 |
| Cache completion markers / fatal file | 9 / none |
| Frame dumps | 62, all RGB-black |

Every dumped BMP hashes to
`9dc5cdcf2b4abd98a266b99496fa0062bec2c56e30b979a7cb5d033275eed1b3`.
No guest-fault, allocation-failure, unresolved-stub execution or missing-
epilogue report was found. The run ended at the diagnostic time limit, not
through normal title exit. Counts are observations, not a performance benchmark.

The 20-second control `codex-alias-off-07` disables only the physical alias,
retaining single execution ownership. Its last report has 52,732 array draws,
all 52,732 with invalid positions and collapsed XY. This directly separates
the memory repair from the ownership repair. Captured draw 11 with aliasing
has screen positions `(160.5,165.5)`, `(478.5,165.5)`, `(160.5,311.5)` and
finite W=`1.531407`, rather than `(0.5,0.5,0,inf)`.

## 4. Diagnostics and validation

- `RECOMP_COMBINER_TRACE=1`: bounded 64-entry exact-state classification,
  per-configuration draw/rejection counts, inline count, invalid-position
  count and collapsed-XY count. Overflow is explicit.
- New configurations trigger draw captures when `RECOMP_DRAW_CAPTURE` is
  enabled. `RECOMP_DRAW_SAMPLE=<draw>` adds an exact late sample. Captures now
  carry a rejection reason and inline-word count; rejected captures do not
  retain stale resolved surface addresses.
- Texture-stage reports group all preparation rejection reasons.
- The GPU input range now includes all vertices, including zeros, and is
  labelled `input x/y`. The old range was only the first vertex per batch.
- Late VSH samples compare physical-window bytes with low-memory bytes.
- Seven CTests and the Python combiner capture/grouping regression pass.
- Texture/modulation, method-to-framebuffer and scanner-ownership tests pass ASan/UBSan with
  `-Wall -Wextra -Werror`; the measured copy's live and generated-pattern
  replay both match all 614,400 destination bytes.

All evidence is in ignored
`build-macos/jsrf-first-fault/render-investigation/<run>`; each run has its
own adjacent `<run>-hdd`. No old HDD directory was reused or deleted.
`codex-physical-03` sampled an inline copy and is not the alias comparison;
`codex-physical-04` sampled the intended array-backed draw.

Recheck with:

```sh
cmake --build build-macos/jsrf-first-fault/build --parallel 6
ctest --test-dir build-macos/jsrf-first-fault/build --output-on-failure
/usr/bin/python3 diagnostics/jsrf_first_fault/test_combiner_trace.py
```

For a new five-minute run, use the handover's fresh-HDD command plus
`RECOMP_COMBINER_TRACE=1`, `RECOMP_VSH_SAMPLE=20000`, and optionally
`RECOMP_DRAW_CAPTURE=<new-run>/draw-`. Do not reuse a completed HDD directory.
The generated tree's existing recovery/timing patches are still required;
fresh end-to-end generation, Windows and the full upstream suite were not tested.

## Next bounded target

Implement the measured fragment state for the now-valid array draws, keeping
each stage separately testable. Captured draw 11 uses:

- texture format `09910C29` (DXT1), address `00010101`;
- alpha test enabled, function `0204` (GREATER), reference 0;
- blend enabled, source `0302`, destination `0303`, equation `8006`;
- culling and depth test enabled, depth function `0203` (LEQUAL), depth writes;
- dithering enabled; surface format `00000123` (linear RGB565 / Z24S8).

The unit-test modulation result is not evidence that this entire state works.
The next success criterion remains a nonzero framebuffer produced by title
geometry on a fresh-HDD run, without suppressing alpha/depth/blend checks.
