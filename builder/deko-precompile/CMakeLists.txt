# deko-precompile: a host tool that builds the deko3d renderer's shader seed (main.cpp, docs/DEKO3D.md).
# Configure it with the host compiler (no Switch toolchain):
#
#   cmake -S builder/deko-precompile -B build/deko-precompile -G Ninja \
#     -DDAWN_HOST_BUILD=<host build of ref/dawn with Tint's GLSL writer> \
#     -DDAWN_GEN_INCLUDE=<any Dawn build's gen/include> -DDEPS_DIR=<the game build's _deps>
#
# It compiles Aurora's GX shader generator from the staged deko copy (builder/integrate-deko.py), the
# renderer's own translator and cache file code, and the vendored uam, so its output matches the console.
cmake_minimum_required(VERSION 3.20)
project(deko_precompile C CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

get_filename_component(_repo "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(AURORA_STAGED "${_repo}/build/aurora-deko" CACHE PATH "Aurora staged by builder/integrate-deko.py")
set(DAWN_SOURCE "${_repo}/ref/dawn" CACHE PATH "Dawn source (Tint headers, Abseil)")
set(DAWN_HOST_BUILD "" CACHE PATH "Host build of ref/dawn: Tint and Abseil archives")
set(DAWN_GEN_INCLUDE "" CACHE PATH "gen/include of any Dawn build (webgpu.h, webgpu_cpp.h)")
set(DEPS_DIR "${_repo}/build/switch-deko/_deps" CACHE PATH "fmt, xxhash, sqlite3 and tracy sources fetched by the game build")
foreach(_dir AURORA_STAGED DAWN_SOURCE DAWN_HOST_BUILD DAWN_GEN_INCLUDE DEPS_DIR)
  if(NOT IS_DIRECTORY "${${_dir}}")
    message(FATAL_ERROR "${_dir} is not a directory: '${${_dir}}'")
  endif()
endforeach()

add_subdirectory("${_repo}/switch/uam" "${CMAKE_BINARY_DIR}/uam")

add_executable(deko-precompile
  main.cpp host_stubs.cpp
  "${_repo}/switch/deko/shader_translate.cpp"
  "${_repo}/switch/deko/dksh_file.cpp"
  "${AURORA_STAGED}/lib/gx/shader.cpp"
  "${AURORA_STAGED}/lib/gx/shader_info.cpp"
  "${AURORA_STAGED}/lib/gx/attr_fmt.cpp"
  "${DEPS_DIR}/xxhash-src/xxhash.c"
  "${DEPS_DIR}/sqlite3-src/sqlite3.c")
target_include_directories(deko-precompile PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/shim"
  "${_repo}/switch/deko" "${_repo}/switch/uam" "${_repo}/switch/src"
  "${AURORA_STAGED}/lib" "${AURORA_STAGED}/include"
  "${DAWN_SOURCE}" "${DAWN_SOURCE}/include" "${DAWN_GEN_INCLUDE}" "${DAWN_SOURCE}/third_party/abseil-cpp"
  "${DEPS_DIR}/fmt-src/include" "${DEPS_DIR}/xxhash-src" "${DEPS_DIR}/sqlite3-src" "${DEPS_DIR}/tracy-src/public")
# The console's Aurora is built with these: the generator's output (and so the cache keys) depends on them.
target_compile_definitions(deko-precompile PRIVATE
  AURORA_GFX_DEKO3D=1 AURORA TARGET_PC AURORA_ENABLE_GX WEBGPU_DAWN FMT_HEADER_ONLY
  SQLITE_OMIT_LOAD_EXTENSION SQLITE_THREADSAFE=0)
set_source_files_properties("${DEPS_DIR}/sqlite3-src/sqlite3.c" "${DEPS_DIR}/xxhash-src/xxhash.c"
  PROPERTIES COMPILE_OPTIONS "-w")

file(GLOB_RECURSE _host_archives "${DAWN_HOST_BUILD}/*.a" "${DAWN_HOST_BUILD}/*.lib")
list(FILTER _host_archives EXCLUDE REGEX "webgpu_dawn|dawn_native|dawn_proc|dawn_wire")
find_package(Threads REQUIRED)  # Abseil's and uam's mutexes
target_link_libraries(deko-precompile PRIVATE uamlib "-Wl,--start-group" ${_host_archives} "-Wl,--end-group"
                      Threads::Threads)
