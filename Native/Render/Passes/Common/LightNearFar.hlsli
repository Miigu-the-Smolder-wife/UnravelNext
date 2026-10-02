// NEAR / FAR classification of a light for a receiver region and the FAR term's vector irradiance
// (RENDERER_REDESIGN_V2 14.1, 14.1c, 14.4; ARCHITECTURE 12.4's tile-corner rule extended). One place for the conditions,
// so the tile FAR term (M, TileLights / ShadeOpaque), the coverage-record FAR field (M, 14.1c), the GI cell FAR term (R,
// GiTrace / HitLocalLights) and the froxel classification (S) agree light by light. Owner: A (shared header; changes are
// announced to M, S2 and R).
//
// Receiver region: every receiving point x lies inside the sphere |x - centre| <= halfExtent (a tile: the 4 corner
// surface points' bounding sphere; a GI cell: 2x its footprint; a froxel depth interval: tile section x interval), and
// every receiving normal n lies inside the cone (normalAxis, half-angle with sine normalSin).
//
// A light is NEAR for the region (evaluated exactly per receiving point: LTC / closed forms, per-pixel visibility) when
// any of these holds, else FAR (one vector irradiance E for the region, interpolated by the receivers, E(n) = n . E):
//   1. interpolation: 6 (t / d)^2 > 1e-3 with t = 2 halfExtent the region's width and d the distance from the light's
//      centre to the region's sphere (the FAR irradiance varies over the region by that fraction);
//   2. area light point equivalence: (r_e / d)^2 > 1e-3 with r_e the emitter's extent (the far-field form below is
//      exact to that order; rect lights are evaluated as polygons, exact at any distance, so they skip this test);
//   3. horizon: some receiving normal of the cone has n . l < NF_HORIZON_MARGIN towards some point of the emitter
//      (E(n) = n . E needs every FAR light above every receiver's horizon with margin; a normal map that leaves the
//      cone widens it, and the light becomes NEAR for that region alone);
//   4. the region has a depth discontinuity (an edge tile; the caller's flag);
//   5. the light casts a shadow the caller has not classified as fully lit over the region (14.3's coarse
//      classification pages; until they exist every shadow caster is NEAR: exact, as today).
// The sum of FAR vector irradiances over the region's lights is exact for every receiver whose normal is inside the
// cone, up to the 1e-3 interpolation and point-equivalence terms; nothing is dropped or merged (the omission rule of
// 14.1b, cumulative 1e-3 of the region's FAR sum, is the caller's).
#ifndef UNX_LIGHT_NEAR_FAR_HLSLI
#define UNX_LIGHT_NEAR_FAR_HLSLI
#include "Scene.hlsli"

#define NF_TOLERANCE 1e-3
#define NF_HORIZON_MARGIN 0.2
#define NF_PI 3.14159265

struct NfRegion
{
    float3 centre;
    float halfExtent;     // radius of the sphere holding every receiving point (m)
    float3 normalAxis;    // unit axis of the receiving normals' cone
    float normalSin;      // sine of the cone's half-angle (0: one normal)
};

// Emitter extent r_e of a light (froxelLightRadius minus the range): 0 for point and spot lights.
float nfLightExtent(GpuLight l)
{
    const uint t = lightType(l);
    return t == LIGHT_TUBE ? 0.5 * l.size.x + l.size.y : (t == LIGHT_RECT ? 0.5 * length(l.size) : (t == LIGHT_DISK || t == LIGHT_SPHERE ? l.size.x : 0.0));
}

// Distance window w(d) of INTERFACES 8.2 (shPunctualIlluminance / shAreaWindow / froxelWindow): 0 beyond the range.
float nfWindow(GpuLight l, float d) { return lightWindow(l, d); }

// Conditions 1-3 for a light whose shadow (5) and the region's continuity (4) the caller handles; d returns the distance
// from the light's centre to the region's sphere.
bool nfIsNearGeometric(GpuLight l, NfRegion r, out float d)
{
    const float3 v = l.position - r.centre;
    const float dist = length(v);
    d = max(dist - r.halfExtent, 1e-4);
    const float t = 2 * r.halfExtent;
    if (6 * t * t > NF_TOLERANCE * d * d) return true;                        // 1. interpolation
    const float re = nfLightExtent(l);
    if (lightType(l) != LIGHT_RECT && re * re > NF_TOLERANCE * d * d) return true;  // 2. point equivalence
    // 3. horizon: the least n . l over the cone (half-angle theta: cos(alpha + theta)) and over the emitter's extent and
    // the region's extent seen from the region (angular radius <= (r_e + h) / dist).
    const float3 lh = v / max(dist, 1e-9);
    const float cosA = dot(r.normalAxis, lh), sinA = sqrt(saturate(1 - cosA * cosA));
    const float cosT = sqrt(saturate(1 - r.normalSin * r.normalSin));
    const float least = cosA * cosT - sinA * r.normalSin - (re + r.halfExtent) / max(dist, 1e-9);
    return least < NF_HORIZON_MARGIN;
}

