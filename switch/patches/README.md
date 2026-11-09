# Patch catalog

The build scripts reverse-check patches before applying them. Source pins are
in [the dependency guide](../../docs/DEPS.md); the maintenance workflow is in
[CONTRIBUTING.md](../../CONTRIBUTING.md).

## Dawn

`ref/dawn`; the Abseil patch targets `third_party/abseil-cpp`.

| Patch | Purpose |
|---|---|
| [dawn-switch-libnx.patch](dawn-switch-libnx.patch) | libnx platform shim, NWindow surface type |
| [dawn-abseil-switch.patch](dawn-abseil-switch.patch) | POSIX guards for newlib |
| [dawn-switch-surface.patch](dawn-switch-surface.patch) | Adds `SurfaceSourceSwitchNativeWindow` (and Vulkan surface code left from the removed NVK path) |
| [dawn-switch-renderdoc.patch](dawn-switch-renderdoc.patch) | Allow the Switch platform in the vendored RenderDoc header |

## SDL

Dusklight patches target `ref/SDL-dusklight`; the legacy patch targets `ref/SDL`.

| Patch | Purpose |
|---|---|
| [sdl-switch-external-graphics.patch](sdl-switch-external-graphics.patch) | External-graphics mode (no EGL ownership) for the legacy SDL 3.4.4 tree (`MELEE_SDL_VARIANT=legacy`) |
| [sdl-dusklight-switch.patch](sdl-dusklight-switch.patch) | Dusklight Switch video, audio, and joystick backend |
| [sdl-dusklight-melee-nx.patch](sdl-dusklight-melee-nx.patch) | External graphics, four-player input, analog triggers, sideways Joy-Cons, and audio handling |

## Aurora

`ref/melee-pc/extern/aurora`.

| Patch | Purpose |
|---|---|
| [aurora-switch-surface.patch](aurora-switch-surface.patch) | Constructs `SurfaceSourceSwitchNativeWindow` from `nwindowGetDefault()` in `BackendBinding.cpp` |
| [aurora-switch-dawn-backends.patch](aurora-switch-dawn-backends.patch) | Backend selection and Fifo presentation (written for the removed Vulkan path; the deko3d staging edits these files) |
| [aurora-switch-platform-compat.patch](aurora-switch-platform-compat.patch) | Preserves the recovered snapshot's AArch64 math and THP compatibility changes |
| [aurora-switch-no-backtrace.patch](aurora-switch-no-backtrace.patch) | Disables unsupported host backtrace integration on Switch |
| [aurora-switch-no-mmap.patch](aurora-switch-no-mmap.patch) | Horizon file access and the extracted-data DVD backend |
| [aurora-switch-io-atomic.patch](aurora-switch-io-atomic.patch) | Horizon rename and exclusive-create fallbacks for persistent settings |
| [aurora-switch-pad-trigger-bind.patch](aurora-switch-pad-trigger-bind.patch) | Allow analog triggers to bind digital GameCube buttons |
| [aurora-switch-mem1-window.patch](aurora-switch-mem1-window.patch) | Makes Aurora resolve Melee's 32-bit disc-pointer slots through the MEM1 4 GiB window |
| [aurora-switch-encoder-state-cache.patch](aurora-switch-encoder-state-cache.patch) | Reuse texture bind groups and destination-alpha blend constants within a render pass |
| [aurora-switch-cache-recovery.patch](aurora-switch-cache-recovery.patch) | Descriptor recovery, compiler pacing, in-flight request tracking, persistence, and telemetry |
| [aurora-switch-blob-cache-batch.patch](aurora-switch-blob-cache-batch.patch) | Dawn cache recovery, batched writes, decoded blob reuse, and lookup telemetry |
| [aurora-switch-texture-telemetry.patch](aurora-switch-texture-telemetry.patch) | Stable texture identity/content checks, dynamic-texture cache bypass, `TEXSTATS` and end-frame timing |
| [aurora-switch-perf-imgui.patch](aurora-switch-perf-imgui.patch) | ImGui pass reuse, Dawn status compatibility, cache flushing, and frame stage timers |
| [aurora-switch-gx-cpu.patch](aurora-switch-gx-cpu.patch) | Dusklight-derived redundant effective-state suppression and bounded exact-key shader analysis; preserves movie texture dirtiness; `GXCPU` counters |
| [aurora-switch-runtime-fixes.patch](aurora-switch-runtime-fixes.patch) | Card-probe TTL, stable texture IDs, and no-cache texture metadata |
| [aurora-switch-thread-sweep.patch](aurora-switch-thread-sweep.patch) | Connects named thread setup to the optional Switch affinity sweep |
| [aurora-switch-uber-shader.patch](aurora-switch-uber-shader.patch) | Uber-shader support: shared WGSL helpers, uber pipeline identity, uniform packing and support check, launch-warm stand-ins (draw hooks live in the GX CPU, cache-recovery and encoder patches) |

