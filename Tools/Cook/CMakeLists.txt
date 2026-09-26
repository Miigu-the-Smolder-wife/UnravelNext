# Owner: C (render C session). Added automatically (cmake/Modules.cmake) when track V is enabled (cmake/Tracks.cmake).
# Asset cooking (C1, plan 13.5): scene textures -> their stored GPU mip chains (block-compressed where the measured error
# bound holds, TextureCook.h), with a memory cache and a disk cache keyed by content.

if(NOT TARGET unx_module_material)
  message(STATUS "Tools/Cook: needs track M (the renderer's mip chain builder); not built")
  return()
endif()

# ---- DirectXTex (External/DirectXTex, MIT; may2026): BC4/BC5 on the CPU, BC7 on the GPU (D3D11 compute) -------------------
set(dxt "${CMAKE_SOURCE_DIR}/External/DirectXTex/DirectXTex")
file(GLOB fxc_candidates "C:/Program Files (x86)/Windows Kits/10/bin/10.*/x64/fxc.exe")
list(SORT fxc_candidates COMPARE NATURAL ORDER DESCENDING)
list(GET fxc_candidates 0 UNX_FXC)
if(NOT EXISTS "${UNX_FXC}")
  message(FATAL_ERROR "Tools/Cook: fxc.exe (Windows SDK) not found; it compiles DirectXTex's BC7 encoder kernels")
endif()
set(dxt_shader_out "${CMAKE_CURRENT_BINARY_DIR}/DirectXTexShaders")
set(dxt_shader_headers)
# As DirectXTex's own Shaders/CompileShaders.cmd: cs_5_0 and cs_4_0 (EMULATE_F16C) of every entry point.
foreach(pair BC7Encode:TryMode456CS BC7Encode:TryMode137CS BC7Encode:TryMode02CS BC7Encode:EncodeBlockCS
             BC6HEncode:TryModeG10CS BC6HEncode:TryModeLE10CS BC6HEncode:EncodeBlockCS)
  string(REPLACE ":" ";" parts "${pair}")
  list(GET parts 0 file)
  list(GET parts 1 entry)
  set(h5 "${dxt_shader_out}/${file}_${entry}.inc")
  set(h4 "${dxt_shader_out}/${file}_${entry}_cs40.inc")
  add_custom_command(OUTPUT "${h5}" "${h4}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${dxt_shader_out}"
    COMMAND "${UNX_FXC}" "${dxt}/Shaders/${file}.hlsl" /nologo /WX /Ges /Zpc /Qstrip_reflect /Qstrip_debug /Tcs_5_0 /E${entry} "/Fh${h5}" /Vn${file}_${entry}
    COMMAND "${UNX_FXC}" "${dxt}/Shaders/${file}.hlsl" /nologo /WX /Ges /Zpc /Qstrip_reflect /Qstrip_debug /Tcs_4_0 /DEMULATE_F16C /E${entry} "/Fh${h4}" /Vn${file}_${entry}
    DEPENDS "${dxt}/Shaders/${file}.hlsl"
    COMMENT "DirectXTex kernel ${file} ${entry}"
    VERBATIM)
  list(APPEND dxt_shader_headers "${h5}" "${h4}")
endforeach()
file(GLOB dxt_sources CONFIGURE_DEPENDS "${dxt}/*.cpp")
list(REMOVE_ITEM dxt_sources "${dxt}/DirectXTexD3D12.cpp")  # D3D12 helpers need d3dx12.h (not vendored); unused
add_library(unx_directxtex STATIC ${dxt_sources} ${dxt_shader_headers})
target_include_directories(unx_directxtex PUBLIC "${dxt}" PRIVATE "${dxt_shader_out}")
target_compile_definitions(unx_directxtex PRIVATE _WIN32_WINNT=0x0A00 NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
target_compile_options(unx_directxtex PRIVATE /W3 /EHsc /permissive- /utf-8 /MP $<$<CONFIG:Release>:/O2 /Oi /Zi>)
target_link_libraries(unx_directxtex PUBLIC d3d11 dxgi ole32 windowscodecs)
set_target_properties(unx_directxtex PROPERTIES FOLDER External)

# ---- Cook library --------------------------------------------------------------------------------------------------------
# Disk cache identity: SHA-256 of every source that decides a cooked chain (this library, the renderer's mip builder
# TextureSystem.cpp, DirectXTex), so a code change invalidates every entry.
file(GLOB cook_hash_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*" "${CMAKE_CURRENT_SOURCE_DIR}/include/unx/cook/*"
  "${CMAKE_SOURCE_DIR}/Native/Render/Passes/Material/TextureSystem.cpp" "${dxt}/*.cpp" "${dxt}/*.h" "${dxt}/Shaders/*.hlsl")
list(SORT cook_hash_sources)
set(cook_hash_header "${CMAKE_CURRENT_BINARY_DIR}/generated/CookSourceHash.generated.h")
add_custom_command(OUTPUT "${cook_hash_header}"
  COMMAND "${CMAKE_COMMAND}" "-DFILES=${cook_hash_sources}" "-DOUT=${cook_hash_header}" -DNAME=UNX_COOK_SOURCE_HASH
          -P "${CMAKE_CURRENT_SOURCE_DIR}/SourceHash.cmake"
  DEPENDS ${cook_hash_sources} "${CMAKE_CURRENT_SOURCE_DIR}/SourceHash.cmake"
  COMMENT "Cook source hash"
  VERBATIM)

add_library(unx_cook STATIC src/TextureCook.cpp src/CookCache.cpp "${cook_hash_header}")
unx_target_defaults(unx_cook)
target_include_directories(unx_cook PUBLIC include PRIVATE src "${CMAKE_CURRENT_BINARY_DIR}/generated")
target_link_libraries(unx_cook PUBLIC unx_render unx_scene unx_core unx_module_material PRIVATE unx_directxtex)

# CPU and GPU (D3D11 BC7 encoder) checks: first hardware run under the GPU lock (Tools/CI/GpuLock.ps1).
add_executable(unx_test_cook Tests/CookTests.cpp)
unx_target_defaults(unx_test_cook)
target_link_libraries(unx_test_cook PRIVATE unx_cook unx_directxtex unx_agility_exports)
target_compile_definitions(unx_test_cook PRIVATE UNX_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
