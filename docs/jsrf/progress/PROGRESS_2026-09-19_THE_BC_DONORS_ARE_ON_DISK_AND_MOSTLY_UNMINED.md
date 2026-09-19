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

## CORRECTION: the oracle cannot be run, and I said it could

An earlier revision of this note said "run the coverage oracle on Blinx —
version-independent, no new input". **The first half is true and the second is
wrong.** Tried, 19 Sep:

`coverage_oracle.py` takes `<module.dll> <our_functions.json>`, and that second
argument is *our* function detection **for the donor title**. Producing it
means disassembling Blinx's own `default.xbe`, and we do not have it:

- the BC package ships the **recompiled 360 module** plus its symbol table,
  not the original Xbox executable;
- the original game is in `Content/Game/DefaultPackage`, an Xbox 360 **STFS
  package** (`PIRS` magic) spanning 11 chunks and 1.6 GB;
- `default.xbe` appears there **by name**, so the guest filesystem is inside —
  but there is no `XBEH` magic anywhere in the chunks, so the image is stored
  compressed or nested rather than plainly.

So the oracle is blocked on an STFS extractor, which is a documented format and
a bounded job, but a job — not "no new input". The Fusion side is fine and was
verified: the module parses, build `20F912`, 1,786 symbols.

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
