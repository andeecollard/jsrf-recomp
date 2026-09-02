"""
Output writers for function identification results.

Produces JSON files and a human-readable summary.
"""

import bisect
import json
import os
from collections import Counter


def write_results(functions, rw_results, crt_results, propagated,
                  rw_modules, output_dir, verbose=False, stub_results=None):
    """
    Write all output files.

    Args:
        functions: Original function list (may include vtable thunks the
            scanner discovered and appended; those carry "type": "vtable_thunk").
        rw_results: RW identification results.
        crt_results: CRT identification results.
        propagated: Clustering/propagation results.
        rw_modules: RW module -> function mappings.
        output_dir: Directory to write output files.
        verbose: Print progress info.
        stub_results: Stub classification results (optional).
    """
    os.makedirs(output_dir, exist_ok=True)
    stub_results = stub_results or {}

    # Build the enriched function database
    enriched = _build_enriched_db(functions, rw_results, crt_results,
                                  propagated, stub_results)

    # Write files
    _write_json(os.path.join(output_dir, "identified_functions.json"), enriched)
    _write_json(os.path.join(output_dir, "rw_modules.json"),
                _serialize_rw_modules(rw_modules))
    _write_json(os.path.join(output_dir, "crt_functions.json"),
                _serialize_crt(crt_results))

    # Feed the discoveries back to the disassembler as function-detection
    # seeds, in the format tools.disasm --seed-functions consumes. Without
    # this file a vtable thunk exists only in identified_functions.json, and
    # the recompiler builds its function database from functions.json -- so
    # the thunk is never translated and gets no dispatch entry. Seeding it
    # makes the detector re-disassemble the thunk with real boundaries, and
    # it enters functions.json like any other detected function.
    _write_seed_file(os.path.join(output_dir, "vtable_thunk_seeds.json"),
                     functions, enriched, verbose)

    summary = _build_summary(enriched, rw_results, crt_results, propagated, rw_modules)
    _write_json(os.path.join(output_dir, "summary.json"), summary)

    if verbose:
        _print_summary(summary)

    return summary


def _write_seed_file(path, functions, enriched, verbose=False):
    """Write the discovered vtable thunks as a --seed-functions file.

    Only entries the scanner *discovered* (method "vtable_thunk") qualify,
    and only those that sit outside every function body the disassembler
    already knew. A vtable entry landing inside a known function is evidence
    the "vtable" is really a pointer array (a jump table, a callback list)
    referencing mid-function locations; seeding it would truncate the host
    function and manufacture a bogus body from its middle. Thunks in the gap
    between functions are what the scanner's thunk pattern is defined for,
    and those are safe to hand to the detector.

    The estimated 32-byte end/size recorded for a discovered thunk is not
    written: it is a placeholder the detector refines on re-disassembly, and
    the seed loader only reads "start" anyway. Provenance fields are kept so
    a reader can see which vtable and slot each seed came from.
    """
    # Bodies of the functions as the disassembler knew them, before this run
    # appended its discoveries. The appended thunks carry "type":
    # "vtable_thunk"; everything else came from functions.json. Their
    # estimated bodies must not be used to filter each other.
    original = [f for f in functions if f.get("type") != "vtable_thunk"]
    starts = sorted(int(f["start"], 16) for f in original if "end" in f)
    ends = [int(f["end"], 16) for f in sorted(
        (f for f in original if "end" in f), key=lambda x: int(x["start"], 16))]

    def inside_body(addr):
        i = bisect.bisect_right(starts, addr) - 1
        return i >= 0 and starts[i] < addr < ends[i]

    seeds = []
    excluded_inside_body = 0
    for entry in enriched:
        if entry.get("method") != "vtable_thunk":
            continue
        addr = int(entry["start"], 16)
        if inside_body(addr):
            excluded_inside_body += 1
            continue
        seed = {"start": entry["start"]}
        for key in ("category", "subcategory", "confidence", "method",
                    "vtable_addr", "vtable_index"):
            if key in entry:
                seed[key] = entry[key]
        seed["source"] = "func_id-vtable-scanner"
        seeds.append(seed)

    _write_json(path, seeds)

    if verbose:
        print(f"  Vtable thunk seeds: {len(seeds)} written to "
              f"{os.path.basename(path)}")
        if excluded_inside_body:
            print(f"    (excluded {excluded_inside_body} vtable entries that "
                  f"land inside a known function body -- pointer arrays, not "
                  f"function starts)")

    return len(seeds)


