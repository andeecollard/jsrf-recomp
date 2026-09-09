# The conditional move that was never emitted

3 September 2026. Continues `CODEX_TO_CLAUDE_HANDOVER_2026-09-03.txt`, which
left G8 open with the startup fade frozen at alpha 0.

## What was wrong

`tools/recomp/lifter.py` translated every x87 instruction in JSRF's `fminf` and
`fmaxf` except the one that does the work. `FCMOVcc` had no case, so it fell
through to the generic handler and became a bare comment:

```c
/* FPU: fcmove st(0), st(1) */
```

The whole of `sub_0014C870` (`fminf`) is

```
fld [esp+4]        ; st0 = a
fld [esp+8]        ; st0 = b, st1 = a
fcom st(1)
xor eax, eax
fnstsw ax
test ah, 1         ; C0: b < a
fcmove st(0), st(1); keep a when b >= a
fxch st(1)
fstp st(0)
```

With the move gone, `fxch`/`fstp` discard `a` unconditionally and the routine
returns `b` — its *second* argument — every time. `sub_0014C850` (`fmaxf`) has
the same shape with `fcmovne`.

That matters because the colour packer `sub_000A4CF0` clamps each channel with
`fminf(c, 1.0f)` and then `fmaxf(c, 0.0f)`, writing the result back into the
object's colour array before packing it:

```
fminf(c, 1.0f) -> 1.0f   (the second argument)
fmaxf(1.0f, 0) -> 0.0f   (the second argument)
```

So every colour the packer touched was zeroed, and the packed ARGB it returned
was 0. Only four x87 instructions in the entire generated tree were untranslated
(`grep '/\* FPU: '` finds two `fnclex` and these two), and both of these sit on
the startup path.

## How it presented

The four-channel interpolator `sub_00024700` at object `0x01A41E60` ramps
`+0x98..+0xA4` toward `+0xA8..+0xB4` by `+0xB8` each tick, then calls the packer
and stores the packed colour in `+0xBC`. The render method `sub_00024400`
returns immediately unless `+0xBC & 0xFF000000`, so a zero packed colour means
the fade overlay never draws and the startup script never sees the fade finish.

Snapshots two seconds apart could not distinguish "never called", "returns
early" and "result discarded". Entry/exit observation at `loc_00024700` and
`loc_00024962` separated them in one run:

| | pre-fix (`claude-interp-14`) | post-fix (`claude-fcmov-15`) |
|---|---|---|
| calls to `sub_00024700` | 18 in 35 s | 45,000+ in 45 s |
| `+0x98` values observed | 1 (`0`) | 242 distinct |
| `+0xBC` after an active tick | `00000000` | `FF..FC..FA..` down to `00` |
| target-colour commands | 2 | 11, then a new one (`AA000000`, step 1/60) |

Pre-fix, call 11 entered with `+0x98 = 1.0`, target `0`, step `1/120` and left
with `+0x98 = 0`: a 120-frame fade collapsed into one tick, because the packer
zeroed the channel rather than the interpolator stepping it. Post-fix the same
call leaves `3F7DDDDE` (0.99167 = 1 − 1/120) and the packed colour steps
`FF000000 -> FC000000 -> FA000000`, one step per tick.

## The fix

- `tools/recomp/lifter.py`: `FCMOVCC_TO_JCC` maps each of the eight FCMOVcc
  forms to the jcc testing the same EFLAGS bits, `_make_fcmovcc_cond` reuses the
  existing `_make_condition` machinery, and `lift_basic_block` emits
  `if (cond) { fp_top() = fp_st(i); }`. The source is `operands[-1]`: capstone
  reports FCMOVcc with both operands, and reading `operands[0]` would move
  `st(0)` onto itself — the same silent no-op `fxch st(i)` once had.
- When no flag setter is tracked the generic handler now says
  `UNTRANSLATED CONDITIONAL MOVE` instead of a bare `/* FPU: ... */`, which
  reads as handled.
- `tools/recomp/test_lifter_fcmov.py`: 5 tests, 8 subtests. Covers both
  conditions, all eight forms, `st(i)` source selection, the untracked-flags
  message, and the whole fmin sequence in order.
- `diagnostics/jsrf_first_fault/backport_fcmov.py` rewrites the two statements
  in the checked-out generated tree, which predates the lifter fix and is not
  regenerated here. Idempotent; `--check` is read-only and is wired up as the
  `jsrf_fcmov_backport` ctest, so a regeneration that loses the fix fails.
- `diagnostics/jsrf_first_fault/main.c`: `RECOMP_REPORT_MS` makes the periodic
  report interval configurable (default 5000). `RECOMP_FB_DUMP` writes one BMP
  per report, so 5 s was also the framebuffer sampling interval — coarser than
  the SEGA logo is on screen, which is why the logo appeared in no dump of the
  first two post-fix runs.
