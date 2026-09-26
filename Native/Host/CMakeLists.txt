# Integration track I (INTERFACES_KO.md 1): the Unity host. Owner: I.
#   unx_host_core   static library: host logic shared by the plugin DLL, tests and gates
#   unx_host        UnravelNext.dll, the Unity native plugin (C ABI: include/unx/host/UnravelNextHost.h); built when
#                   src/Unity exists
#   Tests/*.cpp     unx_test_host_<file>   correctness runs, no GPU lock
#   Gates/*.cpp     unx_gate_host_<file>   measurements, GPU lock (Tools/CI/GpuLock.ps1 -Track I)
#   Kernels/**.hlsl bin/shaders/Host/<path>.dxil (host-only kernels, e.g. the boundary probe); renderer kernels stay in
#                   bin/shaders
set(UNX_UNITY_PLUGIN_API "C:/Program Files/Unity/Hub/Editor/6000.6.0f1/Editor/Data/PluginAPI" CACHE PATH
  "Unity native plugin API headers (the editor version the old repository uses, ProjectSettings/ProjectVersion.txt)")

unx_add_shaders(unx_host_shaders ROOT "${CMAKE_CURRENT_SOURCE_DIR}/Kernels" OUTPUT "${UNX_SHADER_OUTPUT}/Host"
  INCLUDES "${CMAKE_SOURCE_DIR}/Native/Render/Passes/Common" "${CMAKE_SOURCE_DIR}/Native/Render")

# Engine 1's GPU bridge (src/GpuBridge, 84922cc): its ABI headers must be Unravel's RuntimeCommon files byte for byte.
include(src/GpuBridge/AbiIdentity.cmake)

file(GLOB_RECURSE core_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
list(FILTER core_sources EXCLUDE REGEX "/src/Unity/")
add_library(unx_host_core STATIC ${core_sources})
unx_target_defaults(unx_host_core)
target_include_directories(unx_host_core PUBLIC include src)
target_link_libraries(unx_host_core PUBLIC unx_renderer dxguid $<TARGET_NAME_IF_EXISTS:unx_cook>  # C1: cooked textures (Tools/Cook)
  unx_reference_gpu windowscodecs)  # B11 photo mode: E's GPU tracer on the renderer's device; PNG through WIC
add_dependencies(unx_host_core unx_host_shaders)

file(GLOB_RECURSE unity_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/Unity/*.cpp")
if(unity_sources)
  if(NOT EXISTS "${UNX_UNITY_PLUGIN_API}/IUnityGraphicsD3D12.h")
    message(FATAL_ERROR "UNX_UNITY_PLUGIN_API does not hold the Unity plugin API headers: ${UNX_UNITY_PLUGIN_API}")
  endif()
  add_library(unx_host SHARED ${unity_sources})
  unx_target_defaults(unx_host)
  set_target_properties(unx_host PROPERTIES OUTPUT_NAME UnravelNext)
  target_include_directories(unx_host PRIVATE "${UNX_UNITY_PLUGIN_API}")
  target_link_libraries(unx_host PRIVATE unx_host_core shell32)  # shell32: CommandLineToArgvW (-unxLogFile)
  add_dependencies(unx_host unx_render_shaders unx_host_shaders unx_build_identity)
  # The identity of the source this DLL was linked from travels with it (Deploy.ps1 records it next to the plugin):
  # copied after a successful link only, so a later failed build cannot relabel an older DLL.
  add_custom_command(TARGET unx_host POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E copy "${UNX_BUILD_IDENTITY_HEADER}" "$<TARGET_FILE:unx_host>.identity.h"
    VERBATIM)
endif()

foreach(kind Tests Gates)
  file(GLOB exes CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${kind}/*.cpp")
  string(TOLOWER "${kind}" lower)
  string(REGEX REPLACE "s$" "" prefix "${lower}")
  foreach(exe ${exes})
    get_filename_component(stem "${exe}" NAME_WE)
    string(TOLOWER "${stem}" stem)
    unx_add_executable(unx_${prefix}_host_${stem} "${exe}")
    target_link_libraries(unx_${prefix}_host_${stem} PRIVATE unx_host_core)
    target_compile_definitions(unx_${prefix}_host_${stem} PRIVATE UNX_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_dependencies(unx_${prefix}_host_${stem} unx_host_shaders)
  endforeach()
endforeach()