def _build_enriched_db(functions, rw_results, crt_results, propagated,
                       stub_results):
    """Build enriched function entries with classification info."""
    enriched = []
    for f in functions:
        addr = int(f["start"], 16)
        entry = {
            "start": f["start"],
            "end": f["end"],
            "size": f["size"],
            "name": f["name"],
            "section": f["section"],
        }

        if addr in crt_results:
            info = crt_results[addr]
            entry["category"] = "crt"
            entry["identified_name"] = info["name"]
            entry["confidence"] = info["confidence"]
            entry["method"] = info["method"]
        elif addr in rw_results:
            info = rw_results[addr]
            entry["category"] = info["category"]
            entry["module"] = info.get("module", "")
            entry["source_file"] = info.get("source_file", "")
            entry["confidence"] = info["confidence"]
            entry["method"] = info["method"]
        elif addr in propagated:
            info = propagated[addr]
            entry["category"] = info["category"]
            entry["subcategory"] = info.get("subcategory")
            entry["confidence"] = info["confidence"]
            entry["method"] = info["method"]
            # Propagation and the vtable scanner classify without naming, but
            # the D3D8 identifier shares this bucket and does produce a name.
            if info.get("name"):
                entry["identified_name"] = info["name"]
            if info.get("nv2a_methods"):
                entry["nv2a_methods"] = info["nv2a_methods"]
            if "vtable_addr" in info:
                entry["vtable_addr"] = f"0x{info['vtable_addr']:08X}"
                entry["vtable_index"] = info["vtable_index"]
        elif addr in stub_results:
            info = stub_results[addr]
            entry["category"] = info["category"]
            entry["stub_type"] = info.get("stub_type", "")
            entry["confidence"] = info["confidence"]
            entry["method"] = info["method"]
        else:
            entry["category"] = "unknown"
            entry["confidence"] = 0.0
            entry["method"] = "none"

        enriched.append(entry)

    return enriched


def _build_summary(enriched, rw_results, crt_results, propagated, rw_modules):
    """Build summary statistics."""
    total = len(enriched)
    cat_counts = Counter(e["category"] for e in enriched)
    method_counts = Counter(e["method"] for e in enriched)

    # Group subcategories
    rw_total = sum(v for k, v in cat_counts.items() if k.startswith("rw_"))
    game_total = sum(v for k, v in cat_counts.items() if k.startswith("game_"))
    crt_total = cat_counts.get("crt", 0)
    data_init_total = cat_counts.get("data_init", 0)
    unknown_total = cat_counts.get("unknown", 0)

    # Count vtable functions specifically
    vtable_total = sum(1 for e in enriched if e.get("method") in ("vtable_scan", "vtable_ctor"))

    # RW modules with function counts
    rw_module_summary = {}
    for name, mod in rw_modules.items():
        rw_module_summary[name] = {
            "category": mod["category"],
            "path": mod["path"],
            "num_functions": len(mod["functions"]),
        }

    return {
        "total_functions": total,
        "classification": {
            "renderware": rw_total,
            "crt": crt_total,
            "data_init": data_init_total,
            "game_classified": game_total,
            "vtable_methods": vtable_total,
            "unknown": unknown_total,
        },
        "percentages": {
            "renderware": round(rw_total / total * 100, 1) if total else 0,
            "crt": round(crt_total / total * 100, 1) if total else 0,
            "data_init": round(data_init_total / total * 100, 1) if total else 0,
            "game_classified": round(game_total / total * 100, 1) if total else 0,
            "unknown": round(unknown_total / total * 100, 1) if total else 0,
        },
        "by_category": {k: v for k, v in sorted(cat_counts.items())},
        "by_method": {k: v for k, v in sorted(method_counts.items())},
        "rw_modules": rw_module_summary,
        "rw_module_count": len(rw_modules),
    }


def _serialize_rw_modules(rw_modules):
    """Serialize RW modules for JSON output."""
    result = {}
    for name, mod in sorted(rw_modules.items()):
        result[name] = {
            "address": f"0x{mod['address']:08X}",
            "category": mod["category"],
            "path": mod["path"],
            "functions": [f"0x{a:08X}" for a in mod["functions"]],
            "num_functions": len(mod["functions"]),
        }
    return result


def _serialize_crt(crt_results):
    """Serialize CRT results for JSON output."""
    result = []
    for addr in sorted(crt_results):
        info = crt_results[addr]
        result.append({
            "address": f"0x{addr:08X}",
            "name": info["name"],
            "confidence": info["confidence"],
        })
    return result


def _print_summary(summary):
    """Print a human-readable summary to stdout."""
    print("\n" + "=" * 60)
    print("FUNCTION IDENTIFICATION SUMMARY")
    print("=" * 60)
    total = summary["total_functions"]
    cls = summary["classification"]
    pct = summary["percentages"]

    print(f"  Total functions:    {total:,}")
    print(f"  RenderWare:         {cls['renderware']:,}  ({pct['renderware']}%)")
    print(f"  CRT/MSVC:           {cls['crt']:,}  ({pct['crt']}%)")
    print(f"  Data init stubs:    {cls['data_init']:,}  ({pct['data_init']}%)")
    print(f"  Game (classified):  {cls['game_classified']:,}  ({pct['game_classified']}%)")
    vtable_count = cls.get('vtable_methods', 0)
    if vtable_count:
        print(f"    (vtable methods): {vtable_count:,}")
    print(f"  Unknown:            {cls['unknown']:,}  ({pct['unknown']}%)")

    print(f"\n  RW source modules:  {summary['rw_module_count']}")

    print("\n  By category:")
    for cat, count in sorted(summary["by_category"].items(), key=lambda x: -x[1]):
        print(f"    {cat:30s} {count:6,}")

    print("\n  By method:")
    for method, count in sorted(summary["by_method"].items(), key=lambda x: -x[1]):
        print(f"    {method:30s} {count:6,}")
    print("=" * 60)


def _write_json(path, data):
    """Write data as formatted JSON."""
    with open(path, "w", encoding="utf-8") as f:
        json.dump(data, f, indent=2, ensure_ascii=False)