- `diagnostics/jsrf_first_fault/startup_probe.c` + `instrument_startup.py`:
  four more read-only, `RECOMP_INTERP_TRACE`-gated observation sites. No guest
  memory or guest register is written.
- `diagnostics/jsrf_first_fault/test_combiner_trace.py`: config 2 now groups two
  draws, because `vsh_render_test.c` gained the alternate-winding strip after
  that expectation was written. The strip changes cull/front-face only, so
  sharing the combiner config is the correct grouping.

## Evidence

Fresh HDD, unique run directory, `RECOMP_PB_EXEC=1`, no forced transition and no
disabled check.

`claude-fcmov-17` (90 s, `RECOMP_REPORT_MS=400`, 233 dumps) captures the startup
sequence in order. Four distinct title-produced frames:

| frame | md5 | what |
|---|---|---|
| `frame002` | `20b2450ed90b` | **Presented by SEGA** — byte-identical to `codex-graphics-09/frame003` |
| `frame012` | `fea03797e58f` | **CRI ADX** middleware logo — new |
| `frame020`+ | `8ac3f44fdc3f` | **anti-graffiti legal notice** — new, ×20 |
| 204 frames | `f4d48eee3742` | black |

`frame006` and `frame016` are solid cyan and solid blue: those are clear colours
sampled between the clear and the draw, not title-produced pictures, and are not
counted as milestones.

The SEGA logo matching `codex-graphics-09` byte for byte is the check that the
working renderer is preserved: the same pipeline still produces the old frame
and two new ones. `claude-fcmov-15` (45 s) and `claude-fcmov-16` (90 s) show the
same sequence at the coarser 5 s sampling.

Tests: `ctest` 10/10; `test_combiner_trace.py` OK; `tools/recomp` 172 passed /
31 subtests with one pre-existing failure (`test_config.py::
test_fallback_is_replaced`, a cross-test global-state leak that fails
identically without these changes and passes when run alone). The Python lifter
tests need `capstone`, which is not installed on this machine; they were run in
a throwaway venv.

## What this does not claim

- Not gameplay, not menu reachability. Three startup screens, no input.
- The fade overlay itself still does not composite. It is an untextured draw,
  and the renderer rejects those: `claude-fcmov-15` reports
  `[TEXTURE] prepared=16258 rejected=135032`, of which `135030` are
  `texture 0 disabled` and `2` are `texture format / mip layout`. These are new
  because the title now reaches code that draws them, and they are rejected and
  reported rather than silently accepted. Vertex rejections remain 0.
- Sound is still bypassed, host input is still not connected, and there is still
  no Windows/D3D11 validation.

## The next blocker, measured

All three post-fix runs create a zero-byte `Z:\Media\Cache\JSRF_FATAL.ERR` about
37 seconds after the cache completes; no pre-fix run created it at all. This is
the title's own error path, not a guest fault — nothing crashes.

`RECOMP_FILE_BACKTRACE=JSRF_FATAL` (`claude-fatal-18`) names it in the game's own
words, from the object whose `+0x98` carries the `0x400000` bit:

```
+0xA0 -> "There's a problem with the disc $nyou're using. $nIt may be dirty or damaged."
```

The cause is immediately above it in the log, repeating:

```
xbox_HeapAlloc: out of memory (requested 1092096, used 49950908/50855936)
  [FILE] NtOpenFile ... path=Z:\Media\Player\
```

The guest heap is exhausted loading player assets, the retries fail, and the
title raises its disc error. The heap arena is not undersized — 64 MB retail RAM
less the 15.5 MB below `XBOX_HEAP_BASE`. The report the allocator prints on
failure already names the owners:

```
live 1514 blocks / 49301920 bytes; free 1 blocks / 270336 bytes;
largest free 270336; retained-beyond-request 4581336
  ordinal 166  ra=0x0018E6E9 : 225 blocks, 19226360 bytes
  ordinal 184  ra=0x0014903F :   3 blocks,  7340032 bytes
  ordinal 184  ra=0x00145A48 : 281 blocks,  5953592 bytes
  ordinal 166  ra=0x00199789 : 231 blocks,  5525444 bytes
```

That is where the next investigation starts, and it is a memory question, not a
renderer one.

## Runs

Under `build-macos/jsrf-first-fault/render-investigation/`, all fresh-HDD:

- `claude-interp-14` — 35 s, pre-fix, entry/exit observation. The frozen baseline.
- `claude-fcmov-15` — 45 s, first post-fix run. ADX and the notice.
- `claude-fcmov-16` — 90 s, confirmation at 5 s sampling.
- `claude-fcmov-17` — 90 s, `RECOMP_REPORT_MS=400`. The full sequence including
  the SEGA logo. **Use this one.**
- `claude-fatal-18` — 75 s, `RECOMP_FILE_BACKTRACE=JSRF_FATAL`. The heap report.
