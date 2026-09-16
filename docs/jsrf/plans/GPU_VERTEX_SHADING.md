# Moving the NV2A vertex program off the CPU

16 Sep 2026. How `nv2a_vsh_msl.c` gets wired into `nv2a_metal.m`, written after
establishing that the emitter computes the right thing. **This document does not
implement anything.** It exists so that the implementation in `nv2a_metal.m` can
be written once.

The measurement that motivates it, gameplay, per frame:

    vsh 7.00 + submit 5.69 + sync 5.08 + clear 0.25 + rest 3.35 = 21.37 ms

Against a 16.67 ms budget. `vsh` is `prepare_vertices()` in
`src/kernel/nv2a_pb_exec.c`, which runs the guest's vertex program on the CPU
once per index per draw, about 700,000 draws in a 280 s run.

---

## 1. What is now known about the emitter, and what is not

`src/nv2a/nv2a_vsh_msl.c` had never been executed. Its own header said so.
`diagnostics/jsrf_first_fault/vsh_msl_diff_test.m` now dispatches the emitted
code on a real device and diffs all sixteen output registers against
`nv2a_vsh_execute`, over **the 126 distinct vertex programs read out of the
title's own `default.xbe`** plus one synthetic program per opcode.

    title corpus   126 programs   8064 vectors   0 residual disagreements
    synthetic      21 programs    1303 vectors   0 residual disagreements

Three real defects came out of it and are fixed in `nv2a_vsh_msl.c`:

| defect | how it showed | fix |
|---|---|---|
| Metal fuses `a*b+c` into an fma even under `MTLMathModeSafe`; the interpreter does not, because `multiply()`'s zero test is a select | 51 of 8064 vectors, up to 1.78 absolute in clip space, concentrated in seven skinning programs | `#pragma clang fp contract(off)` in the preamble |
| NV2A's rule that `a*0` and `0*b` are `+0` even against infinity was not reproduced | a zero normal takes `rsqrt(0) = +inf` into the normalise multiply: NaN on the GPU, `0` on the CPU, and the NaN reaches `oPos` | `vsh_mul()` helper on MUL and MAD |
| `LIT` used `exp2(w * log2(max(y,0) + 1e-30))` with a bound of 128 | 59 of 64 fixture vectors; any `y <= 0` with a negative exponent | `pow()` and the interpreter's `127.99609375` bound |

**The limits of that evidence, which the implementation has to respect:**

- **`DPH`, `DST`, `EXP`, `LOG`, `LIT` appear in no program this title ships.**
  Their only evidence is a synthetic fixture built with `vsh_encode.h`. If a
  program containing one ever reaches the GPU path, it is untested against the
  title.
- **The emitted screen-space epilogue is checked against a *reading* of
  `prepare_vertices()` and of the fixed `vs` in `nv2a_metal.m`, not against the
  renderer.** FULL mode in the diff test agrees to 1.65e-06, which says the
  transcription is faithful to that reading. It does not say the reading is
  right. Nothing has been rendered.
- **Nothing has been drawn.** `nv2a_metal.m` does not call the emitter, and the
  emitter's header still says so.
- 13 of 8064 title vectors disagree by up to 4.3e-04 relative on `oPos`. The
  test's conditioning probe shows the interpreter's own answer moves that far
  under a one-ulp change to its operands, so these are the programs' own
  conditioning, not the emitter's. **They are still real divergences in the
  rendered image** — see §7 on what that means for an A/B.

---

## 2. The shape of the change

Today:

    pushbuffer -> prepare_vertices()  fetch attrs, run program per index,
                                      snap to 1/16, write s_outputs[i][16][4]
               -> nv2a_metal_draw(state, ..., s_outputs, count, primitive)
               -> vs()  screen -> clip, pass through d0/d1/t0..t3
               -> fs()/fs_hw()/fs_hw_blend()

