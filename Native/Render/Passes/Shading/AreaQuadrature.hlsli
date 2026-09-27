// Area lights for lobes without a closed form (render A, 2026-09-27): the sheen lobe (MATERIAL_LAYERS 1.4) and the
// anisotropic GGX lobe (1.5). HLSL mirror of Tests/AreaQuad.cpp (the double-precision replica and its study; the header
// there states the method and the measured accuracy):
//   region  the light's outline (AreaLight.hlsli's outlines) in the local frame F (rows: world -> local, z = n), clipped
//           to z >= 0 and closed by the horizon chord; convex;
//   fan     outlines of straight edges from their first vertex, outlines with arcs from an interior direction (arcs
//           halved), a whole ellipse above the horizon from its centre (periodic);
//   pieces  straight: Arvo's map of the spherical triangle (B, apex, C); curved: polar about the apex, the exit along each
//           azimuth closed form;
//   sheen   Gauss-Legendre 6 x 6 per piece (composite in azimuth on arcs, groups of <= pi/4; periodic: 48-point trapezoid);
//   aniso   f_s cos d omega = W(l) P22(m) d^2m, W = F G2 (v.h)/(n.v h.n) x compensation; cells of the piece's (x, y) square
//           (K = 6, up to 12 each way for long pieces), each cell's unit-P22 mass exact for its straight-edged image in
//           u = m / alpha (Green's theorem, an atan per edge) plus the outer edge's parabolic sagitta, times W at the centre.
// Bounds (INTERFACES 3.6): <= 6 outline elements, <= 10 pieces, <= 12 x 12 cells per piece: every loop has a fixed cap.
#ifndef UNX_M_AREA_QUADRATURE_HLSLI
#define UNX_M_AREA_QUADRATURE_HLSLI
#include "Passes/Shading/AreaLight.hlsli"
#include "Passes/Common/MaterialModel.hlsli"

#define AQ_PI 3.14159265358979
#define AQ_MAX_ELEMS 8u
#define AQ_MAX_CELLS 12u

struct AqElem
{
    uint kind;            // 0 straight (segment a -> b), 1 arc c + u cos t + w sin t, t from t0 to t1
    float3 a, b, c, u, w;
    float t0, t1;
};
float3 aqArc(AqElem e, float t) { return e.c + e.u * cos(t) + e.w * sin(t); }
AqElem aqLine(float3 a, float3 b)
{
    AqElem e = (AqElem)0;
    e.a = a;
    e.b = b;
    return e;
}
AqElem aqEllipse(float3 c, float3 u, float3 w, float t0, float t1)
{
    AqElem e;
    e.kind = 1;
    e.c = c, e.u = u, e.w = w, e.t0 = t0, e.t1 = t1;
    e.a = aqArc(e, t0);
    e.b = aqArc(e, t1);
    return e;
}
float aqArcParam(float3 c, float3 u, float3 w, float3 q)
{
    const float3 d = q - c;
    return atan2(dot(d, w) / dot(w, w), dot(d, u) / dot(u, u));
}

struct AqRegion
{
    AqElem e[AQ_MAX_ELEMS];
    uint count;
};

