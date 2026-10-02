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
// I(region) = (1 / 2 pi) |sum over the region's boundary of the edge terms| (Lambert: a boundary piece contributes
// int (w x dw).z, a straight edge its angle times the z component of its plane's unit normal). Boundaries are clipped to
// the horizon z >= 0 and closed by the chord on the horizon between the exit and entry directions (a light's region is a
// convex cone: one of each). Every shape is integrated exactly (float rounding), so the quality definition's only term
// is the LTC fit (design revision 1 12.4, M correction: an area-matched N-gon of a clipped curve is off by 5-40 % at
// P99 for N = 8 and by 0.3-0.65 % for N = 32 [prototype, float64 against dense contours]; no curve is polygonised).
//   rect      four straight edges.
//   disk      diffuse, entirely above the horizon: the disk's vector irradiance (closed form). Otherwise the cone over
//             the disk in its principal frame (shConeArc), where the boundary x(psi) = v3 + L1 cos psi v1 + L2 sin psi v2
//             has |x|^2 = alpha + beta cos 2 psi, so the edge integrand is elementary (shArcF: an atan of tan psi, an
//             atanh of sin psi, an atan of cos psi); the horizon cuts it at the roots of one cosine.
//   sphere    diffuse: Snyder's form factor of a sphere (a horizon cutting it included). Specular: the disk it subtends
//             (centre c (1 - r^2/d^2), radius r sqrt(1 - r^2/d^2), facing the shading point), as a disk.
//   tube      the capsule's exact outline as seen from the shading point: the cylinder's two silhouette generators (at
//             m.a_perp = -r, m perpendicular to the axis) and the outer arcs of the end spheres' silhouette circles
//             between their tangent points, each arc in closed form on its cone (circular under the diffuse frames,
//             elliptic under the LTC). Seen end-on (the axis line within r of the point): the nearer end's sphere.
// An integral whose transform puts the light's bounding sphere entirely below the horizon is 0 without evaluation
// (shAreaIntegral), and so are rect and disk seen from behind.
#ifndef UNX_M_AREA_LIGHT_HLSLI
#define UNX_M_AREA_LIGHT_HLSLI
#include "Bindless.hlsli"
#include "Scene.hlsli"
#include "Passes/Shading/ShadingCommon.hlsli"

#define SH_LTC_SIZE 64u

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
// uxw = u x w computed without cancellation by the caller (a transform maps it by its cofactor matrix: T u x T w =
// cof(T) (u x w)): an ellipse the LTC squashes has nearly parallel u and w, whose difference would lose the short
// semi-axis and the plane's normal to rounding (fp32: 24 of 36 thin tube caps off by up to 3x before, 0 after
// [emulation against double]). The short semi-axis is then area / long one.
struct ShCone
{
    float3 w1, w2, w3;
    float x0, y0, e1, e2, mu;
    bool valid;
};

