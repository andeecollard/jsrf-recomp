# Microsoft's Own NV2A Translator — a White-Room Teardown

Companion to [ms-fusion-recompiler.md](ms-fusion-recompiler.md) (package layout, HLE boundary),
[ms-fusion-codegen-teardown.md](ms-fusion-codegen-teardown.md) (what the two translators emit) and
[ms-fusion-corpus.md](ms-fusion-corpus.md) (the four-title corpus). Those documents study the
*recompiler*. This one studies the *graphics translator* that the recompiler carries — Microsoft's
original-Xbox GPU emulator, `xefu.xex`.

**No Microsoft code, IR, data tables or output was copied, transcribed or reused in this
repository.** No decompiled function bodies appear below. Everything here was derived from publicly
distributed retail binaries using standard tools (`pefile`, `capstone`, SHA-1, AES) and is stated as
measurements and design observations in our own words, the way one would describe a published paper.
Subjects analysed 2026-09-18, package version `2608.3123.1.0`.

Every claim below is tagged **MEASURED** (read directly out of a binary, reproducible) or
**INFERENCE** (a reading of measured structure, which could be wrong). Where I could not determine
something, §9 says so.

---

## 1. What the target actually is

**MEASURED.** `xeo3_615ec97d_885b2da5_66df6a27_55e92a1b_703b6904.dll` (2,474,496 B) and its `_no`
sibling (9,669,120 B) are PE32+ DLLs, image base `0x180000000`, four exports
(`InitPrecompiledDll`, `CleanupPrecompiledDll`, `PrecompiledPointers`, `PrecompiledImportTable`) —
the PowerPC-layer ABI from §1 of the recompiler doc. Their version resource names the source module:

```
D:\btsdx\20F912\xbox\emulator\Externals\XenonLKG\free\HardDisk\SystemPartition\Compatibility\xefu.xex
```

Both files are **byte-identical across all four BC packages** (Blinx, Conker, Crimson Skies, Fuzion
Frenzy) — md5 `a30ba56a…` and `56efc3ab…`. Confirmed, not assumed. The build tree is `20F912`,
one build later than the `20F914`/`20F90F`/`20F917`/`20F919` spread the corpus doc recorded for the
`2607` packages, and the per-title `xeo3` hashes have moved with it. So this is a *newer* release of
the same pipeline, and the emulator layer is now shared byte-for-byte by every title.

`SystemPartition/Compatibility/xefu.xex` ships alongside the DLL: the recompiled DLL supplies host
code, the XEX supplies the guest image (data, jump tables, string pool) that the code reads at
runtime through the guest-memory base register.

### The layering, corrected

For an OG-Xbox title on PC there are **three** guest layers, not two:

```
Emu.exe  (+ VGPUDX12.dll, D3D12Core, dxcompiler)        host, x86-64, native
└─ xeo3_615ec97d…  = xefu.xex          Xbox 360 PowerPC → x86-64   ← THIS DOCUMENT
     the original-Xbox emulator, itself a 360 title
   └─ xefu_69c41281…  = xb1krnl.exe    OG Xbox x86-32  → x86-64
   └─ xefu_556879e0…  = default.xbe    OG Xbox x86-32  → x86-64   (the game)
```

The game's own statically-linked D3D8 runs as recompiled x86-64 and writes a real NV2A pushbuffer
into emulated Xbox memory. `xefu.xex` consumes it. **MEASURED** support: the game module's
`PrecompiledSymbolTable` names the OG D3D8 entry points (`_D3DDevice_SetTexture@8`,
`_D3DDevice_SetRenderState_Simple@8`, `?GetPitch@PixelJar@D3D@@…`, and hundreds more) as ordinary
translated guest functions — they are symbols on translated code, not host thunks. **INFERENCE:**
graphics is therefore emulated at the pushbuffer level, not HLE'd at the D3D8 API level.

### What `xefu.xex` was built against

**MEASURED**, from the XEX's plaintext static-library header: `XAPILIB`, `LIBCMT`, `XBOXKRNL`,
`XNET`, `PMCPB`, `XONLINE`, **`D3D9`**, **`XGRAPHC`**, `XAUD` — all version field `2.0.5426.x`, i.e.
one Xbox 360 XDK. It imports from `xam.xex`, `xboxkrnl.exe` and `xefutitle.xex`, and it embeds a
134,304-byte PE resource literally named `xb1krnl` (the OG-Xbox kernel, carried inside the emulator).