// The outline in the local frame; false: nothing (behind a one-sided light, inside an emitter).
bool aqOutline(GpuLight l, float3 pw, float3x3 F, out AqRegion o)
{
    o = (AqRegion)0;
    const uint type = lightType(l);
    const float3 upw = cross(l.forward, l.right);
    const float3 p = mul(F, pw), fwd = mul(F, l.forward), rgt = mul(F, l.right), up = mul(F, upw);
    if (type == LIGHT_RECT || type == LIGHT_DISK)
    {
        if (dot(-p, fwd) <= 0) return false;
        if (type == LIGHT_RECT)
        {
            const float3 ex = rgt * (0.5 * l.size.x), ey = up * (0.5 * l.size.y);
            const float3 q0 = p - ex - ey, q1 = p + ex - ey, q2 = p + ex + ey, q3 = p - ex + ey;
            o.e[0] = aqLine(q0, q1), o.e[1] = aqLine(q1, q2), o.e[2] = aqLine(q2, q3), o.e[3] = aqLine(q3, q0);
            o.count = 4;
            return true;
        }
        o.e[0] = aqEllipse(p, rgt * l.size.x, up * l.size.x, 0, 2 * AQ_PI);
        o.count = 1;
        return true;
    }
    const float r = type == LIGHT_TUBE ? l.size.y : l.size.x;
    const float3 axis = rgt * l.size.x;
    const float3 a = p - 0.5 * axis, b = p + 0.5 * axis;
    const float3 aPerp = a - rgt * dot(a, rgt);
    const float dPerp = length(aPerp);
    if (type == LIGHT_SPHERE || dPerp <= r * 1.0001)
    {
        const float3 centre = type == LIGHT_SPHERE ? p : (dot(a, a) < dot(b, b) ? a : b);
        if (dot(centre, centre) <= r * r) return false;
        float3 c, u, w;
        shSubtendedCircle(centre, r, c, u, w);
        o.e[0] = aqEllipse(c, u, w, 0, 2 * AQ_PI);
        o.count = 1;
        return true;
    }
    if (min(dot(a, a), dot(b, b)) <= r * r) return false;
    const float3 ah = aPerp / dPerp, uh = normalize(cross(rgt, ah));
    const float along = sqrt(1 - r * r / (dPerp * dPerp));
    const float3 mp = ah * (-r / dPerp) + uh * along, mm = ah * (-r / dPerp) - uh * along;
    const float3 a0 = a + mp * r, b0 = b + mp * r, b1 = b + mm * r, a1 = a + mm * r;
    o.e[0] = aqLine(a0, b0);
    o.e[2] = aqLine(b1, a1);
    [unroll] for (uint k = 0; k < 2; ++k)
    {
        const float3 end = k == 0 ? b : a, p0 = k == 0 ? b0 : a1, p1 = k == 0 ? b1 : a0, outward = k == 0 ? rgt : -rgt;
        float3 c, u, w;
        shSubtendedCircle(end, r, c, u, w);
        const float3 ed = normalize(end);
        const float3 mid = c + normalize(outward - ed * dot(outward, ed)) * length(u);
        const float s0 = aqArcParam(c, u, w, p0), s1 = aqArcParam(c, u, w, p1), sm = aqArcParam(c, u, w, mid);
        const float span = s1 - s0 - 2 * AQ_PI * floor((s1 - s0) / (2 * AQ_PI));
        const float toMid = sm - s0 - 2 * AQ_PI * floor((sm - s0) / (2 * AQ_PI));
        AqElem e = aqEllipse(c, u, w, s0, toMid < span ? s0 + span : s0 + span - 2 * AQ_PI);
        e.a = p0, e.b = p1;
        o.e[k == 0 ? 1 : 3] = e;
    }
    o.count = 4;
    return true;
}

