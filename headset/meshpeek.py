#!/usr/bin/env python3
"""Print depths of the newest RoomView3D mesh, using its published grid layout."""
import math
import mmap
import statistics
import struct


def newest(queue):
    with open(f"/dev/shm/sfbq_xrservice_roomview3d_{queue}", "rb") as f:
        with mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
            _, data_size, header_size, count = struct.unpack_from("<IIII", m)
            block_size = (header_size + data_size + 63) & ~63
            if not 0 < count <= 64 or 1088 + count * block_size > len(m):
                raise ValueError(f"invalid {queue} queue")
            for _ in range(3):
                seqs = [struct.unpack_from("<Q", m, 40 + 16 * i)[0] for i in range(count)]
                index = max(range(count), key=seqs.__getitem__)
                if not seqs[index]:
                    raise ValueError(f"no published {queue} block")
                start = 1088 + index * block_size
                block = m[start:start + header_size + data_size]
                if struct.unpack_from("<Q", m, 40 + 16 * index)[0] == seqs[index]:
                    return block[:header_size], block[header_size:], index, seqs[index]
            raise ValueError(f"{queue} changed while reading; retry")


def property_u32(header, path):
    count, _, magic, _ = struct.unpack_from("<IIII", header)
    if magic != 0xACBDF123 or 16 + count * 16 > len(header):
        raise ValueError("invalid property table")
    prop = 2166136261
    for byte in path.encode():
        prop = ((prop ^ byte) * 16777619) & 0xFFFFFFFF
    prop = 0x10000000 | (prop & 0x0FFFFFFF)
    for i in range(count):
        key, _, size, offset = struct.unpack_from("<IIII", header, 16 + i * 16)
        if key == prop and size == 4 and offset + size <= len(header):
            return struct.unpack_from("<I", header, offset)[0]
    raise ValueError(f"missing {path}")


header, data, _, _ = newest("meshLayout")
count = property_u32(header, "/meshLayout/indexCount")
if count < 3 or count * 4 > len(data):
    raise ValueError("invalid mesh index count")
indices = struct.unpack_from(f"<{count}I", data)
width, vertices = indices[1], max(indices) + 1
if not 2 <= width <= 1024 or vertices % width or vertices // width < 2:
    raise ValueError("invalid mesh grid")
height = vertices // width
_, data, index, seq = newest("meshData")
v = struct.unpack_from(f"<{vertices * 3}f", data)
x, y, z = ([value for value in v[axis::3] if math.isfinite(value)] for axis in range(3))
if not x or not y or not z:
    raise ValueError("mesh contains no finite vertices")
print(f"{width}x{height}, block {index} seq {seq}; z (forward) m: min {min(z):.2f} "
      f"median {statistics.median(z):.2f} max {max(z):.2f}; "
      f"x {min(x):.2f}..{max(x):.2f} y {min(y):.2f}..{max(y):.2f}")
row = height // 2 * width
print("z along the middle row, every 15th vertex:",
      " ".join(f"{v[(row + c) * 3 + 2]:.2f}" for c in range(0, width, 15)))
