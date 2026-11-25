#!/usr/bin/env python3
"""Feed the Frame's eye tracking network the wearer's left eye (camera ring 1, upside down) the right way up.

Patches a copy of SteamVR's eyetracking (SteamOS 0.5.10/0.5.11 builds, same code): CGazeEstimatorCdsp::Init negates fy in the left camera
matrix, which flips that eye's undistorted network input about the virtual optical centre, and GazeFromImages
negates the y of that eye's gaze direction and pupil centre, so the rest of the pipeline sees unflipped values.
"""

import argparse
import hashlib
from pathlib import Path
import struct

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("source", type=Path, help="/opt/steamvr/tools/eyetracking/bin/linuxarm64/eyetracking")
parser.add_argument("output", type=Path)
args = parser.parse_args()

# SteamOS 0.5.10 and 0.5.11 builds differ only in their build ID
source_shas = {
    "9c7986a5782c46f945aa1a80aa1102a4f1c79749631a170799726407164c0feb",
    "9fec7572eebe0aa9234e2b6a7adc963d3b9c68dfa8a380ad91bd2cf1d35978f0",
}
hook, cave, segment_end = 0xEABB8, 0xCF6BBC, 0xCF6C00


def branch(at, target):
    return struct.pack("<I", 0x14000000 | (((target - at) >> 2) & 0x3FFFFFF))


data = bytearray(args.source.read_bytes())
if hashlib.sha256(data).hexdigest() not in source_shas:
    parser.error("unsupported eyetracking build; nothing written")

patches = {
    # Init: str s0,[x4,x3] (cy - crop) -> into the cave
    hook: (bytes.fromhex("806823bc"), branch(hook, cave)),
    # Cave: that store, then fy = -fy at [x4, x3 - 4], then back
    cave: (
        bytes(24),
        bytes.fromhex("806823bc" "651000d1" "826865bc" "4240211e" "826825bc") + branch(cave + 20, hook + 4),
    ),
    # GazeFromImages, left eye: fmul s9,s9,s3 (direction y) -> fnmul
    0xEC5F8: (bytes.fromhex("2909231e"), bytes.fromhex("2989231e")),
    # fsub s2,s2,s3 (pupil v - cy) -> fsub s2,s3,s2
    0xEC68C: (bytes.fromhex("4238231e"), bytes.fromhex("6238221e")),
}
for offset, (original, patched) in patches.items():
    if data[offset : offset + len(original)] != original:
        parser.error(f"unexpected bytes at {offset:#x}; nothing written")
    data[offset : offset + len(patched)] = patched

# Map the cave: extend the first (executable) LOAD segment over the zero padding before the data segment
phoff, phnum = struct.unpack_from("<Q", data, 0x20)[0], struct.unpack_from("<H", data, 0x38)[0]
headers = [phoff + i * 56 for i in range(phnum)]
text = next(h for h in headers if struct.unpack_from("<II", data, h) == (1, 5))
if struct.unpack_from("<QQ", data, text + 0x20) != (0xCF6BBA, 0xCF6BBA):
    parser.error("unexpected executable segment; nothing written")
struct.pack_into("<QQ", data, text + 0x20, segment_end, segment_end)

args.output.write_bytes(data)
args.output.chmod(0o755)
print("Patched", args.output)
