# Pinned third-party dependencies, taken only from their official distributions.
#
#   Agility SDK  NuGet package Microsoft.Direct3D.D3D12, downloaded once into External/.cache and
#                verified by SHA-256 (the package's NuGet catalog SHA-512 was checked when pinning).
#   NVAPI        git submodule External/nvapi (github.com/NVIDIA/nvapi), pinned to the R590 SDK commit,
#                the branch of the measured driver 591.86. Used for OMM on drivers without DXR 1.2.
#   FLIP         git submodule External/flip (github.com/NVlabs/flip, BSD-3), pinned to v1.7: image metric.
#   meshoptimizer git submodule External/meshoptimizer (github.com/zeux/meshoptimizer, MIT), pinned to v1.2: cluster
#                LOD hierarchy (demo/clusterlod.h), meshlets, simplification. Unx::meshoptimizer, used by V's builder.
# Track-specific libraries (Embree, ...) are added here by core on request (INTERFACES_KO.md 2.3).
#
# Changing a version here changes measured floors: re-run Tools/Microbench and record the result.

include_guard(GLOBAL)

set(UNX_EXTERNAL_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(UNX_EXTERNAL_CACHE "${UNX_EXTERNAL_DIR}/.cache")

set(UNX_AGILITY_VERSION "1.618.5")
set(UNX_AGILITY_SDK_VERSION 618)  # D3D12SDKVersion the executables export; must match the package
set(UNX_AGILITY_SHA256 "0027fc24f947c48dbded13ada7d280be221eb651644e23a8a476f0f1f0a079dd")

set(UNX_NVAPI_COMMIT "832a3673d66a0fdf6d6e522468821d5cbd925f23")  # R590-Developer SDK

# Downloads and extracts a NuGet package into External/.cache/<id>.<version>, verifying its hash.
function(unx_fetch_nuget id version sha256 out_var)
  string(TOLOWER "${id}" lower)
  set(dir "${UNX_EXTERNAL_CACHE}/${lower}.${version}")
  set(stamp "${dir}/.unx_sha256")
  if(EXISTS "${stamp}")
    file(READ "${stamp}" have)
    string(STRIP "${have}" have)
  endif()
  if(NOT have STREQUAL sha256)
    set(pkg "${UNX_EXTERNAL_CACHE}/${lower}.${version}.nupkg")
    message(STATUS "Fetching NuGet ${id} ${version}")
    file(DOWNLOAD "https://api.nuget.org/v3-flatcontainer/${lower}/${version}/${lower}.${version}.nupkg" "${pkg}"
      EXPECTED_HASH SHA256=${sha256} TLS_VERIFY ON STATUS st)
    list(GET st 0 code)
    if(NOT code EQUAL 0)
      message(FATAL_ERROR "NuGet ${id} ${version} download failed: ${st}")
    endif()
    file(REMOVE_RECURSE "${dir}")
    file(ARCHIVE_EXTRACT INPUT "${pkg}" DESTINATION "${dir}")
    file(WRITE "${stamp}" "${sha256}\n")
  endif()
  set(${out_var} "${dir}" PARENT_SCOPE)
endfunction()

if(NOT TARGET Unx::AgilitySDK)
  unx_fetch_nuget(Microsoft.Direct3D.D3D12 ${UNX_AGILITY_VERSION} ${UNX_AGILITY_SHA256} UNX_AGILITY_DIR)
  set(UNX_AGILITY_DIR "${UNX_AGILITY_DIR}" CACHE INTERNAL "")
  set(UNX_AGILITY_BIN "${UNX_AGILITY_DIR}/build/native/bin/x64" CACHE INTERNAL "")
  add_library(Unx::AgilitySDK INTERFACE IMPORTED GLOBAL)
  # The package headers come first so d3d12.h matches the runtime (the Windows SDK copy is older).
  set_target_properties(Unx::AgilitySDK PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${UNX_AGILITY_DIR}/build/native/include"
    INTERFACE_COMPILE_DEFINITIONS "UNX_AGILITY_SDK_VERSION=${UNX_AGILITY_SDK_VERSION}")
endif()

if(NOT TARGET Unx::NVAPI)
  set(nv "${UNX_EXTERNAL_DIR}/nvapi")
  if(NOT EXISTS "${nv}/nvapi.h" OR NOT EXISTS "${nv}/amd64/nvapi64.lib")
    message(FATAL_ERROR "External/nvapi is missing. Run: git submodule update --init External/nvapi")
  endif()
  if(EXISTS "${nv}/.git")
    execute_process(COMMAND git -C "${nv}" rev-parse HEAD OUTPUT_VARIABLE head OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(head AND NOT head STREQUAL UNX_NVAPI_COMMIT)
      message(FATAL_ERROR "External/nvapi is at ${head}, expected ${UNX_NVAPI_COMMIT}. Run: git submodule update External/nvapi")
    endif()
  endif()
  set(UNX_NVAPI_DIR "${nv}" CACHE INTERNAL "")
  add_library(Unx::NVAPI STATIC IMPORTED GLOBAL)
  set_target_properties(Unx::NVAPI PROPERTIES
    IMPORTED_LOCATION "${nv}/amd64/nvapi64.lib"
    INTERFACE_INCLUDE_DIRECTORIES "${nv}")
endif()

set(UNX_FLIP_COMMIT "b475eb4bf394ab877c42166c9eb0a84a02cc5b14")  # NVlabs/flip v1.7 (BSD-3)
if(NOT TARGET Unx::FLIP)
  set(flip "${UNX_EXTERNAL_DIR}/flip")
  if(NOT EXISTS "${flip}/src/cpp/FLIP.h")
    message(FATAL_ERROR "External/flip is missing. Run: git submodule update --init External/flip")
  endif()
  if(EXISTS "${flip}/.git")
    execute_process(COMMAND git -C "${flip}" rev-parse HEAD OUTPUT_VARIABLE head OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(head AND NOT head STREQUAL UNX_FLIP_COMMIT)
      message(FATAL_ERROR "External/flip is at ${head}, expected ${UNX_FLIP_COMMIT}. Run: git submodule update External/flip")
    endif()
  endif()
  add_library(Unx::FLIP INTERFACE IMPORTED GLOBAL)
  set_target_properties(Unx::FLIP PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${flip}/src/cpp")
endif()

set(UNX_MESHOPTIMIZER_COMMIT "9d9890c73011d75920af614485296d1e03e95448")  # zeux/meshoptimizer v1.2 (MIT)
if(NOT TARGET Unx::meshoptimizer)
  set(mo "${UNX_EXTERNAL_DIR}/meshoptimizer")
  if(NOT EXISTS "${mo}/src/meshoptimizer.h" OR NOT EXISTS "${mo}/demo/clusterlod.h")
    message(FATAL_ERROR "External/meshoptimizer is missing. Run: git submodule update --init External/meshoptimizer")
  endif()
  if(EXISTS "${mo}/.git")
    execute_process(COMMAND git -C "${mo}" rev-parse HEAD OUTPUT_VARIABLE head OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    if(head AND NOT head STREQUAL UNX_MESHOPTIMIZER_COMMIT)
      message(FATAL_ERROR "External/meshoptimizer is at ${head}, expected ${UNX_MESHOPTIMIZER_COMMIT}. Run: git submodule update External/meshoptimizer")
    endif()
  endif()
  file(GLOB mo_sources CONFIGURE_DEPENDS "${mo}/src/*.cpp")
  add_library(unx_meshoptimizer STATIC ${mo_sources})
  # demo/ holds clusterlod.h (header-only, CLUSTERLOD_IMPLEMENTATION in one translation unit of the user).
  target_include_directories(unx_meshoptimizer PUBLIC "${mo}/src" "${mo}/demo")
  set_target_properties(unx_meshoptimizer PROPERTIES FOLDER External)
  add_library(Unx::meshoptimizer ALIAS unx_meshoptimizer)
endif()

# Copies the Agility runtime next to an executable (D3D12/ subfolder, matching D3D12SDKPath ".\\D3D12\\").
# The SDK layers DLL is copied too so the debug layer uses the same runtime version.
function(unx_deploy_agility target)
  add_custom_command(TARGET ${target} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "$<TARGET_FILE_DIR:${target}>/D3D12"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${UNX_AGILITY_BIN}/D3D12Core.dll" "$<TARGET_FILE_DIR:${target}>/D3D12/D3D12Core.dll"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${UNX_AGILITY_BIN}/d3d12SDKLayers.dll" "$<TARGET_FILE_DIR:${target}>/D3D12/d3d12SDKLayers.dll"
    VERBATIM)
endfunction()
