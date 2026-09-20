# The audio defect has a silent, unattended reproducer

20 September 2026, evening. The player asked for rapid automated progress on the
defects they can see. This is the audio one. Nothing here needs a person at the
machine, and **no sound is ever played**: every run sets
`SDL_AUDIODRIVER=no_such_driver`, the log confirms `backend=none`, and the
waveform is taken from `RECOMP_APU_WAV`, whose tap is deliberately placed where
it "exists even when none is" (`apu_core.c:557`).

All runs use `diagnostics/jsrf_first_fault/stage_harness` and adopt the player's
own `paths.conf` switches — 17 of them, `RECOMP_METAL_FF=1` included. Binary
`92ad42df…b897d`.

## What is established

| run | what it did | dropouts after the lead-in |
|---|---|---|
| audio-02 | stood still in the Garage, 335 s | **none**; audio continuous |
| audio-04 | driven continuously, roamed the city, 520 s | **88**, first at **t=132 s** |
| audio-05 | driven continuously, stayed near the spawn, 368 s | **93**, first at **t=217 s** |

A dropout is *absolute* digital silence — every sample zero — lasting 0.05 s to
2.45 s, measured at 50 ms resolution. Once they start they continue to the end
of the run. audio-04's onset at 2.2 minutes is the player's own "about two
minutes into every session".

**Standing still does not do it; playing does.** That is the whole difference
between audio-02 and audio-05: same scene, same duration, same switches.

## Three explanations eliminated, each with its counter's trigger read first

- **The engine stops.** No. `out_frames` is incremented in
  `mcpx_apu_monitor_frame` *before the sink is chosen*, so it is valid with
  `backend=none`, and it held 48,000 Hz unbroken across every run including
  through the dropouts themselves.
- **It decays when idle.** No. audio-02 played continuously for 335 s.
- **The ADX decoder thread wedges.** No. `[ADX] tick=` rose monotonically
  through every dropout in every run. The thread keeps running while the mix
  is silent. (A heartbeat is not proof it decoded anything — it proves the
  thread is not stuck.)

## What is NOT established, and the trap in it

The mechanism. The obvious candidate — outstanding voices (`on` minus `off`)
growing until something is exhausted — **does not fit**: audio-05's outstanding
count climbs 9 → 17 across its onset, while audio-04's sits flat at 5–8 and
drops out earlier. One run's trend is not a mechanism, and a theory that covers
one arm and not the other is not a finding.

Two measurement traps were hit and are recorded so the next session does not
repeat them:

1. **The driver's own timeout looked exactly like the defect.** A first attempt
   showed 42 s of unbroken silence at the end of a run and it was nearly
   reported. The movement driver's budget had expired eleven seconds earlier;
   the position trace froze at the same moment. Cross-check the audio timeline
   against the state trace before believing any silence.
2. **One-second buckets invented a period.** At 1 s resolution the dropouts
   looked like a clean ~19 s cycle. At 50 ms there are four times as many and
   no period at all. Measure at the resolution of the thing being measured.

## What the sink cannot be asked

`backend=none` means every SINK counter — `gen_hz`, `starved`, `queued`,
`min_queued` — reads zero by construction and is void. A defect that lives in
device delivery is invisible to every run above, and the samples reaching the
WAV would look perfect while the player hears nothing. Testing that needs a live
backend. `SDL_AUDIODRIVER=dummy` is SDL's null output: it opens no hardware and
produces no sound, while still exercising submit/queue/starvation. **The player
said "no sound" and has not been asked about `dummy`; do not assume it.**

## What the voice tracer added (run audio-06, `RECOMP_VOICE_LIFECYCLE=1`)

The tracer stamps every event with `g_apu_out_frames`, which is the **same clock
the WAV is written on**, so voice events align to the exact sample where silence
starts. 1,489 events, 628 completed voice spans, 115 dropouts, first at t=54 s.

**The dropouts are not music breaking up. They are the gaps between sound
effects, with nothing underneath.** At the onset only one voice is alive at a
time -- voice 3, then 1, then 2 -- each living 0.3-0.6 s, and the silence is
simply the interval between them. Whatever was playing continuously before is
no longer producing.

**Voices 64-67 are started once at `audio_frames=256` and never receive another
event.** That is the shape of persistent streaming channels and is not in itself
a fault. Voice 68 cycles on/off-command/retire/idle repeatedly, then takes a
final `on` at t=27.6 s and is never retired for the remaining 340 s.

