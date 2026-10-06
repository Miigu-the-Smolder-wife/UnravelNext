// unx-kernel: cs_6_6 main
// gi.lumen, r.gi.lg.probetemporal (gi.lumen_temporal_filter_probes, off by default as in Unreal): the probes' radiance
// blended with last frame's before the spatial filter. Thread = probe map texel. History: the same texel of the up to
// 4 history tile probes around the probe's own previous position that lie on its plane (exp2(-10000 x relative plane
// distance^2) > 0.1), equal weights, rescaled to this frame's exposure. radiance = lerp(new, history, P[2].z) where a
// history probe exists; the new value alone otherwise (history failure: disocclusion, the first frame, off screen).
// P[0] = LgSurface inputs, P[1] = { radiance SRV (this frame, composite), radiance UAV, previous probe depth SRV,
// previous probe position SRV }, P[2] = { previous probe radiance SRV, previous temporal index, history weight (float),
// this exposure / previous exposure (float) }, P[10].z adaptive SRV, P[10].w / P[11].x / P[11].y probe depth / normal /
// position SRVs. b1 = the view.
#include "Passes/GI/Lumen/LgInterpolate.hlsli"

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 atlas = id.xy / LG_GATHER_RES, texel = id.xy % LG_GATHER_RES;
    const uint probe = lgProbeIndex(atlas);
    ByteAddressBuffer adaptive = ResourceDescriptorHeap[P[10].z];
    if (atlas.x >= lgProbeViewSize().x || probe >= lgProbeCount(adaptive)) return;
    Texture2D<float> probeDepth = ResourceDescriptorHeap[P[10].w];
    Texture2D<float4> current = ResourceDescriptorHeap[P[1].x];
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[1].y];
    const float4 fresh = current[id.xy];
    const float depth = probeDepth[atlas];
    if (!(depth > 0))
    {
        output[id.xy] = float4(0, 0, 0, fresh.a);
        return;
    }
    float3 history = 0;
    float weight = 0;
    if (lgHistoryValid())
    {
        Texture2D<float2> probeNormal = ResourceDescriptorHeap[P[11].x];
        Texture2D<float4> probePosition = ResourceDescriptorHeap[P[11].y];
        LgSurface s = (LgSurface)0;
        s.valid = true;
        s.position = probePosition[atlas].xyz;
        s.normal = lgDecodeNormal(probeNormal[atlas]);
        float2 prevPixel;
        float prevDepth;
        if (giPreviousPixel(lgPreviousPosition(lgProbePixel(adaptive, probe), s), (float2)lgViewSize(), prevPixel, prevDepth) && all(prevPixel >= -0.5) &&
            all(prevPixel <= (float2)lgViewSize() + 0.5))
        {
            Texture2D<float> prevProbeDepth = ResourceDescriptorHeap[P[1].z];
            Texture2D<float4> prevProbePosition = ResourceDescriptorHeap[P[1].w];
            Texture2D<float4> prevRadiance = ResourceDescriptorHeap[P[2].x];
            const float2 tileCoord = (prevPixel - (float2)lgTileJitter(P[2].y) - 0.5) / (float)lgTile();
            const float4 plane = float4(s.normal, dot(s.position, s.normal));
            [unroll] for (uint c = 0; c < 4; ++c)
            {
                const uint2 t = (uint2)clamp(tileCoord + float2(c & 1u, c >> 1), 0.0, (float2)lgProbeViewSize() - 1.0);
                if (!(prevProbeDepth[t] > 0)) continue;
                if (!(lgPlaneWeight(plane, depth, prevProbePosition[t].xyz, LG_DEPTH_WEIGHT) > 0.1)) continue;
                history += prevRadiance[t * LG_GATHER_RES + texel].rgb;
                weight += 1;
            }
            if (weight > 0) history = history / weight * asfloat(P[2].w);
        }
    }
    output[id.xy] = float4(lerp(fresh.rgb, history, weight > 0 ? asfloat(P[2].z) : 0.0), fresh.a);
}
