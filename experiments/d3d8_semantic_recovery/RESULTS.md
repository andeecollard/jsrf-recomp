# Initial result — 22 September 2026

## Corrected blind test: should SemIf stay in this workflow?

An audit found that `to_semif.py` included the case `id` inside the model's
evidence. Curated IDs sometimes described the answer, and the first
XbSymbolDatabase fixture used function names as IDs. **All earlier curated
scores and the initial 7/10 symbol-fixture score are contaminated and must
not be used to judge SemIf.** The converter now keeps IDs only as output keys;
the model sees the methods, disassembly, address, and neutral facts.

With the same pinned 4B model and corrected input, the curated score is
**9/16**. The older 27B curated scores have not been rerun and are withdrawn.
The earlier real-function 4B score remains **3/8**; its IDs were numeric
addresses and did not convey labels, though the redundant ID is now removed.

The corrected XbSymbolDatabase test uses ten named D3D8 operations and seven
unrelated D3D8 controls, with symbol names withheld from the model. Signature
addresses often lie inside a disassembler function (sometimes mid-instruction),
so the fixture starts disassembly at the containing function's entry. The
model was given up to 96 instructions, the scanner's method list, function
size, and a boolean indicating an interior symbol. The function-name mapping
is evaluation evidence, not a guarantee of exact function boundaries.

| Corrected 4B result | Count |
|---|---:|
| Previously unnamed operations recovered | **0/5** |
| Indexed draw variants corrected from scanner's broad `DrawVertices` label | 1/2 |
| Known non-indexed `DrawVertices` wrongly called indexed | 1/1 |
| Unrelated controls correctly rejected | 7/7 |
| All symbol-labelled operations, including scanner hits | 2/10 |

The five scanner misses were Clear, SetLight, SetRenderTarget, SetTexture,
and Swap. SemIf abstained on all five. It correctly retained the known vertex
shader constant setter. This is **not evidence that SemIf finds useful
pushbuffer patterns the current scanner misses**. The one indexed-draw
correction is worth inspecting by deterministic dataflow, but the erroneous
non-indexed draw call prevents accepting its labels automatically. For this
static-recomp task, stop investing in SemIf-based discovery unless a different
input representation demonstrates verified finds on a blind holdout.

`build_symbol_cases.py` rebuilds the local instruction fixture from the XBE,
XbSymbolDatabase CLI dump, disassembler functions, and identifier output.
`results_semif_symbol_blind_4b.json` preserves the 17 predictions, expected
labels, scanner labels, option scores, and prompt hashes without XBE bytes.
The retail-derived fixture and corrected scoring outputs are in `/tmp` on
this machine (`jsrf-semif-symbol-blind-v2*` and
`jsrf-semif-curated-blind-v2*`), not in the repository.

## Symbol-anchored DRAW_ARRAYS check

The existing XbSymbolDatabase CLI dump (`~/jsrf-build/jsrf-xbsymbols.txt`)
names `D3D8__D3DDevice_DrawVertices = 0x00199300` for this JSRF XBE.
This is useful independent semantic evidence, but **0x00199300 is inside**
the routine beginning at `0x001992C0`, not its entry: Capstone shows the
prologue at `0x001992C0`, and `0x00199300` is a `mov [eax+4], ecx` after
the packet header store. Thus the signature identifies the neighborhood,
while the recomp disassembly is needed to establish function boundaries.

The routine's command words can be derived without a model. It stores
`0x000417FC` (`SET_BEGIN_END`, count 1), then the primitive type, then
`0x40001810 | (((vertex_count - 1) >> 8) + 1) << 18`
(`DRAW_ARRAYS`, non-incrementing, with one parameter per 256 vertices).
It writes each parameter as `(batch_count - 1) << 24 | start_vertex`,
advancing `start_vertex` by 256 for full batches, then writes
`0x000417FC, 0` to end the primitive. The last parameter is capped at
256 vertices. The function returns with `ret 0xC` and updates the pushbuffer
put pointer. This is consistent with `DrawVertices` and contradicts the
SemIf `draw_indexed` choice for this routine. The source address and
instruction decode used here are local retail-XBE data; they are not
included in the repository.

`check_draw_vertices.py` now executes the original XBE routine bytes in
Unicorn, stubbing only its validation and pushbuffer-reservation calls. In
20 cases (two primitive/start pairs, each with vertex counts 1, 2, 255,
256, 257, 258, 511, 512, 513, and 1024), its complete pushbuffer word
sequence matched the separately encoded packet formula. The emulator also
checked the resulting put pointer, return register, callee-saved registers,
stdcall stack cleanup, and absence of writes elsewhere in the mapped device
and pushbuffer regions. Run it with:

```sh
python3 experiments/d3d8_semantic_recovery/check_draw_vertices.py /path/to/SegaJSRF.xbe
```

This verifies the **packet-writing body** under the two helper stubs. It is
not a whole-call proof, nor a comparison against generated recomp code. Before
installing a static replacement, test the helpers' side effects and compare
the generated recomp output or a proposed replacement against the same
oracle. XbSymbolDatabase provides a useful name and XDK context but does not
establish that equivalence or reliably mark the routine's entry here.

