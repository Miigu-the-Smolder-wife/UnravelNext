# Core-owned: the scene description shared by every track (INTERFACES_KO.md 6).
add_library(unx_scene STATIC src/SceneData.cpp src/MaterialModel.cpp src/TerrainPatch.cpp)
unx_target_defaults(unx_scene)
target_include_directories(unx_scene PUBLIC include)
target_link_libraries(unx_scene PUBLIC unx_core)

# C4 (render C): blend shape and vertex animation data, serialisation and the reference evaluation (CPU).
add_executable(unx_test_scene_morph Tests/SceneMorphTests.cpp)
unx_target_defaults(unx_test_scene_morph)
target_link_libraries(unx_test_scene_morph PRIVATE unx_scene)

# C5 (render C): terrain deformation patches (CPU): replacement meshes of deformed terrain blocks.
add_executable(unx_test_scene_terrainpatch Tests/TerrainPatchTests.cpp)
unx_target_defaults(unx_test_scene_terrainpatch)
target_link_libraries(unx_test_scene_terrainpatch PRIVATE unx_scene)
