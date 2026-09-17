# Dependency acquisition

All reference trees go under `ref/`. They are gitignored — never commit them.

## ref/melee-pc — game source port

```bash
git clone https://github.com/999sian/melee-pc ref/melee-pc
```

Aurora is already vendored at `ref/melee-pc/extern/aurora` — no separate clone needed.

## ref/dawn — WebGPU/Vulkan backend

Use the **exact same revision** as KartPad-NX (already battle-tested on Switch/NVK).

```bash
# Get the revision from KartPad-NX's DawnOptions.cmake comment, e.g.:
git clone https://github.com/encounter/dawn ref/dawn
cd ref/dawn
git checkout 80ee0043018a51532ea0fa2e77496cc66634157e
git submodule update --init --recursive third_party/abseil-cpp
```

If you have KartPad-NX checked out, you can copy its `ref/dawn` directly:
```powershell
robocopy C:\Users\Bunga\Documents\GitHub\KartPad-NX\ref\dawn ref\dawn /E /XD .git
# Then re-init the git metadata if you want patch idempotency checks:
git -C ref/dawn init && git -C ref/dawn add -A && git -C ref/dawn commit -m "import"
```

## ref/SDL — SDL3 with Switch external-graphics patch

Same revision as KartPad-NX.

```bash
git clone https://github.com/libsdl-org/SDL ref/SDL
# Check KartPad-NX's sdl-switch-external-graphics.patch header for the revision.
```

Or copy from KartPad-NX:
```powershell
robocopy C:\Users\Bunga\Documents\GitHub\KartPad-NX\ref\SDL ref\SDL /E /XD .git
```

## Docker image

Use the same `kartpad-dawn` Docker image from KartPad-NX — it already has:
- devkitPro + devkitA64 (GCC aarch64-none-elf)
- LLVM/Clang 19
- Ninja, CMake 3.25+
- devkitPro's `switch-mesa` package (EGL/GLES only — **no Vulkan**, see below)

No new Docker image needed for this part.

## Mesa/NVK — the actual Vulkan implementation

Switch has no vendor Vulkan driver for homebrew to link against, and devkitPro's
own `switch-mesa` package is EGL/GLES-only (confirmed in-container: it installs
`libEGL.a`/`libGLESv1_CM.a`/`libGLESv2.a`/`libglapi.a`, no `libvulkan.a`). The
render path this project uses (`docs/PORTING-NOTES.md`'s Aurora GX → Dawn →
**Vulkan** → NVK chain) needs Mesa's NVK driver specifically — a from-scratch
Vulkan driver for the Tegra X1 that Nintendo Switch homebrew uses, part of it
(NAK, its shader-compiler backend) written in Rust. Rust has no
`aarch64-none-elf`/Horizon target (the same gap `nod` hit — see
`switch/src/nod/`), so building NVK needs its own Rust-enabled cross toolchain
that cross-compiles NAK for `aarch64-unknown-linux-gnu` and bridges the ABI
mismatch at link time. This is **not** part of the `kartpad-dawn` image or of
`builder/build-graphics.sh` — it is its own separate build, and it is
substantial (Meson + a from-source Rust/LLVM pipeline).

KartPad-NX already has this working: `switch/overlays/mesa-switch/` there
builds it via its own `devkitpro-mesa-rust` Docker image
(`switch/overlays/mesa-switch/Docker.rust` + `build-switch.sh`), and — on this
dev machine — the result is already built at
`C:\Users\Bunga\Documents\GitHub\KartPad-NX\ref\mesa-switch-main\builddir-switch`
(confirmed present: `libnvk.a`, `libvulkan.a`, and the rest of the NVK/NAK/NIL
archive set, ~436 MB total). melee-nx reuses that prebuilt tree directly rather
than reproducing the whole Rust/Meson pipeline a second time:

```bash
# Bind-mount KartPad-NX's already-built Mesa/NVK tree read-only, alongside
# this repo, when running any builder/*.sh script in the kartpad-dawn container:
docker run --rm \
  -v /path/to/melee-nx:/project \
  -v "/c/Users/Bunga/Documents/GitHub/KartPad-NX/ref/mesa-switch-main:/mesa-nvk:ro" \
  -w /project kartpad-dawn:latest bash builder/build-melee.sh all
```

`switch/cmake/Graphics.cmake` requires `MESA_NVK_ROOT` (defaults to `/mesa-nvk`,
overridable via the `MESA_NVK_ROOT` env var read by `builder/build-melee.sh`)
and fails configure with a clear message if it doesn't look like a built Mesa
checkout. This is a dev-machine-specific shortcut — a machine without
KartPad-NX checked out needs its own Mesa/NVK build via
`switch/overlays/mesa-switch/build-switch.sh`-equivalent (or a copy of that
tree), not a hard requirement on KartPad-NX's existence in general.
