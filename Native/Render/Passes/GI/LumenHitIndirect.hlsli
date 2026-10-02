// The indirect light of a ray hit that has no mesh card - a deforming instance (skin, wind), a face no card of its mesh
// sees. The reference gives such a hit nothing; before this file it took the sun and one local-light sample here, so a
// skinned character in a mirror was black wherever no light reached it directly. It now also takes the light around
// it, from what the frame already holds:
//   the translucency volume (LumenTranslucencyVolume.hlsli) where the hit lies in a cell of the main view's grid that
//   holds traced light (ltvSampleLit: cells the view sees into) - its SH's irradiance on the hit's normal;
//   else the radiance cache's irradiance probes (LumenRadianceCache.hlsli lrcIrradiance) where probes around the hit
//   exist and see it; the hit also asks for probes there (lrcHitMark, one hit in 16), so that a surface only a mirror
//   shows has them from the next frame on;
//   else nothing, as before.
// Both sources are occluded the way they are for their other readers: a cell's rays start at the cell's sample point
// kept in front of the depth buffer, and a probe answers only for a point its depth map sees - a closed room takes no
// light from outside through either (the furnace room's day equals its night).
// The sources come through the card frame (CardLayout.hlsli words 22..27): the surface cache writes the previous
// frame's volume with the frame (the cache's own rays, the radiance cache's and the volume's run before this frame's
// volume exists), and the final gather names this frame's radiance cache and then its volume once each stands
// (CardFrameSources.hlsl) - the screen probes' rays and the reflections read those.
#ifndef UNX_LUMEN_HIT_INDIRECT_HLSLI
#define UNX_LUMEN_HIT_INDIRECT_HLSLI
#include "Passes/SurfaceCache/CardLayout.hlsli"
#include "Passes/GI/LumenTranslucencyVolume.hlsli"
#include "Passes/GI/LumenRadianceCache.hlsli"

struct LhiSources
{
    uint volume;        // LtvParams SRV (0xFFFFFFFF: none)
    uint rcParams;      // LrcParams SRV (0xFFFFFFFF: none)
    uint rcIndirection, rcIrradiance, rcDepth;
    uint rcMarks;       // the hit-mark list UAV (0xFFFFFFFF: none)
};
// cardFrameSrv 0xFFFFFFFF (no cards): no source.
LhiSources lhiSources(uint cardFrameSrv)
{
    LhiSources s;
    s.volume = s.rcParams = s.rcIndirection = s.rcIrradiance = s.rcDepth = s.rcMarks = 0xFFFFFFFFu;
    if (cardFrameSrv == 0xFFFFFFFFu) return s;
    ByteAddressBuffer frame = ResourceDescriptorHeap[cardFrameSrv];
    const uint4 a = frame.Load4(MC_FRAME_SOURCES);
    const uint2 b = frame.Load2(MC_FRAME_SOURCES + 16);
    s.volume = a.x, s.rcParams = a.y, s.rcIndirection = a.z, s.rcIrradiance = a.w;
    s.rcDepth = b.x, s.rcMarks = b.y;
    return s;
}

uint lhiHash(uint x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// rgb: the indirect irradiance (lux) on a surface of 'normal' at the hit; a: 1 when a source answered (0: rgb = 0).
// seed: any number of the thread (which hits ask the cache for probes).
float4 lhiIrradiance(LhiSources s, float3 position, float3 normal, uint seed)
{
    if (s.volume != 0xFFFFFFFFu)
    {
        float lit;
        const LtvSh sh = ltvSampleLit(ltvParams(s.volume), position, lit);
        if (lit > 0.05) return float4(ltvIrradianceOf(sh, normal), 1);
    }
    if (s.rcParams != 0xFFFFFFFFu && s.rcIrradiance != 0xFFFFFFFFu)
    {
        if (s.rcMarks != 0xFFFFFFFFu && (lhiHash(seed) & 15u) == 0) lrcHitMark(s.rcMarks, position);
        const float4 e = lrcIrradiance(lrcParams(s.rcParams), s.rcIndirection, s.rcIrradiance, s.rcDepth, position, normal);
        if (e.a > 0) return float4(e.rgb, 1);
    }
    return float4(0, 0, 0, 0);
}

#endif
