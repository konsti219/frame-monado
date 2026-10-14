#!/bin/sh
# Counts DSI vblanks and completed page flips over <seconds> (default 5).
set -eu
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
seconds=${1:-5}
trace=$(mktemp)
trap 'rm -f "$trace"' EXIT
"$here/capture-trace.sh" "$seconds" drm:drm_vblank_event dpu:dpu_crtc_complete_flip > "$trace"
awk -v seconds="$seconds" '
    /drm_vblank_event:/ { v++ }
    /dpu_crtc_complete_flip:/ { f++ }
    END { printf "vblank/s %.1f  flips/s %.1f (%d/%d)\n", v / seconds, f / seconds, f, v }
' "$trace"
