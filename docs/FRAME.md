# Steam Frame runtime

This is the standalone Monado fork for the Steam Frame. `main` contains the headset drivers, tracking,
passthrough, compositor and floor estimation.

```sh
git clone git+ssh://github.com_konsti219/konsti219/frame-monado.git
cd frame-monado
just build
just deploy
```

Builds run on the PC in the Podman cross-compilation SDK. Sources are tracked normally and edited here;
`out/` holds ignored build products. The headset receives binaries and session scripts only. Deployment
checks dependencies, preserves whether the shell/runtime were running, and restarts those services.
`FRAME_HEADSET` overrides the default `steamos@frame` SSH target.

The build also produces a matching x86-64 IPC client for Windows games running through FEX and x86-64 Proton.
Only that client runs under emulation; the headset service and compositor stay ARM64. The shared
`share/openxr/1/openxr_frame.json` uses the dynamic linker's `$LIB` token to select `lib/libopenxr_monado.so`
on SteamOS ARM64 or `lib/x86_64-linux-gnu/libopenxr_monado.so` in the x86 Steam Runtime. The client build uses
the headset build's version tag so Monado's IPC version check remains enabled. This client supports Vulkan,
which Proton's OpenXR bridge uses for translated Direct3D applications.
Touch and simple-controller suggestions are rebound to the Frame profile, including grip/aim poses and
haptics, so OpenXR games do not need explicit Frame support. Touch is preferred over the minimal simple
profile when both are available; its left X/Y buttons map to D-pad down/right, matching WiVRn.

Frame shell selects this manifest and shares `$XDG_RUNTIME_DIR/monado_comp_ipc` with Steam's containers through
`PRESSURE_VESSEL_FILESYSTEMS_RW`. It disables automatic OpenXR runtime importing: FEX's separate graphics
provider root cannot resolve libraries in the host home directory. The libraries themselves are already visible
through Steam's normal home-directory mount. These settings belong to the shell session and leave SteamVR's
system runtime selection alone.

Android games (Lepton, Steam's Android compatibility tool) get an Android build of the client
(`lib/android-arm64/libopenxr_monado.so`, `XRT_FEATURE_ANDROID_HOSTED`, NDK fetched by `just build`). It talks to
the Linux service like any other client: socket in `/data/steamvr/runtime` (Lepton's mount of `steamvr path`, which
the frame shell points at a folder with this client and a hard link to the socket), images as dma-buf fds, frames
signalled with sync files, the Linux layer limit (the IPC shared memory layout) and debug options from the
environment (`LEPTON_ENV_XRT_LOG=debug` and so on). Projection layers of alpha blended frames use their alpha
whether or not they set source alpha, as SteamVR does: Unreal games set none.

`headset/` contains SteamOS session integration. `setup-rt.sh` creates the runtime
layout using the headset's installed SteamVR driver and the deployed Monado shim. SteamOS session/SDDM
setup remains machine configuration, not a build requirement.

With `driver_cv`, discovery finishes as soon as the HMD has completed activation; the stable controller slots
accept controllers later. `LH_DISCOVER_WAIT_MS` is the timeout when the HMD is missing, rather than a mandatory
startup delay. Suspend stops the shell/runtime and restarts them on wake; switching from SteamVR still waits
for `vrserver` to release the controller radio before starting Monado.

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

From the compositor's start until the head is first tracked, it shows only the frame shell logo, head-locked on
black (`comp_frame_splash.c`, logo from `frame_shell_logo.rgba`). Apps rendering before tracking would show up
rolled with the panels.

## Settings

Runtime settings that stay go in the `frame` object of Monado's config file, `~/.config/monado/config_v0.json`
on the headset, read when the compositor starts (restart `monado.service` after editing):

```json
{
  "frame": {
    "passthrough_scratch_scale": 0.5,
    "passthrough_behind_opaque": false,
    "gpu_max_mhz": 578,
    "stop_sched_ext": false,
    "mirror": true,
    "mirror_fps": 60,
    "eye_tracking_fps": 36
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
- `mirror` (default true): the headset view in SteamVR's v4l2loopback device (`/dev/video99`, 1920x1080 RGB3), as
  SteamVR's v4l2cam provides it (FrameMate's mirroring): the left eye without lens distortion, turned upright and cropped to 16:9 around
  its forward direction and zoomed to the middle 75% (clear of the passthrough camera's edge and apps' hidden areas), drawn again from the frame's scratch images only while a program streams from the device.
  `mirror_fps` (default 60) caps its rate; at 90 Hz that's every second frame.
- `eye_tracking_fps` (15–90, default 36): the eye cameras' rate. `driver_cv` asks Valve's eye service for
  `steamvr/eyeTrackingRateMin` while gaze dependent eye-to-head is off, which `frame_hmd`'s settings make 15 fps;
  Monado's `cv` settings layer sets it from this. In a panel-on idle comparison, 72 fps cost about 0.5 W more
  than 15 fps, including the shell's eye expressions; the eye service used ~8% of a core versus ~2%.

The eye gaze is corrected with `~/.config/frame-shell/eye-calibration`, which the frame shell keeps fitting from
pointer clicks (the user looks at what they click): `corrected = (measured - offset) / gain` per axis in degrees,
reread within a second. Valve's eye tracking service only calibrates itself from SteamVR's UI clicks, so without it
the gaze keeps its generic model (measured about 7° too high and 1.2× too steep vertically). Delete the file to start
over.
