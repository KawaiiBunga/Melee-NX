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
stage="${1:-all}"
jobs="${MELEE_BUILD_JOBS:-4}"
case "$stage" in patch|configure|build|all) ;;
  *) echo "usage: $0 [patch|configure|build|all]" >&2; exit 64;; esac

melee_pc="$repo/ref/melee-pc"
build_dir="$repo/build/switch"

require_file() { [[ -f "$1" ]] || { echo "Required file missing: $1" >&2; exit 66; }; }

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

do_patch() {
  require_file "$melee_pc/CMakeLists.txt"
  local p="$repo/switch/patches/melee-switch-gcc-compat.patch"
  if [[ -f "$p" ]]; then
    apply_patch_once "$melee_pc" "$p"
  else
    echo "WARNING: melee-switch-gcc-compat.patch not yet created — skipping patch step."
    echo "         This patch removes -no-pie/-Ttext-segment and adds Switch path overrides."
    echo "         Build will likely fail until it exists. See docs/PORTING-NOTES.md."
  fi
}

do_configure() {
  require_file "$repo/build/dawn-switch/src/dawn/native/libwebgpu_dawn.a"
  require_file "$repo/build/sdl-switch/libSDL3.a"
  cmake -S "$repo/switch" -B "$build_dir" -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE="$repo/switch/cmake/SwitchGCC.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    "-DCMAKE_EXE_LINKER_FLAGS=-L$DEVKITPRO/libnx/lib -L$DEVKITPRO/portlibs/switch/lib -specs=$DEVKITPRO/libnx/switch.specs -Wl,--wrap=pthread_create -Wl,--wrap=exit" \
    -DMELEE_DAWN_SOURCE="$repo/ref/dawn" \
    -DMELEE_DAWN_BUILD="$repo/build/dawn-switch" \
    -DMELEE_SDL_SOURCE="$repo/ref/SDL" \
    -DMELEE_SDL_BUILD="$repo/build/sdl-switch"
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
echo "build-melee.sh [$stage] done."
