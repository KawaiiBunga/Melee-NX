cmake_minimum_required(VERSION 3.20)
project(melee_nx_launcher LANGUAGES C CXX)

# The Melee NX launcher NRO. Built in the melee-nx-sdk container:
#   cmake -S launcher -B build/launcher -G Ninja -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/Switch.cmake
#   cmake --build build/launcher
# On Windows without Docker, launcher/build-native.ps1 builds the same thing.
if(NOT DEFINED ENV{DEVKITPRO})
  message(FATAL_ERROR "DEVKITPRO is not set; build inside the melee-nx SDK container or use build-native.ps1")
endif()

set(LAUNCHER_TITLE "Melee NX Launcher")
set(LAUNCHER_AUTHOR "melee-nx")
set(LAUNCHER_VERSION "0.5.0")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_program(PKG_CONFIG_EXECUTABLE NAMES aarch64-none-elf-pkg-config HINTS "$ENV{DEVKITPRO}/portlibs/switch/bin")
find_package(PkgConfig REQUIRED)
pkg_check_modules(DEPS REQUIRED sdl2 SDL2_image SDL2_ttf libcurl zlib)

file(GLOB_RECURSE LAUNCHER_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/source/*.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/source/*.c")
set(GAME_DATA "${CMAKE_CURRENT_SOURCE_DIR}/../switch/src")
add_executable(melee_nx_launcher ${LAUNCHER_SOURCES}
  "${GAME_DATA}/game_data/disc_extractor.cpp" "${GAME_DATA}/nod/gc_disc.c")
target_include_directories(melee_nx_launcher PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}/source" "${GAME_DATA}/game_data"
  ${DEPS_INCLUDE_DIRS})
target_compile_definitions(melee_nx_launcher PRIVATE __SWITCH__=1 CURL_STATICLIB=1
  MNX_LAUNCHER_VERSION_TOKEN=${LAUNCHER_VERSION})
target_compile_options(melee_nx_launcher PRIVATE -Wall -Wno-unused-parameter -ffunction-sections -fdata-sections)
target_link_directories(melee_nx_launcher PRIVATE ${DEPS_LIBRARY_DIRS})
target_link_libraries(melee_nx_launcher PRIVATE -Wl,--start-group ${DEPS_LIBRARIES} -lnx -lm -Wl,--end-group)
target_link_options(melee_nx_launcher PRIVATE -Wl,--gc-sections)

nx_generate_nacp(melee-nx-launcher.nacp NAME "${LAUNCHER_TITLE}" AUTHOR "${LAUNCHER_AUTHOR}" VERSION "${LAUNCHER_VERSION}")
nx_create_nro(melee_nx_launcher
  OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/melee-nx-launcher.nro"
  NACP melee-nx-launcher.nacp
  ICON "${CMAKE_CURRENT_SOURCE_DIR}/../switch/icon.jpg"
  ROMFS "${CMAKE_CURRENT_SOURCE_DIR}/romfs")
