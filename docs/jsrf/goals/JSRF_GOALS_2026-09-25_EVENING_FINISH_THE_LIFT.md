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
