#!/usr/bin/env bash
# Build melee-nx NRO.
# Run inside the melee-nx-dawn Docker image with this checkout mounted at /project.
#
#   builder/build-melee.sh [patch|configure|build|all]
#
# Depends on:
#   - ref/melee-pc         (game source, cloned separately — see docs/DEPS.md)
#   - build/dawn-switch    (from build-graphics.sh)
#   - build/sdl-switch     (from build-graphics.sh)
# Produces: build/switch/melee.nro
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/.." && pwd)"
source "$here/patch-common.sh"
stage="${1:-all}"
jobs="${MELEE_BUILD_JOBS:-4}"
case "$stage" in patch|configure|build|all) ;;
  *) echo "usage: $0 [patch|configure|build|all]" >&2; exit 64;; esac

melee_pc="$repo/ref/melee-pc"
build_dir="$repo/build/switch"

# Must match build-graphics.sh: each SDL variant has its own source and build
# root, and the library has to be linked against the headers it was built from.
sdl_variant="${MELEE_SDL_VARIANT:-dusklight}"
case "$sdl_variant" in
  dusklight) sdl_source="$repo/ref/SDL-dusklight"; sdl_build="$repo/build/sdl-switch-dusklight" ;;
  legacy)    sdl_source="$repo/ref/SDL";           sdl_build="$repo/build/sdl-switch" ;;
  *) echo "MELEE_SDL_VARIANT must be dusklight or legacy" >&2; exit 64 ;;
esac

require_file() { [[ -f "$1" ]] || { echo "Required file missing: $1" >&2; exit 66; }; }

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

do_patch() {
  require_file "$melee_pc/CMakeLists.txt"
  local p="$repo/switch/patches/melee-switch-gcc-compat.patch"
  if [[ -f "$p" ]]; then
    apply_patch_once "$melee_pc" "$p"
    # Disc pointer slots resolved through MEM1's 4GB window; see src/pc/disc.h.
    apply_patch_once "$melee_pc" "$repo/switch/patches/melee-switch-disc-ptr-window.patch"
    apply_patch_once "$melee_pc" "$repo/switch/patches/melee-switch-input-worker.patch"
    apply_patch_once "$melee_pc" "$repo/switch/patches/melee-switch-perf-telemetry.patch"
  else
    echo "WARNING: melee-switch-gcc-compat.patch not yet created — skipping patch step."
    echo "         This patch removes -no-pie/-Ttext-segment and adds Switch path overrides."
    echo "         Build will likely fail until it exists. See docs/PORTING-NOTES.md."
  fi
}

do_configure() {
  require_file "$repo/build/dawn-switch/src/dawn/native/libwebgpu_dawn.a"
  require_file "$sdl_build/libSDL3.a"
  # Mesa/NVK is Switch's only real Vulkan implementation (devkitPro's own
  # switch-mesa package is EGL/GLES-only) and needs its own Rust-enabled cross
  # build -- see docs/DEPS.md. Defaults to the path this repo's docker run
  # invocations bind-mount a prebuilt tree at; override for a different layout.
  : "${MESA_NVK_ROOT:=/mesa-nvk}"
  cmake -S "$repo/switch" -B "$build_dir" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$repo/switch/cmake/SwitchGCC.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    "-DCMAKE_EXE_LINKER_FLAGS=-Wl,--wrap=pthread_create -Wl,--wrap=exit -Wl,--wrap=abort -Wl,--wrap=_exit -Wl,--wrap=__cxa_throw -Wl,--wrap=pthread_detach -Wl,--wrap=pthread_join" \
    -DMELEE_DAWN_SOURCE="$repo/ref/dawn" \
    -DMELEE_DAWN_BUILD="$repo/build/dawn-switch" \
    -DMELEE_SDL_SOURCE="$sdl_source" \
    -DMELEE_SDL_BUILD="$sdl_build" \
    -DMESA_NVK_ROOT="$MESA_NVK_ROOT"
}

do_build() {
  cmake --build "$build_dir" --parallel "$jobs"
}

case "$stage" in
  patch)     do_patch ;;
  configure) do_configure ;;
  build)     do_build ;;
  all)       do_patch; do_configure; do_build ;;
esac
echo "build-melee.sh [$stage] done (SDL variant: $sdl_variant)."
