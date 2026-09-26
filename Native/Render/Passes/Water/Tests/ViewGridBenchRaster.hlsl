// unx-kernel: cs_6_6 main
// View grid microbench (FEATURES_GAME 1.8 B), the whole far-field scatter: a group owns 8 x 8 grid quads; its 9 x 9
// points are displaced (3 cascades) and projected once into groupshared; each thread scatters its quad's two
// triangles into the pixels (centre samples; vertices snapped to 1/256 px and exact integer edge functions, inclusive,
// so the mesh is watertight) with a 64-bit atomic min of (view depth | triangle id).
// A group whose grid block, widened by the displacement bound's angle at its nearest distance, misses the screen
// returns at once.
// P[0] params SRV (ViewGrid.hlsli), displacement SRV, key UAV (raw, 8 B per pixel), counter UAV (raw: +0 triangles past
// the 8 x 8 loop, +4 groups that ran, +8 non-finite displacements); P[1] mode (bit 0: SampleGrad aniso 16 instead of
// SampleLevel at the across footprint; bit 1: read the vertices of ViewGridBenchVertex instead of computing them here),
// displacement bound (m, float), screen window phi min, phi max; P[2] screen window e min, e max, vertex SRV (bit 1)
#include "../ViewGrid.hlsli"

groupshared int2 g_xy[81];     // screen position in 1/256 px (fixed point: exact, watertight edge functions)
groupshared float2 g_zv[81];   // 1 / depth, 0 invalid / 1 inside the water body / 2 outside it
groupshared uint g_run;

int64_t edge(int2 a, int2 b, int2 q) { return int64_t(b.x - a.x) * int64_t(q.y - a.y) - int64_t(b.y - a.y) * int64_t(q.x - a.x); }

void scatter(ViewGridParams p, uint ia, uint ib, uint ic, uint id)
{
    if (g_zv[ia].y == 0 || g_zv[ib].y == 0 || g_zv[ic].y == 0) return;
    if (g_zv[ia].y == 2 && g_zv[ib].y == 2 && g_zv[ic].y == 2) return;  // wholly outside the water body
    const int2 a = g_xy[ia], b = g_xy[ib], c = g_xy[ic];
    int64_t area = edge(a, b, c);
    if (area == 0) return;
    const int64_t sign = area > 0 ? 1 : -1;  // either orientation (a fold flips triangles)
    area *= sign;
    const int2 lo = min(min(a, b), c), hi = max(max(a, b), c);
    // Pixel centres (x + 0.5) 256 inside the box.
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
            // Exact edge functions; inclusive, so a pixel centre on a shared edge is covered by both triangles.
            const int64_t w0 = edge(b, c, q) * sign, w1 = edge(c, a, q) * sign, w2 = edge(a, b, q) * sign;
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
        // The block's angular box widened by the bound's angle at its nearest point, against the screen's window.
        const float phiLo = p.phi0 + float(base.x) * p.theta, phiHi = phiLo + 8 * p.theta;
        const float height = p.camera.y - p.waterLevel;
        const float nearest = viewGridRow(p, uint(base.y)).x, farthest = viewGridRow(p, uint(base.y) + 8).x;
        const float eLo = -atan(height / max(nearest, 1e-3)), eHi = -atan(height / max(farthest, 1e-3));
        const float widen = asfloat(P[1].y) / max(nearest, 1e-3);
        g_run = (phiHi + widen >= asfloat(P[1].z) && phiLo - widen <= asfloat(P[1].w) && eHi + widen >= asfloat(P[2].x) && eLo - widen <= asfloat(P[2].y)) ? 1 : 0;
        if (g_run)
        {
            RWByteAddressBuffer counter = ResourceDescriptorHeap[P[0].w];
            counter.InterlockedAdd(4, 1u);
        }
    }
    GroupMemoryBarrierWithGroupSync();
    if (!g_run) return;
    for (uint k = index; k < 81; k += 64)
    {
        const int2 q = base + int2(k % 9, k / 9);
        if (P[1].x & 2)
        {
            uint3 v = 0;
            if (all(q < int2(p.columns, p.rows)))
            {
                ByteAddressBuffer vertices = ResourceDescriptorHeap[P[2].z];
                v = vertices.Load3((uint(q.y) * p.columns + uint(q.x)) * 12);
            }
            g_xy[k] = asint(v.xy);
            g_zv[k] = float2(abs(asfloat(v.z)), v.z == 0 ? 0 : (asfloat(v.z) > 0 ? 1 : 2));
            continue;
        }
        int2 xy = 0;
        float2 zv = 0;
        float2 x0;
        float d;
        if (all(q < int2(p.columns, p.rows)) && viewGridRest(p, q, x0, d))
        {
            const float2 footprint = viewGridFootprint(p, d, q.y);
            const float2 along = normalize(x0 - p.camera.xz);
            const float3 disp = (P[1].x & 1) ? viewGridDisplacementAniso(P[0].y, p.lengths, x0, footprint, along)
                                       : viewGridDisplacementTrilinear(P[0].y, p.lengths, x0, footprint.x);
            if (any(!isfinite(disp)))
            {
                RWByteAddressBuffer counter = ResourceDescriptorHeap[P[0].w];
                counter.InterlockedAdd(8, 1u);  // a sampler returned a non-finite displacement (checked by the bench)
            }
            const float3 s = viewGridProject(p, float3(x0.x + disp.x, p.waterLevel + disp.y, x0.y + disp.z));
            if (s.z > 0.01)
            {
                xy = int2(round(clamp(s.xy, -4.0e6, 4.0e6) * 256.0));
                zv = float2(1.0 / s.z, viewGridWater(p, x0, length(footprint)) ? 1 : 2);
            }
        }
        g_xy[k] = xy;
        g_zv[k] = zv;
    }
    GroupMemoryBarrierWithGroupSync();
    const int2 q = base + int2(thread.xy);
    if (any(q + 1 >= int2(p.columns, p.rows))) return;
    const uint i = thread.y * 9 + thread.x;
    const uint id = (uint(q.y) * p.columns + uint(q.x)) * 2;
    scatter(p, i, i + 1, i + 10, id);
    scatter(p, i, i + 10, i + 9, id + 1);
}
