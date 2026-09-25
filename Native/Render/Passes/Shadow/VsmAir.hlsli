// Sun visibility of the air (froxel integration, ARCHITECTURE 2.3 "볼륨 산란"). Owner: S.
// The VSM stores, per texel, the top of a solid caster column seen from the sun; an air point is in the casters' shadow
// when a column above it reaches higher towards the sun. Along a straight segment the light-space height is linear, so
// the shadowed length is a 1D measure that the block hierarchy settles for whole sub-segments at once.
#ifndef UNX_VSM_AIR_HLSLI
#define UNX_VSM_AIR_HLSLI
#include "Passes/Shadow/VsmSample.hlsli"

#define VSM_AIR_DEPTH 5u  // halvings of a mixed segment (at most 32 leaves)

// Level for the air of a froxel 'width' metres wide: the finest level whose texel is not larger than width / texelsPerTile
// (atmosphere.froxels.shadow_texels_per_tile: a shadow boundary crossing the tile-centre segment is placed to that
// fraction of the tile). The page requests (VsmMarkAir) and the lookup use this rule.
uint vsmAirLevel(ConstantBuffer<VsmConstants> c, float width, float texelsPerTile) { return vsmLevelForFootprint(c, width / texelsPerTile); }

// Square (light-space lateral centre, radius) the classification of segment p0 -> p1 (light space) reads.
void vsmAirSquare(float3 p0, float3 p1, out float2 centre, out float radius)
{
    centre = 0.5 * (p0.xy + p1.xy);
    radius = 0.5 * length(p1.xy - p0.xy);
}

// Fraction (by length) of the world segment a -> b below the height field held at level k (or the finest resident level
// above it). Sub-segments are settled by vsmRegionClassify against the plane that contains them (gradient along the
// segment, none across it: every texel under the segment is compared with the segment's own height); mixed ones are
// halved (stackless in-order walk) down to a texel or VSM_AIR_DEPTH halvings. A leaf takes the exact fraction of its
// linear height profile below the texel under its midpoint (exact when the leaf lies over one texel).
float vsmAirShadowFraction(VsmResources r, float3 a, float3 b, uint k)
{
    ConstantBuffer<VsmConstants> vc = ResourceDescriptorHeap[r.cbv];
    const float3 pa = vsmLightSpace(vc, a), pb = vsmLightSpace(vc, b);
    const float texel = vsmTexel(k);
    float shadowed = 0;
    uint depth = 0, index = 0;  // node covers [index, index + 1] / 2^depth of the segment
    [loop] for (uint guard = 0; guard < 2u << VSM_AIR_DEPTH; ++guard)
    {
        const float scale = 1.0 / float(1u << depth);
        const float s0 = index * scale, s1 = s0 + scale;
        const float3 p0 = lerp(pa, pb, s0), p1 = lerp(pa, pb, s1);
        const float lateral = length(p1.xy - p0.xy);
        bool descend = false;
        if (lateral <= texel || depth == VSM_AIR_DEPTH)
        {
            const float3 m = 0.5 * (p0 + p1);
            const uint e = vsmHeightAt(r, vsmAbsTexel(vc, m.xy, k), k);
            if (e != VSM_EMPTY)
            {
                const float H = vsmDecode(e), lo = min(p0.z, p1.z), hi = max(p0.z, p1.z);
                shadowed += scale * (hi > lo ? saturate((H - lo) / (hi - lo)) : (lo < H ? 1.0 : 0.0));
            }
        }
        else
        {
            VsmReceiver rc;
            float radius;
            vsmAirSquare(p0, p1, rc.uv, radius);
            rc.h = 0.5 * (p0.z + p1.z);
            rc.slope = (p1.xy - p0.xy) * ((p1.z - p0.z) / (lateral * lateral));
            rc.bias = 0;
            const uint cls = vsmRegionClassify(r, rc, rc.uv, radius, k);
            if (cls == VSM_REGION_UMBRA) shadowed += scale;
            descend = cls == VSM_REGION_MIXED;
        }
        if (descend)
        {
            ++depth;
            index <<= 1;
            continue;
        }
        while ((index & 1) != 0 && depth > 0)
        {
            index >>= 1;
            --depth;
        }
        if (depth == 0) break;
        ++index;
    }
    return shadowed;
}

#endif
