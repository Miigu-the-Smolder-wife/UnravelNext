cmake_minimum_required(VERSION 3.24)
# Design-revision microbenchmarks (COVERAGE_REDESIGN_KO.md 7.3, request 20260925_D_coverage_redesign.md 9).
# Standalone project like Tools/Microbench: it must not sit at Tools/DesignBench/CMakeLists.txt, because
# cmake/Modules.cmake adds every Tools/<name>/CMakeLists.txt to the main build and cmake/Tracks.cmake (core) fails
# the configure of a folder without a track. Build with Tools/DesignBench/Build.ps1, run with Run.ps1 (GPU lock).
project(UnravelDesignBench LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Agility SDK (NuGet), pinned in External/Dependencies.cmake (same runtime as the renderer: SDK 618).
include("${CMAKE_CURRENT_SOURCE_DIR}/../../../External/Dependencies.cmake")
set(DB_DXC_DIR "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64" CACHE PATH "Folder holding dxcompiler.dll and dxil.dll")

add_executable(DesignBench src/DesignBench.cpp)
target_compile_definitions(DesignBench PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX
  DB_SHADER_DIR="${CMAKE_CURRENT_SOURCE_DIR}/shaders"
  DB_DXC_DIR="${DB_DXC_DIR}"
  DB_RESULT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../Results")
target_compile_options(DesignBench PRIVATE /W4 /EHsc /utf-8 /O2 /wd4324 /wd4996)
target_link_libraries(DesignBench PRIVATE Unx::AgilitySDK d3d12 dxgi)
unx_deploy_agility(DesignBench)