After:

    pushbuffer -> nv2a_metal_set_vsh(words, length, constants)   [new]
               -> nv2a_metal_draw_indexed(state, attrs, indices, count, ...)
               -> vsh_main()  the guest's own program, then the same
                              screen -> clip the emitter already emits
               -> fs()/fs_hw()/fs_hw_blend()  unchanged

`prepare_vertices()` keeps existing and keeps running whenever the GPU path
refuses (§6). It is not deleted, and it is the thing the new path is scored
against.

The vertex stage the emitter produces already absorbs both pieces of fixed work
that sit on either side of the interpreter today — the 1/16 subpixel snap from
`prepare_vertices()` and the screen-to-clip divide from `vs()`. That is
deliberate and is why the generated function can replace `vs` outright rather
than feeding it.

---

## 3. A program becomes an MTLFunction

### 3.1 Two caches, not one

`hw_pipeline_for()` keys pipelines on `(blend, blend_src, blend_dst, sblend)`.
That key now gains two more dimensions — which vertex program, and which vertex
layout — and the product of three independent sets does not belong in one flat
table. So:

**Cache A: microcode -> MTLFunction.** Compiling MSL is the expensive half
(`newLibraryWithSource:` is a full front end) and it depends on nothing but the
program. Keyed on the microcode words.

**Cache B: (function, layout, blend key) -> MTLRenderPipelineState.** Keyed on
Cache A's slot index, a layout id (§5.3) and the existing four blend fields.
This is `hw_pipeline_for` with a wider key, and it should stay that function's
shape: a fixed-size linear array, a miss counter, and a refusal when full.

### 3.2 Cache A, concretely

```c
#define VSH_CACHE 192          /* see the sizing note below */
static struct {
    uint32_t words[NV2A_VS_MAX_INSTRUCTIONS][4];
    int      length;
    uint32_t hash;             /* FNV-1a, for the fast reject only */
    id<MTLFunction> fn;
    int      refused;          /* the emitter said no; do not retry */
    unsigned long long uses;
} vsh_fn[VSH_CACHE];
static unsigned vsh_fn_n;
```

A lookup hashes the words, scans for a matching hash, and then **compares the
words in full**. `trace_selected_program()` in `nv2a_pb_exec.c` already
identifies programs by a 32-bit FNV hash and nothing has ever collided, which is
not the same as nothing ever colliding; a cache that hands a draw the wrong
vertex program produces wrong geometry with no error anywhere. The full compare
costs at most 136 * 16 bytes on a hit and is off the per-vertex path entirely.

**Sizing, from a measurement rather than a guess.** The image contains 126
distinct programs (`vsh_xbe_corpus.h`, cross-checked against the title's own
length table: 123 of the 126 carry a matching `(slots<<16)|0x2078` header).
`RECOMP_VSH_TRACE` reports distinct selected programs at runtime but caps its
own table at 32, so it has never been able to say whether a session uses all
126. 192 holds every program in the image with room; it is 192 * (2176 + a
pointer), about 420 KB, once, for a path that would otherwise recompile.

**A full cache must not evict.** Evicting an `MTLFunction` throws away a
compile that will be wanted again next frame, and `newLibraryWithSource:` on
the draw thread is exactly the stall this change exists to remove. If the cache
fills, count it and send the draw to the CPU. The report says so (§8) and the
number to watch is that it stays zero.

**A refusal is cached too.** `nv2a_vsh_generate_msl()` returning 0 is a property
of the program, not of the draw. Re-emitting and re-failing 700,000 times would
cost more than the interpreter. The `refused` flag makes it once per program.

### 3.3 When Cache A is filled

On the draw thread, at the point `prepare_vertices()` would decode: if
`s_vsh.dirty` cleared and the decode succeeded, hash the selected words and look
them up. A miss runs `nv2a_vsh_generate_msl()`, then `newLibraryWithSource:`
with **`MTLCompileOptions.mathMode = MTLMathModeSafe`** — not the default.

