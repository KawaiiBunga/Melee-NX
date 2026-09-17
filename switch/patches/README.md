# switch/patches

Patches applied idempotently by `builder/build-graphics.sh` and `builder/build-melee.sh`.

| File | Tree | Purpose |
|------|------|---------|
| `dawn-switch-libnx.patch` | `ref/dawn` | libnx platform shim, NWindow surface type |
| `dawn-abseil-switch.patch` | `ref/dawn/third_party/abseil-cpp` | POSIX guards for newlib |
| `dawn-switch-surface.patch` | `ref/dawn` | Switch native surface creation |
| `sdl-switch-external-graphics.patch` | `ref/SDL` | External-graphics mode (no EGL ownership) |
| `aurora-switch-dawn-api.patch` | `ref/aurora` | Dawn API surface hooks |
| `aurora-switch-surface.patch` | `ref/aurora` | Switch NWindow surface wiring |
| `aurora-switch-window.patch` | `ref/aurora` | Window/fullscreen handling for Switch |
| `melee-switch-gcc-compat.patch` | `ref/melee-pc` | NRO ABI: remove -no-pie/-Ttext-segment; add Switch paths; GCC aarch64 flags |

Patches are copied from the proven KartPad-NX set (dawn/sdl/aurora) and adapted.
The melee-pc patch is developed here.
