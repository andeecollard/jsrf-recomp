# G1 — the intro fade is a combiner constant the title never computes

Date: 2026-09-10 (Europe/London)
Commit: `893324b` (instrument), on `reference/xemu-oracle`
Build: verified archived gen (`JSRF_BASELINE_2026-09-09_gameplay/gen.tar.gz`,
sha256 checked against `RESTORE_MANIFEST.txt`), 27/27 ctest.

Supersedes the G1 section of `goals/JSRF_GOALS_2026-09-10_FOUR_FAULTS.md` on
two points. Read this before re-opening the fade.

---

## 1. Two claims in the goals document are wrong

### "The guest never asks for the multiply blend"

It does. `DST_COLOR/ZERO` is programmed **397 times from t=31 s**, and every
batch drawn under it is refused:

```
[BLEND-FADE] batches under DST_COLOR=443 short=0 vsh-rejected=0
             prepare-rejected=443 rasterised=0
```

Nothing is lost at the vertex stage or anywhere else. `nv2a_texture_copy.c`'s
accept test throws away all 443.

`rejected=0` in `1512123` was **an artifact of run length**. That run covered
1996 frames / 5968 batches; the multiply blend first appears at draw ~9000,
t≈31 s. A 60 s run reaches it; a 35 s run does not.

**But t=31 s is not the cards.** Measured clear-value timeline, ours:

| card | clear | window |
|---|---|---|
| white logo | `0x0000FFFF` / `0xFFFFFFFF` | t=4.4 – 8.4 s |
| grey Dolby | `0x000020E4` / `0x00231F20` | t=12.6 – 16.8 s |
| multiply blend first programmed | — | t=31.0 s |

So the refused multiply blend is a **real renderer defect on a later fade**,
not the intro-card fade. It is worth fixing on its own evidence; it is not G1.

### "Widening the blend accept set is not a goal"

That entry was justified by the falsified claim above. The accept test *is*
refusing 443 real batches. Re-file it as its own item, with the caveat that
it belongs to whatever runs at t≥31 s.

---

## 2. What the cards actually do — and it matches

Against xemu on the same US ISO, across the white card:

| quantity | ours | xemu |
|---|---|---|
| white-card frames | **726** | **726** |
| clear values | `0xFFFF` | `0xFFFF` |
| blend factors | `0x302 / 0x303` only | `0x302 / 0x303` only |
| combiner factors | `0xFFFFFFFF` throughout | `0xFFFFFFFF` throughout |
| vertex diffuse | `0xFFFFFFFF` (level 255) | `0xFFFFFFFF` |
| gamma LUT | n/a | identity, **all 3092 frames of the run** |

**726 white-card frames on both sides is the positive control.** It is what
makes the rest of the row meaningful: the card sequencing runs identically, so
these are the same frames being compared.

Nothing varies inside the card in *either* emulator. The ramp is not in the
card. That is why every renderer-side hypothesis came back constant.

---

## 3. The divergence

Both traces open on the same JSRF D3D initialisation — same blend defaults,
same sixteen combiner factors set to `0xFFFFFFFF`. **No dashboard renders in
front of the reference**, so the two are comparable from the first method.

Immediately after that init, xemu's guest ramps the combiner constants:

```
FACTOR0[20]:  0xff000100 0xff000200 0xff010300 0xff010601 ... 0xffffffff
              0x60a0ff60 0x9ca0ff60 0xbaa0ff60 0xc4a0ff60
                ^^          ^^         ^^         ^^
                96         156        186        196     (colour beside it holds)
```

107 distinct values on that slot alone.

Ours:

```
[FACTOR] writes=53888 distinct=16 overflow=0
[FACTOR]   0x0A60 = 0xFFFFFFFF hits=3368 draws 0..5033 t=0.00..19.98
   ... all sixteen slots, one value each, for the whole opening
```

**One value. Never another.** There is no fade level in our run to lose.

---

## 4. Ruled out, with the measurement

Do not re-derive these. Each was tested against the reference, not reasoned about.

| hypothesis | killed by |
|---|---|
| renderer refuses the fade's blend | cards use `0x302/0x303` in **both**; refusals are at t≥31 s, a different scene |
| fade rides on vertex diffuse alpha | constant 255 in ours **and** `0xFFFFFFFF` in xemu |
| fade rides on the clear value | five distinct values in xemu, none a ramp; ours matches |
| fade is a gamma-ramp / RAMDAC fade | xemu writes the LUT 768×/frame but it is the **identity ramp in all 3092 frames**; never a fade |
| fade is a combiner-factor ramp we refuse | our `uses_constant` refusal never fires — `rejected` shows only `blending` |

The gamma one is worth naming: `PRMDIO` is `STUB_ENTRY` in `nv2a_core.c`, the
NV2A aperture is plain zeroed RAM on macOS (`xbox_memory_layout.c`, "no register
semantics"), and `dev_SetGammaRamp` is a no-op stub in **both** D3D8 backends.
Every one of those is true, and none of them is this bug. It looked like a
complete explanation right up until the LUT was actually read.

---

## 5. Next

The question is now entirely guest-side and has nothing to do with the NV2A:
**what computes the combiner constant, and why does it produce `0xFFFFFFFF`?**

- The value is a D3DCOLOR-shaped `ARGB`. In xemu the ramping byte is the high
  one while `a0ff60` / `a0ff40` holds beside it — so the title is scaling one
  channel of a fixed colour, which is a fade level times a tint.
- Find the guest write. `RECOMP_TRACE_ENTER` / `RECOMP_TRACE_ARGS` /
  `RECOMP_TRACE_DEREF` sample at call boundaries; there is no general guest
  store trace in this tree. For a true watchpoint, xemu's GDB stub is the
  instrument (`mode_finder.py`, `mode_sampler.py`).
- `0xFFFFFFFF` is the value D3D initialises the factors to. So the likely shape
  is not "computes the wrong level" but **"never calls SetRenderState at all"**
  — the fade object's tick not running, or an early-out before it.

## 6. Reproducing

```sh
cmake -S diagnostics/jsrf_first_fault -B <build> -DRECOMP_GEN_DIR=<gen>
RECOMP_PB_EXEC=1 RECOMP_METAL=1 RECOMP_OHCI_ATTACH=1 RECOMP_BLEND_TRACE=1 \
RECOMP_REPORT_MS=10000 RECOMP_HDD_ROOT=<disposable hdd> <build>/jsrf_first_fault
```

22 s covers both cards. 60 s is needed to reach the t=31 s multiply blend.

xemu side — filter the firehose through a FIFO, never to a file:

```sh
printf 'nv2a_reg_write\nnv2a_pgraph_method\n' > events.txt
mkfifo trace.fifo
grep -aE 'SET_BLEND|SET_COMBINER_FACTOR|SET_COLOR_CLEAR_VALUE' < trace.fifo > out.txt &
xemu -trace events=events.txt,file=trace.fifo
```

Three traps, each of which cost a run here:

- xemu **zero-pads method numbers** (`0x0344`, not `0x344`). A substring filter
  on the unpadded form silently matches nothing, and the empty result reads
  exactly like a finding. Match on the method **name**.
- A second `-trace` without `file=` **redirects the whole firehose to stderr** —
  2.9 GB in ninety seconds.
- Timestamping matched lines with a forked `date` per line throttles the FIFO
  hard enough to slow the emulation and never reach the cards. Use stream order
  and the clear values as scene markers instead.
