// Area lights (INTERFACES 8.2): rect and disk (one-sided, emitting along +forward, axes right and up = forward x
// right), sphere (radius size.x) and tube (capsule along 'right', length size.x, radius size.y), uniform radiance
// L = intensity (nits) x colour, windowed by w(distance to the light's centre) like punctual lights. Owner: M.
//
// A light's radiance at the viewer is L w (f_d int_A cos dw + int_A f_s cos dw). Both integrals run over the light's
// solid angle A, clipped to the shading point's hemisphere, as integrals of a clamped cosine D_o over a linearly
// transformed region (Heitz et al. 2016, linearly transformed cosines):
//   diffuse   M = identity: int_A cos dw = pi I(A);
//   specular  the lobe f_s cos (F = 1) / E is fitted by D_o transformed by M (LtcTable.inl, Tests/LtcFit.cpp; the fit's
//             error is the quality definition's term), so int_A f_s cos dw = E_s(v) I(M^-1 A) with E_s the model's
//             Fresnel-split directional albedo (shSpecularAlbedo, multiple scattering included).
// I(region) = (1 / 2 pi) |sum over the region's boundary of the edge terms| (Lambert: a straight edge contributes its angle
// times the z component of its plane's unit normal). Boundaries are clipped to the horizon z >= 0 and closed by the chord
// on the horizon between the exit and entry directions (a light's region is a convex cone: one of each).
//   rect      four straight edges: exact.
//   disk      an ellipse after the transform. Above the horizon: exact closed form (the elliptic cone's axis from a secular
//             equation solved by Newton, its cosine integral from the eigenvalues' sum and product, no cubic). Crossing
//             it: a 32-gon inscribed in the cone's principal cross-section (uniform in the cross-section's angle, area
//             matched), clipped exactly; worst 1.3e-4 of the lobe against dense integration (Tests, Python check).
//   sphere    exactly the disk it subtends (centre c (1 - r^2/d^2), radius r sqrt(1 - r^2/d^2), facing the shading point).
//   tube      the capsule's exact outline as seen from the shading point: the cylinder's two silhouette generators (at
//             m.a_perp = -r, m perpendicular to the axis) and the outer arcs of the end spheres' silhouette circles
//             between their tangent points, each arc a 16-segment area-matched polyline.
#ifndef UNX_M_AREA_LIGHT_HLSLI
#define UNX_M_AREA_LIGHT_HLSLI
#include "Bindless.hlsli"
#include "Scene.hlsli"
#include "Passes/Shading/ShadingCommon.hlsli"

#define SH_LTC_SIZE 64u
#define SH_CONE_SIDES 32u
#define SH_CONE_AREA_MATCH 1.0032221  // sqrt((2 pi / 32) / sin(2 pi / 32)): the 32-gon's area equals the ellipse's
#define SH_ARC_SEGMENTS 16u

// M^-1 of the fitted lobe in the shading frame (x = tangent towards v, y = n x t, z = n), bilinear over the table's
// (sqrt(1 - NoV), roughness) grid; P = StructuredBuffer<float4> of 64 x 64 (a, b, c, d): [[a, 0, b], [0, 1, 0], [c, 0, d]].
float3x3 shLtcInverse(uint table, float NoV, float roughness)
{
    StructuredBuffer<float4> t = ResourceDescriptorHeap[table];
    const float2 c = float2(sqrt(saturate(1 - NoV)), saturate(roughness)) * (SH_LTC_SIZE - 1);
    const uint2 i0 = min(uint2(c), SH_LTC_SIZE - 2);
    const float2 f = c - float2(i0);
    const uint k = i0.y * SH_LTC_SIZE + i0.x;
    const float4 m = lerp(lerp(t[k], t[k + 1], f.x), lerp(t[k + SH_LTC_SIZE], t[k + SH_LTC_SIZE + 1], f.x), f.y);
    return float3x3(m.x, 0, m.y, 0, 1, 0, m.z, 0, m.w);
}

// Rows: the shading frame (tangent towards v, bitangent, normal); world vectors map to it by mul(frame, x).
float3x3 shShadingFrame(float3 n, float3 v, float NoV)
{
    float3 t = v - n * NoV;
    const float tl = dot(t, t);
    if (tl < 1e-12) t = abs(n.x) < 0.9 ? cross(n, float3(1, 0, 0)) : cross(n, float3(0, 1, 0));  // normal incidence
    t = normalize(t);
    return float3x3(t, cross(n, t), n);
}

// ---- boundary integration in the transformed space (points relative to the shading point)
struct ShLtcAcc
{
    float sum;              // sum of edge terms over the part above the horizon
    float3 exitPoint;       // where the boundary leaves z >= 0
    float3 entryPoint;      // where it comes back
    uint crossings;         // bit 0 exit seen, bit 1 entry seen
};

