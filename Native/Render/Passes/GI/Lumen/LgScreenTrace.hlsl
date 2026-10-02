// unx-kernel: cs_6_6 main
// gi.lumen, r.gi.lg.screentrace (gi.lumen_screen_traces): the probes' rays walk the depth pyramid first (S2's shared
// screen trace, Passes/Reflection/ScreenTrace.hlsli - one trace for the reflection rays and these, as Lumen's
// LumenScreenTracing). Thread = trace texel. A certain hit (not behind a thin feature: the thickness steps; the
// reference runs none while it skips foliage hits, its default) whose point was on screen and visible last frame takes
// last frame's colour there and is final: LgTrace skips its world ray. A hit on a Foliage surface is not taken (the
// reference's SkipFoliageHits: a leaf's screen colour is its lit side's; the world ray goes on). Otherwise the
// trace word carries how far the screen walk got in front of the scene, and the world ray starts there (LgTrace).
// Moving: as LgTrace's hits - the hit pixel's own speed (its surface one frame ago) against the probe's.
// Output: trace radiance (x exposure; 0 without a hit) and the trace word of every live probe texel.
// P[0] = LgSurface inputs (P[0].x = depth), P[1] = { ray info SRV, trace radiance UAV, trace word UAV, depth pyramid SRV },
// P[2] = { previous colour SRV, M's material word SRV (the Foliage test; 0xFFFFFFFF: none), radiance cache params SRV
// (0xFFFFFFFF: none), exposure ratio (float; FrameContext::upscale) }, P[3] = { iterations | thickness steps << 16 |
// bit 31: the previous colour's alpha is its frame's depth (the history depth test), relative thickness (float), radiance
// cache indirection SRV, max distance (float, m) }: with the cache the walk ends at the probe's coverage distance, as
// its world ray does (LgTrace.hlsl) - past it the cache answers,
// P[4..7] = the previous frame's view-projection (rows; upscale.prevViewProj), P[11].w = moving threshold (float),
// P[10].z adaptive SRV, P[10].w / P[11].x / P[11].y probe depth / normal / position SRVs. b1 = the main view.
#include "Passes/GI/Lumen/LgSurface.hlsli"
#include "Passes/GI/Lumen/LgRadianceCache.hlsli"
#include "Passes/Reflection/ScreenTrace.hlsli"
#include "Scene.hlsli"
#include "Passes/Atmosphere/FogVolume.hlsli"  // (atmosphere.fog.on_gi_rays: the fog along a screen hit's ray)

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 coord = id.xy;
    const uint2 atlas = coord / LG_TRACE_RES;
    const uint probe = lgProbeIndex(atlas);
    ByteAddressBuffer adaptive = ResourceDescriptorHeap[P[10].z];
    Texture2D<float> probeDepth = ResourceDescriptorHeap[P[10].w];
    if (atlas.x >= lgProbeViewSize().x || probe >= lgProbeCount(adaptive)) return;
    RWTexture2D<float4> traceRadiance = ResourceDescriptorHeap[P[1].y];
    RWTexture2D<uint> traceWord = ResourceDescriptorHeap[P[1].z];
    const float depthAtProbe = probeDepth[atlas];
    if (!(depthAtProbe > 0))
    {
        traceRadiance[coord] = 0;
        traceWord[coord] = lgEncodeTrace(0, false, false, false);
        return;
    }
    Texture2D<float2> probeNormal = ResourceDescriptorHeap[P[11].x];
    Texture2D<float4> probePosition = ResourceDescriptorHeap[P[11].y];
    Texture2D<uint> rayInfo = ResourceDescriptorHeap[P[1].x];
    const float4 positionSpeed = probePosition[atlas];
    const float3 normal = lgDecodeNormal(probeNormal[atlas]);
    const uint2 tile = lgTileOfPixel(lgProbePixel(adaptive, probe));
    uint2 rayTexel;
    uint level;
    lgUnpackRay(rayInfo[coord], rayTexel, level);
    const float mapSize = (float)((LG_TRACE_RES * 2) >> level);
    const float3 direction = lgSphere((float2(rayTexel) + lgTexelCentre(tile)) / mapSize);

    Texture2D<float> depth = ResourceDescriptorHeap[P[0].x];
    Texture2D<float> pyramid = ResourceDescriptorHeap[P[1].w];
    // off the surface by its slope across a pixel (the depth buffer's own steps must not stop the ray)
    const float pixelWorld = depthAtProbe * 2 * g_tanHalfFovY / g_viewHeight;
    const float3 view = normalize(g_cameraPosition - positionSpeed.xyz);
    const float3 origin = positionSpeed.xyz + normal * (2 * pixelWorld * sqrt(max(1 - pow(dot(normal, view), 2), 0.0)) + 1e-3);
    float maxDistance = asfloat(P[3].w);
    if (P[2].z != 0xFFFFFFFFu)
    {
        const LrcCoverage coverage = lrcCoverageChecked(lrcParams(P[2].z), P[3].z, positionSpeed.xyz, lgRcDither(atlas));
        if (coverage.valid) maxDistance = min(maxDistance, coverage.minTraceDistance);
    }
    const SctResult r = sctTrace(depth, pyramid, lgViewSize(), origin, direction, maxDistance, P[3].x & 0xFFFFu, asfloat(P[3].y), (P[3].x >> 16) & 0x7FFFu);
    const float3 end = sctWorld(r.at);
    bool hit = r.hit && !r.uncertain;
    float3 radiance = 0;
    bool moving = false;
    if (hit)
    {
        Texture2D<float4> previous = ResourceDescriptorHeap[P[2].x];
        const float4x4 prevViewProj = float4x4(asfloat(P[4]), asfloat(P[5]), asfloat(P[6]), asfloat(P[7]));
        const float noise = lgNoise1(coord + 4513u, lgFrame());
        uint2 previousSize;
        previous.GetDimensions(previousSize.x, previousSize.y);
        hit = sctPreviousColour(previous, previousSize, prevViewProj, end, asfloat(P[2].w), noise, radiance, (P[3].x >> 31) != 0);
        const uint2 hitPixel = (uint2)clamp(r.at.xy, 0.0, (float2)lgViewSize() - 1.0);
        if (hit && P[2].y != 0xFFFFFFFFu)
        {
            Texture2D<uint> words = ResourceDescriptorHeap[P[2].y];
            if (materialClass(loadMaterial(words.Load(int3(hitPixel, 0)) & 0xFFFFu)) == MATERIAL_FOLIAGE) hit = false;
        }
        if (hit)
        {
            const LgSurface hs = lgSurface(hitPixel);
            if (hs.valid)
            {
                const float hitSpeed = distance(hs.position, lgPreviousPosition(hitPixel, hs));
                moving = abs(positionSpeed.w - hitSpeed) / max(depthAtProbe, 1.0) > asfloat(P[11].w);
            }
        }
    }
    // (atmosphere.fog.on_gi_rays, off by default: the fog between the probe and the screen hit - FogVolume.hlsli)
    if (hit) radiance = fogOverGiRay((float2(lgProbePixel(adaptive, probe)) + 0.5) / float2(lgViewSize()), depthAtProbe, origin, direction, distance(end, origin), radiance);
    // how far the walk got: to the hit, or to the last point in front of the scene (a clean miss clears 2 cm more)
    const float travelled = min(distance(end, origin) + (!r.hit ? 0.02 : 0.0), maxDistance);
    traceRadiance[coord] = float4(hit ? min(radiance * g_exposure, 64000.0) : float3(0, 0, 0), 1);
    traceWord[coord] = lgEncodeTrace(travelled, hit, moving, false);
}
