// unx-kernel: cs_6_6 main
// Water view grid, scatter (ViewGrid.hlsli, FEATURES_GAME 1.8 B): a group owns 8 x 8 grid quads, evaluates their 9 x 9
// vertices (viewGridVertex; the halo row and column again, bit-identical to the neighbour's) into groupshared, and each
// thread scatters its quad's two triangles into the pixel centres
// with exact int64 edge functions on the 1/256 px vertices (inclusive: watertight) and a 64-bit atomic min of
// (view depth | triangle id). A triangle is drawn when a vertex is inside the water body (the surface reaches one cell
// past the shore). A group whose block, widened by the displacement bound's angle at its nearest row, misses the
// screen window returns at once. A triangle whose pixel box exceeds 8 x 8 is counted (structural loop bound).
// P[0] params SRV, displacement SRV, key UAV (raw, 8 B per pixel), counter UAV (raw: +0 triangles past the loop bound);
// P[1] displacement bound (m, float), window phi min, phi max, e min; P[2] window e max
#include "ViewGrid.hlsli"

groupshared int2 g_xy[81];
groupshared float2 g_zv[81];  // 1 / depth, 0 invalid / 1 inside the water body / 2 outside
groupshared uint g_run;

int64_t vgEdge(int2 a, int2 b, int2 q) { return int64_t(b.x - a.x) * int64_t(q.y - a.y) - int64_t(b.y - a.y) * int64_t(q.x - a.x); }

void vgScatter(ViewGridParams p, uint ia, uint ib, uint ic, uint id)
{
    const float fa = g_zv[ia].y, fb = g_zv[ib].y, fc = g_zv[ic].y;
    if (fa == 0 || fb == 0 || fc == 0 || (fa == 2 && fb == 2 && fc == 2)) return;
    const int2 a = g_xy[ia], b = g_xy[ib], c = g_xy[ic];
    int64_t area = vgEdge(a, b, c);
    if (area == 0) return;
    const int64_t sign = area > 0 ? 1 : -1;  // either orientation (a fold flips triangles)
    area *= sign;
    const int2 lo = min(min(a, b), c), hi = max(max(a, b), c);
    const int2 first = max((lo - 128 + 255) >> 8, int2(0, 0)), last = min((hi - 128) >> 8, int2(p.width, p.height) - 1);
    if (any(last < first)) return;
    if (any(last - first >= VG_TRIANGLE_PIXELS))
    {
        RWByteAddressBuffer counter = ResourceDescriptorHeap[P[0].w];
        counter.InterlockedAdd(0, 1u);
    }
    const int2 stop = min(last, first + VG_TRIANGLE_PIXELS - 1);
    RWByteAddressBuffer keys = ResourceDescriptorHeap[P[0].z];
    const float za = g_zv[ia].x, zb = g_zv[ib].x, zc = g_zv[ic].x, inverseArea = 1.0 / float(area);
    for (int y = first.y; y <= stop.y; ++y)
        for (int x = first.x; x <= stop.x; ++x)
        {
            const int2 q = int2(x, y) * 256 + 128;
            const int64_t w0 = vgEdge(b, c, q) * sign, w1 = vgEdge(c, a, q) * sign, w2 = vgEdge(a, b, q) * sign;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            const float depth = 1.0 / ((float(w0) * za + float(w1) * zb + float(w2) * zc) * inverseArea);  // 1 / z is affine
            keys.InterlockedMin64((uint(y) * p.width + uint(x)) * 8, (uint64_t(asuint(depth)) << 32) | uint64_t(id));
        }
}

[numthreads(8, 8, 1)]
void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID, uint index : SV_GroupIndex)
{
    const ViewGridParams p = viewGridParams(P[0].x);
    const int2 base = int2(group.xy) * 8;
    if (index == 0)
    {
        const float phiLo = p.phi0 + float(base.x) * p.theta, phiHi = phiLo + 8 * p.theta;
        const float height = p.camera.y - p.waterLevel;
        const float nearest = viewGridRow(p, uint(base.y)).x, farthest = viewGridRow(p, uint(base.y) + 8).x;
        const float eLo = -atan(height / max(nearest, 1e-3)), eHi = -atan(height / max(farthest, 1e-3));
        const float widen = asfloat(P[1].x) / max(nearest, 1e-3);
        g_run = (phiHi + widen >= asfloat(P[1].y) && phiLo - widen <= asfloat(P[1].z) && eHi + widen >= asfloat(P[1].w) && eLo - widen <= asfloat(P[2].x)) ? 1 : 0;
    }
    GroupMemoryBarrierWithGroupSync();
    if (!g_run) return;
    for (uint k = index; k < 81; k += 64)
    {
        int2 xy;
        float inverseDepth;
        uint flag;
        float2 x0;
        viewGridVertex(p, base + int2(k % 9, k / 9), P[0].y, xy, inverseDepth, flag, x0);
        g_xy[k] = xy;
        g_zv[k] = float2(inverseDepth, float(flag));
    }
    GroupMemoryBarrierWithGroupSync();
    const int2 q = base + int2(thread.xy);
    if (any(q + 1 >= int2(p.columns, p.rows))) return;
    const uint i = thread.y * 9 + thread.x;
    const uint id = (uint(q.y) * p.columns + uint(q.x)) * 2;
    vgScatter(p, i, i + 1, i + 10, id);
    vgScatter(p, i, i + 10, i + 9, id + 1);
}
