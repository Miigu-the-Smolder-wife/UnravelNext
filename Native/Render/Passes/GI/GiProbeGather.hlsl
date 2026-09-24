// unx-kernel: cs_6_6 main
// Screen probe fill (no rays): trilinear cache SH at the probe's surface point, the choice of the cache entry with the
// largest trilinear weight as the probe's radiance map source (copied by GiProbeMaps once per entry: the lowest probe
// index reading it owns the copy), and near occlusion there: 16 fixed
// cosine-distributed hemisphere points within r = min(gi.near_occlusion_radius_m, cache cell edge) — occlusion below
// the cache's own resolution only, so the cache (whose rays already see larger occluders) is not darkened twice.
// A point is occluded when the depth buffer shows a surface in front of it closer than r along the view ray.
// P[0] = { cache UAV (raw), depth SRV, gbuffer SRV, probes UAV }, P[1] = { probesX, probesY, width, height },
// P[2] = { spacing, near occlusion radius (float bits), map owner list UAV, map dispatch args UAV } (reset here for
// GiProbeMapOwners); frame constants b1 = main view.
#include "GBuffer.hlsli"
#include "Passes/GI/GiInternal.hlsli"

float nearOcclusion(Texture2D<float> depth, float3 p, float3 n, float radius, uint rotation, uint2 size)
{
    float3 t, bt;
    giBasis(n, t, bt);
    const float phase = (rotation & 1023u) * (6.28318530718 / 1024.0);
    float occluded = 0, total = 0;
    [unroll] for (uint i = 0; i < 16; ++i)
    {
        // Cosine-weighted hemisphere (Vogel spiral on the disk, lifted), radius fraction sqrt-stratified.
        const float u = (i + 0.5) / 16.0;
        const float phi = i * 2.39996323 + phase;
        const float sr = sqrt(u);
        const float3 local = float3(sr * cos(phi), sr * sin(phi), sqrt(1 - u));
        const float reach = radius * sqrt((i % 4 + 0.5) / 4.0);
        const float3 q = p + (t * local.x + bt * local.y + n * local.z) * reach;
        const float4 clip = mul(g_viewProj, float4(q, 1));
        if (clip.w <= 0) continue;
        const float2 ndc = clip.xy / clip.w;
        const int2 pixel = int2((ndc * float2(0.5, -0.5) + 0.5) * float2(size));
        total += 1;
        if (any(pixel < 0) || any(pixel >= int2(size))) continue;
        const float sceneDepth = linearDepth(depth.Load(int3(pixel, 0)));
        const float sampleDepth = clip.w;  // view distance along the axis (reversed-Z infinite projection: w = view depth)
        if (sceneDepth < sampleDepth - 1e-3 * sampleDepth && sampleDepth - sceneDepth < radius) occluded += 1;
    }
    return total > 0 ? 1 - occluded / total : 1;
}

[numthreads(8, 8, 1)]
void main(uint2 probe : SV_DispatchThreadID)
{
    const uint2 count = P[1].xy, size = P[1].zw;
    const uint spacing = P[2].x;
    RWTexture2D<uint4> probes = ResourceDescriptorHeap[P[0].w];
    if (all(probe == 0))
    {
        probes[uint2(0, count.y * 4)] = uint4(spacing, count.x, count.y, 0);
        RWByteAddressBuffer list = ResourceDescriptorHeap[P[2].z];
        RWByteAddressBuffer args = ResourceDescriptorHeap[P[2].w];
        list.Store(0, 0u);
        args.Store4(0, uint4(512, 0, 1, 0));
    }
    if (probe.x >= count.x || probe.y >= count.y) return;
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].y];
    Texture2D<uint2> gbuffer = ResourceDescriptorHeap[P[0].z];
    const GiHeader h = giHeader(b);
    uint2 offset;
    float d;
    float3 c[9];
    if (!giProbePixel(depth, probe, spacing, size, offset, d))
    {
        [unroll] for (uint k = 0; k < 9; ++k) c[k] = 0;
        giStoreProbe(probes, probe, c, 0, float3(0, 0, 1), 1, offset, false);
        probes[uint2(probe.x * 8 + 5, probe.y * 4 + 3)] = uint4(GI_ENTRY_PENDING, 0, 0, 0);  // radiance map source (GiProbeMaps)
        return;
    }
    const uint2 pixel = probe * spacing + offset;
    const float3 p = worldFromDepth(float2(pixel), d);
    const float3 n = decodeGBuffer(gbuffer.Load(int3(pixel, 0))).normal;
    uint mapEntry;
    giCacheShAt(b, h, p, n, c, mapEntry);
    probes[uint2(probe.x * 8 + 5, probe.y * 4 + 3)] = uint4(mapEntry, 0, 0, 0);  // radiance map source (GiProbeMaps)
    if (mapEntry != GI_ENTRY_PENDING) b.InterlockedMin(b.Load(GI_H_MAP_OWNER) + mapEntry * 4, probe.y * count.x + probe.x);
    const float radius = min(asfloat(P[2].y), giCellSize(h, giLevel(h, p)));
    const float occlusion = nearOcclusion(depth, p + n * (1e-3 * linearDepth(d)), n, radius, probe.x * 7919u + probe.y * 104729u, size);
    giStoreProbe(probes, probe, c, linearDepth(d), n, occlusion, offset, true);
}
