// unx-kernel: cs_6_6 main
// Water view grid, near-field scatter (ViewGrid.hlsli, FEATURES_GAME 1.8 B.2): dispatch z = near level; a group owns
// 8 x 8 quads of the level's world lattice, evaluates their 9 x 9 vertices (viewGridNearVertex, bit-identical in every
// evaluation) and rasterises the quads' triangles (ViewGridRaster.hlsli) with ids 0x80000000 | level << 28 |
// (quad index x 2 + triangle). A triangle is drawn when a vertex lies in the level's ring and the water body (rings
// overlap by a cell: no gap between levels or with the far field). A group whose block lies outside its ring, or whose
// block widened by the displacement bound's angle misses the screen window, returns at once.
// P[0] params SRV, displacement SRV, key UAV, counter UAV; P[1] bound (m, float), window phi min, phi max, e min;
// P[2] window e max, big list UAV, big capacity, slopes SRV
#include "ViewGridRaster.hlsli"

groupshared int2 g_xy[81];
groupshared float2 g_zv[81];
groupshared uint g_run;

static const float kVgPi = 3.14159265;

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
    const uint level = group.z;
    if (level >= viewGridNearLevels(p)) return;
    const ViewGridNearLevel l = viewGridNearLevel(p, level);
    const int2 base = int2(group.xy) * 8;
    if (any(base + 1 >= int(l.points))) return;
    if (index == 0)
    {
        // The block's rest box, its distance range and azimuth range from the camera.
        const float2 lo = float2(l.origin + base) * l.spacing - p.camera.xz, hi = lo + 8 * l.spacing;
        const float2 nearestPoint = clamp(float2(0, 0), lo, hi);
        const float rMin = length(nearestPoint), rMax = length(max(abs(lo), abs(hi)));
        bool run = rMax >= l.inner - 2 * l.spacing && rMin <= l.outer + 2 * l.spacing;
        if (run && viewGridWiden(p, rMin, asfloat(P[1].x)) < 1.5)  // otherwise every direction is reachable
        {
            const float centre = 0.5 * (asfloat(P[1].y) + asfloat(P[1].z));
            float phiLo = 1e9, phiHi = -1e9;
            [unroll] for (uint k = 0; k < 4; ++k)
            {
                const float2 corner = float2((k & 1) ? hi.x : lo.x, (k & 2) ? hi.y : lo.y);
                float phi = atan2(corner.y, corner.x);
                phi += 2 * kVgPi * round((centre - phi) / (2 * kVgPi));  // nearest to the window's centre
                phiLo = min(phiLo, phi);
                phiHi = max(phiHi, phi);
            }
            const float height = p.camera.y - p.waterLevel, widen = viewGridWiden(p, rMin, asfloat(P[1].x));
            const float eLo = -atan(height / rMin), eHi = -atan(height / max(rMax, 1e-3));
            run = (phiHi - phiLo > kVgPi) ||  // a block spanning half the circle: keep
                  (phiHi + widen >= asfloat(P[1].y) && phiLo - widen <= asfloat(P[1].z));
            run = run && eHi + widen >= asfloat(P[1].w) && eLo - widen <= asfloat(P[2].x);
        }
        g_run = run ? 1 : 0;
    }
    GroupMemoryBarrierWithGroupSync();
    if (!g_run) return;
    for (uint k = index; k < 81; k += 64)
    {
        int2 xy;
        float inverseDepth;
        uint flag;
        float2 x0;
        viewGridNearVertex(p, level, base + int2(k % 9, k / 9), P[0].y, P[2].w, xy, inverseDepth, flag, x0);
        g_xy[k] = xy;
        g_zv[k] = float2(inverseDepth, float(flag));
    }
    GroupMemoryBarrierWithGroupSync();
    const int2 q = base + int2(thread.xy);
    if (any(q + 1 >= int(l.points))) return;
    VgTarget t = { P[0].z, P[0].w, P[2].y, P[2].z, p.width, p.height };
    const uint i = thread.y * 9 + thread.x;
    const uint id = 0x80000000u | (level << 28) | ((uint(q.y) * (l.points - 1) + uint(q.x)) * 2);
    vgQuadTriangle(t, i, i + 1, i + 10, id);
    vgQuadTriangle(t, i, i + 10, i + 9, id + 1);
}
