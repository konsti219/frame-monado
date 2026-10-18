# Steam Frame runtime

This is the standalone Monado fork for the Steam Frame. `main` contains the headset drivers, tracking,
passthrough, compositor, positional timewarp and floor estimation.

```sh
git clone git+ssh://github.com_konsti219/konsti219/frame-monado.git
cd frame-monado
./build-headset
./deploy
```

Builds run on the PC in the Podman cross-compilation SDK. Sources are tracked normally and edited here;
`out/` holds ignored build products. The headset receives binaries and session scripts only. Deployment
checks dependencies, preserves whether the shell/runtime were running, and restarts those services.
`FRAME_HEADSET` overrides the default `steamos@frame` SSH target.

`headset/` contains SteamOS session integration and diagnostic scripts. `setup-rt.sh` creates the runtime
layout using the headset's installed SteamVR driver and the deployed Monado shim. SteamOS session/SDDM
setup remains machine configuration, not a build requirement.

The `wivrn` branch is the PC runtime dependency: upstream Monado `09741cbcb` with WiVRn's integrations
and app frame metrics applied as committed source. It deliberately uses WiVRn's runtime revision rather
than the headset branch. Frame shell pins its commit in `flake.lock`; it does not apply runtime patches.
For host-runtime changes, create a normal worktree, edit and commit there, push, then update the shell lock:

```sh
git worktree add ../frame-monado-host wivrn
cd ../frame-monado-host
# From frame-shell: FRAME_HOST_MONADO=../frame-monado-host ./build host
# Commit and push the runtime change, then update frame-shell: nix flake update host-monado
```

The headset shell/client/server/UI live separately in `frame-shell`. Building or deploying that repository
does not build or replace the headset Monado runtime.

## Settings

Runtime settings that stay go in the `frame` object of Monado's config file, `~/.config/monado/config_v0.json`
on the headset, read when the compositor starts (restart `monado.service` after editing):

```json
{
  "frame": {
    "passthrough_scratch_scale": 0.5,
    "passthrough_behind_opaque": false,
    "gpu_max_mhz": 422,
    "stop_sched_ext": true
  }
}
```

- `passthrough_scratch_scale` (0–1, default 0.5): size of the scratch images while they only hold passthrough.
  The cameras resolve about 330 px/rad against about 1100 on the panel; 0.35 is visibly softer.
- `passthrough_behind_opaque` (default false): keep the cameras and passthrough running while an opaque
  projection layer (a PC VR stream without alpha) hides them. Off saves ~0.8 W while streaming.

- `gpu_max_mhz` (default 422): GPU clock cap for the session, applied by `monado.service` (restored afterwards).
  Under the spin test, 422 against 578 MHz saves ~0.3 W with no late frames and ~0.7 ms longer compositor budget;
  366 saves nothing more.
- `stop_sched_ext` (default true): stop SteamOS's `scx_lavd` scheduler for the session (started again afterwards).
  lavd keeps the CPU clusters near their top clocks at 20–30% load; the kernel's default scheduler with schedutil
  saves ~0.25 W of CPU (0.1–0.3 W total) in the spin test with no more late frames. Realtime threads (compositor,
  Valve's tracking) aren't scheduled by sched_ext either way.

The eye gaze is corrected with `~/.config/frame-shell/eye-calibration`, which the frame shell keeps fitting from
pointer clicks (the user looks at what they click): `corrected = (measured - offset) / gain` per axis in degrees,
reread within a second. Valve's eye tracking service only calibrates itself from SteamVR's UI clicks, so without it
the gaze keeps its generic model (measured about 7° too high and 1.2× too steep vertically). Delete the file to start
over.
