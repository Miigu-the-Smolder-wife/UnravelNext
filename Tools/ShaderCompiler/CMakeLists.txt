add_executable(unx_shaderc ShaderCompiler.cpp)
unx_target_defaults(unx_shaderc)
target_compile_definitions(unx_shaderc PRIVATE UNX_DXC_DIR=L"${UNX_DXC_DIR}")
