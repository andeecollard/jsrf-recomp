#!/usr/bin/env python3
"""Capture healthy and stalled JSRF pad-poll samples from one process.

The in-guest function-entry probes are heavy enough to precipitate the input
failure they are meant to observe.  macOS's ``sample`` profiler has no per-call
cost, so this tool launches the uninstrumented diagnostic, watches its existing
five-second reports, and profiles the same process in two states:

* healthy: the pad-poll counter is advancing and the game is not paused;
* stalled: the counter is unchanged for several reports and the game is not
  in CoveredPause mode.

Raw profiles, logs, extracted guest-frame sets, and both set differences are
kept together under ``build-macos/jsrf-first-fault/render-investigation``.
"""

from __future__ import annotations

import argparse
from collections import Counter
import os
from pathlib import Path
import re
import subprocess
import sys
import time


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_BUILD = ROOT / "build-macos/jsrf-first-fault/build"
DEFAULT_OUTPUT_ROOT = (
    ROOT / "build-macos/jsrf-first-fault/render-investigation"
)

POLL_RE = re.compile(r"\[PAD-POLL\]\s+polls=(\d+)")
PAUSE_RE = re.compile(
    r"\[JSRF-PAUSE\]\s+\+3C=([0-9A-Fa-f]+)\s+\+40=([0-9A-Fa-f]+)"
)
GUEST_FRAME_RE = re.compile(r"\bsub_([0-9A-Fa-f]{8})\b")
SAMPLE_COUNT_RE = re.compile(r"^\s*(\d+)\s+")


def parse_reports(text: str) -> list[tuple[int, bool]]:
    """Return complete (poll count, covered-pause) report pairs."""
    reports: list[tuple[int, bool]] = []
    pending_poll: int | None = None
    pending_pause: bool | None = None
    for line in text.splitlines():
        poll = POLL_RE.search(line)
        if poll:
            pending_poll = int(poll.group(1))
        pause = PAUSE_RE.search(line)
        if pause:
            pending_pause = int(pause.group(1), 16) != 0 and int(
                pause.group(2), 16
            ) != 0
        if pending_poll is not None and pending_pause is not None:
            reports.append((pending_poll, pending_pause))
            pending_poll = None
            pending_pause = None
    return reports


def guest_frames(sample_text: str) -> Counter[str]:
    """Extract guest function names and approximate sample weights."""
    frames: Counter[str] = Counter()
    for line in sample_text.splitlines():
        match = GUEST_FRAME_RE.search(line)
        if not match:
            continue
        count = SAMPLE_COUNT_RE.match(line)
        frames[f"sub_{match.group(1).upper()}"] += (
            int(count.group(1)) if count else 1
        )
    return frames


def write_frames(path: Path, frames: Counter[str]) -> None:
    path.write_text(
        "".join(f"{name} {count}\n" for name, count in frames.most_common())
    )


def write_difference(
    path: Path, left: Counter[str], right: Counter[str], heading: str
) -> None:
    only = sorted(set(left) - set(right))
    path.write_text(
        heading + "\n" + "".join(f"{name} {left[name]}\n" for name in only)
    )


