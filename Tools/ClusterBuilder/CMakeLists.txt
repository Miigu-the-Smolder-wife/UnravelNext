# Owner: V (core session). Added automatically (cmake/Modules.cmake) when track V is enabled.
# Cluster LOD builder: meshes -> clusters, LOD DAG, hierarchy nodes, LOD cuts, minimum feature widths (ClusterData).
add_library(unx_clusterbuilder STATIC src/ClusterBuilder.cpp src/FeatureWidth.cpp)
unx_target_defaults(unx_clusterbuilder)
target_include_directories(unx_clusterbuilder PUBLIC include PRIVATE src)
target_link_libraries(unx_clusterbuilder PUBLIC unx_render unx_scene unx_core PRIVATE Unx::meshoptimizer)

# CPU self-checks on procedural meshes: limits, crack-free cuts, monotone errors, widths, determinism.
add_executable(unx_test_clusterbuilder Tests/ClusterBuilderTests.cpp)
unx_target_defaults(unx_test_clusterbuilder)
target_link_libraries(unx_test_clusterbuilder PRIVATE unx_clusterbuilder unx_agility_exports)
target_include_directories(unx_test_clusterbuilder PRIVATE src)
target_compile_definitions(unx_test_clusterbuilder PRIVATE UNX_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
