# Building melee-nx

Everything builds inside one Docker container, so the only things you install on
your own machine are Docker and git.

> **Read this first:** one dependency — the Mesa/NVK Vulkan driver — is **not**
> handled by these scripts and is a substantial separate build. There is no way
> around it today. [Mesa/NVK](#mesanvk) explains why and what your options are.
> Everything else is two commands.

---

## 1. What you need

| | |
|---|---|
| **Host** | Linux, macOS, or Windows with Git Bash (ships with [Git for Windows](https://git-scm.com/download/win)) |
| **Docker** | [Docker Desktop](https://www.docker.com/products/docker-desktop/) or `docker.io` |
| **Disk** | ~40 GB. Dawn alone is large, and the Mesa/NVK tree is ~450 MB built |
| **Time** | First full build is 1–3 hours depending on cores. Later builds are minutes |
| **A Switch** | Running [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere) |
| **A disc image** | Your own Melee dump: NTSC-U 1.02, `GALE01`, SHA-1 `d4e70c064cc714ba8400a849cf299dbd1aa326fc` |

No game code or assets are distributed here. You supply your own disc image.

---

## 2. Quick start

```bash
git clone https://github.com/KawaiiBunga/Melee-NX melee-nx
cd melee-nx
bash builder/fetch-deps.sh
```

Then get a Mesa/NVK tree (see [below](#mesanvk)) and point at it:

```bash
export MESA_NVK_ROOT="/path/to/mesa-switch-main"
```

Then build:

```bash
bash builder/docker.sh image            # build the container (once, ~10 min)
bash builder/docker.sh graphics all     # Dawn + SDL3 (once, slow)
bash builder/docker.sh melee all        # apply game patches, configure, build
```

The result is `build/switch/melee.nro`.

To use more cores: `export MELEE_BUILD_JOBS=8`.

---

## 3. Mesa/NVK

**This is the one hard part.** Read this section rather than skipping it.

melee-nx renders through **Aurora GX → Dawn (WebGPU) → Vulkan → NVK → libnx
NWindow**. Nintendo ships no Vulkan driver that homebrew can link against, and
devkitPro's own `switch-mesa` package is EGL/GLES-only — it installs `libEGL.a`
and `libGLESv2.a` but no `libvulkan.a`. So the Vulkan implementation has to be
[Mesa's NVK](https://docs.mesa3d.org/drivers/nvk.html), a from-scratch driver
for the Tegra X1, cross-compiled for Switch.

The catch is that NVK's shader compiler (NAK) is written in Rust, and Rust has
no `aarch64-none-elf` / Horizon target. Building it needs its own Rust-enabled
cross toolchain that compiles NAK for `aarch64-unknown-linux-gnu` and bridges
the ABI mismatch at link time. That is a separate Meson + Rust pipeline, not
something the build container here does.

### What melee-nx needs from it

Point `MESA_NVK_ROOT` at a Mesa checkout that has the Switch/NVK overlay applied
**and has been built**. `builder/docker.sh` checks for exactly two things:

- `$MESA_NVK_ROOT/src/nouveau/vulkan/rust_switch_stubs.c`
- `$MESA_NVK_ROOT/builddir-switch/**/*.a` — the built archive set (`libnvk.a`,
  `libvulkan.a` and friends, roughly 450 MB)

### How to get one

The working build lives in the sibling project
[KartPad-NX](https://github.com/KawaiiBunga/Kartpad-NX), under
`switch/overlays/mesa-switch/`:

```bash
git clone https://github.com/KawaiiBunga/Kartpad-NX
cd Kartpad-NX/switch/overlays/mesa-switch
cat README.md              # read this; it builds its own Rust image first
bash build-switch.sh
```

That produces `ref/mesa-switch-main/builddir-switch/` inside the KartPad-NX
checkout, which is what you export:

```bash
export MESA_NVK_ROOT=/path/to/Kartpad-NX/ref/mesa-switch-main
```

If you already have KartPad-NX checked out and built, you are done — just export
the path; nothing needs to be rebuilt or copied.

Quote the path if it contains spaces. Point to the complete built tree, not an
old checkout location left behind after moving the project. The wrapper checks
for `src/nouveau/vulkan/rust_switch_stubs.c` and `builddir-switch/`; linking also
requires the archives inside that build directory.

**Honest status:** vendoring this into melee-nx so it builds with one command is
open work. Right now it is a cross-repo dependency, and that is the main thing
standing between this project and a genuine one-command build.

---

## 4. What each step does

| Command | What happens |
|---|---|
| `fetch-deps.sh` | Clones `ref/melee-pc`, `ref/dawn`, `ref/SDL`, `ref/SDL-dusklight` at pinned revisions. Safe to re-run; existing trees are left alone |
| `docker.sh image` | Builds the container from `switch/docker/Dockerfile`: devkitA64 GCC + LLVM Clang 19 |
| `docker.sh graphics prepare` | Applies Dawn, SDL and Aurora patches. Idempotent — patches are reverse-checked first |
| `docker.sh graphics all` | `prepare`, then builds Dawn and SDL3. The slow one |
| `docker.sh melee configure` | CMake configure for the NRO |
| `docker.sh melee build` | Compiles and links `build/switch/melee.nro` |
| `docker.sh melee all` | Applies game patches, then configures and builds the NRO |
| `docker.sh shell` | Drops you into the container with everything mounted |

After the first full build you normally only need:

```bash
bash builder/docker.sh melee build
```

Re-run `graphics prepare` and `melee all` after pulling patch changes. Rebuild
SDL or Dawn as well when patches change their sources. For a source audit, run
`python builder/verify-patches.py` (Python 3 required on the host). The September
21 patch set covers the current GX CPU optimizations and previously uncaptured
card/texture/movie fixes; there is no need to bypass a failed patch check.

### Why two compilers

The game's C needs GCC's
`__attribute__((scalar_storage_order("big-endian")))` to read GameCube data
structures in place — Clang does not implement it. Dawn's C++ needs a compiler
new enough for C++20/23 against devkitA64's libstdc++-15 headers, which
devkitPro's bundled Clang is not. So: GCC for C, Clang 19 for C++. See
`switch/cmake/SwitchGCC.cmake`.

### SDL variants

`MELEE_SDL_VARIANT` picks which SDL tree is used:

| Value | Tree | Notes |
|---|---|---|
| `dusklight` (default) | `ref/SDL-dusklight` — SDL 3.4.10 + Dusklight Switch backend | Current default; used by the September 21 hardware-tested build |
| `legacy` | `ref/SDL` — SDL 3.4.4 | Comparison/fallback tree |

It must match between `graphics` and `melee` stages, so set it once:

```bash
export MELEE_SDL_VARIANT=legacy
bash builder/docker.sh graphics sdl
bash builder/docker.sh melee configure
bash builder/docker.sh melee build
```

Each variant has its own build directory, so switching never links a library
against another variant's headers.

---

## 5. Installing on the console

Copy the NRO and your disc image to the SD card:

```
sdmc:/switch/melee-nx/melee.nro
sdmc:/switch/melee-nx/GALE01.iso
```

Launch it from the Homebrew Menu. On first run it extracts the game data from
the disc image into `sdmc:/switch/melee-nx/files/`, which takes a few minutes.

Two logs are written next to the NRO and are the first thing to look at when
something goes wrong:

- `melee-nx-boot.log` — early startup, one line per stage
- `melee-nx-runtime.log` — everything after that, including the build ID, source
  revisions, patch-set hash, and `PERF` frame-timing lines

### Deploying over FTP

If you run an FTP server on the console (e.g. sys-ftpd), you can skip the SD
card shuffle. **Always download the file back and compare hashes** — attributing
a hardware result to the wrong binary costs far more time than the check:

```bash
curl --fail --upload-file build/switch/melee.nro \
  ftp://YOUR_SWITCH_IP:5000/sdmc:/switch/melee-nx/melee.nro
curl --fail --output /tmp/deployed.nro \
  ftp://YOUR_SWITCH_IP:5000/sdmc:/switch/melee-nx/melee.nro
sha256sum build/switch/melee.nro /tmp/deployed.nro
```

---

## 6. Current state

Work in progress, but playable and improving.

- It boots, reaches the main menu, and plays.
- Gameplay runs ~36 fps median (up to 60) at 720p on stock clocks with the
  default `dusklight` variant. A locked 60 is the remaining goal.
- Audio works but is latent.
- Rendering can briefly drop geometry while a shader pipeline is still compiling.
- Match/stage loads can still hitch for a second or two while streaming assets.

`PERF` lines in `melee-nx-runtime.log` report per-second frame timing split into
`sim` (game logic), `submit` and `wait`; `STAGES` lines break the frame down
further (`fifo`, `texture`, `alarms`, …). `PIPELINES` lines report how many draws
were skipped for a not-yet-compiled pipeline. Optional thread-affinity
experiments are off by default and selected through a `perf.cfg` next to the NRO
(`affinity main|split|baseline`) for matched hardware comparisons.

---

## 7. Troubleshooting

**`MESA_NVK_ROOT is not set`** — see [Mesa/NVK](#mesanvk). There is no default.

**`docker not found on PATH`** — on Windows, run these commands from **Git
Bash**, not PowerShell or CMD, and make sure Docker Desktop is running.

**`ERROR: patch conflicts with ...`** — a reference tree under `ref/` has drifted
from the revision a patch was written against, or has local edits. Check
`git -C ref/<tree> status`. Do not force-apply; the patches are reverse-checked
first precisely so a conflict is visible rather than silently doubled.

**A build fails with a syntax error in `builder/*.sh` that you cannot reproduce**
— you edited the script while the container was running it. Bash reads a script
by byte offset as it executes, so an edit mid-run derails the rest. Re-run.

**Out of disk** — `build/` holds separate Dawn and SDL build trees and the
unstripped ELF is ~220 MB. `rm -rf build/` is always safe; it only costs time.

---

## 8. Layout

```
builder/
  fetch-deps.sh        clone pinned reference trees into ref/
  docker.sh            run a build stage in the container
  build-graphics.sh    patch trees, build Dawn + SDL3
  build-melee.sh       configure and build the NRO
switch/
  docker/Dockerfile    the build image
  CMakeLists.txt       top-level Switch build
  cmake/               toolchain, Dawn options, Dawn/SDL/Aurora imports
  patches/             every change made to the reference trees (see its README)
  src/                 NRO entry point, libnx integration, SQLite VFS, disc gate
ref/                   upstream sources — gitignored, never committed
build/                 build trees and output — gitignored
```

Exact dependency revisions and the deployment workflow are in
[docs/DEPS.md](docs/DEPS.md). Per-patch rationale is in
[switch/patches/README.md](switch/patches/README.md).
