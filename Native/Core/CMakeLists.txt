add_library(unx_core STATIC
  src/Config.cpp
  src/Jobs.cpp
  src/Log.cpp
  src/Sha256.cpp
  src/File.cpp)
unx_target_defaults(unx_core)
target_include_directories(unx_core PUBLIC include "${CMAKE_BINARY_DIR}/generated")
add_dependencies(unx_core unx_build_identity)
