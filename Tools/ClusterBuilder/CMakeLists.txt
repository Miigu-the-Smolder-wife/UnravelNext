# Owner: V (core session). Added automatically (cmake/Modules.cmake) when track V is enabled.
# Cluster LOD builder: meshes -> clusters, LOD DAG, hierarchy nodes, LOD cuts, minimum feature widths (ClusterData).
add_library(unx_clusterbuilder STATIC src/ClusterBuilder.cpp src/FeatureWidth.cpp src/BrickBaker.cpp)
unx_target_defaults(unx_clusterbuilder)
target_include_directories(unx_clusterbuilder PUBLIC include PRIVATE src)
target_link_libraries(unx_clusterbuilder PUBLIC unx_render unx_scene unx_core PRIVATE Unx::meshoptimizer)
# Disk cache identity (C1): SHA-256 of every source that decides the hierarchy (this builder and meshoptimizer).
file(GLOB cb_hash_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*" "${CMAKE_SOURCE_DIR}/External/meshoptimizer/src/*"
  "${CMAKE_SOURCE_DIR}/External/meshoptimizer/demo/clusterlod.h")
list(SORT cb_hash_sources)
set(cb_hash_header "${CMAKE_CURRENT_BINARY_DIR}/generated/ClusterBuilderSourceHash.generated.h")
add_custom_command(OUTPUT "${cb_hash_header}"
  COMMAND "${CMAKE_COMMAND}" "-DFILES=${cb_hash_sources}" "-DOUT=${cb_hash_header}" -P "${CMAKE_CURRENT_SOURCE_DIR}/SourceHash.cmake"
  DEPENDS ${cb_hash_sources} "${CMAKE_CURRENT_SOURCE_DIR}/SourceHash.cmake"
  COMMENT "Cluster builder source hash"
  VERBATIM)
target_sources(unx_clusterbuilder PRIVATE "${cb_hash_header}")
target_include_directories(unx_clusterbuilder PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/generated")

# CPU self-checks on procedural meshes: limits, crack-free cuts, monotone errors, widths, determinism.
add_executable(unx_test_clusterbuilder Tests/ClusterBuilderTests.cpp)
unx_target_defaults(unx_test_clusterbuilder)
target_link_libraries(unx_test_clusterbuilder PRIVATE unx_clusterbuilder unx_agility_exports)
target_include_directories(unx_test_clusterbuilder PRIVATE src)
target_compile_definitions(unx_test_clusterbuilder PRIVATE UNX_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
# With track C: the fill report over C's generated scenes (cluster_fill_of_scene_meshes).
target_link_libraries(unx_test_clusterbuilder PRIVATE $<TARGET_NAME_IF_EXISTS:unx_scenegen>)
target_compile_definitions(unx_test_clusterbuilder PRIVATE $<$<TARGET_EXISTS:unx_scenegen>:UNX_HAS_SCENEGEN=1>)
