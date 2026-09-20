# REFUTED BY ITS OWN TEST: the fence was never lying

> **Read this box before the document.** The title below is wrong. The
> acknowledgement it accuses was ALREADY gated on a full drain, two tokens to
> the left of the line I quoted: `if (getp && consumed && ...)`, where
> `consumed` is `jsrf_pb_poll`'s return, `!stream_fault && g_pb_last==now`.
> The comment above that line says so as well. I read the assignment, missed
> the guard, and built a causal story on it.
>
> The fix built on that story WEDGED THE TITLE at `flips=1`: it wrote the
> parser's ring cursor into `*(dev+0x34)`, which is a COUNTER, not an address
> -- the run showed `idx put=11 get=5695356` -- so `PUT - *fence` came out as
> a huge unsigned value, the title decided it had no room and blocked in
> `D3D_BlockOnTime` for ever. One run, 50 seconds, and it took the premise
> down with it.
>
> **What survives is everything that was measured rather than argued**: the
> 149 clobbered text draws, the decode of the corrupt frame, the named text
> path, and the disassembly of SetVertexData4f / MakeSpace / BlockOnTime. What
> dies is the CAUSE. The ring is still being overwritten under the parser and
> we still do not know why.

# (superseded) The fence tells the game the ring is empty, and the game believes it

20 September 2026, night. No new runs were needed for any of this: the
mechanism is read out of the game's own code, and the artifact it predicts was
already measured earlier the same evening.

## The one-line version

`jsrf_pushbuffer_ack` writes the producer's own PUT into the fence the game
reads, so the game computes its free ring space as `PUT - PUT = 0 used`, wraps,
and overwrites push-buffer dwords our parser has not read yet. The text
vertices live in those dwords.

## How the game asks

`0x00199640` is the library's inline vertex write. It is three instructions of
bounds check and five of payload:

```
mov esi, [0x19dce0]      ; the D3D device -- the SAME global main.c calls
mov eax, [esi]           ;   JSRF_D3D_CHANNEL_PTR. +0x00 = write cursor,
cmp eax, [esi+4]         ;   +0x04 = limit
jb  ok
call 0x1916b0            ; out of room: the reserve
ok:
mov [eax],   0x100000 + ((reg + 0x1a0) << 4)
mov [eax+4], x   [eax+8], y   [eax+0xc], z   [eax+0x10], w
add eax, 0x14
mov [esi], eax
```

The header decodes exactly: `reg` 9 gives `0x101A90`, whose method field is
`0x1A90` = `NV097_SET_VERTEX_DATA4F_M(9)` with a count of 4. **Text vertices are
written INLINE into the ring**, not into a vertex buffer somewhere else. (An
earlier note in this corpus inferred the opposite from `[PB-DRAW-ARRAYS]` and
the executor's `inline:` counter; that counter tracks `INLINE_ARRAY` (0x1818),
which is a different method from `SET_VERTEX_DATA4F`.)

## How the game decides it has room

`0x1916b0` -> `0x191530` -> `0x00191440`:

```
mov edi, [0x19dce0]
mov eax, [edi+0x34]    ; the fence POINTER    <- main.c's JSRF_D3D_GETPTR_OFFSET
mov ecx, [eax]         ; *fence: how far the GPU has consumed
mov eax, [edi+0x30]    ; PUT: how far the game has written
sub edx, ...           ; edx = PUT - *fence   <- the outstanding backlog
sub ecx, esi           ; distance to the point it wants
cmp ecx, edx
jae ...                ; room?
```

`0x1914E2` re-reads both and goes round again. Free space is `PUT - *fence`.

## What we write into that word

`diagnostics/jsrf_first_fault/main.c`, in `jsrf_pushbuffer_ack`:

```c
if (getp && consumed && MEM32(getp)!=submitted) MEM32(getp)=submitted;
```

`getp` is `*(dev+0x34)` and `submitted` is `*(dev+0x30)` -- PUT. So we set
`*fence = PUT`, making `PUT - *fence` **zero**: the ring is entirely free, every
time, whatever the parser has actually read. That is the largest lie the field
can carry, and the loop above is built to believe exactly that number.

The comment beside the assignment already warned about acknowledging
"newer, unconsumed work"; it guards against re-READING PUT after the poll, which
is a smaller version of the same error. The fix for the bigger one is to publish
the parser's own cursor, which `jsrf_pb_poll` already maintains and already
publishes honestly, one step at a time, to the NV2A register at `0xFD800044`.

