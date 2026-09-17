cmake_minimum_required(VERSION 3.25)
project(melee-nx LANGUAGES C CXX ASM)

# Matches melee-pc's own CMakeLists.txt. Without this, PC_SOURCES/SWITCH_SOURCES
# fall back to Clang's C++17 default and std::span (used by aurora/rmlui.hpp,
# game_data_gate.cpp's std::filesystem usage, etc.) fails to resolve.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Require GCC for C (scalar_storage_order). Clang handles C++.
if(NOT CMAKE_C_COMPILER_ID STREQUAL "GNU")
  message(FATAL_ERROR "melee-nx requires GCC as the C compiler (same as melee-pc upstream). Use SwitchGCC.cmake.")
endif()

# ── Graphics stack (Dawn/SDL3/Aurora/NVK) ────────────────────────────────────
# Included before the CMAKE_CXX_STANDARD_LIBRARIES override below so the NVK
# archive list it computes (MELEE_NVK_ARCHIVES) can be folded into that same
# group -- Dawn's Vulkan backend only resolves against NVK's libvulkan.a.
include("${CMAKE_CURRENT_LIST_DIR}/cmake/Graphics.cmake")

# Overrides devkitPro's own CMAKE_CXX_STANDARD_LIBRARIES ("-lnx -lm", force-set
# moments ago by Platform/Generic-dkP.cmake during the project() call above)
# with melee's -nodefaultlibs replacement group (see the melee target below
# for why). libsysbase is devkitA64's newlib retarget layer (_read_r/_write_r/
# __libc_lock_acquire_recursive/etc., connecting reentrant syscall stubs and
# libc locking to real Horizon OS syscalls via libnx) -- it's a separate
# archive from both libc.a and libnx.a, easy to miss since nothing else in
# this project links libc++ programs against devkitA64 this way. This is the ONE hook late enough in CMake's generated link rule
# to land after target_link_libraries's entire resolved graph (SDL3, sqlite3,
# Dawn, aurora, melee_game...) -- confirmed empirically that "-lnx -lm" always
# lands last regardless of what target_link_libraries/target_link_options add,
# and setting this same variable from the toolchain file gets silently
# overwritten right here instead (project()'s enable_language() runs after
# the toolchain file, and Generic-dkP.cmake force-sets it unconditionally).
# Moving this whole override to after include(Graphics.cmake) above (rather
# than immediately after project()) is safe: the force-set this works around
# happens once, synchronously, inside project()'s enable_language() call at
# the top of this file, well before Graphics.cmake runs.
#
# The Mesa/NVK archives (MELEE_NVK_ARCHIVES) are folded into this same
# --start-group/--end-group rather than a separate one: NVK's own archives
# are mutually cyclic (libnvk.a <-> libnak.a <-> libnil.a <-> ...), and Dawn's
# single undefined ref into NVK (vkGetInstanceProcAddr) resolves fine either
# way since Dawn itself is linked earlier via aurora::main -- one shared group
# is simplest and matches how KartPad-NX's own working recipe does it
# (switch/cmake/GraphicsLink.cmake there, one rescan group for everything).
# crtend.o/crtn.o are switch.specs' missing *endfile pair (separate from
# *startfile, which only supplies crti.o/crtbegin.o -- see melee's
# target_link_options below). Without them, .init/.fini contain only crti.o's
# register-save prologue and no ret: confirmed via addr2line/objdump on a
# crashed build that __libc_init_array's unconditional early call to _init()
# fell straight through .init into .fini's prologue and off the end of .fini
# into unmapped zero bytes (Undefined Instruction, opcode 0) -- before main()
# ever ran. Must be the literal last objects on the link line (this variable
# is the one hook proven to land there), same as -lgcc/crtbegin.o must be
# proven-first via target_link_options.
set(CMAKE_CXX_STANDARD_LIBRARIES
  "-Wl,--start-group -lstdc++ -lsupc++ -lpthread -lsysbase -lc -lm -lgcc -lunwind -lnx ${MELEE_NVK_ARCHIVES} -ljpeg -lexpat -Wl,--end-group ${MELEE_LIBGCC_DIR}/crtend.o ${MELEE_LIBGCC_DIR}/crtn.o"
  CACHE STRING "" FORCE)

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
  -fPIC
  # Force-included into every game translation unit (matches melee-pc's own
  # CMakeLists.txt): defines DISC_STRUCT/DISC_PTR/DP/DISC_ASSERT_SIZE via
  # pc/disc.h. Missing this causes exactly the "struct DISC_STRUCT Foo {"
  # parse errors seen across src/sysdolphin -- these headers never #include
  # disc.h themselves, they rely on this force-include like upstream does.
  -include ${MELEE_PC_ROOT}/src/pc/compat.h)