### Deko3D staging (not a patch)

The deko3d renderer needs edits to files that the patches above already own (`gpu.cpp`, `aurora.cpp`,
`pipeline_cache.cpp`, `gpu_cache.cpp`). Rather than fold them into those patches, `builder/integrate-deko.py`
copies the patched Aurora to `build/aurora-deko` and edits the copy behind `AURORA_GFX_DEKO3D`. The
reference tree, the ownership rule and `verify-patches.py` are untouched. Dawn's Tint GLSL writer gets
two options from `builder/prepare-deko.py` (idempotent, applied by `build-graphics.sh`).

## Melee

`ref/melee-pc`.

| Patch | Purpose |
|---|---|
| [melee-switch-gcc-compat.patch](melee-switch-gcc-compat.patch) | Platform and launcher compatibility, file cache, audio, and extracted-data boot wiring; the launcher's mod overlay (`MELEE_MODS_MANIFEST` in `file_cache.cpp`, see [docs/LAUNCHER.md](../../docs/LAUNCHER.md#how-the-game-applies-mods)) |
| [melee-switch-disc-ptr-window.patch](melee-switch-disc-ptr-window.patch) | Adds MEM1-window pointer encoding/resolution and converts raw disc-slot casts to `DP()` |
| [melee-switch-input-worker.patch](melee-switch-input-worker.patch) | Avoids creating an unused auxiliary SDL input worker on Switch; `PADRead` remains on the main thread |
| [melee-switch-rollback-hooks.patch](melee-switch-rollback-hooks.patch) | Rollback self-test hooks in the engine loop (`gm_801A4D34`): body begin/end and the pad renew point; inert unless perf.cfg `rollback_selftest` is set (see [docs/SLIPPI.md](../../docs/SLIPPI.md#rollback-self-test)) |
| [melee-switch-perf-telemetry.patch](melee-switch-perf-telemetry.patch) | Entry-point rename, shared logging, OS compatibility, and frame telemetry |
| [melee-switch-slippi-hooks.patch](melee-switch-slippi-hooks.patch) | Slippi online play: pad sampling, match start and exit, the lobby call, and Slippi's gameplay codes at their sites (see [docs/SLIPPI.md](../../docs/SLIPPI.md#how-online-play-is-built)); inert outside an online match |

Slippi hunks for files another patch already owns live in that patch: `fighter.c`, `ftlib.c` and
`ftparts.c` in disc-ptr-window, `gmscene.c` in rollback-hooks, `os.c` and `vi.c` in
perf-telemetry, and `launcher.cpp` (the status line) in gcc-compat.

## Build dependencies

Tracy is fetched into the build directory and patched by `Graphics.cmake`.

| Patch | Purpose |
|---|---|
| [tracy-switch-platform.patch](tracy-switch-platform.patch) | Switch platform guards for the optional Tracy dependency |

Dawn and legacy SDL integration derive from KartPad-NX. Melee and Aurora
patches target melee-pc's vendored sources and cannot be replaced directly with
patches for another Aurora revision.

## SDL patch stack

`sdl-dusklight-switch.patch` and `sdl-dusklight-melee-nx.patch` are one stack,
not two independent patches: the first adds `src/{video,audio,joystick}/switch`
and the second edits those same new files. Once the top layer is applied the
base no longer reverse-identifies, so `build-graphics.sh` reverse-checks the top
layer only and treats the whole stack as applied. Both need `git apply
--recount`: the upstream Dusklight patch's hunk line counts do not match its
bodies. Regenerate the melee-nx layer by reading `release-3.4.10` into a
scratch index, `git apply --cached --recount` the Dusklight patch, then
`git diff` the worktree against that index.

## Ownership and verification

The Melee/Aurora patches own disjoint files against melee-pc revision
`7c9a468f4f8206780c4cd762be1da7772daaeabf`. Verification covers 20 patches
and 74 Melee/Aurora files, plus the 19-file SDL stack. Keep each file's changes
in its owning patch.

Aurora patch paths are relative to `extern/aurora`; Melee patch paths are
relative to melee-pc. Git must run from its actual repository root with an
explicit directory prefix for nested trees. A success exit code from
`git -C extern/aurora apply` can otherwise mean every hunk was skipped.

Run `python builder/verify-patches.py` after editing reference sources. It
reconstructs patched files from pinned Git objects in a disposable directory,
checks application and reversal, rejects overlapping ownership, and compares
the result with the live sources. Do not reset or clean the reference trees.
