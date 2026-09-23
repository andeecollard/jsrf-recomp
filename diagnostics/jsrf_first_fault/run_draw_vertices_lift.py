#!/usr/bin/env python3
"""Run sequential smoke arms with separate copied HDDs and an opt-in lift.

Requires local macOS GPU access. No launch config, installed app or original
HDD is changed. Timing is diagnostic only; verify scene agreement separately.
"""
import argparse
import os
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("game", type=Path)
    parser.add_argument("hdd", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--seconds", type=int, default=60)
    args = parser.parse_args()
    if not (args.game / "default.xbe").is_file():
        parser.error("game/default.xbe is required")
    args.output.mkdir(parents=True, exist_ok=False)
    # Explicit smoke preset derived from the local working configuration.
    # Do not inherit trace/recording paths that write into the user's session.
    env = {k: v for k, v in os.environ.items() if not k.startswith("RECOMP_")}
    for flag in ("PB_EXEC", "METAL", "OHCI_ATTACH", "NV2A_PMC_UPMIRROR",
                 "APU_SELFLINK_END", "APU_LIST_MOVE_TO_FRONT", "VSH_DP_ZERO",
                 "APU_IDLE_TRAP_EDGE", "METAL_FF", "APU_FEDEC_HOLD",
                 "APU_CYCLE_BREAK", "METAL_NO_DEPTH_SYNC", "KERNEL_THREADS",
                 "APU_TRAP_THREADS", "APU_IDLE_HANDOFF_GUARD",
                 "APU_ADPCM_HW_HEADER", "ADX_SERIALIZE", "TEXMODE_APPROX",
                 "SCENE_REPORT", "PAD_INJECT"):
        env["RECOMP_" + flag] = "1"
    env.update(RECOMP_XBE_PATH=str(args.game.resolve() / "default.xbe"),
               RECOMP_GAME_DIR=str(args.game.resolve()), RECOMP_REPORT_MS="10000",
               RECOMP_PAD_SCRIPT="@" + str(Path(__file__).resolve().parent / "pad/measure.pad"))
    for arm in (0, 1):
        directory = args.output.resolve() / f"arm{arm}"
        directory.mkdir()
        subprocess.run(["cp", "-cR", str(args.hdd.resolve()), str(directory / "hdd")], check=True)
        env.update(RECOMP_HDD_ROOT=str(directory / "hdd"), RECOMP_JSRF_DRAW_LIFT=str(arm))
        with (directory / "run.log").open("w") as log:
            process = subprocess.Popen([str(args.binary.resolve())], cwd=directory,
                                       env=env, stdout=log, stderr=subprocess.STDOUT)
            print(f"arm={arm} pid={process.pid} log={directory / 'run.log'}", flush=True)
            try:
                process.wait(timeout=args.seconds)
            except subprocess.TimeoutExpired:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        print(f"arm={arm} exit={process.returncode}", flush=True)


if __name__ == "__main__":
    main()