// Clip to z >= 0, close with the horizon chord (exit -> entry, first when clipped). False: nothing above the horizon.
bool aqClip(AqRegion inR, out AqRegion o)
{
    AqRegion pieces = (AqRegion)0;
    float3 exitP = 0, entryP = 0;
    bool haveExit = false, haveEntry = false;
    uint exitAfter = 0;
    [loop] for (uint i = 0; i < inR.count; ++i)
    {
        const AqElem e = inR.e[i];
        if (e.kind == 0)
        {
            if (e.a.z < 0 && e.b.z < 0) continue;
            AqElem s = e;
            if (e.a.z < 0)
            {
                s.a = e.a + (e.b - e.a) * (e.a.z / (e.a.z - e.b.z));
                s.a.z = 0;
                entryP = s.a, haveEntry = true;
            }
            else if (e.b.z < 0)
            {
                s.b = e.a + (e.b - e.a) * (e.a.z / (e.a.z - e.b.z));
                s.b.z = 0;
                exitP = s.b, haveExit = true;
            }
            if (pieces.count < AQ_MAX_ELEMS) pieces.e[pieces.count++] = s;
            if (e.b.z < 0) exitAfter = pieces.count - 1;
            continue;
        }
        // arc: the roots of c.z + u.z cos t + w.z sin t = 0 inside the parameter interval (at most two)
        const float A = e.u.z, B = e.w.z, R = sqrt(A * A + B * B), lo = min(e.t0, e.t1), hi = max(e.t0, e.t1);
        float cut[4];
        uint nc = 0;
        cut[nc++] = e.t0;
        if (R > abs(e.c.z))
        {
            const float ph = atan2(B, A), g = acos(-e.c.z / R);
            [unroll] for (uint k = 0; k < 2; ++k)
            {
                const float root = k == 0 ? ph + g : ph - g;
                float x = root - 2 * AQ_PI * floor((root - lo) / (2 * AQ_PI));
                if (x > lo && x < hi && nc < 3) cut[nc++] = x;
            }
        }
        // (the interval spans <= 2 pi: each root has at most one copy inside)
        if (nc == 3 && ((e.t1 >= e.t0) != (cut[1] < cut[2])))
        {
            const float t = cut[1];
            cut[1] = cut[2];
            cut[2] = t;
        }
        cut[nc++] = e.t1;
        [loop] for (uint k = 0; k + 1 < nc; ++k)
        {
            AqElem s = e;
            s.t0 = cut[k], s.t1 = cut[k + 1];
            if (aqArc(s, 0.5 * (s.t0 + s.t1)).z < 0) continue;
            s.a = k == 0 ? e.a : aqArc(s, s.t0);
            s.b = k + 2 == nc ? e.b : aqArc(s, s.t1);
            if (k > 0)
            {
                s.a.z = 0;
                entryP = s.a, haveEntry = true;
            }
            bool exits = false;
            if (k + 2 < nc)
            {
                s.b.z = 0;
                exitP = s.b, haveExit = true, exits = true;
            }
            if (pieces.count < AQ_MAX_ELEMS) pieces.e[pieces.count++] = s;
            if (exits) exitAfter = pieces.count - 1;
        }
    }
    o = (AqRegion)0;
    if (pieces.count == 0) return false;
    if (!haveExit || !haveEntry)
    {
        o = pieces;
        return true;
    }
    o.e[0] = aqLine(exitP, entryP);
    o.count = 1;
    [loop] for (uint k = 1; k <= pieces.count; ++k)
        if (o.count < AQ_MAX_ELEMS) o.e[o.count++] = pieces.e[(exitAfter + k) % pieces.count];
    return true;
}

// ---- the fan
struct AqFan
{
    float3 A, t1, t2;
};
AqFan aqFanAt(float3 A)
{
    AqFan f;
    f.A = A;
    const float3 r = abs(A.z) < 0.9 ? float3(0, 0, 1) : float3(1, 0, 0);
    f.t1 = normalize(r - A * dot(r, A));
    f.t2 = cross(A, f.t1);
    return f;
}
float aqAzimuth(AqFan f, float3 q) { return atan2(dot(q, f.t2), dot(q, f.t1)); }
float aqWrap(float x) { return x - 2 * AQ_PI * floor((x + AQ_PI) / (2 * AQ_PI)); }
float aqRadial(AqFan f, AqElem e, float3 dir)
{
    if (e.kind == 0)
    {
        const float3 g = cross(e.a, e.b);
        const float a = dot(f.A, g), b = dot(dir, g), sa = a < 0 ? -1.0 : 1.0;
        return atan2(a * sa, -b * sa);
    }
    const float3 k = cross(f.A, dir);
    const float p = dot(e.u, k), q = dot(e.w, k), c0 = dot(e.c, k), R = sqrt(p * p + q * q);
    const float ph = atan2(q, p), g = acos(clamp(-c0 / max(R, 1e-30), -1.0, 1.0));
    const float3 P0 = normalize(aqArc(e, ph + g)), P1 = normalize(aqArc(e, ph - g));
    return max(atan2(dot(P0, dir), dot(P0, f.A)), atan2(dot(P1, dir), dot(P1, f.A)));
}
float3 aqArcTangentAtApex(AqFan f, AqElem e, bool atStart)
{
    const float t = atStart ? e.t0 : e.t1, sgn = (e.t1 >= e.t0 ? 1.0 : -1.0) * (atStart ? 1.0 : -1.0);
    float3 d = (-e.u * sin(t) + e.w * cos(t)) * sgn;
    return normalize(d - f.A * dot(d, f.A));
}

