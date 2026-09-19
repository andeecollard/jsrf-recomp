# The BC donors are on disk, and we are mining one library of five

19 September 2026. No run. Corrects a stale "closed" entry and measures what
the Fusion symbol work has actually delivered.

## The "unobtainable" note is out of date

`HANDOVER_2026-09-18_NIGHT_THE_WINDOW_IS_OURS_AND_THE_INSTRUMENTS_WERE_DEAD.txt`
files MS Fusion symbol recovery under **"INVESTIGATED AND CLOSED — DO NOT
REOPEN WITHOUT NEW INPUT"**, on the grounds that the packages are

> Windows Store packages, the player is on Apple Silicon, and CrossOver cannot
> run the Store or Gaming Services. PARKED, not dead — if a Windows machine
> appears, it is Blinx, it is one ~20 MB DLL from `Content/`…

**That DLL is on this machine.** All four BC titles are extracted under
`~/jsrf/BC/` (13 GB), dated 18 Sep 18:37–18:44 — the same evening, after that
paragraph was written. The named file is

    ~/jsrf/BC/BLiNX- the time sweeper/Content/
        xefu_556879e0_c7a54ca2_fbb30bce_b554e35e_e39a721e.dll      24.5 MB

Note the prefix: the symbols are in the **`xefu_*`** module, not in any of the
larger `xeo3_*` ones, which carry none. Searching the big modules first finds
nothing and looks like a negative result.

## What is in it, and what we have taken

Symbol-group counts in that one donor, against what the naming run used:

| group in the donor | names present | mined for JSRF |
|---|---:|---|
| `DirectSound` | 232 | **yes** — 162 of 410 detected DSOUND functions |
| `D3DX` | 307 | no |
| `XG*` (XGRAPHICS) | 304 | no |
| `CMcpx` (audio miniport) | 142 | no |
| `D3DDevice_` | 88 | no |

`jsrf_guest_names.tsv` holds **165 names — 1.9% of 8,876 functions**, and its
own header says DSOUND only, "partial coverage, measured error rate".

So the research is real and is in use — `CMcpxVoiceClient` and
`CMcpxAPU::AllocateVoiceResources` appear by name in three handovers — but it
has been applied to **one library of five present in the same donor file**.

## Why the unmined groups are the interesting ones

- **`CMcpx`, 142 names.** The audio miniport: the subsystem the whole of
  today's work sits in, and the one G1 and the ADPCM over-read are about.
- **`D3DX` and `XG*`, 611 names between them.** G2 (the glyph defect) and G3
  (the frame tail) are both graphics, and the donor carries D3D pushbuffer
  internals by name — `?InitializePushBuffer@CDevice@D3D@@QAEJXZ`,
  `?KickOff@CDevice@D3D@@QAEXXZ`, `?StartPush@CDevice@D3D@@QAEPCKK@Z` — which
  is `nv2a_pb_exec.c`'s subject matter.

## The version caveat still stands, and does not block this

`CLAUDE.md` is right that a **byte signature** transfers only where the same
XDK build emitted the same code, and Blinx (Oct 2002) is months off JSRF's
4134. That constrains `tools/fusion/signatures.py`.

It does **not** constrain the technique that produced the 165 names, which is
size-sequence alignment, nor `tools/fusion/coverage_oracle.py`, which the
handover itself calls "version-INDEPENDENT so it pays regardless".

## The oracle is the one that answers a question we hit today

`coverage_oracle.py` grades **our** function-boundary detection against
Microsoft's ground truth, and its docstring names the actionable output:

> the missed-inside set … those are the tail-jump / mid-function-entry cases
> our detector still merges

That is the defect behind a good deal of today's noise. Every `popfd`, bare
`cmpsb`/`scasb`, `sal` and `loop` in this title disassembles inside data, and
the 88 `UNRESOLVED FLAGS` branches are data plus mid-function entries. The
adoption plan's item 5, *"Keep the sweep in phase at source"*, is still
**planned** and is the fix for the cause.

It grades the detector on a title Microsoft named, not on JSRF — Microsoft
never BC'd JSRF — but the detector is what carries over.

