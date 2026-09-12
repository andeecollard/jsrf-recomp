# Zero-divergence dump pair — Windows vs macOS at guest clock ~2000

Preserved as evidence. Do not regenerate these files; take new anchors as new
directories beside this one.

## What was compared

| file | host | build | guest clock at dump |
|---|---|---|---|
| `windows_clang_clock2006.json` | Windows (CrossOver, bottle `recomp-gate`) | clang/mingw, native TLS (`tools/windows/clang-mingw.cmake`) | **2006** |
| `macos_metal_clock2008.json` | macOS arm64 | clang, `RECOMP_METAL=1` | **2008** |
| `macos_metal_clock2132_runB.json` | macOS arm64 | same as above, second run | **2132** |

Guest clock = entries to `sub_000123E0`, the manager's per-frame tick, counted
by `jsrf_func_hit` and read with `jsrf_func_hit_count()`. Both hosts ran the
**same gen tree** and the same recompiled guest code.

## Method

    RECOMP_OBJECT_DUMP=<dir> RECOMP_OBJECT_DUMP_AT=2000

`jsrf_object_dump()` writes the inventory once, when the clock first reaches
the requested value. Comparison is
`diagnostics/jsrf_first_fault/compare_objects.py A.json B.json`, which resolves
pointer-valued fields **symbolically** (to the id of the object pointed at) so
that two different heap layouts do not read as divergence. Vtables are compared
raw, because the XBE is mapped at the same VA on both hosts.

## Result

    root address     0x040d3a70 on BOTH hosts (delta +0x0)
    manager          all 12 compared fields identical
    id set           identical, 9 ids
    per-object       every common object matches on every compared field
    -> 0 divergent field group(s)

Compared ranges: manager fields `live, skip_draw, draw_mode, state_7930,
state_7934, state_7EC4, exec_root, draw_root, draw_root_tail, draw_sort_root,
draw_sort_tail, draw_sort_bins`; per object `vtable, flags, stored_id,
draw_child_mask, zsort_key, zsort, extra_44, extra_48, parent, child,
sibling_before, sibling_next, draw_next, draw_before_ptr, draw_last_ptr,
fz, tx, ty, tz`.

## Noise floor (same host, two runs)

macOS run A (clock 2008) against macOS run B (clock 2132): **3 divergent field
groups** — `zsort` and `extra_44`, which hold heap pointers but sit in the
comparator's RAW list so two heaps disagree by construction, and one `tx`
float differing by -0.0000.

So the same-host noise floor is *worse* than the cross-host result at this
anchor. Anything above 3 field groups at a later anchor is signal.

## What this does NOT establish

* Nothing about the **tutorial**. Clock 2000 is before the title teardown
  (~2400). The Corn sequence is much later and has not been compared.
* Nothing about Windows being a correct control. At the deepest point measured
  Windows reaches **12 of 17** CPlayer gameplay markers where macOS reaches
  17 of 17. That gap is unexplained and must be resolved before Windows is
  trusted as a known-good reference for anything in gameplay.
