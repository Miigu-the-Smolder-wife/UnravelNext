unx_add_executable(unx_unit_tests Unit/UnitTests.cpp Gates/EmptyFrameScene.cpp)
target_include_directories(unx_unit_tests PRIVATE Gates)
target_compile_definitions(unx_unit_tests PRIVATE UNX_SOURCE_DIR="${CMAKE_SOURCE_DIR}")

unx_add_executable(unx_gate_empty_frame Gates/EmptyFrameGate.cpp Gates/EmptyFrameScene.cpp Gates/GraphOverheadExperiments.cpp)
target_compile_definitions(unx_gate_empty_frame PRIVATE UNX_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
