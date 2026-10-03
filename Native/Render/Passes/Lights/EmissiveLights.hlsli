// Emissive surfaces as area lights (RENDERER_REDESIGN_V2 14.1b, L2b; owner A): the quadtree image of
// unx/lights/EmissiveLights.h, walked per receiver. Readers: M (ShadeOpaque, diffuse direct light of the converted
// emitters), later the tile corners (L2), S's froxels, R's cells and hits.
//   header 32 B { planeCount, nodeCount, planesOffset, nodesOffset, bitsetOffset, bitsetWords, 0, 0 }
//   plane 64 B  { origin, pad, right, pad, up, pad, normal, rootNode }
//   node 48 B   { centre.xy, half, luminance, radiance.xyz, plane, firstChild (4 consecutive quadrants, missing =
//                 0xFFFFFFFF; leaf = 0xFFFFFFFF), pad x3 }
//   bitset      converted materials (bit per Scene::materials index)
// Node selection (14.1b, distance rule): a node is evaluated as one Lambert polygon of its mean radiance when its
// diagonal a_n <= 0.8 d (d the distance from the receiver to the node's square), else its children; leaves always.
// Evaluation: the LTC edge integral in the receiver's frame (AreaLight.hlsli shLtcSegment, horizon-clipped, exact for a
// polygon of constant radiance), irradiance = pi x form factor x radiance. A receiver behind a plane sees none of it.
// Exposure floor: a node whose peak illuminance L A / d^2 is under epsAbs is skipped (every receiving pixel changes by
// less than half a display code at this frame's exposure: the absolute, display-grade omission; the cumulative 1e-3
// rule of 14.1b joins with the tile term).
#ifndef UNX_EMISSIVE_LIGHTS_HLSLI
#define UNX_EMISSIVE_LIGHTS_HLSLI
#include "Passes/Shading/AreaLight.hlsli"

#define EMISSIVE_STACK 48  // 3 x depth 14 + root per plane

struct EmissiveNodeRec
{
    float2 centre;
    float half, luminance;
    float3 radiance;
    uint plane, firstChild;
    bool fullCoverage;
};
EmissiveNodeRec emissiveNode(ByteAddressBuffer b, uint nodesOffset, uint i)
{
    const uint o = nodesOffset + i * 48;
    const float4 a = asfloat(b.Load4(o)), c = asfloat(b.Load4(o + 16));
    const uint2 d = b.Load2(o + 28);
    EmissiveNodeRec n;
    n.centre = a.xy;
    n.half = a.z;
    n.luminance = a.w;
    n.radiance = c.xyz;
    n.plane = d.x;
    n.firstChild = d.y;
    n.fullCoverage = b.Load(o + 36) != 0;
    return n;
}

// True when material 'm' was converted to node lights (the double-counting rule: GI update rays and reflection hits
// record emission 0 for it; its direct view keeps the material's emission).
bool emissiveLightsConverted(uint buffer, uint m)
{
    if (buffer == 0xFFFFFFFFu) return false;
    ByteAddressBuffer b = ResourceDescriptorHeap[buffer];
    const uint bitsetOffset = b.Load(16), words = b.Load(20);
    if ((m >> 5) >= words) return false;
    return (b.Load(bitsetOffset + (m >> 5) * 4) & (1u << (m & 31))) != 0;
}

// Diffuse irradiance (W/m^2 per unit colour: rgb nits x pi x form factor) at xRel (camera-relative, as the shading
// kernels keep positions) from every converted emitter, for the
// receiving frame T (rows: tangent, bitangent, normal; a rotation): the Lambert integral over each selected node clipped
// at the receiver's horizon. epsAbs: the exposure floor (illuminance). evaluated: nodes integrated (statistics).
float3 emissiveLightsIrradiance(uint buffer, float3 xRel, float3x3 T, float epsAbs, out uint evaluated)
{
    evaluated = 0;
    float3 E = 0;
    if (buffer == 0xFFFFFFFFu) return E;
    ByteAddressBuffer b = ResourceDescriptorHeap[buffer];
    const uint planeCount = b.Load(0), planesOffset = b.Load(8), nodesOffset = b.Load(12);
    // The absolute omission allowance belongs to the whole query, not every leaf.
    // Descending a partially filled node must not multiply the allowed omitted energy.
    const float nodeAllowance = epsAbs / max(b.Load(4), 1u);
    uint stack[EMISSIVE_STACK];
    for (uint p = 0; p < planeCount; ++p)
    {
        const uint po = planesOffset + p * 64;
        const float3 origin = asfloat(b.Load3(po)), right = asfloat(b.Load3(po + 16)), up = asfloat(b.Load3(po + 32)), normal = asfloat(b.Load3(po + 48));
        const uint root = b.Load(po + 60);
        if (root == 0xFFFFFFFFu) continue;
        const float3 originRel = origin - g_cameraPosition;
        const float3 rel = xRel - originRel;
        const float height = dot(rel, normal);
        if (height <= 0) continue;  // behind the emitting side: none of the plane's nodes is seen
        const float2 xy = float2(dot(rel, right), dot(rel, up));
        uint top = 0;
        stack[top++] = root;
        [loop] while (top > 0)
        {
            const EmissiveNodeRec n = emissiveNode(b, nodesOffset, stack[--top]);
            if (!(n.luminance > 0)) continue;
            // distance to the square (closest point on it) and the node's diagonal
            const float2 q = clamp(xy, n.centre - n.half, n.centre + n.half);
            const float2 dq = xy - q;
            const float d2 = dot(dq, dq) + height * height, d = sqrt(d2);
            const float area = 4 * n.half * n.half;
            if (n.luminance * area < nodeAllowance * d2) continue;
            const float diag = 2.8284271 * n.half;
            if (n.firstChild != 0xFFFFFFFFu && (!n.fullCoverage || diag > 0.8 * d))
            {
                if (top + 4 <= EMISSIVE_STACK)
                {
                    [unroll] for (uint c = 0; c < 4; ++c)
                    {
                        const uint child = n.firstChild + c;
                        stack[top++] = child;
                    }
                }
                continue;
            }
            // one Lambert polygon: the square's corners relative to x, in the receiver's frame
            const float3 c0 = originRel + right * (n.centre.x - n.half) + up * (n.centre.y - n.half) - xRel;
            const float3 ex = right * (2 * n.half), ey = up * (2 * n.half);
            ShLtcAcc acc = shLtcBegin();
            const float3 a0 = mul(T, c0), a1 = mul(T, c0 + ex), a2 = mul(T, c0 + ex + ey), a3 = mul(T, c0 + ey);
            shLtcSegment(acc, a0, a1);
            shLtcSegment(acc, a1, a2);
            shLtcSegment(acc, a2, a3);
            shLtcSegment(acc, a3, a0);
            const float I = shLtcFinish(acc);
            E += n.radiance * (SH_PI * I);
            ++evaluated;
        }
    }
    return E;
}

#endif