def take_sample(pid: int, seconds: int, path: Path) -> None:
    print(f"sampling pid={pid} for {seconds}s -> {path}", flush=True)
    result = subprocess.run(
        ["/usr/bin/sample", str(pid), str(seconds), "-file", str(path)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"sample failed with exit {result.returncode}: {result.stdout.strip()}"
        )


def terminate(child: subprocess.Popen[bytes]) -> None:
    if child.poll() is not None:
        return
    child.terminate()
    try:
        child.wait(timeout=5)
    except subprocess.TimeoutExpired:
        child.kill()
        child.wait()


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("name", help="new output-directory name")
    parser.add_argument("--seconds", type=int, default=5,
                        help="duration of each profile (default: %(default)s)")
    parser.add_argument("--timeout", type=int, default=300,
                        help="maximum run time (default: %(default)s)")
    parser.add_argument("--stable-reports", type=int, default=3,
                        help="identical unpaused reports required for a stall")
    parser.add_argument("--min-healthy-polls", type=int, default=100,
                        help="do not capture healthy state before this count")
    parser.add_argument("--build", type=Path, default=DEFAULT_BUILD)
    parser.add_argument("--output-root", type=Path, default=DEFAULT_OUTPUT_ROOT)
    parser.add_argument("--fake-pad", default="1",
                        help="RECOMP_FAKE_PAD value (default: %(default)s)")
    args = parser.parse_args()

    executable = args.build.resolve() / "jsrf_first_fault"
    if not executable.is_file():
        parser.error(f"missing executable: {executable}")
    if not Path("/usr/bin/sample").is_file():
        parser.error("this tool requires macOS /usr/bin/sample")
    if args.seconds < 1 or args.timeout < 1 or args.stable_reports < 2:
        parser.error("durations must be positive and --stable-reports at least 2")

    output = args.output_root.resolve() / args.name
    output.mkdir(parents=True, exist_ok=False)
    stdout_path = output / "stdout.log"
    stderr_path = output / "stderr.log"
    healthy_path = output / "healthy.sample.txt"
    stalled_path = output / "stalled.sample.txt"

    env = dict(os.environ)
    env["XBOX_LOG_LEVEL"] = "0"
    env["RECOMP_FAKE_PAD"] = args.fake_pad
    # This diagnostic's generated tree is validated with the contiguous guest
    # reserve path.  The experimental separate-reserve path has its own known
    # 0xFFFFFFxx guest-address fault and would terminate before the state pair
    # this tool is meant to observe.
    env["RECOMP_SEPARATE_RESERVE"] = "0"
    # The pause fields are part of the bounded scene report.  A flat poll
    # counter without this companion observation is intentionally not enough
    # to classify the run as the genuine input bug.
    env["RECOMP_SCENE_REPORT"] = "1"
    # Function-hit probes may still be compiled into a generated tree.  Their
    # getenv gate must stay off for this zero-per-call-cost observation.
    env.pop("RECOMP_FUNC_HIT_TRACE", None)

    reports_seen = 0
    last_poll: int | None = None
    unchanged = 0
    healthy_captured = False
    deadline = time.monotonic() + args.timeout

    with stdout_path.open("wb") as stdout, stderr_path.open("wb") as stderr:
        child = subprocess.Popen(
            [str(executable)], cwd=args.build.resolve(), env=env,
            stdout=stdout, stderr=stderr,
        )
        print(f"child={child.pid} logs={output}", flush=True)
        try:
            while time.monotonic() < deadline:
                if child.poll() is not None:
                    raise RuntimeError(f"child exited before both samples: {child.returncode}")

                text = stderr_path.read_text(errors="replace")
                reports = parse_reports(text)
                for polls, paused in reports[reports_seen:]:
                    reports_seen += 1
                    print(
                        f"report={reports_seen} polls={polls} paused={int(paused)}",
                        flush=True,
                    )
                    # The first report has already advanced from the process's
                    # known initial value of zero.  Capturing it also avoids
                    # losing the healthy half when an unrelated later guest
                    # fault terminates a run before its second report.
                    advancing = last_poll is None or polls > last_poll
                    if (not healthy_captured and not paused
                            and polls >= args.min_healthy_polls and advancing):
                        take_sample(child.pid, args.seconds, healthy_path)
                        healthy_captured = True

                    if paused or last_poll is None or polls != last_poll:
                        unchanged = 0
                    else:
                        unchanged += 1

                    if (healthy_captured and not paused
                            and unchanged >= args.stable_reports - 1):
                        take_sample(child.pid, args.seconds, stalled_path)
                        healthy = guest_frames(healthy_path.read_text(errors="replace"))
                        stalled = guest_frames(stalled_path.read_text(errors="replace"))
                        write_frames(output / "healthy_guest_frames.txt", healthy)
                        write_frames(output / "stalled_guest_frames.txt", stalled)
                        write_difference(
                            output / "healthy_only_guest_frames.txt", healthy, stalled,
                            "# Present in the healthy sample and absent when stalled",
                        )
                        write_difference(
                            output / "stalled_only_guest_frames.txt", stalled, healthy,
                            "# Present in the stalled sample and absent when healthy",
                        )
                        print(
                            f"captured pair: healthy={len(healthy)} guest frames, "
                            f"stalled={len(stalled)}; results={output}",
                            flush=True,
                        )
                        return 0
                    last_poll = polls
                time.sleep(0.5)
            raise RuntimeError(f"timed out after {args.timeout}s before both samples")
        except (RuntimeError, KeyboardInterrupt) as exc:
            print(str(exc), file=sys.stderr, flush=True)
            return 1
        finally:
            terminate(child)


if __name__ == "__main__":
    raise SystemExit(main())
