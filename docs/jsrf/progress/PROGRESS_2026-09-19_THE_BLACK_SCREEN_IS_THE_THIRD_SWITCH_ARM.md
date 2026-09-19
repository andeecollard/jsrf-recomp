# The black screen is the third switch arm, and it is fixed

19 Sep 2026, afternoon. Follows
`PROGRESS_2026-09-19_THE_FREEZE_AND_THE_CRASH_ARE_ONE_SWITCH_ARM.md` and the
midday handover, which listed the black screen at t=245 as new and unexplained
and `0x001063CF` as the one switch arm still open. They are the same thing.

## What was done

The four finished branches were merged (`wt-walker`, `wt-icall`, `wt-padrec`,
`wt-switches`; the fixpoint probe deliberately excluded), then the tree was
regenerated ONCE, in that order, because `regenerate.sh` copies
`templates/runtime/recomp_types.h` into the gen tree and `wt-icall` changes it.
All four merged with no conflicts. HEAD is `2bf7c1d`; the gen's manifest records
`git_head=2bf7c1d`, `git_dirty=no`.

## The finding: the black screen is 0x001063CF

In the player's preserved 12:28 session the `[ICALL] Failed to resolve VA`
banner appears EXACTLY ONCE in 307 seconds, at line 55745 — between the
`[FB] t= 244.01` line (line 55577, `nonzero=153338`) and the `[FB] t= 245.01`
line (line 55748, `nonzero=0`). The VA is `0x001063CF`.

Read the counter's trigger before quoting it, as ever. `recomp_icall_fail_log`
caps its prints at `FAIL_PRINT_LIMIT = 100` and had printed once, so this was
ONE failure event, not a storm. The `total calls: 43238453` in that line is
`g_icall_count`, which counts ALL indirect calls, not failures — the same
counter whose billions the runaway storms inflate.

So the session contains a single stranded switch arm, and it fires in the
one-second window in which the picture goes black and stays black.

### What actually stops, measured

`tris_drawn` is incremented from `nv2a_gpu_draw()`'s return on the Metal path
(`nv2a_pb_exec.c:4470`), so unlike `tris_skipped_offscreen` it is live on this
run. Per report window, across the boundary:

|      t | fb nonzero | Δ triangles | Δ draws | Δ clears | Δ flips |   fps |
|-------:|-----------:|------------:|--------:|---------:|--------:|------:|
| 240.01 |    153,332 |  10,780,216 |  42,730 |      249 |     250 |  50.0 |
| 245.01 |          0 |   9,036,577 |  33,780 |      311 |     311 |  62.2 |
| 250.01 |          0 |       1,923 |   1,923 |    1,923 |   1,923 | 384.6 |
| 255.01 |          0 |       1,847 |   1,847 |    1,847 |   1,847 | 369.4 |

After the event every counter carries the SAME delta, and that delta is the
flip count: one clear, one draw, one triangle and one vertex-shader batch per
frame. Per frame, submission falls from ~171 draws to exactly 1, and the frame
rate RISES 7.6x because there is nothing left to render.

That discriminates the three candidates the goals file asks about:

- NOT presentation selecting an empty surface. The scanout address keeps
  rotating through all three buffers and `presented nonzero=0` as well — the
  surface genuinely contains zeros.
- NOT rendering producing black from good input. Draws collapse in lockstep
  with triangles; there is no submitted geometry being lost.
- The guest's per-frame scene submission collapses to a single draw, and the
  loop free-runs.

"Frames kept presenting (28,800)" is therefore not evidence of health, which is
how the midday handover read it. They kept presenting at 7.6x the rate, and
that is the symptom.

### Why it happened, and why it no longer does

`0x001063CF` is an arm of the six-arm table in `sub_001063A0`. The container was
truncated by two spurious starts, and the midday handover printed the bytes:
`0010651D`/`0010651F` are an x87 `fstp st(0)` pair, the second being the second
instruction of a two-instruction sequence. `wt-icall`'s detector fix removes
both. Measured on the regenerated tree:

- starts `0x0010651D` and `0x0010651F`: ABSENT (both present in the crashfix
  bundle the player ran — checked with `nm`)
- `sub_001063A0` now spans `0x1063A0`–`0x106561`, which contains the arm
- the dispatch lifts to gotos: `if (_jt == 0x001063CFu) goto loc_001063CF;`

The same holds for the other two: `sub_000A5B60` is 1,348 bytes with
`POP32` edi/esi/ebp/ebx then `esp += 0x50` — the full 0x60 restored — and
`sub_00114A80` spans `0x114A80`–`0x114F90` with `goto loc_00114B66`. All four
spurious starts (`0x000A5BE9`, `0x00114D34`, `0x00114F7D`, plus the two above)
are gone. The trailing `RECOMP_ITAIL` in each is the default arm and cannot be
reached by an enumerated value.

