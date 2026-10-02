# Core-owned: the scene description shared by every track (INTERFACES_KO.md 6).
add_library(unx_scene STATIC src/SceneData.cpp src/MaterialModel.cpp src/HairModel.cpp src/TerrainPatch.cpp src/MeshCards.cpp)
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

# A9 sheen layer (render A): the model, its albedo table and the scene block (CPU).
add_executable(unx_test_scene_sheen Tests/SheenTests.cpp)
unx_target_defaults(unx_test_scene_sheen)
target_link_libraries(unx_test_scene_sheen PRIVATE unx_scene)
add_executable(unx_test_scene_aniso Tests/AnisoTests.cpp)
unx_target_defaults(unx_test_scene_aniso)
target_link_libraries(unx_test_scene_aniso PRIVATE unx_scene)
# A9 thin film (render A): method (c) against the 1 nm spectral reference, the renderer's table, energy, the scene block.
add_executable(unx_test_scene_film Tests/ThinFilmTests.cpp)
unx_target_defaults(unx_test_scene_film)
target_link_libraries(unx_test_scene_film PRIVATE unx_scene)
target_compile_definitions(unx_test_scene_film PRIVATE UNX_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
# v1.93 (render A): a light's own shadow-ray end bias in the scene file (the optional LEND block).
add_executable(unx_test_scene_lightendbias Tests/LightEndBiasTests.cpp)
unx_target_defaults(unx_test_scene_lightendbias)
target_link_libraries(unx_test_scene_lightendbias PRIVATE unx_scene)
# Mesh cards (render A, MESH_CARDS_INTERFACE_KO.md): the per-mesh card generator (CPU).
add_executable(unx_test_scene_meshcards Tests/MeshCardsTests.cpp)
unx_target_defaults(unx_test_scene_meshcards)
target_link_libraries(unx_test_scene_meshcards PRIVATE unx_scene)
# Hair class (M's hair records): the strand model's twin against its definitions (CPU; Passes/Hair/HairScattering.hlsli).
add_executable(unx_test_scene_hairmodel Tests/HairModelTests.cpp)
unx_target_defaults(unx_test_scene_hairmodel)
target_link_libraries(unx_test_scene_hairmodel PRIVATE unx_scene)
