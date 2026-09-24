// Sun visibility through the virtual shadow map (ARCHITECTURE 2.3 "SMRT", 2.11). Owner: S.
// The sun is a disk of angular radius theta_s; every stored texel is the top of a solid column (the height field seen
// from the sun). See vsmSunVisibility for the estimator and its exactness condition.
#ifndef UNX_VSM_SAMPLE_HLSLI
#define UNX_VSM_SAMPLE_HLSLI
#include "Passes/Shadow/VsmCommon.hlsli"

#define ATMO_PI_FOR_VSM 3.14159265358979323846

struct VsmResources
{
    ByteAddressBuffer table;
    Texture2D<uint> pool;
    StructuredBuffer<uint4> meta;
    VsmConstants c;
};

// Page entry of an absolute page at level k, or 0 when not resident or out of the window.
uint vsmEntry(VsmResources r, int2 page, uint k)
{
    if (!vsmInWindow(r.c, page, k)) return 0;
    const uint2 e = r.table.Load2(vsmSlot(page, k) * 8);
    return ((e.x & VSM_FLAG_RESIDENT) != 0 && e.y == vsmTag(page)) ? e.x : 0;
}

// Encoded height at an absolute texel of level k, from the finest resident level at or above k (a coarser level stores
// the same height field at a coarser texel). Returns VSM_EMPTY when no level holds it.
uint vsmHeightAt(VsmResources r, int2 texel, uint k)
{
    [loop] for (uint j = k; j < VSM_LEVELS; ++j)
    {
        const int2 t = texel >> (int)(j - k);
        const uint e = vsmEntry(r, t >> (int)VSM_PAGE_SHIFT, j);
        if (e != 0) return r.pool.Load(int3(vsmPhysBase(r.c, e & VSM_PHYS_MASK) + uint2(t & (int)(VSM_PAGE - 1)), 0));
    }
    return VSM_EMPTY;
}

// The receiver's plane in light space: its height above a lateral offset q from the receiver is hr + dot(slope, q).
// Comparing every texel with the plane at the texel's own centre removes self-shadowing on any planar receiver without
// moving the lookup point (a normal offset shifts the whole penumbra laterally by about a texel).
struct VsmReceiver
{
    float2 uv;      // light-space lateral position
    float h;        // light-space height (towards the sun)
    float2 slope;   // height gradient of the receiver's plane, clamped
    float bias;     // texel-scale tolerance (the stored height is the surface at the texel centre)
};

// Height the receiver's plane has under texel tj of level k, plus the level's tolerance.
float vsmPlaneHeight(VsmConstants c, VsmReceiver rc, int2 tj, uint k)
{
    const float2 centre = (float2(tj) + 0.5) * c.level[k].texel;
    return rc.h + dot(rc.slope, centre - rc.uv) + rc.bias * c.level[k].texel * (1 + length(rc.slope));
}

// Fraction (bilinear over the 2 x 2 texels around uv, level k) of columns that rise above the receiver's plane.
float vsmOccupancy(VsmResources r, VsmReceiver rc, float2 uv, uint k)
{
    const float2 t = uv / r.c.level[k].texel - 0.5;
    const int2 t0 = int2(floor(t));
    const float2 f = t - float2(t0);
    float o[4];
    [unroll] for (uint j = 0; j < 4; ++j)
    {
        const int2 tj = t0 + int2(j & 1, j >> 1);
        o[j] = vsmHeightAt(r, tj, k) > vsmEncode(vsmPlaneHeight(r.c, rc, tj, k)) ? 1.0 : 0.0;
    }
    return lerp(lerp(o[0], o[1], f.x), lerp(o[2], o[3], f.x), f.y);
}

// Highest caster over the 3 x 3 pages around uv at level k (resident ones; coarser level where absent).
float vsmSearchHeight(VsmResources r, float2 uv, uint k)
{
    uint m = VSM_EMPTY;
    const int2 page = vsmAbsPage(vsmAbsTexel(r.c, uv, k));
    [unroll] for (int dy = -1; dy <= 1; ++dy)
        [unroll] for (int dx = -1; dx <= 1; ++dx)
        {
            [loop] for (uint j = k; j < VSM_LEVELS; ++j)
            {
                const uint e = vsmEntry(r, (page + int2(dx, dy)) >> (int)(j - k), j);
                if (e != 0)
                {
                    m = max(m, r.meta[e & VSM_PHYS_MASK].w);
                    break;
                }
            }
        }
    return m == VSM_EMPTY ? -3.0e38 : vsmDecode(m);
}

