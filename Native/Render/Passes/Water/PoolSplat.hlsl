// unx-kernel: cs_6_6 main
// Closed basins (Pool.hlsli): one group per source adds it to the accumulation buffer. The source's Gaussian footprint w
// (sigma >= the larger sample spacing) is sampled at the unfolded sample positions around its centre out to 4 sigma and
// normalised by its own discrete sum S; a footprint sample beyond a wall folds back onto its mirror sample inside (the
// source's image: the wall reflects it), and a sample on a wall takes its weight twice (it is its own image), so the
// trapezoid sum of the added field over the basin is exactly the footprint's (walls 1/2 per axis x 2 = 1).
//   impulse I: phi changes by -(I / rho) w / (S hx hz): the water receives exactly the vertical impulse I;
//   volume V (water pushed out of the footprint): eta changes by -V w / (S hx hz) + V / (Lx Lz): the surface under the
//     body goes down by V and the basin's mean level stays (the water's volume is conserved; the uniform term, the
//     k = 0 part of the footprint removed, is the host's sum over the frame's sources in double: PoolRows adds P[3].w).
// 2^24 fixed point (order independent).
#include "Pool.hlsli"

groupshared float g_sum[256];

[numthreads(256, 1, 1)]
void main(uint t : SV_GroupThreadID, uint g : SV_GroupID)
{
    if (g >= P[2].y) return;
    ByteAddressBuffer sources = ResourceDescriptorHeap[P[2].x];
    RWByteAddressBuffer accum = ResourceDescriptorHeap[P[0].w];
    const float4 s = asfloat(sources.Load4(32 * g));
    const float volume = asfloat(sources.Load(32 * g + 16));
    const float2 h = poolH();
    const float sigma = max(s.z, max(h.x, h.y));
    const int2 reach = min(int2(ceil(4.0 * sigma / h)), int2(POOL_MAX_REACH, POOL_MAX_REACH)), width = 2 * reach + 1;
    const int2 centre = int2(floor(s.xy + 0.5));
    const float inv2s2 = 1.0 / (2.0 * sigma * sigma);
    const int count = width.x * width.y;
    float partial = 0;
    for (int i = int(t); i < count; i += 256)
    {
        const float2 d = (float2(centre + int2(i % width.x, i / width.x) - reach) - s.xy) * h;
        partial += exp(-dot(d, d) * inv2s2);
    }
    g_sum[t] = partial;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint stride = 128; stride > 0; stride >>= 1)
    {
        if (t < stride) g_sum[t] += g_sum[t + stride];
        GroupMemoryBarrierWithGroupSync();
    }
    const float area = h.x * h.y;
    const float amplitude = -(s.w / POOL_RHO) / (g_sum[0] * area), lowering = -volume / (g_sum[0] * area);
    const int edge = int(POOL_N / 2u);
    for (int j = int(t); j < count; j += 256)
    {
        const int2 u = centre + int2(j % width.x, j / width.x) - reach;  // unfolded sample
        const float2 d = (float2(u) - s.xy) * h;
        float w = exp(-dot(d, d) * inv2s2);
        const int2 q = select(u < 0, -u, select(u > edge, 2 * edge - u, u));  // folded into the basin
        if (any(q < 0) || any(q > edge)) continue;  // beyond one fold (the host keeps source centres inside the basin)
        w *= (q.x == 0 || q.x == edge ? 2.0 : 1.0) * (q.y == 0 || q.y == edge ? 2.0 : 1.0);
        const uint at = 8 * (uint(q.y) * POOL_Q + uint(q.x));
        if (volume != 0) accum.InterlockedAdd(at, int(round(lowering * w * POOL_FIXED)));
        if (s.w != 0) accum.InterlockedAdd(at + 4, int(round(amplitude * w * POOL_FIXED)));
    }
}