ShLtcAcc shLtcBegin()
{
    ShLtcAcc a;
    a.sum = 0;
    a.exitPoint = 0;
    a.entryPoint = 0;
    a.crossings = 0;
    return a;
}

// Angle between directions a and b times the z component of their plane's unit normal (either length).
float shLtcEdge(float3 a, float3 b)
{
    const float3 c = cross(a, b);
    const float s = length(c);
    return s > 0 ? atan2(s, dot(a, b)) * (c.z / s) : 0;
}

void shLtcSegment(inout ShLtcAcc acc, float3 a, float3 b)
{
    if (a.z < 0 && b.z < 0) return;
    float3 p = a, q = b;
    if (a.z < 0)
    {
        p = lerp(a, b, a.z / (a.z - b.z));
        p.z = 0;
        acc.entryPoint = p;
        acc.crossings |= 2;
    }
    else if (b.z < 0)
    {
        q = lerp(a, b, a.z / (a.z - b.z));
        q.z = 0;
        acc.exitPoint = q;
        acc.crossings |= 1;
    }
    acc.sum += shLtcEdge(p, q);
}

float shLtcFinish(ShLtcAcc acc)
{
    float s = acc.sum;
    if (acc.crossings == 3) s += shLtcEdge(acc.exitPoint, acc.entryPoint);
    return abs(s) * (0.5 / SH_PI);
}

// The elliptic cone over the ellipse c + u cos t + w sin t, in the frame of the ellipse's principal axes (w1, w2, normal
// w3 towards c): at unit distance along w3 the ellipse has centre (x0, y0) and semi-axes e1, e2, so the cone is
// ((X - x0 Z) / e1)^2 + ((Y - y0 Z) / e2)^2 <= Z^2. Its negative eigenvalue -mu solves
// mu (1 + x0^2 / (1 + mu e1^2) + y0^2 / (1 + mu e2^2)) = 1 (increasing and concave in mu: Newton from the thin-cone
// value 1 / (1 + x0^2 + y0^2) converges monotonically from below); the axis is (x0 / (1 + mu e1^2), y0 / (1 + mu e2^2), 1).
struct ShCone
{
    float3 w1, w2, w3;
    float x0, y0, e1, e2, mu;
    bool valid;
};

ShCone shLtcCone(float3 c, float3 u, float3 w)
{
    ShCone k;
    const float d11 = dot(u, u), d22 = dot(w, w), d12 = dot(u, w);
    const float ts = 0.5 * atan2(2 * d12, d11 - d22);
    float sn, cs;
    sincos(ts, sn, cs);
    const float3 p1 = u * cs + w * sn, p2 = w * cs - u * sn;
    const float l1 = length(p1), l2 = length(p2);
    k.w1 = p1 / max(l1, 1e-30);
    k.w2 = p2 / max(l2, 1e-30);
    k.w3 = cross(k.w1, k.w2);
    if (dot(k.w3, c) < 0) k.w3 = -k.w3;
    const float L = dot(k.w3, c);
    k.valid = l1 > 0 && l2 > 0 && L > 0;
    k.x0 = dot(k.w1, c) / L;
    k.y0 = dot(k.w2, c) / L;
    k.e1 = l1 / L;
    k.e2 = l2 / L;
    const float e1s = k.e1 * k.e1, e2s = k.e2 * k.e2, x2 = k.x0 * k.x0, y2 = k.y0 * k.y0;
    float mu = 1 / (1 + x2 + y2);
    [unroll] for (uint i = 0; i < 6; ++i)
    {
        const float a1 = 1 / (1 + mu * e1s), a2 = 1 / (1 + mu * e2s);
        const float f = mu * (1 + x2 * a1 + y2 * a2) - 1;
        mu -= f / (1 + x2 * a1 * a1 + y2 * a2 * a2);
    }
    k.mu = mu;
    return k;
}

// I of a full ellipse lying entirely above the horizon: the cone's cosine integral about its axis is
// pi mu^1.5 e1 e2 / sqrt(1 + mu^2 s + mu^3 e1^2 e2^2), s = e2^2 (1 + x0^2) + e1^2 (1 + y0^2) + (mu - 1) e1^2 e2^2 (the
// other two eigenvalues enter only through their sum and product), times the axis' z. Scale-free: no product of large
// eigenvalues is formed.
float shLtcEllipseAbove(float3 c, float3 u, float3 w)
{
    const ShCone k = shLtcCone(c, u, w);
    if (!k.valid) return 0;
    const float e1s = k.e1 * k.e1, e2s = k.e2 * k.e2, mu = k.mu;
    const float a1 = 1 / (1 + mu * e1s), a2 = 1 / (1 + mu * e2s);
    const float3 axis = normalize(k.w1 * (k.x0 * a1) + k.w2 * (k.y0 * a2) + k.w3);
    const float E = e1s * e2s, sE = e2s * (1 + k.x0 * k.x0) + e1s * (1 + k.y0 * k.y0) + (mu - 1) * E;
    return mu * sqrt(mu) * k.e1 * k.e2 / sqrt(1 + mu * mu * sE + mu * mu * mu * E) * axis.z;
}