target_link_libraries(melee_game PUBLIC
  aurora::core aurora::gx aurora::gd aurora::vi aurora::pad aurora::si aurora::mtx
  aurora::os aurora::dvd aurora::card aurora::thp)

# ── PC platform layer ─────────────────────────────────────────────────────────
file(GLOB PC_SOURCES CONFIGURE_DEPENDS
  "${MELEE_PC_ROOT}/src/pc/*.c"
  "${MELEE_PC_ROOT}/src/pc/*.cpp")
list(REMOVE_ITEM PC_SOURCES "${MELEE_PC_ROOT}/src/pc/vtxarray.c")
# Self-update over the GitHub releases API makes no sense for homebrew, and its
# recursive JsonValue trips a libstdc++-15 completeness check devkitA64 enforces
# more strictly than other platforms' standard libraries. Replaced by
# switch/src/updater_stub_switch.cpp (satisfies launcher.cpp's pc::updater::
# calls, which this port's boot flow never actually reaches).
list(REMOVE_ITEM PC_SOURCES "${MELEE_PC_ROOT}/src/pc/updater.cpp")

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
# Rust-vs-newlib ABI bridge (mmap/getauxval/__errno_location/sysconf/execvp/
# waitpid/fchown/...) required by the NVK archives in CMAKE_CXX_STANDARD_LIBRARIES
# above -- see Graphics.cmake's Mesa/NVK section for why this exists as a
# separate object rather than living inside one of those archives.
target_sources(melee PRIVATE "${MELEE_NVK_STUBS_SOURCE}")

