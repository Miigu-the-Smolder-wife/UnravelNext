# GI and reflections trace through the R track's ray scene (same track: RayTracing, Passes/GI, Passes/Reflection).
target_link_libraries(${UNX_MODULE_TARGET} PUBLIC unx_module_raytracing)
# The R gate's --integrated mode needs V's cluster builder, present in the integrated build only.
target_link_libraries(${UNX_MODULE_TARGET} INTERFACE $<TARGET_NAME_IF_EXISTS:unx_clusterbuilder>)
target_compile_definitions(${UNX_MODULE_TARGET} INTERFACE $<$<TARGET_EXISTS:unx_clusterbuilder>:UNX_HAS_CLUSTERBUILDER=1>)
# gi.lumen_cap_snap_exposure asks M's automatic exposure whether the frame is a snap frame (shading::exposureSnapping)
# when that track is in the build; without it the cap keeps the frame's exposure.
target_link_libraries(${UNX_MODULE_TARGET} PUBLIC $<TARGET_NAME_IF_EXISTS:unx_module_shading>)
target_compile_definitions(${UNX_MODULE_TARGET} PRIVATE $<$<TARGET_EXISTS:unx_module_shading>:UNX_GI_HAS_SHADING=1>)
