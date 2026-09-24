# Owner: C. Added automatically (cmake/Modules.cmake). The generator's sources go in src/; until they exist the
# library is interface-only (the API header of INTERFACES_KO.md 10.1).
file(GLOB_RECURSE scenegen_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
if(scenegen_sources)
  add_library(unx_scenegen STATIC ${scenegen_sources})
  unx_target_defaults(unx_scenegen)
  target_include_directories(unx_scenegen PUBLIC include)
  target_link_libraries(unx_scenegen PUBLIC unx_scene)
else()
  add_library(unx_scenegen INTERFACE)
  target_include_directories(unx_scenegen INTERFACE include)
  target_link_libraries(unx_scenegen INTERFACE unx_scene)
endif()

# unx_scenegen --list | --scene <name|all> [--seed N] [--scale S] [--out <dir>]: writes .unxscene files and statistics.
add_executable(unx_scenegen_tool Tools/SceneGenTool.cpp)
unx_target_defaults(unx_scenegen_tool)
set_target_properties(unx_scenegen_tool PROPERTIES OUTPUT_NAME unx_scenegen)
target_link_libraries(unx_scenegen_tool PRIVATE unx_scenegen)

# Self-checks: determinism (same request = same contentHash), validate(), cameras/paths, gate light counts.
add_executable(unx_test_scenegen Tests/SceneGenTests.cpp)
unx_target_defaults(unx_test_scenegen)
target_link_libraries(unx_test_scenegen PRIVATE unx_scenegen)
