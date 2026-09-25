# JSRF goals — finish the Direct3D lift, 25 September 2026 (evening)

Supersedes `JSRF_GOALS_2026-09-25_DAY_FROM_PLAYABLE_TO_POLISHED.md` for
ORDERING. That file keeps G70–G72 and the lift's measurements so far; the open
items there (G71 and G65 need xemu captures, G63 needs the Poison Jam chase,
G69 has its post-mortem armed) stand behind this.

**Where the lift is.** JSRF.app is built with `JSRF_APP_LIFT=1` (ea98e36),
which turns on `RECOMP_D3D8_HOST_2D/FF/VS=draw` and `RECOMP_D3D8_HOST_FF_GPU=1`.
First player session with it (17:45, `measure/player-2026-09-25-lift-on.log`):
"seemed good", no fault. It came out at 21.8 ms mean (46 fps), p50 19.5, against
17.5 ms in the executor session before it. Those are not scene-matched, and the
agent's scene-matched arms had the lift faster on all four stages. The log
names where the lift stands:

| cost | in the 17:45 session |
|---|---|
| binding the target, one GPU drain each | 12.6 s over 8,157 binds, ~1.8 ms a frame |
| flip read-back (wait, copy the surface to guest RAM, upload it again) | every flip |
| points and lines left to the executor | 214,263 (~30 a frame) |
| texture shader modes left to the executor | 25,650 (~3.5 a frame) |
| draw-thread pipeline builds | 74, 496 ms, worst 82 ms (first lift session) |
| host coverage | 91% of rasterised batches (97–98.5% in harness free play) |

## Order

