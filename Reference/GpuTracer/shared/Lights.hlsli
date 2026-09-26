// Local lights of INTERFACES_KO.md 8.2 for next-event estimation and MIS, the shared form of
// Reference/PathTracer/src/Lights.{h,cpp}: range window, importance, candidate choice over the uniform cell grid (the
// same running float sums, so the same choice and probability), sampling and emitter intersection per light type.
// The includer defines rtLightFetch(i), rtLightCellStart(c) (cells + 1 entries) and rtLightCellLight(k), and the grid
// is described by RtLightGrid. The CPU's per-cell candidate vectors become two passes over the cell list (total, then
// choice), so no per-thread storage is needed.
#ifndef UNX_RT_LIGHTS_HLSLI
#define UNX_RT_LIGHTS_HLSLI
#include "Compat.hlsli"

RT_BEGIN_NAMESPACE
RT_CONST uint kRtLightPoint = 0;
RT_CONST uint kRtLightSpot = 1;
RT_CONST uint kRtLightRect = 2;
RT_CONST uint kRtLightDisk = 3;
RT_CONST uint kRtLightSphere = 4;
RT_CONST uint kRtLightTube = 5;

// scene::Light after LightSet's normalisation (forward unit, right orthogonalised, up = forward x right).
struct RtLight
{
    float3 position;
    uint type;
    float3 forward;
    float intensity;
    float3 right;
    float range;
    float3 up;
    float spotScale;
    float3 color;
    float spotOffset;
    float2 size;
    uint castShadow;
    uint pad;
};

struct RtLightGrid
{
    float3 minCorner;
    uint count;       // lights in the scene (0: no local lights)
    float3 cell;
    uint dimX;
    uint dimY;
    uint dimZ;
    uint pad0;
    uint pad1;
};

RtLight rtLightFetch(uint i);          // defined by the includer
uint rtLightCellStart(uint cell);      // defined by the includer
uint rtLightCellLight(uint k);         // defined by the includer

struct RtLightSample
{
    float3 wi;
    float distance;
    float3 L;
    float pdf;
    bool delta;
};

RT_INLINE float rtLightWindow(RtLight l, float3 x)
{
    const float3 d = x - l.position;
    const float r = sqrt(dot(d, d)) / l.range;
    const float r4 = r * r * r * r;
    const float w = saturate(1 - r4);
    return w * w;
}

RT_INLINE float rtLightImportance(RtLight l, float3 x)
{
    const float3 d = x - l.position;
    const float d2 = dot(d, d);
    if (d2 >= l.range * l.range) return 0;
    const float w = rtLightWindow(l, x);
    if (w <= 0) return 0;
    const float lum = max(0.2126f * l.color.x + 0.7152f * l.color.y + 0.0722f * l.color.z, 1e-6f);
    if (l.type == kRtLightPoint) return l.intensity * lum * w / max(d2, 1e-4f);
    if (l.type == kRtLightSpot)
    {
        const float c = dot(d, l.forward) / sqrt(max(d2, 1e-12f));
        const float sp = saturate(c * l.spotScale + l.spotOffset);
        return l.intensity * lum * w * sp * sp / max(d2, 1e-4f);
    }
    if (l.type == kRtLightRect || l.type == kRtLightDisk)
    {
        if (dot(d, l.forward) <= 0) return 0;
        const float area = l.type == kRtLightRect ? l.size.x * l.size.y : kRtPi * l.size.x * l.size.x;
        return l.intensity * lum * area * w / max(d2, area);
    }
    if (l.type == kRtLightSphere)
    {
        const float a = kRtPi * l.size.x * l.size.x;
        return l.intensity * lum * a * w / max(d2, a);
    }
    const float at = 2 * l.size.y * l.size.x + kRtPi * l.size.y * l.size.y;  // tube
    return l.intensity * lum * at * w / max(d2, at);
}

