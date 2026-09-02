"""Run only this diagnostic child for a bounded interval, preserving its logs."""
import argparse
import os
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("name")
parser.add_argument("--seconds", type=float, default=15)
args = parser.parse_args()
root = Path(__file__).resolve().parents[2]
build = root / "build-macos/jsrf-first-fault/build"
logs = root / "build-macos/jsrf-first-fault/render-investigation" / args.name
logs.mkdir(parents=True, exist_ok=False)
env = dict(os.environ, XBOX_LOG_LEVEL="0")
with (logs / "stdout.log").open("wb") as out, (logs / "stderr.log").open("wb") as err:
    child = subprocess.Popen([str(build / "jsrf_first_fault")], cwd=build,
                             env=env, stdout=out, stderr=err)
    print(f"child={child.pid} logs={logs}", flush=True)
    try:
        child.wait(timeout=args.seconds)
        print(f"exited={child.returncode}", flush=True)
    except subprocess.TimeoutExpired:
        child.terminate()
        try:
            child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait()
        print(f"bounded-stop={args.seconds}s", flush=True)
