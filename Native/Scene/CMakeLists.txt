# Core-owned: the scene description shared by every track (INTERFACES_KO.md 6).
add_library(unx_scene STATIC src/SceneData.cpp src/MaterialModel.cpp)
unx_target_defaults(unx_scene)
target_include_directories(unx_scene PUBLIC include)
target_link_libraries(unx_scene PUBLIC unx_core)
