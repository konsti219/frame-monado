# Steam Frame runtime

This is the standalone Monado fork for the Steam Frame. `main` contains the headset drivers, tracking,
passthrough, compositor and floor estimation.

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
    "gpu_max_mhz": 578,
    "stop_sched_ext": false
  }
}
```

- `passthrough_scratch_scale` (0–1, default 0.5): size of the scratch images while they only hold passthrough.
  The cameras resolve about 330 px/rad against about 1100 on the panel; 0.35 is visibly softer.
- `passthrough_behind_opaque` (default false): keep the cameras and passthrough running while an opaque
  projection layer (a PC VR stream without alpha) hides them. Off saves ~0.8 W while streaming.

- `gpu_max_mhz` (default 578): GPU clock cap for the session, applied by `monado.service` (restored afterwards).
  422 saves ~0.3 W in the spin test, but in VRChat with overlays it missed ~3 display frames a minute (vs ~0.3).
- Projection layers can carry a foveated image (`XR_FRAME_foveated_projection`, chained to the projection view): the
  distortion pass expands it through per-axis lookup tables (`mesh_foveated.frag`), so the frame shell's client
  doesn't expand its stream to the full render size first. Opaque layers on the direct paths only; a squashed one
  is shown unexpanded (logged once).
- `stop_sched_ext` (default false): stop SteamOS's `scx_lavd` scheduler for the session (started again afterwards).
  Saves ~0.25 W of CPU, but the frame shell's client then delivers ~6% of its frames too late for Monado (reused,
  a visible jump with motion) against ~0.4% under lavd.

The eye gaze is corrected with `~/.config/frame-shell/eye-calibration`, which the frame shell keeps fitting from
pointer clicks (the user looks at what they click): `corrected = (measured - offset) / gain` per axis in degrees,
reread within a second. Valve's eye tracking service only calibrates itself from SteamVR's UI clicks, so without it
the gaze keeps its generic model (measured about 7° too high and 1.2× too steep vertically). Delete the file to start
over.
