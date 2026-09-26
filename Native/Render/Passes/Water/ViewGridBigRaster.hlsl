// unx-kernel: cs_6_6 main
// Water view grid, big triangles, step 2 (ViewGridRaster.hlsli): one thread per 8 x 8 tile of a big triangle's pixel
// box; the thread finds its triangle in the exclusive prefix (binary search) and rasterises its tile.
// P[0] counter SRV (raw), big list SRV (raw), prefix SRV (raw), big capacity; P[1] width, height, key UAV, 0
#include "ViewGridRaster.hlsli"

[numthreads(64, 1, 1)]
void main(uint3 group : SV_GroupID, uint lane : SV_GroupIndex)
{
    ByteAddressBuffer counters = ResourceDescriptorHeap[P[0].x];
    ByteAddressBuffer big = ResourceDescriptorHeap[P[0].y];
    ByteAddressBuffer prefix = ResourceDescriptorHeap[P[0].z];
    const uint tile = (group.y * 65535u + group.x) * 64 + lane;
    if (tile >= counters.Load(8)) return;
    const uint n = min(counters.Load(0), P[0].w);
    // The last triangle whose first tile is <= tile.
    uint lo = 0, hi = n;
    while (hi - lo > 1)
    {
        const uint mid = (lo + hi) / 2;
        if (prefix.Load(mid * 4) <= tile) lo = mid; else hi = mid;
    }
    const uint4 r0 = big.Load4(lo * VG_BIG_RECORD), r1 = big.Load4(lo * VG_BIG_RECORD + 16), r2 = big.Load4(lo * VG_BIG_RECORD + 32);
    VgTarget t = { P[1].z, 0, 0, 0, P[1].x, P[1].y };  // vgRasterBox writes the keys only
    const int2 a = asint(r0.zw), b = asint(r1.xy), c = asint(r1.zw);
    int2 first, last;
    if (!vgBox(t, a, b, c, first, last)) return;
    const uint local = tile - prefix.Load(lo * 4), across = uint(last.x - first.x) / VG_TRIANGLE_PIXELS + 1;
    const int2 tileFirst = first + int2(local % across, local / across) * VG_TRIANGLE_PIXELS;
    vgRasterBox(t, a, b, c, asfloat(r2.x), asfloat(r2.y), asfloat(r2.z), r0.x, tileFirst, min(tileFirst + VG_TRIANGLE_PIXELS - 1, last));
}
