// unx-kernel: cs_6_6 main
// Water view grid, adaptive near field (ViewGrid.hlsli, FEATURES_GAME 1.8 B.2): one group per 8 x 8-quad block of near
// level l (world lattice, spacing s_l = s_0 2^l; level origins aligned so a block's children are whole blocks of level
// l - 1). The block's nearest possible distance t to the displaced surface is bounded from the ocean bounds pyramid
// (OceanBounds.hlsl: heights and horizontal displacement over the block's rest box); the block needs spacing
// s(t) = coefficient sqrt(max(t, t_floor)). If s_l fits (or l = 0) the block is drawn with a one-quad rim (10 x 10 quads,
// so blocks drawn at neighbouring levels overlap: no crack), else its four children go to the next level's list.
// Level L - 1 is dispatched over a rectangle of blocks around the camera, lower levels indirectly over their lists.
// Triangle ids as viewGridNearVertex: 0x80000000 | l << 28 | (quad b x 8191 + quad a) x 2 + triangle.
// P[0] params SRV, displacement SRV, key UAV (raw), counter UAV (raw, ViewGridRaster.hlsli; +12 blocks the ids or the
// lists could not hold, drawn at the coarser level instead); P[1] big list UAV, big capacity, slopes SRV, bounds pyramid
// SRV; P[2] level, input list SRV (raw: count, then int2 blocks from byte 16), next list UAV, list capacity; P[3] rectangle
// first block (int x, z), rectangle width (blocks), rectangle mode (1: the top level over the rectangle, 0: the list).
// Diagnostics: with a drawn-block list in the parameters (byte 416, 0 = none), every drawn block is appended (level, x, z).
#include "ViewGridRaster.hlsli"

groupshared int2 g_xy[121];
groupshared float2 g_zv[121];
groupshared uint g_draw;

// Largest h, smallest h and largest horizontal displacement of the ocean over the rest box [lo, hi] (metres), summed
// over the cascades (each from the pyramid mip whose 2 x 2 texels cover the box).
float3 vgOceanBounds(ViewGridParams p, float2 lo, float2 hi)
{
    Texture2DArray<float4> pyramid = ResourceDescriptorHeap[P[1].w];
    float3 b = 0;
    [unroll] for (uint c = 0; c < 3; ++c)
    {
        const float texel = p.lengths[c] / 512.0;
        const float2 a = lo / texel, z = hi / texel;  // texel units of mip 0 (texel i spans [i, i + 1])
        const uint mip = uint(clamp(ceil(log2(max(max(z.x - a.x, z.y - a.y), 1.0))), 0.0, 9.0));
        const int size = int(512u >> mip);
        const int2 first = int2(floor(a / float(1u << mip))), last = int2(floor(z / float(1u << mip)));
        float3 m = float3(-3.0e38, 3.0e38, 0);
        for (int y = first.y; y <= last.y; ++y)
            for (int x = first.x; x <= last.x; ++x)
            {
                const float4 t = pyramid.Load(int4(((x % size) + size) % size, ((y % size) + size) % size, c, mip));
                m = float3(max(m.x, t.x), min(m.y, t.y), max(m.z, t.z));
            }
        b += m;
    }
    return b;
}