// Pieces: the fan's apex frame and per piece its element (halved arcs), azimuth range and periodic flag.
struct AqPieces
{
    AqFan fan;
    AqElem e[10];
    float p0[10], p1[10];
    uint count;
    bool periodic;
};
AqPieces aqPieces(AqRegion r)
{
    AqPieces o = (AqPieces)0;
    const bool whole = r.count == 1 && r.e[0].kind == 1 && abs(abs(r.e[0].t1 - r.e[0].t0) - 2 * AQ_PI) < 1e-4;
    if (whole)
    {
        o.fan = aqFanAt(normalize(r.e[0].c));
        // (the period starts at the ellipse's parameter origin: a split defined by the light, not by the frame's axes)
        const float s0 = aqAzimuth(o.fan, normalize(aqArc(r.e[0], r.e[0].t0)));
        o.e[0] = r.e[0], o.p0[0] = s0, o.p1[0] = s0 + 2 * AQ_PI, o.count = 1, o.periodic = true;
        return o;
    }
    bool curved = false;
    [loop] for (uint i = 0; i < r.count; ++i) curved = curved || r.e[i].kind == 1;
    if (curved)
    {
        float3 c = 0;
        [loop] for (uint i = 0; i < r.count; ++i)
        {
            c += normalize(r.e[i].a) + normalize(r.e[i].b);
            if (r.e[i].kind == 1) c += 2 * normalize(aqArc(r.e[i], 0.5 * (r.e[i].t0 + r.e[i].t1)));
        }
        o.fan = aqFanAt(normalize(c));
        [loop] for (uint i = 0; i < r.count; ++i)
        {
            const AqElem e = r.e[i];
            const uint halves = e.kind == 1 ? 2 : 1;
            [loop] for (uint h = 0; h < halves; ++h)
            {
                AqElem q = e;
                if (e.kind == 1)
                {
                    q.t0 = e.t0 + (e.t1 - e.t0) * 0.5 * h, q.t1 = e.t0 + (e.t1 - e.t0) * 0.5 * (h + 1);
                    q.a = h == 0 ? e.a : aqArc(q, q.t0);
                    q.b = h == 1 ? e.b : aqArc(q, q.t1);
                }
                const float a0 = aqAzimuth(o.fan, normalize(q.a)), d = aqWrap(aqAzimuth(o.fan, normalize(q.b)) - a0);
                if (abs(d) < 1e-7 || o.count >= 10) continue;
                o.e[o.count] = q, o.p0[o.count] = a0, o.p1[o.count] = a0 + d;
                ++o.count;
            }
        }
        return o;
    }
    o.fan = aqFanAt(normalize(r.e[0].a));
    [loop] for (uint i = 1; i + 1 < r.count; ++i)  // (straight outline: the elements at the apex span nothing)
    {
        const float a0 = aqAzimuth(o.fan, normalize(r.e[i].a)), d = aqWrap(aqAzimuth(o.fan, normalize(r.e[i].b)) - a0);
        if (abs(d) < 1e-7 || o.count >= 10) continue;
        o.e[o.count] = r.e[i], o.p0[o.count] = a0, o.p1[o.count] = a0 + d;
        ++o.count;
    }
    return o;
}

