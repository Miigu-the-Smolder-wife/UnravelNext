# Compiler settings shared by every native target.
include_guard(GLOBAL)

# DXC from the Windows SDK (1.8.2502.11). Used by the build-time shader compiler only.
set(UNX_DXC_DIR "C:/Program Files (x86)/Windows Kits/10/bin/10.0.26100.0/x64" CACHE PATH "Folder holding dxcompiler.dll and dxil.dll")

function(unx_target_defaults target)
  target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8 /EHsc /Zc:__cplusplus /MP
    /wd4324  # structure padded due to alignment specifier
    $<$<CONFIG:Release>:/O2 /Oi /GS- /Zi>)
  target_compile_definitions(${target} PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
  if(MSVC)
    target_link_options(${target} PRIVATE $<$<CONFIG:Release>:/DEBUG /OPT:REF /OPT:ICF>)
  endif()
endfunction()
