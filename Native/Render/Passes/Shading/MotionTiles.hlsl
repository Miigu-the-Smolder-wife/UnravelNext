// unx-kernel: cs_6_6 main
// unx-variants: STEP=0,1
// Motion blur (MotionBlur.cpp) velocity tiles, 32 x 32 pixels (the longest streak the gather covers, MOTION_TILE):
//   STEP=0: the tile's velocity of largest magnitude (ties: the first in the tile's row-major order, deterministic);
//   STEP=1: the largest of the 3 x 3 tiles around (a streak reaching into this tile starts at most one tile away).
// P[0] = { source SRV (velocity RG16F or tile maxima), destination UAV (RG16F), source width, height }.
#include "Bindless.hlsli"

#define MOTION_TILE 32u

#if STEP == 0
groupshared float4 gs_best[256];  // (velocity, |v|^2, index)

[numthreads(16, 16, 1)]
void main(uint2 gid : SV_GroupID, uint2 gtid : SV_GroupThreadID, uint flat : SV_GroupIndex)
{
    Texture2D<float2> velocity = ResourceDescriptorHeap[P[0].x];
    float4 best = float4(0, 0, -1, 0);
    [unroll] for (uint q = 0; q < 4; ++q)
    {
        const uint2 local = gtid * 2 + uint2(q & 1, q >> 1);
        const uint2 px = gid * MOTION_TILE + local;
        if (any(px >= P[0].zw)) continue;
        const float2 v = velocity.Load(int3(px, 0));
        const float m = dot(v, v);
        const float index = (float)(local.y * MOTION_TILE + local.x);
        if (m > best.z || (m == best.z && index < best.w)) best = float4(v, m, index);
    }
    gs_best[flat] = best;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 128; s > 0; s >>= 1)
    {
        if (flat < s)
        {
            const float4 a = gs_best[flat], b = gs_best[flat + s];
            gs_best[flat] = b.z > a.z || (b.z == a.z && b.w < a.w) ? b : a;
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (flat == 0)
    {
        RWTexture2D<float2> tiles = ResourceDescriptorHeap[P[0].y];
        tiles[gid] = gs_best[0].z >= 0 ? gs_best[0].xy : float2(0, 0);
    }
}
#else
[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= P[0].zw)) return;
    Texture2D<float2> tiles = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<float2> neighbour = ResourceDescriptorHeap[P[0].y];
    float2 best = 0;
    float m = -1;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            const int2 t = int2(id) + int2(x, y);
            if (any(t < 0) || any(t >= int2(P[0].zw))) continue;
            const float2 v = tiles.Load(int3(t, 0));
            const float mv = dot(v, v);
            if (mv > m) { m = mv; best = v; }
        }
    neighbour[id] = best;
}
#endif