1. **G73 — the host owns the render target and presents.** Remove the
   per-bind GPU drain and the flip read-back when the host drew the frame.
   The step-2 attempt (`parked/step2-async-flip`) broke the picture: the flip
   stopped writing the bound surface back to guest RAM, and something after
   the flip (probably the swap's copy quad into the display target) reads it.
   Find that reader first. `RECOMP_METAL_DEFER_SWAP` alone removed the bind
   drain but the flip drain grew by the same amount. Done when bind plus flip
   waits are near zero, the frame time falls by that much, and
   `gametools/frame_match.py` scores the host arm against the executor at
   motion level (~2–15%) on Garage, Rokkaku 2:40, Shibuya 2:10, Sky Dino 6:60
   and one cutscene.
2. **G74 — points and lines on the host.** ~30 draws a frame, most of the
   remaining 9%. Name what they are (particles, trails), draw them on the
   host, and check frame_match plus a per-draw look.
3. **G75 — texture shader modes and refused formats on the host.** Bump env
   (G60's displacement must survive), cube, volume; the texture formats
   refused today (16-bit swizzled 0x02/0x05, linear 32-bit 0x12/0x1E, G59).
   Goal: no draw changes path mid-frame.
4. **G76 — measure what is left.** With the executor out of the frame, split
   the frame into guest D3D bookkeeping, host encode (~10–18 µs a draw) and
   GPU. That decides whether 60 fps in Shibuya and Sky Dino needs D3D's own
   functions lifted.
5. **G77 — make the lift the default.** After 1–3 and a scene-matched player
   session: the app's default, the executor as the fallback switch.

## Rules for this phase

- **One game at a time.** Agents share the machine: take
  `~/jsrf-build/runs/.game-lock` (mkdir) before launching, remove it after.
  Launch nothing while `pgrep -x jsrf-engine` shows the player playing.
- **Whole-frame A/B, not only VERIFY.** VERIFY and the glitch watch both
  passed the frozen-frame experiment; frame_match caught it.
- **Scene-match every number.** A player session is compared with a harness
  arm on the same stage, or not at all.
- **Nothing reaches JSRF.app untested.** Experiments stay on branches; the
  player's bundle is rebuilt only from verified commits.

## G73 result (25-26 Sep, night)

Branch commits 7221c09, cf1394f, aae7be1, fe47c09, the VERIFY_FROM commit and this one.

**The reader.** A per-draw flight capture around a flip (7221c09 adds each
draw's target, host/executor, and whether its textures were behind the GPU)
shows every frame with the same shape. Draw 0 is D3D's swap copy quad, drawn
by the executor because D3D issues it, not the title. It samples the back
buffer 0x5F0000 (linear R5G6B5) into one front buffer, 0x688000 or 0x71E000,
alternating. Then comes the scene into 0x5F0000. The FLIP_STALL names the
other front buffer. The copy quad reads 0x5F0000 out of guest RAM at the
start of the next frame, and the flip's sync of the bound surface was the
only thing that wrote it there. The parked async flip removed that sync, so
the copy quad drew last frame's back buffer.

**What absorbed DEFER_SWAP's saving.** Two things, one behind the other:
- The batch is committed only at a sync. With the bind drain gone, the
  scene's command buffer went to the GPU at the flip, and the flip waited for
  all of it. CPU and GPU never overlapped.
- Once `RECOMP_METAL_ASYNC_WRITEBACK` removed the waits (cf1394f), the frame
  still barely moved: the pusher sat idle waiting for the title
  (`[STAGE] idle`, Shibuya 3.1 -> 8.0 ms). The title's thread was spending
  12-24 ms a frame in the mirror's draw hooks, 11-22 ms of it one
  byte-per-multiply vertex hash (a diagnostic). aae7be1 cut the hooks to
  0.8-2.3 ms a frame.

**The switch** (default off; the executor-only path is unchanged): a deferred
swap blits the outgoing surface to a shared buffer behind its draws. The debt
stays, and paying it means copying that buffer out, waiting only if the blit
has not run. The copy quad samples a GPU blit of the bound back buffer. The
flip commits the frame and makes current only the range it names. Unit arms:
`jsrf_metal_async_writeback_on/off`.

Measured with FLIP_PACE=0, map_run free play, two rounds (r1 / r2). The
"before" arm is the lift as on main (before binary, or
`RECOMP_D3D8_HOST_BISECT=8192`). The "after" arm adds
`RECOMP_METAL_ASYNC_WRITEBACK=1` on the new hash. Waits are ms a frame:
bind = the host's bind, flip = `[STAGE] sync`.

| stage | arm | mean | p50 | p90 | bind | flip |
|---|---|---|---|---|---|---|
| Garage 1:00 | executor | 18.17 / 18.55 | 18.0 / 18.5 | 20.5 / 22.0 | - | 3.83 / 3.93 |
| | lift before | 16.51 / 16.54 | 16.5 / 16.5 | 17.0 / 18.0 | 1.34 / 1.69 | 4.40 / 4.41 |
| | lift after | **9.60 / 9.63** | 9.0 / 9.5 | 11.5 / 12.0 | 0.02 / 0.02 | 0.02 / 0.03 |
| Rokkaku-dai 2:40 | executor | 22.73 / 20.98 | 23.0 / 21.0 | 26.0 / 24.5 | - | 3.67 / 3.52 |
| | lift before | 18.64 / 16.92 | 17.0 / 17.0 | 21.8 / 18.2 | 1.81 / 1.76 | 3.31 / 3.56 |
| | lift after | **10.78 / 11.33** | 10.5 / 10.5 | 12.0 / 13.0 | 0.02 / 0.02 | 0.02 / 0.02 |
| Shibuya 2:10 | executor | 28.95 / 28.69 | 28.5 / 28.0 | 32.0 / 32.0 | - | 2.84 / 2.80 |
| | lift before | 25.76 / 25.65 | 25.5 / 25.5 | 27.0 / 26.5 | 2.51 / 2.57 | 3.63 / 3.44 |
| | lift after | **18.13 / 17.80** | 17.5 / 17.5 | 20.5 / 19.0 | 0.02 / 0.02 | 0.03 / 0.03 |
| Sky Dino 6:60 | executor | 35.46 / 37.14 | 35.0 / 37.0 | 39.0 / 40.5 | - | 4.36 / 4.41 |
| | lift before | 28.75 / 28.39 | 27.5 / 27.5 | 33.0 / 32.0 | 2.26 / 2.03 | 4.81 / 4.55 |
| | lift after | **21.22 / 20.19** | 21.0 / 20.0 | 23.2 / 21.0 | 0.02 / 0.02 | 0.03 / 0.03 |

The new hash alone, without the switch, gives Garage 14.86, Rokkaku 15.44,
Shibuya 22.84 and Sky Dino 27.97: the pusher then waits on the GPU again. The
switch then takes off 4.4-7.3 ms, about the waits it removes. At a flip, the
async write-back waited 0.07 ms on average (worst 3.7 ms). Pusher idle is ~0
in the after arm: the pusher is now the bottleneck (G76).

**Pictures.** frame_match against the executor needs scene matching.
`RECOMP_FLIGHT_AT` names a guest frame, and the jump fires at a different
one in every arm, so a radio line or a crowd scored 12-24% in the before
arm as well as the after. `RECOMP_FLIGHT_AT_FREEPLAY=900` (fe47c09) fixes
the moment. Cutscenes are paced (`RECOMP_FLIP_PACE=1`) or cut at the
event's end (`RECOMP_CHAPTER_JUMP_MARK=1`). Results:

| scene (48 frames, 150 for the cutscene) | after vs executor: median / worst | before vs executor | after vs before |
|---|---|---|---|
| Garage 1:00, free play +900 | 0.00 / 0.00% | 0.00 / 0.00% | 0.00 / 0.00% |
| Rokkaku-dai 2:40, free play +900 | 0.78 / 1.53% | 0.78 / 0.83% | 0.53 / 1.16% |
| Shibuya 2:10, free play +900 | 0.43 / 0.47% | 0.43 / 0.48% | - |
| Sky Dino 6:60, free play +900 | 0.40 / 1.02% | 0.53 / 0.58% | 0.50 / 1.16% |
| Chapter-2 intro (2:96, e210's last 150 frames, paced) | 4.85 / 7.07% | 5.01 / 7.95% | 7.03 / 9.41% |

Every frame of every arm pairs with a distinct executor frame (47-150 of
47-150), so none is frozen. In Garage, after and executor differ by more
than 8 in only 0.001% of pixels; the rest is 1-LSB rounding. Unmatched, the same arms
scored 3-24%, the before arm included, which is the reason for the
trigger.

**VERIFY=120** (free play, per-flip lines; after / before):

| stage | draws compared | mismatching | px over tolerance a flip | max error, median flip |
|---|---|---|---|---|
| Garage | 4,376 / 1,925 | 2.3% / 2.3% | 7 / 7 | 16 / 16 |
| Rokkaku-dai | 5,067 / 3,158 | 0.8% / 0.8% | 2 / 2 | 20 / 20 |
| Shibuya | 3,328 / 1,792 | 12.0% / 12.6% | 214 / 396 | 41 / 56 |
| Sky Dino | 3,072 / 1,792 | 7.9% / 6.8% | 257 / 221 | 46 / 46 |

VS draws: 0 mismatching in every arm. No verify flip over 10k px in any
free-play arm. Shibuya and Sky Dino match the earlier lift runs (10.8% and
7.8%, 25 Sep afternoon), and nearly all of the mismatching draws are fixed-function, as they were then.

VERIFY runs lose the harness's title input: 13 of 15 first attempts missed the jump, all ten
after-arm attempts among them. A verify flip stalls up to 1.8 s, and an
unpaced title reaches one every 0.4 s. `RECOMP_D3D8_HOST_VERIFY_FROM=<flip>`
starts VERIFY after the title. The glitch watch wrote the same captures in
every arm of a stage: none in Garage, Rokkaku or Shibuya, 12 in Sky Dino in
the executor, before and after alike, and 1 in the chapter-2 intro (at the
Garage, before the jump) in each.

**Ready for the player?** Not yet. The numbers and pictures are in, and
every intercepted reader of guest RAM is paid. What it has not had is a player session. It
changes when guest RAM becomes current: DEFER_SWAP's exposure to a guest
CPU read that nothing intercepts. Only the four stages in free play and the
chapter-2 intro have been compared. To try it: build JSRF.app from
this branch with `RECOMP_METAL_ASYNC_WRITEBACK=1` added to paths.conf, or to
the `JSRF_APP_LIFT` block in make_app.sh. aae7be1 (the hash) is lift-only
and safe on its own. It reaches the app with the next bundle build either
way.
