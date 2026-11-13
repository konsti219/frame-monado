# Frame Monado

<!--
Copyright 2018-2021, Collabora, Ltd.

SPDX-License-Identifier: CC-BY-4.0
-->

[Monado](https://monado.freedesktop.org/), the open source OpenXR runtime, running natively on the Valve Steam
Frame in place of SteamVR. [Frame shell](https://github.com/konsti219/frame-shell) is the shell that runs on it.

This is a fork of upstream Monado, not part of it: the Frame's changes are commits on top of an upstream
revision, rebased now and then. Most are specific to the Frame, its Valve driver and the frame shell.
It supports:

- head and controller tracking, including finger tracking, through Valve's own driver (driver_cv and XRService)
- eye gaze (`XR_EXT_eye_gaze_interaction`), corrected by a calibration the frame shell keeps fitting
- colour passthrough behind alpha-blended apps, and the floor height estimated from the cameras' room mesh
- SteamVR's lens distortion and frame pacing at 72 Hz, foveated projection layers, and the headset view as a
  webcam device
- OpenXR clients for x86-64 games (FEX/Proton) and Android games (Lepton); Touch and simple controller bindings
  are remapped to the Frame's controllers

## Install

Requirements: a Steam Frame reachable over SSH as `steamos` (with a password set), and a PC with Podman and `just`.

```sh
just build      # cross-builds in Podman
just setup      # once: registers the Monado session with SteamOS (asks for the sudo password)
just deploy     # over SSH (FRAME_HEADSET, default steamos@frame.local)
```

This installs:

- Monado's service and its OpenXR clients (ARM64, x86-64, Android) in `~/.local/opt/monado`
- systemd user units that run it as its own session, next to SteamVR's driver in a runtime layout in
  `~/.local/share/frame-monado`
- the session entry for SteamOS's session switch, in `/etc/frame-monado` and `/etc/sddm.conf.d`

On the headset, `steamosctl switch-to-desktop-mode` starts Monado and `steamosctl switch-to-game-mode` goes back
to SteamVR; Monado failing also switches back.

## More

- [doc/steam-frame.md](doc/steam-frame.md): how it works, settings, and the branch layout
- Branches: `main` is the headset runtime; `wivrn` is the PC runtime for frame shell (WiVRn's Monado revision)
- Upstream Monado: <https://gitlab.freedesktop.org/monado/monado>

## License

BSL-1.0, see [LICENSE](LICENSE) and [LICENSES/](LICENSES).