## What it predicts, and what was measured

Predicted: the producer laps the parser, and a batch already submitted gets its
opening dwords rewritten by whatever the game writes next.

Measured, hours earlier and before any of this was read (`clobber.py` over a
replay's `runtime.log`):

| | |
|---|---:|
| page-0 text draws | 15,707 |
| draws with a page-1 pass behind them | 2,494 |
| **draws whose opening quads were the FOLLOWING page-1 batch** | **149 (5.97%)** |
| glyphs lost | 268 |

One frame of it, decoded:

```
corrupt  q00 x409.2 y60 r0c1      q01 x468.2 y60 r0c3      q02 'm' x325.9
page-1   q00 x409.2 y60 r0c1='w'  q01 x468.2 y60 r0c3='y'
correct  q00 'G' x302  q01 'u' x314  q02 'm' x325.9
```

The page-1 quads are sitting on the `G` and `u` of the `Gum` name label, in a
draw that has page 0 bound -- so they sample page 0's cells of the same number
and come out as `"` and `$`. **`$ou` for "you" and the vanished leading
characters are the same event**, which is what the corpus had been treating as
two defects.

## The text path, named

From KeybadeBlox' symbol table (read in place; see below):

```
TextRenderer_MAYBE::draw            0x0003C310
  CMGameGL::setAlphaBlendEnabled    vtable +0x124   (vtable at 0x001E0F00)
  (unnamed)                         vtable +0x128 -> 0x001508B0
  CMGameGL::setTextureByIndex       vtable +0x144 -> 0x0014FDE0
      D3DDevice_SetTexture          0x0018DF10
  bind cache                        [0x251D54], second stage [0x251D58]
```

The bind cache the 19 Sep entry described is confirmed instruction for
instruction, and `setTextureByIndex` is now a name rather than an offset.

## The fix, already in the tree, NOT yet run

- `RECOMP_PB_HONEST_FENCE=1` publishes the parser's cursor into `*(dev+0x34)`
  instead of `submitted`, carrying `submitted`'s high bits so the value stays in
  the game's address space, and leaving the subroutine case alone because a
  cursor inside a called buffer is not a ring address. Default off.
- `[PB-FENCE]` reports `acked-past-the-parser`, so a run says whether the lie
  is ever material before anyone believes the fix.
- Score with `clobber.py <runtime.log>`. A clean run is exactly 0.

Expect a frame-rate cost: the game will now actually wait when the ring is full,
which it has never done here.

## It does NOT explain the audio

Tempting, and wrong. The lie is unconditional -- no switch touches it -- so it
is identical in every arm ever run. The three `--bare` arms carried the highest
voice activity of the series (1,109 and 1,107 spans, 42,750 idle traps) and
never lost the music; the player's-switch arms die at 34-43 dropouts. A constant
cannot discriminate between those groups.

## The audio bisect, as far as it got

Re-established on this binary, then stopped at the player's request:

| arm | dropouts | first |
|---|---:|---:|
| control (the player's 17) | 34 | 150.2 s |
| `RECOMP_WILD_PTR=0` | 43 | 55.5 s |
| `RECOMP_METAL_NO_DEPTH_SYNC=0` | 7 | 214.8 s |

WILD_PTR and METAL_NO_DEPTH_SYNC are eliminated by the presence rule.
`METAL_FF`, `VSH_DP_ZERO` and `WILD_PTR_SELFTEST` are untested. Note
METAL_NO_DEPTH_SYNC's 7 dropouts with an onset later than any previously
recorded (the historical maximum was 158.7 s) -- if all five come back dirty,
replicate that one first.

## The symbol table

`../JSRF-Decompilation/ghidra/symboltable.tsv`, cloned from
`https://codeberg.org/KeybadeBlox/JSRF-Decompilation`. 1,332 named functions,
99.0% exact match on our function starts. `symbolize.py` reads it in place.
**The project publishes no licence and asks that no language-model output be
contributed back to it. Read it; do not vendor it; do not push to it.**

Its own decompiled C is at 0.71% and does not cover this path -- the value here
is the NAMES, not the code.

## XbSymbolDatabase named the other half, and confirmed every inference

`Cxbx-Reloaded/XbSymbolDatabase` (MIT) builds on this host -- its CLI target
does; its UnitTest target does not compile with this toolchain, which does not
matter. One scan of our XBE returns **363 symbols: 163 D3D8, 137 DSOUND, 59
XAPILIB, 4 XGRAPHC**. The two databases are disjoint: the decompilation names
the GAME, XbSymbolDatabase names the XDK the game was statically linked against,
which is why everything at `0x0019xxxx` read as bare hex.

Every address inferred above came back with the name the inference predicted:

| address | inferred from the disassembly | XbSymbolDatabase |
|---|---|---|
| `0x0019DCE0` | "the D3D device global, = `JSRF_D3D_CHANNEL_PTR`" | `D3D8__D3D_g_pDevice` |
| `0x00199640` | "writes `SET_VERTEX_DATA4F_M(reg)` inline" | `D3D8__D3DDevice_SetVertexData4f` |
| `0x001916B0` | "the reserve / make room" | `D3D8__D3DDevice_MakeSpace` |
| `0x00191530` | "make room" | `D3D8__D3D_MakeRequestedSpace_8` |
| `0x00191440` | "computes `PUT - *fence`, where it waits" | `D3D8__D3D_BlockOnTime` |
| `0x00191710` | "kick, reads `dev+0x30`" | `D3D8__D3D_KickOffAndWaitForIdle` |
| `0x00191390` | (called when `esi == PUT`) | `D3D8__D3D_SetFence` |
| `0x0018DF10` | "the XDK SetTexture" | `D3D8__D3DDevice_SetTexture` |

The routine the fence lie disarms is called **`D3D_BlockOnTime`**. It is the
XDK's blocking wait, and what it blocks on is the word we overwrite with PUT.

It also names an address this corpus has carried unnamed for days:
`0x0018CE50`, where "every D3D thread blocked for ever" in the 09-10 note, is
`D3D8__D3DDevice_BlockUntilVerticalBlank`.

**And 137 DSOUND names land on the audio problem**, including the voice path
the APU work has been chasing blind: `CMcpxBuffer_Play` (`0x001A490D`),
`CMcpxBuffer_Stop`, `CMcpxBuffer_SetBufferData`, `CMcpxBuffer_GetStatus`,
`CMcpxVoiceClient_SetVolume/SetPitch/SetMixBins`, and the whole
`CDirectSoundVoice_*` surface. "A guarded MMIO page swallowed every VOICE_ON"
now has a guest-side entry point to trace from.

### Regenerating and using it

```sh
git clone --depth 1 https://github.com/Cxbx-Reloaded/XbSymbolDatabase.git
cmake -S XbSymbolDatabase -B XbSymbolDatabase/build -DCMAKE_BUILD_TYPE=Release
cmake --build XbSymbolDatabase/build -j8            # UnitTest fails; CLI builds
XbSymbolDatabase/build/projects/cli/XbSymbolDatabaseCLI <default.xbe>     > ~/jsrf-build/jsrf-xbsymbols.txt
python3 scratchpad/merge_symbols.py                  # -> jsrf-symbols-merged.tsv
```

The merge emits `symbolize.py`'s own TSV shape -- 1,695 rows, the game's names
winning any tie -- so every existing tool takes it directly:

```sh
symbolize.py --symbols ~/jsrf-build/jsrf-symbols-merged.tsv lookup 0x00191440
0x00191440  undefined __cdecl D3D8__D3D_BlockOnTime()
```

`symbolize.py annotate` will now rewrite XDK addresses in any log into names.

---

# The pump stopped being per-title

Written after the above, and the reason it could be written at all is that the
device global now has a name.

## What moved

`src/kernel/d3d8_ring.{c,h}` -- new, in the shared runtime rather than the JSRF
harness:

- resolves `D3D8__D3D_g_pDevice` from a symbol table (`RECOMP_XDK_SYMBOLS`,
  accepting both the XbSymbolDatabase CLI dump and `symbolize.py`'s TSV), with
  `RECOMP_D3D8_DEVICE_GLOBAL` as an explicit override and the calling title's
  own constant as the fallback;
- names the SDK field offsets -- write cursor `+0x00`, limit `+0x04`, ring
  bounds `+0x24`/`+0x28`, submitted PUT `+0x30`, fence pointer `+0x34` -- each
  one read out of the shipped library rather than assumed;
- owns `d3d8_ring_publish_fence()`, so the honest-fence policy and its
  `acked-past-the-parser` counter belong to every title, not to this one.

`main.c` keeps its constants **as defaults only**; the macros resolve through
the runtime, so all twenty existing use-sites are untouched and a run with no
symbol table behaves exactly as before. `JSRF_D3D_CHANNEL_PTR_DEFAULT` is
0x0019DCE0 = `D3D_g_pDevice`, and `JSRF_PB_DEVICE_DEFAULT` is 0x0019B200 =
the device struct, which is why the four hand-derived ring VAs are that address
plus {0x00, 0x04, 0x24, 0x28}.

The harness's own comment said this could not move "because the addresses are
this title's", and that the general fix was to learn them. That is what
happened; the comment is updated in place.

## Tested without a game

`jsrf_d3d8_ring_test`, five cases (`ctest -R jsrf_d3d8_ring`): the CLI format,
the TSV format, the explicit override, no table, and a table that does not
exist. **Every case passes a default that is never the expected answer**, because
the first version of the TSV parser matched nothing, fell back to the built-in
default, and a scratch check called that a PASS -- the default happened to be
right. Both decoys in the fixtures (a name that contains the wanted one without
ending in it, and a right-looking address on a wrong-named row) are there for
the same reason.

## Suite state

90 tests, 2 failures, **neither new**:

- `jsrf_switch_audit` -- 8 hand-rolled switch reads over its baseline of 125.
  Measured against `git archive HEAD` in a temp tree: **HEAD is also 8**. This
  session took it to 14 and then brought it back: two booleans moved to
  `recomp_switch_on` (`RECOMP_GLYPH_DUMP_LATIN`, `RECOMP_PB_ACK_AFTER_EXEC`),
  three value-carrying names were exempted with reasons (`RECOMP_XDK_SYMBOLS`,
  `RECOMP_D3D8_DEVICE_GLOBAL`, `RECOMP_FONT_TRACE`, which now carries a line
  cap), and `RECOMP_PB_DRAW_ARRAYS` -- the previous session's, uncommitted --
  moved to `recomp_switch_on_default(..., 1)`, which keeps its default-on
  meaning and differs only for a malformed value like "0abc".
- `jsrf_input_hotplug` -- reads live hardware and reports
  `[PAD] port 0: PS4 Controller (opened)`. It fails because a real controller
  is attached to this machine.

## Still not run

`RECOMP_PB_HONEST_FENCE=1` remains untested against the game. The prediction
stands: `clobber.py` should score 0 where it scored 149, `[D3D8-RING]` should
report a non-zero `acked-past-the-parser`, and frame rate should fall, because
the title will begin waiting in `D3D_BlockOnTime` where it has never waited.


---

# The test, and what it killed

`RECOMP_PB_HONEST_FENCE=1`, same replay, same glyph dump, symbol table supplied.

**The symbol path worked in situ** -- first live proof of the new runtime
module: `[D3D8-RING] D3D_g_pDevice = 0x0019DCE0 (.../jsrf-symbols-merged.tsv)`.

**Everything else failed, and fast.** At 50 seconds the title had not left
`sequence 0`:

| | wedged run | healthy baseline |
|---|---|---|
| flips | **1** | thousands |
| `idx put / get` | **11 / 5695356** | 393317 / 393291 |
| `[PB-ACK]` | 1,490,950 loops/s, already=**0** | 94,766 loops/s, already=21,194,547 |

`idx put=11 get=5695356` is the whole diagnosis. The fence field holds a
COUNTER. I wrote a masked ring cursor into it.

Note the `already=0` as well: `main.c`'s own arm counter compares
`MEM32(getp)==submitted`, so the moment the published value stopped being
`submitted` that counter read zero -- a counter measuring something other than
its name, exactly as CLAUDE.md warns.

## What the code actually does

```c
int consumed = jsrf_pb_poll();          /* !stream_fault && g_pb_last==now */
...
if (getp && consumed && MEM32(getp)!=submitted) MEM32(getp)=submitted;
```

The acknowledgement has always required a full drain. In a healthy run
`not-consumed=119,388` polls did not drain, and every one of them correctly
withheld the fence. There was no lie to fix.

## What is in the tree now

The switch is gone -- a default-off no-op for a case that cannot arise is
clutter. `d3d8_ring_publish_fence()` keeps the invariant instead: it publishes
only when the pump says drained, and counts refusals, which for a correct pump
stays at zero and is therefore a positive control for any future title's pump.
The device discovery, the field offsets and the five tests are untouched and
still stand on their own.

## Where G2 goes next

The artifact is real and unexplained. The page-0 batch is rasterised with its
opening quads replaced by the page-1 batch that follows it, and the vertices
are inline in the ring via `SetVertexData4f`. Two candidates, neither tested:

1. **A ring overwrite the fence cannot prevent**, because the guest writes
   forward from `dev+0x00` and only consults the fence at `dev+0x04`, the
   limit. Anything between the published PUT and the write cursor is
   unprotected by construction.
2. **An executor state bug at the batch boundary**, in which case no memory
   race is involved at all and the head of one batch is being filled from the
   next one's vertex registers. `SET_VERTEX_DATA4F` builds a vertex register by
   register, so a mishandled BEGIN/END would look exactly like this.

Candidate 2 costs nothing to test: it predicts the corruption survives with the
pump stopped, and it is the one I would take first, because it needs no theory
about memory at all.

---

# What the vertex buffer turned out to be, and a 60-second reproducer

## The text quads are in guest RAM, at ONE address, shared by both passes

`RECOMP_FONT_TRACE` now prints attribute 0's base and stride beside the sheet.
Booted to the Garage, with the glyph detector naming the two Latin pages:

| page | texture | batches (vertices) | vertex buffer |
|---|---|---|---|
| 0 | `00DAC000` | 300, 330, 312, 240, 84, 24 | **`0x00B8B000` stride 32** |
| 1 | `00DBC000` | 24, 12, 6 | **`0x00B8B000` stride 32** |

One line of text is drawn in two passes **out of one buffer at one address**,
refilled from the base for the second pass. That is the glyph dump's finding
seen from the other end, and it is now an address rather than an inference.

**Two of my own claims died getting here**, both from generalising off the
wrong function:

- "Text vertices are written INLINE into the ring via `SetVertexData4f`" --
  wrong. A scan for direct callers of `D3DDevice_SetVertexData4f` finds twelve,
  all in `CMGameGLFont::draw` (the OVERLAY font) and a stencil helper, and none
  in the `0x3Cxxx` banner path.
- "So the vertex buffer is outside the fence's protection" -- also wrong.
  `D3DVertexBuffer_Lock` (`0x001997C0`) blocks unless the caller passes
  `NOOVERWRITE`/`DISCARD`, via `0x001917B0`, which reads the resource's
  last-use fence at `[resource+8]` and calls **`D3D_BlockOnTime`** -- the same
  routine, reading the same `*(dev+0x34)`, that the ring uses. Ring and
  resource share one fence.

## A reproducer that costs 60 seconds instead of 6 minutes

The replay was only ever needed for the tutorial banner. The **Garage boot
reproduces the defect on Corn's dialogue**:

```
scenario garage.json, RECOMP_GLYPH_DUMP=20000 RECOMP_GLYPH_DUMP_LATIN=1
clobber.py: 3 of 177 opportunities (1.69%), 9 glyphs lost
```

Small n -- a clean run at that rate happens by chance about 5% of the time, so
score two. But every G2 A/B is now a minute, not six.

## The question that is left, stated so it can be answered

The fence protects the buffer **if the title locks it without
`NOOVERWRITE`/`DISCARD`**. The single `Lock` call site is a pass-through
wrapper at `0x00155E30` whose flags come from its own caller, and it is reached
through a vtable, so the flags are one indirection beyond a direct-call scan.

Two ways to settle it, both cheap, neither yet done:

1. **Watch the buffer.** `recomp_mem_watch` on `[0x00B8B000, +stride*verts)`
   counts guest writes that land while a draw referencing that range is still
   pending. Non-zero is the race, proven, and it also answers the flags
   question without reading them: a title that blocked could not write.
2. **Read the flags.** Find `0x00155E30` in a vtable, then the callers of that
   slot, the way `CMGameGL::setTextureByIndex` was reached from
   `[0x251d6c]+0x144`.

Do (1) first. It measures the thing itself rather than the mechanism that is
supposed to prevent it, and this session has now twice been wrong about a
mechanism while the measurement stood.
