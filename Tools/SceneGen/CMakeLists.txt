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