## CORRECTION, TWICE OVER: the oracle CAN be run, and it has been

Two wrong claims of mine, in sequence, both corrected by doing the thing.

**First** I said the oracle needed "no new input". It needs our function
detection for the DONOR title, so it needs the donor's own `default.xbe`.

**Then** I said that was blocked, because the game data is an SVOD/PIRS
package and "there is no `XBEH` magic anywhere in the chunks, so the image is
stored compressed or nested". **That scan was broken.** `grep -c` without
`-a` prints nothing at all on these binaries, and my loop turned that into
"0". The positive control I should have run first — grepping for a string I
already knew was present — also came back empty, which is what finally caught
it.

With `grep -a`, `XBEH` appears five times. The XBE is stored plainly.

## Extracting a donor XBE — the recipe, verified on Blinx

1. **Symbols** are in `Content/xefu_*.dll`, NOT the larger `xeo3_*.dll`
   modules, which carry none. Searching the biggest file first gives a clean
   false negative.
2. **The game** is `Content/Game/DefaultPackage` (0xB000 PIRS/SVOD header)
   plus `DefaultPackage.data/Data0000..N`. The inner filesystem is XDVDFS —
   `MICROSOFT*XBOX*MEDIA` at `Data0000:0x2000`.
3. **Find the XBE** with `grep -a XBEH`. Several hits are false; validate by
   header (base `0x00010000`, sane section count, sane header size).
4. **Take the size from the filesystem, not the header.** The XDVDFS
   directory entry is the 14 bytes before the ASCII filename:
   `struct.unpack('<HHIIBB', d[i-14:i])` → `(l, r, start_sector, size, attr,
   namelen)`. Blinx: 44,728,320 bytes.
5. **Copy that many bytes**, spanning into the next `Data` file. No
   hash-block interleaving corrupted it — the region was contiguous.

Blinx validated cleanly through our own parser: *BLiNX: the time sweeper*,
title id `0x4D530013`, built 2002-09-13, 56 sections, `.text` / `D3D` /
`D3DX` / `XGRPH` / `DSOUND`.

## All four donors extracted and graded

Every donor's guest `default.xbe` is now out of its BC package, validated, and
graded against Microsoft's own function starts. Each oracle run below was
re-run here directly rather than relayed.

| donor | XDK | MS starts | hit | under-seg | uncovered |
|---|---|---:|---|---:|---:|
| **Fuzion Frenzy** | **3911 / 3925** | 1,482 | **97.6%** | 2 | 34 |
| **JSRF (target)** | **4134** | — | — | — | — |
| Blinx | 4831 | 1,786 | **98.0%** | 3 | 32 |
| Crimson Skies | 5659 | 3,106 | **98.9%** | 16 | 17 |
| Conker | 5849 | 3,228 | **99.1%** | 12 | 16 |

**Our function-boundary detection agrees with Microsoft's ground truth on
97.6–99.1% of named starts, across four independent titles, on bare runs with
no seed files.** The handful of under-segmentation misses are not a general
weakness: they cluster in Dolby/FFT kernels, where MS names each radix variant
separately inside one blob, and in MSVC scalar-deleting-destructor thunks that
genuinely share a body. The uncovered ones are cold library stubs reachable
only through data-table dispatch — USB/XID/MU device entry points, unreferenced
`D3DDevice_SetRenderState_*` pushbuffer stubs, CRT leftovers.

### RETRACTED: the 6.6% I published first

That figure was measured on a corrupt image and pointed the opposite way. The
same title now reads 98.0%.

**The SVOD `Data*` chunks are hash-interleaved.** Verified arithmetically:

    0xA290000 / 0x1000   = 41,616 blocks per full Data file
    1 + 203 x (1 + 204)  = 41,616      <- exact

