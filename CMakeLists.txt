cmake_minimum_required(VERSION 3.28)
project(UnravelNext LANGUAGES CXX)

# Shared file (core-owned). Tracks never edit it: their folders are registered automatically (cmake/Modules.cmake).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

include(External/Dependencies.cmake)
include(cmake/Toolchain.cmake)
include(cmake/BuildIdentity.cmake)
include(cmake/Shaders.cmake)
include(cmake/Tracks.cmake)
include(cmake/Modules.cmake)

add_subdirectory(Tools/ShaderCompiler)
add_subdirectory(Native/Core)
add_subdirectory(Native/Scene)
add_subdirectory(Native/Render)
unx_add_tool_folders()
add_subdirectory(Tests)
