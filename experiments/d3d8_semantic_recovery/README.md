# D3D8 semantic recovery experiment

This experiment asks a local LM Studio model to classify Xbox D3D8/NV2A
function evidence into a small semantic operation family.  It is an offline
feasibility test for static recompilation; it is not part of the renderer.

The follow-up runs the same fixtures through actual SemIf direct option
scoring. Results and model identities are in `RESULTS.md`.

An earlier version copied case IDs into model evidence, leaking some labels.
`to_semif.py` and `run.py` now omit IDs and label metadata from prompts.
Use the corrected scores in `RESULTS.md`; older curated result files are
historical and invalid for accuracy comparisons.

The committed fixture contains difficult cases taken from
`tools/func_id/test_d3d8_identifier.py`, including broad state blocks,
incidental methods, draw variants, and cases that must abstain.  The labels
are hidden from the model and scored after each response.

`real_cases.jsonl` is generated from the retail JSRF XBE with the repository's
disassembler and deterministic NV2A identifier. Its labels are a pipeline
sanity check rather than independent ground truth.

## Run

1. Start LM Studio's local server and load the Qwen model.
2. Run:

```sh
python3 experiments/d3d8_semantic_recovery/run.py
```

Run the extracted real-function fixture with:

```sh
python3 experiments/d3d8_semantic_recovery/run.py \
  --cases experiments/d3d8_semantic_recovery/real_cases.jsonl
```

The runner discovers the loaded model from `http://127.0.0.1:1234/v1/models`.
Override either value when needed:

```sh
python3 experiments/d3d8_semantic_recovery/run.py \
  --base-url http://127.0.0.1:1234/v1 \
  --model lmstudio/local/qwen3.8-27b \
  --output /tmp/d3d8-semantic-results.json
```

Use `--repeat 3` to expose unstable classifications.  The output contains the
exact model id, response, latency, and score for every attempt.

## Interpretation

This phase answers only a gate question: can the model reliably recover known
operation families from bounded static evidence and abstain when the evidence
is insufficient?  A useful result requires:

- 100% on decisive draw, clear, and swap cases;
- 100% on abstention and broad-state-block cases;
- identical choices across repeated temperature-zero runs.

Passing does not justify rewriting functions.  The next phase must use real
function bodies and require deterministic equivalence of emitted pushbuffer
words, guest-memory effects, return values, and register effects.

To regenerate the real fixture after running `tools.func_id`:

```sh
python3 experiments/d3d8_semantic_recovery/build_real_cases.py \
  /path/to/func-id/identified_functions.json \
  -o experiments/d3d8_semantic_recovery/real_cases.jsonl
```

To add bounded disassembly from the same XBE, run the disassembler and then:

```sh
PYTHONPATH=/path/to/capstone/site-packages python3 \
  experiments/d3d8_semantic_recovery/build_enriched_cases.py \
  --cases experiments/d3d8_semantic_recovery/real_cases.jsonl \
  --functions /path/to/disasm/functions.json \
  --xbe /path/to/SegaJSRF.xbe \
  -o /tmp/d3d8-enriched-cases.jsonl
```

Convert that fixture with `to_semif.py` and score it using the SemIf command
below. Keep the generated instruction text outside the source tree.

LM Studio's chat endpoint does not expose SemIf's direct option logits. This
runner uses constrained JSON classification as the chat baseline. `to_semif.py`
converts the same fixtures for SemIf's direct-logit scorer.

For the XbSymbolDatabase holdout, build the local retail-XBE fixture outside
the repository:

```sh
PYTHONPATH=/path/to/capstone/site-packages python3 \
  experiments/d3d8_semantic_recovery/build_symbol_cases.py \
  --symbols /path/to/jsrf-xbsymbols.txt \
  --functions /path/to/disasm/functions.json \
  --identified /path/to/func-id/identified_functions.json \
  --xbe /path/to/SegaJSRF.xbe \
  -o /tmp/jsrf-semif-symbol-cases.jsonl
```

## SemIf direct scoring

With SemIf installed, convert and score either fixture:

```sh
python3 experiments/d3d8_semantic_recovery/to_semif.py \
  experiments/d3d8_semantic_recovery/cases.jsonl \
  -o /tmp/d3d8-semif-input.jsonl

semif-score --backend mlx --mode direct \
  --model Qwen/Qwen3.5-4B \
  --revision 851bf6e806efd8d0a36b00ddf55e13ccb7b8cd0a \
  --input /tmp/d3d8-semif-input.jsonl \
  --output /tmp/d3d8-semif-results.jsonl
```

For the real fixture, change `cases.jsonl` to `real_cases.jsonl` and use new
input and output paths. SemIf's output files are create-only. The model is
downloaded from Hugging Face unless the pinned weights are already cached.