// Sunflower point i of n in the unit disk (equal area).
float2 vsmDiskPoint(uint i, uint n)
{
    const float rr = sqrt((i + 0.5) / n);
    const float a = i * 2.399963229728653;
    return rr * float2(cos(a), sin(a));
}

// Level at or above k whose texel is closest to (not above) 'size' metres.
uint vsmLevelForSize(VsmConstants c, uint k, float size)
{
    return min(k + (uint)max(0.0, floor(log2(max(size / c.level[k].texel, 1.0)))), VSM_LEVELS - 1);
}

// Sun visibility in [0, 1] of a receiver at world position p with shading normal n and pixel footprint (m).
// A column of the height field at lateral offset q from the receiver, at height d above it, blocks exactly the disk
// directions within one texel of q / (d tan theta_s): an occluder at a single height d_b blocks the part of the disk
// that its footprint covers inside the lateral disk of radius d_b tan theta_s. So:
//  1. reach: the highest caster over the neighbouring pages (page maxima) bounds d; nothing above -> lit;
//  2. blocker height: `searchTaps` bilinear taps over the reach disk, on the level whose texel matches their spacing;
//     d_b = mean height above the receiver of the texels that can block some sun direction; none -> lit;
//  3. visibility = 1 - mean occupancy of `filterTaps` bilinear taps (sunflower) over the disk of radius d_b tan theta_s,
//     taken from the level whose texel matches their spacing, so the result is continuous in the receiver position.
// Exact for an occluder at one height above the receiver when the taps resolve its footprint; with blockers at several
// heights d_b is their mean (contact hardening follows the nearest dominant blocker). No noise, no per-pixel pattern.
float vsmSunVisibility(VsmResources r, float3 p, float3 n, float footprint, float tanSun, uint searchTaps, uint filterTaps)
{
    const VsmConstants c = r.c;
    const uint k = vsmLevelForFootprint(c, footprint);
    const float3 ls = vsmLightSpace(c, p);
    const float3 nl = vsmLightSpace(c, n);
    VsmReceiver rc;
    rc.uv = ls.xy;
    rc.h = ls.z;
    const float2 slope = -nl.xy / max(abs(nl.z), 1e-4);
    rc.slope = slope * min(1.0, c.maxReceiverSlope / max(length(slope), 1e-6));
    rc.bias = c.receiverBiasTexels;
    const float dmax = vsmSearchHeight(r, ls.xy, k) - ls.z;
    if (dmax <= 0) return 1;
    // 2. Blocker height over the reach disk.
    const float reach = dmax * tanSun;
    const uint ks = vsmLevelForSize(c, k, reach * sqrt(ATMO_PI_FOR_VSM / searchTaps));
    float sum = 0, count = 0;
    [loop] for (uint i = 0; i < searchTaps; ++i)
    {
        const float2 t = (ls.xy + vsmDiskPoint(i, searchTaps) * reach) / c.level[ks].texel - 0.5;
        const int2 t0 = int2(floor(t));
        [unroll] for (uint j = 0; j < 4; ++j)
        {
            const int2 tj = t0 + int2(j & 1, j >> 1);
            const uint e = vsmHeightAt(r, tj, ks);
            const float plane = vsmPlaneHeight(c, rc, tj, ks);
            if (e <= vsmEncode(plane)) continue;
            const float d = vsmDecode(e) - plane;
            // Lateral distance of the texel from the receiver (less half a texel diagonal) within the cone it can shade.
            const float2 centre = (float2(tj) + 0.5) * c.level[ks].texel - ls.xy;
            if (length(centre) - 0.7071 * c.level[ks].texel <= d * tanSun)
            {
                sum += d;
                count += 1;
            }
        }
    }
    if (count == 0) return 1;
    // 3. Occupancy over the penumbra disk.
    const float radius = (sum / count) * tanSun;
    const uint kf = vsmLevelForSize(c, k, radius * sqrt(ATMO_PI_FOR_VSM / filterTaps));
    float occ = 0;
    [loop] for (uint i = 0; i < filterTaps; ++i)
        occ += vsmOccupancy(r, rc, ls.xy + vsmDiskPoint(i, filterTaps) * radius, kf);
    return 1 - occ / filterTaps;
}

#endif
