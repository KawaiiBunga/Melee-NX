# Switch toolchain: devkitA64 GCC for C, Clang for C++.
# GCC supplies scalar_storage_order for the game; Clang builds Dawn.
# Run through builder/docker.sh; DEVKITPRO must name the toolchain root.

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

# Every object and runtime archive must be position-independent for an NRO.
set(_MELEE_COMMON_FLAGS
  "-mcpu=cortex-a57+crc+crypto -ffunction-sections -fdata-sections -D__SWITCH__ -D_GNU_SOURCE -D_DEFAULT_SOURCE -Wno-multichar -fPIC")

# Keep the game endian, Shift-JIS, aliasing, and floating-point conventions.
set(CMAKE_C_FLAGS
  "${_MELEE_COMMON_FLAGS} -fexec-charset=CP932 -fno-strict-aliasing -fwrapv -ffp-contract=off"
  CACHE STRING "" FORCE)
set(CMAKE_C_FLAGS_RELEASE "-O3 -DNDEBUG" CACHE STRING "" FORCE)

# Clang C++ flags: stdlib + gcc-toolchain so it finds libstdc++ ABI headers.
set(CMAKE_CXX_FLAGS
  "-stdlib=libstdc++ --gcc-toolchain=${DEVKITPRO}/devkitA64 ${_MELEE_COMMON_FLAGS} -ftls-model=local-exec${_MELEE_CXX_MULTILIB_INC}"
  CACHE STRING "" FORCE)
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -DNDEBUG" CACHE STRING "" FORCE)

set(CMAKE_ASM_FLAGS "-mcpu=cortex-a57+crc+crypto" CACHE STRING "" FORCE)

string(APPEND CMAKE_EXE_LINKER_FLAGS_INIT " -stdlib=libstdc++")

# Clang requests libunwind, but devkitA64 provides unwinding in libgcc.
# An empty libunwind archive satisfies the lookup. Select the PIC multilib
# for libgcc, the startup objects, and the C/C++ runtime archives.
execute_process(
  COMMAND "${MELEE_GCC}" -march=armv8-a+crc+crypto -fPIC -print-libgcc-file-name
  OUTPUT_VARIABLE _MELEE_LIBGCC_PATH OUTPUT_STRIP_TRAILING_WHITESPACE)
get_filename_component(_MELEE_LIBGCC_DIR "${_MELEE_LIBGCC_PATH}" DIRECTORY)
set(_MELEE_LIBCXX_PIC_DIR "${DEVKITPRO}/devkitA64/aarch64-none-elf/lib/pic")

set(_MELEE_UNWIND_STUB_DIR "${CMAKE_BINARY_DIR}/switch-stubs")
file(MAKE_DIRECTORY "${_MELEE_UNWIND_STUB_DIR}")
if(NOT EXISTS "${_MELEE_UNWIND_STUB_DIR}/libunwind.a")
  find_program(MELEE_AR NAMES aarch64-none-elf-ar REQUIRED
    PATHS "${DEVKITPRO}/devkitA64/bin" NO_DEFAULT_PATH)
  execute_process(COMMAND "${MELEE_AR}" rcs libunwind.a
    WORKING_DIRECTORY "${_MELEE_UNWIND_STUB_DIR}")
endif()

# Export paths for the executable target on every configure.
set(MELEE_LIBGCC_DIR "${_MELEE_LIBGCC_DIR}" CACHE PATH "" FORCE)
set(MELEE_UNWIND_STUB_DIR "${_MELEE_UNWIND_STUB_DIR}" CACHE PATH "" FORCE)
set(MELEE_LIBCXX_PIC_DIR "${_MELEE_LIBCXX_PIC_DIR}" CACHE PATH "" FORCE)

# Set CMAKE_CXX_STANDARD_LIBRARIES after project() in switch/CMakeLists.txt;
# devkitPro platform initialization would overwrite a toolchain-file override.