// Arvo's map of the spherical triangle (A, B, C), degenerate at B, u2 = 1 on the edge AC.
float aqAngle(float3 at, float3 b, float3 c)
{
    const float3 n1 = cross(at, b), n2 = cross(at, c);
    const float l1 = length(n1), l2 = length(n2);
    return l1 > 1e-20 && l2 > 1e-20 ? acos(clamp(dot(n1, n2) / (l1 * l2), -1.0, 1.0)) : 0;
}
float3 aqArvo(float3 A, float3 B, float3 C, float alpha, float area, float cosC, float u1, float u2)
{
    const float ah = u1 * area, sn = sin(ah - alpha), cs = cos(ah - alpha);
    const float u = cs - cos(alpha), v = sn + sin(alpha) * cosC;
    const float q = clamp(((v * cs - u * sn) * cos(alpha) - v) / ((v * sn + u * cs) * sin(alpha)), -1.0, 1.0);
    const float3 perpC = normalize(C - A * dot(C, A));
    const float3 Ch = A * q + perpC * sqrt(max(0.0, 1 - q * q));
    const float z = 1 - u2 * (1 - dot(Ch, B));
    const float3 d = Ch - B * dot(Ch, B);
    const float dl = length(d);
    return dl > 1e-12 ? normalize(B * z + d * (sqrt(max(0.0, 1 - z * z)) / dl)) : B;
}

static const float kAqX[6] = { -0.93246951, -0.66120939, -0.23861919, 0.23861919, 0.66120939, 0.93246951 };
static const float kAqW[6] = { 0.17132449, 0.36076157, 0.46791393, 0.46791393, 0.36076157, 0.17132449 };

// A piece's point: straight pieces by Arvo (B, apex, C) at (x, y); curved by the polar fan (x: azimuth fraction, y: area-
// preserving radius fraction).
struct AqPiece
{
    AqFan fan;
    AqElem e;
    float p0, p1;
    float3 B, C;
    float alpha, area, cosC;
};
AqPiece aqPiece(AqPieces ps, uint i)
{
    AqPiece p;
    p.fan = ps.fan, p.e = ps.e[i], p.p0 = ps.p0[i], p.p1 = ps.p1[i];
    p.B = normalize(p.e.a), p.C = normalize(p.e.b);
    p.alpha = aqAngle(p.B, p.fan.A, p.C);
    p.area = p.alpha + aqAngle(p.fan.A, p.C, p.B) + aqAngle(p.C, p.B, p.fan.A) - AQ_PI;
    p.cosC = dot(p.B, p.fan.A);
    return p;
}
float3 aqPoint(AqPiece p, float x, float y)
{
    if (p.e.kind == 0) return aqArvo(p.B, p.fan.A, p.C, p.alpha, p.area, p.cosC, x, y);
    const float psi = p.p0 + (p.p1 - p.p0) * x;
    const float3 dir = p.fan.t1 * cos(psi) + p.fan.t2 * sin(psi);
    const float R = aqRadial(p.fan, p.e, dir), ct = 1 - y * (1 - cos(R)), st = sqrt(max(0.0, 1 - ct * ct));
    return p.fan.A * ct + dir * st;
}