ShCone shLtcCone(float3 c, float3 u, float3 w, float3 uxw)
{
    ShCone k;
    const float d11 = dot(u, u), d22 = dot(w, w), d12 = dot(u, w);
    const float ts = 0.5 * atan2(2 * d12, d11 - d22);
    float sn, cs;
    sincos(ts, sn, cs);
    const float3 p1 = u * cs + w * sn;  // the long semi-axis (the larger eigenvalue of the Gram matrix)
    const float l1 = length(p1), area = length(uxw), l2 = area / max(l1, 1e-30);
    k.w1 = p1 / max(l1, 1e-30);
    k.w3 = uxw / max(area, 1e-30);
    if (dot(k.w3, c) < 0) k.w3 = -k.w3;
    k.w2 = cross(k.w3, k.w1);
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

// ---- exact boundary pieces on cones (design revision 1 12.4, M correction)
// A cone in its principal frame (v1, v2, v3 right-handed, L1 >= L2): its cross-section at unit distance along v3 is the
// centred ellipse (L1 cos psi, L2 sin psi), so a boundary point is x(psi) = v3 + L1 cos psi v1 + L2 sin psi v2 with
// |x|^2 = alpha + beta cos 2 psi and (x x x').z = L1 L2 v3.z - L2 v1.z cos psi - L1 v2.z sin psi. The edge integrand
// (x x x').z / |x|^2 then integrates in closed form: with P = 1 + L1^2, P2 = 1 + L2^2, Qd = L1^2 - L2^2 >= 0,
//   F(psi) = L1 L2 v3.z G / sqrt(P P2) - L2 v1.z Jc - L1 v2.z Js,
//   G  = atan(k tan psi) continued across the poles, k = sqrt(P2 / P):  psi - atan((1 - k) s c / (c^2 + k s^2)),
//   Jc = atanh(s sqrt(Qd / P)) / sqrt(P Qd)   (-> s / P for a circular cone),
//   Js = -atan(c sqrt(Qd / P2)) / sqrt(P2 Qd) (-> -c / P2),
// s = sin psi, c = cos psi. The horizon is z(psi) = A + B cos(psi - phi) (A = v3.z, B cos phi = L1 v1.z, B sin phi =
// L2 v2.z): the part above it is psi - phi in (-gamma, gamma), gamma = acos(-A / B).
struct ShArc
{
    float3 v1, v2, v3;
    float L1, L2;
    float g;           // L1 L2 v3.z / sqrt(P P2): the whole cone's I (signed) and G's weight
    float k;           // sqrt(P2 / P)
    float sc, ss;      // sqrt(Qd / P), sqrt(Qd / P2)
    float invP, invP2;
};

ShArc shArcMake(float3 v1, float3 v2, float3 v3, float L1, float L2)
{
    ShArc a;
    if (L1 < L2)
    {
        const float3 t = v1;
        v1 = v2;
        v2 = t;
        const float l = L1;
        L1 = L2;
        L2 = l;
    }
    if (dot(cross(v1, v2), v3) < 0) v2 = -v2;
    a.v1 = v1;
    a.v2 = v2;
    a.v3 = v3;
    a.L1 = L1;
    a.L2 = L2;
    a.invP = 1 / (1 + L1 * L1);
    a.invP2 = 1 / (1 + L2 * L2);
    const float Qd = (L1 - L2) * (L1 + L2);
    a.k = sqrt((1 + L2 * L2) * a.invP);
    a.g = v3.z * (L1 * sqrt(a.invP)) * (L2 * sqrt(a.invP2));
    a.sc = sqrt(Qd * a.invP);
    a.ss = sqrt(Qd * a.invP2);
    return a;
}

// The cone over the ellipse c + u cos t + w sin t (relative to the apex; its plane misses the apex; uxw = u x w without
// cancellation, shLtcCone): the elliptic cone's axis from shLtcCone, then the principal axes of its cross-section normal
// to the axis. The cross-section's form (Q restricted to the plane normal to the axis) has eigenvalues whose product is
// E / mu (det Q = -E^2, the axis' eigenvalue -mu E), so the smaller one is (E / mu) / larger, not trace - larger (which
// cancels for eccentric cross-sections); L = sqrt(mu E / lambda): mu sqrt(larger) and sqrt(mu E / larger). Degenerate
// ellipses give L = 0.
ShArc shConeArc(float3 c, float3 u, float3 w, float3 uxw)
{
    const ShCone k = shLtcCone(c, u, w, uxw);
    if (!k.valid) return shArcMake(float3(1, 0, 0), float3(0, 1, 0), float3(0, 0, 1), 0, 0);
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
    const float larger = max(0.5 * (q11 + q22 + length(float2(q11 - q22, 2 * q12))), 1e-30);
    const float longL = k.mu * sqrt(larger), shortL = sqrt(k.mu * E / larger);
    const float3x3 toT = transpose(float3x3(k.w1, k.w2, k.w3));  // (X, Y, Z) in (w1, w2, w3) -> the transform's space
    // The larger eigenvalue's direction is the short axis.
    const bool b1Short = dot(b1, mul(Q, b1)) >= dot(b2, mul(Q, b2));
    return shArcMake(mul(toT, b1Short ? b2 : b1), mul(toT, b1Short ? b1 : b2), mul(toT, a3), longL, shortL);
}

// The circular cone of half-angle beta about the unit axis a (sin beta, cos beta).
ShArc shCircleArc(float3 a, float sinB, float cosB)
{
    const float3 v1 = normalize(cross(a, abs(a.x) < 0.9 ? float3(1, 0, 0) : float3(0, 1, 0)));
    const float L = sinB / cosB;
    return shArcMake(v1, cross(a, v1), a, L, L);
}

// F(psi) of the header, up to a constant.
float shArcF(ShArc a, float psi)
{
    float s, c;
    sincos(psi, s, c);
    const float G = psi - atan((1 - a.k) * s * c / (c * c + a.k * s * s));
    // atanh(x) / x at x = |s| sqrt(Qd / P) < 1, with 1 - x^2 = (1 + L1^2 c^2 + L2^2 s^2) / P free of cancellation.
    const float x = abs(s) * a.sc, x2 = x * x;
    float rc;
    if (x2 < 0.01) rc = 1 + x2 * (1.0 / 3 + x2 * (1.0 / 5 + x2 * (1.0 / 7 + x2 / 9)));
    else rc = (log(1 + x) - 0.5 * log((1 + a.L1 * a.L1 * c * c + a.L2 * a.L2 * s * s) * a.invP)) / x;
    // atan(y) / y at y = |c| sqrt(Qd / P2).
    const float y = abs(c) * a.ss, y2 = y * y;
    const float rs = y2 < 0.01 ? 1 - y2 * (1.0 / 3 - y2 * (1.0 / 5 - y2 * (1.0 / 7 - y2 / 9))) : atan(y) / y;
    return a.g * G - a.L2 * a.v1.z * (s * a.invP * rc) + a.L1 * a.v2.z * (c * a.invP2 * rs);
}

// The boundary point at psi, dropped onto the horizon (for the chord; called where z(psi) = 0).
float3 shArcHorizonPoint(ShArc a, float psi)
{
    float s, c;
    sincos(psi, s, c);
    const float3 x = a.v3 + a.v1 * (a.L1 * c) + a.v2 * (a.L2 * s);
    return float3(x.xy, 0);
}

// psi of a direction x on the cone.
float shArcPsi(ShArc a, float3 x)
{
    return atan2(dot(x, a.v2) * a.L1, dot(x, a.v1) * a.L2);
}

// The horizon on the cone: z(psi) = A + B cos(psi - phi).
void shArcHorizon(ShArc a, out float A, out float B, out float phi)
{
    const float bc = a.L1 * a.v1.z, bs = a.L2 * a.v2.z;
    A = a.v3.z;
    B = length(float2(bc, bs));
    phi = atan2(bs, bc);
}

// The arc psi0 -> psi1 (either direction, |psi1 - psi0| <= 2 pi) clipped to z >= 0 and appended to acc (at most two
// pieces: the part above the horizon is one interval of psi modulo 2 pi).
void shLtcArc(inout ShLtcAcc acc, ShArc a, float psi0, float psi1)
{
    float A, B, phi;
    shArcHorizon(a, A, B, phi);
    if (A <= -B) return;
    const float lo = min(psi0, psi1), hi = max(psi0, psi1), dir = psi1 >= psi0 ? 1.0 : -1.0;
    float pa0 = lo, pb0 = hi, pa1 = 0, pb1 = -1;  // second piece empty
    if (A < B)
    {
        const float gamma = acos(clamp(-A / B, -1.0, 1.0));
        const float first = (phi - gamma) + 2 * SH_PI * floor((lo - (phi - gamma)) / (2 * SH_PI));  // interval start <= lo
        pa0 = max(lo, first);
        pb0 = min(hi, first + 2 * gamma);
        pa1 = max(lo, first + 2 * SH_PI);
        pb1 = min(hi, first + 2 * SH_PI + 2 * gamma);
    }
    [unroll] for (uint i = 0; i < 2; ++i)
    {
        const float pa = i == 0 ? pa0 : pa1, pb = i == 0 ? pb0 : pb1;
        if (!(pa < pb)) continue;
        acc.sum += dir * (shArcF(a, pb) - shArcF(a, pa));
        // An end inside the traversal is a horizon crossing: forwards the piece's start enters and its end leaves.
        if (pa > lo)
        {
            if (dir > 0) { acc.entryPoint = shArcHorizonPoint(a, pa); acc.crossings |= 2; }
            else { acc.exitPoint = shArcHorizonPoint(a, pa); acc.crossings |= 1; }
        }
        if (pb < hi)
        {
            if (dir > 0) { acc.exitPoint = shArcHorizonPoint(a, pb); acc.crossings |= 1; }
            else { acc.entryPoint = shArcHorizonPoint(a, pb); acc.crossings |= 2; }
        }
    }
}

// I of a full ellipse c + u cos t + w sin t (either side of the horizon; uxw: shLtcCone).
float shLtcEllipse(float3 c, float3 u, float3 w, float3 uxw)
{
    const float R = length(float2(u.z, w.z));
    if (c.z <= -R) return 0;
    const ShArc a = shConeArc(c, u, w, uxw);
    float A, B, phi;
    shArcHorizon(a, A, B, phi);
    if (A >= B) return abs(a.g);
    if (A <= -B) return 0;
    const float gamma = acos(clamp(-A / B, -1.0, 1.0));
    ShLtcAcc acc = shLtcBegin();
    acc.sum = shArcF(a, phi + gamma) - shArcF(a, phi - gamma);
    acc.exitPoint = shArcHorizonPoint(a, phi + gamma);
    acc.entryPoint = shArcHorizonPoint(a, phi - gamma);
    acc.crossings = 3;
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

// Form factor of a sphere (centre p relative to the shading point, radius r < |p|) to a differential area with unit
// normal nz (Snyder 1996; Lagarde and de Rousiers 2014, eq. 17): sin^2(alpha) cos(beta) above the horizon, 0 below,
// and between them (1 / pi h^2) (cos beta acos y - x sin beta sqrt(1 - y^2)) + (1 / pi) atan(sin beta sqrt(1 - y^2) / x)
// with h = |p| / r, x = sqrt(h^2 - 1), y = -x cot beta.
float shSphereFormFactor(float3 p, float r, float3 nz)
{
    const float d2 = dot(p, p), s2 = r * r / d2;  // sin^2 alpha = 1 / h^2
    const float cb = dot(nz, p) * rsqrt(d2), sa = sqrt(s2);
    if (cb >= sa) return cb * s2;
    if (cb <= -sa) return 0;
    const float x = sqrt(max(1 / s2 - 1, 0)), sb = sqrt(saturate(1 - cb * cb));
    const float y = clamp(-x * cb / sb, -1.0, 1.0), sy = sqrt(saturate(1 - y * y));
    return ((cb * acos(y) - x * sb * sy) * s2 + atan(sb * sy / x)) / SH_PI;
}

// V / pi of a disk (centre p relative to the shading point, emitting normal m with the point on its side, radius R),
// V = int_A w dw: I = nz . V / pi for a disk entirely above nz's horizon. With h the point's height over the disk's
// plane, rho its offset from the axis, k = h^2 + rho^2 - R^2 and Q = sqrt(k^2 + 4 R^2 h^2): the axial part is
// (1 - k / Q) / 2 (the coaxial-offset form factor, written without cancellation) and the radial part
// 2 h rho R^2 / (Q (Q + h^2 + rho^2 + R^2)).
float3 shDiskVector(float3 p, float3 m, float R)
{
    const float h = -dot(p, m);
    const float3 xp = -p - m * h;
    const float rho2 = dot(xp, xp), R2 = R * R, h2 = h * h;
    const float k = h2 + rho2 - R2, Q = sqrt(k * k + 4 * R2 * h2);
    const float axial = k >= 0 ? 2 * R2 * h2 / (Q * (Q + k)) : (Q - k) / (2 * Q);
    return -m * axial - xp * (2 * h * R2 / (Q * (Q + h2 + rho2 + R2)));
}

// The outer arc of an end sphere's silhouette circle (sphere at 'end', radius r) from its tangent point p0 to p1,
// bulging towards 'outward', appended to acc: a circular cone about T end under an orthonormal T, else the elliptic cone
// over the transformed circle (C = cof(T)).
void shTubeCap(inout ShLtcAcc acc, float3x3 T, float3x3 C, bool orthonormal, float3 end, float r, float3 p0, float3 p1, float3 outward)
{
    const float d2 = dot(end, end), k = r * r / d2;
    const float3 e = end * rsqrt(d2), cc = end * (1 - k);
    const float rc = r * sqrt(1 - k);
    ShArc a;
    if (orthonormal) a = shCircleArc(mul(T, e), r * rsqrt(d2), sqrt(1 - k));
    else
    {
        const float3 e1 = normalize(cross(e, abs(e.x) < 0.9 ? float3(1, 0, 0) : float3(0, 1, 0)));
        a = shConeArc(mul(T, cc), mul(T, e1 * rc), mul(T, cross(e, e1) * rc), mul(C, e) * (rc * rc));
    }
    const float3 mid = cc + normalize(outward - e * dot(outward, e)) * rc;
    const float s0 = shArcPsi(a, mul(T, p0)), s1 = shArcPsi(a, mul(T, p1)), sm = shArcPsi(a, mul(T, mid));
    const float span = s1 - s0 - 2 * SH_PI * floor((s1 - s0) / (2 * SH_PI));   // (s1 - s0) mod 2 pi
    const float toMid = sm - s0 - 2 * SH_PI * floor((sm - s0) / (2 * SH_PI));
    shLtcArc(acc, a, s0, toMid < span ? s0 + span : s0 + span - 2 * SH_PI);
}

// ---- lights: I of the light's region under the transform T (world -> transformed space), p = light position relative to
// the shading point; 'orthonormal': T is a rotation (the diffuse frames), else the LTC transform. Rect and disk: 0 on
// their back side; inside a sphere or tube: 0. A rect with barn doors: the part of it the point sees past them
// (Scene.hlsli lightBarnDoorRect). shAreaIntegral: the same times the light's scale of that part of the shading - the
// diffuse scale under the diffuse frames, the specular one under an LTC transform.
float shAreaIntegralUnscaled(GpuLight l, float3 p, float3x3 T, bool orthonormal)
{
    const uint type = lightType(l);
    // The region lies where (T x).z > 0: a light whose bounding sphere is entirely on the other side contributes 0.
    const float bound = type == LIGHT_RECT ? 0.5 * length(l.size) : (type == LIGHT_TUBE ? 0.5 * l.size.x + l.size.y : l.size.x);
    if (dot(T[2], p) <= -bound * length(T[2])) return 0;
    const float3 up = cross(l.forward, l.right);
    if ((type == LIGHT_RECT || type == LIGHT_DISK) && dot(-p, l.forward) <= 0) return 0;  // behind the emitting side
    // T's cofactor matrix (T a x T b = C (a x b); a rotation's is itself): planar curves' normals in the transformed space.
    const float3x3 C = orthonormal ? T : float3x3(cross(T[1], T[2]), cross(T[2], T[0]), cross(T[0], T[1]));
    if (type == LIGHT_RECT)
    {
        float3 centre = p;
        float2 halfSize = 0.5 * l.size;
        if (l.barnDoor != 0 && !lightBarnDoorRect(l, p, up, centre, halfSize)) return 0;
        const float3 ex = l.right * halfSize.x, ey = up * halfSize.y;
        ShLtcAcc acc = shLtcBegin();
        const float3 a = mul(T, centre - ex - ey), b = mul(T, centre + ex - ey), c = mul(T, centre + ex + ey), d = mul(T, centre - ex + ey);
        shLtcSegment(acc, a, b);
        shLtcSegment(acc, b, c);
        shLtcSegment(acc, c, d);
        shLtcSegment(acc, d, a);
        return shLtcFinish(acc);
    }
    if (type == LIGHT_DISK)
    {
        const float R = l.size.x;
        if (orthonormal)
        {
            const float mz = dot(T[2], l.forward), cz = dot(T[2], p), Rz = R * sqrt(saturate(1 - mz * mz));
            if (cz >= Rz) return dot(T[2], shDiskVector(p, l.forward, R));
        }
        return shLtcEllipse(mul(T, p), mul(T, l.right * R), mul(T, up * R), mul(C, l.forward) * (R * R));
    }
    const float r = type == LIGHT_TUBE ? l.size.y : l.size.x;
    const float3 axis = l.right * l.size.x;
    const float3 a = p - 0.5 * axis, b = p + 0.5 * axis;
    const float3 aPerp = a - l.right * dot(a, l.right);
    const float dPerp = length(aPerp);
    // A tube whose axis line passes within r of the shading point is seen end-on: its nearer end's sphere.
    if (type == LIGHT_SPHERE || dPerp <= r * 1.0001)
    {
        const float3 centre = type == LIGHT_SPHERE ? p : (dot(a, a) < dot(b, b) ? a : b);
        if (dot(centre, centre) <= r * r) return 0;  // inside the emitter
        if (orthonormal) return shSphereFormFactor(centre, r, T[2]);
        float3 ec, eu, ew;
        shSubtendedCircle(centre, r, ec, eu, ew);
        return shLtcEllipse(mul(T, ec), mul(T, eu), mul(T, ew), mul(C, normalize(centre)) * dot(eu, eu));
    }
    if (min(dot(a, a), dot(b, b)) <= r * r) return 0;
    // Silhouette generators a + r m +- s axis: m perpendicular to the axis with m . a = -r.
    const float3 ah = aPerp / dPerp, uh = normalize(cross(l.right, ah));
    const float along = sqrt(1 - r * r / (dPerp * dPerp));
    const float3 mp = ah * (-r / dPerp) + uh * along, mm = ah * (-r / dPerp) - uh * along;
    const float3 a0 = a + mp * r, b0 = b + mp * r, b1 = b + mm * r, a1 = a + mm * r;
    ShLtcAcc acc = shLtcBegin();
    shLtcSegment(acc, mul(T, a0), mul(T, b0));
    shTubeCap(acc, T, C, orthonormal, b, r, b0, b1, l.right);
    shLtcSegment(acc, mul(T, b1), mul(T, a1));
    shTubeCap(acc, T, C, orthonormal, a, r, a1, a0, -l.right);
    return shLtcFinish(acc);
}

float shAreaIntegral(GpuLight l, float3 p, float3x3 T, bool orthonormal)
{
    return shAreaIntegralUnscaled(l, p, T, orthonormal) * (orthonormal ? lightDiffuseScale(l) : lightSpecularScale(l));
}

// w(d) of the light's centre (INTERFACES 8.2; Scene.hlsli lightWindow: with the view's draw-distance fade), d = |p|.
float shAreaWindow(GpuLight l, float3 p) { return lightWindow(l, length(p)); }

// The colour of an area light's radiance toward a shading point (p: the light's centre relative to it): the light's
// colour, and for a rect that shows an image (scene::Light::sourceTexture; Unreal's rect light source texture) times the
// image around the point's foot on the emitter (held on the emitter), at the level Unreal's lookup takes:
// log2(distance to the emitter's plane / sqrt(its area)) + log2(the image's smaller side) - 2 - a texel four across
// the image one emitter size away, the image itself at the emitter. One lookup tints the diffuse and the specular
// integral alike (the reference makes one each: along the vector irradiance, along the lobe's mean direction).
float3 shAreaColor(GpuLight l, float3 p)
{
    if (l.sourceTexture != 0 && lightType(l) == LIGHT_RECT)
    {
        Texture2D<float4> image = ResourceDescriptorHeap[l.sourceTexture - 1];
        const float3 up = cross(l.forward, l.right);
        const float3 s = float3(dot(-p, l.right), dot(-p, up), dot(-p, l.forward));  // the point in the light's frame
        const float2 uv = float2(clamp(s.x / max(l.size.x, 1e-6), -0.5, 0.5), -clamp(s.y / max(l.size.y, 1e-6), -0.5, 0.5)) + 0.5;
        float width, height, levels;
        image.GetDimensions(0, width, height, levels);
        const float level = clamp(log2(max(s.z, 1e-6) * rsqrt(max(l.size.x * l.size.y, 1e-12))) + log2(min(width, height)) - 2, 0, levels - 1);
        return l.color * image.SampleLevel(g_linearClamp, uv, level).rgb;
    }
    return l.color;
}

#endif
