// unx-kernel: cs_6_6 main
// Ripples (Ripple.hlsli): one group per source adds its impulsive pressure to the accumulation buffer: the surface
// potential changes by -(I / rho) w_i / (h^2 sum_j w_j) over a Gaussian footprint w (sigma >= h) sampled at texel
// centres out to 4 sigma, normalised by its own discrete sum (the fixed-order group reduction), so the water receives
// exactly the impulse I whatever sigma is; 2^24 fixed point (order independent). Footprint texels outside the window
// are dropped (the impulse left the simulated patch).
#include "Ripple.hlsli"

#define RIPPLE_RHO 1000.0
#define RIPPLE_MAX_REACH 64

groupshared float g_sum[256];

[numthreads(256, 1, 1)]
void main(uint t : SV_GroupThreadID, uint g : SV_GroupID)
{
    if (g >= P[2].y) return;
    ByteAddressBuffer sources = ResourceDescriptorHeap[P[2].x];
    RWByteAddressBuffer accum = ResourceDescriptorHeap[P[0].w];
    const float4 s = asfloat(sources.Load4(16 * g));
    const float h = rippleH(), sigma = max(s.z, h);
    const int reach = min(int(ceil(4.0 * sigma / h)), RIPPLE_MAX_REACH), width = 2 * reach + 1;
    const int2 centre = int2(floor(s.xy + 0.5));
    const float inv2s2 = 1.0 / (2.0 * sigma * sigma);
    float partial = 0;
    for (int i = int(t); i < width * width; i += 256)
    {
        const float2 d = (float2(centre + int2(i % width - reach, i / width - reach)) - s.xy) * h;
        partial += exp(-dot(d, d) * inv2s2);
    }
    g_sum[t] = partial;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint stride = 128; stride > 0; stride >>= 1)
    {
        if (t < stride) g_sum[t] += g_sum[t + stride];
        GroupMemoryBarrierWithGroupSync();
    }
    const float amplitude = -(s.w / RIPPLE_RHO) / (g_sum[0] * h * h);
    for (int j = int(t); j < width * width; j += 256)
    {
        const int2 texel = centre + int2(j % width - reach, j / width - reach);
        if (any(texel < 0) || any(texel >= int(RIPPLE_N))) continue;
        const float2 d = (float2(texel) - s.xy) * h;
        const float value = amplitude * exp(-dot(d, d) * inv2s2);
        accum.InterlockedAdd(8 * (texel.y * RIPPLE_N + texel.x) + 4, int(round(value * RIPPLE_FIXED)));
    }
}
