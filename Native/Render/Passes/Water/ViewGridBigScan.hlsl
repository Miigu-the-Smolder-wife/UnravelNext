// unx-kernel: cs_6_6 main
// Water view grid, big triangles, step 1 (ViewGridRaster.hlsli): one group of 1024 threads turns the list's tile counts
// (ceil(box width / 8) x ceil(box height / 8) per triangle) into an exclusive prefix (triangle -> its first tile), the
// total and the dispatch arguments of the tile pass (64 tiles per group; x at most 65535, y the rest).
// P[0] counter UAV (raw), big list UAV (raw), prefix UAV (raw, uint per triangle), big capacity; P[1] width, height,
// dispatch argument UAV (raw: x, y, z)
#include "ViewGridRaster.hlsli"

groupshared uint g_sum[1024];

uint vgTiles(RWByteAddressBuffer big, uint i, VgTarget t)
{
    const uint4 r0 = big.Load4(i * VG_BIG_RECORD), r1 = big.Load4(i * VG_BIG_RECORD + 16);
    int2 first, last;
    if (!vgBox(t, asint(r0.zw), asint(r1.xy), asint(r1.zw), first, last)) return 0;
    const uint2 tiles = uint2(last - first) / VG_TRIANGLE_PIXELS + 1;
    return tiles.x * tiles.y;
}

[numthreads(1024, 1, 1)]
void main(uint lane : SV_GroupIndex)
{
    RWByteAddressBuffer counters = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer big = ResourceDescriptorHeap[P[0].y];
    RWByteAddressBuffer prefix = ResourceDescriptorHeap[P[0].z];
    const uint n = min(counters.Load(0), P[0].w);
    VgTarget t = { 0, 0, 0, 0, P[1].x, P[1].y };
    const uint chunk = (n + 1023) / 1024, begin = min(lane * chunk, n), end = min(begin + chunk, n);
    uint sum = 0;
    for (uint i = begin; i < end; ++i) sum += vgTiles(big, i, t);
    g_sum[lane] = sum;
    GroupMemoryBarrierWithGroupSync();
    // Inclusive scan of the 1024 partial sums (Hillis-Steele).
    for (uint offset = 1; offset < 1024; offset <<= 1)
    {
        const uint add = lane >= offset ? g_sum[lane - offset] : 0;
        GroupMemoryBarrierWithGroupSync();
        g_sum[lane] += add;
        GroupMemoryBarrierWithGroupSync();
    }
    uint running = g_sum[lane] - sum;
    for (uint i = begin; i < end; ++i)
    {
        prefix.Store(i * 4, running);
        running += vgTiles(big, i, t);
    }
    if (lane == 1023)
    {
        const uint total = g_sum[1023], groups = (total + 63) / 64;
        const uint x = min(groups, 65535u), y = x ? (groups + x - 1) / x : 0;
        counters.Store(8, total);
        RWByteAddressBuffer arguments = ResourceDescriptorHeap[P[1].z];
        arguments.Store3(0, uint3(x, y, groups ? 1u : 0u));
    }
}
