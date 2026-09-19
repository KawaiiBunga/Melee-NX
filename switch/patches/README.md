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
| `aurora-switch-status-compat.patch` | `ref/melee-pc/extern/aurora` | Dawn/WebGPU status API compatibility for the pinned source snapshot |
| `aurora-switch-no-backtrace.patch` | `ref/melee-pc/extern/aurora` | Disables unsupported host backtrace integration on Switch |
| `aurora-switch-no-mmap.patch` | `ref/melee-pc/extern/aurora` | Replaces unsupported mmap-dependent behavior for Horizon/newlib |
| `aurora-switch-pipeline-cache-io-lock.patch` | `ref/melee-pc/extern/aurora` | Serializes cache I/O against Switch filesystem lifetime hazards; retained pending measured removal testing |
| `aurora-switch-mem1-window.patch` | `ref/melee-pc/extern/aurora` | Makes Aurora resolve Melee's 32-bit disc-pointer slots through the MEM1 4 GiB window |
| `aurora-switch-encoder-state-cache.patch` | `ref/melee-pc/extern/aurora` | Dusklight-derived render-pass-scoped suppression of redundant texture bind-group and destination-alpha blend-constant calls |
| `aurora-switch-cache-recovery.patch` | `ref/melee-pc/extern/aurora` | Quarantines and rebuilds an unreadable dawn/pipeline cache instead of disabling caching for every future launch; counts draws skipped because their pipeline was still compiling |
| `aurora-switch-perf-imgui.patch` | `ref/melee-pc/extern/aurora` | Local performance/profiler and ImGui integration changes; currently applied in the reference snapshot but not yet owned by `build-graphics.sh` |
| `melee-switch-gcc-compat.patch` | `ref/melee-pc` | Renames melee-pc's `main()` to `melee_main_impl()` under `__SWITCH__` (avoids clashing with `main_switch.cpp`'s NRO entry); points `SDL_GetPrefPath`/window title at `melee-nx`; forces `startFullscreen` |
| `melee-switch-disc-ptr-window.patch` | `ref/melee-pc` | Adds MEM1-window pointer encoding/resolution and converts raw disc-slot casts to `DP()` |
| `melee-switch-input-worker.patch` | `ref/melee-pc` | Avoids creating an unused auxiliary SDL input worker on Switch; `PADRead` remains on the main thread |
| `melee-switch-perf-telemetry.patch` | `ref/melee-pc` | Enables the existing frame profiler by default on Switch and writes its per-second stage breakdown to `melee-nx-runtime.log` |
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

## Current patch-stack caveat

The 2026-09-19 local `ref/melee-pc` tree is a layered historical snapshot.
Several later focused patches and manual edits overlap files and context owned
by `melee-switch-gcc-compat.patch`, so that old monolithic patch does not
reverse-match the fully layered tree even though its functional changes are
present. Do not force-apply it or reset the reference tree. The currently
verified build path is `build-graphics.sh prepare`, then the separate
`build-melee.sh configure` and `build-melee.sh build` stages. New patches must
still follow reverse-check-first idempotency and should own disjoint source
regions wherever possible. Patch-stack normalization is tracked in
`docs/TODO.md` and `docs/PLAN-CPU-SDL-DUSKLIGHT.md`.
