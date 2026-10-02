# The mesh-card surface cache lights its cards through the R track's ray scene.
target_link_libraries(${UNX_MODULE_TARGET} PUBLIC unx_module_raytracing)
# The card capture reads M's texture table (terrain splats, normal maps) when the material module is in the build.
target_link_libraries(${UNX_MODULE_TARGET} PRIVATE $<TARGET_NAME_IF_EXISTS:unx_module_material>)
target_compile_definitions(${UNX_MODULE_TARGET} PRIVATE $<$<TARGET_EXISTS:unx_module_material>:UNX_SC_HAS_MATERIAL=1>)
