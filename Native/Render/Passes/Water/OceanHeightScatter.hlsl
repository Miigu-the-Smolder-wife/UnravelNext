// unx-kernel: cs_6_6 main
// Water height clipmap, step 1 of the build (OceanHeight.hlsli, FEATURES_GAME 1.8): the upper envelope of the displaced
// surface as a mesh. Level l's rest lattice (the clipmap lattice widened by M = ceil(R / s_l) + 1 points on every side,
// R the bound of the horizontal displacement) is displaced with the field low-passed to s_l; each rest quad is split into
// two triangles and every clipmap point a displaced triangle covers receives the triangle's interpolated height and rest
// position through a 64-bit atomic max, height first: the highest sheet wins wherever the surface folds (the upper
// envelope, exact for this mesh; inverted triangles of a fold cover points like any other). Key = ordered height (32) |
// (x0 - w) / s_l in 1/64 steps (2 x 16, biased): OceanHeightResolve polishes it on the continuous field.
// Root constants: P[0] displacement SRV, key UAV (raw, level-major 512^2 x 8 B), margin M, levels; P[1], P[2] as
// OceanHeight.hlsli; P[3] counter UAV (raw: +4 = points past a triangle's 8 x 8 loop), 0, level, slopes SRV.
#include "OceanHeight.hlsli"

#define OH_TRIANGLE_POINTS 8  // lattice points per triangle and axis (structural bound; the excess is counted)

groupshared float3 g_point[9 * 9];  // displaced rest point: lattice coordinates (x, z) relative to origin_l, height

uint ohOrderedHeight(float h)
{
    const uint u = asuint(h);
    return (u & 0x80000000u) ? ~u : (u | 0x80000000u);
}

void ohCover(float3 a, float3 b, float3 c, float2 ra, float2 rb, float2 rc, uint level)
{
    const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (abs(area) < 1e-12) return;
    const float2 lo = min(min(a.xy, b.xy), c.xy), hi = max(max(a.xy, b.xy), c.xy);
    const int2 first = max(int2(ceil(lo)), int2(0, 0)), last = min(int2(floor(hi)), int2(OH_N - 1, OH_N - 1));
    if (any(last < first)) return;
    if (any(last - first >= OH_TRIANGLE_POINTS))
    {
        RWByteAddressBuffer counter = ResourceDescriptorHeap[P[3].x];
        counter.InterlockedAdd(4, 1u);
    }
    const int2 stop = min(last, first + OH_TRIANGLE_POINTS - 1);
    RWByteAddressBuffer keys = ResourceDescriptorHeap[P[0].y];
    const float eps = -1e-6;
    for (int z = first.y; z <= stop.y; ++z)
        for (int x = first.x; x <= stop.x; ++x)
        {
            const float2 p = float2(x, z);
            // Barycentrics by signed areas (either orientation); inclusive edges so neighbours leave no gap.
            const float l0 = ((b.x - p.x) * (c.y - p.y) - (b.y - p.y) * (c.x - p.x)) / area;
            const float l1 = ((c.x - p.x) * (a.y - p.y) - (c.y - p.y) * (a.x - p.x)) / area;
            const float l2 = 1 - l0 - l1;
            if (l0 < eps || l1 < eps || l2 < eps) continue;
            const float h = l0 * a.z + l1 * b.z + l2 * c.z;
            const float2 offset = clamp((l0 * ra + l1 * rb + l2 * rc - p) * 64.0 + 32768.0, 0.0, 65535.0);
            const uint low = (uint(offset.x) << 16) | uint(offset.y);
            const uint64_t key = (uint64_t(ohOrderedHeight(h)) << 32) | uint64_t(low);
            keys.InterlockedMax64(((level * OH_N + uint(z)) * OH_N + uint(x)) * 8, key);
        }
}

[numthreads(8, 8, 1)]
void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID, uint index : SV_GroupIndex)
{
    const uint level = P[3].z;
    const int margin = int(P[0].z);
    const int rest = int(OH_N) + 2 * margin;  // rest points per side
    const float s = ohLevelSpacing(level);
    const int2 origin = ohOrigin(level, asfloat(P[1].xy));
    const int2 base = int2(group.xy) * 8;
    for (uint k = index; k < 81; k += 64)
    {
        const int2 q = base + int2(k % 9, k / 9);
        float3 v = 0;
        if (all(q < rest))
        {
            const int2 r = q - margin;  // rest point relative to the lattice origin (cells)
            const float3 d = ohDisplacement(float2(origin + r) * s, s);
            v = float3(float2(r) + d.xz / s, asfloat(P[1].w) + d.y);
        }
        g_point[k] = v;
    }
    GroupMemoryBarrierWithGroupSync();
    const int2 q = base + int2(thread.xy);
    if (any(q + 1 >= rest)) return;
    const uint i = thread.y * 9 + thread.x;
    const float3 p00 = g_point[i], p10 = g_point[i + 1], p01 = g_point[i + 9], p11 = g_point[i + 10];
    const float2 r00 = float2(q - margin);
    ohCover(p00, p10, p11, r00, r00 + float2(1, 0), r00 + float2(1, 1), level);
    ohCover(p00, p11, p01, r00, r00 + float2(1, 1), r00 + float2(0, 1), level);
}
