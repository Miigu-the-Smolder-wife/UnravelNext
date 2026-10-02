// unx-kernel: cs_6_6 main
// m.post.le.grid (LocalExposure.hlsli): the bilateral grid of the exposed image's log2 luminance. One group per grid tile
// (LE_TILE x LE_TILE output pixels), 8 x 8 threads, each over its 16 x 16 pixels at every second pixel (64 x 64 samples
// a tile, as the reference's half-resolution input). A sample is split between its two nearest luminance buckets; the
// group's sums are kept as integers (weights in 1/1024, log luminance above LE_LOG_MIN in 1/16384: 4096 samples of 24
// stops stay under 2^32), so the result does not depend on the threads' order.
// P[0] = { exposed image SRV, grid UAV (RWTexture3D<float2>: sum of log luminance x weight, sum of weight),
//          tile mean UAV (RWTexture2D<float>: the tile's mean log luminance), 0 }, P[1] = { width, height, 0, 0 }
#include "Bindless.hlsli"
#include "Passes/Shading/LocalExposure.hlsli"

groupshared uint gs_weight[LE_DEPTH];
groupshared uint gs_sum[LE_DEPTH];

[numthreads(8, 8, 1)]
void main(uint3 tile : SV_GroupID, uint2 local : SV_GroupThreadID, uint flat : SV_GroupIndex)
{
    if (flat < LE_DEPTH)
    {
        gs_weight[flat] = 0;
        gs_sum[flat] = 0;
    }
    GroupMemoryBarrierWithGroupSync();
    Texture2D<float4> image = ResourceDescriptorHeap[P[0].x];
    const uint2 origin = tile.xy * LE_TILE + local * 16u;
    for (uint j = 0; j < 8; ++j)
        for (uint i = 0; i < 8; ++i)
        {
            const uint2 pixel = origin + uint2(i, j) * 2u;
            if (any(pixel >= P[1].xy)) continue;
            const float logLum = log2(leLuminance(image.Load(int3(pixel, 0)).rgb));
            const float at = leBucketPosition(logLum) * (LE_DEPTH - 1);
            const uint b0 = min((uint)at, LE_DEPTH - 1), b1 = min(b0 + 1, LE_DEPTH - 1);
            const float f = at - b0;
            const float above = clamp(logLum - LE_LOG_MIN, 0.0, LE_LOG_MAX - LE_LOG_MIN);
            const uint w0 = (uint)((1 - f) * 1024.0 + 0.5), w1 = 1024u - w0;
            InterlockedAdd(gs_weight[b0], w0);
            InterlockedAdd(gs_sum[b0], (uint)(above * w0 * 16.0 + 0.5));
            InterlockedAdd(gs_weight[b1], w1);
            InterlockedAdd(gs_sum[b1], (uint)(above * w1 * 16.0 + 0.5));
        }
    GroupMemoryBarrierWithGroupSync();
    if (flat < LE_DEPTH)
    {
        RWTexture3D<float2> grid = ResourceDescriptorHeap[P[0].y];
        const float weight = gs_weight[flat] / 1024.0;
        // (sum of (log - min) x w in 1 / (1024 x 16), back to the sum of log x w)
        grid[uint3(tile.xy, flat)] = float2(gs_sum[flat] / 16384.0 + LE_LOG_MIN * weight, weight);
    }
    if (flat == LE_DEPTH)
    {
        float weight = 0, sum = 0;
        for (uint b = 0; b < LE_DEPTH; ++b)
        {
            weight += gs_weight[b] / 1024.0;
            sum += gs_sum[b] / 16384.0;
        }
        RWTexture2D<float> mean = ResourceDescriptorHeap[P[0].z];
        mean[tile.xy] = weight > 0 ? sum / weight + LE_LOG_MIN : log2(0.18);
    }
}
