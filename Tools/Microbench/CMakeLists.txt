cmake_minimum_required(VERSION 3.24)
project(UnravelMicrobench LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Agility SDK (NuGet) and NVAPI (NVIDIA GitHub submodule), pinned in External/Dependencies.cmake.
include("${CMAKE_CURRENT_SOURCE_DIR}/../../External/Dependencies.cmake")
# Offline shader compiler used at run time (dxcompiler.dll + dxil.dll side by side).
set(MB_DXC_DIR "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64" CACHE PATH "Folder holding dxcompiler.dll and dxil.dll")

add_executable(Microbench src/Microbench.cpp)
target_compile_definitions(Microbench PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
  MB_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/shaders"
  MB_DXC_DIR="${MB_DXC_DIR}"
  MB_RESULT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Results"
  MB_NVAPI_DIR="${UNX_NVAPI_DIR}")
target_compile_options(Microbench PRIVATE /W4 /EHsc /utf-8 /O2 /wd4324 /wd4996)
target_link_libraries(Microbench PRIVATE Unx::AgilitySDK Unx::NVAPI d3d12 dxgi)
unx_deploy_agility(Microbench)
