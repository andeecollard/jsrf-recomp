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

## THE NUMBER — and a retraction

**WITHDRAWN: the Blinx figure of 6.6% was measured on a corrupt image.** It is
wrong and it pointed the opposite way from the truth. What follows replaces it.

### The SVOD chunks are hash-interleaved; a flat copy is silently corrupt

Verified arithmetically, not taken on trust:

    0xA290000 / 0x1000            = 41,616 blocks per full Data file
    1 + 203 x (1 + 204)           = 41,616      <- exact
    => block 0 is an L1 hash block, then 203 groups of
       [1 L0 hash block + 204 data blocks], 41,412 logical blocks per file

Logical block n therefore lives at

    file = n // 41412
    off  = (2 + (n % 41412) // 204 * 205 + (n % 41412) % 204) * 0x1000

**Why the validation gate did not catch it.** The XBE header and section table
sit inside the first clean 204-block run, so `xbe_parser` printed a correct
title, build date, base address and 56 plausible sections for a file that is
corrupt from roughly 832 KB in. A gate that only parses the header proves
nothing about the body. The honest checks are: map the directory entry's
start_sector through the de-interleave and confirm it lands on one of the raw
`XBEH` hits, and confirm the last section's raw end falls inside the file.

### The real number, from Crimson Skies

Extracted with de-interleaving, integrity-checked, and re-run independently:

    module: default.xbe [20F90F]   MS-named function starts: 3,106
      hit (we detect a start there):      3,073   (98.9%)
      missed, inside a detected function:    16   <- under-segmentation
      missed, in no detected function:       17   <- uncovered

**98.9%**, on a bare run with no seed files. Our function-boundary detection
agrees with Microsoft's ground truth almost everywhere. The 16 under-
segmentation misses are not a general weakness: they cluster in Dolby/FFT
kernels, where MS names each radix variant separately inside one blob, and in
MSVC scalar-deleting-destructor thunks that genuinely share a body.

That is the opposite of the conclusion the corrupt Blinx run supported, and it
changes what the oracle says about adoption-plan item 5: on this evidence
"keep the sweep in phase at source" is not buying a large correctness win.

### Blinx remains unextracted, and its layout is not Crimson's

Its descriptor parses (`MICROSOFT*XBOX*MEDIA` at logical block 0) but the root
directory it names is outside the stored data: root sector 1,693,790 needs
logical block 846,879 and the package holds 417,798. There is exactly one
volume descriptor in the whole package, and no valid `default.xbe` directory
entry appears in the first 4,000 logical blocks. Crimson's root sits at sector
34; Blinx's does not. Blinx is DVD_X2 media, so a partition base is the
obvious suspect, but deriving one from the root-sector-minus-two assumption
lands on hash data. Unresolved, and a separate piece of work.

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
