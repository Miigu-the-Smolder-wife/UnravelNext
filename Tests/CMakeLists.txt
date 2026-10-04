# Core-owned tests and gates. Track tests and gates live in their own folders (Native/Render/<module>/Tests, Gates).
# Pure CPU consumer-index equivalence test; no renderer, D3D12 or shaders.
add_executable(unx_test_scene_update_writer Unit/SceneUpdateWriter.cpp)
unx_target_defaults(unx_test_scene_update_writer)

add_executable(unx_test_runtime_dirty_ranges Unit/RuntimeDirtyRanges.cpp)
unx_target_defaults(unx_test_runtime_dirty_ranges)

add_executable(unx_test_sorted_prefix Unit/SortedPrefix.cpp)
unx_target_defaults(unx_test_sorted_prefix)

add_executable(unx_test_mesh_card_scene_cpu
  ../Native/Render/Passes/SurfaceCache/Tests/MeshCardScene.cpp
  ../Native/Render/Passes/SurfaceCache/MeshCardScene.cpp)
unx_target_defaults(unx_test_mesh_card_scene_cpu)
target_include_directories(unx_test_mesh_card_scene_cpu PRIVATE ../Native/Render/Passes/SurfaceCache/include)
target_link_libraries(unx_test_mesh_card_scene_cpu PRIVATE unx_scene)

add_executable(unx_test_skeleton_instances Unit/SkeletonInstances.cpp)
unx_target_defaults(unx_test_skeleton_instances)
target_include_directories(unx_test_skeleton_instances PRIVATE
  ../Native/Core/include ../Native/Scene/include ../Native/Render/include)

unx_add_executable(unx_unit_tests Unit/UnitTests.cpp Gates/EmptyFrameScene.cpp)
target_include_directories(unx_unit_tests PRIVATE Gates)
target_link_libraries(unx_unit_tests PRIVATE unx_renderer)
target_compile_definitions(unx_unit_tests PRIVATE UNX_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
# With track V enabled, V's visibility entry needs the scene's cluster hierarchy: the frame test builds it.
if(TARGET unx_clusterbuilder)
  target_link_libraries(unx_unit_tests PRIVATE unx_clusterbuilder)
  target_compile_definitions(unx_unit_tests PRIVATE UNX_HAS_CLUSTERBUILDER=1)
endif()

unx_add_executable(unx_gate_empty_frame Gates/EmptyFrameGate.cpp Gates/EmptyFrameScene.cpp Gates/GraphOverheadExperiments.cpp)
target_link_libraries(unx_gate_empty_frame PRIVATE unx_renderer)
target_compile_definitions(unx_gate_empty_frame PRIVATE UNX_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