// I of a full ellipse (either side of the horizon). Crossing it: the cone's principal cross-section at unit distance
// along its axis v3 is the centred ellipse (L1 cos psi, L2 sin psi) on (v1, v2), L_i = sqrt(mu / lambda_i) with lambda_i
// the positive eigenvalues (the 2 x 2 restriction of the cone's form to the plane normal to v3); the boundary is an
// area-matched 32-gon uniform in psi, whose angular speed stays even where the ellipse's own parameter would crowd.
float shLtcEllipse(float3 c, float3 u, float3 w)
{
    const float R = length(float2(u.z, w.z));
    if (c.z >= R) return shLtcEllipseAbove(c, u, w);
    if (c.z <= -R) return 0;
    const ShCone k = shLtcCone(c, u, w);
    if (!k.valid) return 0;
    const float e1s = k.e1 * k.e1, e2s = k.e2 * k.e2, E = e1s * e2s;
    const float a1 = 1 / (1 + k.mu * e1s), a2 = 1 / (1 + k.mu * e2s);
    const float3 a3 = normalize(float3(k.x0 * a1, k.y0 * a2, 1));  // axis in (w1, w2, w3)
    // The cone's form scaled by e1^2 e2^2, in (w1, w2, w3).
    const float3x3 Q = float3x3(e2s, 0, -e2s * k.x0, 0, e1s, -e1s * k.y0, -e2s * k.x0, -e1s * k.y0, e2s * k.x0 * k.x0 + e1s * k.y0 * k.y0 - E);
    const float3 s1 = normalize(cross(a3, abs(a3.x) < 0.9 ? float3(1, 0, 0) : float3(0, 1, 0))), s2 = cross(a3, s1);
    const float q11 = dot(s1, mul(Q, s1)), q12 = dot(s1, mul(Q, s2)), q22 = dot(s2, mul(Q, s2));
    const float th = 0.5 * atan2(2 * q12, q11 - q22);
    float sn, cs;
    sincos(th, sn, cs);
    const float3 b1 = s1 * cs + s2 * sn, b2 = s2 * cs - s1 * sn;
    // Both positive for a proper cone (the form is positive definite on the plane normal to its axis); guarded against
    // rounding for degenerate ones.
    const float lambda1 = max(dot(b1, mul(Q, b1)), 1e-30), lambda2 = max(q11 + q22 - dot(b1, mul(Q, b1)), 1e-30);
    const float L1 = sqrt(k.mu * E / lambda1) * SH_CONE_AREA_MATCH, L2 = sqrt(k.mu * E / lambda2) * SH_CONE_AREA_MATCH;
    const float3x3 toLtc = transpose(float3x3(k.w1, k.w2, k.w3));  // (X, Y, Z) in (w1, w2, w3) -> the transformed space
    const float3 v1 = mul(toLtc, b1) * L1, v2 = mul(toLtc, b2) * L2, v3 = mul(toLtc, a3);
    ShLtcAcc acc = shLtcBegin();
    float3 prev = v3 + v1 * cos(SH_PI / SH_CONE_SIDES) + v2 * sin(SH_PI / SH_CONE_SIDES);
    [loop] for (uint j = 1; j <= SH_CONE_SIDES; ++j)
    {
        float s, co;
        sincos(2 * SH_PI * ((j % SH_CONE_SIDES) + 0.5) / SH_CONE_SIDES, s, co);
        const float3 next = v3 + v1 * co + v2 * s;
        shLtcSegment(acc, prev, next);
        prev = next;
    }
    return shLtcFinish(acc);
}

// The circle a sphere of radius r at p (relative to the shading point, |p| > r) subtends: centre p (1 - r^2/|p|^2),
// radius r sqrt(1 - r^2/|p|^2), in the plane facing the shading point; axes u, w.
void shSubtendedCircle(float3 p, float r, out float3 c, out float3 u, out float3 w)
{
    const float d2 = dot(p, p), k = r * r / d2, rc = r * sqrt(1 - k);
    const float3 dir = p * rsqrt(d2);
    const float3 t1 = normalize(abs(dir.x) < 0.9 ? cross(dir, float3(1, 0, 0)) : cross(dir, float3(0, 1, 0)));
    c = p * (1 - k);
    u = t1 * rc;
    w = cross(dir, t1) * rc;
}

