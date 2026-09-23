#!/usr/bin/env python3
"""Build blinded XbSymbolDatabase-positive and negative-control cases.

Write output outside the repository: it contains retail XBE disassembly.
Signature addresses can be inside instructions; each is mapped to the
containing disassembler function before decoding from that function's entry.
"""

import argparse
import bisect
import json
import re
import sys
from pathlib import Path

from capstone import Cs, CS_ARCH_X86, CS_MODE_32

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
from tools.func_id.d3d8_identifier import _file_offset, _parse_sections

LABELS = {
    "D3DDevice_Clear": "clear",
    "D3DDevice_DrawIndexedVertices": "draw_indexed",
    "D3DDevice_DrawIndexedVerticesUP": "draw_indexed",
    "D3DDevice_DrawVertices": "draw_vertices",
    "D3DDevice_DrawVerticesUP": "draw_up",
    "D3DDevice_SetLight": "set_light",
    "D3DDevice_SetRenderTarget": "set_render_target",
    "D3DDevice_SetTexture": "set_texture",
    "D3DDevice_SetVertexShaderConstant": "set_vertex_shader_constant",
    "D3DDevice_Swap": "swap",
    "D3DDevice_AddRef": "insufficient_evidence",
    "D3DDevice_GetBackBuffer": "insufficient_evidence",
    "D3DDevice_GetDepthStencilSurface": "insufficient_evidence",
    "D3DDevice_GetDeviceCaps": "insufficient_evidence",
    "D3DDevice_GetGammaRamp": "insufficient_evidence",
    "D3DDevice_GetTransform": "insufficient_evidence",
    "D3DDevice_IsBusy": "insufficient_evidence",
}


def build(symbol_text, functions, identified, xbe):
    functions = sorted(functions, key=lambda f: int(f["start"], 16))
    starts = [int(f["start"], 16) for f in functions]
    identified = {f["start"].lower(): f for f in identified}
    sections = _parse_sections(xbe)
    decoder = Cs(CS_ARCH_X86, CS_MODE_32)
    for line in symbol_text.splitlines():
        match = re.fullmatch(r"D3D8__(\w+) = 0x([0-9A-Fa-f]+)", line.strip())
        if not match or match.group(1) not in LABELS:
            continue
        name, address = match.group(1), int(match.group(2), 16)
        index = bisect.bisect_right(starts, address) - 1
        if index < 0:
            continue
        function = functions[index]
        start, size = starts[index], function["size"]
        if not start <= address < start + size:
            continue
        offset = _file_offset(sections, start, size, len(xbe))
        if offset is None:
            continue
        instructions = list(decoder.disasm(xbe[offset:offset + size], start))[:96]
        prior = identified.get(function["start"].lower(), {})
        yield {
            "id": f"symbol_{address:08x}",
            "address": function["start"],
            "methods": prior.get("nv2a_methods", []),
            "facts": {"function_size_bytes": size,
                      "symbol_inside_function": address != start},
            "disassembly": [
                f"{ins.address:08X}: {ins.mnemonic} {ins.op_str}".strip()
                for ins in instructions
            ],
            "expected": LABELS[name],
            "label_source": "XbSymbolDatabase XDK 4134 CLI, mapped to containing function",
        }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--symbols", type=Path, required=True)
    parser.add_argument("--functions", type=Path, required=True)
    parser.add_argument("--identified", type=Path, required=True)
    parser.add_argument("--xbe", type=Path, required=True)
    parser.add_argument("--output", "-o", type=Path, required=True)
    args = parser.parse_args()
    rows = list(build(args.symbols.read_text(),
                      json.loads(args.functions.read_text()),
                      json.loads(args.identified.read_text()),
                      args.xbe.read_bytes()))
    with args.output.open("x", encoding="utf-8") as handle:
        for row in rows:
            handle.write(json.dumps(row, separators=(",", ":")) + "\n")
    print(f"wrote {len(rows)} cases to {args.output}")


if __name__ == "__main__":
    main()
