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

The guest-CPU half is as large as all graphics work combined.

**Whether that 15 ms is compute or blocked waiting: measured 2026-09-14, and it
is blocked.** A 10 s `sample` of the live process at gameplay, top-of-stack:

| samples | frame | |
|---|---|---|
| 330 | `__semwait_signal` | blocked |
| 271 | `__psynch_cvwait` | blocked |
| 209 | `semaphore_wait_trap` | blocked |
| 205 | `__workq_kernreturn` | idle workers |
| 158 | `mach_msg2_trap` | blocked |
| 68 | `sub_0013B180` | **guest compute** |
| 11 | `nv2a_vsh_execute` | host compute |
| 8 | `_platform_memcmp` | host compute |
| 5 | `draw_primitive` | host compute |

≈93% blocked against ≈100 samples of real compute. That reproduces the earlier
"93% blocked" profile, but **at gameplay** rather than at the title, which is
what was actually in doubt.

The one large guest entry, `sub_0013B180`, is a spin on `0x0025EFC0` that
increments a counter at `0x0025EFA8` while it waits. Its exit path pushes
`0xF0000001`, writes `0x0025EFC4 = 1`, calls `sub_00147E4E` and ends in an
`int3` — the shape of a watchdog or assertion, read statically from the
generated C and **not** confirmed by a run.

So the remaining question is not *whether* the guest is waiting but *what for*.
Note the chain under the hot guest frame: `kernel_thunk_dispatch` →
`bridge_KeWaitForSingleObject` → `Sleep` → `nanosleep`, i.e. the wait is a
host sleep-poll. Poll *granularity* has already been settled as noise
(±2.4%, paired by `live=`); this is about what the guest is waiting on.

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
