#!/usr/bin/env python3
"""Where atomic commits land relative to the previous panel flip. Just after the flip means frames wait in the
FIFO queue (one refresh of extra latency); late in the period means they're presented just in time."""
import bisect, re, statistics, sys

text = open(sys.argv[1]).read()
flips = sorted(float(m.group(1)) for m in re.finditer(r"\s(\d+\.\d+): dpu_crtc_complete_flip", text))
commits = sorted(float(m.group(1)) for m in re.finditer(r"\s(\d+\.\d+): msm_atomic_commit_tail_start", text))
if len(flips) < 2:
    sys.exit("need at least two panel flips")
after = []
for c in commits:
    i = bisect.bisect_right(flips, c) - 1
    if 0 <= i < len(flips) - 1:
        after.append((c - flips[i]) * 1000)
period = statistics.median(b - a for a, b in zip(flips, flips[1:])) * 1000
after.sort()
if not after:
    sys.exit("no commits overlap the flip trace")
print(f"period {period:.2f} ms, {len(after)} commits, after previous flip: median {statistics.median(after):.2f} ms, "
      f"p10 {after[len(after) // 10]:.2f}, p90 {after[len(after) * 9 // 10]:.2f}")
