#!/usr/bin/env bash
# Clone the reference source trees melee-nx builds against, at pinned revisions.
#
#   builder/fetch-deps.sh          # everything missing
#   builder/fetch-deps.sh melee-pc # just one
#
# All of these land under ref/ and are gitignored — they are upstream sources,
# never committed here. Re-running is safe: an existing tree is left alone and
# only reported.
#
# This does NOT fetch Mesa/NVK. That one is a separate, substantial build with
# its own Rust cross toolchain; BUILDING.md explains it.
set -euo pipefail
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo="$(cd "$here/.." && pwd)"
cd "$repo"

# Pinned revisions. Change these deliberately: the patches under switch/patches
# are written against these exact trees, and a drifting upstream is the usual
# reason a patch stops applying.
MELEE_PC_URL="https://github.com/999sian/melee-pc"
MELEE_PC_REV="7c9a468f4f8206780c4cd762be1da7772daaeabf"

DAWN_URL="https://github.com/encounter/dawn"
DAWN_REV="80ee0043018a51532ea0fa2e77496cc66634157e"

SDL_URL="https://github.com/libsdl-org/SDL"
SDL_LEGACY_TAG="release-3.4.4"
SDL_DUSKLIGHT_TAG="release-3.4.10"

have() { command -v "$1" >/dev/null 2>&1; }
have git || { echo "git not found on PATH." >&2; exit 69; }

skip_if_present() {
  local dir="$1"
  if [[ -d "$dir" ]]; then
    printf 'Already present, leaving alone: %s\n' "$dir"
    return 0
  fi
  return 1
}

fetch_melee_pc() {
  skip_if_present ref/melee-pc && return 0
  echo "==> ref/melee-pc ($MELEE_PC_REV)"
  git clone "$MELEE_PC_URL" ref/melee-pc
  git -C ref/melee-pc checkout --detach "$MELEE_PC_REV"
  # Aurora is vendored inside melee-pc; no separate clone.
  git -C ref/melee-pc submodule update --init --recursive
}

fetch_dawn() {
  skip_if_present ref/dawn && return 0
  echo "==> ref/dawn ($DAWN_REV)"
  git clone "$DAWN_URL" ref/dawn
  git -C ref/dawn checkout --detach "$DAWN_REV"
  # Only abseil is needed; the full submodule set is enormous and unused here.
  git -C ref/dawn submodule update --init --recursive third_party/abseil-cpp
}

fetch_sdl_legacy() {
  skip_if_present ref/SDL && return 0
  echo "==> ref/SDL ($SDL_LEGACY_TAG, comparison/fallback variant)"
  git clone --depth 1 --branch "$SDL_LEGACY_TAG" "$SDL_URL" ref/SDL
}

fetch_sdl_dusklight() {
  skip_if_present ref/SDL-dusklight && return 0
  echo "==> ref/SDL-dusklight ($SDL_DUSKLIGHT_TAG, default variant)"
  git clone --depth 1 --branch "$SDL_DUSKLIGHT_TAG" "$SDL_URL" ref/SDL-dusklight
}

mkdir -p ref

case "${1:-all}" in
  melee-pc)      fetch_melee_pc ;;
  dawn)          fetch_dawn ;;
  sdl)           fetch_sdl_legacy; fetch_sdl_dusklight ;;
  sdl-legacy)    fetch_sdl_legacy ;;
  sdl-dusklight) fetch_sdl_dusklight ;;
  all)
    fetch_melee_pc
    fetch_dawn
    fetch_sdl_legacy
    fetch_sdl_dusklight
    ;;
  *)
    echo "usage: $0 [all|melee-pc|dawn|sdl|sdl-legacy|sdl-dusklight]" >&2
    exit 64
    ;;
esac

cat <<'MSG'

Reference trees ready under ref/.

Still required before building: a built Mesa/NVK Switch tree, exported as
MESA_NVK_ROOT. Switch ships no vendor Vulkan driver for homebrew and devkitPro's
switch-mesa package is EGL/GLES-only, so this is not optional and not something
this script can shortcut. See BUILDING.md, section "Mesa/NVK".

Next:
  bash builder/docker.sh image
  bash builder/docker.sh graphics all
  bash builder/docker.sh melee configure
  bash builder/docker.sh melee build
MSG
