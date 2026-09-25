# Shading reads the material resolve's per-view outputs (same track: Passes/Material, Passes/Shading).
target_link_libraries(${UNX_MODULE_TARGET} PUBLIC unx_module_material)
# The M gate renders C's procedural scenes through V's clusters when those tracks are in the build (the integrated gate
# build is); a core + M build compiles the gate without them and it refuses to run.
target_link_libraries(${UNX_MODULE_TARGET} INTERFACE $<TARGET_NAME_IF_EXISTS:unx_scenegen> $<TARGET_NAME_IF_EXISTS:unx_clusterbuilder>)
target_compile_definitions(${UNX_MODULE_TARGET} INTERFACE $<$<TARGET_EXISTS:unx_scenegen>:UNX_M_HAS_SCENEGEN=1> $<$<TARGET_EXISTS:unx_clusterbuilder>:UNX_M_HAS_CLUSTERBUILDER=1>)
