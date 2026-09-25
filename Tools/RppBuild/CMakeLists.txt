# Owner: RPP (INTERFACES 1, v1.40). Added automatically (cmake/Modules.cmake). CPU tool: RPP-1 world, bodies files, path.
file(GLOB_RECURSE rppbuild_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp")
add_library(unx_rppbuild STATIC ${rppbuild_sources})
unx_target_defaults(unx_rppbuild)
target_include_directories(unx_rppbuild PUBLIC src)
target_link_libraries(unx_rppbuild PUBLIC unx_scenegen unx_scene)

# unx_rppbuild --out <dir> [--seed N]
add_executable(unx_rppbuild_tool Tools/RppBuildTool.cpp)
unx_target_defaults(unx_rppbuild_tool)
set_target_properties(unx_rppbuild_tool PROPERTIES OUTPUT_NAME unx_rppbuild)
target_link_libraries(unx_rppbuild_tool PRIVATE unx_rppbuild)

# unx_gamebench --out <dir> --preset standard|fluid|all  (game bench scenes, Content/RPP1/GameBench/GAMEBENCH_KO.md)
add_executable(unx_gamebench_tool Tools/GameBenchTool.cpp)
unx_target_defaults(unx_gamebench_tool)
set_target_properties(unx_gamebench_tool PROPERTIES OUTPUT_NAME unx_gamebench)
target_link_libraries(unx_gamebench_tool PRIVATE unx_rppbuild)
