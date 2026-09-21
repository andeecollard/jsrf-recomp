# PLAN -- 21 September 2026
# WHAT THE SYMBOLS MAKE POSSIBLE

Supersedes the "next measurement" in
`HANDOVER_2026-09-20_NIGHT3_THREE_THEORIES_DIED_AND_THE_TEXT_DEFECT_HAS_AN_ADDRESS.txt`
section 3. That measurement was never needed: the answer was in the guest's
own code, and reading it took less time than the instrument built to avoid
reading it.

---

## 0. THE METHOD, WHICH IS THE REAL ASSET

Three sessions of black-box measurement found correlations. One session of
reading named guest code found the mechanism, and it was not what any of the
correlations suggested.

What actually worked, stated so it can be repeated:

> Take a value our runtime writes into guest-visible memory. Find the guest
> routine that reads it -- now possible, because it has a name. Read the
> arithmetic that consumes it. Ask what range and units that arithmetic
> assumes. Check we satisfy them.

The fence failed on **units**, not on logic. `d3d8_ring_publish_fence`'s drain
gate was correct and had been defended at length; the value it gated was one
whole fence generation too high, which no amount of reasoning about *when* to
publish could have surfaced. Only the consumer's arithmetic shows it:

    00191456  sub edx,ecx    ; edx = cur - *fence
    0019145C  cmp ecx,edx / jae <return>      ; *fence == cur  =>  always taken

**The generalisation, and the single highest-value piece of work available:**
enumerate every value the runtime puts where the guest will read it, and audit
each against the invariant its consumer implies. That set is finite and, for
the first time, every consumer in it has a name. Start with the D3D8 device
struct, whose other five fields have had exactly as much scrutiny as the fence
had -- which is to say, none:

    +0x00 write cursor   +0x04 limit   +0x24/+0x28 ring bounds
    +0x30 submitted      +0x34 fence pointer   (done, 20 Sep)

then the DSOUND surface (137 names), then the kernel notification values.

An invariant worth writing down as a test the moment it is known. The fence's
is `*fence <= [dev+0x30] - 2`, and a one-line assertion of it would have
failed on day one.

---

## 1. THE FENCE FIX CHANGED THE CLOCK. PRIOR TIMING RESULTS ARE VOID.

The title now blocks where it has never blocked. Every measurement whose
value depends on thread interleaving was taken under the old, wait-free
regime.

**The audio bisect is invalid and must be re-established.** Its own handover
already states the rule -- "re-established on this binary first, because the
earlier 14 arms were scored on 92ad42df" -- and this is a larger change than
that one was. Specifically void:

  - control (34 dropouts, first at 150.2 s)
  - `RECOMP_WILD_PTR=0` (43, 55.5 s)
  - `RECOMP_METAL_NO_DEPTH_SYNC=0` (7, 214.8 s) and the flag on it

The three untested arms (`METAL_FF`, `VSH_DP_ZERO`, `WILD_PTR_SELFTEST`) must
not be scored against the old control. Re-run control first, with the >214.8 s
window the corpus now requires.

There is a real mechanism here, not just bookkeeping: the guest's ADX sound
server is driven by a wait on the D3D vblank event and needs >= 43.07 passes a
second to break even. The fence fix changes how much CPU the guest gets and
when it yields. The dropouts may be better, worse, or differently distributed.

---

## 2. THE TWO TEXT DEFECTS THAT REMAIN

The clobber is fixed and measured (0 of 663 opportunities, three runs,
scene-matched). Play on 21 Sep showed two further defects that clobber.py
cannot see, because it compares quad *positions and cells*, not pixels.

### 2a. A line struck through the glyphs

The path is mapped end to end and every hop is named:

    TextRenderer_MAYBE::draw   0x0003C310
      batcher vtable 0x001E144C  (+0x0C Begin/Lock, +0x14 AddQuad,
                                  +0x10 End/Unlock, +0x28 Draw)
      AddQuad  0x0015A880 -> 0x0015A6A0   movaps, 4 verts x 32B = 128B/quad
      Draw     0x0015A660 -> SetStreamSource 0x00155B20
                           -> SetIndices     0x0018E090
                           -> DrawIndexedVertices 0x001993A0
                              D3DPT_TRIANGLELIST, 6 indices per quad

