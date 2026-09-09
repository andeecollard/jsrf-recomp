# Canonical traversal mapping — 2026-09-06

**CONFIRMED semantic mapping: our `sub_00011D00` is the canonical `CActBase::recursiveExec1Default`, formerly `GameObj::recursivePostExecDefault`.** The US machine code, recursive control flow, deletion callees, object offsets, virtual slot and generated recompilation agree. It is the second execution traversal, including deferred deletion, not a draw traversal. The historical “mode 0” means the Default family because all four family-selection booleans are clear.

This confidence applies to the inspected mapping, not a claim of byte-identical complete executables or original developer symbol provenance.

## Source and executable identity

Cloned directly from https://codeberg.org/KeybadeBlox/JSRF-Decompilation.git to `/private/tmp/jsrf-canonical-20260906`. No mirror used.

- Examined HEAD: `a4f7af133a15d75ec6a33ff02b3c8b7b64ddffd4`, 2026-08-13, “Fix delinking for Action.obj”.
- Canonical README explicitly targets **Jet Set Radio Future North American Standalone**. No exact target executable digest was found in the inspected repository documentation/build configuration; a particular mastering beyond that release is therefore not independently established.
- Our executable: `../Jet Set Radio Future (US)/default.xbe`, 2,281,472 bytes.
- SHA-256: `bb2410618c35ccab1ab8ad989194bbd50619eeb648a03b21483efca57f36547d`.
- XBE certificate title ID `0x5345000A`, version `1`, allowed-region mask `7`. The region mask alone does not prove North American provenance.
- The static disassembler's `../jsrf_stock_test/disasm_output/summary.json` records this US executable as its input. The currently instrumented `build-macos/jsrf-first-fault/gen/recomp_0000.c` retains the same offsets, branches and calls in `sub_00011D00`, including `[vtable+8]`, child recursion and deletion dispatch.
- Independent correspondence anchors: constructor at `0x12100`, destructor at `0x11000`, parent accessor at `0x11BD0`, base vtable at `0x1C4390`, manager global at `0x22FCE0`, object registration at `0x12870`, and manager state-selection branches. These agree structurally, not just numerically.

## Exact canonical evidence and historical names

Current evidence is `ghidra/symboltable.tsv`, `decompile/src/JSRF/Action.hpp`, and `Action.cpp`. Most traversal bodies in `Action.cpp` are explicitly **unimplemented**. They are declarations/symbol evidence, not working recovered implementations to copy.

Canonical commit `836b5eaa41148263a5803a1559cd9b7ea12ba7cc` (“Incorporate known names from Smilebit code”, 2026-03-31) directly renames the user's requested symbols. It changes `Game` to `CActMan`, `GameObj` to `CActBase`, `recursiveExecDefault` to `recursiveExec0Default`, `drawListDefault` to `drawManyDefault`, and `recursivePostExecDefault` to `recursiveExec1Default`, with corresponding changes in the other families. Its message explains that analogous names were informed by other Smilebit games; these are not proof that JSRF shipped with those exact symbols. The relevant history diff is preserved alongside this report.

| Family | Old recursiveExec / current recursiveExec0 | Old drawList / current drawMany | drawTree1 | drawTree2 | recursiveExec1 |
|---|---|---|---|---|---|
| Default | 00011070 | 000110A0 | 00011220 | 00011260 | 00011D00 |
| Event | 000112A0 | 000112D0 | 00011450 | 00011490 | 00011DA0 |
| CoveredPause | 000114D0 | 00011500 | 00011680 | 000116C0 | 00011E40 |
| FreezeCam | 00011700 | 00011730 | 000118B0 | 000118F0 | 00011EE0 |
| UncoveredPause | 00011930 | 00011960 | 00011AE0 | 00011B20 | 00011F80 |

The extra family is directly relevant to Default selection. Current `Action.cpp` accidentally repeats the `0x11D00` address comment for `recursiveExec1Event`; the symbol table correctly gives `0x11DA0`. This is one reason not to trust comments alone.

## Verified Default control flow

- `0x11070`: skip objects marked for deletion by bit 31 of `+4`; call virtual `+4` (Exec0Default); recurse through child `+0x28`; iterate sibling `+0x30`. It does not call drawMany or either drawTree function.
- `0x110A0`: filtered draw-list traversal, uses flags `+4`, draw-child mask `+0xC`, draw links `+0x34`, depth/sort key `+0x10/+0x14`, and sorted link `+0x40`. Calls virtual `+0xC` with an argument; the sorting branch calls `0x129D0`, `0x12A00`, `0x131A0`, `0x129F0`. It does not directly invoke either drawTree function.
- `0x11220`: **confirmed drawTreeDefault1**; tests flag `0x40000`, calls virtual `+0xC` with argument zero only when clear, then recurses into child `+0x28` and iterates sibling `+0x30`.
- `0x11260`: **confirmed drawTreeDefault2**; same traversal and virtual slot, but calls the draw callback only when `0x40000` is set. Both tree variants use matrix helpers `0x1BA9C0` and `0x1BAA50` around the callback.
- `0x11D00`: saves next sibling before callbacks. If deletion bit is set, destroys descendants via `0x11B90` and virtual slot zero with argument 1. Otherwise calls matrix helper `0x1BA8A0`, then virtual `+8` at `0x11D67`, checks deletion again, and either destroys or recurses at `0x11D88`.

