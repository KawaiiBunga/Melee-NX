# Graphics.cmake — wire prebuilt Switch Dawn, SDL3, and Aurora into the melee build.
# Mirrors KartPad-NX's Graphics.cmake exactly; only path roots differ.
include_guard(GLOBAL)

get_filename_component(_repo "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(MELEE_DAWN_SOURCE "${_repo}/ref/dawn"            CACHE PATH "Patched Dawn source")
set(MELEE_DAWN_BUILD  "${_repo}/build/dawn-switch"   CACHE PATH "Switch Dawn build")
set(MELEE_SDL_SOURCE  "${_repo}/ref/SDL"             CACHE PATH "Patched SDL3 source")
set(MELEE_SDL_BUILD   "${_repo}/build/sdl-switch"    CACHE PATH "Switch SDL3 build")
set(MELEE_AURORA_SOURCE "${_repo}/ref/melee-pc/extern/aurora" CACHE PATH "Aurora source (from melee-pc)")

foreach(_archive IN ITEMS
    "${MELEE_DAWN_BUILD}/src/dawn/native/libwebgpu_dawn.a"
    "${MELEE_SDL_BUILD}/libSDL3.a")
  if(NOT EXISTS "${_archive}")
    message(FATAL_ERROR "Build the Switch graphics dependency first (builder/build-graphics.sh): ${_archive}")
  endif()
endforeach()

add_library(dawn::webgpu_dawn STATIC IMPORTED GLOBAL)
set_target_properties(dawn::webgpu_dawn PROPERTIES
  IMPORTED_LOCATION "${MELEE_DAWN_BUILD}/src/dawn/native/libwebgpu_dawn.a"
  INTERFACE_INCLUDE_DIRECTORIES "${MELEE_DAWN_BUILD}/gen/include;${MELEE_DAWN_SOURCE}/include")

add_library(dawn::dawncpp_headers INTERFACE IMPORTED GLOBAL)
set_target_properties(dawn::dawncpp_headers PROPERTIES
  INTERFACE_LINK_LIBRARIES dawn::webgpu_dawn)

foreach(_name flat_hash_map btree)
  add_library(absl::${_name} INTERFACE IMPORTED GLOBAL)
  set_target_properties(absl::${_name} PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${MELEE_DAWN_SOURCE}/third_party/abseil-cpp"
    INTERFACE_LINK_LIBRARIES dawn::webgpu_dawn)
endforeach()

add_library(SDL3::SDL3-static STATIC IMPORTED GLOBAL)
set_target_properties(SDL3::SDL3-static PROPERTIES
  IMPORTED_LOCATION "${MELEE_SDL_BUILD}/libSDL3.a"
  INTERFACE_INCLUDE_DIRECTORIES "${MELEE_SDL_SOURCE}/include")

# ── nod (disc reading) ────────────────────────────────────────────────────────
# Real `nod` is a Rust crate (github.com/encounter/nod); Rust has no official
# Switch/Horizon target. switch/src/nod/ is a from-scratch C reimplementation
# of the small slice of nod's C ABI melee-pc/aurora actually call (see
# nod_shim.c), backed by a plain-C single-partition GameCube disc reader
# (gc_disc.c). Predefining nod::nod here (same trick as Dawn/SDL3 above) makes
# AuroraNodProvider.cmake's "system" branch use it instead of trying to
# FetchContent+Corrosion the real crate.
add_library(nod_shim STATIC
  "${CMAKE_CURRENT_LIST_DIR}/../src/nod/gc_disc.c"
  "${CMAKE_CURRENT_LIST_DIR}/../src/nod/nod_shim.c")
target_include_directories(nod_shim PUBLIC "${CMAKE_CURRENT_LIST_DIR}/../src/nod")
add_library(nod::nod ALIAS nod_shim)
set(nod_FOUND TRUE)

set(Dawn_FOUND TRUE)
set(SDL3_FOUND TRUE)
set(AURORA_DAWN_PROVIDER system CACHE STRING "" FORCE)
set(AURORA_SDL3_PROVIDER system CACHE STRING "" FORCE)
set(AURORA_SDL3_LINKAGE  static CACHE STRING "" FORCE)
set(AURORA_NOD_PROVIDER  system CACHE STRING "" FORCE)

# melee-pc uses DVD, CARD, THP, and RmlUi — keep them all on.
set(AURORA_ENABLE_DVD   ON  CACHE BOOL "" FORCE)
set(AURORA_ENABLE_CARD  ON  CACHE BOOL "" FORCE)
set(AURORA_ENABLE_THP   ON  CACHE BOOL "" FORCE)
set(AURORA_ENABLE_RMLUI ON  CACHE BOOL "" FORCE)
set(AURORA_ENABLE_GX    ON  CACHE BOOL "" FORCE)
set(AURORA_PLATFORM_SWITCH ON CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF)
set(TRACY_ENABLE OFF CACHE BOOL "" FORCE)

add_subdirectory("${MELEE_AURORA_SOURCE}" aurora)

# Strip libdl from Tracy — it doesn't exist on libnx.
if(TARGET TracyClient)
  foreach(_link_property LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
    get_target_property(_tracy_links TracyClient ${_link_property})
    if(_tracy_links)
      list(REMOVE_ITEM _tracy_links dl)
      set_property(TARGET TracyClient PROPERTY ${_link_property} "${_tracy_links}")
    endif()
  endforeach()

  # Tracy's own platform detection doesn't know __SWITCH__ and hard #errors
  # (GetThreadHandleImpl) or reaches for getlogin_r (unavailable on newlib).
  # TRACY_ENABLE is OFF, so these values are never read at runtime -- unlike
  # ref/dawn/ref/SDL/melee-pc's extern/aurora, Tracy is FetchContent'd fresh
  # into the build tree each configure, so it's patched here with plain
  # `patch` (idempotent via -N) instead of through apply_patch_once/git apply.
  find_program(_melee_patch_exe patch REQUIRED)
  # tracy_SOURCE_DIR is set deep inside aurora's own add_subdirectory() scope
  # and never propagates up here, so this is FetchContent's well-known default
  # layout (${CMAKE_BINARY_DIR}/_deps/<name>-src) spelled out explicitly.
  set(_melee_tracy_src "${CMAKE_BINARY_DIR}/_deps/tracy-src")
  if(EXISTS "${_melee_tracy_src}/public/common/TracySystem.cpp")
    execute_process(
      COMMAND "${_melee_patch_exe}" -p1 -N --forward -r -
              -i "${CMAKE_CURRENT_LIST_DIR}/../patches/tracy-switch-platform.patch"
      WORKING_DIRECTORY "${_melee_tracy_src}"
      RESULT_VARIABLE _melee_tracy_patch_result
      OUTPUT_VARIABLE _melee_tracy_patch_output
      ERROR_VARIABLE _melee_tracy_patch_output)
    if(NOT _melee_tracy_patch_result EQUAL 0)
      message(WARNING "melee-nx: tracy-switch-platform.patch did not apply cleanly:\n${_melee_tracy_patch_output}")
    endif()
  else()
    message(WARNING "melee-nx: Tracy source not found at ${_melee_tracy_src}; tracy-switch-platform.patch was not applied")
  endif()
endif()

if(TARGET sqlite3)
  # The amalgamation's unix VFS backend includes <sys/mman.h> whenever WAL or
  # mmap I/O is enabled; devkitA64/newlib has no mmap at all. We already want
  # journal_mode=MEMORY for the FAT32 SD card shader cache (see
  # docs/PORTING-NOTES.md §4), so WAL was never going to be used anyway --
  # disabling both compiles out the include instead of stubbing mmap().
  target_compile_definitions(sqlite3 PRIVATE
    SQLITE_OMIT_WAL SQLITE_MAX_MMAP_SIZE=0
    SQLITE_OMIT_LOAD_EXTENSION) # dlopen()/<dlfcn.h> don't exist on libnx
  include("${MELEE_AURORA_SOURCE}/cmake/AuroraSwitchSQLite.cmake" OPTIONAL)
  if(COMMAND aurora_configure_switch_sqlite)
    aurora_configure_switch_sqlite(sqlite3)
  endif()
endif()

# ── Vulkan driver (Mesa NVK) ─────────────────────────────────────────────────
# Dawn's Vulkan backend needs a real Vulkan implementation to resolve
# vkGetInstanceProcAddr against; devkitPro's own switch-mesa package (installed
# in this container) is EGL/GLES-only (libEGL.a/libGLESv2.a, no libvulkan.a) --
# actual Switch Vulkan comes only from NVK, Mesa's from-scratch driver for the
# Tegra X1 written partly in Rust (NAK, its shader compiler backend). Building
# NVK from source needs its own Rust-enabled cross toolchain (see KartPad-NX's
# switch/overlays/mesa-switch/, which builds it via a separate Docker image
# with rustup targeting aarch64-unknown-linux-gnu, since Rust has no Horizon/
# aarch64-none-elf target -- the same fundamental gap `nod`/switch/src/nod hit,
# solved there by cross-targeting Linux and bridging the ABI mismatch at link
# time instead of by rewriting NAK/NVK from scratch, which is well out of
# scope here). Reusing that already-built output (~436 MB of cross-compiled
# archives at MESA_NVK_ROOT/builddir-switch) is far faster than reproducing
# the whole Rust/Meson pipeline for this port too.
set(MESA_NVK_ROOT "" CACHE PATH
  "Mesa/NVK Switch cross-build root (has builddir-switch/**/*.a and src/nouveau/vulkan/rust_switch_stubs.c) -- see docs/DEPS.md")
if(MESA_NVK_ROOT STREQUAL "")
  message(FATAL_ERROR "MESA_NVK_ROOT not set. See docs/DEPS.md for how to obtain a built Mesa/NVK Switch tree.")
endif()
set(_nvk_build "${MESA_NVK_ROOT}/builddir-switch")
set(_nvk_stubs "${MESA_NVK_ROOT}/src/nouveau/vulkan/rust_switch_stubs.c")
if(NOT EXISTS "${_nvk_stubs}")
  message(FATAL_ERROR "Missing ${_nvk_stubs} -- MESA_NVK_ROOT does not look like a Mesa checkout with the Switch/NVK overlay applied.")
endif()
file(GLOB_RECURSE _nvk_archives CONFIGURE_DEPENDS "${_nvk_build}/*.a")
list(FILTER _nvk_archives EXCLUDE REGEX "/libsanity_check_for_rust\\.a$")
if(NOT _nvk_archives)
  message(FATAL_ERROR "No .a archives found under ${_nvk_build} -- run switch/overlays/mesa-switch/build-switch.sh (see docs/DEPS.md) first.")
endif()
list(SORT _nvk_archives)
list(JOIN _nvk_archives " " MELEE_NVK_ARCHIVES)
set(MELEE_NVK_STUBS_SOURCE "${_nvk_stubs}")
