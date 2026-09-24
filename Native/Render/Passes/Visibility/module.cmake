# V module: the cluster hierarchy layout (ClusterHierarchy.h) and buffer names come from V's cluster builder.
target_link_libraries(${UNX_MODULE_TARGET} PUBLIC unx_clusterbuilder)
# V's gate renders C's procedural scenes when track C is enabled in the build (the integrated gate build is).
target_link_libraries(${UNX_MODULE_TARGET} INTERFACE $<TARGET_NAME_IF_EXISTS:unx_scenegen>)
target_compile_definitions(${UNX_MODULE_TARGET} INTERFACE $<$<TARGET_EXISTS:unx_scenegen>:UNX_HAS_SCENEGEN=1>)
