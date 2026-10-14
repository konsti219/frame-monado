#!/usr/bin/env python3
"""Compares Monado's desired present times (fake pacer trace log) with real panel flips (ftrace, mono clock).
Usage: latency.py <monado log> <ftrace text>"""
import bisect, re, statistics, sys

desired = [int(m.group(1)) for m in re.finditer(r"desired_present (\d+)", open(sys.argv[1]).read())]
flips = sorted(int(float(m.group(1)) * 1e9) for m in re.finditer(r"\s(\d+\.\d+): dpu_crtc_complete_flip", open(sys.argv[2]).read()))
if not flips:
    sys.exit("no flips")
lat = []
for d in desired:
    if not flips[0] - 50e6 < d < flips[-1]:
        continue
    i = bisect.bisect_left(flips, d - 2_000_000)
    if i < len(flips):
        lat.append((flips[i] - d) / 1e6)
lat.sort()
if not lat:
    sys.exit("no desired presents overlap the flip trace")
print(f"{len(lat)} frames, flip after desired present: median {statistics.median(lat):.2f} ms, "
      f"p10 {lat[len(lat) // 10]:.2f}, p90 {lat[len(lat) * 9 // 10]:.2f}")
