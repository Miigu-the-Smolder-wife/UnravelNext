// Water view grid: the triangle raster shared by the far and near scatters and the big-triangle tiles (FEATURES_GAME
// 1.8 B.3). Vertices are screen positions in 1/256 px; pixel centres are tested with exact int64 edge functions,
// inclusive (a centre on a shared edge belongs to both triangles: watertight), and a covered pixel receives a 64-bit
// atomic min of (view depth | triangle id); 1 / depth is affine in screen space. A triangle whose pixel box exceeds
// 8 x 8 goes to the big-triangle list (48 B records), rasterised afterwards one 8 x 8 tile per thread
// (ViewGridBigScan, ViewGridBigRaster): every thread's work is bounded.
// Counter buffer (raw): +0 big triangles appended, +4 big triangles past the list's capacity (lost: the gate is 0),
// +8 big tiles; the tile pass's dispatch arguments are a separate buffer (ViewGridBigScan).
#ifndef UNX_WATER_VIEW_GRID_RASTER_HLSLI
#define UNX_WATER_VIEW_GRID_RASTER_HLSLI
#include "ViewGrid.hlsli"

#define VG_BIG_RECORD 48u

struct VgTarget
{
    uint keys, counters, big, bigCapacity, width, height;
};
int64_t vgEdge(int2 a, int2 b, int2 q) { return int64_t(b.x - a.x) * int64_t(q.y - a.y) - int64_t(b.y - a.y) * int64_t(q.x - a.x); }
// Pixel box of a triangle (centres (x + 0.5) 256 inside its bounds, clipped to the screen); false when empty.
bool vgBox(VgTarget t, int2 a, int2 b, int2 c, out int2 first, out int2 last)
{
    const int2 lo = min(min(a, b), c), hi = max(max(a, b), c);
    first = max((lo - 128 + 255) >> 8, int2(0, 0));
    last = min((hi - 128) >> 8, int2(t.width, t.height) - 1);
    return all(last >= first);
}
// The pixels of [first, stop] inside the triangle.
void vgRasterBox(VgTarget t, int2 a, int2 b, int2 c, float za, float zb, float zc, uint id, int2 first, int2 stop)
{
    int64_t area = vgEdge(a, b, c);
    if (area == 0) return;
    const int64_t sign = area > 0 ? 1 : -1;  // either orientation (a fold flips triangles)
    area *= sign;
    RWByteAddressBuffer keys = ResourceDescriptorHeap[t.keys];
    const float inverseArea = 1.0 / float(area);
    for (int y = first.y; y <= stop.y; ++y)
        for (int x = first.x; x <= stop.x; ++x)
        {
            const int2 q = int2(x, y) * 256 + 128;
            const int64_t w0 = vgEdge(b, c, q) * sign, w1 = vgEdge(c, a, q) * sign, w2 = vgEdge(a, b, q) * sign;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            const float depth = 1.0 / ((float(w0) * za + float(w1) * zb + float(w2) * zc) * inverseArea);
            keys.InterlockedMin64((uint(y) * t.width + uint(x)) * 8, (uint64_t(asuint(depth)) << 32) | uint64_t(id));
        }
}
void vgRasterTriangle(VgTarget t, int2 a, int2 b, int2 c, float za, float zb, float zc, uint id)
{
    if (vgEdge(a, b, c) == 0) return;
    int2 first, last;
    if (!vgBox(t, a, b, c, first, last)) return;
    if (any(last - first >= VG_TRIANGLE_PIXELS))
    {
        RWByteAddressBuffer counters = ResourceDescriptorHeap[t.counters];
        uint slot;
        counters.InterlockedAdd(0, 1u, slot);
        if (slot >= t.bigCapacity) { counters.InterlockedAdd(4, 1u); return; }
        RWByteAddressBuffer big = ResourceDescriptorHeap[t.big];
        const uint at = slot * VG_BIG_RECORD;
        big.Store4(at, uint4(id, 0, asuint(a)));
        big.Store4(at + 16, uint4(asuint(b), asuint(c)));
        big.Store4(at + 32, uint4(asuint(za), asuint(zb), asuint(zc), 0));
        return;
    }
    vgRasterBox(t, a, b, c, za, zb, zc, id, first, last);
}
#endif