That is not a style preference. `initialize()` already sets it for the fixed
shader, and the differential test measured the emitted code against the
interpreter under safe math only. Under fast math `rsqrt`, `pow` and the
reciprocals are different functions and the result in §1 does not transfer.

Compilation is synchronous and will stall the first draw that uses a program.
126 programs at a few milliseconds each is a fraction of a second spread over a
session's first appearance of each shader, but it is a visible hitch at the
moment a new one appears. If that shows up in a frame trace, the fix is to
compile on a worker and use the interpreter until the function is ready — which
the fallback in §6 already provides for free. Do not build that first; measure
whether it is needed.

---

## 4. The constant file

The guest writes `c0..c191` through `NV097_SET_TRANSFORM_CONSTANT` and
`nv2a_pb_exec.c` keeps them in `s_vsh.constants[192][4]`. For skinned geometry
they change **between draws**, so they are per-draw state, not per-program
state.

The emitter declares them as `constant float4 *c [[buffer(1)]]`, 192 entries,
**3072 bytes**. That is inside Metal's 4 KB `setVertexBytes` limit, and the file
comment in `nv2a_vsh_msl.c` records that this is why the constant file is 192
and not the hardware's 256: 256 float4 is 4096 bytes, over the limit, and would
need a real `MTLBuffer` and a staging allocation per draw.

So: **`[encoder setVertexBytes:constants length:3072 atIndex:1]`, once per
draw.** No allocation, no ring reservation, no lifetime question.

**Buffer index 1 currently holds `Params` on the vertex stage.** The existing
encoder line binds vertex buffer 0 = vertices, 1 = `Params`, 2 = indices. The
generated function does not use `Params` at all — everything it needs is in the
constant file and the viewport block — so on a generated-program pipeline,
vertex buffer 1 is the constant file and vertex buffer 2 is `VSH_Viewport`.
`Params` stays at *fragment* buffer 1, which is a separate namespace and is
untouched. **The two pipelines have incompatible vertex bindings and the encoder
must not set both.** Getting this wrong is silent: the shader reads whatever is
bound and produces geometry.

`VSH_Viewport` is `{ width, height, depth }` — `NV097_SET_SURFACE_CLIP`'s width
and height and the 16777215 z divisor — the same three values `vs()` reads out
of `Params` today. 12 bytes, `setVertexBytes` at index 2.

The a0-relative read is emitted as `c[min((a0 + N) & 255, 191)]`. The 8-bit wrap
is hardware behaviour and matches `read_source()`. **The clamp is not**: the
interpreter refuses the whole draw when the wrapped index lands in 192..255, and
a vertex function cannot refuse. Across 8064 vectors of the title's own programs
the interpreter never refused for this reason, so it is not known to happen —
but "not observed in a synthetic sweep" is not "cannot happen", and §6's
counters are where it would show up if the clamp ever starts mattering.

---

## 5. Vertex attributes

This is the part with the least evidence behind it and the most room to be
wrong.

### 5.1 What the guest declares

`VertexAttr` in `nv2a_pb_exec.c` is `{ offset, type, size, stride }` per
attribute, sixteen of them, where `offset` is a **guest address** and `type` is
the NV097 format nibble. `fetch_attr()` decodes exactly three:

    TYPE_F       (2)  1..4 floats
    TYPE_UB_D3D  (0)  4 bytes B,G,R,A -> /255
    TYPE_UB_OGL  (4)  1..4 bytes      -> /255

and returns 0 for `S1`, `S32K` and `CMP`, which rejects the draw. Anything the
CPU path cannot fetch, the GPU path must also refuse — **not** silently produce
something else.

### 5.2 The mapping to Metal

`[[stage_in]]` with an `MTLVertexDescriptor`, which is what
`vsh_msl_compile_test.m` already builds and links against. The formats line up:

    TYPE_F size 1..4    -> MTLVertexFormatFloat, Float2, Float3, Float4
    TYPE_UB_OGL size 4  -> MTLVertexFormatUChar4Normalized
    TYPE_UB_D3D size 4  -> MTLVertexFormatUChar4Normalized_BGRA
    everything else     -> refuse the draw, count it by type