**The rate that separates a clean run from a dropping one is idle-voice traps
per voice, not per run:**

| run | idle traps | voice spans | traps per span |
|---|---:|---:|---:|
| audio-02, no dropouts | 9 | 14 | 0.6 |
| audio-06, 115 dropouts | 9,213 | 628 | **14.7** |

Twenty-three times as many traps for each sound played. A voice raises an idle
trap when it runs out of data, so on the dropping run every short effect is
running dry roughly fifteen times inside its own 0.4 s lifetime. The edge
counters for the same run read `edge_would=216851 edge_suppressed=207851
rearm=203 reraise=9000 of 9213 raises`.

**Do not read that as the cause yet.** More sounds do mean more traps; the
normalisation above is what makes the comparison meaningful, and it is two runs,
not a series. The busiest voices (v3=6453, v6=2239) are effects voices, not the
streaming channels, so this says effects are starving and does not yet say what
happened to the music.

## THE RESULT: one of the player's own switches causes it

Nine runs, interleaved so host drift could not land on one group, every one
367 s of identical heavy driving at the same scene, all silent.

| condition | runs | music died | first dropout |
|---|---:|---|---|
| the player's `paths.conf` switches (17, adopted) | 6 | **6 of 6** | 27, 28, 72, 73, 76, 159 s |
| runtime defaults (`--bare`) | 3 | **0 of 3** | never, over 365 s each |

No overlap between the groups.

**The controls that make this worth acting on.** The bare runs are not quiet
runs: they carried the two highest voice-activity counts of the entire series —
1,109 and 1,107 voice spans, 42,750 and 35,467 idle traps — and still never lost
the music, while a player-switch run that died had 453 spans and 6,307 traps.
So the effect is not activity, and **the idle-trap-rate theory this document
entertained earlier is dead**: traps run four to seven times higher in the arms
that never break.

`RECOMP_APU_TRAP_THREADS=0` alone does not prevent it (69 dropouts, first at
72.4 s), so it is not that switch by itself. Sixteen candidates remain.

**What this does NOT license.** `first_dropout` scatters 27–159 s, so only the
*presence* of dropouts discriminates, never the timing — do not read a later
death as an improvement. And a switch that causes this may be load-bearing
elsewhere: this corpus records switches turned on for good reasons and backed
out for others. The bisect yields a name to investigate, not a switch to flip.

## Round 2: the culprit is in the nine non-APU switches

Subtracted from the player's real configuration, not compared against a bare
runtime, so everything else stayed fixed. Interleaved.

| arm | forced to 0 | dropouts |
|---|---|---:|
| `noAPU-a` / `noAPU-b` | the 8 APU switches | **429 / 470** — far worse than any other arm |
| `noOTHER-a` | the 9 non-APU switches | **0** |

**The APU eight are protective, not causal.** Removing them roughly quadrupled
the defect, which is what one would hope of switches added as audio fixes, and
it is a second independent refutation of the idle-trap-rate theory: those arms
carried 40,115 and 40,820 traps.

So the culprit is one of `IRQ_LATENCY, IRQ_THREAD, KERNEL_THREADS, METAL_FF,
METAL_NO_DEPTH_SYNC, NV2A_PMC_UPMIRROR, VSH_DP_ZERO, WILD_PTR,
WILD_PTR_SELFTEST`. The first three are the ones worth suspecting: a streaming
voice that runs dry raises an idle trap serviced through an interrupt, so
switches that move interrupt and thread scheduling are what would make a refill
arrive late. **That is a prediction, not a finding, and round 3 tests it.**

## Round 3: the scheduling prediction was WRONG

`noTHREADS-a` — `IRQ_LATENCY`, `IRQ_THREAD` and `KERNEL_THREADS` forced to 0,
everything else the player runs kept — **still died**: 57 dropouts, first at
159.3 s. `noOTHER-b2`, the valid re-run of the void arm, was clean (0 dropouts,
459 spans), so removing all nine still cures it and the culprit is one of the
remaining six:

    METAL_FF  METAL_NO_DEPTH_SYNC  NV2A_PMC_UPMIRROR
    VSH_DP_ZERO  WILD_PTR  WILD_PTR_SELFTEST

The late-interrupt story was reasonable and it is dead. Written here because it
was recorded before the run, which is the only thing that makes a wrong
prediction worth anything.

