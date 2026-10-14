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
