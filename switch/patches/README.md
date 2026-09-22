# switch/patches

Patches applied idempotently by `builder/build-graphics.sh` and `builder/build-melee.sh`.

| File | Tree | Purpose |
|------|------|---------|
| `dawn-switch-libnx.patch` | `ref/dawn` | libnx platform shim, NWindow surface type |
| `dawn-abseil-switch.patch` | `ref/dawn/third_party/abseil-cpp` | POSIX guards for newlib |
| `dawn-switch-surface.patch` | `ref/dawn` | Adds `SurfaceSourceSwitchNativeWindow` + `VK_NN_vi_surface` Vulkan surface creation |
| `dawn-switch-renderdoc.patch` | `ref/dawn` | `third_party/renderdoc`'s vendored header `#error`s on any platform it doesn't recognize; adds `__SWITCH__` alongside Linux/BSD (empty calling convention) |
| `sdl-switch-external-graphics.patch` | `ref/SDL` | External-graphics mode (no EGL ownership) for the legacy SDL 3.4.4 tree (`MELEE_SDL_VARIANT=legacy`) |
| `sdl-dusklight-switch.patch` | `ref/SDL-dusklight` | Dusklight's Switch backend for pinned SDL `release-3.4.10`, copied verbatim from `dusklight-nx/platforms/switch/patches/sdl3-switch.patch` |
| `sdl-dusklight-melee-nx.patch` | `ref/SDL-dusklight` | melee-nx's layer on that backend: external-graphics mode, four-player pad enumeration with per-device state and analog triggers, sideways single Joy-Con support (single-Joy-Con styles + horizontal hold, SL/SR→L/R), audren open/close error handling and short-buffer padding |
| `aurora-switch-surface.patch` | `ref/melee-pc/extern/aurora` | Constructs `SurfaceSourceSwitchNativeWindow` from `nwindowGetDefault()` in `BackendBinding.cpp` |
| `aurora-switch-dawn-backends.patch` | `ref/melee-pc/extern/aurora` | Declares Vulkan-only `DAWN_ENABLE_*` for `CMAKE_SYSTEM_NAME=NintendoSwitch`; forces present mode to Fifo on Switch for bring-up |
| `aurora-switch-platform-compat.patch` | `ref/melee-pc/extern/aurora` | Preserves the recovered snapshot's AArch64 math and THP compatibility changes |
| `aurora-switch-no-backtrace.patch` | `ref/melee-pc/extern/aurora` | Disables unsupported host backtrace integration on Switch |
| `aurora-switch-no-mmap.patch` | `ref/melee-pc/extern/aurora` | Replaces unsupported mmap-dependent behavior for Horizon/newlib; also the DVD loose-disc backend that boots from a `files/` extraction with no disc image (`CommandDataLoose`, `aurora_dvd_open_files`/`aurora_dvd_save_meta`) |
| `aurora-switch-io-atomic.patch` | `ref/melee-pc/extern/aurora` | Atomic file write (`io.cpp`) falls back past Horizon/sdmc rename and `O_EXCL` failures so controller/keyboard bindings and other atomic writes actually persist |
| `aurora-switch-pad-trigger-bind.patch` | `ref/melee-pc/extern/aurora` | Lets an analog trigger (reported by SDL as an axis) drive a digital GC button, so GC Z can bind to the ZL/ZR triggers (`pad.cpp`) |
| `aurora-switch-mem1-window.patch` | `ref/melee-pc/extern/aurora` | Makes Aurora resolve Melee's 32-bit disc-pointer slots through the MEM1 4 GiB window |
| `aurora-switch-encoder-state-cache.patch` | `ref/melee-pc/extern/aurora` | Dusklight-derived render-pass-scoped suppression of redundant texture bind-group and destination-alpha blend-constant calls |
| `aurora-switch-cache-recovery.patch` | `ref/melee-pc/extern/aurora` | Descriptor-cache recovery/I/O lock, adaptive compiler pacing, optional second worker, in-flight request tracking/write deduplication, pipeline telemetry |
| `aurora-switch-blob-cache-batch.patch` | `ref/melee-pc/extern/aurora` | Dawn cache recovery, batched writes, flush API declarations, hit/miss and callback timing telemetry |
| `aurora-switch-texture-telemetry.patch` | `ref/melee-pc/extern/aurora` | Stable texture identity/content checks, dynamic-texture cache bypass, `TEXSTATS` and end-frame timing |
| `aurora-switch-perf-imgui.patch` | `ref/melee-pc/extern/aurora` | ImGui pass reuse, Dawn status compatibility, flush API implementation and end-frame stage timers |
| `aurora-switch-gx-cpu.patch` | `ref/melee-pc/extern/aurora` | Dusklight-derived redundant effective-state suppression and bounded exact-key shader analysis; preserves movie texture dirtiness; `GXCPU` counters |
| `aurora-switch-runtime-fixes.patch` | `ref/melee-pc/extern/aurora` | Previously uncaptured card-probe TTL, stable texture IDs and no-cache texture API/metadata |
| `aurora-switch-thread-sweep.patch` | `ref/melee-pc/extern/aurora` | Connects named thread setup to the optional Switch affinity sweep |
| `melee-switch-gcc-compat.patch` | `ref/melee-pc` | Launcher/resource/file-cache compatibility, recovered audio changes and negotiated-device telemetry; also ISO-free boot wiring (`launcher.cpp` loose sentinel + metadata capture) and `discfont.c` DOL save/restore |
| `melee-switch-disc-ptr-window.patch` | `ref/melee-pc` | Adds MEM1-window pointer encoding/resolution and converts raw disc-slot casts to `DP()` |
| `melee-switch-input-worker.patch` | `ref/melee-pc` | Avoids creating an unused auxiliary SDL input worker on Switch; `PADRead` remains on the main thread |
| `melee-switch-perf-telemetry.patch` | `ref/melee-pc` | Entry-point rename, Switch startup/log sink, OS compatibility and frame/alarms/retrace telemetry |
| `tracy-switch-platform.patch` | fetched Tracy source | Switch platform guards for the optional Tracy dependency |

