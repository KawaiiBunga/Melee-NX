#!/usr/bin/env bash
# Build Dawn and SDL; Aurora is built with the game. See BUILDING.md.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/.." && pwd)"
source "$here/patch-common.sh"
stage="${1:-all}"
jobs="${MELEE_BUILD_JOBS:-4}"
case "$stage" in prepare|dawn|sdl|aurora|all) ;;
  *) echo "usage: $0 [prepare|dawn|sdl|aurora|all]" >&2; exit 64;; esac
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo "MELEE_BUILD_JOBS must be positive" >&2; exit 64; }

dawn="$repo/ref/dawn"
aurora="$repo/ref/melee-pc/extern/aurora"
dawn_build="$repo/build/dawn-switch"

# Keep separate SDL source/build pairs to avoid mixing libraries and headers.
sdl_variant="${MELEE_SDL_VARIANT:-dusklight}"
case "$sdl_variant" in
  dusklight) sdl="$repo/ref/SDL-dusklight"; sdl_build="$repo/build/sdl-switch-dusklight" ;;
  legacy)    sdl="$repo/ref/SDL";           sdl_build="$repo/build/sdl-switch" ;;
  *) echo "MELEE_SDL_VARIANT must be dusklight or legacy" >&2; exit 64 ;;
esac

require_file() { [[ -f "$1" ]] || { echo "Required source missing: $1" >&2; exit 66; }; }

# The SDL patches form a stack: reverse-check only the top layer.
# --recount compensates for incorrect hunk counts in the donor patch.
apply_sdl_stack() {
  local tree="$1"; shift
  local top="${!#}"
  require_file "$top"
  if git_apply_tree "$tree" --recount --ignore-space-change --reverse --check "$top" >/dev/null 2>&1; then
    printf 'Already applied: SDL patch stack (%s)\n' "${top##*/}"
    return
  fi
  local patch_file
  for patch_file in "$@"; do
    require_file "$patch_file"
    git_apply_tree "$tree" --recount --ignore-space-change --check "$patch_file"
    git_apply_tree "$tree" --recount --ignore-space-change "$patch_file"
    printf 'Applied: %s\n' "${patch_file##*/}"
  done
}

apply_patch_once() {
  local tree="$1" patch_file="$2"
  require_file "$patch_file"
  if git_apply_tree "$tree" --ignore-space-change --reverse --check "$patch_file" >/dev/null 2>&1; then
    printf 'Already applied: %s\n' "${patch_file##*/}"
  elif git_apply_tree "$tree" --ignore-space-change --check "$patch_file"; then
    git_apply_tree "$tree" --ignore-space-change "$patch_file"
    printf 'Applied: %s\n' "${patch_file##*/}"
  else
    echo "ERROR: patch conflicts with $tree: $patch_file" >&2
    exit 65
  fi
}

prepare() {
  require_file "$dawn/CMakeLists.txt"
  require_file "$dawn/third_party/abseil-cpp/absl/base/config.h"
  require_file "$aurora/CMakeLists.txt"
  if [[ "$sdl_variant" == dusklight ]]; then
    require_file "$sdl/CMakeLists.txt"
  else
    require_file "$sdl/src/video/switch/SDL_switchvideo.c"
  fi

  apply_patch_once "$dawn"                        "$repo/switch/patches/dawn-switch-libnx.patch"
  apply_patch_once "$dawn/third_party/abseil-cpp" "$repo/switch/patches/dawn-abseil-switch.patch"
  apply_patch_once "$dawn"                        "$repo/switch/patches/dawn-switch-surface.patch"
  apply_patch_once "$dawn"                        "$repo/switch/patches/dawn-switch-renderdoc.patch"
  if [[ "$sdl_variant" == dusklight ]]; then
    apply_sdl_stack "$sdl" "$repo/switch/patches/sdl-dusklight-switch.patch" \
                           "$repo/switch/patches/sdl-dusklight-melee-nx.patch"
  else
    apply_patch_once "$sdl"                       "$repo/switch/patches/sdl-switch-external-graphics.patch"
  fi
  apply_patch_once "$aurora"                      "$repo/switch/patches/aurora-switch-surface.patch"
  apply_patch_once "$aurora"                      "$repo/switch/patches/aurora-switch-dawn-backends.patch"
  apply_patch_once "$aurora"                      "$repo/switch/patches/aurora-switch-no-backtrace.patch"
  apply_patch_once "$aurora"                      "$repo/switch/patches/aurora-switch-no-mmap.patch"
  apply_patch_once "$aurora"                      "$repo/switch/patches/aurora-switch-mem1-window.patch"
  apply_patch_once "$aurora"                      "$repo/switch/patches/aurora-switch-encoder-state-cache.patch"
  # File-disjoint patches include the recovered build's complete source delta.
  apply_patch_once "$aurora"                      "$repo/switch/patches/aurora-switch-cache-recovery.patch"
  apply_patch_once "$aurora"                      "$repo/switch/patches/aurora-switch-blob-cache-batch.patch"
  apply_patch_once "$aurora"                      "$repo/switch/patches/aurora-switch-texture-telemetry.patch"
  apply_patch_once "$aurora"                      "$repo/switch/patches/aurora-switch-platform-compat.patch"
  apply_patch_once "$aurora"                      "$repo/switch/patches/aurora-switch-perf-imgui.patch"
  apply_patch_once "$aurora"                      "$repo/switch/patches/aurora-switch-thread-sweep.patch"
  apply_patch_once "$aurora"                      "$repo/switch/patches/aurora-switch-gx-cpu.patch"
  apply_patch_once "$aurora"                      "$repo/switch/patches/aurora-switch-runtime-fixes.patch"
  apply_patch_once "$aurora"                      "$repo/switch/patches/aurora-switch-io-atomic.patch"
  apply_patch_once "$aurora"                      "$repo/switch/patches/aurora-switch-pad-trigger-bind.patch"
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
echo "build-graphics.sh [$stage] done (SDL variant: $sdl_variant)."
