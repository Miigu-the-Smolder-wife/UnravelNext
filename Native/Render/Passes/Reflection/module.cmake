# Reflections trace through the R track's ray scene and read the GI cache (same track).
target_link_libraries(${UNX_MODULE_TARGET} PUBLIC unx_module_raytracing unx_module_gi)
# Screen traces read the previous frame's colour from M's temporal upscale (shading::upscalePreviousColor) when that
# track is in the build; without it the screen traces are skipped (world rays only).
target_link_libraries(${UNX_MODULE_TARGET} PUBLIC $<TARGET_NAME_IF_EXISTS:unx_module_shading>)
target_compile_definitions(${UNX_MODULE_TARGET} PRIVATE $<$<TARGET_EXISTS:unx_module_shading>:UNX_R_HAS_SHADING=1>)
