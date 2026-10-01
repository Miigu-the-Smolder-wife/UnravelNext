// gi.lumen: the probes a pixel interpolates (LgCommon.hlsli). Four tile probes around the pixel, bilinear weights in
// the tile grid of this frame's jitter (widened by one pixel), times the plane weight of each probe against the pixel's
// plane. A corner whose weight stays under the minimum takes the best adaptive probe of that corner's tile.
// Readers bind: probe depth SRV = P[10].w, probe position SRV = P[11].y, adaptive buffer SRV = P[10].z.
// Depth weight scales (exp2(scale x relative plane distance^2)): -10000 (the default), x 0.25 for foliage, x 0.1 for
// the fallback weights used when nothing passes.
#ifndef UNX_GI_LUMEN_INTERPOLATE_HLSLI
#define UNX_GI_LUMEN_INTERPOLATE_HLSLI
#include "Passes/GI/Lumen/LgSurface.hlsli"

#define LG_DEPTH_WEIGHT -10000.0
#define LG_DEPTH_WEIGHT_FOLIAGE -2500.0
#define LG_DEPTH_WEIGHT_FALLBACK -1000.0

struct LgProbeSample
{
    uint2 atlas[4];
    float4 weights;
    float4 fallback;
};
// The tile whose probe is the top-left corner of the pixel's 4, and the 4 uniform probes' weights.
void lgUniformWeights(float2 pixel, float2 noiseOffset, float3 position, float depth, float3 normal, bool foliage, out uint2 tile00, inout LgProbeSample o)
{
    Texture2D<float> probeDepth = ResourceDescriptorHeap[P[10].w];
    Texture2D<float4> probePosition = ResourceDescriptorHeap[P[11].y];
    const float tile = (float)lgTile();
    const float2 full = clamp(pixel - (float2)lgTileJitter(lgTemporalIndex()) + noiseOffset, 0.0, (float2)lgViewSize() - 1.0);
    tile00 = min((uint2)(full / tile), lgProbeViewSize() - 2);
    const float2 f = (full - (float2)tile00 * tile + 1.0) / (tile + 2.0);
    const float4 bilinear = float4((1 - f.y) * (1 - f.x), (1 - f.y) * f.x, f.y * (1 - f.x), f.y * f.x);
    const float4 plane = float4(normal, dot(position, normal));
    float4 w = 0, wf = 0;
    [unroll] for (uint c = 0; c < 4; ++c)
    {
        const uint2 t = tile00 + uint2(c & 1u, c >> 1);
        if (probeDepth[t] > 0)
        {
            const float3 p = probePosition[t].xyz;
            w[c] = lgPlaneWeight(plane, depth, p, foliage ? LG_DEPTH_WEIGHT_FOLIAGE : LG_DEPTH_WEIGHT);
            wf[c] = lgPlaneWeight(plane, depth, p, LG_DEPTH_WEIGHT_FALLBACK);
        }
    }
    o.weights = bilinear * w;
    o.fallback = bilinear * wf;
}
// Weight of an adaptive probe at a pixel (plane weight x how close it is to the pixel along its nearer axis).
float2 lgAdaptiveWeight(float2 pixel, float4 plane, float depth, bool foliage, float2 probePixel, float3 probePosition)
{
    const float2 d = abs(probePixel - pixel);
    const float corner = 1 - saturate(min(d.x, d.y) / (float)lgTile());
    return float2(lgPlaneWeight(plane, depth, probePosition, foliage ? LG_DEPTH_WEIGHT_FOLIAGE : LG_DEPTH_WEIGHT),
                  lgPlaneWeight(plane, depth, probePosition, LG_DEPTH_WEIGHT_FALLBACK)) * corner;
}
void lgProbeWeights(float2 pixel, float2 noiseOffset, float3 position, float depth, float3 normal, bool useAdaptive, bool foliage, out LgProbeSample o)
{
    o = (LgProbeSample)0;
    uint2 tile00;
    lgUniformWeights(pixel, noiseOffset, position, depth, normal, foliage, tile00, o);
    [unroll] for (uint c0 = 0; c0 < 4; ++c0) o.atlas[c0] = tile00 + uint2(c0 & 1u, c0 >> 1);
    if (!useAdaptive) return;
    ByteAddressBuffer adaptive = ResourceDescriptorHeap[P[10].z];
    Texture2D<float> probeDepth = ResourceDescriptorHeap[P[10].w];
    Texture2D<float4> probePosition = ResourceDescriptorHeap[P[11].y];
    const float4 plane = float4(normal, dot(position, normal));
    [unroll] for (uint c = 0; c < 4; ++c)
    {
        if (o.weights[c] >= LG_MIN_INTERPOLATION_WEIGHT) continue;
        const uint2 tile = tile00 + uint2(c & 1u, c >> 1);
        const uint header = lgTileHeaderAddress(tile);
        const uint count = min(adaptive.Load(header), LG_TILE_ADAPTIVE);
        [loop] for (uint k = 0; k < count; ++k)
        {
            const uint a = adaptive.Load(header + 4 + k * 4);
            if (a >= lgMaxAdaptive()) continue;
            const uint probe = lgUniformProbes() + a;
            const uint2 atlas = lgAtlasCoord(probe);
            if (!(probeDepth[atlas] > 0)) continue;
            const float2 w = lgAdaptiveWeight(pixel, plane, depth, foliage, (float2)lgUnpackScreen(adaptive.Load(16 + a * 4)), probePosition[atlas].xyz);
            if (w.x > o.weights[c])
            {
                o.weights[c] = w.x;
                o.fallback[c] = w.y;
                o.atlas[c] = atlas;
            }
        }
    }
}
#endif