// Conditions 1-2 alone (the coverage FAR field, 14.1c: the horizon is handled per record by direction bins).
bool nfIsNearGeometricNoHorizon(GpuLight l, NfRegion r, out float d)
{
    const float3 v = l.position - r.centre;
    d = max(length(v) - r.halfExtent, 1e-4);
    const float t = 2 * r.halfExtent;
    if (6 * t * t > NF_TOLERANCE * d * d) return true;
    const float re = nfLightExtent(l);
    return lightType(l) != LIGHT_RECT && re * re > NF_TOLERANCE * d * d;
}

// All five conditions. shadowLitOverRegion: the caller's verdict that the light's visibility is 1 over the whole region
// (false for every caster until 14.3's classification exists).
bool nfIsNear(GpuLight l, NfRegion r, bool discontinuous, bool shadowLitOverRegion, out float d)
{
    if (nfIsNearGeometric(l, r, d)) return true;
    if (discontinuous) return true;
    return lightCastsShadow(l) && !shadowLitOverRegion;
}

// Lambert vector irradiance of a planar polygon of constant radiance 1 at x (vertices relative to x, counter-clockwise
// seen from x's side; no horizon clipping: FAR polygons are above every receiver's horizon by condition 3):
// E = 1/2 sum_i gamma_i u_i with gamma_i the angle between consecutive vertex directions and u_i the unit normal of their
// plane. E(n) = n . E then equals the integral of cos over the polygon's solid angle (shLtcFinish's 1/(2 pi) sum times
// pi, AreaLight.hlsli). Exact at any distance.
float3 nfPolygonVector(float3 a, float3 b, float3 c, float3 d)
{
    float3 e = 0;
    float3 v[4] = { a, b, c, d };
    [unroll] for (uint i = 0; i < 4; ++i)
    {
        const float3 p = v[i], q = v[(i + 1) & 3];
        const float3 cr = cross(p, q);
        const float s = length(cr);
        if (s > 0) e += cr * (atan2(s, dot(p, q)) / s);
    }
    return 0.5 * e;
}

// FAR vector irradiance (W/m^2 per unit colour, times l.color) of light l at the receiving point x: the direction
// weighted irradiance a receiver of normal n gets as max(0, n . E). Point and spot: shPunctualIlluminance's value along
// the light direction (exact). Rect: the polygon form above with the window at the centre (exact; 0 behind the emitting
// side, as shAreaIntegral). Disk, sphere, tube: the far-field point equivalent (radiance x projected area / d^2, the
// window at the centre; error O((r_e / d)^2), bounded by condition 2). Visibility is the caller's (1 for FAR). The FAR
// term is diffuse light: the value carries the light's diffuse scale (a rect's barn doors are not in it).
float3 nfVectorIrradiance(GpuLight l, float3 x)
{
    const uint type = lightType(l);
    const float3 v = l.position - x;
    const float d2 = max(dot(v, v), 1e-12), d = sqrt(d2);
    const float3 lh = v / d;
    const float w = nfWindow(l, d) * lightDiffuseScale(l);
    if (w <= 0) return 0;
    if (type == LIGHT_POINT || type == LIGHT_SPOT)
    {
        float i = l.intensity * w / d2;
        if (type == LIGHT_SPOT)
        {
            const float sp = saturate(dot(-lh, l.forward) * l.spotScale + l.spotOffset);
            i *= sp * sp;
        }
        return l.color * (i * lh);
    }
    const float3 up = cross(l.forward, l.right);
    if (type == LIGHT_RECT)
    {
        if (dot(-v, l.forward) <= 0) return 0;  // behind the emitting side
        const float3 ex = l.right * (0.5 * l.size.x), ey = up * (0.5 * l.size.y);
        // Counter-clockwise seen from the receiver (the emitter faces it): the LTC segment order of shAreaIntegral.
        const float3 e = nfPolygonVector(v - ex - ey, v + ex - ey, v + ex + ey, v - ex + ey);
        return l.color * (l.intensity * w) * (dot(e, lh) >= 0 ? e : -e);
    }
    float area;
    if (type == LIGHT_DISK)
    {
        const float cosE = dot(-lh, l.forward);
        if (cosE <= 0) return 0;
        area = NF_PI * l.size.x * l.size.x * cosE;
    }
    else if (type == LIGHT_SPHERE) area = NF_PI * l.size.x * l.size.x;
    else
    {
        // Tube: cylinder of length size.x, radius size.y along l.right, hemispherical caps: projected area at angle phi
        // from the axis = 2 r L sin(phi) + pi r^2.
        const float cosP = abs(dot(lh, l.right)), sinP = sqrt(saturate(1 - cosP * cosP));
        area = 2 * l.size.y * l.size.x * sinP + NF_PI * l.size.y * l.size.y;
    }
    return l.color * (l.intensity * area * w / d2) * lh;
}

#endif
