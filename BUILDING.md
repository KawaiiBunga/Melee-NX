# Building melee-nx

melee-nx is two NROs: the game (`melee.nro`, drawn with the native deko3d renderer) and the launcher
(`melee-nx-launcher.nro`). The game build runs in Docker; host tools are Git, Docker and Bash. On
Windows, run the commands in Git Bash with Docker Desktop running.

Allow roughly 40 GB of disk space and 1–3 hours for the first graphics build. Later game builds are
much shorter. Dependency revisions are in [docs/DEPS.md](docs/DEPS.md).

## Get the sources

```bash
git clone https://github.com/KawaiiBunga/Melee-NX melee-nx
cd melee-nx
bash builder/fetch-deps.sh
```

The fetcher clones pinned Melee, Dawn and SDL trees into `ref/`. Existing trees are left alone,
including local edits. Aurora is vendored inside melee-pc.

## Build the game

```bash
bash builder/docker.sh image
bash builder/docker.sh graphics all
bash builder/docker.sh melee all
```

`image` builds the `melee-nx-build` toolchain image (devkitA64, libnx, deko3d, Clang 19);
`MELEE_DOCKER_IMAGE` selects another compatible image. `graphics all` applies the graphics patches,
then builds Dawn (its Null backend plus Tint's GLSL writer) and SDL. `melee all` applies the game
patches, stages Aurora with the deko3d seams (`builder/integrate-deko.py`), configures CMake and
builds `build/switch-deko/melee.nro`. Aurora and the shader compiler in `switch/uam` are built with
the game.

After editing only the port sources, use `melee all`. If SDL changes, run `graphics prepare` and
`graphics sdl` first. Never edit a builder shell script while a container is executing it: Bash
reads the file incrementally.

| Command | Purpose |
|---|---|
| `docker.sh image` | Build the compiler toolchain image |
| `docker.sh graphics prepare` | Apply Dawn, SDL and Aurora patches |
| `docker.sh graphics dawn` | Build Dawn |
| `docker.sh graphics sdl` | Build the selected SDL variant |
| `docker.sh melee patch` | Apply game patches |
| `docker.sh melee configure` | Configure the game build |
| `docker.sh melee build` | Compile and link the configured build |
| `docker.sh shell` | Open a shell in the selected image |

Run these through `bash builder/`, as in the examples above. Individual stages assume their
prerequisites are already prepared.

### Settings

| Variable | Default | Purpose |
|---|---|---|
| `MELEE_DOCKER_IMAGE` | `melee-nx-build:latest` | Image used to run builds |
| `MELEE_BUILD_JOBS` | `4` | Parallel compile jobs |
| `MELEE_SDL_VARIANT` | `dusklight` | SDL source and build pair |
| `MELEE_NX_CMAKE_ARGS` | Unset | Extra CMake arguments for `melee configure` |

The default SDL backend is Dusklight on SDL 3.4.10; `legacy` selects the SDL 3.4.4 fallback. Keep
the variant the same for graphics and game builds. GCC compiles the game's C code; Clang 19 compiles
C++. The [architecture notes](docs/ARCHITECTURE.md) explain the compiler and linker constraints, and
[docs/DEKO3D.md](docs/DEKO3D.md) the renderer.
## Launcher

The launcher (`melee-nx-launcher.nro`) is a separate SDL2 app built from `launcher/`. It needs only
devkitPro with the SDL2, SDL2_ttf, SDL2_image, libcurl and zlib portlibs; it does not need Dawn or the game build. On Windows without Docker:

```powershell
powershell -ExecutionPolicy Bypass -File launcher\build-native.ps1 -DevkitPro E:\tools\devkitPro
powershell -ExecutionPolicy Bypass -File launcher\tests\run-host-tests.ps1
```

The first command writes `build\launcher\melee-nx-launcher.nro`; the second runs the host tests.
In the toolchain container, configure `launcher/` with devkitPro's `Switch.cmake`. See
[docs/LAUNCHER.md](docs/LAUNCHER.md) for the design, the game handoff and the tests.

## Installing on the console

Use a Nintendo Switch running [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere)
and your own Melee NTSC-U 1.02 dump (`GALE01`; SHA-1
`d4e70c064cc714ba8400a849cf299dbd1aa326fc`). Copy:

```text
sdmc:/switch/melee-nx/melee-nx-launcher.nro
sdmc:/switch/melee-nx/bin/melee.nro
sdmc:/switch/melee-nx/GALE01.iso
```

Open **Melee NX Launcher** from the Homebrew Menu (start it from a game so it gets full memory).
Game Files offers to extract the disc to `files/` for faster loading. After the first boot with the
disc image present, `disc.meta` and `disc-boot.bin` let the game start without it. Details:
[docs/LAUNCHER.md](docs/LAUNCHER.md#install).

### FTP deployment

With an FTP server running on the console:

```bash
curl --fail --upload-file build/switch-deko/melee.nro ftp://YOUR_SWITCH_IP:5000/switch/melee-nx/bin/melee.nro
curl --fail --upload-file build/launcher/melee-nx-launcher.nro ftp://YOUR_SWITCH_IP:5000/switch/melee-nx/melee-nx-launcher.nro
curl --fail --output build/deployed.nro ftp://YOUR_SWITCH_IP:5000/switch/melee-nx/bin/melee.nro
sha256sum build/switch-deko/melee.nro build/deployed.nro
```

The hashes must match before recording hardware results against this build.

## Troubleshooting

| Symptom | Check |
|---|---|
| Docker cannot connect | Start Docker Desktop or the Docker daemon |
| Patch conflict | Inspect `git -C ref/<tree> status`; verify the pinned revision and local edits before continuing |
| Shell syntax error during a build | If the script was edited mid-run, rerun after all edits are finished |
| Missing geometry during play | Shader pipelines may still be compiling; inspect `PIPELINES` records in the runtime log |
| Startup failure | Read `melee-nx-boot.log`, then `melee-nx-runtime.log`, next to the NRO |
| Out of disk | Inspect `build/` and Docker image usage; preserve tested binaries and captures before removing outputs |

Do not force failed patch checks or reset reference trees to hide a conflict.
For source checks and patch maintenance, see [CONTRIBUTING.md](CONTRIBUTING.md).
