# Dependency acquisition

> **For the end-user build guide, see [../BUILDING.md](../BUILDING.md).** That
> file is the tracked, user-facing document; this one records exact revisions,
> provenance and the dev-machine shortcuts. `builder/fetch-deps.sh` automates
> everything below except Mesa/NVK.


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

The currently built fallback tree has SDL headers **3.4.4** and uses the
KartPad-NX external-graphics patch. The requested Dusklight migration targets a
separate pinned **SDL release-3.4.10** tree plus Dusklight patch SHA-256
`61327a8d285880bf4dbf08a7071694f761aa208d00ac6d73160f531697409fcd`.
Do not overwrite `ref/SDL`; acquire the migration candidate as
`ref/SDL-dusklight` so the known-build fallback remains available.

```bash
git clone https://github.com/libsdl-org/SDL ref/SDL
# Check KartPad-NX's sdl-switch-external-graphics.patch header for the revision.
```

Or copy from KartPad-NX:
```powershell
robocopy C:\Users\Bunga\Documents\GitHub\KartPad-NX\ref\SDL ref\SDL /E /XD .git
```

## Docker image

`switch/docker/Dockerfile` now defines the build image in this repo:
`bash builder/docker.sh image` builds it as `melee-nx-build`. It is the same
recipe as KartPad-NX's `kartpad-dawn`, so either works — set
`MELEE_DOCKER_IMAGE=kartpad-dawn` to reuse an existing one.

`builder/docker.sh` wraps the `docker run` invocation with both bind mounts, so
no build command needs to know host paths:

```bash
export MESA_NVK_ROOT=/path/to/mesa-switch-main
bash builder/docker.sh graphics prepare
bash builder/docker.sh melee configure
bash builder/docker.sh melee build
```

Historical note — use the same `kartpad-dawn` Docker image from KartPad-NX — it already has:
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
  -w /project kartpad-dawn:latest bash builder/build-melee.sh configure
```

`switch/cmake/Graphics.cmake` requires `MESA_NVK_ROOT` (defaults to `/mesa-nvk`,
overridable via the `MESA_NVK_ROOT` env var read by `builder/build-melee.sh`)
and fails configure with a clear message if it doesn't look like a built Mesa
checkout. This is a dev-machine-specific shortcut — a machine without
KartPad-NX checked out needs its own Mesa/NVK build via
`switch/overlays/mesa-switch/build-switch.sh`-equivalent (or a copy of that
tree), not a hard requirement on KartPad-NX's existence in general.

## Current build and deployment workflow

The local `ref/melee-pc` snapshot contains layered historical edits that
overlap the old monolithic compatibility patch. For this snapshot, do not force
`build-melee.sh all` through a failed reverse check. The verified sequence is:

```bash
bash builder/build-graphics.sh prepare
bash builder/build-graphics.sh sdl      # only when the SDL source/variant changed
bash builder/build-melee.sh configure
bash builder/build-melee.sh build
```

### SDL variant

`MELEE_SDL_VARIANT` selects the SDL source and build root, and must be the same
for `build-graphics.sh` and `build-melee.sh` so the library and the headers it
is linked against match:

| Value | Source | Build root |
|---|---|---|
| `dusklight` (default) | `ref/SDL-dusklight` — pinned `release-3.4.10` | `build/sdl-switch-dusklight` |
| `legacy` | `ref/SDL` — the original 3.4.4 tree | `build/sdl-switch` |

Acquire the Dusklight tree with:

```bash
git clone --depth 1 --branch release-3.4.10 https://github.com/libsdl-org/SDL ref/SDL-dusklight
```

`build-graphics.sh prepare` then applies `sdl-dusklight-switch.patch` (Dusklight's
Switch backend, copied verbatim from
`dusklight-nx/platforms/switch/patches/sdl3-switch.patch`) followed by
`sdl-dusklight-melee-nx.patch`. The legacy tree is kept as the comparison and
fallback artifact; it is not the selected backend.

Run those commands inside `kartpad-dawn` with the project mounted at `/project`
and the prebuilt Mesa tree mounted read-only at `/mesa-nvk`, as shown above.

Deploy and verify:

```bash
curl --fail --upload-file build/switch/melee.nro \
  ftp://192.168.1.171:5000/sdmc:/switch/melee-nx/melee.nro
curl --fail --output /tmp/melee.deployed.nro \
  ftp://192.168.1.171:5000/sdmc:/switch/melee-nx/melee.nro
sha256sum build/switch/melee.nro /tmp/melee.deployed.nro
```

The 2026-09-19 afternoon deployed build is 85,896,341 bytes with SHA-256
`84d88dd602959e1abef4c6d35220b7838a1ed977cfb44b7c0a289d9afc3b1342`
(SDL variant `dusklight`). The morning build it replaced was 85,879,957 bytes,
`184480f708c5f26fd22bc115ca5319b42c9e922e67d2ec07948bdf5b7742ece5`, kept at
`scratch/perf-2026-09-19b/pre/melee.nro`.

Never edit a `builder/*.sh` script while a container is running it: bash reads
the file by byte offset as it goes, so rewriting it derails the rest of the run
(observed 2026-09-19 — the build itself completed, the wrapper then died on a
phantom syntax error).
