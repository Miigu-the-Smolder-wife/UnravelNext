add_library(unx_render STATIC
  src/Descriptors.cpp
  src/Device.cpp
  src/GpuProfiler.cpp
  src/Harness.cpp
  src/RenderGraph.cpp
  src/Shaders.cpp)
unx_target_defaults(unx_render)
target_include_directories(unx_render PUBLIC include)
target_link_libraries(unx_render PUBLIC unx_core Unx::AgilitySDK d3d12 dxgi PRIVATE Unx::NVAPI)

# Exports D3D12SDKVersion / D3D12SDKPath from every executable that links it (object library: always linked).
add_library(unx_agility_exports OBJECT src/AgilityExports.cpp)
target_link_libraries(unx_agility_exports PRIVATE Unx::AgilitySDK)

set(UNX_SHADER_OUTPUT "${CMAKE_BINARY_DIR}/bin/shaders" CACHE INTERNAL "")
unx_add_shaders(unx_render_shaders ROOT "${CMAKE_CURRENT_SOURCE_DIR}/Passes" OUTPUT "${UNX_SHADER_OUTPUT}"
  INCLUDES "${CMAKE_CURRENT_SOURCE_DIR}/Passes/Common")

# An engine executable: Agility exports, runtime DLLs in bin/D3D12, and the compiled kernels.
function(unx_add_executable name)
  add_executable(${name} ${ARGN})
  unx_target_defaults(${name})
  target_link_libraries(${name} PRIVATE unx_render unx_agility_exports)
  add_dependencies(${name} unx_render_shaders)
  unx_deploy_agility(${name})
endfunction()
