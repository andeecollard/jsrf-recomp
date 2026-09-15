# The frame is 31.8 ms, and macOS is the least finished of the three backends

15 Sep 2026, evening. Written after a player reported the game "feels like
floating" and the frame decomposed exactly:

    clear 10.10 + submit 8.91 + vsh 7.93 + sync 1.05 + rest 3.80 = 31.79 ms

31.5 fps against a title that runs its physics per frame, so everything moves at
half speed. This is the plan to fix it, what it is expected to be worth, and
what it will not reach.

## The finding that reframes all of it

**macOS has no resident clear, and no ownership map, and presents through a
second GPU API.** None of that is true on Windows. From the backend macro table
at `src/kernel/nv2a_pb_exec.c:8-34`:

    __APPLE__   draw  sync  invalidate  report
    _WIN32      draw  sync  invalidate  report  clear_color  clear_depth_stencil

The clear call sites exist in the pushbuffer executor and are `#ifdef`-guarded,
so on this build **every clear falls through to an invalidate**, which drops the
Metal textures and forces a full surface re-upload on the next draw. That is why
`clear` costs 10.10 ms, and part of `submit` is re-uploading a surface we threw
away a moment earlier.

And the flip does this, every frame, verified end to end:

    Metal texture
      -> nv2a_gpu_sync_range()   blocking GPU readback     nv2a_pb_exec.c:1069
      -> convert to RGB565 -> guest RAM -> copy to s_snap
      -> fb_row_to_rgba()        per-pixel CPU convert + vertical flip
                                                            d3d8_gl.c:665
      -> glTexImage2D()          upload the whole frame back to the GPU
                                                            d3d8_gl.c:670
      -> glDrawArrays()          one quad

We render in Metal, read the result back to the CPU, convert it twice, and
upload it to **OpenGL** to show it. Two graphics APIs in one process with a full
CPU round trip between them.

So the framing is not "the emulator is slow". It is that upstream solved these
on the D3D11 path and the Metal path never caught up. `nv2a_d3d11.c` is 1890
lines against `nv2a_metal.m`'s 1469, and the difference is mostly this.

## What OpenGL is, and is not, good for here

`src/d3d/d3d8_gl.c` is upstream's cross-platform **D3D8-level** backend. JSRF
never calls D3D8 — it links Microsoft's D3D8 into the XBE and drives the NV2A
pushbuffer directly — so as a renderer it is dead code for this title, exactly
like `src/audio/` is for DirectSound. Its only live role is the presenter above,
where it is a cost rather than an asset. **The reference to copy is the D3D11
backend, not the GL one.**

## The plan, cheapest first

### 1. `RECOMP_VSH_REUSE` — no build, no code, already in the binary

Caches the transform of a repeated vertex index within a batch
(`nv2a_pb_exec.c:194-224`). Commit `1a1f160` measured **51.4 / 51.3 / 50.9 %
duplicate indices across three independent 300 s gameplay runs** and verified
the cache twice (1 mismatch in 117,548,557 hits; 0 in 108,122,680).

* **Expected: 3.2-4.0 ms off `vsh`. ESTIMATED** — the duplicate rate is
  measured, the per-hit `memcpy` cost is not.
* **Risk:** one unexplained mismatch. Its capture is already armed and dumps the
  inputs; **inputs differ means a guest race and the cache is exonerated,
  inputs identical means the cache is wrong and this step dies.**
* **Its performance effect has never been measured at all** — `grep -rl
  VSH-REUSE` over every archived run returns nothing.
* Verify: one `RECOMP_VSH_REUSE_VERIFY=1` correctness run, then
  `ab_switch.sh vshreuse RECOMP_VSH_REUSE 3 280`. **The `vsh` column must be
  the one that moves.**

### 2. Teach the Metal backend to arm the ownership map — prerequisite for 3

`recomp_gpu_own.c` is the map of guest byte ranges the GPU currently owes
pixels to, so that a translated guest load of those bytes demands a write-back
first. `recomp_gpu_own_hold` / `_release` / `_set_sync` are called from
**`nv2a_d3d11.c` only** — Metal does not include the header.

This is not optional garnish. The D3D11 resident clear is gated behind
`RECOMP_D3D11_RESIDENT_CLEARS` and its own comment says why, naming this title:

> A static recompile currently has no general guest-CPU read callback for
> framebuffer pages. Keep the GPU-authoritative transition experimental until
> those reads can demand a range sync; **otherwise JSRF consumes stale RAM after
> its first batch and stops advancing.**

So a resident clear on Metal without the ownership half does not make the game
slow, it makes it **stop**. Mirror `publish_ownership()`
(`nv2a_d3d11.c:1185-1221`); Metal's bookkeeping is simpler because it retains
one surface rather than a four-entry cache.

