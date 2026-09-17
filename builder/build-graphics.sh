#!/usr/bin/env bash
# Build Switch graphics dependencies: Dawn (Vulkan/NVK), SDL3, Aurora.
# Run inside the melee-nx-dawn Docker image (same as kartpad-dawn) with
# this checkout mounted at /project.
#
#   builder/build-graphics.sh [prepare|dawn|sdl|aurora|all]
#
# Prereqs: ref/dawn, ref/SDL, ref/melee-pc/extern/aurora must exist.
# See docs/DEPS.md for exact revisions and clone commands.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/.." && pwd)"
stage="${1:-all}"
jobs="${MELEE_BUILD_JOBS:-4}"
case "$stage" in prepare|dawn|sdl|aurora|all) ;;
  *) echo "usage: $0 [prepare|dawn|sdl|aurora|all]" >&2; exit 64;; esac
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo "MELEE_BUILD_JOBS must be positive" >&2; exit 64; }

dawn="$repo/ref/dawn"
sdl="$repo/ref/SDL"
aurora="$repo/ref/melee-pc/extern/aurora"
dawn_build="$repo/build/dawn-switch"
sdl_build="$repo/build/sdl-switch"

require_file() { [[ -f "$1" ]] || { echo "Required source missing: $1" >&2; exit 66; }; }

apply_patch_once() {
  local tree="$1" patch_file="$2"
  require_file "$patch_file"
  if git -C "$tree" apply --ignore-space-change --reverse --check "$patch_file" >/dev/null 2>&1; then
    printf 'Already applied: %s\n' "${patch_file##*/}"
  elif git -C "$tree" apply --ignore-space-change --check "$patch_file"; then
    git -C "$tree" apply --ignore-space-change "$patch_file"
    printf 'Applied: %s\n' "${patch_file##*/}"
  else
    echo "ERROR: patch conflicts with $tree: $patch_file" >&2
    exit 65
  fi
}

prepare() {
  require_file "$dawn/CMakeLists.txt"
  require_file "$dawn/third_party/abseil-cpp/absl/base/config.h"
  require_file "$sdl/src/video/switch/SDL_switchvideo.c"
  require_file "$aurora/CMakeLists.txt"

  apply_patch_once "$dawn"                        "$repo/switch/patches/dawn-switch-libnx.patch"
  apply_patch_once "$dawn/third_party/abseil-cpp" "$repo/switch/patches/dawn-abseil-switch.patch"
  apply_patch_once "$dawn"                        "$repo/switch/patches/dawn-switch-surface.patch"
  apply_patch_once "$dawn"                        "$repo/switch/patches/dawn-switch-renderdoc.patch"
  apply_patch_once "$sdl"                         "$repo/switch/patches/sdl-switch-external-graphics.patch"
  apply_patch_once "$aurora"                      "$repo/switch/patches/aurora-switch-surface.patch"
  apply_patch_once "$aurora"                      "$repo/switch/patches/aurora-switch-dawn-backends.patch"
  apply_patch_once "$aurora"                      "$repo/switch/patches/aurora-switch-status-compat.patch"
  apply_patch_once "$aurora"                      "$repo/switch/patches/aurora-switch-no-backtrace.patch"
  apply_patch_once "$aurora"                      "$repo/switch/patches/aurora-switch-no-mmap.patch"
}

build_dawn() {
  cmake -S "$dawn" -B "$dawn_build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$repo/switch/cmake/SwitchGCC.cmake" \
    -C "$repo/switch/cmake/DawnOptions.cmake"
  cmake --build "$dawn_build" --target webgpu_dawn --parallel "$jobs"
}

build_sdl() {
  cmake -S "$sdl" -B "$sdl_build" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$DEVKITPRO/cmake/Switch.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TESTS=OFF -DSDL_TEST_LIBRARY=OFF \
    -DSDL_EXAMPLES=OFF -DSDL_SWITCH_EXTERNAL_GRAPHICS=ON \
    -DSDL_OPENGL=OFF -DSDL_OPENGLES=OFF -DSDL_RENDER_GPU=OFF
  cmake --build "$sdl_build" --target SDL3-static --parallel "$jobs"
}

case "$stage" in
  prepare) prepare ;;
  dawn)    build_dawn ;;
  sdl)     build_sdl ;;
  aurora)  echo "Aurora is built as part of build-melee.sh (add_subdirectory)." ;;
  all)     prepare; build_dawn; build_sdl ;;
esac
echo "build-graphics.sh [$stage] done."
