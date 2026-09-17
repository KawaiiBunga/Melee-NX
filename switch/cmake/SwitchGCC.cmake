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

# -fPIC: matches devkitPro's own Generic-dkP.cmake, which appends this to every
# NintendoSwitch target's arch flags (confirmed by reading that Platform module
# inside the container). Every NRO is a PIE, and a PIE's read-only segments may
# not carry text relocations (-z text below enforces this) -- omitting -fPIC
# produces "read-only segment has dynamic relocations" at final link, from any
# object (ours or a static lib's) that was compiled position-dependent. This
# also means Dawn (built via this same toolchain file, see build-graphics.sh)
# needs a rebuild after this flag was added, not just melee itself.
set(_MELEE_COMMON_FLAGS
  "-march=armv8-a+crc+crypto -mtune=cortex-a57 -ffunction-sections -fdata-sections -D__SWITCH__ -D_GNU_SOURCE -D_DEFAULT_SOURCE -Wno-multichar -fPIC")

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

# Clang's BareMetal driver hardcodes "-lstdc++ -lsupc++ -lunwind -lc -lm -lgcc"
# for every C++ link on this target, ignoring -specs=/--rtlib=/--unwindlib=
# entirely (confirmed with -v: none of those flags change the generated link
# line). -lgcc resolves once its directory is on the search path; -lunwind
# does not, because devkitA64 has no libunwind.a at all -- its unwinder
# (_Unwind_Resume et al.) is bundled directly into libgcc.a instead. An empty
# stub archive satisfies the linker's "-lunwind" lookup without providing
# anything, and the real symbols are then found in -lgcc right after it.
#
# -fPIC here matches how it's actually compiled (see _MELEE_COMMON_FLAGS):
# devkitA64 GCC keeps a second, position-independent multilib set for every
# archive under aarch64-none-elf/lib/pic and lib/gcc/.../pic (libgcc.a,
# crti.o, crtbegin.o, libstdc++.a, libsupc++.a, libpthread.a, libsysbase.a,
# libc.a, ...) alongside the default non-PIC ones at the plain lib/ paths.
# Linking melee's PIE against the non-PIC set is what actually produced
# "read-only segment has dynamic relocations" -- confirmed in-container by
# linking a trivial PIE both ways.
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

# Exposed for switch/CMakeLists.txt's melee target (a target_link_options
# -L there is reliable across reconfigures; CMAKE_EXE_LINKER_FLAGS_INIT is
# not -- it only seeds CMAKE_EXE_LINKER_FLAGS on a build directory's very
# first configure, and this one has been reconfigured many times already).
set(MELEE_LIBGCC_DIR "${_MELEE_LIBGCC_DIR}" CACHE PATH "" FORCE)
set(MELEE_UNWIND_STUB_DIR "${_MELEE_UNWIND_STUB_DIR}" CACHE PATH "" FORCE)
set(MELEE_LIBCXX_PIC_DIR "${_MELEE_LIBCXX_PIC_DIR}" CACHE PATH "" FORCE)

# NOTE: the CMAKE_CXX_STANDARD_LIBRARIES override for melee's runtime-library
# relink group lives in switch/CMakeLists.txt, not here. devkitPro's own
# Platform/Generic-dkP.cmake force-sets that same variable (to "-lnx -lm")
# from inside its NintendoSwitch.cmake Platform module, which CMake loads
# during enable_language() -- i.e. during project() in switch/CMakeLists.txt,
# which runs AFTER this toolchain file. Setting it here just gets silently
# overwritten a moment later; see that file's comment for where it actually
# has to happen.
