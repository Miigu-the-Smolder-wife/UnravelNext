cmake_minimum_required(VERSION 3.24)
project(UnravelMicrobench LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Agility SDK 1.618.x runtime (D3D12SDKVersion 618). Copied next to the executable, never installed.
set(MB_AGILITY_DIR "C:/Program Files/Epic Games/UE_5.8/Engine/Binaries/Win64/D3D12/x64" CACHE PATH "Folder holding D3D12Core.dll (1.618.x)")
# Offline shader compiler used at run time (dxcompiler.dll + dxil.dll side by side).
set(MB_DXC_DIR "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64" CACHE PATH "Folder holding dxcompiler.dll and dxil.dll")
# NVAPI SDK (headers + nvapi64.lib), used read-only from the Unreal ThirdParty folder for OMM/SER on drivers without DXR 1.2.
set(MB_NVAPI_DIR "C:/Program Files/Epic Games/UE_5.8/Engine/Source/ThirdParty/NVIDIA/nvapi" CACHE PATH "Folder holding nvapi.h and amd64/nvapi64.lib")

add_executable(Microbench src/Microbench.cpp)
target_compile_definitions(Microbench PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
  MB_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/shaders"
  MB_DXC_DIR="${MB_DXC_DIR}"
  MB_RESULT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/Results"
  MB_NVAPI_DIR="${MB_NVAPI_DIR}")
target_compile_options(Microbench PRIVATE /W4 /EHsc /utf-8 /O2 /wd4324 /wd4996)
target_include_directories(Microbench PRIVATE "${MB_NVAPI_DIR}")
target_link_libraries(Microbench PRIVATE d3d12 dxgi "${MB_NVAPI_DIR}/amd64/nvapi64.lib")
add_custom_command(TARGET Microbench POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:Microbench>/D3D12"
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${MB_AGILITY_DIR}/D3D12Core.dll" "$<TARGET_FILE_DIR:Microbench>/D3D12/D3D12Core.dll"
  VERBATIM)
