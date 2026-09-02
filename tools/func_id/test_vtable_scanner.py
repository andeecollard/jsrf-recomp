"""Self-checks for section-aware, conservative vtable discovery.

Run: py -3 tools/func_id/test_vtable_scanner.py
"""

import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", ".."))

from tools.func_id.vtable_scanner import (  # noqa: E402
    _get_code_ranges_from_sections,
    _is_plausible_thunk_entry,
    scan_vtables,
)


def _sections():
    return [
        {"name": ".text", "va": 0x10000, "size": 0x100,
         "raw": 0x100, "raw_size": 0x100, "executable": True},
        {"name": "DSOUND", "va": 0x20000, "size": 0x100,
         "raw": 0x200, "raw_size": 0x100, "executable": True},
        # Xbox linkers can mark both of these executable. They remain data.
        {"name": ".rdata", "va": 0x30000, "size": 0x100,
         "raw": 0x300, "raw_size": 0x100, "executable": True},
        {"name": ".data", "va": 0x40000, "size": 0x100,
         "raw": 0x400, "raw_size": 0x100, "executable": True},
    ]


def _functions():
    return [
        {"start": "0x00010010", "end": "0x00010020", "size": 16,
         "section": ".text", "name": "text_known"},
        {"start": "0x00020000", "end": "0x00020010", "size": 16,
         "section": "DSOUND", "name": "sound_previous"},
        {"start": "0x00020030", "end": "0x00020040", "size": 16,
         "section": "DSOUND", "name": "sound_known"},
    ]


def test_code_ranges_match_disassembler_policy():
    ranges = _get_code_ranges_from_sections(_sections())
    assert (0x10000, 0x10100) in ranges
    assert (0x20000, 0x20100) in ranges
    assert (0x30000, 0x30100) not in ranges
    assert (0x40000, 0x40100) not in ranges


def test_non_text_entry_requires_nearby_boundary_and_padding():
    image = bytearray(0x500)
    image[0x210:0x214] = b"\x90\xCC\x90\x90"
    funcs = _functions()
    secs = _sections()

    assert _is_plausible_thunk_entry(0x20010, image, funcs, secs)
    assert _is_plausible_thunk_entry(0x20014, image, funcs, secs)
    assert not _is_plausible_thunk_entry(0x20008, image, funcs, secs)
    assert not _is_plausible_thunk_entry(0x20028, image, funcs, secs)

    image[0x211] = 0x41
    assert not _is_plausible_thunk_entry(0x20014, image, funcs, secs)


def test_scan_discovers_anchored_dsound_entry_only():
    image = bytearray(0x500)
    # Strong vtable shape: two known starts surrounding one missed method.
    struct.pack_into("<III", image, 0x300,
                     0x10010, 0x20010, 0x20030)
    # Same broad code-range evidence, but no nearby function boundary.
    struct.pack_into("<III", image, 0x320,
                     0x10010, 0x20070, 0x20030)

    results, _vtables = scan_vtables(
        image, _functions(), {}, sections=_sections())

    assert results[0x20010]["method"] == "vtable_thunk"
    assert results[0x20010]["section"] == "DSOUND"
    assert 0x20070 not in results


def test_widened_table_does_not_create_new_text_seed():
    image = bytearray(0x500)
    # The DSOUND pointer joins two .text values into a three-entry run only in
    # the widened scan. Neither .text value formed an upstream vtable, so the
    # new scan must not promote the unknown one as a side effect.
    struct.pack_into("<III", image, 0x300,
                     0x10055, 0x20010, 0x10010)

    results, _vtables = scan_vtables(
        image, _functions(), {}, sections=_sections())

    assert 0x10055 not in results
    assert results[0x20010]["method"] == "vtable_thunk"


if __name__ == "__main__":
    failures = 0
    for name, fn in sorted(globals().items()):
        if not name.startswith("test_"):
            continue
        try:
            fn()
            print(f"  ok   {name}")
        except AssertionError as exc:
            failures += 1
            print(f"  FAIL {name}: {exc}")
    print("vtable scanner: " + ("OK" if not failures else f"{failures} FAILED"))
    sys.exit(1 if failures else 0)
