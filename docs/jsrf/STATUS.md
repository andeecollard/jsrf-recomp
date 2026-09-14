# JSRF on xboxrecomp — status

Last measured 2026-09-14. Every number here came from a run; nothing is
estimated. Where something is unknown it says so.

## Where the title has got to

Jet Set Radio Future (US) **boots, renders, presents, plays music, accepts a
controller, reaches gameplay and is playable** on macOS ARM64 (Apple M1 Max,
Metal). It has been played past the opening tutorial to the "Collect 10 Spray
Cans" objective with a DualShock 4 over Bluetooth.

It is not yet *enjoyable*, for reasons measured below.

## What works

| | evidence |
|---|---|
| Boot to gameplay, unattended | 0 guest faults over 150 s runs |
| Rendering | 700k draws, 68.7M triangles in 150 s; VSH 314,598 batches with 164 rejected (0.05%); 697,789 textures prepared, 0 rejected |
| Audio output path | SDL2 at 48 kHz native, `gen_hz` 48003, never starved, queue never empty |
| Music | ADX streaming, correct pitch, correct content |
| Controller | PS4 pad over Bluetooth; 44% of polls non-neutral during play |
| File I/O, saves, EEPROM, clocks, DPCs, events | real host implementations, audited 2026-09-14 |
| Tests | 21/23 ctest; the 2 failures are deliberate (see 05e0465) |

## What does not

| | measured | reference |
|---|---|---|
| **Frame rate** | 11.7–14.3 fps (attract), 31.3 fps (gameplay) | xemu holds **59.2–60.1** |
| **Audio engine uptime** | ~52% during real play — the rest is lost to an APU front-end trap storm | n/a |
| **Music ring** | ~23% of the buffer is the previous lap replayed | xemu clean |
| First 29.3 s | digital silence | xemu plays a logo chime in its first 10 s |
| FMV | no decode on this host at all (`video_player.c` is Media Foundation behind `#if defined(_WIN32)`) | — |
| APU DSP (GP/EP) | stubbed, passthrough — no hardware reverb or effects | — |
| Stability | ~13% of runs SIGSEGV in the OHCI path; controller hotplug aborts the process | — |

**The frame rate is the headline.** JSRF is a 60 fps title and era-typical
fixed-step simulation means half the frame rate is half the *game speed* — so it
does not merely look choppy, it runs in slow motion.

## Frame budget, gameplay

```
vsh 7.52 | submit 8.41 | GPU sync 0.98 | rest (guest CPU) 15.03  = 31.94 ms
                                                     budget for 60 fps = 16.67
```

The guest-CPU half is as large as all graphics work combined. Whether that
15 ms is compute or blocked waiting is **unresolved** and is the single most
valuable thing left to measure.

## Host support

| host | state |
|---|---|
| macOS ARM64 + Metal | primary; everything above |
| Windows (mingw/CrossOver) | renders and reaches gameplay; used as a differential oracle, because macOS's APU trap is write-only and cannot exercise the model's read path at all |

## What to fix next

See [ACCURACY_GAPS.md](ACCURACY_GAPS.md). Short version: build flags first
(`-fwrapv -fno-strict-aliasing`, drop `-w`, then a UBSan run), then two small
kernel bridges, then settle whether the guest's 15 ms is compute or waiting.

## A caveat about this project's own measurements

Every scripted pad schedule in this repository parks the player. Nothing skates,
nothing grinds, no sound effect ever finishes, and no voice is ever retired
(`on=5 off=0 idle_trap=0` in every scripted run ever taken here). Two of the
defects listed above were invisible to the test harness for the life of the
project and were found in minutes by a person holding a controller. Treat any
audio or performance conclusion drawn from a scripted run as provisional.
