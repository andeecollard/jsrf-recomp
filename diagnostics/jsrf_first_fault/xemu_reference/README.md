# xemu reference rig

xemu is configured for the **US** ISO — the same title this port targets — so it
is the only oracle available here that is neither our runtime nor static
reading. These are the host-side tools for asking it questions.

Nothing here touches the recomp. All three are read-only against a running xemu.

| tool | question it answers |
|---|---|
| `capture_reference.sh` | what should this scene look and sound like? |
| `mode_sampler.py` | how does a guest value move over time? |
| `mode_finder.py` | which guest values changed between two game states? |

## capture_reference.sh

Video via avfoundation, plus **sample-exact** audio taken straight off the APU
through SDL3's disk driver — no host mixing, no resampling, no loopback driver
to install.

```sh
./capture_reference.sh <outdir> <seconds>
```

Three things that cost a take if you get them wrong:

- `SDL_AUDIO_DISK_TIMESCALE` is a **direct multiplier, not a percentage**. The
  plausible-looking `100` runs ~100x too slowly. `1` is realtime.
- The disk driver takes audio **off the speakers** for that run. Expect silence;
  it is not a fault.
- `caffeinate -dimsu` wraps both xemu and ffmpeg. Without it the Mac locks
  partway and the rest of the capture is the lock screen.

`-audiodev wav` does **not** work — xemu exits with "no default audio driver
available". APU output goes through SDL, not QEMU's audiodev.

## Gated method traces

Not a script, because the whole point is to enable them late. Launch with a
monitor socket, then open the gate only once you are in the scene you want:

```sh
xemu -monitor unix:/tmp/xemu-mon,server,nowait -trace file=pgraph.trace
printf 'trace-event nv2a_pgraph_method on\n'  | nc -U /tmp/xemu-mon
sleep 2
printf 'trace-event nv2a_pgraph_method off\n' | nc -U /tmp/xemu-mon
```

Two seconds is ~40 MB / 544k methods. Enabled from boot it is ~10 GB.
`mcpx_apu_method` works the same way and is far lower volume.

Decode the APU methods against `src/apu/apu_regs.h` rather than guessing:
`0x8000` is `SE2FE_IDLE_VOICE`, not an unknown.

## mode_finder.py / mode_sampler.py

Both talk to the GDB stub (`xemu -s`, port 1234). Guest VAs match ours, and the
game object is reached the same way: `MEM32(0x0022FCE0)`.

**Always use that indirection.** The literal shifts between runs — the same root
was `0x005E3A70`, `0x040D3A70` and `0x00363A70` on three occasions. Only the low
16 bits are stable. Every documented offset held.

```sh
python3 mode_finder.py A     # snapshot a state
python3 mode_finder.py B     # again, SAME state — this is the noise filter
python3 mode_finder.py C     # now the state you care about
```

A value that moved A→B is noise. One that held A→B and moved A→C is signal. In
practice that is 6 noisy dwords out of 8704, which is enough to read a state
change directly off the diff.

**Pair every absence measurement with a positive control.** Pausing the game
sets `+0x3C` and `+0x40` to 1, which proves the offsets and the object are
right. Without that control, "all zero during a cutscene" looks like a finding;
with it, it is correctly read as normal. That distinction cost a session.

`+0x87E8` (live registered object count) is the cheapest check that you are
reading the right object at all: 61 during the Corn cutscene, 65 in gameplay,
70 paused.
