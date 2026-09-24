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
  EXCLUDE ${disabled_module_dirs})

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
