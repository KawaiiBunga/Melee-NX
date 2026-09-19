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
| `sdl-dusklight-melee-nx.patch` | `ref/SDL-dusklight` | melee-nx's layer on that backend: external-graphics mode, four-player pad enumeration with per-device state and analog triggers, audren open/close error handling and short-buffer padding |
| `aurora-switch-surface.patch` | `ref/melee-pc/extern/aurora` | Constructs `SurfaceSourceSwitchNativeWindow` from `nwindowGetDefault()` in `BackendBinding.cpp` |
| `aurora-switch-dawn-backends.patch` | `ref/melee-pc/extern/aurora` | Declares Vulkan-only `DAWN_ENABLE_*` for `CMAKE_SYSTEM_NAME=NintendoSwitch`; forces present mode to Fifo on Switch for bring-up |
| `aurora-switch-platform-compat.patch` | `ref/melee-pc/extern/aurora` | Preserves the recovered snapshot's AArch64 math and THP compatibility changes |
| `aurora-switch-no-backtrace.patch` | `ref/melee-pc/extern/aurora` | Disables unsupported host backtrace integration on Switch |
| `aurora-switch-no-mmap.patch` | `ref/melee-pc/extern/aurora` | Replaces unsupported mmap-dependent behavior for Horizon/newlib |
| `aurora-switch-mem1-window.patch` | `ref/melee-pc/extern/aurora` | Makes Aurora resolve Melee's 32-bit disc-pointer slots through the MEM1 4 GiB window |
| `aurora-switch-encoder-state-cache.patch` | `ref/melee-pc/extern/aurora` | Dusklight-derived render-pass-scoped suppression of redundant texture bind-group and destination-alpha blend-constant calls |
| `aurora-switch-cache-recovery.patch` | `ref/melee-pc/extern/aurora` | Descriptor-cache recovery and I/O lock, compiler pin/priority/pacing, skipped-draw telemetry |
| `aurora-switch-blob-cache-batch.patch` | `ref/melee-pc/extern/aurora` | Dawn cache recovery, batched writes, flush API declarations |
| `aurora-switch-perf-imgui.patch` | `ref/melee-pc/extern/aurora` | ImGui pass reuse, Dawn status compatibility, flush API implementation and end-frame stage timers |
| `aurora-switch-thread-sweep.patch` | `ref/melee-pc/extern/aurora` | Connects named thread setup to the optional Switch affinity sweep |
| `melee-switch-gcc-compat.patch` | `ref/melee-pc` | Launcher/resource/file-cache compatibility, recovered audio changes and negotiated-device telemetry |
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
hunks) turned out to be unnecessary here — see `docs/PORTING-NOTES.md` §2 for why.

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

## Reconstructed patch ownership (2026-09-19)

The Melee/Aurora patches now own disjoint files against melee-pc revision
`7c9a468f4f8206780c4cd762be1da7772daaeabf`. They preserve the complete recovered
source state, including previously uncaptured performance and compatibility
edits. The old status and pipeline-I/O patches are absorbed into the owning
patches above. `build-graphics.sh` applies the profiler patch as well.

Aurora patch paths are relative to `extern/aurora`; Melee patch paths are
relative to melee-pc. Git must run from its actual repository root with an
explicit directory prefix for nested trees. A success exit code from
`git -C extern/aurora apply` can otherwise mean every hunk was skipped.

Run `python builder/verify-patches.py` after editing reference sources. It
reconstructs patched files from pinned Git objects in a disposable directory,
checks application and reversal, rejects overlapping ownership, and compares
the result with the live sources. Do not reset or clean the reference trees.
Performance experiments are described in [PERFORMANCE.md](../../PERFORMANCE.md).
