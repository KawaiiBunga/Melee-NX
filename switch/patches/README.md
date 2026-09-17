# switch/patches

Patches applied idempotently by `builder/build-graphics.sh` and `builder/build-melee.sh`.

| File | Tree | Purpose |
|------|------|---------|
| `dawn-switch-libnx.patch` | `ref/dawn` | libnx platform shim, NWindow surface type |
| `dawn-abseil-switch.patch` | `ref/dawn/third_party/abseil-cpp` | POSIX guards for newlib |
| `dawn-switch-surface.patch` | `ref/dawn` | Adds `SurfaceSourceSwitchNativeWindow` + `VK_NN_vi_surface` Vulkan surface creation |
| `dawn-switch-renderdoc.patch` | `ref/dawn` | `third_party/renderdoc`'s vendored header `#error`s on any platform it doesn't recognize; adds `__SWITCH__` alongside Linux/BSD (empty calling convention) |
| `sdl-switch-external-graphics.patch` | `ref/SDL` | External-graphics mode (no EGL ownership) |
| `aurora-switch-surface.patch` | `ref/melee-pc/extern/aurora` | Constructs `SurfaceSourceSwitchNativeWindow` from `nwindowGetDefault()` in `BackendBinding.cpp` |
| `aurora-switch-dawn-backends.patch` | `ref/melee-pc/extern/aurora` | Declares Vulkan-only `DAWN_ENABLE_*` for `CMAKE_SYSTEM_NAME=NintendoSwitch`; forces present mode to Fifo on Switch for bring-up |
| `melee-switch-gcc-compat.patch` | `ref/melee-pc` | Renames melee-pc's `main()` to `melee_main_impl()` under `__SWITCH__` (avoids clashing with `main_switch.cpp`'s NRO entry); points `SDL_GetPrefPath`/window title at `melee-nx`; forces `startFullscreen` |

Patches are adapted from the proven KartPad-NX set (dawn/sdl) where the underlying
tree is identical. **The three `aurora-*` and `melee-switch-gcc-compat` patches were
rewritten from scratch for this project** — melee-pc vendors its own aurora fork at a
different point than KartPad-NX's aurora checkout (different namespace style, a
simpler `create_window()`, and Dawn API shapes that had already moved past what
KartPad's patches were written against), so KartPad's originals did not apply and in
two cases (`aurora-switch-dawn-api.patch`, `aurora-switch-window.patch`'s window.cpp
hunks) turned out to be unnecessary here — see `docs/PORTING-NOTES.md` §2 for why.
