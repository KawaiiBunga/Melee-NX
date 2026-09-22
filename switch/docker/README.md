# Build images

The compiler toolchain and Mesa SDK are separate layers. Neither contains
Melee source, disc images, extracted assets, or a project working directory.

| Image | Contents | External input when building Melee |
|---|---|---|
| `melee-nx-build` | devkitA64 GCC, libnx, Clang 19, CMake, Ninja, Python | Built Mesa tree via `MESA_NVK_ROOT` |
| `melee-nx-sdk` | A chosen toolchain image plus a verified Mesa snapshot | Only this checkout and its dependencies |

Both images can be used by another compatible Switch project. The repository
wrapper supplies Melee-specific mounts, environment, and build commands.

## Toolchain image

From the repo root:

```bash
bash builder/docker.sh image
```

Or build it independently of the wrapper:

```bash
docker build -t switch-toolchain:local switch/docker
export MELEE_DOCKER_IMAGE=switch-toolchain:local
```

The build context contains only the recipe. The recipe retains the existing
`devkitpro/devkita64:latest` base and LLVM 19 installation; it does not pin
every package. Save a tested image when exact compiler reuse matters.

Existing compatible images, including `kartpad-dawn:latest`, remain usable
through `MELEE_DOCKER_IMAGE`. No build script depends on that name.

## Package a Mesa SDK

Select the toolchain used with your known Mesa build, then package it:

```bash
export MELEE_DOCKER_IMAGE=melee-nx-build:latest
export MESA_NVK_ROOT="/path/to/built/mesa-switch-main"
export MELEE_DOCKER_SDK_IMAGE=melee-nx-sdk:latest
bash builder/docker.sh sdk-image
```

`sdk-image` streams a Docker build context containing only:

- The archive set selected by `Graphics.cmake`, excluding the Rust sanity check
- The object files referenced by thin archives, preserving their relative paths
- `rust_switch_stubs.c`, the ABI bridge compiled with the game
- `manifest.json`, with file sizes, hashes, and the source toolchain image ID
- `SHA256SUMS`, checked while building the image

The original tree is mounted read-only. Its objects and archives are copied
without rebuilding or repacking. Do not rebuild that Mesa tree during packaging.
Absolute or out-of-tree thin-archive members cannot be relocated safely and
are rejected. See [Mesa provenance](../../docs/DEPS.md#mesa) for the source-build
limitations of the recovered driver.

Use the resulting image without the external checkout:

```bash
export MELEE_DOCKER_IMAGE=melee-nx-sdk:latest
unset MESA_NVK_ROOT
bash builder/docker.sh graphics all
bash builder/docker.sh melee all
```

Setting `MESA_NVK_ROOT` explicitly still mounts that host tree over the SDK's
copy, so the existing workflow remains available.

## Verify and transfer

```bash
docker run --rm melee-nx-sdk:latest sh -c 'cd /mesa-nvk && sha256sum --check SHA256SUMS'
docker run --rm melee-nx-sdk:latest cat /mesa-nvk/manifest.json
docker image inspect melee-nx-sdk:latest --format '{{.Id}}'
docker save -o melee-nx-sdk.tar melee-nx-sdk:latest
```

On the receiving machine:

```bash
docker load -i melee-nx-sdk.tar
export MELEE_DOCKER_IMAGE=melee-nx-sdk:latest
unset MESA_NVK_ROOT
```

The saved image includes its toolchain layers. Building Melee still needs the
pinned reference sources described in [BUILDING.md](../../BUILDING.md).

## Container interface

`builder/docker.sh` mounts the current checkout at `/project`, sets that working
directory, and passes build jobs and SDL selection. A supplied host Mesa tree
is mounted read-only at `/mesa-nvk`; otherwise the SDK provides that directory.
Graphics preparation and an interactive shell also work with a plain toolchain
image and no Mesa mount.

The wrapper uses argument arrays for paths containing spaces and disables Git
Bash path conversion for container paths. `--help` works without contacting
Docker. Wrapper and packaging checks are in `builder/test-docker.py`.