Patches are adapted from the proven KartPad-NX set (dawn/sdl) where the underlying
tree is identical. **The three `aurora-*` and `melee-switch-gcc-compat` patches were
rewritten from scratch for this project** — melee-pc vendors its own aurora fork at a
different point than KartPad-NX's aurora checkout (different namespace style, a
simpler `create_window()`, and Dawn API shapes that had already moved past what
KartPad's patches were written against), so KartPad's originals did not apply and in
two cases (`aurora-switch-dawn-api.patch`, `aurora-switch-window.patch`'s window.cpp
hunks) turned out to be unnecessary here.

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

## Reconstructed patch ownership (2026-09-21)

The Melee/Aurora patches now own disjoint files against melee-pc revision
`7c9a468f4f8206780c4cd762be1da7772daaeabf`. They preserve the current
hardware-tested source state, including previously uncaptured September 20
card/texture/movie and compiler/cache edits. Verification covers 20 patches
and 74 Melee/Aurora files, plus the SDL stack. The old status and pipeline-I/O patches are absorbed into the owning
patches above. `build-graphics.sh` applies the profiler patch as well.

Aurora patch paths are relative to `extern/aurora`; Melee patch paths are
relative to melee-pc. Git must run from its actual repository root with an
explicit directory prefix for nested trees. A success exit code from
`git -C extern/aurora apply` can otherwise mean every hunk was skipped.

Run `python builder/verify-patches.py` after editing reference sources. It
reconstructs patched files from pinned Git objects in a disposable directory,
checks application and reversal, rejects overlapping ownership, and compares
the result with the live sources. Do not reset or clean the reference trees.
