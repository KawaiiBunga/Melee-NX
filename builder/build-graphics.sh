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
source "$here/patch-common.sh"
stage="${1:-all}"
jobs="${MELEE_BUILD_JOBS:-4}"
case "$stage" in prepare|dawn|sdl|aurora|all) ;;
  *) echo "usage: $0 [prepare|dawn|sdl|aurora|all]" >&2; exit 64;; esac
[[ "$jobs" =~ ^[1-9][0-9]*$ ]] || { echo "MELEE_BUILD_JOBS must be positive" >&2; exit 64; }

dawn="$repo/ref/dawn"
aurora="$repo/ref/melee-pc/extern/aurora"
dawn_build="$repo/build/dawn-switch"

# SDL variant. "dusklight" is pinned SDL release-3.4.10 plus Dusklight's Switch
# backend and this port's changes on top; "legacy" is the original 3.4.4 tree,
# kept as a comparison/fallback artifact (docs/PLAN-CPU-SDL-DUSKLIGHT.md P2).
# Each variant gets its own source and build root so switching between them
# never links a library against mismatched headers.
sdl_variant="${MELEE_SDL_VARIANT:-dusklight}"
case "$sdl_variant" in
  dusklight) sdl="$repo/ref/SDL-dusklight"; sdl_build="$repo/build/sdl-switch-dusklight" ;;
  legacy)    sdl="$repo/ref/SDL";           sdl_build="$repo/build/sdl-switch" ;;
  *) echo "MELEE_SDL_VARIANT must be dusklight or legacy" >&2; exit 64 ;;
esac

require_file() { [[ -f "$1" ]] || { echo "Required source missing: $1" >&2; exit 66; }; }

# The two SDL patches are one stack, not two independent patches: the Dusklight
# backend patch adds src/{video,audio,joystick}/switch, and the melee-nx patch
# edits those same new files. Once the top layer is applied the base no longer
# reverse-identifies, so reverse-check the top layer and treat the whole stack
# as done -- the same layering caveat AGENTS.md records for melee-pc. --recount
# is required: the upstream Dusklight patch's hunk line counts do not match its
# bodies.
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
  apply_patch_once "$aurora"                      "$repo/switch/patches/aurora-switch-platform-compat.patch"
  apply_patch_once "$aurora"                      "$repo/switch/patches/aurora-switch-perf-imgui.patch"
  apply_patch_once "$aurora"                      "$repo/switch/patches/aurora-switch-thread-sweep.patch"
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
