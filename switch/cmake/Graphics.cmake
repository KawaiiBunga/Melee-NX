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
endif()

if(TARGET sqlite3)
  include("${MELEE_AURORA_SOURCE}/cmake/AuroraSwitchSQLite.cmake" OPTIONAL)
  if(COMMAND aurora_configure_switch_sqlite)
    aurora_configure_switch_sqlite(sqlite3)
  endif()
endif()
