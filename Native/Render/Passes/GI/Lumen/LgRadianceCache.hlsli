// gi.lumen: how the screen probes use A's far-field radiance cache (Passes/GI/LumenRadianceCache.hlsli;
// lumen.radiance_cache). A probe's clipmap is chosen with one dither value per probe and frame, the same where the
// probe marks its cells (LgRcMark.hlsl), where its rays stop and read the cache (LgTrace.hlsl) and where its lighting
// density reads it (LgLightingPdf.hlsl).
#ifndef UNX_GI_LUMEN_RADIANCE_CACHE_HLSLI
#define UNX_GI_LUMEN_RADIANCE_CACHE_HLSLI
#include "Passes/GI/Lumen/LgCommon.hlsli"
#include "Passes/GI/LumenRadianceCache.hlsli"
#include "Passes/GI/Lumen/LgProbeCacheLayout.h"

float lgRcDither(uint2 atlas) { return lgUnit(lgHash(atlas.x * 0x9E3779B1u + atlas.y * 0x85EBCA77u + lgFrame() * 0x27D4EB2Fu)); }

bool lgRcPrepared() { return (P[9].z & LG_PROBE_CACHE_PREPARED) != 0; }

// Called once per live screen probe, after this frame's radiance-cache update.
// Even a zero-weight corner participates in the all-eight-present check.
void lgRcPrepare(RWByteAddressBuffer output, uint probe, LrcParams p, uint indirectionSrv, float3 position, float dither)
{
    const uint base = probe * LG_PROBE_CACHE_BYTES;
    LrcCoverage coverage = lrcCoverage(p, position, dither);
    int3 corner = 0;
    float3 fraction = 0;
    if (coverage.valid)
    {
        const float3 f = lrcCoordFloat(p, position, coverage.clipmap) - 0.5;
        corner = int3(floor(f));
        fraction = f - floor(f);
    }
    Texture3D<uint> indirection = ResourceDescriptorHeap[indirectionSrv];
    const bool inClipmap = coverage.valid;
    for (uint i = 0; i < 8; ++i)
    {
        const int3 o = int3(i & 1, (i >> 1) & 1, i >> 2);
        const uint slot = inClipmap ? lrcIndirection(indirection, p, corner + o, coverage.clipmap) : LRC_INVALID;
        const float weight = (o.x ? fraction.x : 1 - fraction.x) * (o.y ? fraction.y : 1 - fraction.y) * (o.z ? fraction.z : 1 - fraction.z);
        output.Store2(base + 32 + i * 8, uint2(slot, asuint(weight)));
        if (slot >= LRC_USED) coverage.valid = false;
    }
    if (!coverage.valid && inClipmap) coverage.minTraceDistance = 1e7;
    output.Store4(base, uint4(coverage.valid, coverage.clipmap, asuint(coverage.minTraceDistance), 0));
    output.Store4(base + 16, uint4(asuint(corner), 0));
}

LrcCoverage lgRcCoverage(uint preparedSrv, uint probe)
{
    ByteAddressBuffer prepared = ResourceDescriptorHeap[preparedSrv];
    const uint4 header = prepared.Load4(probe * LG_PROBE_CACHE_BYTES);
    LrcCoverage c;
    c.valid = header.x != 0;
    c.clipmap = header.y;
    c.minTraceDistance = asfloat(header.z);
    return c;
}

LrcCoverage lgRcCoverageAt(LrcParams p, uint lookupSrv, uint probe, float3 position, float dither, bool prepared)
{
    if (prepared) return lgRcCoverage(lookupSrv, probe);
    return lrcCoverageChecked(p, lookupSrv, position, dither);
}

float4 lgRcSample(LrcParams p, uint lookupSrv, uint atlasSrv, uint depthSrv, LrcCoverage coverage,
                 uint screenProbe, float3 position, float3 direction, float3 seenFrom, bool prepared)
{
    if (!prepared) return lrcSample(p, lookupSrv, atlasSrv, depthSrv, coverage, position, direction, seenFrom);
    ByteAddressBuffer lookup = ResourceDescriptorHeap[lookupSrv];
    Texture2D<float4> atlas = ResourceDescriptorHeap[atlasSrv];
    const uint base = screenProbe * LG_PROBE_CACHE_BYTES;
    const int3 corner = asint(lookup.Load3(base + 16));
    float4 sum = 0;
    for (uint i = 0; i < 8; ++i)
    {
        const uint2 entry = lookup.Load2(base + 32 + 8 * i);
        const float weight = asfloat(entry.y);
        if (!(weight > 0) || entry.x >= LRC_USED) continue;
        const int3 o = int3(i & 1, (i >> 1) & 1, i >> 2);
        const float3 centre = lrcProbePosition(p, uint3(corner + o), coverage.clipmap);
        if (depthSrv != 0xFFFFFFFFu)
        {
            Texture2D<uint> depth = ResourceDescriptorHeap[depthSrv];
            if (!lrcProbeSees(p, depth, entry.x, centre, coverage.clipmap, seenFrom)) continue;
        }
        sum += lrcProbeRadiance(p, atlas, entry.x, centre, coverage.clipmap, position, direction) * weight;
    }
    return sum.a >= LRC_MIN_ANSWER ? float4(sum.rgb / sum.a, sum.a) : float4(0, 0, 0, 0);
}

float lgRcSampleDistance(LrcParams p, uint lookupSrv, uint depthSrv, LrcCoverage coverage,
                         uint screenProbe, float3 position, float3 direction, bool prepared)
{
    if (!prepared) return lrcSampleDistance(p, lookupSrv, depthSrv, coverage, position, direction);
    ByteAddressBuffer lookup = ResourceDescriptorHeap[lookupSrv];
    const uint base = screenProbe * LG_PROBE_CACHE_BYTES;
    const int3 corner = asint(lookup.Load3(base + 16));
    const int3 coord = int3(floor(lrcCoordFloat(p, position, coverage.clipmap)));
    const int3 o = coord - corner;
    const uint slot = lookup.Load(base + 32 + 8 * (uint(o.x) + 2u * uint(o.y) + 4u * uint(o.z)));
    if (slot >= LRC_USED) return coverage.minTraceDistance;
    Texture2D<uint> depth = ResourceDescriptorHeap[depthSrv];
    const uint2 texel = min(uint2(lrcDirectionToUv(direction) * float(p.probeResolution)), p.probeResolution - 1);
    return max(lrcDepthDistance(depth.Load(int3(lrcAtlasCoord(p, slot) * p.probeResolution + texel, 0))), coverage.minTraceDistance);
}
#endif
