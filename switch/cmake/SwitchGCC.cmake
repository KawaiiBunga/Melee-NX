# SwitchGCC.cmake — devkitPro Switch platform, C compiled with aarch64-none-elf-gcc,
# C++ compiled with Clang.
#
# Why split compilers: melee-pc's game code (src/melee, src/sysdolphin) requires GCC
# because it uses __attribute__((scalar_storage_order("big-endian"))), which only GCC
# implements. The aurora/Dawn/SDL3 stack is compiler-agnostic. We use the devkitA64 GCC
# for C and Windows LLVM Clang for C++ so both sides link into a single NRO.
#
# Usage (PowerShell or devkitPro msys2 shell):
#   cmake -B build/switch -S switch -G Ninja \
#         -DCMAKE_TOOLCHAIN_FILE=switch/cmake/SwitchGCC.cmake
#
# Requires DEVKITPRO in the environment.

if(NOT DEFINED ENV{DEVKITPRO})
  message(FATAL_ERROR "DEVKITPRO not set in the environment")
endif()
set(DEVKITPRO "$ENV{DEVKITPRO}")

# GCC for C (required by scalar_storage_order), Clang for C++
find_program(MELEE_GCC   NAMES aarch64-none-elf-gcc   REQUIRED
  PATHS "${DEVKITPRO}/devkitA64/bin" NO_DEFAULT_PATH)
find_program(MELEE_CLANGXX NAMES clang++ clang++.exe REQUIRED)

set(CMAKE_C_COMPILER   "${MELEE_GCC}"     CACHE FILEPATH "aarch64 gcc"    FORCE)
set(CMAKE_CXX_COMPILER "${MELEE_CLANGXX}" CACHE FILEPATH "aarch64 clang++" FORCE)
set(CMAKE_ASM_COMPILER "${MELEE_GCC}"     CACHE FILEPATH "aarch64 gcc asm" FORCE)

set(CMAKE_C_COMPILER_TARGET   aarch64-none-elf)
set(CMAKE_CXX_COMPILER_TARGET aarch64-none-elf)
set(CMAKE_ASM_COMPILER_TARGET aarch64-none-elf)
set(CMAKE_SYSROOT "${DEVKITPRO}/devkitA64/aarch64-none-elf")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# Pull in libnx include/lib paths, switch.specs, elf2nro/nacptool helpers.
include("${DEVKITPRO}/cmake/Switch.cmake")

# Detect devkitA64 GCC multilib dir for libstdc++ headers (needed by Clang CXX).
file(GLOB _MELEE_CXXCFG_DIRS
     "${DEVKITPRO}/devkitA64/aarch64-none-elf/include/c++/*/aarch64-none-elf")
set(_MELEE_CXX_MULTILIB_INC "")
foreach(_d IN LISTS _MELEE_CXXCFG_DIRS)
  if(EXISTS "${_d}/bits/c++config.h")
    string(APPEND _MELEE_CXX_MULTILIB_INC " -I${_d}")
  endif()
endforeach()

set(_MELEE_COMMON_FLAGS
  "-march=armv8-a+crc+crypto -mtune=cortex-a57 -ffunction-sections -fdata-sections -D__SWITCH__ -D_GNU_SOURCE -D_DEFAULT_SOURCE -Wno-multichar")

# GCC C flags: big-endian struct support, Shift-JIS charset. melee-pc's disc-pointer
# scheme (src/pc/disc.h) already tolerates MEM1 living above 4GB via an ext-pointer
# table, so unlike the Linux build there is no -no-pie/-Ttext-segment here to match.
set(CMAKE_C_FLAGS
  "${_MELEE_COMMON_FLAGS} -fexec-charset=CP932 -fno-strict-aliasing -fwrapv -ffp-contract=off"
  CACHE STRING "" FORCE)

# Clang C++ flags: stdlib + gcc-toolchain so it finds libstdc++ ABI headers.
set(CMAKE_CXX_FLAGS
  "-stdlib=libstdc++ --gcc-toolchain=${DEVKITPRO}/devkitA64 ${_MELEE_COMMON_FLAGS} -ftls-model=local-exec${_MELEE_CXX_MULTILIB_INC}"
  CACHE STRING "" FORCE)

set(CMAKE_ASM_FLAGS "-march=armv8-a+crc+crypto" CACHE STRING "" FORCE)

string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT " -stdlib=libstdc++")
