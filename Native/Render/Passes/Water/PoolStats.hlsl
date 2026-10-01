// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1
// Closed basins (W2): the surface's statistics for the host (UnxPoolStatsLatest; FEATURES_GAME 1.10 "max |eta - mean|"
// decides the plane + normal-field drawing, the game reads the agitation): over the 257 x 257 samples of the field the
// mean of eta, the RMS of eta - mean and max |eta - mean|. Deterministic: fixed-order tree reductions.
//   MODE 0: one group per row z (257): the row's sum, sum of squares, max and min of eta -> partials[z] (16 B).
//   MODE 1: one group: the rows in order -> (mean, rms, maxDeviation, 0) at partials[257].
// P[0].x field SRV (Texture2D<float4>: eta in .x), P[0].y statistics UAV (raw: 257 x 16 B partials, then the result)
#include "Bindless.hlsli"

#define POOL_STATS_Q 257u

groupshared float4 gs_v[256];

float4 reduceTree(uint t, float4 v)
{
    gs_v[t] = v;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint s = 128; s > 0; s >>= 1)
    {
        if (t < s)
        {
            const float4 o = gs_v[t + s];
            gs_v[t] = float4(gs_v[t].x + o.x, gs_v[t].y + o.y, max(gs_v[t].z, o.z), min(gs_v[t].w, o.w));
        }
        GroupMemoryBarrierWithGroupSync();
    }
    return gs_v[0];
}

[numthreads(256, 1, 1)]
void main(uint t : SV_GroupThreadID, uint z : SV_GroupID)
{
    RWByteAddressBuffer stats = ResourceDescriptorHeap[P[0].y];
#if MODE == 0
    Texture2D<float4> field = ResourceDescriptorHeap[P[0].x];
    float4 v = float4(0, 0, -1e30, 1e30);  // sum, sum of squares, max, min
    [unroll] for (uint part = 0; part < 2; ++part)
    {
        const uint x = t + part * 256u;
        if (x < POOL_STATS_Q)
        {
            const float e = field.Load(int3(x, z, 0)).x;
            v = float4(v.x + e, v.y + e * e, max(v.z, e), min(v.w, e));
        }
    }
    const float4 r = reduceTree(t, v);
    if (t == 0) stats.Store4(z * 16, asuint(r));
#else
    float4 v = float4(0, 0, -1e30, 1e30);
    [unroll] for (uint part = 0; part < 2; ++part)
    {
        const uint z2 = t + part * 256u;
        if (z2 < POOL_STATS_Q)
        {
            const float4 p = asfloat(stats.Load4(z2 * 16));
            v = float4(v.x + p.x, v.y + p.y, max(v.z, p.z), min(v.w, p.w));
        }
    }
    const float4 r = reduceTree(t, v);
    if (t == 0)
    {
        const float n = float(POOL_STATS_Q * POOL_STATS_Q);
        const float mean = r.x / n;
        const float rms = sqrt(max(r.y / n - mean * mean, 0.0));
        const float dev = max(r.z - mean, mean - r.w);
        stats.Store4(POOL_STATS_Q * 16, asuint(float4(mean, rms, dev, 0)));
    }
#endif
}
