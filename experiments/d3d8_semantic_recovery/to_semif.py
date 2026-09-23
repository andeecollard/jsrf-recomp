#!/usr/bin/env python3
"""Convert existing blinded D3D8 cases to SemIf's decision JSONL format."""

import argparse
import json
from pathlib import Path


DESCRIPTIONS = {
    "swap": "Present or flip the display surface.",
    "draw_indexed": "Draw geometry using an index or array-element stream.",
    "draw_up": "Draw geometry from an inline vertex array.",
    "draw_vertices": "Draw non-indexed geometry without an inline array.",
    "clear": "Clear a color, depth, or stencil surface.",
    "set_texture": "Bind or configure a texture's storage address and format.",
    "set_texture_state": "Configure texture sampling, filtering, or control state.",
    "set_render_target": "Bind or configure color or depth render surfaces.",
    "set_vertex_shader_constant": "Upload transform or vertex shader constants.",
    "set_fog_state": "Configure fog state.",
    "set_pixel_shader_state": "Configure pixel combiners or pixel shader state.",
    "set_light": "Configure a light.",
    "block_on_fence": "Issue or wait for a GPU semaphore or fence.",
    "state_block": "Configure several unrelated pipeline areas in one broad block.",
    "insufficient_evidence": "The evidence does not uniquely establish an operation.",
}


def convert(cases):
    options = [{"id": key, "description": value} for key, value in DESCRIPTIONS.items()]
    for case in cases:
        evidence = {key: value for key, value in case.items()
                    if key not in ("id", "expected", "label_source")}
        yield {
            "id": case["id"],
            "state": evidence,
            "question": "Which Xbox D3D8 operation is best supported by this recovered NV2A evidence?",
            "options": options,
        }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("cases", type=Path)
    parser.add_argument("--output", "-o", type=Path, required=True)
    args = parser.parse_args()
    cases = [json.loads(line) for line in args.cases.read_text().splitlines() if line.strip()]
    with args.output.open("x", encoding="utf-8") as handle:
        for row in convert(cases):
            handle.write(json.dumps(row, separators=(",", ":")) + "\n")


if __name__ == "__main__":
    main()
