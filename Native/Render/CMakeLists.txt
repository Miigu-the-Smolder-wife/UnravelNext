# Core-owned. Render modules (Passes/<Name>, RayTracing) register themselves by folder (cmake/Modules.cmake).
add_library(unx_render STATIC
  src/Descriptors.cpp
  src/Device.cpp
  src/GpuProfiler.cpp
  src/GpuLock.cpp
  src/Harness.cpp
  src/RenderGraph.cpp
  src/Shaders.cpp)
unx_target_defaults(unx_render)
target_include_directories(unx_render PUBLIC include)
target_link_libraries(unx_render PUBLIC unx_core unx_scene Unx::AgilitySDK d3d12 dxgi PRIVATE Unx::NVAPI)

# Exports D3D12SDKVersion / D3D12SDKPath from every executable that links it (object library: always linked).
add_library(unx_agility_exports OBJECT src/AgilityExports.cpp)
target_link_libraries(unx_agility_exports PRIVATE Unx::AgilitySDK)

# Render module folders and their tracks (cmake/Tracks.cmake).
file(GLOB module_dirs LIST_DIRECTORIES true "${CMAKE_CURRENT_SOURCE_DIR}/Passes/*")
list(APPEND module_dirs "${CMAKE_CURRENT_SOURCE_DIR}/RayTracing")
set(enabled_module_dirs)
set(disabled_module_dirs)
set(disabled_tracks)
foreach(dir ${module_dirs})
  get_filename_component(n "${dir}" NAME)
  if(IS_DIRECTORY "${dir}" AND NOT n STREQUAL "Common" AND NOT n STREQUAL "Test")
    unx_track_of(${n} track)
    unx_track_enabled(${track} track_on)
    if(track_on)
      list(APPEND enabled_module_dirs "${dir}")
    else()
      list(APPEND disabled_module_dirs "${dir}")
      list(APPEND disabled_tracks ${track})
    endif()
  endif()
endforeach()
list(REMOVE_DUPLICATES disabled_tracks)

# Every kernel under Native/Render (Passes/**, RayTracing/**) of the enabled tracks, named by its path relative to
# this folder.
set(UNX_SHADER_OUTPUT "${CMAKE_BINARY_DIR}/bin/shaders" CACHE INTERNAL "")
unx_add_shaders(unx_render_shaders ROOT "${CMAKE_CURRENT_SOURCE_DIR}" OUTPUT "${UNX_SHADER_OUTPUT}"
  INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/Passes/Common" "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/Passes/Atmosphere"  # WindField.hlsli for FX's stream copy (VfxWindTurbulence.hlsli, byte-identical to NativeVfx)
  EXCLUDE ${disabled_module_dirs})

# Shader ABI stamp (coordination 2026-09-26): the headers that fix every kernel's binding contract - root constants
# (Bindless.hlsli), frame constants (Frame.hlsli), scene records (Scene.hlsli) - hashed at configure time. The shader
# target writes the hash next to the kernels (bin/shaders/abi.stamp, after all of them compiled) and the renderer
# compiles it in; ShaderLibrary refuses a kernel folder with another stamp (an executable and kernels from different
# builds removed the device twice after the 32 -> 48 root constant change).
set(unx_abi_text "")
foreach(h Passes/Common/Bindless.hlsli Passes/Common/Frame.hlsli Passes/Common/Scene.hlsli)
  file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/${h}" unx_abi_part)
  string(APPEND unx_abi_text "${unx_abi_part}")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${h}")
endforeach()
string(SHA256 UNX_SHADER_ABI "${unx_abi_text}")
string(SUBSTRING "${UNX_SHADER_ABI}" 0 16 UNX_SHADER_ABI)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/abi.stamp" "${UNX_SHADER_ABI}")
add_custom_command(TARGET unx_render_shaders POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${CMAKE_CURRENT_BINARY_DIR}/abi.stamp" "${UNX_SHADER_OUTPUT}/abi.stamp"
  VERBATIM)
set_source_files_properties(src/Shaders.cpp PROPERTIES COMPILE_DEFINITIONS "UNX_SHADER_ABI=\"${UNX_SHADER_ABI}\"")

# An engine executable: Agility exports, runtime DLLs in bin/D3D12, and the compiled kernels.
function(unx_add_executable name)
  add_executable(${name} ${ARGN})
  unx_target_defaults(${name})
  target_link_libraries(${name} PRIVATE unx_render unx_agility_exports)
  add_dependencies(${name} unx_render_shaders)
  unx_deploy_agility(${name})
endfunction()

# Render modules of the enabled tracks: one per folder.
foreach(dir ${enabled_module_dirs})
  unx_register_render_module("${dir}")
endforeach()

# The frame: orders the tracks' entry points (INTERFACES_KO.md 5) and provides cross-track services. Entry points of
# disabled tracks come from core's empty stubs.
get_property(modules GLOBAL PROPERTY UNX_RENDER_MODULES)
set(stub_sources)
foreach(t ${disabled_tracks})
  list(APPEND stub_sources Frame/Stubs/Track${t}.cpp)
endforeach()
add_library(unx_frame STATIC Frame/FrameRenderer.cpp Frame/GpuScene.cpp ${stub_sources})
unx_target_defaults(unx_frame)
target_link_libraries(unx_frame PUBLIC unx_render unx_scene ${modules})

# Everything a renderer executable needs.
add_library(unx_renderer INTERFACE)
target_link_libraries(unx_renderer INTERFACE unx_frame ${modules})

unx_create_module_executables()