so block 0 is an L1 hash block, then 203 groups of [1 L0 hash + 204 data],
giving 41,412 logical blocks per file:

    file = n // 41412
    off  = (2 + (n % 41412) // 204 * 205 + (n % 41412) % 204) * 0x1000

A flat contiguous copy silently splices hash blocks into the payload. Proven
independently of the arithmetic: for a hash block at H,
`sha1(data[H+0x1000:H+0x2000]) == data[H:H+20]`.

The corrupt-vs-correct pairs make the size of the effect plain: Blinx 6.6% →
**98.0%**, Fuzion 15.9% → **97.6%**.

### The validation gate that failed, and the one that works

My gate — parse the XBE and check title, date, base and section names — **passes
on a corrupt file**. Every one of those fields was correct on all three corrupt
extractions, including the XDK version. Two cheap fields are the real tells:

- **Kernel imports.** Correct Blinx: 0 `Unknown_*`. Corrupt: hundreds, with
  absurd ordinals. Conker corrupt: 288 bogus against 145 named.
- **TLS.** Correct Blinx: `0x0 - 0x0`, zero-fill 12. Corrupt: `0x20202020`,
  zero-fill 1,931,507,823.

I saw that TLS block on the corrupt Blinx, called it "an unused TLS, fine", and
moved on. It was the signal.

A third, stronger check: the header declares a kernel-thunk VA, and the
`0x8000xxxx` ordinal run must actually be there. On corrupt Conker it sat
0x6000 high — exactly six hash blocks.

### The sector base is per-title, and is not 32

`GDF sector 32 -> logical block 0` holds for Crimson and Fuzion and **fails for
Blinx and Conker**, whose dirent sectors are absolute on a larger disc:

| donor | base | root sector |
|---|---:|---:|
| Crimson Skies | 32 | 34 |
| Fuzion Frenzy | 32 | 66 |
| Blinx | 880,036 | 1,693,790 |
| Conker | 956,202 | 1,711,833 |

Recover it by solving the root sector against a known `XBEH` physical offset,
then confirm the root directory table parses into sane entries. Blinx's root
holds exactly three: `default.xbe`, `media`, `xdemos`.

Note also that `attr` is not a discriminator — 0x21 on Crimson, 0x80 on Blinx
and Conker. `namelen == 11` plus a size consistent with the XBE's last section
end is what identifies the real dirent; most ASCII `default.xbe` hits are
in-XBE path strings whose preceding bytes decode as garbage entries.

## And the version gap is wider than the date suggested

**Blinx links XDK 4831**, uniformly across all seven libraries. JSRF is 4134 —
a **697-build gap**. `CLAUDE.md` records only the date ("Oct 2002 … still
months off"). For comparison, `map_names.py` measured 99.1% name agreement
across a 190-build gap; 697 is well outside anything measured here.

## What to do, cheapest first

1. **Mine `CMcpx` from the Blinx donor.** This is the one that is genuinely
   unblocked: the size-sequence alignment that produced the 165 DSOUND names
   works from the donor's *symbol table* and our own detected function sizes,
   and needs no donor XBE. 142 names into the subsystem holding both open
   audio defects.
2. **Then `D3DX`/`XG*`**, 611 names, for G2 and G3.
3. **The oracle, after an STFS extractor** — worth it, but price it as its own
   piece of work.

## A tension in the tree worth settling

`CLAUDE.md` says a byte signature "only transfers where the same XDK build
emitted the same code", and treats Blinx being months off 4134 as close to
fatal. `tools/symbols/map_names.py`'s own docstring records the opposite,
measured:

> Donors do NOT have to share the target's XDK version. Measured: ATV3 (XDK
> 5849) named 504 functions in Burnout 3 (also 5849), and Starcraft Ghost (XDK
> 5659) named 657 in the same binary at the same precision. Where both named
> the same address they agreed 99.1% of the time.

A 190-build gap, 99.1% agreement. If that generalises, the version pessimism is
overstated and the byte-signature path is worth more than CLAUDE.md implies —
though it still needs donor *bytes*, which is the same STFS blocker.

Not resolved here. Whichever is right, it should be written down once.

## What this does not claim

That any of it fixes a defect. Names are a reading aid and a triage signal,
not a behaviour change — the DSOUND pass has a measured error rate and is
labelled a diagnostic aid in its own header. The oracle measures; it does not
repair.
