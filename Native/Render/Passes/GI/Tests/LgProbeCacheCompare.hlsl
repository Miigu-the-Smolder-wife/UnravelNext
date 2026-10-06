// unx-kernel: cs_6_6 main
#include "Passes/GI/Lumen/LgRadianceCache.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 group : SV_GroupID, uint3 lane : SV_GroupThreadID)
{
    const uint probe = lgProbeIndex(group.xy);
    const LrcParams p = lrcParams(P[0].x);
    RWByteAddressBuffer stats = ResourceDescriptorHeap[P[1].y];
    Texture2D<float4> positions = ResourceDescriptorHeap[P[11].y];
    Texture2D<float> depths = ResourceDescriptorHeap[P[10].w];
    ByteAddressBuffer adaptive = ResourceDescriptorHeap[P[10].z];
    const LrcCoverage got = lgRcCoverage(P[0].z, probe);
    if (probe >= lgProbeCount(adaptive) || !(depths[group.xy] > 0))
    {
        if (got.valid) stats.InterlockedAdd(12, 1);
        if (all(lane.xy == 0)) stats.InterlockedAdd(28, 1);
        return;
    }
    const float3 position = positions[group.xy].xyz;
    const LrcCoverage original = lrcCoverageChecked(p, P[0].y, position, lgRcDither(group.xy));
    if (got.valid != original.valid || got.clipmap != original.clipmap || asuint(got.minTraceDistance) != asuint(original.minTraceDistance))
        stats.InterlockedAdd(0, 1);
    stats.InterlockedAdd(16, 1);
    if (!original.valid) return;
    stats.InterlockedAdd(20, 1);
    const float3 direction = lgSphere((float2(lane.xy) + float2(0.25, 0.75)) / 8.0);
    const uint depthSrv = lgFrame() & 1 ? P[1].x : 0xFFFFFFFFu;
    const float3 seen = lrcSeenFrom(p, original, position, direction);
    const float4 a = lrcSample(p, P[0].y, P[0].w, depthSrv, original, position, direction, seen);
    const float4 b = lgRcSample(p, P[0].z, P[0].w, depthSrv, got, probe, position, direction, seen, true);
    if (any(asuint(a) != asuint(b))) stats.InterlockedAdd(4, 1);
    if (a.a > 0) stats.InterlockedAdd(24, 1);
    const float da = lrcSampleDistance(p, P[0].y, P[1].x, original, position, direction);
    const float db = lgRcSampleDistance(p, P[0].z, P[1].x, got, probe, position, direction, true);
    if (asuint(da) != asuint(db)) stats.InterlockedAdd(8, 1);
}