// Cell of x, or ~0u outside the grid.
RT_INLINE uint rtLightCell(RtLightGrid g, float3 x)
{
    if (g.count == 0) return ~0u;
    const float3 r = x - g.minCorner;
    const int cx = (int)floor(r.x / g.cell.x), cy = (int)floor(r.y / g.cell.y), cz = (int)floor(r.z / g.cell.z);
    if (cx < 0 || cy < 0 || cz < 0 || cx >= (int)g.dimX || cy >= (int)g.dimY || cz >= (int)g.dimZ) return ~0u;
    return ((uint)cz * g.dimY + (uint)cy) * g.dimX + (uint)cx;
}

// Sum of positive importances at x over the cell's list (the CPU's LightCandidates::total, same order of additions).
RT_INLINE float rtLightTotal(uint cell, float3 x)
{
    if (cell == ~0u) return 0;
    float total = 0;
    const uint k1 = rtLightCellStart(cell + 1);
    for (uint k = rtLightCellStart(cell); k < k1; ++k)
    {
        const float imp = rtLightImportance(rtLightFetch(rtLightCellLight(k)), x);
        if (imp > 0) total += imp;
    }
    return total;
}

// LightSet::choose: the first candidate whose running sum exceeds u * total (the last one if none does); returns the
// scene light index and its probability (difference of running sums / total).
RT_INLINE uint rtLightChoose(uint cell, float3 x, float total, float u, RT_OUT(float) probability)
{
    const float target = u * total;
    float cum = 0, prevCum = 0;
    uint chosen = ~0u;
    float chosenCum = 0, chosenPrev = 0;
    const uint k1 = rtLightCellStart(cell + 1);
    for (uint k = rtLightCellStart(cell); k < k1; ++k)
    {
        const uint li = rtLightCellLight(k);
        const float imp = rtLightImportance(rtLightFetch(li), x);
        if (imp <= 0) continue;
        prevCum = cum;
        cum += imp;
        chosen = li;
        chosenCum = cum;
        chosenPrev = prevCum;
        if (cum > target) break;
    }
    probability = chosen == ~0u ? 0.0f : (chosenCum - chosenPrev) / total;
    return chosen;
}

