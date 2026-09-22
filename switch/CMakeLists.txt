cmake_minimum_required(VERSION 3.25)
project(melee-nx LANGUAGES C CXX ASM)

# Aurora and the platform layer require C++20 (including std::span).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Require GCC for C (scalar_storage_order). Clang handles C++.
if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
  message(FATAL_ERROR "melee-nx requires GCC as the C compiler (same as melee-pc upstream). Use SwitchGCC.cmake.")
endif()

# Graphics dependencies must be included before forming the NVK link group.
include("${CMAKE_CURRENT_LIST_DIR}/cmake/Graphics.cmake")

# Set this after project(), which overwrites the toolchain value. The group
# resolves cyclic runtime and NVK archives after all application libraries.
# crtend/crtn must be last to close the startup sections opened by crti/crtbegin.
# See docs/ARCHITECTURE.md for the compiler and link-order constraints.
set(CMAKE_CXX_STANDARD_LIBRARIES
  "-Wl,--start-group -lstdc++ -lsupc++ -lpthread -lsysbase -lc -lm -lgcc -lunwind -lnx ${MELEE_NVK_ARCHIVES} -ljpeg -lexpat -Wl,--end-group ${MELEE_LIBGCC_DIR}/crtend.o ${MELEE_LIBGCC_DIR}/crtn.o"
  CACHE STRING "" FORCE)

# Game sources
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
  "${MELEE_PC_ROOT}/src/sdk_include"
  "${CMAKE_CURRENT_LIST_DIR}/src")
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
  -fPIC
  # Game headers rely on DISC_STRUCT and DP from this upstream compatibility header.
  -include ${MELEE_PC_ROOT}/src/pc/compat.h)
target_link_libraries(melee_game PUBLIC
  aurora::core aurora::gx aurora::gd aurora::vi aurora::pad aurora::si aurora::mtx
  aurora::os aurora::dvd aurora::card aurora::thp)

# PC platform layer
file(GLOB PC_SOURCES CONFIGURE_DEPENDS
  "${MELEE_PC_ROOT}/src/pc/*.c"
  "${MELEE_PC_ROOT}/src/pc/*.cpp")
list(REMOVE_ITEM PC_SOURCES "${MELEE_PC_ROOT}/src/pc/vtxarray.c")
# Use the Switch updater stub; the desktop JSON updater is not built here.
list(REMOVE_ITEM PC_SOURCES "${MELEE_PC_ROOT}/src/pc/updater.cpp")

# Disc reading and SQLite have their own targets; the data gate needs SDL headers.
file(GLOB SWITCH_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/src/*.c"
  "${CMAKE_CURRENT_LIST_DIR}/src/*.cpp")
# sqlite_vfs.cpp is compiled directly into the sqlite3 target by Graphics.cmake
list(REMOVE_ITEM SWITCH_SOURCES "${CMAKE_CURRENT_LIST_DIR}/src/sqlite_vfs.cpp")
file(GLOB GAME_DATA_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_LIST_DIR}/src/game_data/*.cpp")
list(APPEND SWITCH_SOURCES ${GAME_DATA_SOURCES})

add_executable(melee ${PC_SOURCES} ${SWITCH_SOURCES})
target_compile_options(melee PRIVATE -mcpu=cortex-a57+crc+crypto)

# Identify the ignored reference sources through their base revision and patch digest.
execute_process(COMMAND git -C "${_repo}" rev-parse --short=12 HEAD
  OUTPUT_VARIABLE MELEE_NX_ROOT_REV OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
execute_process(COMMAND git -C "${MELEE_PC_ROOT}" rev-parse --short=12 HEAD
  OUTPUT_VARIABLE MELEE_NX_MELEE_PC_REV OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
file(GLOB MELEE_NX_PATCH_FILES LIST_DIRECTORIES false "${CMAKE_CURRENT_LIST_DIR}/patches/*.patch")
list(SORT MELEE_NX_PATCH_FILES)
set(MELEE_NX_PATCH_MANIFEST "")
foreach(_patch IN LISTS MELEE_NX_PATCH_FILES)
  file(SHA256 "${_patch}" _patch_sha)
  get_filename_component(_patch_name "${_patch}" NAME)
  string(APPEND MELEE_NX_PATCH_MANIFEST "${_patch_name}:${_patch_sha};")
endforeach()
string(SHA256 MELEE_NX_PATCHSET_ID "${MELEE_NX_PATCH_MANIFEST}")
target_compile_definitions(melee PRIVATE
  MELEE_NX_ROOT_REV="${MELEE_NX_ROOT_REV}"
  MELEE_NX_MELEE_PC_REV="${MELEE_NX_MELEE_PC_REV}"
  MELEE_NX_PATCHSET_ID="${MELEE_NX_PATCHSET_ID}"
  MELEE_APP_VERSION="v0.2.0")
target_include_directories(melee PRIVATE "${CMAKE_CURRENT_LIST_DIR}/src/game_data")
target_link_libraries(melee PRIVATE melee_game aurora::main ${MELEE_CRYPTO_LIBS} ${MELEE_NETWORK_LIBS})
# Bridge the Rust archive ABI to newlib.
target_sources(melee PRIVATE "${MELEE_NVK_STUBS_SOURCE}")

# Clang ignores switch.specs on this target. Supply the PIE linker script,
# startup objects, and runtime libraries explicitly with devkitA64 ld.
target_link_options(melee PRIVATE
  -fuse-ld=${DEVKITPRO}/devkitA64/bin/aarch64-none-elf-ld
  -stdlib=libstdc++
  # Use the PIC runtime archives and the empty unwind archive required by Clang.
  -L${MELEE_LIBCXX_PIC_DIR}
  -L${MELEE_LIBGCC_DIR}
  -L${MELEE_UNWIND_STUB_DIR}
  -L${DEVKITPRO}/libnx/lib
  -L${DEVKITPRO}/portlibs/switch/lib
  # Startup objects must precede application objects.
  ${MELEE_LIBGCC_DIR}/crti.o
  ${MELEE_LIBGCC_DIR}/crtbegin.o
  # Expand the supported Switch linker options.
  -Wl,-T,${DEVKITPRO}/libnx/switch.ld
  -Wl,-pie
  -Wl,--no-dynamic-linker
  -Wl,-z,text
  -Wl,-z,now
  -Wl,--build-id=sha1
  -Wl,--require-defined=main
  # Keep the game's SDK __assert implementation, which precedes newlib in link order.
  -Wl,--allow-multiple-definition
  # Replace the bare-metal defaults with the runtime group set above.
  -nodefaultlibs
  -Wl,--gc-sections)
# CMAKE_CXX_STANDARD_LIBRARIES above places runtime libraries after all targets.

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
  VERSION "0.2.0")
nx_create_nro(melee
  NACP melee.nacp
  ICON "${CMAKE_CURRENT_LIST_DIR}/icon.jpg"
  ROMFS "${MELEE_PC_ROOT}/resources")
