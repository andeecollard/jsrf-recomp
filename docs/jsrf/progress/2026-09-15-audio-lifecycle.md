# Audio lifecycle investigation — 15 September 2026

## Established from the saved run

`/Users/andrewcollard/jsrf-build/jsrf-first-fault/play/20260915-110630/stderr.log`
contains the reported v3 storm. Its final voice report is on=83, off=79,
release=0, idle_trap=19585, fe_methods=66514, set_current_voice=10045;
v3 accounts for 19552 idle encounters. This establishes repetition, not
software-object ownership or the cause of failure to unlink.

`off` counts calls to `voice_off`, including envelope and sample exhaustion,
not just guest VOICE_OFF. `fe_methods` includes internal SE2FE_IDLE_VOICE.
These were not suitable evidence of explicit guest teardown or continued
method submission, respectively.

## Instrumentation added

- `guest_methods`: arrivals at `mcpx_apu_vp_write`; internal idle events do
  not increment it. This is method traffic, not all APU register writes.
- `off_commands`: explicit VOICE_OFF commands, separate from retirements.
- `RECOMP_VOICE_LIFECYCLE=1`: opt-in ON, OFF command, RELEASE command,
  retirement and first idle encounter after each ON, including hardware
  state, next handle, FECTL and FETFORCE1. Repeated idle encounters are
  suppressed so a storm does not bury its initiating transition.

The trace runs before each named operation. `audio_frames` is a capture
position, not elapsed wall time. It does not observe guest allocations or
frees. No audio behavior or trap policy was changed.

## Guest code paths to probe next

Current generated `recomp_0008.c` shows:

- `sub_001A241F` skips handling if the handle is >=256, `[this+0x2C0]`
  is nonzero, or the hardware voice remains active.
- `sub_001A200D` looks up `[this+handle*4+0x2C4]`, then checks its selected
  handle and whether its software list node is self-linked before unlinking.
- `sub_001A2E2E` reads software previous/next links and updates the hardware
  list. A NULL object or bad software link can lead to the observed low-address
  dereference. Stack membership and last trap handle alone do not establish
  that the object was freed.

These are static paths, not observations of why v3 was skipped. A next guest
probe should capture the lookup pointer and these early-return conditions,
then follow the writer that changes that pointer/list. Do not suppress a trap
or forcibly unlink a voice based on the freed-object hypothesis alone.

## Validation

Built an isolated executable in `/tmp/jsrf-audio-investigation-build` against
existing generated sources without regenerating or modifying the gen tree.
`jsrf_apu_register` passes with the lifecycle trace both enabled and disabled.
Its assertions establish that an internal idle encounter does not increment
guest method arrivals, and an explicit OFF increments the command and
retirement counters. The opt-in trace also prints the expected idle,
off-command, retire sequence.

An initial sandbox run had no audio/display devices and was stopped. It is
not an audio reproduction. A separate desktop run uses live SDL audio,
Metal and a disposable HDD copy; its log is
`/tmp/jsrf-audio-lifecycle-desktop/stderr.log`.

## Desktop reproduction result

The 150-second run reached 1410 NtOpenFile log entries and reproduced a trap
storm. Last periodic report: on=61, off=54, off_commands=52, idle_trap=3208,
guest_methods=43205; busiest handles v3=2253, v5=629, v71=288. No guest crash
was recorded. Guest method traffic continued, so this run does not reproduce
the later complete traffic freeze in the supplied run.

The new observation is self-linked inactive hardware voices, following
explicit OFF and reuse. Selected trace entries (positions are output frames):

| seq | frames | event | voice | next |
| --- | --- | --- | --- | --- |
| 79 | 5441792 | off-command | 5 | FFFF |
| 80 | 5441792 | retire | 5 | FFFF |
| 81 | 5441792 | on | 5 | FFFF |
| 82 | 5448704 | off-command | 5 | 0005 |
| 84 | 5448704 | on | 5 | 0005 |
| 86 | 5453824 | off-command | 5 | 0005 |
| 88 | 5453824 | idle | 5 | 0005 |
| 92 | 5695488 | off-command | 3 | 0006 |
| 94 | 5695744 | on | 3 | 0006 |
| 98 | 5700864 | off-command | 3 | 0003 |
| 101 | 5700864 | idle | 3 | 0003 |

The idle observation establishes that the self-link is in the list being
walked, rather than merely an unlinked voice's initial contents. OFF before
idle is now directly observed for these handles. Freeing a guest object is
still unobserved. A likely next test is whether ON inserts a voice that is
already linked; capture FEAV, list heads and the guest unlink decision around
OFF/ON. Do not treat a self-link printed before the first ON as evidence of
corruption: the trace intentionally precedes insertion.

The diagnosis has therefore narrowed to list ownership/reuse and unlink
ordering. No speculative automatic unlink or trap suppression was applied.
