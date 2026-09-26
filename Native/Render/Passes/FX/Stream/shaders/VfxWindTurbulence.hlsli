// VfxParticleMath's NV_WIND_TURBULENCE hook for HLSL executors (stream executor version 3,
// include/NativeVfxStream.h NV_STREAM_EXECUTOR_WIND_TURBULENCE). Include before the second include of
// VfxParticleMath.hlsli, with the renderer's WindField.hlsli on the include path (UnravelNext
// Passes/Atmosphere/WindField.hlsli; its byte-identical copy Native/RuntimeCommon/WindField.hlsli here): the term is the
// World sampler's (RuntimeCommon/WindTurbulence.h add_turbulence) - wfCurl(d * inv_length, phase, octaves, seed) * rms,
// with inv_length and phase prepared on the CPU in float as the World does.
#ifndef NV_VFX_WIND_TURBULENCE_HLSLI
#define NV_VFX_WIND_TURBULENCE_HLSLI
#include "WindField.hlsli"
#define NV_WIND_TURBULENCE(d, field) (wfCurl((d) * (field).inv_length, (field).phase, (field).octaves_seed & 255u, (field).octaves_seed >> 8) * (field).rms)
#endif