// ---- sheen: int over the light of the sheen lobe x cos (C = 1), local frame (n = z), unit v
float aqSheen(AqRegion r, float3 v, float roughness)
{
    const AqPieces ps = aqPieces(r);
    float total = 0;
    [loop] for (uint i = 0; i < ps.count; ++i)
    {
        const AqPiece p = aqPiece(ps, i);
        float s = 0;
        if (p.e.kind == 0)
        {
            if (!(p.area > 1e-9)) continue;
            [loop] for (uint j = 0; j < 36; ++j)
            {
                // Arvo on the triangle (apex, B, C): degenerate at B there; the sheen piece uses the apex-first order
                const float3 l = aqArvo(p.fan.A, p.B, p.C, aqAngle(p.fan.A, p.B, p.C), p.area, dot(p.fan.A, p.B), 0.5 * (kAqX[j % 6] + 1), 0.5 * (kAqX[j / 6] + 1));
                s += 0.25 * kAqW[j % 6] * kAqW[j / 6] * modelSheenLobe(roughness, float3(0, 0, 1), v, l) * max(l.z, 0.0);
            }
            total += s * p.area * (p.p1 >= p.p0 ? 1.0 : -1.0);
            continue;
        }
        const bool periodic = ps.periodic;
        const uint groups = periodic ? 1u : clamp((uint)ceil(abs(p.p1 - p.p0) / 0.7854), 1u, 4u);
        const uint np = periodic ? 48u : 6u * groups;
        [loop] for (uint j = 0; j < np; ++j)
        {
            const uint gi = periodic ? 0 : j / 6, gj = periodic ? j : j % 6;
            const float g0 = p.p0 + (p.p1 - p.p0) * gi / groups, g1 = p.p0 + (p.p1 - p.p0) * (gi + 1) / groups;
            const float psi = periodic ? p.p0 + (j + 0.5) * (p.p1 - p.p0) / np : g0 + (g1 - g0) * 0.5 * (kAqX[gj] + 1);
            const float wp = periodic ? (p.p1 - p.p0) / np : 0.5 * (g1 - g0) * kAqW[gj];
            const float3 dir = p.fan.t1 * cos(psi) + p.fan.t2 * sin(psi);
            const float R = aqRadial(p.fan, p.e, dir), q = 1 - cos(R);
            float inner = 0;
            [loop] for (uint k = 0; k < 6; ++k)
            {
                const float ct = 1 - 0.5 * (kAqX[k] + 1) * q, st = sqrt(max(0.0, 1 - ct * ct));
                const float3 l = p.fan.A * ct + dir * st;
                inner += 0.5 * kAqW[k] * modelSheenLobe(roughness, float3(0, 0, 1), v, l) * max(l.z, 0.0);
            }
            s += wp * q * inner;
        }
        total += s;
    }
    return abs(total);
}

