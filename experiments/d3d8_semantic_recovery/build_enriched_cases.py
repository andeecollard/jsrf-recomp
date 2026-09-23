#!/usr/bin/env python3
"""Add bounded x86 evidence to real JSRF D3D8 function cases."""

import argparse
import json
import sys
from pathlib import Path

from capstone import CS_ARCH_X86, CS_MODE_32, Cs

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))
from tools.func_id.d3d8_identifier import _file_offset, _parse_sections  # noqa: E402


def enrich(cases, functions, xbe):
    by_start = {f["start"].lower(): f for f in functions}
    sections = _parse_sections(xbe)
    decoder = Cs(CS_ARCH_X86, CS_MODE_32)
    for case in cases:
        function = by_start[case["address"].lower()]
        address = int(function["start"], 16)
        size = function["size"]
        offset = _file_offset(sections, address, size, len(xbe))
        if offset is None:
            raise ValueError("function is not file-backed: " + function["start"])
        instructions = list(decoder.disasm(xbe[offset:offset + size], address))
        if not instructions:
            raise ValueError("function did not disassemble: " + function["start"])
        evidence = {k: v for k, v in case.items() if k != "expected"}
        evidence["facts"] = {
            **case["facts"],
            "called_by_count": len(function.get("called_by", [])),
            "direct_call_count": len(function.get("calls_to", [])),
            "disassembly_complete": len(instructions) <= 64,
        }
        evidence["disassembly"] = [
            f"{ins.address:08X}: {ins.mnemonic} {ins.op_str}".strip()
            for ins in instructions[:64]
        ]
        evidence["direct_call_targets"] = function.get("calls_to", [])[:8]
        yield {**evidence, "expected": case["expected"]}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--cases", type=Path, required=True)
    parser.add_argument("--functions", type=Path, required=True)
    parser.add_argument("--xbe", type=Path, required=True)
    parser.add_argument("--output", "-o", type=Path, required=True)
    args = parser.parse_args()
    cases = [json.loads(line) for line in args.cases.read_text().splitlines() if line.strip()]
    functions = json.loads(args.functions.read_text())
    xbe = args.xbe.read_bytes()
    with args.output.open("x", encoding="utf-8") as handle:
        for case in enrich(cases, functions, xbe):
            handle.write(json.dumps(case, separators=(",", ":")) + "\n")


if __name__ == "__main__":
    main()
