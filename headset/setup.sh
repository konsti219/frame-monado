#!/bin/sh
# One-time headset setup: the Monado session as SteamOS's desktop session. /usr/local is wiped by updates, so the
# session entry is in /etc.
set -eu
here="$HOME/.local/share/frame-monado"
sudo install -Dm644 "$here/monado.desktop" /etc/frame-monado/wayland-sessions/monado.desktop
sudo install -Dm644 "$here/zz-frame-monado-sddm.conf" /etc/sddm.conf.d/zz-frame-monado.conf
install -Dm644 "$here/steamos-manager-xdg.conf" "$HOME/.config/systemd/user/steamos-manager.service.d/xdg.conf"
systemctl --user daemon-reload
systemctl --user restart steamos-manager.service
steamosctl set-default-desktop-session monado.desktop
echo "Done. Start Monado with: steamosctl switch-to-desktop-mode (back: steamosctl switch-to-game-mode)"
