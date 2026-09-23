#!/usr/bin/env python3
"""Score a local LM Studio model on bounded D3D8 semantic recovery cases."""

from __future__ import annotations

import argparse
import json
import time
import urllib.error
import urllib.request
from collections import Counter, defaultdict
from pathlib import Path


HERE = Path(__file__).resolve().parent
OPTIONS = [
    "swap",
    "draw_indexed",
    "draw_up",
    "draw_vertices",
    "clear",
    "set_texture",
    "set_texture_state",
    "set_render_target",
    "set_vertex_shader_constant",
    "set_fog_state",
    "set_pixel_shader_state",
    "set_light",
    "block_on_fence",
    "state_block",
    "insufficient_evidence",
]

SYSTEM = """You classify statically recovered Xbox D3D8/NV2A function evidence.
Choose exactly one supplied operation family. Use insufficient_evidence when
the methods do not uniquely establish an operation. A broad function that
programs many unrelated pipeline areas is state_block unless it contains a
decisive operation marker such as a draw bracket, clear, or complete flip.
Return JSON only, with keys choice and confidence. confidence is a number from
0 to 1. Do not infer facts absent from the evidence."""


def request_json(url: str, payload=None, timeout=120):
    data = None if payload is None else json.dumps(payload).encode("utf-8")
    req = urllib.request.Request(url, data=data)
    req.add_header("Content-Type", "application/json")
    with urllib.request.urlopen(req, timeout=timeout) as response:
        return json.load(response)


def discover_model(base_url: str) -> str:
    result = request_json(base_url.rstrip("/") + "/models", timeout=5)
    models = result.get("data", [])
    if not models:
        raise RuntimeError("LM Studio returned no loaded models")
    return models[0]["id"]


def load_cases(path: Path):
    with path.open(encoding="utf-8") as handle:
        return [json.loads(line) for line in handle if line.strip()]


def extract_json(text: str):
    text = text.strip()
    if text.startswith("```"):
        lines = text.splitlines()
        text = "\n".join(lines[1:-1])
        if text.lstrip().startswith("json"):
            text = text.lstrip()[4:].lstrip()
    try:
        return json.loads(text)
    except json.JSONDecodeError:
        start, end = text.find("{"), text.rfind("}")
        if start >= 0 and end > start:
            return json.loads(text[start : end + 1])
        raise


def classify(base_url: str, model: str, case: dict, timeout: int):
    evidence = {k: v for k, v in case.items()
                if k not in ("id", "expected", "label_source")}
    prompt = {
        "evidence": evidence,
        "allowed_choices": OPTIONS,
        "instruction": "Choose the single best supported operation family.",
    }
    payload = {
        "model": model,
        "temperature": 0,
        "max_tokens": 512,
        "reasoning_effort": "none",
        "response_format": {
            "type": "json_schema",
            "json_schema": {
                "name": "d3d8_semantic_choice",
                "strict": True,
                "schema": {
                    "type": "object",
                    "properties": {
                        "choice": {"type": "string", "enum": OPTIONS},
                        "confidence": {"type": "number", "minimum": 0, "maximum": 1},
                    },
                    "required": ["choice", "confidence"],
                    "additionalProperties": False,
                },
            },
        },
        "messages": [
            {"role": "system", "content": SYSTEM},
            {"role": "user", "content": json.dumps(prompt, separators=(",", ":"))},
        ],
    }
    started = time.perf_counter()
    result = request_json(base_url.rstrip("/") + "/chat/completions", payload, timeout)
    elapsed = time.perf_counter() - started
    completion = result["choices"][0]
    message = completion["message"]
    content = message.get("content") or ""
    if not content.strip():
        # LM Studio currently places structured output from some Qwen MLX
        # models in reasoning_content even when reasoning_effort is "none".
        # Accept it as the response, but retain the raw value in the artifact.
        content = message.get("reasoning_content") or message.get("reasoning") or ""
    if not content.strip():
        raise ValueError("empty content; finish_reason=%r" % completion.get("finish_reason"))
    parsed = extract_json(content)
    choice = parsed.get("choice")
    if choice not in OPTIONS:
        raise ValueError(f"invalid choice {choice!r}")
    confidence = float(parsed.get("confidence", 0.0))
    return choice, confidence, content, elapsed


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--base-url", default="http://127.0.0.1:1234/v1")
    parser.add_argument("--model", help="default: first model returned by /v1/models")
    parser.add_argument("--cases", type=Path, default=HERE / "cases.jsonl")
    parser.add_argument("--output", type=Path, default=HERE / "results.json")
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()

    try:
        model = args.model or discover_model(args.base_url)
    except (OSError, urllib.error.URLError, RuntimeError) as exc:
        parser.error(
            f"cannot reach LM Studio at {args.base_url}: {exc}. "
            "Start its local server and load the Qwen model."
        )

    cases = load_cases(args.cases)
    rows = []
    choices_by_case = defaultdict(list)
    print(f"model={model} cases={len(cases)} repeat={args.repeat}")
    for attempt in range(args.repeat):
        for case in cases:
            row = {"id": case["id"], "expected": case["expected"], "attempt": attempt + 1}
            try:
                choice, confidence, raw, elapsed = classify(
                    args.base_url, model, case, args.timeout
                )
                row.update(
                    choice=choice,
                    confidence=confidence,
                    correct=choice == case["expected"],
                    latency_seconds=round(elapsed, 4),
                    raw=raw,
                )
                choices_by_case[case["id"]].append(choice)
                mark = "PASS" if row["correct"] else "FAIL"
                print(f"{mark:4} {case['id']:<27} {choice:<28} {elapsed:6.2f}s")
            except Exception as exc:  # preserve every failed attempt in results
                row.update(correct=False, error=f"{type(exc).__name__}: {exc}")
                print(f"ERR  {case['id']:<27} {row['error']}")
            rows.append(row)

    correct = sum(bool(row.get("correct")) for row in rows)
    stable = ({
        case_id: len(set(choices)) == 1
        for case_id, choices in choices_by_case.items()
        if len(choices) == args.repeat
    } if args.repeat > 1 else {})
    summary = {
        "model": model,
        "base_url": args.base_url,
        "cases": len(cases),
        "attempts": len(rows),
        "correct": correct,
        "accuracy": correct / len(rows) if rows else 0.0,
        "stable_cases": sum(stable.values()) if args.repeat > 1 else None,
        "fully_observed_cases": len(stable) if args.repeat > 1 else None,
        "stability_measured": args.repeat > 1,
        "choice_counts": Counter(row.get("choice", "ERROR") for row in rows),
    }
    artifact = {"summary": summary, "rows": rows}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(artifact, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2))
    print(f"wrote {args.output}")
    raise SystemExit(0 if correct == len(rows) else 1)


if __name__ == "__main__":
    main()
