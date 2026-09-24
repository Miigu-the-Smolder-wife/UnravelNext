# Build identity: git commit, dirty flag and a hash of the uncommitted diff, regenerated on every build so
# every performance report can name the exact source it came from (a source change invalidates evidence).
include_guard(GLOBAL)

set(UNX_BUILD_IDENTITY_HEADER "${CMAKE_BINARY_DIR}/generated/unx/BuildIdentity.generated.h")
add_custom_target(unx_build_identity
  COMMAND "${CMAKE_COMMAND}" -DSOURCE_DIR=${CMAKE_SOURCE_DIR} -DOUT=${UNX_BUILD_IDENTITY_HEADER}
          -DAGILITY=${UNX_AGILITY_VERSION} -DNVAPI=${UNX_NVAPI_COMMIT}
          -P "${CMAKE_SOURCE_DIR}/cmake/WriteBuildIdentity.cmake"
  BYPRODUCTS "${UNX_BUILD_IDENTITY_HEADER}"
  COMMENT "Build identity"
  VERBATIM)