`UChar4Normalized_BGRA` reproduces `fetch_attr`'s channel order exactly. Confirm
that against a rendered frame before believing it: a swapped colour channel is
the kind of thing this project has got wrong from reading before
(`nv2a_metal.m`'s B5G6R5 note is the same mistake in the other direction, and it
cost a measurement to find).

A component count below 4 needs care. `fetch_attr` zeroes the components it does
not read and sets `w = 1`; Metal's `Float2` fills `z = 0, w = 1` for a `float4`
stage_in member, which is the same. `UChar4Normalized` has no 1/2/3-component
normalized-byte equivalent that matches, so **refuse `UB_OGL` with `size != 4`**
rather than approximate it.

### 5.3 Where the bytes come from

The guest's arrays live in the mapped guest RAM region. Two options:

**(a) Wrap guest RAM once.** `newBufferWithBytesNoCopy:` over the guest memory
region gives one `MTLBuffer` in which every attribute is an `offset` and a
`stride`, bound with `setVertexBuffer:offset:atIndex:`. No copy, no gather, no
per-draw allocation — the whole 7 ms goes away rather than shrinking. The
requirements are that the region is page-aligned and that its length is a
multiple of the page size, and that nothing unmaps or re-protects it under the
GPU. **That last one is not a small caveat in this tree**: CLAUDE.md's
guarded-MMIO rule exists because a thread unprotecting a page cost 13.5M lost
writes. Whether the guest RAM region is stable enough to hand to the GPU for the
lifetime of a command buffer is an open question and must be answered by
measurement, not by reading the allocator.

**(b) Gather into the existing staging ring.** Walk the batch's indices and copy
each used attribute's bytes into a packed interleaved buffer, then bind that.
This is a memcpy per attribute per index and it keeps a real share of the
current cost — it removes the *program execution* but not the *fetch*. It has no
lifetime question at all: the ring already solves that, with a self-test
(`metal_ring_test`).

**Do (b) first.** It is strictly less risky, it is bounded by machinery that
already exists and is tested, and it answers the question that actually matters
— how much of the 7 ms is the program and how much is the fetch — which nothing
currently knows. Then decide whether (a) is worth its risk.

Attribute buffer indices must avoid 1 and 2. Use 3 upward, one per used
attribute (`program->inputs_read` says which), or a single interleaved buffer at
index 3 under (b).

### 5.4 Indices

Today `vs()` does its own index lookup: `Vertex x = v[indices[id]]` with a
`device uint*` at buffer 2 and `drawPrimitives`. A `[[stage_in]]` function
cannot do that — the vertex fetch happens before the shader runs — so the
generated path must use `drawIndexedPrimitives:indexCount:indexType:
indexBuffer:indexBufferOffset:`. The index array is already staged in the ring.
`MTLIndexTypeUInt16` matches `s_gpu.idx`'s `uint16_t`; the assembly array is
12288 entries, well inside it.

This is a second behavioural difference between the two pipelines and a second
thing that has to be got right per pipeline rather than per encoder.

---

## 6. What happens when the emitter refuses

**This is the part that must be designed rather than discovered.** Today
`prepare_vertices()` has five named ways to fail and `note_vsh_reject()` counts
each one separately with the first offending detail. All five still exist. A
vertex function has none of them: it cannot return "no", and a program it cannot
express is a program that must not reach it.

`nv2a_vsh_generate_msl()` returns 0 for: a null or invalid program, no `FINAL`,
a write to the constant file, or a buffer too small. `nv2a_vsh_parse()` sets
`valid = 0` for an out-of-range opcode, temp or output register. Compilation and
pipeline creation can fail for anything. Attribute formats can be undecodable.
The caches can be full.