**The candidate that replaces it, from this repo's own rules.** CLAUDE.md:
"A guarded MMIO page loses writes if anything else unprotects it... this cost
13.5M windows in 45 s and swallowed every `VOICE_ON`." `NV2A_PMC_UPMIRROR` is
the NV2A aperture double-mapping switch. If it perturbs the guard on the MCPX
aperture, guest `VOICE_ON` writes complete silently against RAM: the game starts
a voice, the write is swallowed, and the music stops while every engine counter
stays green — which is precisely the shape the voice tracer recorded. **Also a
prediction, also recorded before the run.**

## Searching faster, and why it is allowed

Across 14 runs the outcome is binary with no overlap: clean arms score exactly
0 dropouts, failing arms 69 to 470. The latest onset ever observed is 158.7 s.
So during the search a single 200 s arm classifies a switch, and replication is
spent on the final answer instead of on every arm. That took a round from ~30
minutes to ~14.

## A void arm, and the guard it produced

`noOTHER-b` reported `0 dropouts` and was nearly counted as a second clean run.
It never left the title screen — `Navigation deadline at leave-title`, the boot
lottery this corpus already records at 8 of 10 — so its WAV held no gameplay and
its `spans=0, traps=0, p50=3.5 ms, fps=279` gave it away.

The cause was a change made earlier the same day: `run.py` now serves the
debugger window **after a failed step**, which is right for debugging and wrong
for a batch, because waiting for `DEBUG READY` stopped proving the scenario
passed. A sweep must read `result.json`'s outcome. All 13 other arms were
audited and every one reached the Garage.

## The bisect, and why it is valid

`recomp_switch_on` reads `v && v[0] && strcmp(v,"0") != 0`
(`src/recomp_switch.h:41`), so `RECOMP_X=0` genuinely disables, and in the stage
harness an explicit `--env` beats `paths.conf` adoption. The halves can
therefore be subtracted from the player's real configuration rather than
compared against a bare runtime, which keeps every other variable fixed:

- 8 APU: `ADPCM_HW_HEADER CYCLE_BREAK FEDEC_HOLD IDLE_HANDOFF_GUARD
  IDLE_TRAP_EDGE LIST_MOVE_TO_FRONT SELFLINK_END TRAP_THREADS`
- 9 other: `IRQ_LATENCY IRQ_THREAD KERNEL_THREADS METAL_FF METAL_NO_DEPTH_SYNC
  NV2A_PMC_UPMIRROR VSH_DP_ZERO WILD_PTR WILD_PTR_SELFTEST`

Two runs per half, interleaved. About three more rounds reaches one switch, all
unattended, roughly 90 minutes.

## Frame time, taken for free at a fixed scene

Every arm reports `p50`. Across the sweep it ranged **16.0 to 21.5 ms** with
scene and activity alone, on the player's own render path. Any frame-time A/B
that does not hold the scene and the activity fixed is measuring that spread.
The whole-run corpus figure of p50 19.5 ms was taken on the CPU fixed-function
path and does not compare.

## The next measurement





Not another theory. Two things the runs above can answer cheaply:

1. **Does the trap rate per voice track the dropout rate across a series?**
   Two runs is a contrast, not a trend. Five runs of varying activity, plotting
   traps-per-span against dropouts-per-minute, either establishes it or kills
   it. Entirely unattended, ~6 minutes each.
2. **What are the streaming channels (64-67) doing during a dropout?** The
   lifecycle tracer is silent about them by construction -- they never generate
   an event after startup -- so it cannot see the music stop. `[VOICE-TOP]`,
   `RECOMP_VOICE_RATES` and the mixdown counters report per-voice activity and
   would.

The `RECOMP_APU_IDLE_TRAP_*` family already in this tree exists because this
area has been worked before; read the G-numbered audio entries in the goals
corpus before treating any of the above as new.

## Reproducing

```sh
python3 diagnostics/jsrf_first_fault/stage_harness/run.py \
  --scenario diagnostics/jsrf_first_fault/stage_harness/garage.json \
  --binary <build>/jsrf_first_fault \
  --game "$HOME/Library/Application Support/JSRF/game" \
  --hdd "$HOME/jsrf-build/emulated-hdd-warm" \
  --out /tmp/jsrf-audio-NN --env SDL_AUDIODRIVER=no_such_driver \
  --env RECOMP_APU_WAV=/tmp/jsrf-audio-NN.wav --debug-seconds 330
```

Then drive it — any continuous movement will do; standing still will not
reproduce — and measure the WAV at 50 ms resolution. Evidence for the runs above
is in `/tmp/jsrf-audio-0{2,4,5}` with their WAVs beside them.