// ---- anisotropic GGX: int over the light of f_s cos (Fresnel, compensation), frame (t, b, n) = local axes
float aqLambda(float3 w, float2 alpha)
{
    const float x = w.x * alpha.x, y = w.y * alpha.y;
    return 0.5 * (-1 + sqrt(1 + (x * x + y * y) / (w.z * w.z)));
}
float3 aqAnisoWeight(float3 v, float3 l, float2 alpha, float3 f0, float3 comp)
{
    if (l.z <= 0) return 0;
    const float3 h = normalize(v + l);
    const float vh = dot(v, h);
    const float3 F = f0 + (1 - f0) * pow(1 - saturate(vh), 5);
    const float G2 = 1 / (1 + aqLambda(v, alpha) + aqLambda(l, alpha));
    return F * comp * (G2 * vh / (v.z * h.z));
}
float2 aqU(float3 v, float3 l, float2 alpha)
{
    const float3 h = normalize(v + l);
    return -h.xy / (h.z * alpha);
}
float aqEdgeMass(float2 a, float2 b)  // (a x d) / (2 pi) int_0^1 dt / (1 + |a + t d|^2)
{
    const float2 d = b - a;
    const float cr = a.x * d.y - a.y * d.x, q2 = dot(d, d), q1 = 2 * dot(a, d);
    if (q2 < 1e-24) return 0;
    const float D = sqrt(4 * (q2 + cr * cr));
    // atan(x1) - atan(x0) as one atan2 (x1 - x0 = 2 q2 / D exactly; both in (-pi/2, pi/2), so the difference's branch is
    // atan2's): the two atans of a narrow lobe's large u cancel in float otherwise
    const float x0 = q1 / D;
    return cr / (AQ_PI * D) * atan2(2 * q2 / D, 1 + x0 * (x0 + 2 * q2 / D));
}
float3 aqAniso(AqRegion r, float3 v, float2 alpha, float3 f0, float3 comp)
{
    const AqPieces ps = aqPieces(r);
    float3 total = 0;
    // (a whole ellipse: its two halves of azimuth, 12 cells each - the replica's 24 over 2 pi)
    const uint n = ps.periodic ? 2 : ps.count;
    [loop] for (uint i = 0; i < n; ++i)
    {
        AqPiece p = aqPiece(ps, ps.periodic ? 0 : i);
        if (ps.periodic) p.p0 = ps.p0[0] + AQ_PI * i, p.p1 = ps.p0[0] + AQ_PI * (i + 1);
        if (p.e.kind == 0 && !(p.area > 1e-9)) continue;
        // cells: 6 each way, up to 12 for long pieces (24 per 2 pi of the piece's angular extent)
        const float across = p.e.kind == 0 ? acos(clamp(dot(p.B, p.C), -1.0, 1.0)) : abs(p.p1 - p.p0);
        // (a whole ellipse: the replica's rule on the full period, its middle azimuth p0 + pi, for both halves)
        AqPiece whole = p;
        if (ps.periodic) whole.p0 = ps.p0[0], whole.p1 = ps.p0[0] + 2 * AQ_PI;
        const float along = p.e.kind == 0 ? max(acos(clamp(dot(p.fan.A, p.B), -1.0, 1.0)), acos(clamp(dot(p.fan.A, p.C), -1.0, 1.0)))
                                          : acos(clamp(dot(p.fan.A, aqPoint(whole, 0.5, 1)), -1.0, 1.0));
        const uint KP = clamp((uint)ceil(across / (2 * AQ_PI) * 24), 6u, AQ_MAX_CELLS), KS = clamp((uint)ceil(along / (2 * AQ_PI) * 24), 6u, AQ_MAX_CELLS);
        float2 prev[AQ_MAX_CELLS + 1], cur[AQ_MAX_CELLS + 1];
        [loop] for (uint k = 0; k <= KS; ++k) prev[k] = aqU(v, aqPoint(p, 0, (float)k / KS), alpha);
        float3 s = 0;
        [loop] for (uint j = 1; j <= KP; ++j)
        {
            [loop] for (uint k2 = 0; k2 <= KS; ++k2) cur[k2] = aqU(v, aqPoint(p, (float)j / KP, (float)k2 / KS), alpha);
            [loop] for (uint k = 0; k < KS; ++k)
            {
                const float2 u00 = prev[k], u10 = cur[k], u11 = cur[k + 1], u01 = prev[k + 1];
                const float mass = aqEdgeMass(u00, u10) + aqEdgeMass(u10, u11) + aqEdgeMass(u11, u01) + aqEdgeMass(u01, u00);
                float m2 = mass;
                if (k == KS - 1)
                {
                    // the outer edge on the region's boundary: the parabolic segment P (2/3) |chord| sagitta
                    const float2 um = aqU(v, aqPoint(p, (j - 0.5) / KP, 1), alpha);
                    const float2 c = u11 - u01;
                    const float cl = length(c);
                    if (cl > 1e-12)
                    {
                        const float sag = dot(um - 0.5 * (u01 + u11), float2(c.y, -c.x)) / cl;
                        const float inner = dot(0.5 * (u00 + u10) - 0.5 * (u01 + u11), float2(c.y, -c.x)) / cl;
                        const float r2 = dot(um, um), P = 1 / (AQ_PI * (1 + r2) * (1 + r2));
                        m2 += (mass < 0 ? -1.0 : 1.0) * P * (2.0 / 3.0) * cl * abs(sag) * ((sag > 0) == (inner > 0) ? -1.0 : 1.0);
                    }
                }
                s += m2 * aqAnisoWeight(v, aqPoint(p, (j - 0.5) / KP, (k + 0.5) / KS), alpha, f0, comp);
            }
            [loop] for (uint k3 = 0; k3 <= KS; ++k3) prev[k3] = cur[k3];
        }
        total += s;
    }
    return abs(total);
}

// Entry points: the light's integral for a world frame F (rows: world -> local) and p = light centre - shading point.
float shAreaSheen(GpuLight l, float3 p, float3x3 F, float3 v, float roughness)
{
    AqRegion o, r;
    if (!aqOutline(l, p, F, o) || !aqClip(o, r)) return 0;
    return aqSheen(r, mul(F, v), roughness);
}
float3 shAreaAniso(GpuLight l, float3 p, float3 t, float3 b, float3 n, float3 v, float2 alpha, float3 f0, float3 comp)
{
    const float3x3 F = float3x3(t, b, n);
    AqRegion o, r;
    if (!aqOutline(l, p, F, o) || !aqClip(o, r)) return 0;
    return aqAniso(r, mul(F, v), alpha, f0, comp);
}

#endif