RT_INLINE bool rtLightSample(RtLight l, float3 x, float u1, float u2, RT_OUT(RtLightSample) s)
{
    s.wi = float3(0, 0, 1);
    s.distance = 0;
    s.L = rtSplat3(0);
    s.pdf = 0;
    s.delta = false;
    const float w = rtLightWindow(l, x);
    if (w <= 0) return false;
    if (l.type == kRtLightPoint || l.type == kRtLightSpot)
    {
        const float3 d = l.position - x;
        const float d2 = dot(d, d);
        if (d2 <= 0) return false;
        s.distance = sqrt(d2);
        s.wi = d / s.distance;
        float f = l.intensity * w / d2;
        if (l.type == kRtLightSpot)
        {
            const float sp = saturate(dot(-s.wi, l.forward) * l.spotScale + l.spotOffset);
            f *= sp * sp;
        }
        if (f <= 0) return false;
        s.L = l.color * f;
        s.pdf = 1;
        s.delta = true;
        return true;
    }
    if (l.type == kRtLightRect || l.type == kRtLightDisk)
    {
        float3 p;
        float area;
        if (l.type == kRtLightRect)
        {
            p = l.position + l.right * ((u1 - 0.5f) * l.size.x) + l.up * ((u2 - 0.5f) * l.size.y);
            area = l.size.x * l.size.y;
        }
        else
        {
            // Concentric disk mapping (Shirley-Chiu).
            const float a = 2 * u1 - 1, b = 2 * u2 - 1;
            float rr = 0, phi = 0;
            if (a != 0 || b != 0)
            {
                if (abs(a) > abs(b))
                {
                    rr = a;
                    phi = (kRtPi / 4) * (b / a);
                }
                else
                {
                    rr = b;
                    phi = kRtPi / 2 - (kRtPi / 4) * (a / b);
                }
            }
            p = l.position + l.right * (l.size.x * rr * cos(phi)) + l.up * (l.size.x * rr * sin(phi));
            area = kRtPi * l.size.x * l.size.x;
        }
        const float3 d = p - x;
        const float d2 = dot(d, d);
        s.distance = sqrt(d2);
        s.wi = d / s.distance;
        const float cosL = -dot(s.wi, l.forward);
        if (cosL <= 0 || area <= 0) return false;
        s.pdf = d2 / (area * cosL);
        s.L = l.color * (l.intensity * w);
        return true;
    }
    if (l.type == kRtLightSphere)
    {
        const float3 dc = l.position - x;
        const float dist2 = dot(dc, dc), r = l.size.x;
        if (dist2 <= r * r) return false;
        const float dist = sqrt(dist2);
        const float sin2 = r * r / dist2, cosMax = sqrt(max(0.0f, 1 - sin2));
        const float oneMinus = sin2 / (1 + cosMax);
        const float cosT = 1 - u1 * oneMinus, sinT = sqrt(max(0.0f, 1 - cosT * cosT)), phi = 2 * kRtPi * u2;
        const float3 axis = dc / dist;
        float3 t1, t2;
        rtOrthonormal(axis, t1, t2);
        s.wi = normalize(axis * cosT + t1 * (sinT * cos(phi)) + t2 * (sinT * sin(phi)));
        const float b = dot(s.wi, dc), h = b * b - (dist2 - r * r);
        s.distance = b - sqrt(max(0.0f, h));
        s.pdf = 1.0f / (2 * kRtPi * oneMinus);
        s.L = l.color * (l.intensity * w);
        return true;
    }
    // Tube (capsule): uniform over the side and the two hemispherical caps.
    const float len = l.size.x, rad = l.size.y;
    const float3 ea = l.position - l.right * (0.5f * len), eb = l.position + l.right * (0.5f * len);
    const float sideArea = 2 * kRtPi * rad * len, capArea = 4 * kRtPi * rad * rad, areaT = sideArea + capArea;
    float3 pt, n;
    const float t1u = u1 * areaT;
    float3 e1, e2;
    rtOrthonormal(l.right, e1, e2);
    if (t1u < sideArea)
    {
        const float along = t1u / sideArea * len, phi = 2 * kRtPi * u2;
        n = e1 * cos(phi) + e2 * sin(phi);
        pt = ea + l.right * along + n * rad;
    }
    else
    {
        const float v = (t1u - sideArea) / capArea;
        const float z = 1 - 2 * v, sr = sqrt(max(0.0f, 1 - z * z)), phi = 2 * kRtPi * u2;
        n = l.right * z + e1 * (sr * cos(phi)) + e2 * (sr * sin(phi));
        pt = (z >= 0 ? eb : ea) + n * rad;
    }
    const float3 d = pt - x;
    const float d2 = dot(d, d);
    s.distance = sqrt(d2);
    s.wi = d / s.distance;
    const float cosL = -dot(s.wi, n);
    if (cosL <= 0) return false;
    s.pdf = d2 / (areaT * cosL);
    s.L = l.color * (l.intensity * w);
    return true;
}

