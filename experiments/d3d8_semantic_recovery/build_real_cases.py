#!/usr/bin/env python3
"""Build a blinded fixture from tools.func_id identified_functions.json."""

import argparse
import json
from pathlib import Path

NAME_TO_CHOICE = {
    "D3DDevice_Swap": "swap", "D3DDevice_DrawIndexedVertices": "draw_indexed",
    "D3DDevice_DrawVerticesUP": "draw_up", "D3DDevice_DrawVertices": "draw_vertices",
    "D3DDevice_Clear": "clear", "D3DDevice_SetTexture": "set_texture",
    "D3DDevice_SetTextureState": "set_texture_state",
    "D3DDevice_SetRenderTarget": "set_render_target",
    "D3DDevice_SetVertexShaderConstant": "set_vertex_shader_constant",
    "D3DDevice_SetFogState": "set_fog_state",
    "D3DDevice_SetPixelShaderState": "set_pixel_shader_state",
    "D3DDevice_SetLight": "set_light", "D3DDevice_BlockOnFence": "block_on_fence",
    "D3DDevice_StateBlock": "state_block",
}

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("identified_functions", type=Path)
    parser.add_argument("--output", "-o", type=Path, required=True)
    args = parser.parse_args()
    functions = json.loads(args.identified_functions.read_text(encoding="utf-8"))
    cases = []
    for function in functions:
        if function.get("method") != "d3d8_nv2a_method":
            continue
        expected = NAME_TO_CHOICE.get(function.get("identified_name"))
        if expected is None:
            continue
        methods = function.get("nv2a_methods", [])
        cases.append({
            "id": "real_" + function["start"].lower().replace("0x", ""),
            "address": function["start"], "methods": methods,
            "facts": {"section": function.get("section"),
                      "function_size_bytes": function.get("size"),
                      "distinct_method_count": len(methods),
                      "methods_are_exhaustive": True},
            "expected": expected,
            "label_source": "tools.func_id deterministic NV2A classifier",
        })
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8") as handle:
        for case in cases:
            handle.write(json.dumps(case, separators=(",", ":")) + "\n")
    print(f"wrote {len(cases)} real-function cases to {args.output}")

if __name__ == "__main__":
    main()
