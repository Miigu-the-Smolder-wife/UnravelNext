// unx-kernel: cs_6_6 main
// Water view grid, far-field scatter (ViewGrid.hlsli, FEATURES_GAME 1.8 B): a group owns 8 x 8 grid quads, evaluates
// their 9 x 9 vertices (viewGridVertex; the halo row and column again, bit-identical to the neighbour's) into
// groupshared, and each thread rasterises its quad's two triangles (ViewGridRaster.hlsli). A triangle is drawn when a
// vertex is inside the water body (the surface reaches one cell past the shore). A group whose block, widened by the
// displacement bound's angle at its nearest row, misses the screen window returns at once.
// P[0] params SRV, displacement SRV, key UAV (raw, 8 B per pixel), counter UAV (raw, ViewGridRaster.hlsli);
// P[1] displacement bound (m, float), window phi min, phi max, e min; P[2] window e max, big list UAV, big capacity, slopes SRV
#include "ViewGridRaster.hlsli"

groupshared int2 g_xy[81];
groupshared float2 g_zv[81];  // 1 / depth, 0 invalid / 1 inside the water body / 2 outside
groupshared uint g_run;

void vgQuadTriangle(VgTarget t, uint ia, uint ib, uint ic, uint id)
{
    const float fa = g_zv[ia].y, fb = g_zv[ib].y, fc = g_zv[ic].y;
    if (fa == 0 || fb == 0 || fc == 0 || (fa == 2 && fb == 2 && fc == 2)) return;
    vgRasterTriangle(t, g_xy[ia], g_xy[ib], g_xy[ic], g_zv[ia].x, g_zv[ib].x, g_zv[ic].x, id);
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
        const float widen = viewGridWiden(p, nearest, asfloat(P[1].x));
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
        viewGridVertex(p, base + int2(k % 9, k / 9), P[0].y, P[2].w, xy, inverseDepth, flag, x0);
        g_xy[k] = xy;
        g_zv[k] = float2(inverseDepth, float(flag));
    }
    GroupMemoryBarrierWithGroupSync();
    const int2 q = base + int2(thread.xy);
    if (any(q + 1 >= int2(p.columns, p.rows))) return;
    VgTarget t = { P[0].z, P[0].w, P[2].y, P[2].z, p.width, p.height };
    const uint i = thread.y * 9 + thread.x;
    const uint id = (uint(q.y) * p.columns + uint(q.x)) * 2;
    vgQuadTriangle(t, i, i + 1, i + 10, id);
    vgQuadTriangle(t, i, i + 10, i + 9, id + 1);
}
