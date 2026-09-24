# Track selection (INTERFACES_KO.md 2.5, 3.1). Core-owned.
#
# The four sessions share one working tree, so one track's unfinished file must not break another track's build.
# UNX_TRACKS lists the tracks whose folders are compiled (core is always on): "all" (integrated build, used for gate
# measurements) or a subset of V;M;S;R;C. A disabled track's render modules, kernels, tools, tests and gates are not
# configured; its entry points (Tracks.h) come from core's empty stubs (Native/Render/Frame/Stubs), which declare no
# passes. Tools/CI/Build.ps1 -Track <name> selects the tracks of that session.
include_guard(GLOBAL)

set(UNX_TRACKS "all" CACHE STRING "Enabled tracks: all, or a list of V;M;S;R;C (core is always on)")
set(UNX_ALL_TRACKS V M S R C)

if(NOT UNX_TRACKS STREQUAL "all")
  foreach(t ${UNX_TRACKS})
    if(NOT t IN_LIST UNX_ALL_TRACKS)
      message(FATAL_ERROR "UNX_TRACKS: unknown track '${t}' (known: all or ${UNX_ALL_TRACKS})")
    endif()
  endforeach()
endif()
message(STATUS "UnravelNext tracks: ${UNX_TRACKS}")

# Folder -> track (INTERFACES_KO.md 1). A new module or tool folder is added here by core.
set(UNX_TRACK_OF_Visibility V)
set(UNX_TRACK_OF_Material M)
set(UNX_TRACK_OF_Shading M)
set(UNX_TRACK_OF_Shadow S)
set(UNX_TRACK_OF_Atmosphere S)
set(UNX_TRACK_OF_RayTracing R)
set(UNX_TRACK_OF_GI R)
set(UNX_TRACK_OF_Reflection R)
set(UNX_TRACK_OF_ClusterBuilder V)
set(UNX_TRACK_OF_SceneGen C)
set(UNX_TRACK_OF_Reference C)

function(unx_track_of folder out)
  if(NOT DEFINED UNX_TRACK_OF_${folder})
    message(FATAL_ERROR "Folder '${folder}' has no track: add it to cmake/Tracks.cmake (core) and INTERFACES_KO.md 1")
  endif()
  set(${out} ${UNX_TRACK_OF_${folder}} PARENT_SCOPE)
endfunction()

function(unx_track_enabled track out)
  if(UNX_TRACKS STREQUAL "all" OR track IN_LIST UNX_TRACKS)
    set(${out} TRUE PARENT_SCOPE)
  else()
    set(${out} FALSE PARENT_SCOPE)
  endif()
endfunction()

# Is the folder (render module or tool) compiled in this build? (Result variables must not be named like if()
# constants such as "on": if(on) is always true.)
function(unx_folder_enabled folder out)
  unx_track_of(${folder} track)
  unx_track_enabled(${track} track_on)
  set(${out} ${track_on} PARENT_SCOPE)
endfunction()
