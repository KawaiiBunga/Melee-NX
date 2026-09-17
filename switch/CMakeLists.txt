cmake_minimum_required(VERSION 3.25)
project(melee-nx LANGUAGES C CXX ASM)

# Require GCC for C (scalar_storage_order). Clang handles C++.
if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
  message(FATAL_ERROR "melee-nx requires GCC as the C compiler (same as melee-pc upstream). Use SwitchGCC.cmake.")
endif()

# ── Graphics stack (Dawn/SDL3/Aurora) ────────────────────────────────────────
include("${CMAKE_CURRENT_LIST_DIR}/cmake/Graphics.cmake")

# ── Game code from melee-pc ───────────────────────────────────────────────────
get_filename_component(_repo "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(MELEE_PC_ROOT "${_repo}/ref/melee-pc" CACHE PATH "melee-pc source root")

if(NOT EXISTS "${MELEE_PC_ROOT}/src/melee")
  message(FATAL_ERROR "melee-pc source not found at ${MELEE_PC_ROOT}. Clone it into ref/melee-pc.")
endif()

# Reuse melee-pc's crypto/network detection (falls back to builtin SHA1 on Switch)
set(MELEE_CRYPTO_LIBS)
set(MELEE_NETWORK_LIBS)
add_compile_definitions(MELEE_USE_BUILTIN_SHA1)

file(GLOB_RECURSE GAME_SOURCES CONFIGURE_DEPENDS
  "${MELEE_PC_ROOT}/src/melee/*.c"
  "${MELEE_PC_ROOT}/src/sysdolphin/*.c")
list(APPEND GAME_SOURCES "${MELEE_PC_ROOT}/src/pc/vtxarray.c")
list(FILTER GAME_SOURCES EXCLUDE REGEX "baselib/debugconsole_main\\.c$")

add_library(melee_game STATIC ${GAME_SOURCES})
target_include_directories(melee_game PUBLIC
  "${MELEE_PC_ROOT}/extern/aurora/include"
  "${MELEE_PC_ROOT}/src"
  "${MELEE_PC_ROOT}/src/sdk_include")
target_compile_definitions(melee_game PUBLIC TARGET_PC=1 MELEE_PC=1 __SWITCH__=1)
target_compile_options(melee_game PRIVATE
  -fpermissive
  -Wno-all -Wno-extra
  -Werror=int-conversion
  -Werror=implicit-function-declaration
  -Werror=incompatible-pointer-types
  -fno-strict-aliasing
  -fwrapv
  -ffp-contract=off
  -fexec-charset=CP932
  -Wno-scalar-storage-order
  -fPIC)
target_link_libraries(melee_game PUBLIC
  aurora::core aurora::gx aurora::gd aurora::vi aurora::pad aurora::si aurora::mtx
  aurora::os aurora::dvd aurora::card aurora::thp)

# ── PC platform layer ─────────────────────────────────────────────────────────
file(GLOB PC_SOURCES CONFIGURE_DEPENDS
  "${MELEE_PC_ROOT}/src/pc/*.c"
  "${MELEE_PC_ROOT}/src/pc/*.cpp")
list(REMOVE_ITEM PC_SOURCES "${MELEE_PC_ROOT}/src/pc/vtxarray.c")

# Switch-specific platform shim sources. src/nod/ is built as its own static
# lib (nod_shim, see Graphics.cmake) and src/game_data/ needs SDL3's headers,
# so both are globbed separately from the top-level shims below.
file(GLOB SWITCH_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/src/*.c"
  "${CMAKE_CURRENT_LIST_DIR}/src/*.cpp")
file(GLOB GAME_DATA_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/src/game_data/*.cpp")
list(APPEND SWITCH_SOURCES ${GAME_DATA_SOURCES})

add_executable(melee ${PC_SOURCES} ${SWITCH_SOURCES})
target_compile_options(melee PRIVATE -march=armv8-a+crc+crypto -mtune=cortex-a57)
target_include_directories(melee PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src/game_data")
target_link_libraries(melee PRIVATE melee_game aurora::main ${MELEE_CRYPTO_LIBS} ${MELEE_NETWORK_LIBS})

# NRO uses PIE + specs; drop melee-pc's Linux-only -no-pie/-Ttext-segment.
# MEM1 does not need a fixed low address at all — melee-pc's disc-pointer
# scheme already tolerates addresses above 4GB (see docs/PORTING-NOTES.md §2).
target_link_options(melee PRIVATE
  -stdlib=libstdc++
  -L${DEVKITPRO}/libnx/lib
  -L${DEVKITPRO}/portlibs/switch/lib
  -specs=${DEVKITPRO}/libnx/switch.specs
  -Wl,--gc-sections)

set_target_properties(melee PROPERTIES OUTPUT_NAME melee)

# Copy resources next to the binary (launcher fonts etc.)
add_custom_target(launcher_resources ALL
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${MELEE_PC_ROOT}/resources" "$<TARGET_FILE_DIR:melee>/resources")
add_dependencies(melee launcher_resources)

# Package as NRO
nx_generate_nacp(melee.nacp
  NAME "Melee NX"
  AUTHOR "melee-nx"
  VERSION "0.1.0")
nx_create_nro(melee
  NACP melee.nacp)