# NRO uses PIE + specs; drop melee-pc's Linux-only -no-pie/-Ttext-segment.
# MEM1 does not need a fixed low address at all — melee-pc's disc-pointer
# scheme already tolerates addresses above 4GB (see docs/PORTING-NOTES.md §2).
#
# -specs=switch.specs is NOT passed here (unlike a GCC-linked devkitPro
# project) because Clang's driver silently ignores -specs= for this target —
# confirmed both by -v (it never appears in the generated link line) and by
# the symbol fallout of leaving it out: switch.specs' *link block supplies
# -T switch.ld (defines __tls_*/__bss_*/__got_*/_DYNAMIC/__argdata__/etc.,
# which is why those were undefined), -pie, --no-dynamic-linker, -z text/now,
# --build-id=sha1, --gc-sections; its *startfile block supplies
# crti.o/crtbegin.o + --require-defined=main. Everything below is that same
# spec expanded by hand. Two flags from the real switch.specs are dropped:
# --nx-module-name and --spare-dynamic-tags=0 (crash-report metadata and
# dynamic-tag padding respectively) — neither this container's system lld nor
# devkitA64's own GNU ld recognizes them (checked --help on both), and
# neither is required to produce a bootable NRO.
#
# The linker itself is also switched from Clang's default (system lld) to
# devkitA64's own aarch64-none-elf-ld: that's the one whose --help actually
# lists --require-defined/-z nodynamic-undefined-weak/--spare-dynamic-tags,
# i.e. the one switch.specs was written for.
target_link_options(melee PRIVATE
  -fuse-ld=${DEVKITPRO}/devkitA64/bin/aarch64-none-elf-ld
  -stdlib=libstdc++
  # Clang's BareMetal driver hardcodes "-lstdc++ -lsupc++ -lunwind -lc -lm -lgcc"
  # for this target regardless of -specs=/--rtlib=/--unwindlib= (confirmed with
  # -v; none change the generated link line). These entries are what actually
  # resolve the runtime-library group in CMAKE_CXX_STANDARD_LIBRARIES below:
  # MELEE_LIBCXX_PIC_DIR for -lstdc++/-lsupc++/-lpthread/-lsysbase/-lc (the
  # PIC multilib set — required, see SwitchGCC.cmake's -fPIC comment),
  # MELEE_LIBGCC_DIR for -lgcc (also now the PIC multilib's libgcc.a), and
  # MELEE_UNWIND_STUB_DIR for an empty libunwind.a stub, since devkitA64 has
  # no real one -- its unwinder is bundled into libgcc.a instead.
  -L${MELEE_LIBCXX_PIC_DIR}
  -L${MELEE_LIBGCC_DIR}
  -L${MELEE_UNWIND_STUB_DIR}
  -L${DEVKITPRO}/libnx/lib
  -L${DEVKITPRO}/portlibs/switch/lib
  # switch.specs' *startfile block: crti.o/crtbegin.o (global constructor
  # bootstrap) must precede every other object file, which is exactly where
  # target_link_options places its own literal items (see the CMake
  # link-ordering note further down) -- these come from the same PIC gcc
  # multilib dir as libgcc.a, right above.
  ${MELEE_LIBGCC_DIR}/crti.o
  ${MELEE_LIBGCC_DIR}/crtbegin.o
  # switch.specs' *link block, hand-expanded (see the comment above this
  # target_link_options call for why -specs= itself can't be used instead).
  -Wl,-T,${DEVKITPRO}/libnx/switch.ld
  -Wl,-pie
  -Wl,--no-dynamic-linker
  -Wl,-z,text
  -Wl,-z,now
  -Wl,--build-id=sha1
  -Wl,--require-defined=main
  # src/sysdolphin/baselib/debug.c defines its own __assert(str, line, file)
  # matching the original GameCube SDK's ASSERTLINE/ASSERTMSGLINE signature --
  # unrelated to and incompatible with <assert.h>'s __assert, but the same
  # symbol name. newlib's libc.a strongly defines it too (glibc doesn't, which
  # is why Linux/Windows/Android builds never hit this), so the linker sees a
  # real duplicate. melee_game.a is listed first, so this keeps the decomp's
  # own definition, not newlib's.
  -Wl,--allow-multiple-definition
  # devkitA64's newlib splits pthread_* into its own libpthread.a (needed by
  # SDL3, sqlite3, aurora_dvd/gx, and libstdc++'s own std::condition_variable/
  # __cxa_guard/thread_local internals) and needs libnx/libsysbase for syscall
  # glue (_read_r, _write_r, _exit, mkdir, nanosleep, ...). None of these are
  # in Clang's implicit bare-metal library tail, so -nodefaultlibs drops that
  # tail entirely; the actual replacement list is added below via
  # target_link_libraries (target_link_options always lands BEFORE the
  # object files/application archives here, too early to resolve their
  # undefined refs -- see the second target_link_libraries call below for why
  # this needs to be there instead).
  -nodefaultlibs
  -Wl,--gc-sections)
# The actual libstdc++/libpthread/libc/libgcc/libunwind/libnx relink group
# that -nodefaultlibs above requires lives in SwitchGCC.cmake
# (CMAKE_CXX_STANDARD_LIBRARIES) -- that's the one hook late enough in
# CMake's generated link rule to resolve undefined refs coming from every
# transitively-linked archive; a target_link_libraries/target_link_options
# call here lands too early (see that file's comment for why).

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
