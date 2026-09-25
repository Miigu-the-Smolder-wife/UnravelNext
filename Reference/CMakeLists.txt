# Owner: C (reference and content). Added automatically by cmake/Modules.cmake (unx_add_tool_folders).
# Metrics skeleton created by core in P0b; the reference path tracer (Embree, CPU) is added here by C.
add_library(unx_metrics STATIC Metrics/src/Metrics.cpp Metrics/src/Census.cpp)
unx_target_defaults(unx_metrics)
target_include_directories(unx_metrics PUBLIC Metrics/include)
target_link_libraries(unx_metrics PUBLIC unx_core PRIVATE Unx::FLIP)

add_executable(unx_metrics_tool Metrics/Tools/MetricsTool.cpp)
unx_target_defaults(unx_metrics_tool)
set_target_properties(unx_metrics_tool PROPERTIES OUTPUT_NAME unx_metrics)
target_link_libraries(unx_metrics_tool PRIVATE unx_metrics)

add_executable(unx_test_metrics Metrics/Tests/MetricsTests.cpp)
unx_target_defaults(unx_test_metrics)
target_link_libraries(unx_test_metrics PRIVATE unx_metrics)

# Embree 4.4.1 (Apache-2.0, with oneTBB): requested from core as Unx::Embree (Docs/Design/Requests/20260925_C_embree.md).
# Until core pins it in External/Dependencies.cmake, the same official release archive is fetched into this build folder
# with the same SHA-256; once core defines the target this block is skipped.
if(NOT TARGET Unx::Embree)
  set(c_embree_version "4.4.1")
  set(c_embree_sha256 "5aa1fa3161a720f9c610b06c652b80f421cb2b1fe8221fdf92b74157336fdf8f")
  set(c_embree_root "${CMAKE_BINARY_DIR}/_deps/embree-${c_embree_version}")
  set(c_embree_dir "${c_embree_root}/embree-${c_embree_version}.x64.windows")
  if(NOT EXISTS "${c_embree_dir}/lib/embree4.lib")
    set(archive "${c_embree_root}.zip")
    message(STATUS "Fetching Embree ${c_embree_version} (official release, SHA-256 pinned)")
    file(DOWNLOAD "https://github.com/RenderKit/embree/releases/download/v${c_embree_version}/embree-${c_embree_version}.x64.windows.zip" "${archive}"
      EXPECTED_HASH SHA256=${c_embree_sha256} TLS_VERIFY ON STATUS st)
    list(GET st 0 code)
    if(NOT code EQUAL 0)
      message(FATAL_ERROR "Embree download failed: ${st}")
    endif()
    file(ARCHIVE_EXTRACT INPUT "${archive}" DESTINATION "${c_embree_root}")
  endif()
  add_library(Unx::Embree SHARED IMPORTED GLOBAL)
  set_target_properties(Unx::Embree PROPERTIES
    IMPORTED_IMPLIB "${c_embree_dir}/lib/embree4.lib"
    IMPORTED_LOCATION "${c_embree_dir}/bin/embree4.dll"
    INTERFACE_INCLUDE_DIRECTORIES "${c_embree_dir}/include")
  set(UNX_EMBREE_RUNTIME "${c_embree_dir}/bin/embree4.dll;${c_embree_dir}/bin/tbb12.dll;${c_embree_dir}/bin/tbbmalloc.dll")
endif()
function(c_deploy_embree target)
  if(COMMAND unx_deploy_embree)
    unx_deploy_embree(${target})
  else()
    foreach(dll ${UNX_EMBREE_RUNTIME})
      add_custom_command(TARGET ${target} POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${dll}" "$<TARGET_FILE_DIR:${target}>" VERBATIM)
    endforeach()
  endif()
endfunction()

# Reference path tracer (INTERFACES_KO.md 10.2).
file(GLOB reference_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/PathTracer/src/*.cpp")
add_library(unx_reference STATIC ${reference_sources})
unx_target_defaults(unx_reference)
target_include_directories(unx_reference PUBLIC PathTracer/include PRIVATE PathTracer/src)
target_link_libraries(unx_reference PUBLIC unx_metrics unx_scene PRIVATE Unx::Embree)

# unx_reference: render / cache reference images, census (see PathTracer/Tools/ReferenceTool.cpp).
add_executable(unx_reference_tool PathTracer/Tools/ReferenceTool.cpp)
unx_target_defaults(unx_reference_tool)
set_target_properties(unx_reference_tool PROPERTIES OUTPUT_NAME unx_reference)
target_link_libraries(unx_reference_tool PRIVATE unx_reference unx_scenegen)
target_compile_definitions(unx_reference_tool PRIVATE UNX_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
c_deploy_embree(unx_reference_tool)

# Correctness checks of the estimator against closed forms (no GPU).
add_executable(unx_test_reference PathTracer/Tests/ReferenceTests.cpp)
unx_target_defaults(unx_test_reference)
target_include_directories(unx_test_reference PRIVATE PathTracer/src)
target_link_libraries(unx_test_reference PRIVATE unx_reference unx_scenegen Unx::Embree)
target_compile_definitions(unx_test_reference PRIVATE UNX_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
c_deploy_embree(unx_test_reference)

# Census cross-check against the microbench's GPU census (same scene rebuilt from the same RNG stream).
add_executable(unx_test_census_replica PathTracer/Tests/CensusReplica.cpp)
unx_target_defaults(unx_test_census_replica)
target_link_libraries(unx_test_census_replica PRIVATE unx_reference)
c_deploy_embree(unx_test_census_replica)

# Material v2 approximation studies (Docs/Design/MATERIAL_LAYERS_KO.md 3): thin film vs spectral Airy, clearcoat vs a
# Monte Carlo layered model. Data (CIE 1931 CMFs, Johnson & Christy n,k) in Studies/data.
add_executable(unx_study_material_layers Studies/MaterialLayers.cpp)
unx_target_defaults(unx_study_material_layers)
target_include_directories(unx_study_material_layers PRIVATE PathTracer/src)
target_link_libraries(unx_study_material_layers PRIVATE unx_reference Unx::Embree)
target_compile_definitions(unx_study_material_layers PRIVATE UNX_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
c_deploy_embree(unx_study_material_layers)
