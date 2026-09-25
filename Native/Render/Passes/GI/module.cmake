# GI and reflections trace through the R track's ray scene (same track: RayTracing, Passes/GI, Passes/Reflection).
target_link_libraries(${UNX_MODULE_TARGET} PUBLIC unx_module_raytracing)
# The R gate's --integrated mode needs V's cluster builder, present in the integrated build only.
target_link_libraries(${UNX_MODULE_TARGET} INTERFACE $<TARGET_NAME_IF_EXISTS:unx_clusterbuilder>)
target_compile_definitions(${UNX_MODULE_TARGET} INTERFACE $<$<TARGET_EXISTS:unx_clusterbuilder>:UNX_HAS_CLUSTERBUILDER=1>)