Backlog, same tool the handover used: 196 dispatch sites through 84 tables and
607 unreachable arms BEFORE, 152 through 62 and 487 AFTER; affected tables 76 →
55. The classes the two fixes targeted — `short-table`, `clamped-by-neighbour`,
`arm-is-a-start` — are all now zero.

### Pre-registered, before the player's next session

The graffiti transition on the regenerated build should NOT go black, and the
run should contain no `[ICALL] Failed to resolve VA 0x001063CF`. If it goes
black anyway, the mechanism above is wrong and the free-running one-draw loop
is the thing to chase, not the ITAIL.

Supporting but NOT conclusive: a 200 s scripted gameplay run on the merged
build (state 30, `on=215`, `off=198`) produced zero ICALL/ITAIL failures and no
black frame after boot. It is not known to have reached the graffiti part.

## G1: the 2D dropout is 100 seconds, not two windows

Same log, `[APU-BIN]` against the same timeline. The handover's window
numbering caught only the tail of it.

- 2D climbs ~14,000/window from t=14, then FREEZES at 232,604 from t=94 to
  t=195 — 101 seconds — while 3D goes on climbing for much of it.
- 2D resumes t=195–210, then freezes at 260,998 to the end.
- 3D freezes t=130–175, resumes, freezes t=215–240, then emits one last burst
  of 13,713 in the window containing the black screen and stops.

So the two bins do NOT freeze simultaneously with the picture. Both had been
flat for 25–30 seconds BEFORE t=245, and the only bin event in the black-screen
window is 3D's final burst. The midday handover's "BOTH frozen, which coincides
with the black screen — so (a) and (b) are probably one event" does not survive
the timeline. `lost=0` and `starved=0 empty=0` throughout: it stops being
produced, not dropped.

## The recorder works, and it has now been exercised

`wt-padrec` shipped with one thing its ctest could not cover: that the frame
counter advances once per guest frame in a live session. It does.

- `[PAD-POLL] polls=` advances 1701, 1711, 1708, 1716 per report — flat, the
  120 Hz USB timer.
- `[PUSHER] flips=` advances 556, 458, 507, 419 over the same reports.
- The recorder's own `frame=11415` against `flips=11475` at the last report.

A 200 s run recorded 9,358 events over 11,814 frames. Replaying it:
`checkpoints ok=35 BAD=0`, `state=aligned in sync` at every report, the same
scene reached (`NtOpenFile` 1410, state 30, dwell 135.1 s against 138.4 s), and
the pad handed back at the end rather than going dead.

Re-recording the replay gives 1,709 differing lines of ~9,360 — and ZERO of
them involve a script-driven deflection. Every difference is the attached
controller's analog noise floor and run-length boundaries. The replay
reproduces the real input exactly.

One defect found in the harness around it: `play_scripted.sh` reports
`pad events fired: 0` for a padrec replay, because it counts schedule events
and a `#!padrec` file replaces pad state instead. The run played perfectly and
the gate reads as "no input". Do not score a replay on that line.

## The harness is audibly broken, and the counters say it is fine

Reported by the player while these two runs were playing through the speakers:
"making crazy bleeps and buzzing noises". The same runs report
`[APU-BIN] lost=0`, `[APU-PACE] starved=0 empty=0`, `off=198` retired voices —
every counter healthy.

`[APU-ADPCM] ok=37137602 fail=2195365 silenced=2195365` is 5.6% of blocks
silenced, with `hw_header OFF`. The player's `paths.conf` sets
`RECOMP_APU_ADPCM_HW_HEADER=1`; the harness does not, and `wt-switches`
deliberately did NOT promote that one. So the harness and the bundle differ on
exactly the switch that governs the failing path.

Two things follow. Audio quality has no instrument — the bins and the pace
counters measure delivery, not correctness, and they cannot tell garbage from
music. And any run not about audio should set
`SDL_AUDIODRIVER=no_such_driver`, which silences it.

## State

`ctest 78/78`, including `jsrf_gen_header_current` (the stale-header gate,
green only because the regeneration followed the merges) and
`jsrf_apu_idle_trap_lock_shipping`. `recomp_0004.c.o` references both
`_recomp_icall_fail_log` and `_recomp_itail_fail_log`, which is the
merged-without-regenerating discriminator the handover named.

`JSRF.app` rebuilt from the merged build. Discriminators, all four as expected:
`_sub_000A5BE9` absent, `_sub_0010651D` absent, `_sub_0010651F` absent,
`_recomp_itail_fail_log` PRESENT. The bundle's binary is stamped with the new
translator `c4400dbb0025f56d`; the crashfix bundle carries `b6f29140efaf1b21`.
The bundle's sha differs from `build-feav`'s binary because `install_name_tool`
and the ad-hoc signature rewrite the copy — use the stamp or the symbols, not
the hash.

The previous shared gen is preserved at `gen.pre-merge-20260919-KEEP`.

Coverage: my own interval measure reads 1,613,483 unique bytes against the
handover's gate of 1,614,033. It is NOT the same measurement — the gate's tool
is not named in the tree, and this regeneration merged the player's 12:28 icall
dump, which the handover's trees did not have, so the function sets differ by
construction. Not treated as a regression, and not treated as a pass either;
the structural checks above are what the two fixes were verified on.

