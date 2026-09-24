# Core-owned tests and gates. Track tests and gates live in their own folders (Native/Render/<module>/Tests, Gates).
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
