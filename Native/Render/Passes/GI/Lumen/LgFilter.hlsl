// unx-kernel: cs_6_6 main
// gi.lumen, r.gi.lg.filter<k>: one pass of the probes' spatial filter (3 by default). Thread = probe map texel. The
// texel's radiance becomes the weighted mean of its own (weight 1 when a trace landed in it) and the same texel of
// the uniform probes of the 4 tiles next to the probe's tile:
//   position weight: exp2(-P[2].y x ((neighbour depth - depth) / depth)^2) (no surface there: 0);
//   angle weight: what the neighbour saw in this direction, placed at the neighbour's hit distance (clipped to this
//   texel's own), must lie within P[2].x radians of this direction as seen from this probe: 1 - angle / P[2].x.
// A probe with moving traces (its moving share > 0.01) also takes the 8 next tiles (2 away on the axes, the diagonals)
// and drops the angle weight; a probe flagged as disoccluded (LgScreenData.hlsl) drops the angle weight too.
// P[0] = { radiance SRV (RGBA16F, a = moving share, kept), hit distance SRV, probe moving SRV, BRDF / flag buffer SRV },
// P[1] = { radiance UAV, 0, 0, 0 }, P[2] = { max hit angle (float, rad), position weight scale (float), pass, 0 },
// P[10].z adaptive SRV, P[10].w / P[11].y probe depth / position SRVs. b1 = the view.
#include "Passes/GI/Lumen/LgCommon.hlsli"

void lgGather(int2 neighbourTile, uint2 texel, float3 position, float3 direction, float depth, float hitDistance, bool relaxed, inout float3 sum, inout float weightSum)
{
    if (any(neighbourTile < 0) || any(neighbourTile >= (int2)lgProbeViewSize())) return;
    Texture2D<float> probeDepth = ResourceDescriptorHeap[P[10].w];
    Texture2D<float4> probePosition = ResourceDescriptorHeap[P[11].y];
    Texture2D<float4> radiance = ResourceDescriptorHeap[P[0].x];
    Texture2D<float> hitDistances = ResourceDescriptorHeap[P[0].y];
    const uint2 atlas = (uint2)neighbourTile;
    const float neighbourDepth = probeDepth[atlas];
    if (!(neighbourDepth > 0)) return;
    const float relative = abs(neighbourDepth - depth) / depth;
    const float positionWeight = exp2(-asfloat(P[2].y) * relative * relative);
    if (!(positionWeight > 0)) return;
    const uint2 coord = atlas * LG_GATHER_RES + texel;
    float neighbourDistance = lgDecodeHitDistance(hitDistances[coord]);
    if (neighbourDistance < 0) return;
    float angleWeight = 1;
    if (!relaxed)
    {
        if (hitDistance >= 0) neighbourDistance = min(neighbourDistance, hitDistance);
        const float3 toHit = probePosition[atlas].xyz + direction * neighbourDistance - position;
        const float angle = acos(clamp(dot(toHit, direction) / max(length(toHit), 1e-6), -1.0, 1.0));
        angleWeight = 1 - saturate(angle / asfloat(P[2].x));
    }
    const float w = positionWeight * angleWeight;
    sum += radiance[coord].rgb * w;
    weightSum += w;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 atlas = id.xy / LG_GATHER_RES, texel = id.xy % LG_GATHER_RES;
    const uint probe = lgProbeIndex(atlas);
    ByteAddressBuffer adaptive = ResourceDescriptorHeap[P[10].z];
    if (atlas.x >= lgProbeViewSize().x || probe >= lgProbeCount(adaptive)) return;
    Texture2D<float> probeDepth = ResourceDescriptorHeap[P[10].w];
    Texture2D<float4> probePosition = ResourceDescriptorHeap[P[11].y];
    Texture2D<float4> radiance = ResourceDescriptorHeap[P[0].x];
    Texture2D<float> hitDistances = ResourceDescriptorHeap[P[0].y];
    Texture2D<float> probeMoving = ResourceDescriptorHeap[P[0].z];
    ByteAddressBuffer flags = ResourceDescriptorHeap[P[0].w];
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[1].x];
    const float depth = probeDepth[atlas];
    const float4 own = radiance[id.xy];
    if (!(depth > 0))
    {
        output[id.xy] = float4(0, 0, 0, own.a);
        return;
    }
    const bool strong = probeMoving[atlas] > 0.01;
    const bool relaxed = strong || asfloat(flags.Load(probe * 40 + 36)) > 0.5;
    const int2 tile = (int2)lgTileOfPixel(lgProbePixel(adaptive, probe));
    const float3 position = probePosition[atlas].xyz;
    const float3 direction = lgSphere((float2(texel) + lgTexelCentre((uint2)tile)) / (float)LG_GATHER_RES);
    const float hitDistance = lgDecodeHitDistance(hitDistances[id.xy]);
    float3 sum = 0;
    float weightSum = 0;
    if (hitDistance >= 0)
    {
        sum = own.rgb;
        weightSum = 1;
    }
    static const int2 kNear[4] = { int2(-1, 0), int2(1, 0), int2(0, -1), int2(0, 1) };
    [unroll] for (uint i = 0; i < 4; ++i) lgGather(tile + kNear[i], texel, position, direction, depth, hitDistance, relaxed, sum, weightSum);
    if (strong)
    {
        static const int2 kFar[8] = { int2(-2, 0), int2(2, 0), int2(0, -2), int2(0, 2), int2(-1, 1), int2(1, 1), int2(-1, -1), int2(1, -1) };
        [unroll] for (uint j = 0; j < 8; ++j) lgGather(tile + kFar[j], texel, position, direction, depth, hitDistance, relaxed, sum, weightSum);
    }
    output[id.xy] = float4(weightSum > 0 ? sum / weightSum : float3(0, 0, 0), own.a);
}
