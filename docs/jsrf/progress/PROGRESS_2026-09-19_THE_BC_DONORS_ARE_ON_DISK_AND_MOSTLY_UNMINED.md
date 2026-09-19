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

## The number

    module: default.xbe [20F912]   MS-named function starts: 1,786
      hit (we detect a start there):        118   (6.6%)
      missed, inside a detected function: 1,486   <- under-segmentation
      missed, in no detected function:      182   <- uncovered

We detect **17,859** functions in Blinx — ten times what Microsoft names —
and agree on 118 of their 1,786 starts. **83% of MS's starts land inside
something we already called one function.** The samples are unambiguous:
`0x000F2B80` contains `_XWriteTitleInfoNoReboot@24`, `_XGetLaunchInfo@8` and
`_XWriteTitleInfoAndRebootA@20`; `0x000F33FC` contains three more.

That is the adoption plan's item 5 — *"Keep the sweep in phase at source"* —
with a number against it for the first time.

**Caveat.** JSRF's pipeline feeds `--seed-functions` and `--function-bounds`
from title-specific analysis; Blinx has none, so this was a bare run. Seeds
can only ADD starts, so 6.6% is a floor. It is also the honest figure for a
cold title, which is what every new game is.

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
