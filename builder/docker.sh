#!/usr/bin/env bash
# Container entry point. See BUILDING.md and switch/docker/README.md.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/.." && pwd)"

image="${MELEE_DOCKER_IMAGE:-melee-nx-build:latest}"
jobs="${MELEE_BUILD_JOBS:-4}"
sdl_variant="${MELEE_SDL_VARIANT:-dusklight}"
# Explicit tags also work with Docker daemons that reject tagless image inspect.
[[ "${image##*/}" == *:* || "$image" == *@* ]] || image+=:latest

usage() {
  cat <<'HELP'
Usage: bash builder/docker.sh COMMAND [STAGE]

  image                         Build the reusable GCC/Clang toolchain image
  graphics [prepare|dawn|sdl|aurora|all]
  melee [patch|configure|build|all]
  shell                         Open a container shell

Environment:
  MELEE_DOCKER_IMAGE             Toolchain image (default: melee-nx-build)
  MELEE_BUILD_JOBS              Parallel compile jobs (default: 4)
  MELEE_SDL_VARIANT              dusklight (default) or legacy
  MELEE_NX_CMAKE_ARGS            Extra CMake arguments for `melee configure`

See BUILDING.md for setup.
HELP
}

stage="${1:-help}"
case "$stage" in
  -h|--help|help) usage; exit 0 ;;
  image|graphics|melee|shell) ;;
  *) echo "Unknown command: $stage" >&2; usage >&2; exit 64 ;;
esac

command -v docker >/dev/null 2>&1 || {
  echo "docker not found on PATH. Install Docker and start its daemon." >&2
  exit 69
}

build_image() {
  docker build -t "$image" "$repo/switch/docker"
  echo "Built image: $image"
}

ensure_image() {
  if ! docker image inspect "$image" >/dev/null 2>&1; then
    echo "Image '$image' not found; building it first."
    build_image
  fi
}

run_in_container() {
  local tty=()
  ensure_image
  [[ -t 1 ]] && tty=(-t)
  # Git Bash must leave the container-side paths in -v and -w intact.
  MSYS_NO_PATHCONV=1 docker run --rm -i "${tty[@]}" \
    -v "$repo:/project" -w /project \
    -e MELEE_BUILD_JOBS="$jobs" \
    -e MELEE_SDL_VARIANT="$sdl_variant" \
    -e MELEE_NX_CMAKE_ARGS="${MELEE_NX_CMAKE_ARGS:-}" \
    "$image" "$@"
}

case "$stage" in
  image) build_image ;;
  graphics) run_in_container bash builder/build-graphics.sh "${2:-all}" ;;
  melee) run_in_container bash builder/build-melee.sh "${2:-all}" ;;
  shell) run_in_container bash ;;
esac