**Every one of those sends the draw to `prepare_vertices()` and the existing
`vs`/`fs` pipeline, and every one is counted under its own name.** The decision
is per draw and it is made before any GPU work is encoded, so there is never a
half-transformed batch.

    gpu_vsh_eligible(draw):
        if (!vsh_on())                     -> cpu, reason "switch off"
        if (s_vsh.mode != 2)               -> cpu, reason "fixed function"
        if (!decoded.valid || !has_final)  -> cpu, reason "undecodable program"
        if (cacheA[slot].refused)          -> cpu, reason "emitter refused"
        if (cacheA full)                   -> cpu, reason "program cache full"
        if (library/function failed)       -> cpu, reason "compile failed"
        for each attribute in inputs_read:
            if (!format_supported(type,size)) -> cpu, reason "attribute format"
        if (cacheB full || pipeline failed)-> cpu, reason "pipeline"
        -> gpu

The reason table should be the same shape as `s_vsh_reject[]`: reason strings as
literals compared by pointer, a fixed-size table, and the first offending detail
kept. "38,472 rejected" named no cause and could not be acted on; that lesson is
already written into `nv2a_pb_exec.c` and this arm should not have to learn it
again.

**The fallback must be a fallback, not a silent second renderer.** A frame that
takes the CPU path for some draws and the GPU path for others is running two
different vertex stages with two different rounding behaviours (§1), into one
depth buffer. `nv2a_metal.m` already has exactly this hazard named for the
fragment stage — `g_hw_mixed`, "MIXED draws (software tail while hw on)" — and
the vertex stage needs the same counter for the same reason.

---

## 7. `RECOMP_METAL_VSH`

Opt-in, off by default, read through **`recomp_switch_on()` from
`src/recomp_switch.h`**:

```c
#include "recomp_switch.h"
static int vsh_on(void)
{
    static int on = -1;
    if (on < 0) on = recomp_switch_on("RECOMP_METAL_VSH");
    return on;
}
```

Not `getenv(...) != NULL`. That header exists because three switches here were
presence-tested and their control arms ran with the guard on —
`RECOMP_APU_SELFLINK_END`, `RECOMP_VSH_REUSE` and `RECOMP_PB_EXEC` — and the
obvious way to take the control arm of this A/B is `RECOMP_METAL_VSH=0`, which a
presence test reads as ON. Read once and cache: the switch cannot change during
a run and `getenv` on the draw path is a cost for nothing.

**A second switch, `RECOMP_METAL_VSH_VERIFY`, also boolean, also through
`recomp_switch_on()`.** With it set, the GPU path runs *and* `prepare_vertices()`
runs, and the two transformed vertex sets are compared on the CPU, component by
component, with the first few disagreements printed by draw, index, register and
component. It is far slower than either arm and is not a mode anyone plays in.

It is the whole reason to believe this change. `vsh_msl_diff_test.m` compares the
two implementations on synthetic operand data; VERIFY compares them on the
title's actual vertices, its actual constants and its actual a0 values — which is
the only place the 4.3e-04 conditioning divergences and the constant-index clamp
can be shown to matter or not to. Build it at the same time as the fast path,
not after.

---

## 8. What the `[METAL]` report must print

`ab_score.py` refuses to compare two arms that report the same switch state, and
it can only check a switch that names itself. Every line below prints
unconditionally, in both arms, in `nv2a_metal_report()`:

