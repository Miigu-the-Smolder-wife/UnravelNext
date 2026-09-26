# S's weather (B6): the rain shadow map is traced by the R track (RayTracingTrack.cpp) in integrated builds; the weather
# record names the map only when that module exists.
target_compile_definitions(${UNX_MODULE_TARGET} PRIVATE $<$<TARGET_EXISTS:unx_module_raytracing>:UNX_HAS_RAYTRACING=1>)