// The outer arc of an end sphere's silhouette circle (sphere at 'end', radius r) from its tangent point p0 to p1, bulging
// towards 'outward', as SH_ARC_SEGMENTS segments whose interior vertices are pushed out so each segment's area matches
// its sector; transformed by T and appended to acc (p0 excluded: the previous edge ends there).
void shLtcCapArc(inout ShLtcAcc acc, float3x3 T, float3 end, float r, float3 p0, float3 p1, float3 outward)
{
    const float d2 = dot(end, end), k = r * r / d2;
    const float3 cc = end * (1 - k);
    const float rc = r * sqrt(1 - k);
    const float3 e1 = normalize(p0 - cc);
    float3 e2 = normalize(cross(end, e1));
    if (dot(e2, outward) < 0) e2 = -e2;
    const float3 q = p1 - cc;
    float span = atan2(dot(q, e2), dot(q, e1));
    if (span <= 0) span += 2 * SH_PI;
    const float dt = span / SH_ARC_SEGMENTS, grow = sqrt(dt / sin(dt));
    float3 prev = mul(T, p0);
    [loop] for (uint m = 1; m <= SH_ARC_SEGMENTS; ++m)
    {
        float s, co;
        sincos(dt * m, s, co);
        const float3 next = m == SH_ARC_SEGMENTS ? mul(T, p1) : mul(T, cc + (e1 * co + e2 * s) * (rc * grow));
        shLtcSegment(acc, prev, next);
        prev = next;
    }
}

// ---- lights: I of the light's region under the transform T (world -> transformed space), p = light position relative to
// the shading point. Rect and disk: 0 on their back side; inside a sphere or tube: 0.
float shAreaIntegral(GpuLight l, float3 p, float3x3 T)
{
    const uint type = lightType(l);
    const float3 up = cross(l.forward, l.right);
    if ((type == LIGHT_RECT || type == LIGHT_DISK) && dot(-p, l.forward) <= 0) return 0;  // behind the emitting side
    if (type == LIGHT_RECT)
    {
        const float3 ex = l.right * (0.5 * l.size.x), ey = up * (0.5 * l.size.y);
        ShLtcAcc acc = shLtcBegin();
        const float3 a = mul(T, p - ex - ey), b = mul(T, p + ex - ey), c = mul(T, p + ex + ey), d = mul(T, p - ex + ey);
        shLtcSegment(acc, a, b);
        shLtcSegment(acc, b, c);
        shLtcSegment(acc, c, d);
        shLtcSegment(acc, d, a);
        return shLtcFinish(acc);
    }
    const float r = type == LIGHT_TUBE ? l.size.y : l.size.x;
    const float3 axis = l.right * l.size.x;
    const float3 a = p - 0.5 * axis, b = p + 0.5 * axis;
    const float3 aPerp = a - l.right * dot(a, l.right);
    const float dPerp = length(aPerp);
    // A tube whose axis line passes within r of the shading point is seen end-on: its nearer end's circle.
    const bool endOn = dPerp <= r * 1.0001;
    if (type != LIGHT_TUBE || endOn)
    {
        float3 ec, eu, ew;
        if (type == LIGHT_DISK)
        {
            ec = p;
            eu = l.right * l.size.x;
            ew = up * l.size.x;
        }
        else
        {
            const float3 centre = type == LIGHT_SPHERE ? p : (dot(a, a) < dot(b, b) ? a : b);
            if (dot(centre, centre) <= r * r) return 0;  // inside the emitter
            shSubtendedCircle(centre, r, ec, eu, ew);
        }
        return shLtcEllipse(mul(T, ec), mul(T, eu), mul(T, ew));
    }
    if (min(dot(a, a), dot(b, b)) <= r * r) return 0;
    // Silhouette generators a + r m +- s axis: m perpendicular to the axis with m . a = -r.
    const float3 ah = aPerp / dPerp, uh = normalize(cross(l.right, ah));
    const float along = sqrt(1 - r * r / (dPerp * dPerp));
    const float3 mp = ah * (-r / dPerp) + uh * along, mm = ah * (-r / dPerp) - uh * along;
    const float3 a0 = a + mp * r, b0 = b + mp * r, b1 = b + mm * r, a1 = a + mm * r;
    ShLtcAcc acc = shLtcBegin();
    shLtcSegment(acc, mul(T, a0), mul(T, b0));
    shLtcCapArc(acc, T, b, r, b0, b1, l.right);
    shLtcSegment(acc, mul(T, b1), mul(T, a1));
    shLtcCapArc(acc, T, a, r, a1, a0, -l.right);
    return shLtcFinish(acc);
}

// w(d) of the light's centre (INTERFACES 8.2), d = |p|.
float shAreaWindow(GpuLight l, float3 p)
{
    const float x = length(p) / max(l.range, 1e-6), x2 = x * x;
    const float w = saturate(1 - x2 * x2);
    return w * w;
}

#endif