## SemIf direct option scoring

SemIf commit `1f2dea3e25379f9dfc98cb83c324f00ab5deda37` ran through its
MLX backend. The pinned frozen baseline was `Qwen/Qwen3.5-4B` revision
`851bf6e806efd8d0a36b00ddf55e13ccb7b8cd0a`, in source precision.
The comparison models were local 4-bit Qwen3.8-27B MLX checkpoints. The
fixtures and options were identical within each model's two runs.

| Model and readout | Curated | Real extracted functions |
|---|---:|---:|
| SemIf direct logits, pinned Qwen3.5-4B | 9/16 corrected; earlier 10/16 invalid | **3/8** |
| SemIf direct logits, local Qwen3.8-27B standard 4-bit | earlier 13/16 invalid; not rerun | 3/8 |
| SemIf direct logits, local Qwen3.8-27B uncensored 4-bit | earlier 13/16 invalid; not rerun | not run |
| LM Studio generated JSON, local Qwen3.8-27B uncensored | earlier 13/16 invalid; not rerun | 1/8 |

### Adding bounded disassembly

I regenerated the eight real cases with up to 64 x86 instructions per
function, caller and direct-call counts, and direct-call targets. The same
pinned 4B model scored **3/8 again**. It retained the three setter matches,
abstained on three draw candidates, called one `draw_indexed`, and called
one `swap`. Median decision time rose from 0.68 to 1.62 seconds.

The `draw_indexed` candidate is `0x001992C0`. Its disassembly constructs a
pushbuffer word whose low method bits are `0x1810`, `NV097_DRAW_ARRAYS`; the
indexed interpretation is contradicted by that instruction. The `swap`
candidate is `0x00199660`, which lacks the flip-method pair required for a
swap. These are substantive mistakes, even though the overall score stayed
at 3/8.

The labels for the eight exact API variants still come from the existing
identifier. The XDK 4134 signature database in this workspace has no matching
signature for these eight addresses, so this run cannot establish independent
exact-call accuracy. It does establish that adding these bounded instruction
windows did not make the pinned 4B model reliable enough for static rewriting.

The generated disassembly fixture is kept at
`/tmp/semif-jsrf-enriched-cases.jsonl` on this machine to avoid placing
derived retail XBE instruction text in the source tree. The SemIf row output
is `results_semif_enriched_real_4b.jsonl`.

The 4B direct scorer had a 0.63-second median per curated decision after
loading in the invalid ID-leaking run. Its historical curated misses were `flip_stall_alone`, `inline_draw`,
`plain_draw`, `render_target`, `broad_decisive_draw`, and
`broad_weak_texture`. On real functions it correctly selected the fog,
combiner, and transform-constant setters, then abstained on all five draw
functions whose recovered method list contains only `SET_BEGIN_END`.

The model's abstention on those five may be prudent: the deterministic
identifier assigns a broad `DrawVertices` label from one marker, but the
method set alone does not prove the exact API variant. Thus the 3/8 score
measures agreement with that identifier, not independently verified D3D8
semantics. The curated cases are synthetic shapes from the identifier tests;
they are a useful gate, not a blind external benchmark.

This is enough to reject automatic function replacement from method names
alone. A useful follow-up needs bounded disassembly and independently
verified labels, then must compare pushbuffer and guest-state effects before
accepting any rewrite.

Per-case probabilities and exact model artifact hashes are in
`results_semif_curated_4b.jsonl`, `results_semif_real_4b.jsonl`, and the
corresponding 27B files. `to_semif.py` generates the SemIf input without
showing expected labels to the model.

## LM Studio chat baseline

Model: `qwen3.8-27b-uncensored-mlx`, served locally by LM Studio.

| Fixture | Correct | Accuracy | Meaning |
|---|---:|---:|---|
| Curated adversarial method sets | 13/16 invalid | — | Case IDs leaked into the prompt; withdrawn |
| Real functions extracted from `SegaJSRF.xbe` | 1/8 | 12.5% | Bare recovered method sets are insufficient |

These curated misses describe the invalid ID-leaking run and are retained
only as history:

- `NV097_SET_BEGIN_END` plus `NV097_INLINE_ARRAY` was called `draw_vertices`
  instead of `draw_up`;
- the plain `SET_BEGIN_END` case was rejected as insufficient evidence;
- a broad indexed draw was called a state block despite its decisive draw
  markers.

On the real-function fixture the model identified `SetFogState`, then abstained
on the combiner setter, transform-constant setter, and five draw functions.
The real labels come from the existing deterministic NV2A classifier and are
therefore a pipeline comparison rather than independent ground truth.

This result rejects automatic semantic rewriting from method names alone. A
second experiment is justified only after adding bounded disassembly, call
graph position, device-field accesses, argument flow, and independent XDK
signature labels. Any accepted rewrite still needs deterministic equivalence
checking; model confidence is not proof.

The LM Studio endpoint placed structured JSON in `reasoning_content` rather
than `content`; the runner now handles that behavior. Median latency was a few
seconds per case, which is acceptable offline and irrelevant to runtime.
