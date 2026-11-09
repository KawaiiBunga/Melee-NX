# Build image

`melee-nx-build` holds the toolchain: devkitA64 GCC, libnx, deko3d, Clang 19, CMake, Ninja and
Python. It has no Melee source, disc images, extracted assets or project working directory, so
another Switch project can use it too.

```bash
bash builder/docker.sh image
```

Or build it without the wrapper:

```bash
docker build -t switch-toolchain:local switch/docker
export MELEE_DOCKER_IMAGE=switch-toolchain:local
```

The recipe keeps the `devkitpro/devkita64:latest` base and LLVM 19; it does not pin every package.
Save a tested image when exact compiler reuse matters:

```bash
docker save -o melee-nx-build.tar melee-nx-build:latest
docker load -i melee-nx-build.tar
```

Existing compatible images, such as `kartpad-dawn:latest`, work through `MELEE_DOCKER_IMAGE`.

## Container interface

`builder/docker.sh` mounts the checkout at `/project`, sets it as the working directory and passes
the job count, SDL variant and extra CMake arguments. It uses argument arrays for paths with
spaces and disables Git Bash path conversion. `--help` works without Docker. Wrapper checks are in
`builder/test-docker.py`.
