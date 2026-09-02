"""
CLI entry point for the disassembly tool.

Usage:
    py -3 -m tools.disasm <path_to_xbe> [options]

Examples:
    py -3 -m tools.disasm "Burnout 3 Takedown/default.xbe" --text-only --stats-only -v
    py -3 -m tools.disasm "Burnout 3 Takedown/default.xbe" --text-only
    py -3 -m tools.disasm "Burnout 3 Takedown/default.xbe" -o output/
"""

import argparse
import sys

from .disasm import Disassembler


def main():
    parser = argparse.ArgumentParser(
        prog="tools.disasm",
        description="Burnout 3 XBE Disassembly Tool - "
                    "Static analysis and function detection for Xbox executables",
    )

    parser.add_argument(
        "xbe_path",
        help="Path to the Xbox XBE executable file",
    )
    parser.add_argument(
        "-o", "--output",
        dest="output_dir",
        default=None,
        help="Output directory for JSON databases and ASM listings "
             "(default: tools/disasm/output/)",
    )
    parser.add_argument(
        "--analysis-json",
        default=None,
        help="Path to burnout3_analysis.json (auto-detected if not specified)",
    )
    parser.add_argument(
        "--text-only",
        action="store_true",
        help="Only disassemble the .text section (faster, ~2.73 MB)",
    )
    parser.add_argument(
        "--stats-only",
        action="store_true",
        help="Print statistics only, don't write output files",
    )
    parser.add_argument(
        "-v", "--verbose",
        action="store_true",
        help="Verbose output with progress information",
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="Force re-analysis even if cache is valid",
    )
    parser.add_argument(
        "--extra-sections",
        default=None,
        help="Comma-separated list of additional section names to treat as code "
             "(for sections marked non-executable in the XBE but containing code, "
             "e.g., --extra-sections XIPS,DOLBY)",
    )
    parser.add_argument(
        "--seed-functions",
        action="append",
        default=None,
        metavar="JSON",
        help="JSON file with additional function entry points to seed the detector. "
             "Format: array of objects with 'start' field (hex address string). "
             "Use vtable_thunk_seeds.json from func_id to feed back the vtable "
             "thunks its scanner discovered (not identified_functions.json itself: "
             "it also holds classifications of functions the detector already "
             "found, and seeding those is a no-op at best), "
             "or icall_targets.json from tools.recomp.icall_feedback to feed back "
             "measured indirect-branch targets. "
             "Repeatable: pass it once per file. Hand-maintained seed lists and "
             "machine-generated ones stay separate files rather than being merged "
             "into each other.",
    )

    args = parser.parse_args()

    try:
        extra = [s.strip() for s in args.extra_sections.split(",")] if args.extra_sections else []
        seed_funcs = []
        for _seed_path in (args.seed_functions or []):
            _got = _load_seed_functions(_seed_path)
            seed_funcs.extend(_got)
            if args.verbose:
                print(f"  Seed file {_seed_path}: {len(_got)} addresses")
        seed_funcs = sorted(set(seed_funcs))
        disassembler = Disassembler(
            xbe_path=args.xbe_path,
            analysis_json=args.analysis_json,
            output_dir=args.output_dir,
            text_only=args.text_only,
            stats_only=args.stats_only,
            verbose=args.verbose,
            force=args.force,
            extra_sections=extra,
            seed_functions=seed_funcs,
        )
        success = disassembler.run()
        sys.exit(0 if success else 1)

    except FileNotFoundError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
    except ValueError as e:
        print(f"Error: {e}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print("\nInterrupted.", file=sys.stderr)
        sys.exit(130)
    except Exception as e:
        print(f"Unexpected error: {e}", file=sys.stderr)
        import traceback
        traceback.print_exc()
        sys.exit(2)


def _load_seed_functions(path):
    """Load seed function addresses from a JSON file."""
    import json
    with open(path) as f:
        data = json.load(f)
    addrs = []
    for entry in data:
        if isinstance(entry, dict) and "start" in entry:
            addrs.append(int(entry["start"], 16))
        elif isinstance(entry, int):
            addrs.append(entry)
    return addrs


if __name__ == "__main__":
    main()