The verified hierarchy is:

```
CActMan::ActionExec (0x123E0)
  -> selected recursiveExec0 family (Default 0x11070)
  -> intervening manager/subsystem work
  -> selected recursiveExec1 family (Default 0x11D00)

CActMan::drawListSub (0x125E0)
  -> selected drawMany family (Default 0x110A0)
  -> virtual DrawList callback (Default slot +0xC)

CActMan::drawTree1 (0x12680)
  -> selected drawTree1 family (Default 0x11220)
  -> virtual DrawList callback (Default slot +0xC)
```

The proposed linear `recursiveExecDefault -> drawListDefault -> drawTreeDefault1/2` chain is not present in those walker bodies. Additional behavior can be reached through derived virtual callbacks; that does not make these walkers one function or prove that chain.

## Object layout and dispatch

US constructor `0x12100` writes base vtable `0x1C4390`, clears all four tree links, stores ID and flags, registers through `0x12870`, links through `0x12020`, sets draw-child mask to 1, clears sort key and increments manager `+0x87E8`. Destructor `0x11000` repairs sibling/parent/root links and decrements that count. `0x11B90` destroys descendants before their parent via slot zero.

| Offset | Meaning |
|---|---|
| +00 | vtable pointer |
| +04 / +08 | flags / action ID |
| +0C | draw-child mask |
| +10 / +14 / +18 | depth / sort key / translation vector |
| +24 / +28 | parent / first child |
| +2C / +30 | previous / next sibling |
| +34 / +38 / +3C | draw-next / back-link pointer / last-link pointer |
| +40 | sorted draw link |

The header declares callbacks as virtual methods, not independent function pointers stored in the tree-link fields. US base vtable contents confirm destructor followed by five triples: `Exec0`, `Exec1`, `DrawList`. Default slots are `+4/+8/+0xC`; Event `+0x10/+0x14/+0x18`; CoveredPause `+0x1C/+0x20/+0x24`; FreezeCam `+0x28/+0x2C/+0x30`; UncoveredPause `+0x34/+0x38/+0x3C`. Base Exec methods alias `0x11C90`, and base one-argument draw methods alias `0x11C80`.

## Family selection and “mode 0”

Manager pointer is `[0x22FCE0]`. State booleans are manager `+0x40` CoveredPause, `+0x44` Event, `+0x48` FreezeCam, `+0x4C` UncoveredPause, in that precedence. **Default means all four are zero.**

Machine-code selectors are `0x12418..0x124BE` for Exec0, `0x12509..0x12547` for Exec1, `0x12587..0x125C4` for drawOne, `0x12614..0x12674` for drawMany, and `0x1268A..0x126C6` for drawTree1. The Default Exec1 branch directly calls `0x11D00` at `0x12547`.

Setters `0x126D0/0x126F0/0x12710/0x12730` queue enable/disable requests in `+0x50/+0x54`, `+0x58/+0x5C`, `+0x60/+0x64`, `+0x68/+0x6C`. `IdleSub` applies them at `0x13A85..0x13AFA`, then clears requests at `0x13AFD..0x13B12`.

Historical provenance is `CLAUDE_HANDOVER_2026-09-04_STALL_FOUND.txt`, which explicitly describes all four booleans as zero and calls that “mode 0”. Its subsequent “draw pass” label for slot +8, and the same label in `JSRF_GOALS_2026-09-04_AUDIO_STALL.md`, are incorrect. The separate `eDRAWMODE` and GPU/VSH mode counters must not be substituted for these state booleans.

## Consequence for the recorded failure

Mapping is established before interpreting the failure. The historical stack includes `0x11D42`, `0x11D8D`, `0x1254C`, `0x13B24`. `0x11D42` is the return from descendant destruction (`call 0x11B90` at `0x11D3D`), whereas the normal Exec1 virtual-call return is `0x11D6A`. Therefore that stack does **not** establish that `sub_00177FE0` was reached through the normal +8 callback, and it does not establish a draw callback at all. Deletion traversal/slot-zero dispatch must remain a live alternative.

The current probe at `0x11D63` tests `[vtable+8] == 0x177FE0`; that is one hypothesis, not evidence that it happened. No matching TREE record was found in the inspected build log search. Determining the corrupted object's first invalid link or actual dispatch requires a discriminating execution trace; this mapping report does not claim the corruption's root cause.

No crash fix, runtime symbol import, or NV2A implementation change was made. Existing uncommitted work was preserved.

## Reproduction

Run from the repository root:

```
../phase1/venv/bin/python diagnostics/jsrf_first_fault/canonical_mapping/inspect_us.py > diagnostics/jsrf_first_fault/canonical_mapping/us_disassembly.txt
```

The script reads the US XBE section table directly and uses Capstone 5.0.7. `us_disassembly.txt` preserves bytes and decoded instructions for the inspected functions, family variants, manager selectors, link operations, and base vtable. No executable is distributed with this report.