---

# RESULT: the prediction held, 13:09-13:29

Appended after the player's session on the regenerated build, so the
prediction above and its outcome stay in one file. Preserved as
`last-run-2026-09-19_1329-NEWBUILD-PLAYER-GRAFFITI-KEEP.log` (17.7 MB) and
`padrec/graffiti-2026-09-19_1309.padrec.KEEP` (1.9 MB), both verified
byte-identical to the live files after the process exited.

|                     | morning, crashfix | afternoon, merged |
|---------------------|------------------:|------------------:|
| session length      |          307.02 s |        1209.00 s  |
| ITAIL/ICALL failures|                 1 |                **0** |
| black frames        |                63 |                **0** |
| guest faults        |                 0 |                 0 |
| ADX freezes         |                 0 |                 0 |
| voices on / off     |       168 / 163   |       **289 / 279** |

1,209 seconds is 4.9x the 245 s at which the crashfix build went black, and
the failure path did not fire once. `off=279` is the most voices this project
has ever retired in a session; the previous best was that morning's 163, and
before 19 Sep no scripted run had retired a single one.

That closes the black screen as an ITAIL stack-corruption event. It does NOT
close the three defects the player photographed, none of which the switch-arm
fix was predicted to touch:

  - TEXT. Glyph substitution AND variable leading-character loss. The
    substitution corroborates the existing note's section 5 rather than
    contradicting it: `Try it again` renders the `y` correctly while
    `ot too shabbS, kid.` renders the same letter wrong, minutes apart in one
    session. The truncation is new and may be the cheaper half --
    `llect 10 SpraS Cans` begins flush against the left edge of the window,
    which is what a line centred on a mis-measured width looks like. If a
    wrong glyph carries a wrong advance, both symptoms are one bug. The
    `Farside Stab Soul` -> `de Stab Soul` banner is NOT flush left, so that
    case is unexplained by this and may be a second mechanism.
  - GRAFFITI DOES NOT PLANT. New, in no handover. The sound fires and the can
    count decrements, and nothing is committed to the wall. The HUD keeps
    showing the spray-can graphic after the challenge completes.
  - SPRAY CANS. Partial, not absent: cans on the ground render correctly while
    others render as black silhouettes, and the blue/orange checkerboard
    rectangles in the 13:11-13:12 captures sit exactly where tags belong.
    Black, checkerboard or absent reads like one broken texture path rather
    than three faults.

## The glyph instrument may be blind to the defect

Checked before anyone spends a session on it. `ff_watch_vertex` stores
`inputs[0]` and `inputs[9]` (`nv2a_pb_exec.c:781`), so it has the right data,
but:

  1. It is a CHANGE detector. It prints only when a batch differs from the
     previous draw of the same batch ordinal, and only the first differing
     vertex. It cannot say which atlas cell a malformed letter sampled, which
     is the measurement actually wanted.
  2. It needs the font atlas address up front: `RECOMP_FF_BATCH_WATCH_TEX` is
     a hex texture offset and it early-returns unless
     `NV097_SET_TEXTURE_OFFSET` matches exactly. Nothing checks that the
     address given is the font.
  3. BOTH call sites (`nv2a_pb_exec.c:4026`, `:4060`) are inside the
     fixed-function branches. Neither is on the guest vertex-program path.
     This session ran 8,288,270 GPU vsh draws against 5,099,920 fixed-function
     batches, and nothing establishes which path draws the text.

If the glyphs go through a guest vertex program, the watcher reads "0 changes"
forever against a live defect -- the absence-measurement trap -- and that is a
candidate explanation for why it "initially produced misleading results".
Establish the path first, with a positive control beside it.

## Two smaller things this session settled

A lead REFUTED, so it is not chased again: 200,070 fixed-function batches
reported "left on the CPU (degenerate-normal shape)", and on a Metal run the
CPU rasteriser draws nothing -- which looked like it could explain missing
glyphs. It cannot. The same report reads `vsh draws: 1635392 GPU, 190963 CPU`
against `hw draws=1826355`, and 1,635,392 + 190,963 = 1,826,355 exactly. Every
one of those batches became a hardware draw; the CPU does the vertex transform
and Metal still rasterises. What IS real is that `nv2a_ff.c:571` says the
degenerate-normal rejection "is expected to be zero or near it" and that
archived runs all read "0 still rejected" -- it read 200,070, pushing 11% of
batches onto the CPU vertex path for nothing. A G21 item, not a correctness
one; `RECOMP_FF_GPU_NORMAL_ZERO=1` is the arm.

And the recorder writes NO end marker on a clean quit. 48,511 events over
59,110 frames were captured and the file is sound, but a replay of it will
report "NO END MARKER (the recorded run was killed or crashed)", which is
wrong and will mislead someone. The flush is on the crash handler and the
SIGTERM path; a normal window close reaches neither.
