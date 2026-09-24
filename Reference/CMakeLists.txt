# Owner: C (reference and content). Added automatically by cmake/Modules.cmake (unx_add_tool_folders).
# Metrics skeleton created by core in P0b; the reference path tracer (Embree, CPU) is added here by C.
add_library(unx_metrics STATIC Metrics/src/Metrics.cpp)
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