A horizontal line across a glyph quad is most cheaply explained by the shared
edge of its two triangles, or by an index/stride mismatch making one triangle
degenerate or over-long. Two cheap checks, in order:

  1. Dump the 6 indices and 4 vertex positions for one text quad at execution
     time and check the winding and the shared edge. The batcher's own
     constructor walks the buffer `add eax,0x20`, and the FVF is 0x1C4 ->
     stride 32, so a stride disagreement in our decode is directly visible.
  2. `NV097_BREAK_VERTEX_BUFFER_CACHE` (0x1710) is emitted by every text Lock
     (flags 0 means NOFLUSH is clear) and **our executor does not decode it**.
     It is very unlikely to draw a line, but it is an undecoded method the
     title places at exactly the moment of each text lock, and decoding it
     costs one case.

### 2b. Banner text outliving its dismissal

One discriminator settles which half of the system is at fault, and it needs
no new code -- `RECOMP_GLYPH_DUMP` already timestamps every text draw:

  - `[GLYPH] draw:` lines CONTINUE after the banner should be gone
        -> the guest is still drawing it. Guest-side lifetime: the banner is
           an ACT (`CActMan::GetAction(0x1166)`), mode dispatch in
           `TextRenderer_MAYBE::draw` (modes 0/3/8 draw, mode 7 zeroes
           `[this+0x10]`), count reset at 0x0003C671.
  - the lines STOP but the pixels remain
        -> ours. A surface not cleared, or a flip presenting a stale buffer.

Run the Garage scenario, find the last `[GLYPH]` timestamp for the banner,
compare against the frame it is still visible in.

---

## 3. GRAFFITI

Needs characterisation before any work: "broken" could be the sprayed tag's
appearance on the wall, the spray-can HUD, the graffiti menu, or the soul
collection. The names differ completely by case, and the corpus already
records a *crash* here on 19 Sep that was fixed, so this is new behaviour.

    CSysTagManager          0x0003E690   (getTagList, countTagsByPlayer,
                                          allTagsFinished, updateSaveDataTagState)
    Tag::Tag                0x0003F4E0
    TagList::TagList        0x00040330
    CPlayer::spray_MAYBE    0x0008D280
    CPlayer::playSpraySound 0x000A2310
    CGraffitiMenu           0x00127590

Ask for one screenshot and one sentence before opening any of them.

---

## 4. INFRASTRUCTURE: MAKE THE NAMES ALWAYS-ON

The symbol layer exists but each run has to be told about it.

  - Wire `RECOMP_XDK_SYMBOLS` into the harness default so every run resolves
    `D3D8__D3D_g_pDevice` by name rather than by the title's constant. The
    `[D3D8-RING] D3D_g_pDevice = ... (the title's own constant)` line in the
    20 Sep runs shows the fallback is what is actually being used.
  - Name the guest functions in existing traces. `recomp_mem_watch_guest_store`
    already carries `guest_function` on every store and prints it as a bare
    address; 1,332 of those have names.
  - `merge_symbols.py` should be a build step, not a manual one.

---

## 5. TRAPS, CARRIED FORWARD

  - **`[PB-ACK] already=` is now meaningless as a health signal.** It compares
    `MEM32(getp)==submitted`, and the published value is deliberately no
    longer `submitted`. It will read ~0. That same counter has already misled
    this project once, on 20 Sep, for the same reason.
  - **`main.c`'s "GET moves about four times a second" comment is stale** by
    ~30x. Measured 20 Sep: 6,810-6,932 poll loops/s, ring already drained on
    96.9-97.1% of ~1.29M polls. Anything reasoning from pusher lag is
    reasoning from a dead fact.
  - **The XDK 4134 lock bits are not the PC ones.** `0x10` NOFLUSH, `0x20`
    NOOVERWRITE, `0x40` TILED, `0x80` READONLY -- read out of the title's own
    linked D3D8, not a header. `src/d3d/d3d8_xbox.h:715-718` carries the PC
    values in a file named `d3d8_xbox.h`; harmless for JSRF, wrong for the
    next title.
  - **Arming a guest-store watch perturbs what it measures.** The three 20 Sep
    runs with `RECOMP_MEM_WATCH` live all scored below the unperturbed 1.69%
    baseline. Prefer pusher-side instruments.
  - **clobber.py measures quad positions and cells, not pixels.** It scored 0
    on runs that still show a line artifact. It is sound for what it claims
    and blind to everything else.
