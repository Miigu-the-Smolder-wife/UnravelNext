# Owner: C. Streaming module extras (cmake/Modules.cmake includes this with UNX_MODULE_TARGET set).
# DirectStorage (NuGet Microsoft.Direct3D.DirectStorage 1.3.0, MIT; user-approved 2026-09-26): the package is fetched once
# into External/.cache and verified by SHA-256 (its NuGet catalog SHA-512 was checked when pinning). dstorage.dll and
# dstoragecore.dll are copied next to the executables (build/<t>/bin).
set(UNX_DSTORAGE_VERSION "1.3.0")
set(UNX_DSTORAGE_SHA256 "8ab6c1082537565388b79050d94fdd7f5f437cd0f1d909e7b19f786042b97177")
if(NOT TARGET Unx::DirectStorage)
  unx_fetch_nuget(Microsoft.Direct3D.DirectStorage ${UNX_DSTORAGE_VERSION} ${UNX_DSTORAGE_SHA256} UNX_DSTORAGE_DIR)
  add_library(Unx::DirectStorage STATIC IMPORTED GLOBAL)
  set_target_properties(Unx::DirectStorage PROPERTIES
    IMPORTED_LOCATION "${UNX_DSTORAGE_DIR}/native/lib/x64/dstorage.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${UNX_DSTORAGE_DIR}/native/include")
  set(UNX_DSTORAGE_BIN "${UNX_DSTORAGE_DIR}/native/bin/x64" CACHE INTERNAL "")
  add_custom_target(unx_dstorage_deploy
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${CMAKE_BINARY_DIR}/bin"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${UNX_DSTORAGE_BIN}/dstorage.dll" "${UNX_DSTORAGE_BIN}/dstoragecore.dll" "${CMAKE_BINARY_DIR}/bin"
    COMMENT "DirectStorage runtime DLLs"
    VERBATIM)
endif()
target_link_libraries(${UNX_MODULE_TARGET} PUBLIC Unx::DirectStorage)
add_dependencies(${UNX_MODULE_TARGET} unx_dstorage_deploy)
