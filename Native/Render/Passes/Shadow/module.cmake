# S's froxel air takes the local lights' in-scattering from light samples with shadow rays (shading.mega_lights_volume,
# FroxelSystem.cpp recordSampledLocal) when the R track's ray scene is in the build; without it the integration keeps its
# loop over the lists.
target_link_libraries(${UNX_MODULE_TARGET} PUBLIC $<TARGET_NAME_IF_EXISTS:unx_module_raytracing>)
target_compile_definitions(${UNX_MODULE_TARGET} PRIVATE $<$<TARGET_EXISTS:unx_module_raytracing>:UNX_S_HAS_RAYTRACING=1>)
