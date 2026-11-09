# Native Maxwell renderer for MELEE_NX_GFX=deko3d (docs/DEKO3D.md). Included by Graphics.cmake after
# Aurora, which it hooks: Dawn keeps the Null backend's WebGPU objects, switch/deko draws them.
# Adapted from KartPadNX (SwitchWaker 04372277d03f); see switch/deko/THIRD_PARTY.md.
include_guard(GLOBAL)
set(_dk "${_repo}/switch/deko")
set(NX_ROOT "$ENV{DEVKITPRO}/libnx")
add_subdirectory("${_repo}/switch/uam" "${CMAKE_BINARY_DIR}/uam" EXCLUDE_FROM_ALL)

# The renderer's own shaders (present pass, boot text) are compiled to DKSH at build time.
file(GLOB _glsl "${_dk}/shaders/*.glsl")
set(_dksh)
foreach(_shader IN LISTS _glsl)
  get_filename_component(_name "${_shader}" NAME_WE)
  if(_name MATCHES "_vsh$")
    set(_stage vert)
  else()
    set(_stage frag)
  endif()
  nx_add_shader_program(melee_${_name} "${_shader}" ${_stage} OUTPUT "${CMAKE_BINARY_DIR}/dksh/${_name}.dksh")
  list(APPEND _dksh melee_${_name})
endforeach()
dkp_add_embedded_binary_library(melee_deko_builtin ${_dksh})

add_library(melee_deko STATIC
  "${_dk}/device.cpp" "${_dk}/memory.cpp" "${_dk}/descriptors.cpp"
  "${_dk}/pattern.cpp" "${_dk}/shader_cache.cpp"
  "${_dk}/shaders.cpp" "${_dk}/shader_translate.cpp" "${_dk}/dksh_file.cpp"
  "${_dk}/aurora/dk_objects.cpp"
  "${_dk}/backend_objects.cpp" "${_dk}/backend_pipelines.cpp" "${_dk}/backend_commands.cpp"
  "${_dk}/backend_present.cpp")
target_include_directories(melee_deko PUBLIC "${_dk}" PRIVATE
  "${MELEE_DAWN_SOURCE}" "${MELEE_DAWN_BUILD}/gen" "${_dk}/aurora"
  "$ENV{DEVKITPRO}/portlibs/switch/include")
target_compile_definitions(melee_deko PRIVATE
  COS_SWITCH_NRO_DIR="/switch/melee-nx" COS_SWITCH_DATA_DIR="sdmc:/switch/melee-nx")

# Diagnostics for the NVIDIA channel timeout (2348-0006) KartPadNX saw at scene loads. All of it is
# inert until perf.cfg `gpu_steady_v9 1` (backend_commands.cpp), so a console that hits the timeout
# can switch the mitigation on without a rebuild; the cost of having it compiled in is memory.
option(MELEE_NX_DEKO_DEBUG "Link the checked deko3d library (validates every API call)" OFF)
option(MELEE_NX_DEKO_GPU_CHECKPOINTS "Wait for the GPU after every draw and transfer (diagnostic, slow)" OFF)
option(MELEE_NX_DEKO_GPU_FLUSHES "Bound native submissions without CPU waits (diagnostic, perf.cfg gpu_steady_v9)" ON)
option(MELEE_NX_DEKO_GPU_BREADCRUMBS "GPU-written completion markers for the flush policy" ON)
set(MELEE_NX_DEKO_GPU_FLUSH_INTERVAL "32" CACHE STRING "Operations per diagnostic submit batch (1-64)")
if(MELEE_NX_DEKO_GPU_BREADCRUMBS)
  if(NOT MELEE_NX_DEKO_GPU_FLUSHES)
    message(FATAL_ERROR "GPU breadcrumbs require MELEE_NX_DEKO_GPU_FLUSHES")
  endif()
  target_compile_definitions(melee_deko PRIVATE COS_DK_GPU_BREADCRUMBS=1)
endif()
if(MELEE_NX_DEKO_GPU_FLUSHES)
  if(MELEE_NX_DEKO_GPU_CHECKPOINTS)
    message(FATAL_ERROR "Select either GPU checkpoints or asynchronous operation flushes")
  endif()
  if(NOT MELEE_NX_DEKO_GPU_FLUSH_INTERVAL MATCHES "^[1-9][0-9]*$" OR MELEE_NX_DEKO_GPU_FLUSH_INTERVAL GREATER 64)
    message(FATAL_ERROR "MELEE_NX_DEKO_GPU_FLUSH_INTERVAL must be between 1 and 64")
  endif()
  target_compile_definitions(melee_deko PRIVATE COS_DK_GPU_FLUSHES=1
    COS_DK_GPU_FLUSH_INTERVAL=${MELEE_NX_DEKO_GPU_FLUSH_INTERVAL})
endif()
if(MELEE_NX_DEKO_GPU_CHECKPOINTS)
  if(NOT MELEE_NX_DEKO_DEBUG)
    message(FATAL_ERROR "GPU checkpoints require MELEE_NX_DEKO_DEBUG=ON")
  endif()
  target_compile_definitions(melee_deko PRIVATE COS_DK_GPU_CHECKPOINTS=1)
endif()
if(MELEE_NX_DEKO_DEBUG)
  set(_deko_library deko3dd)
  target_compile_definitions(melee_deko PRIVATE COS_DK_DEBUG_LIB=1)
else()
  set(_deko_library deko3d)
endif()
target_link_libraries(melee_deko PRIVATE melee_deko_builtin xxhash uamlib
  PUBLIC dawn::webgpu_dawn ${_deko_library} z nx)

# Aurora's seams (builder/integrate-deko.py edits the staged copy behind this definition).
target_compile_definitions(aurora_core PUBLIC AURORA_GFX_DEKO3D=1)
target_include_directories(aurora_core PRIVATE "${_dk}")
target_include_directories(aurora_gx PRIVATE "${_dk}")
target_link_libraries(aurora_core PUBLIC melee_deko)

# Dawn's component archives (Tint's GLSL writer and reader among them) and deko3d, for the link group
# in switch/CMakeLists.txt. The monolithic archive is the imported dawn::webgpu_dawn.
file(GLOB_RECURSE _dawn_archives "${MELEE_DAWN_BUILD}/*.a")
list(REMOVE_ITEM _dawn_archives "${MELEE_DAWN_BUILD}/src/dawn/native/libwebgpu_dawn.a")
list(SORT _dawn_archives)
# The list is spliced into the link command as one string, so every path is quoted: a checkout under
# a directory with a space (D:/Code Projects/...) would otherwise split.
set(MELEE_GFX_LINK_LIBS "\"${MELEE_DAWN_BUILD}/src/dawn/native/libwebgpu_dawn.a\"")
foreach(_archive IN LISTS _dawn_archives)
  string(APPEND MELEE_GFX_LINK_LIBS " \"${_archive}\"")
endforeach()
string(APPEND MELEE_GFX_LINK_LIBS " -l${_deko_library}")