[numthreads(8, 8, 1)]
void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID, uint index : SV_GroupIndex)
{
    const ViewGridParams p = viewGridParams(P[0].x);
    const uint level = P[2].x;
    const ViewGridNearLevel l = viewGridNearLevel(p, level);
    int2 block;
    if (P[3].w)
    {
        if (group.y * P[3].z + group.x >= P[3].z * P[3].z) return;
        block = asint(P[3].xy) + int2(group.xy);
    }
    else
    {
        ByteAddressBuffer list = ResourceDescriptorHeap[P[2].y];
        const uint entry = group.y * 65535u + group.x;
        if (entry >= min(list.Load(0), P[2].w)) return;
        block = asint(list.Load2(16 + 8 * entry));
        if (block.x == 0x7FFFFFFF) return;
    }
    if (index == 0)
    {
        uint draw = 0;
        // The block's rest box with its rim, from the camera; its surface box with the displacement bounds.
        const float2 lo = (float2(l.origin + block * 8) - 1.0) * l.spacing, hi = lo + 10.0 * l.spacing;
        const float2 near2 = clamp(p.camera.xz, lo, hi) - p.camera.xz;
        const float rMin = length(near2);
        if (rMin <= l.outer + l.spacing)
        {
            const float3 ob = vgOceanBounds(p, lo, hi);
            const float horizontal = max(rMin - ob.z, 0.0);
            const float below = p.camera.y - (p.waterLevel + ob.x), above = (p.waterLevel + ob.y) - p.camera.y;
            const float vertical = max(max(below, above), 0.0);
            const float t = sqrt(horizontal * horizontal + vertical * vertical);
            const float required = viewGridNearCoefficient(p) * sqrt(max(t, viewGridNearFloor(p)));
            // Visible at all: the box's directions widened by the bound's angle must meet the screen window.
            bool visible = true;
            const float widen = viewGridWiden(p, rMin, ob.z + max(abs(ob.x), abs(ob.y)));
            if (widen < 1.5)
            {
                const float4 window = viewGridWindow(p);
                const float centre = 0.5 * (window.x + window.y);
                float phiLo = 1e9, phiHi = -1e9;
                [unroll] for (uint k = 0; k < 4; ++k)
                {
                    const float2 corner = float2((k & 1) ? hi.x : lo.x, (k & 2) ? hi.y : lo.y) - p.camera.xz;
                    float phi = atan2(corner.y, corner.x);
                    phi += 6.2831853 * round((centre - phi) / 6.2831853);
                    phiLo = min(phiLo, phi);
                    phiHi = max(phiHi, phi);
                }
                const float h = p.camera.y - p.waterLevel, rMax = length(max(abs(lo - p.camera.xz), abs(hi - p.camera.xz)));
                const float eLo = -atan(h / max(rMin, 1e-3)), eHi = -atan(h / max(rMax, 1e-3));
                const float widenAzimuth = viewGridWidenAzimuth(rMin, ob.z + max(abs(ob.x), abs(ob.y)));
                visible = (widenAzimuth > 3.1415927 || phiHi - phiLo > 3.1415927 || (phiHi + widenAzimuth >= window.x && phiLo - widenAzimuth <= window.y)) &&
                          eHi + widen >= window.z && eLo - widen <= window.w;
            }
            if (visible)
            {
                if (level == 0 || l.spacing <= required) draw = 1;
                else
                {
                    // Children: level l - 1's quads 2 (origin_l + 8 block) - origin_(l-1) .. + 16, whole blocks.
                    const ViewGridNearLevel c = viewGridNearLevel(p, level - 1);
                    const int2 child = (2 * (l.origin + block * 8) - c.origin) / 8;
                    const bool encodable = all(child >= 1) && all(child * 8 + 17 < int(c.points) - 1);
                    RWByteAddressBuffer next = ResourceDescriptorHeap[P[2].z];
                    uint slot = 0;
                    if (encodable) next.InterlockedAdd(0, 4u, slot);
                    if (encodable && slot + 4 <= P[2].w)
                    {
                        [unroll] for (uint k = 0; k < 4; ++k) next.Store2(16 + 8 * (slot + k), asuint(child + int2(k & 1, k >> 1)));
                    }
                    else
                    {
                        // Slots taken past the capacity hold no block (the next level skips the sentinel).
                        if (encodable) [unroll] for (uint k = 0; k < 4; ++k) if (slot + k < P[2].w) next.Store2(16 + 8 * (slot + k), uint2(0x7FFFFFFFu, 0x7FFFFFFFu));
                        RWByteAddressBuffer counters = ResourceDescriptorHeap[P[0].w];
                        counters.InterlockedAdd(12, 1u);
                        draw = 1;  // the coarser level draws it
                    }
                }
            }
        }
        g_draw = draw;
    }
    GroupMemoryBarrierWithGroupSync();
    if (!g_draw) return;
    const uint drawn = viewGridDrawnList(p);
    if (drawn != 0 && index == 0)
    {
        RWByteAddressBuffer list = ResourceDescriptorHeap[drawn];
        uint slot;
        list.InterlockedAdd(0, 1u, slot);
        if (slot < (1u << 20)) list.Store3(16 + 16 * slot, uint3(level, asuint(block)));
    }
    const int2 base = block * 8 - 1;  // the rim's first quad
    for (uint k = index; k < 121; k += 64)
    {
        int2 xy;
        float inverseDepth;
        uint flag;
        float2 x0;
        viewGridNearVertex(p, level, base + int2(k % 11, k / 11), P[0].y, P[1].z, xy, inverseDepth, flag, x0);
        g_xy[k] = xy;
        g_zv[k] = float2(inverseDepth, float(flag));
    }
    GroupMemoryBarrierWithGroupSync();
    VgTarget t = { P[0].z, P[0].w, P[1].x, P[1].y, p.width, p.height };
    for (uint q = index; q < 100; q += 64)
    {
        const uint2 local = uint2(q % 10, q / 10);
        const uint i = local.y * 11 + local.x;
        const int2 quad = base + int2(local);
        const uint id = 0x80000000u | (level << 28) | ((uint(quad.y) * (l.points - 1) + uint(quad.x)) * 2);
        const float fa = g_zv[i].y, fb = g_zv[i + 1].y, fc = g_zv[i + 12].y, fd = g_zv[i + 11].y;
        if (fa != 0 && fb != 0 && fc != 0 && !(fa == 2 && fb == 2 && fc == 2))
            vgRasterTriangle(t, g_xy[i], g_xy[i + 1], g_xy[i + 12], g_zv[i].x, g_zv[i + 1].x, g_zv[i + 12].x, id);
        if (fa != 0 && fc != 0 && fd != 0 && !(fa == 2 && fc == 2 && fd == 2))
            vgRasterTriangle(t, g_xy[i], g_xy[i + 12], g_xy[i + 11], g_zv[i].x, g_zv[i + 12].x, g_zv[i + 11].x, id + 1);
    }
}
