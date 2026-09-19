#!/usr/bin/env bash
# Run a melee-nx build stage inside the build container.
#
#   builder/docker.sh image                 # build the container image
#   builder/docker.sh graphics prepare      # builder/build-graphics.sh prepare
#   builder/docker.sh graphics all          # Dawn + SDL3 (slow, once)
#   builder/docker.sh melee configure
#   builder/docker.sh melee build
#   builder/docker.sh shell                 # interactive shell in the container
#
# Everything the container needs is mounted here, so no build step has to know
# about host paths:
#   /project    this checkout
#   /mesa-nvk   a built Mesa/NVK Switch tree (read-only) — see BUILDING.md
#
# Environment:
#   MESA_NVK_ROOT        host path to the built Mesa/NVK tree. Required for the
#                        melee stages. See BUILDING.md for how to produce one.
#   MELEE_DOCKER_IMAGE   image to use (default: melee-nx-build). Set this to
#                        kartpad-dawn to reuse KartPad-NX's identical image.
#   MELEE_BUILD_JOBS     parallel compile jobs (default 4).
#   MELEE_SDL_VARIANT    dusklight (default) or legacy.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/.." && pwd)"

image="${MELEE_DOCKER_IMAGE:-melee-nx-build}"
jobs="${MELEE_BUILD_JOBS:-4}"
sdl_variant="${MELEE_SDL_VARIANT:-dusklight}"

usage() {
  sed -n '2,20p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
  exit "${1:-64}"
}

command -v docker >/dev/null 2>&1 || {
  echo "docker not found on PATH. Install Docker Desktop (Windows/macOS) or docker.io (Linux)." >&2
  exit 69
}

build_image() {
  docker build -t "$image" "$repo/switch/docker"
  echo "Built image: $image"
}

have_image() { docker image inspect "$image" >/dev/null 2>&1; }

# Mesa/NVK is Switch's only usable Vulkan implementation and is a separate,
# substantial build (Meson plus a Rust cross toolchain for NAK). It is not part
# of this image and not vendored here; BUILDING.md explains how to get one.
resolve_mesa() {
  local root="${MESA_NVK_ROOT:-}"
  if [[ -z "$root" ]]; then
    cat >&2 <<'MSG'
MESA_NVK_ROOT is not set.

melee-nx renders through Aurora GX -> Dawn -> Vulkan -> NVK, and Switch has no
vendor Vulkan driver for homebrew. You need a built Mesa tree carrying the
Switch/NVK overlay. See BUILDING.md, section "Mesa/NVK".

  export MESA_NVK_ROOT=/path/to/mesa-switch-main
MSG
    exit 66
  fi
  [[ -d "$root" ]] || { echo "MESA_NVK_ROOT does not exist: $root" >&2; exit 66; }
  if [[ ! -f "$root/src/nouveau/vulkan/rust_switch_stubs.c" ]]; then
    echo "MESA_NVK_ROOT does not look like a Mesa checkout with the Switch/NVK overlay: $root" >&2
    echo "Expected \$MESA_NVK_ROOT/src/nouveau/vulkan/rust_switch_stubs.c" >&2
    exit 66
  fi
  if ! compgen -G "$root/builddir-switch/*" >/dev/null 2>&1; then
    echo "No builddir-switch/ under $root -- the Mesa tree is present but not built." >&2
    echo "See BUILDING.md, section \"Mesa/NVK\"." >&2
    exit 66
  fi
  printf '%s' "$root"
}

# Docker Desktop on Windows is reached through Git Bash, which rewrites
# arguments that look like absolute POSIX paths; MSYS_NO_PATHCONV stops it
# mangling the container-side halves of -v and -w.
run_in_container() {
  local mesa; mesa="$(resolve_mesa)"
  have_image || { echo "Image '$image' not found; building it first."; build_image; }
  # -t only when stdout really is a terminal, so CI and scripted runs work too.
  local tty=(); [[ -t 1 ]] && tty=(-t)
  MSYS_NO_PATHCONV=1 docker run --rm -i "${tty[@]}" \
    -v "$repo:/project" \
    -v "$mesa:/mesa-nvk:ro" \
    -w /project \
    -e MELEE_BUILD_JOBS="$jobs" \
    -e MELEE_SDL_VARIANT="$sdl_variant" \
    -e MESA_NVK_ROOT=/mesa-nvk \
    "$image" "$@"
}

stage="${1:-}"
case "$stage" in
  image) build_image ;;
  graphics)
    shift
    run_in_container bash builder/build-graphics.sh "${1:-all}"
    ;;
  melee)
    shift
    run_in_container bash builder/build-melee.sh "${1:-all}"
    ;;
  shell) run_in_container bash ;;
  ""|-h|--help|help) usage 0 ;;
  *) echo "unknown stage: $stage" >&2; usage ;;
esac
