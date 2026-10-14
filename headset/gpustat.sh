#!/bin/bash
# Captures 3 s of flips, commits and GPU jobs into <trace file>, then prints GPU stats.
set -euo pipefail
if [ "$#" -ne 1 ]; then echo "usage: $0 <trace file>" >&2; exit 1; fi
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
pid=$(pgrep -x monado-service | head -1)
tids=$(for task in /proc/"$pid"/task/*; do printf '%s\n' "${task##*/}"; done)
"$here/capture-trace.sh" 3 drm_msm_gpu:msm_gpu_submit_retired dpu:dpu_crtc_complete_flip \
    drm_msm_atomic:msm_atomic_commit_tail_start > "$1"
awk -v tids="$tids" '
BEGIN { n = split(tids, ids); for (i = 1; i <= n; i++) threads[ids[i]] = 1 }
/msm_gpu_submit_retired/ {
    pid = elapsed = ""
    for (i = 1; i <= NF; i++) {
        if ($i ~ /^pid=/) { pid = $i; sub(/^pid=/, "", pid) }
        if ($i ~ /^elapsed=/) { elapsed = $i; sub(/^elapsed=/, "", elapsed) }
    }
    if (pid in threads && elapsed != "") print elapsed
}' "$1" | sort -n | awk '{a[NR]=$1} END {if (NR) printf "monado GPU jobs %d, median %.2f ms, p90 %.2f ms\n", NR, a[int(NR/2)+1]/1e6, a[int((NR-1)*0.9)+1]/1e6}'
awk '{for (i = 1; i <= NF; i++) if ($i ~ /^mhz=/) { sub(/^mhz=/, "", $i); print $i }}' "$1" |
    sort -n | awk '{a[NR]=$1} END {if (NR) printf "GPU clock median %d MHz\n", a[int(NR/2)+1]}'
