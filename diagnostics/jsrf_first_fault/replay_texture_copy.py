"""Replay a captured draw without booting JSRF. No game data is committed.

--pattern replaces only the texture with a generated RGB565 test pattern and
checks the captured 1:1 geometry against an independent row-copy expectation.
The default compares the replay against the live completed-draw framebuffer.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("capture", type=Path)
parser.add_argument("--renderer", type=Path, default=Path(__file__).resolve().parents[2] /
                    "build-macos/jsrf-first-fault/build/jsrf_texture_copy_replay")
parser.add_argument("--pattern", action="store_true")
args = parser.parse_args()
d = json.loads(args.capture.read_text())
assert d["version"] == 1 and d["copy_supported"] and d["primitive"] == 5
m = [0] * 2048
for key, value in d["registers"].items():
    address = int(key, 16)
    assert 0 <= address < 8192 and address % 4 == 0
    m[address // 4] = value
reg = lambda address: m[address // 4]
ramin = args.capture.with_suffix(".ramin").read_bytes()

def check_dma(handle, offset, size, expected):
    start = ((d["ramht"] >> 4) & 31) << 12
    length = 4096 << ((d["ramht"] >> 16) & 3)
    for pos in range(start, start + length, 8):
        key, context = struct.unpack_from("<II", ramin, pos)
        if key == handle and context & 0x80000000 and not context & 0x1F020000:
            flags, limit, frame = struct.unpack_from("<III", ramin, (context & 65535) << 4)
            assert flags & 0xFFF == 0x3D and flags & 0x30000 == 0
            base = ((frame & 0xFFFFF000) | (flags >> 20)) & 0x7FFFFFF
            assert offset + size <= limit + 1 and base + offset == expected
            return
    raise AssertionError(f"DMA handle {handle} missing")

texture = bytearray(args.capture.with_suffix(".texture").read_bytes())
target = bytearray(args.capture.with_suffix(".before").read_bytes())
check_dma(reg(0x184 if reg(0x1B04) & 3 == 1 else 0x188), reg(0x1B00), len(texture), d["texture_address"])
check_dma(reg(0x194), reg(0x210), len(target), d["target_address"])
expected = args.capture.with_suffix(".after").read_bytes()
if args.pattern:
    width, height = reg(0x1B1C) >> 16, reg(0x1B1C) & 65535
    pitch, target_pitch = reg(0x1B10) >> 16, reg(0x20C) & 65535
    assert reg(0x200) == width << 16 and reg(0x204) == height << 16 and reg(0x208) & 15 == 3
    assert len(d["vertices"]) == 3
    points = [v[0][:2] for v in d["vertices"]]
    assert points == [[0, 0], [2560, 0], [0, 1920]] and width <= 640 and height <= 480
    for vertex in d["vertices"]:
        assert vertex[0][3] == vertex[9][3] == 1 and vertex[0][:2] == vertex[9][:2]
    for y in range(height):
        for x in range(width):
            value = ((x // 8) % 32) << 11 | ((y // 4) % 64) << 5 | ((x + y) % 32)
            struct.pack_into("<H", texture, y * pitch + x * 2, value)
    target[:] = b"\xA5" * len(target)
    expected = bytearray(target)
    for y in range(height):
        expected[y * target_pitch:y * target_pitch + width * 2] = texture[y * pitch:y * pitch + width * 2]

suffix = ".pattern-replayed" if args.pattern else ".replayed"
output = args.capture.with_suffix(suffix)
with tempfile.TemporaryDirectory(prefix="jsrf-copy-replay-") as tmp:
    packed = Path(tmp) / "draw.bin"
    with packed.open("wb") as f:
        f.write(struct.pack("<6I", 0x4354584E, 1, 5, len(d["vertices"]), len(texture), len(target)))
        f.write(struct.pack("<2048I", *m))
        for vertex in d["vertices"]:
            assert len(vertex) == 16 and all(len(v) == 4 for v in vertex)
            f.write(struct.pack("<64f", *(0 if value is None else value for vec in vertex for value in vec)))
        f.write(texture)
        f.write(target)
    subprocess.run([str(args.renderer), str(packed), str(output)], check=True)
actual = output.read_bytes()
assert actual == expected, f"replay differs in {sum(a != b for a, b in zip(actual, expected))} bytes"
print(f"{args.capture.name}: {'pattern expectation' if args.pattern else 'live framebuffer'} matches "
      f"all {len(actual)} bytes; sha256={hashlib.sha256(actual).hexdigest()}")