**This is the single most important structural fact in the document:** Microsoft's NV2A translator
does not target GPU registers. It targets **the Xbox 360's own D3D9 API**, statically linked into
the same module. The NV2A→Xenos translation is a translation between two *graphics APIs' state
models*, with the 360's D3D9 doing the command-buffer construction. On PC that D3D9 is in turn
consumed by the 360 emulator (`VGPUDX12.dll`, `DX12EdramResolveShaders.sbin`) and becomes D3D12.

### Recovering the guest image

**MEASURED, method note.** The XEX is `XEX2`, image base `0x82000000`, image size `0x5E0000`
(6,160,384 B), entry `0x820C4148`, encryption type 1, compression type 2 (LZX, 32 KB window),
12 chained blocks, 94 × 64 KB page descriptors. The file key decrypts under the **all-zero (devkit)
key**, not the retail key; all twelve block SHA-1s verify. LZX decompression yields exactly
6,160,384 bytes laid out by RVA (not by the PE header's raw offsets, which are stale). Correctness
was established independently of the page digests: the PE header and section table parse cleanly;
the guest `.pdata` gives 2,444 function starts that are monotonic and land on real prologues;
99.08 % of non-zero `.text` words decode as PowerPC under capstone (the 0.92 % remainder is VMX128,
which capstone does not model); the string pool is clean; and — decisively — the jump-table
addresses recovered *independently* from Microsoft's recompiled x86-64 output point at arrays of
valid guest code addresses in the decompressed image, byte for byte.

Guest sizes: `.text` 1,196,428 B / 2,444 functions; `.rdata` 119,924 B; `.data` 4,501,072 B virtual
(168,960 B initialised); plus `.pdata`, `.idata`, `.XBMOVIE`, `.XBLD`, `.reloc`.

---

## 2. The two tiers are the same program twice

**MEASURED.**

| | `…703b6904.dll` (Pri) | `…703b6904_no.dll` (Fb) |
|---|---:|---:|
| `.text` | 2,208,486 B | 7,169,398 B |
| `.rdata` | 220,392 B | 2,454,568 B |
| `.pdata` RUNTIME_FUNCTIONs | **2,566** | **2,566** |
| bytes inside functions | 1,923,921 (87.1 %) | 6,174,351 (86.1 %) |
| median function size | 381 B | 1,130 B |
| largest function | 15,989 B | 58,618 B |

The function *count* is identical to the entry. Only the size distribution moves, and it moves
uniformly (median ×2.97, mean ×3.21). The `Comments` resource confirms the cause: the two builds
differ by exactly one control file (`Fallback.ctrl.json`) and by `…Pri.json` vs `…Fb.json` /
`xefuPri\glopt` vs `xefuFb\glopt`.

**INFERENCE.** This settles the question the project's notes left open. `Fb` is **not** a fallback
*interpreter* and **not** a debug/checked build. It is the same 2,566 translation units compiled
with optimisation and speculation disabled — three times the code for the same control-flow graph.
Read alongside the corpus doc's finding that the guest image ships separately and both tiers load
it, the tiering is a *confidence* mechanism: `Pri` is compiled against recorded traces and
enlightenments, `Fb` is compiled to be correct without them, and the runtime can fall back per
module when `Pri`'s assumptions do not hold.

**MEASURED**, `Pri` host code shape: 400,409 instructions; 42,426 `movbe` (one per guest memory
access — the endianness story from the codegen teardown, unchanged); 14,748 direct calls, 614
indirect calls, 251 register-indirect jumps. 241 of those 251 are guest `switch` statements, and
Ficl preserves them faithfully: it computes the guest jump-table address, loads the entry
byte-reversed out of guest memory, and resolves the resulting *guest* address through the global
address map. **The guest's switch structure survives translation exactly** — I recovered 312
jump-table sites from the guest PowerPC and the case counts match the bounds checks in the x86-64
one for one (405, 255, 251, 250, 241, 228, 217, …). That cross-validation is what makes the rest of
this document possible.

---

## 3. Q1 — How the pushbuffer is consumed

**MEASURED.** The NV2A method interpreter is a single 3,216-byte guest function. Its inner loop:

1. Loads the next command word from guest memory **byte-reversed in the load instruction**
   (`lwbrx`) — the same trick as `movbe`, applied to the pushbuffer rather than to guest code.
2. Tests the word against a mask that simultaneously requires packet-type bits, the reserved bits
   and the subchannel field to be zero. A single compare against zero separates
   "increasing methods on subchannel 0", which is essentially every packet a title emits, from
   everything else. Non-zero goes to a general path.
3. Extracts the method as `word & 0x1FFC` — i.e. **the raw byte offset, not a shifted index**.
4. Uses that byte offset directly to index a table.

**There is a method dispatch table, and it is as wide and as dense as it can be.** **MEASURED:**

* **2,048 entries, 4 bytes each, 8 KB**, covering the whole `0x0000`–`0x1FFC` Kelvin method space
  with no gaps, no hashing, no range checks and no binary search. The method's own bit pattern is
  the index; the `& 0x1FFC` *is* the bounds check.
* **1,182 entries are live; 866 are zero.** A zero entry dispatches to the same body as handler
  slot 0 — a shared "consume the parameter and continue" routine. **Unknown methods are not
  logged, not trapped and not special-cased; they cost one load and one indirect branch.**
* The entries are **not code pointers**. They are packed descriptors (measured: the OR of all 2,048
  entries is `0x07007FFF`, so only bits 0–14 and 24–26 are ever used) whose low 6 bits select a
  handler.
* **216 distinct descriptor values** across 1,182 live methods — the table is a compressed encoding,
  not 1,182 hand-written cases.
* The handler table is **38 slots holding 34 distinct code addresses**, all of them labels *inside
  the interpreter function itself*. This is a computed-goto interpreter: 34 handler bodies sharing
  one stack frame, one register allocation and one set of live loop invariants. There is no call
  per method.

So: **two-level dispatch.** Method → dense 2,048-entry descriptor → 6-bit handler id → one of 34
inline handler bodies. One load, one bit-extract, one load, one `bctr`.

**MEASURED**, the handler population, which shows where the design spends its specialisation:

| handler slots | methods covered | what the method range looks like |
|---:|---:|---|
| 1 (the bulk handler) | **972** | everything that is only state |
| 5 handlers | 16 each | five blocks at strides 8, 4, 4, 8 and 16 bytes |
| 1 handler | 40 | a `0x80`-strided pattern repeated 8 times |
| ~27 handlers | 1–18 each | individually specialised methods |

**INFERENCE** (matching those measured ranges against the *public* NV2A/Kelvin method map, which is
documented outside Microsoft): the five 16-entry blocks are the sixteen immediate-mode vertex
attributes in their five encodings; the eight `0x80`-strided repeats are the eight register-combiner
stages; the individually specialised methods include no-op, surface format, the two bulk
transform-program/constant load windows, begin/end, the four draw methods, the semaphore pair, and
the three clear methods. I did not find symbol names anywhere in the shipped binaries, so every
name in that sentence is inference from address arithmetic, not a read symbol.

### A second, specialised scanner

**MEASURED.** There is a second 4,568-byte function whose only job is to walk a *run* of consecutive
draw packets. It applies a mask that admits exactly the four draw methods (`0x1800`, `0x1808`,
`0x1810`, `0x1818`), advances by `(count + 1) * 4` per packet, prefetches 512 bytes ahead of both
the command and data pointers with four explicit `dcbt`s per stream, and accumulates a running
**minimum and maximum of unsigned 16-bit values in VMX registers** (`vminuh` / `vmaxuh`).

**INFERENCE.** That is an index-range pre-pass: consume every consecutive indexed-draw packet in
one sweep, compute the vertex index range the whole batch touches, and hand back a single range so
one vertex-buffer translation covers the entire run. It is called *from* the indexed-draw handler,
not from the dispatcher.

### Direct threading between handlers

**MEASURED.** The begin/end handler, after resolving state, peeks at the next command word itself
and — if it is an indexed-draw packet on subchannel 0 — branches straight into the draw handler
without returning to the dispatch loop. The draw handler, after the scanner returns, peeks again for
a one-parameter begin/end packet and branches straight back. The end-primitive path goes further: it
compares the next *three* words against a begin/end packet, a matching primitive type and a
following draw packet, and if they all match it **skips the end/begin pair entirely** and keeps the
batch open.

**INFERENCE.** The hot path is the `BEGIN → draw run → END → BEGIN(same prim) → …` sequence that
D3D8's `DrawIndexedVertices` emits, and it is threaded so that a long sequence of same-primitive
draws never touches the dispatcher and never closes a batch.

---

## 4. Q2 — Immediate translation, or shadow state?

**Shadow state, resolved at draw time, with a dirty model.** **MEASURED:**

* The general path writes the parameter **byte-reversed into `[per-subchannel context + method]`** —
  i.e. the shadow state block is itself indexed by the raw method number, exactly like the
  descriptor table. Method `0x0000` on any subchannel is handled specially: it looks the object
  handle up in a table and stores the resulting context pointer into the subchannel's slot, which is
  the standard NV object-binding mechanism.
* Separately there is a **register-file model addressed at true NV2A MMIO offsets**. Specialised
  handlers write *both*: the shadow method slot and a modelled hardware register, through a common
  register-write routine. Offsets observed include the PGRAPH clear-value registers, a PGRAPH FIFO
  register and the channel DMA put/get registers — all at their real hardware offsets.
* Every descriptor is **OR-accumulated into one running 32-bit word** as its method is processed.
* At begin-primitive, that accumulated word is passed to a 976-byte resolve routine and the
  accumulator is reset to zero.
* The resolve routine tests **exactly three bits** of it — `0x01000000`, `0x02000000`, `0x04000000`
  — which is precisely the `0x07000000` that the measured OR-of-all-descriptors says are the only
  top-byte bits any descriptor ever sets. The three groups fan out to: a 2,608-byte translator, a
  four-word invalidation, and a loop over eight slots that decodes a 5-bit type code per slot and
  dispatches an 18-case table.

So the dirty model has **two granularities**:

* **Coarse (measured):** a 3-bit dirty mask, OR-accumulated per method from the descriptor table and
  tested once per draw. Not per-register, not per-state-object — *three groups for the entire
  pipeline*.
* **Fine (measured, partially):** the dispatch prologue also forms **two 64-bit single-bit masks**
  per method, by indexing a table of `1 << i` values — one bit chosen by the method's low 6 dword
  bits, one chosen by a 6-bit "group" field in the descriptor and indexed from bit 63 downwards.
  Where those two masks are combined and stored I could not determine: the code that consumes them
  is VMX128, which capstone does not decode. **INFERENCE:** they are per-group 64-bit dirty words
  maintained in vector registers, which is why the coarse test can be only three bits — the coarse
  bit says "some bit in this group's 64-bit word moved", and the group's translator reads the word.

**The design observation, stated plainly.** Microsoft does *not* translate per method. Per method
they do the cheapest possible thing — one table load, one OR, one byte-reversed store into a
method-indexed shadow block — and they pay the translation cost once per `BEGIN`, gated on a
three-bit mask. The dirty bits are not computed by the handlers; they are **data in the same table
that selects the handler**, so adding a method to a dirty group is a table edit, not a code edit.

---

## 5. Q3 — Textures

**MEASURED.** The entire top eighth of the method space — 320 consecutive methods from `0x1B00` to
`0x1FFC`, which is where the NV2A texture units live — maps to the *bulk state handler*. Not one
texture method has a specialised handler. Nothing is converted, uploaded, validated or cached at
method time; the parameters go into the shadow block and set a coarse dirty bit.

**INFERENCE.** Texture translation is therefore entirely deferred to the draw-time resolve, and is
driven by the shadow state rather than by the command stream. The first coarse dirty group's
translator (2,608 bytes) fans out into a ~200 KB subsystem that eventually reaches the D3D9 command
emitters, and a texture path must live in there.

**NOT DETERMINED.** I could not establish where swizzle/Morton de-tiling, palette expansion, DXT
handling or mip-chain construction happen, whether they run on the CPU or on the 360 GPU, or what
the cache key is. The bit-interleave constants that would mark a CPU Morton converter appear only in
the statically-linked 360 D3D9 code, where they are PM4 register-write masks, not swizzle masks.
The likeliest explanation is that `XGRAPHC` — the 360's texture-layout library, which is statically
linked here and whose whole purpose is exactly this kind of relayout — does the work, but I did not
confirm it, and I am not going to present it as a finding. See §9.

---

## 6. Q4 — Vertices and the fixed-function pipeline

**MEASURED.**

* The sixteen immediate-mode vertex attributes get **five dedicated handlers**, one per encoding
  (two-float, two-short, four-unsigned-byte, four-short, four-float), each covering exactly 16
  methods at the encoding's natural stride. That is the one place in the whole table where
  per-method code is spent on plain data.
* The two bulk-load windows (transform program and transform constants) have **one handler each for
  the first method of a 32-dword window** and *no* entries for the other 31. **INFERENCE:** the
  handler consumes the whole burst itself rather than re-entering dispatch 32 times.
* Indexed draw, array draw and inline-array draw each have their own handler. Indexed draw calls the
  min/max index scanner described in §3; array draw passes a dword count; inline-array draw passes a
  start/end byte range.
* Begin-primitive maps the primitive type through a **10-entry table** (the ten NV2A primitive
  types), and each arm sets a target primitive type plus two small per-primitive integers.
  **INFERENCE:** those are the vertex-count-to-primitive-count coefficients, and the arms that share
  a target are the cases the 360 has no native primitive for.

**NOT DETERMINED.** I found no evidence either way for a *shader generator* for the fixed-function
pipeline. There is no string pool, no cache-file path and no obvious code-generation loop in the
NV2A region. The shipped `XeO3_ShaderCache/*/V0.1.4_JIT_*.pak` files belong to the **360** layer
(Xenos shader JIT), not to `xefu.xex`. Since `xefu.xex` targets D3D9 on a Xenos part, and Xenos has
no fixed-function hardware, *something* must synthesise shaders — but whether that happens inside
`xefu.xex` or is delegated to the 360 D3D9 runtime linked beside it, I could not tell. See §9.

---

## 7. Q5 — Surfaces, clears and resolves

**MEASURED.**

* Surface format gets its own handler; surface clip, pitch and the colour/zeta offsets go to the
  bulk state handler.
* The two clear-*value* methods write both the method-indexed shadow slot and the corresponding
  PGRAPH register through the generic register-write routine.
* The clear-*surface* method has its own handler and **executes immediately** — it calls out of the
  interpreter and bumps a counter, rather than deferring like state does. Clears are events, not
  state.
* The interpreter reads the emulated channel's DMA put/get registers at their real NV2A offsets and
  compares the consumed pointer against put, so the pushbuffer is driven by the guest's own
  put pointer.

**MEASURED**, from the shipped launch arguments (which are per-title configuration for exactly this
layer): `xoallowtitletoskipresolves=true` (Blinx), `xoallowtitletoskipunsampledselftexresolve=true`
(Conker), `vertexBuffersWriteProtected` (Blinx), `aaBoostOn`, `aaBoostTargetMsaa=1`, and per-title
`scalingResolutions` lists. `xo` is the original-Xbox layer.

**INFERENCE.** Three things follow. First, resolves are modelled and are expensive enough to have a
per-title opt-out, which means Microsoft's surface tracking *does* synthesise EDRAM-style resolves
that the OG hardware never needed — the 360's EDRAM forces them. Second,
`vertexBuffersWriteProtected` says they **write-protect guest vertex buffers and invalidate
translated copies on guest writes**, rather than re-translating per draw or hashing contents. Third,
the emulator resolution-scales, and the valid scales are a per-title list rather than a free
parameter.

**NOT DETERMINED.** How colour and depth surfaces are keyed and tracked, how surface aliasing is
detected, and where the resolve is actually issued. See §9.

---

## 8. Q6 — Timing and synchronisation

**MEASURED.** The NV2A semaphore/fence methods have their own handler, and it does not write the
semaphore. It appends a **12-byte record to a 1,024-entry ring buffer**, masking the write index to
10 bits, and traps with a fatal message if the index would meet the consumer. That message is the
only NV2A-specific string in the entire 6 MB guest image:

> `KelvinSemaphoreRequests array full`

Around the ring's shared counter the code disables interrupts across a reservation
(`mfmsr` / `mtmsrd` around `lwarx`/`stwcx.`), so the consumer runs at interrupt or hypervisor level.
There is also a 360 D3D9 diagnostic in the same image that begins `ERR[D3D]: Unanticipated
CPU_INTERRUPT.  Sign of a corrupt command buffer?`, which belongs to the linked 360 runtime, not to
the NV2A code.

**INFERENCE.** Fences are the decoupling point. The guest's GPU-completion signals are *queued*, not
satisfied inline, and 1,024 outstanding requests is the budget for how far the emulated GPU may lag
the guest before correctness is at risk — at which point the emulator chooses to die loudly rather
than to stall or drop. Everything else in the design is arranged so that the pushbuffer consumer
never has to block: state is deferred to `BEGIN`, draws are batched by the scanner, clears execute
immediately, and only fences cross into the asynchronous world.

---

## 9. What I could not determine

Stated plainly, because a short honest report beats a long speculative one.

1. **Texture conversion.** Where swizzling, palette expansion, DXT and mip generation happen; CPU or
   GPU; cached or per-draw; what the cache key is (§5). I have only the negative result that none of
   it happens at method-dispatch time.
2. **Fixed-function shader generation.** Whether `xefu.xex` synthesises vertex/pixel shaders for the
   NV2A fixed-function and register-combiner pipelines, or delegates to the linked 360 D3D9 (§6).
3. **Surface tracking.** The key, the lifetime and the resolve trigger for colour/depth surfaces
   (§7).
4. **The fine-grained dirty words.** The two 64-bit single-bit masks are demonstrably *formed* per
   method; where they are stored and combined is inside VMX128 code that capstone 5.0.7 does not
   decode (§4). 0.92 % of the guest `.text` is undecodable for this reason, and it is concentrated
   exactly in the hot paths, because that is where the vector code is.
5. **Method semantics by name.** There are no symbols. Every method *name* in this document is
   inference from address arithmetic checked against the public NV2A method map, never a read
   symbol.
6. **The general (non-subchannel-0, non-increasing) path.** I traced its entry and its
   per-subchannel context store but did not follow it to completion.

---

## 10. What this suggests for our NV2A layer

Concrete, and aimed at `src/kernel/nv2a_pb_exec.c`, `src/kernel/nv2a_pb_scan.c` and
`src/nv2a/nv2a_metal.m` / `nv2a_metal_state.c` as they stand today.

**1. Replace the per-method `switch` with a 2,048-entry descriptor table.**
`pb_exec_method_body` is a ~900-line `switch (method)` with range tests layered in front of it
(`method >= NV097_SET_VERTEX_DATA4UB && method < … + 16*4`, and several more). Microsoft's answer is
a flat `uint32_t desc[2048]` indexed by `method >> 2`, where the low 6 bits select a handler and the
rest is data. The win is not the branch — it is that **the dirty groups, the parameter count, the
attribute index and the handler all become table data**, so adding a method or moving it between
dirty groups stops being a code edit. Build the table at startup from a static initialiser list; the
memory cost is 8 KB.

**2. Make unknown methods free.** Our executor currently does work per unrecognised method
(verbose slots, trace counters, subchannel accounting). Microsoft's unknown-method cost is one table
load and one indirect branch into a shared "skip" body, with no logging path at all. Keep our
tracing, but gate it on a descriptor bit so the hot path never tests for it.

**3. Introduce a real dirty model, and put it in the table.** We have one `s_vsh.dirty` flag and a
`surface_dirty`/`depth_dirty` pair. Microsoft has a per-method dirty-group field OR-accumulated into
a single word and tested **once per `BEGIN`, with three bits**. Adopt the shape, not the number:
give each method a small group id in the descriptor, OR the group's bit into an accumulator in the
dispatch prologue, and resolve on `NV097_SET_BEGIN_END`. This directly attacks our frame time —
today anything that resolves per method, or per draw without a dirty test, is doing work
Microsoft proved unnecessary.

**4. The shadow state block should be indexed by the raw method number.** Both Microsoft's shadow
block and their descriptor table use `method` (the byte offset) as the index, so no method ever
needs an ad-hoc index computation. A `uint32_t shadow[8][2048]` (subchannel × method) is 64 KB and
removes an entire category of off-by-one in the attribute-index and combiner-stage arithmetic.

**5. Keep `nv2a_pb_scan.c`'s pre-pass, and make it compute the index range.** Our scanner surveys;
Microsoft's equivalent scanner *consumes a whole run of draw packets and returns the min/max vertex
index for the batch*. That is the mechanism by which one vertex-buffer translation covers many
draws. Our scanner already walks packets correctly; extending it to return `(new_pb_pointer,
min_index, max_index, vertex_count)` for a run of `0x1800/0x1808/0x1810/0x1818` packets is a small
change with a large payoff, because it converts N per-draw buffer preparations into one.

**6. Thread the `END → BEGIN(same prim)` case.** Microsoft peeks three words ahead and elides the
pair, keeping the batch open. On our side that maps to *not* ending the Metal render-command encoder
and not re-resolving pipeline state when the next primitive is the same type. Given
`nv2a_metal.m` does blending, depth and stencil by hand through `raster_order_group(0)`, every
avoided batch boundary is worth more to us than it was to them.

**7. Textures: stop doing work at method time.** Microsoft gives the entire `0x1B00`–`0x1FFC` block
the *bulk* handler — no validation, no cache probe, no upload. All 320 texture methods are pure
state. If our executor touches `texture_cache` or computes anything from a texture method before the
draw, move it into the draw-time resolve behind a dirty bit.

**8. Fences should be queued, not satisfied.** Microsoft's semaphore handler appends to a 1,024-deep
ring and never blocks; it treats overflow as fatal rather than stalling. If our layer waits on
anything at fence time, that is a per-frame stall Microsoft chose not to take. A bounded queue with
a loud overflow is the design they shipped, and it is a better failure mode than a silent stall.

**9. Prefetch the command stream.** Microsoft issues four explicit cache-line prefetches on *both*
the command and the data pointer, 512 bytes ahead, in both parser loops. The equivalent on Apple
silicon is less dramatic, but the shape — treat the pushbuffer as a stream you are always 512 bytes
ahead of — is right, and it is free.

**10. Note what they did *not* do.** No per-method function pointers. No hash tables. No
string-keyed anything. No per-method logging. No interpreter/JIT split for the command stream — the
command stream is always interpreted, and the *state translation* is what gets deferred and cached.
That is the correct place to spend complexity, and it is where our ~19 ms is most likely going.

---

## Appendix — reproducing the measurements

`/usr/bin/python3` only (it is the interpreter with `pefile` 2024.8.26 and `capstone` 5.0.7).

1. **PE layer.** `pefile` on both DLLs: sections, the four exports, `.pdata`, and the `Comments` /
   `FileDescription` version strings.
2. **Host disassembly.** `capstone` `CS_ARCH_X86 / CS_MODE_64`, one pass per `.pdata` range.
   Switch recovery: a register-indirect `jmp` preceded by `shl …, 2` and an `add`/`lea` of a
   constant above `0x10000000`; the table base is that constant times four, and the preceding
   `cmp` immediate is the case count.
3. **Guest image.** Parse the XEX2 optional headers; read the file key from the security info at
   `+0x150`; AES-128-ECB-decrypt it under the all-zero key; AES-128-CBC-decrypt the base file with
   IV 0 (CommonCrypto via `ctypes` on `libSystem.B.dylib` — macOS refuses a direct `libcrypto`
   load); walk the 12-block chain, verifying each block's SHA-1 and concatenating its size-prefixed
   chunks; LZX-decompress with a 32 KB window. Two details cost real time and are worth writing
   down: the bitstream **realigns to a 16-bit boundary at every 32,768-byte output frame boundary,
   including before a block header**, and the repeated-offset LRU update for the third slot swaps
   R0 and R2 while **leaving R1 untouched** — getting that wrong produces a stream that stays in
   sync, passes every block-length check, and silently corrupts a few percent of the output.
4. **Guest disassembly.** `capstone` `CS_ARCH_PPC / CS_MODE_64 | CS_MODE_BIG_ENDIAN` over `.text` at
   RVA `0x30000`. Jump tables: a `bctr` preceded by `mtctr`/`lwzx`, with the base from the
   `lis`/`addi` pair and the count from the preceding `cmplwi`.
5. **The method table.** Found by scanning `rlwinm` encodings for a mask of `0x1FFC` adjacent to a
   shift-by-18 mask of `0x7FF` — the NV2A command-word decode — then following the two table base
   registers set up in the enclosing function's prologue.