// First intersection of a ray with a capsule (segment pa-pb, radius r), -1 if none (Quilez).
RT_INLINE float rtCapsuleIntersect(float3 ro, float3 rd, float3 pa, float3 pb, float r)
{
    const float3 ba = pb - pa, oa = ro - pa;
    const float baba = dot(ba, ba), bard = dot(ba, rd), baoa = dot(ba, oa), rdoa = dot(rd, oa), oaoa = dot(oa, oa);
    const float a = baba - bard * bard, b = baba * rdoa - baoa * bard, c = baba * oaoa - baoa * baoa - r * r * baba;
    float h = b * b - a * c;
    if (h >= 0 && a > 1e-12f)
    {
        const float t = (-b - sqrt(h)) / a;
        const float y = baoa + t * bard;
        if (y > 0 && y < baba) return t;
        const float3 oc = y <= 0 ? oa : ro - pb;
        const float b2 = dot(rd, oc), c2 = dot(oc, oc) - r * r;
        h = b2 * b2 - c2;
        if (h > 0) return -b2 - sqrt(h);
        return -1;
    }
    float best = -1;
    for (int k = 0; k < 2; ++k)
    {
        const float3 e = k == 0 ? pa : pb;
        const float3 oc = ro - e;
        const float b2 = dot(rd, oc), c2 = dot(oc, oc) - r * r, h2 = b2 * b2 - c2;
        if (h2 > 0)
        {
            const float t = -b2 - sqrt(h2);
            if (t > 0 && (best < 0 || t < best)) best = t;
        }
    }
    return best;
}

// The ray from o along unit d hits the emitting side of an area light before tmax: distance, radiance, solid-angle
// pdf. The window is evaluated at o (LightSet::intersect).
RT_INLINE bool rtLightIntersect(RtLight l, float3 o, float3 d, float tmax, RT_OUT(float) t, RT_OUT(float3) L, RT_OUT(float) pdf)
{
    t = 0;
    L = rtSplat3(0);
    pdf = 0;
    const float w = rtLightWindow(l, o);
    if (w <= 0) return false;
    if (l.type == kRtLightPoint || l.type == kRtLightSpot) return false;
    if (l.type == kRtLightRect || l.type == kRtLightDisk)
    {
        const float dn = dot(d, l.forward);
        if (dn >= 0) return false;
        const float th = dot(l.position - o, l.forward) / dn;
        if (!(th > 0 && th < tmax)) return false;
        const float3 q = o + d * th - l.position;
        float area;
        if (l.type == kRtLightRect)
        {
            if (abs(dot(q, l.right)) > 0.5f * l.size.x || abs(dot(q, l.up)) > 0.5f * l.size.y) return false;
            area = l.size.x * l.size.y;
        }
        else
        {
            if (dot(q, q) > l.size.x * l.size.x) return false;
            area = kRtPi * l.size.x * l.size.x;
        }
        t = th;
        pdf = th * th / (area * -dn);
        L = l.color * (l.intensity * w);
        return true;
    }
    if (l.type == kRtLightSphere)
    {
        const float3 dc = l.position - o;
        const float dist2 = dot(dc, dc), r = l.size.x;
        if (dist2 <= r * r) return false;
        const float b = dot(d, dc), h = b * b - (dist2 - r * r);
        if (h < 0) return false;
        const float th = b - sqrt(h);
        if (!(th > 0 && th < tmax)) return false;
        const float sin2 = r * r / dist2, cosMax = sqrt(max(0.0f, 1 - sin2));
        t = th;
        pdf = 1.0f / (2 * kRtPi * (sin2 / (1 + cosMax)));
        L = l.color * (l.intensity * w);
        return true;
    }
    const float len = l.size.x, rad = l.size.y;
    const float3 ea = l.position - l.right * (0.5f * len), eb = l.position + l.right * (0.5f * len);
    const float th = rtCapsuleIntersect(o, d, ea, eb, rad);
    if (!(th > 0 && th < tmax)) return false;
    const float3 p = o + d * th;
    const float3 ba = eb - ea;
    const float sp = clamp(dot(p - ea, ba) / dot(ba, ba), 0.0f, 1.0f);
    const float3 n = normalize(p - (ea + ba * sp));
    const float cosL = -dot(d, n);
    if (cosL <= 0) return false;
    const float area = 2 * kRtPi * rad * len + 4 * kRtPi * rad * rad;
    t = th;
    pdf = th * th / (area * cosL);
    L = l.color * (l.intensity * w);
    return true;
}
RT_END_NAMESPACE

#endif