* Verify on its own, before any clear exists: `[GPU-OWN]` should report touches
  and hits at gameplay. The D3D11 measurement was 49,137 guest touches, 0 of
  which hit a resident surface — so a plausible Metal result is also near zero,
  and that is a *result*, not a failure. It says the bet is safe.

### 3. A resident clear on Metal

Mirror `nv2a_d3d11_clear_color` (`nv2a_d3d11.c:1335-1400`): two new exports, two
`#define`s in the `__APPLE__` arm, a pipeline with a trivial vertex function
emitting one oversized triangle (a quad leaves a diagonal seam — D3D11 learned
that), `setScissorRect` for the window `clear_surface` already computes.

**The channel split is the part that is Metal-specific and is where this will go
wrong if it goes wrong.** This backend packs 24-bit depth into the colour
attachment's **alpha** and stencil into a separate `R8Uint` attachment
(`nv2a_metal.m:1386-1388`, readback at `:967-991`). So `MTLLoadActionClear` is
unusable — it would destroy depth while clearing colour. Use
`colorAttachments[n].writeMask`: colour clear writes `Red|Green|Blue` on
attachment 0; depth/stencil clear writes `Alpha` on attachment 0 and `Red` on
attachment 1.

**Require bit-exactness rather than a tolerance.** Write the float values the
upload path would have produced from the RGB565 clear value — `r=(c>>11)/31`,
`g=((c>>5)&63)/63`, `b=(c&31)/31`, exactly `nv2a_metal.m:1387` — so the eventual
readback quantises to the identical 565 word and the test is a byte comparison.

Fall back to the CPU clear when the range is not the retained surface or the
component mask is partial, as D3D11 does.

* **Expected: 3-7 ms.** The readback half is **measured** at up to 3.16 ms/frame
  (28,173 ms / 8,906 flips); the CPU clear loops and the forced re-upload are
  estimated, and the re-upload is the unknown that step 0's `surface_uploads`
  counter would size.
* Default OFF until a person has played through it, same discipline as
  `RECOMP_METAL_BATCH`.

### 4. A Metal presenter — delete the OpenGL round trip

Present the Metal texture directly instead of reading it back, converting it
twice and uploading it to GL. This removes the readback, both conversion loops,
the upload, **and one of the ~4.8 GPU syncs per frame**.

* **Expected: large but uncosted.** Nobody has measured it.
* **Risk:** the window is currently GL-backed; this needs a `CAMetalLayer`.
  Self-contained but not small.

## What does not add up, stated rather than padded around

Best case for everything above, with `RECOMP_METAL_BATCH`'s measured 2.23 ms:

    31.79  baseline
    -2.23  RECOMP_METAL_BATCH          MEASURED
    -4.05  RECOMP_VSH_REUSE            ESTIMATED, upper bound
    -5.0   resident clear              ESTIMATED, mid of 3-7
    -0.7   texture compare + reuse     ESTIMATED, low confidence
    -----
     19.8  and VSH_REUSE and any future MSL vertex path are the SAME milliseconds

**Best case ~16-17 ms, likely 19-21 ms.** The list is not padded to close the
gap.

Two things would have to give beyond it. **The serialisation:** ~4.8 GPU syncs
per frame means the CPU and GPU essentially never overlap — the frame is CPU
*plus* GPU (≈20 + 8 ms) rather than the maximum of the two, so perfect
overlap alone is worth ~10 ms and steps 3 and 4 each remove one sync. **The
fragment shader's data layout:** a 16-bit guest surface rendered into an
RGBA32Float attachment with the guest's blending, depth test and combiners
implemented by hand and read back through `raster_order_group(0)`, which
serialises every overdrawn pixel. If the GPU turns out to be the wall, that is
the wall.

## And the target is softer than it looks

The vblank is delivered at a steady 58 Hz, but the observed per-window `p50`
frame times — 25.5, 26.0, 31.0, 32.0, 33.0, 39.0 ms — **do not cluster on
multiples of 16.683**. The title's loop is not hard-gated on vblank, so
improvement is continuous rather than a cliff at 16.68 ms.

**31.8 → 22 ms already restores about 45% of the missing game speed.** That is
worth knowing before committing to the most expensive item on the list.

## Ordering

Steps 1 is independent and needs no build. Step 2 blocks step 3. Steps 3 and 4
each need a rebuild, which invalidates any pinned binary, so batch them: build
once, then A/B each switch in turn against the one new binary.

**No `src/**.{c,h,m}` may be edited while an A/B is running** — the staleness
guard aborts the next run. `.sh`, `.py`, `docs/` and `*_test.c` are safe;
`ab_switch.sh` itself is not, because `sh` reads a script incrementally.
