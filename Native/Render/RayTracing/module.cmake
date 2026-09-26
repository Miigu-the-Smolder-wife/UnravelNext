# R module: skin-aware RT proxy simplification (ProxyPoseBound.cpp) uses meshoptimizer.
target_link_libraries(${UNX_MODULE_TARGET} PRIVATE Unx::meshoptimizer)
# Decals at hits (recordDecals) read M's texture table when the material module is in the build.
target_link_libraries(${UNX_MODULE_TARGET} PRIVATE $<TARGET_NAME_IF_EXISTS:unx_module_material>)
target_compile_definitions(${UNX_MODULE_TARGET} PRIVATE $<$<TARGET_EXISTS:unx_module_material>:UNX_HAS_MATERIAL=1>)
