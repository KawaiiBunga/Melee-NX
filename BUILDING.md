# Building melee-nx

The build runs in Docker and produces `build/switch/melee.nro`. Host tools are
Git, Docker, and Bash; on Windows, run the commands below in Git Bash with
Docker Desktop running.

Allow roughly 40 GB of disk space and 1–3 hours for the first graphics build.
Later game builds are usually much shorter. Dependency revisions are recorded
in [docs/DEPS.md](docs/DEPS.md).

## Get the sources

```bash
git clone https://github.com/KawaiiBunga/Melee-NX melee-nx
cd melee-nx
bash builder/fetch-deps.sh
```

The fetcher clones pinned Melee, Dawn, and SDL trees into `ref/`. Existing trees
are left alone, including local edits. Aurora is vendored inside melee-pc.

## Choose the build environment

### Use an existing SDK image

An SDK image contains the compiler toolchain and the built Switch Mesa driver.
Load a saved image if needed, then select its tag:

```bash
docker load -i melee-nx-sdk.tar
export MELEE_DOCKER_IMAGE=melee-nx-sdk:latest
unset MESA_NVK_ROOT
```

No Mesa checkout is needed on this machine. The repo does not currently provide
a published SDK download; use an image you have built or received separately.

### Use a toolchain image and an external Mesa build

```bash
bash builder/docker.sh image
export MESA_NVK_ROOT="/path/to/built/mesa-switch-main"
```

`image` builds `melee-nx-build` from the recipe in this repo. Set
`MELEE_DOCKER_IMAGE` to use another compatible image. The Mesa directory is
mounted read-only at `/mesa-nvk`; it can live anywhere on the host.

See [Mesa/NVK](#mesanvk) for the required files. To package this environment for
future builds or another machine, follow the [SDK guide](switch/docker/README.md).

## Build

```bash
bash builder/docker.sh graphics all
bash builder/docker.sh melee all
```

`graphics all` applies the graphics patches, then builds Dawn and SDL.
`melee all` applies the game patches, configures CMake, and builds the NRO.
Aurora is built with the game.

After editing only the port sources, use `melee all`. If SDL changes, run
`graphics prepare` and `graphics sdl` first. Never edit a builder shell script
while a container is executing it: Bash reads the file incrementally.

| Command | Purpose |
|---|---|
| `docker.sh image` | Build the compiler toolchain image |
| `docker.sh sdk-image` | Package that toolchain with an existing Mesa build |
| `docker.sh graphics prepare` | Apply Dawn, SDL, and Aurora patches |
| `docker.sh graphics dawn` | Build Dawn |
| `docker.sh graphics sdl` | Build the selected SDL variant |
| `docker.sh melee patch` | Apply game patches |
| `docker.sh melee configure` | Configure the game build |
| `docker.sh melee build` | Compile and link the configured build |
| `docker.sh shell` | Open a shell in the selected image |

Run these through `bash builder/`, as in the examples above. Individual build
stages assume their prerequisites are already prepared.

### Settings

| Variable | Default | Purpose |
|---|---|---|
| `MELEE_DOCKER_IMAGE` | `melee-nx-build:latest` | Image used to run builds |
| `MELEE_DOCKER_SDK_IMAGE` | `melee-nx-sdk:latest` | Output tag for `sdk-image` |
| `MESA_NVK_ROOT` | Unset | Optional host Mesa tree; overrides the image's copy |
| `MELEE_BUILD_JOBS` | `4` | Parallel compile jobs |
| `MELEE_SDL_VARIANT` | `dusklight` | SDL source and build pair |

The default SDL backend is Dusklight on SDL 3.4.10. `legacy` selects the SDL
3.4.4 fallback. Keep the variant consistent between graphics and game builds:

```bash
export MELEE_BUILD_JOBS=8
export MELEE_SDL_VARIANT=legacy
bash builder/docker.sh graphics prepare
bash builder/docker.sh graphics sdl
bash builder/docker.sh melee all
```

Each SDL variant has its own build directory. GCC compiles the game C code;
Clang 19 compiles C++. The [architecture notes](docs/ARCHITECTURE.md) explain the
compiler and linker constraints.

## Mesa/NVK

The Vulkan path needs a built Mesa tree with the Switch/NVK adaptations.
devkitPro's EGL/GLES libraries do not replace it. The link inputs are:

- `src/nouveau/vulkan/rust_switch_stubs.c`, the Rust/newlib compatibility source
- The archives under `builddir-switch/`, including `libnvk.a` and `libvulkan.a`
- Any object files referenced by thin archives, at their original relative paths

An SDK image packages these inputs and verifies their SHA-256 checksums during
image creation. A host tree must provide the same layout; copying only the
`.a` files is insufficient when some are thin archives.

The existing driver build originated in KartPad-NX. Its recovered overlay is
not a complete pinned source-build recipe, so cloning that project and running
one script is not a verified way to reconstruct this driver. Use a known built
tree or an SDK snapshot. Reproducing the Mesa fork and Rust cross build from
clean sources remains separate work; see [dependency provenance](docs/DEPS.md#mesa).

## Installing on the console

Use a Nintendo Switch running [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere)
and your own Melee NTSC-U 1.02 dump (`GALE01`; SHA-1
`d4e70c064cc714ba8400a849cf299dbd1aa326fc`). Copy:

```text
sdmc:/switch/melee-nx/melee.nro
sdmc:/switch/melee-nx/GALE01.iso
```

Launch from the Homebrew Menu. Press **A** to extract to `files/`, **B** to play
from the disc image, or **+** to exit. Extraction takes several minutes.

Once extraction is complete, boot the game with the image present to capture
`disc.meta` and `disc-boot.bin`. Keep those files next to the NRO. With the
completed `files/` directory and both metadata files present, the image can be
removed from the SD card.

### FTP deployment

With an FTP server running on the console:

```bash
curl --fail --upload-file build/switch/melee.nro \
  ftp://YOUR_SWITCH_IP:5000/sdmc:/switch/melee-nx/melee.nro
curl --fail --output build/switch/deployed.nro \
  ftp://YOUR_SWITCH_IP:5000/sdmc:/switch/melee-nx/melee.nro
sha256sum build/switch/melee.nro build/switch/deployed.nro
```

The hashes must match before recording hardware results against this build.

## Troubleshooting

| Symptom | Check |
|---|---|
| Docker cannot connect | Start Docker Desktop or the Docker daemon |
| Mesa/NVK is missing | Select an SDK image or set `MESA_NVK_ROOT` to the built tree |
| Patch conflict | Inspect `git -C ref/<tree> status`; verify the pinned revision and local edits before continuing |
| Shell syntax error during a build | If the script was edited mid-run, rerun after all edits are finished |
| Missing geometry during play | Shader pipelines may still be compiling; inspect `PIPELINES` records in the runtime log |
| Startup failure | Read `melee-nx-boot.log`, then `melee-nx-runtime.log`, next to the NRO |
| Out of disk | Inspect `build/` and Docker image usage; preserve tested binaries and captures before removing outputs |

Do not force failed patch checks or reset reference trees to hide a conflict.
For source checks and patch maintenance, see [CONTRIBUTING.md](CONTRIBUTING.md).