```
[METAL] vsh: %s (metal_vsh %s, verify %s)
        -> "GPU vertex programs" / "CPU interpreter", then the two switch
           states as read, not as intended. This line is what names the arm.

[METAL] vsh draws: %llu GPU, %llu CPU, %llu MIXED frames
        -> the split. MIXED counts frames in which both paths ran, which is
           the hazard in section 6, not a statistic.

[METAL] vsh vertices: %llu transformed on GPU, %llu on CPU
        -> the number the 7.00 ms is proportional to. A GPU draw count that
           moves while this stays at zero means the counter is on the wrong
           side of the branch.

[METAL] vsh programs: %llu distinct, %llu cache hits, %llu compiles,
        %llu compile failures, %llu emitter refusals, %llu cache-full
        -> "distinct" is the population of cache A and should settle well
           under 126. "compiles" should stop growing after the first minute
           of a session; if it does not, the key is wrong and the cache is
           thrashing. "cache-full" must stay 0 or VSH_CACHE is too small.

[METAL] vsh pipelines: %llu built, %llu cache hits, %llu refusals
        -> the same three hw_pipeline_for already reports, for the wider key.
           Refusals must stay 0; each one is a draw on the CPU.

[METAL] vsh CPU fallback by reason: <literal>=%llu (detail %u) ...
        -> the s_vsh_reject[] shape. Eight slots, reason strings compared by
           pointer, first detail retained. Without this, a fallback count is
           a number with no action attached to it.

[METAL] vsh attribute formats refused: type%u size%u = %llu ...
        -> S1, S32K and CMP are undecoded on BOTH paths today. If one of them
           ever appears this says which, which is what decides whether it is
           worth implementing.

[METAL] vsh verify: %llu vertices compared, %llu components differed,
        worst %.3g relative, first at draw %u index %u o%d.%c
        -> only under RECOMP_METAL_VSH_VERIFY. THE POSITIVE CONTROL FOR THE
           WHOLE ARM: "0 differed" means the two vertex stages agree only if
           "compared" is large. Print both or the zero proves nothing.
```

**Every absence-measurement here needs its positive control.** `GPU draws` being
large is the control for `CPU draws` being small; `vertices compared` being large
is the control for `components differed` being zero; the reason table being
populated is the control for a fallback count that is not zero. A report that
prints only the zeros is the failure mode this project keeps paying for.

---

## 9. How to know it worked

The frame decomposition already exists and already separates `vsh`. The A/B is:

    RECOMP_METAL_VSH=0 ... jsrf_first_fault    control
    RECOMP_METAL_VSH=1 ... jsrf_first_fault    arm

One pinned binary, one pad script, run serially, and check the binary hash
changed between builds before believing any difference — same-second mtimes have
silently produced four identical "different" arms in this tree before.

Expected: `vsh` falls towards zero and `submit` rises, because the work moved
rather than vanished, and the net is whatever the GPU does with it. **A drop in
`vsh` with no change in total frame time is a real possible outcome** and would
mean the draw thread was never the bottleneck — which is worth knowing and is
not a failure of this document.

Before that A/B is meaningful, three things must be true and each is a separate
check:

1. `RECOMP_METAL_VSH_VERIFY=1` over a real mission reports `components
   differed = 0` with `vertices compared` in the millions. This is the claim
   `vsh_msl_diff_test.m` cannot make, because it uses synthetic operands.
2. The `[METAL]` report shows `CPU draws = 0` — or names every reason it is not.
3. A frame captured with the arm on is compared against the control by the
   `x0 mod 4` structural score already used for the fragment-path work, not by
   eye. Six image metrics failed on the last artefact here before one worked.

---

## 10. Files

| file | role |
|---|---|
| `src/nv2a/nv2a_vsh_msl.c` | the emitter. Checked against the interpreter; still never drawn with. |
| `src/nv2a/nv2a_vsh.c` | the interpreter. The reference, and the fallback. Unchanged. |
| `src/nv2a/nv2a_metal.m` | where all of section 3 to 8 goes. Not touched by this document. |
| `src/kernel/nv2a_pb_exec.c` | `prepare_vertices()`, `s_vsh`, the reject table. The branch in section 6 starts here. |
| `diagnostics/jsrf_first_fault/vsh_msl_diff_test.m` | the differential test. Run it after any emitter change. |
| `diagnostics/jsrf_first_fault/vsh_xbe_corpus.h` | reads the title's 126 programs out of `default.xbe` at run time. Needs `JSRF_GAME_DIR`. |
| `diagnostics/jsrf_first_fault/vsh_msl_compile_test.m` | does the emitted text compile and link against a vertex layout. |
