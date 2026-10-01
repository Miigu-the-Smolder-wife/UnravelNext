// gi.lumen: how the screen probes use A's far-field radiance cache (Passes/GI/LumenRadianceCache.hlsli;
// lumen.radiance_cache). A probe's clipmap is chosen with one dither value per probe and frame, the same where the
// probe marks its cells (LgRcMark.hlsl), where its rays stop and read the cache (LgTrace.hlsl) and where its lighting
// density reads it (LgLightingPdf.hlsl).
#ifndef UNX_GI_LUMEN_RADIANCE_CACHE_HLSLI
#define UNX_GI_LUMEN_RADIANCE_CACHE_HLSLI
#include "Passes/GI/Lumen/LgCommon.hlsli"
#include "Passes/GI/LumenRadianceCache.hlsli"

float lgRcDither(uint2 atlas) { return lgUnit(lgHash(atlas.x * 0x9E3779B1u + atlas.y * 0x85EBCA77u + lgFrame() * 0x27D4EB2Fu)); }
#endif
