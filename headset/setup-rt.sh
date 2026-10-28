#!/bin/sh
# Creates the SteamVR-shaped layout driver_cv derives from /proc/self/exe.
set -eu
runtime="$HOME/sf/rt"
shim="$HOME/.local/opt/monado/lib/monado/vrclient.so"
[ -f "$shim" ] || { echo "Deploy the cross-built Monado runtime first" >&2; exit 1; }
rm -rf "$runtime"
mkdir -p "$runtime/bin/linuxarm64" "$runtime/drivers/cv/bin/linuxarm64"
ln -s /opt/steamvr/resources "$runtime/resources"
"$HOME/sf/frame-monado/eyetracking-tools"
for d in /opt/steamvr/drivers/*; do [ "${d##*/}" = cv ] || ln -s "$d" "$runtime/drivers/"; done
for f in /opt/steamvr/drivers/cv/*; do [ "${f##*/}" = bin ] || ln -s "$f" "$runtime/drivers/cv/"; done
for f in /opt/steamvr/drivers/cv/bin/linuxarm64/*; do ln -s "$f" "$runtime/drivers/cv/bin/linuxarm64/"; done
# Helpers driver_cv runs from the runtime, e.g. start_eyetracking.sh; not SteamVR's vrclient.so or executables
for f in /opt/steamvr/bin/linuxarm64/*; do
	case "${f##*/}" in vrclient.so | vrserver | vrcompositor* | vrstartup | vrdashboard | vrmonitor) continue ;; esac
	ln -s "$f" "$runtime/bin/linuxarm64/"
done
# A hardlink, since /proc/self/exe resolves symlinks
ln -f "$HOME/.local/opt/monado/bin/monado-service" "$runtime/bin/linuxarm64/monado-service"
# XRService loads the shim through VR_OVERRIDE, which driver_cv sets to this layout.
install -m 755 "$shim" "$runtime/bin/linuxarm64/vrclient.so"
