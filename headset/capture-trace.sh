#!/bin/sh
# Captures selected ftrace events in a private instance and writes the trace to stdout.
set -eu
if [ "$#" -lt 2 ]; then
    echo "usage: $0 <seconds> <group:event> ..." >&2
    exit 1
fi
seconds=$1
shift
case "$seconds" in ''|*[!0-9]*|0) echo "seconds must be a positive integer" >&2; exit 1 ;; esac
if [ "$(id -u)" -ne 0 ]; then
    exec sudo "$0" "$seconds" "$@"
fi
trace=/sys/kernel/tracing/instances/steamframe-$$
mkdir "$trace"
cleanup() {
    echo 0 > "$trace/tracing_on"
    echo 0 > "$trace/events/enable"
    rmdir "$trace"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
echo 0 > "$trace/tracing_on"
echo 8192 > "$trace/buffer_size_kb"
echo mono > "$trace/trace_clock"
for event; do
    case "$event" in *[!a-zA-Z0-9_:]*|*:*:*) echo "invalid event: $event" >&2; exit 1 ;; esac
    echo 1 > "$trace/events/${event%:*}/${event#*:}/enable"
done
echo 1 > "$trace/tracing_on"
sleep "$seconds"
echo 0 > "$trace/tracing_on"
cat "$trace/trace"
