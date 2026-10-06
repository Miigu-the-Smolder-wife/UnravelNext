// unx-kernel: cs_6_6 main
// gi.lumen, r.gi.lg.composite: the probe's 64 traces gathered into its 8 x 8 radiance map. One group per probe; thread
// = trace on the way in, = map texel on the way out. A trace of level L covers (8 / mapSize(L))^2 of a texel (1 at 8 x 8,
// 1/4 at 16 x 16) and adds its radiance x that share to the texel its direction falls in - of the 4 texels around its
// continuous position one, chosen by a per-tile random number (dithered, so the frames' average is the bilinear splat),
// wrapped across the octahedral map's edges. Before the sum the trace's weighted radiance is scaled down so that its
// largest channel is at most P[1].w (the ray intensity cap: a firefly bound, a bias - a parameter, Unreal's default 10
// in exposed units; on a snap frame in the exposure LgMeter metered on the frame's traces, P[0].w). The sums use fixed
// point (atomic adds between the group's threads).
// Per texel out: radiance (rgb) and the share of moving traces (a); the nearest hit distance (lgEncodeHitDistance; 0 =
// no trace landed). Per probe: the mean moving share (the probe filter widens for it, the pixel filter speeds up).
// P[0] = { trace radiance SRV, trace word SRV, ray info SRV, exposure reference SRV (LgMeter.hlsl; 0xFFFFFFFF: the
// frame's exposure) }, P[1] = { probe radiance UAV (RGBA16F, atlas x 8),
// hit distance UAV (R16_UNORM, atlas x 8), probe moving UAV (R8_UNORM, atlas), ray intensity cap (float) },
// P[10].z adaptive SRV, P[10].w probe depth SRV.
#include "Passes/GI/Lumen/LgCommon.hlsli"

groupshared uint gs_sum[64][5];
groupshared uint gs_min[64];

[numthreads(8, 8, 1)]
void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    const uint2 atlas = group.xy;
    const uint probe = lgProbeIndex(atlas);
    ByteAddressBuffer adaptive = ResourceDescriptorHeap[P[10].z];
    Texture2D<float> probeDepth = ResourceDescriptorHeap[P[10].w];
    RWTexture2D<float4> radianceOut = ResourceDescriptorHeap[P[1].x];
    RWTexture2D<float> distanceOut = ResourceDescriptorHeap[P[1].y];
    RWTexture2D<float> movingOut = ResourceDescriptorHeap[P[1].z];
    const uint2 coord = atlas * LG_TRACE_RES + thread.xy;
    const uint index = thread.y * 8 + thread.x;
    if (atlas.x >= lgProbeViewSize().x || probe >= lgProbeCount(adaptive)) return;
    if (!(probeDepth[atlas] > 0))
    {
        radianceOut[coord] = 0;
        distanceOut[coord] = lgEncodeHitDistance(LG_MAX_HIT_DISTANCE);
        if (index == 0) movingOut[atlas] = 0;
        return;
    }
    [unroll] for (uint z = 0; z < 5; ++z) gs_sum[index][z] = 0;
    gs_min[index] = asuint(LG_MAX_HIT_DISTANCE);
    GroupMemoryBarrierWithGroupSync();
    float cap = asfloat(P[1].w);
    if (P[0].w != 0xFFFFFFFFu)
    {
        ByteAddressBuffer reference = ResourceDescriptorHeap[P[0].w];
        cap /= max(asfloat(reference.Load(0)), 1e-20);  // exposed radiance x c <= cap
    }
    const float perThread = 4294967295.0 / 64.0;
    {
        Texture2D<float4> traceRadiance = ResourceDescriptorHeap[P[0].x];
        Texture2D<uint> traceWord = ResourceDescriptorHeap[P[0].y];
        Texture2D<uint> rayInfo = ResourceDescriptorHeap[P[0].z];
        uint2 rayTexel;
        uint level;
        lgUnpackRay(rayInfo[coord], rayTexel, level);
        const float mapSize = (float)((LG_TRACE_RES * 2) >> level);
        const float share = ((float)LG_GATHER_RES / mapSize) * ((float)LG_GATHER_RES / mapSize);
        float3 lighting = traceRadiance[coord].rgb * share;
        const float largest = max(lighting.r, max(lighting.g, lighting.b));
        if (largest > cap) lighting *= cap / largest;
        const uint2 tile = lgTileOfPixel(lgProbePixel(adaptive, probe));
        const float2 position = (float2(rayTexel) + lgTexelCentre(tile)) * (float)LG_GATHER_RES / mapSize - 0.5;
        const float2 random = lgNoise2(tile + 4099u, lgRayIndex() + 32u);
        const int2 q = (int2)floor(position) + int2(frac(position.x) < random.x ? 0 : 1, frac(position.y) < random.y ? 0 : 1);
        const uint2 target = lgOctWrap(q + 1, LG_GATHER_RES, 1);
        const uint t = target.y * LG_GATHER_RES + target.x;
        const uint3 fixed = (uint3)(lighting * (perThread / cap));
        InterlockedAdd(gs_sum[t][0], fixed.x);
        InterlockedAdd(gs_sum[t][1], fixed.y);
        InterlockedAdd(gs_sum[t][2], fixed.z);
        InterlockedAdd(gs_sum[t][3], 1u);
        const uint word = traceWord[coord];
        if (lgTraceMoving(word)) InterlockedAdd(gs_sum[t][4], (uint)(share * perThread));
        InterlockedMin(gs_min[t], asuint(lgTraceDistance(word)));
    }
    GroupMemoryBarrierWithGroupSync();
    const float3 lighting = float3(gs_sum[index][0], gs_sum[index][1], gs_sum[index][2]) * (cap / perThread);
    const bool landed = gs_sum[index][3] > 0;
    const float moving = gs_sum[index][4] / perThread;
    radianceOut[coord] = float4(lighting, saturate(moving));
    distanceOut[coord] = lgEncodeHitDistance(landed ? asfloat(gs_min[index]) : -1.0);
    if (index == 0)
    {
        float total = 0;
        [loop] for (uint i = 0; i < 64; ++i) total += gs_sum[i][4] / perThread;
        movingOut[atlas] = saturate(total / 64.0);
    }
}
