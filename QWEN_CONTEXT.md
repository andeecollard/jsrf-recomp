# JSRF stock-upstream regeneration

Run all commands from the repository root.

- Regeneration: `diagnostics/jsrf_first_fault/regenerate.sh`
- Python requirement: Python 3.10+ with Capstone; override the interpreter with `PYTHON=/path/to/python` when needed.
- Canonical XBE: `/Users/andrewcollard/Library/Mobile Documents/com~apple~CloudDocs/Jet Set Radio Future/Jet Set Radio Future (US)/default.xbe`
- Canonical analysis JSON: `build-macos/jsrf-first-fault/jsrf_analysis.json`
- Disassembly output: `build-macos/jsrf-first-fault/disasm`
- Function-identification output: `build-macos/jsrf-first-fault/func-id`
- Generated C/dispatch output: `build-macos/jsrf-first-fault/gen`
- ICALL seed file: `diagnostics/jsrf_first_fault/icall_seed.json`
- Thread/callback seed file: `diagnostics/jsrf_first_fault/thread_start_seed.json`
- Recompiler trace list: `diagnostics/jsrf_first_fault/reach_trace.json`
- Build: `cmake --build build-macos/jsrf-first-fault/build --target jsrf_first_fault --parallel`
- Runtime: `build-macos/jsrf-first-fault/build/jsrf_first_fault`

The seed files are consumed only by `tools.disasm --seed-functions`; `func_id` classifications do not add entries to the recompiler's disassembly-backed function database. The proven regeneration result with ICALL seeds `0x00181036` and `0x0017BF79` is 3,889/3,889 translated functions with zero failures.
