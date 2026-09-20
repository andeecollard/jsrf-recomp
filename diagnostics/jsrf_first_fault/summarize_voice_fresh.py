#!/usr/bin/env python3
"""Separate unchanged audio from unobserved audio in an existing run log.

Freshness hashes decoded content; it does not intercept guest stores. Replayed
static effects, silence, handle reuse and inactive voices cannot establish a
streaming producer failure. Report-window ordinals below are not timestamps.
"""
import argparse
import collections
import re


def summarize(lines):
    counts = collections.Counter()
    voices = collections.defaultdict(collections.Counter)
    current = None
    tops = set()
    for line in lines:
        m = re.search(r"\[VOICE-RATE\]\s+voice\s+(\d+):", line)
        if m:
            current = int(m[1])
        m = re.search(r"\[VOICE-FRESH-WIN\].*? (\d+) fresh, (\d+) stale", line)
        if m and current is not None:
            fresh, stale = map(int, m.groups())
            kind = ("no_comparable_samples" if fresh + stale == 0 else
                    "unchanged_only" if fresh == 0 else "changed_content")
            counts[kind] += 1
            voices[current][kind] += 1
        m = re.search(r"\[VOICE-TOP-RING\]\s+([\d.]+) ms (2D|3D|MP) "
                      r"([0-9A-F]+) -> ([0-9A-F]+).*?\[([A-Z]+)\]", line)
        if m and "A" in m[5] and "U" in m[5]:
            tops.add((float(m[1]), m[2], int(m[3], 16), int(m[4], 16), m[5]))
    return counts, voices, sorted(tops)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log")
    args = parser.parse_args()
    with open(args.log, errors="replace") as stream:
        counts, voices, tops = summarize(stream)
    print("Per-voice report rows (not elapsed-time windows):")
    for key in ("no_comparable_samples", "unchanged_only", "changed_content"):
        print(f"  {key}: {counts[key]}")
    print("voice  no-comparison  unchanged-only  changed-content")
    for voice, row in sorted(voices.items()):
        print(f"{voice:5d} {row['no_comparable_samples']:14d}"
              f" {row['unchanged_only']:15d} {row['changed_content']:16d}")
    print("Guest unlinks of heads still marked active (deduplicated ring entries):")
    for ms, group, old, new, flags in tops:
        print(f"  audio={ms / 1000:.6f}s {group}: v{old} -> v{new} [{flags}]")
    print("No comparable samples is not evidence of a stalled writer. Unchanged"
          " content alone is not evidence of a missing write. Active unlinks"
          " identify a boundary to trace, not whether removal was erroneous.")


if __name__ == "__main__":
    main()
