// Exact coverage of a triangle inside a pixel (INTERFACES_KO.md 5.5.1, 7.1). Owner: V. Shared by V's coverage layer
// (band B fragments) and M's edge composite (band A edge pixels), so both see the same geometry.
//   coverageTriangleArea: area of triangle (a, b, c) inside the pixel square [pixel, pixel + 1)^2, exact up to float
//                         rounding (Sutherland-Hodgman clip against the four pixel edges, shoelace area). Screen pixels,
//                         either winding.
//   coverageTriangleMask: which of the 32 subsamples (kCoverageSamples, stratified in x and y) lie inside the triangle;
//                         orders overlapping fragments inside one pixel (ARCHITECTURE 2.1: error <= 1/32 of the pixel).
#ifndef UNX_COVERAGE_HLSLI
#define UNX_COVERAGE_HLSLI

#define COVERAGE_SAMPLES 32u
#define COVERAGE_MAX_CLIPPED 7u  // a triangle clipped by four half-planes has at most 7 vertices

// Subsample i: x = (i + 0.5) / 32, y = bit-reversed i (base-2 radical inverse) + 1/64: one sample per row and column
// of the 32 x 32 grid.
float2 coverageSample(uint i)
{
    return float2((i + 0.5) / 32.0, (reversebits(i) >> 27) / 32.0 + 1.0 / 64.0);
}

// Clips polygon 'p' (n vertices) by the half-plane dot(axis, v) * sign <= limit * sign, i.e. keeps coord <= limit
// (sign = 1) or coord >= limit (sign = -1) of the chosen axis.
void coverageClip(inout float2 p[COVERAGE_MAX_CLIPPED], inout uint n, uint axis, float limit, float sign)
{
    float2 o[COVERAGE_MAX_CLIPPED];
    uint m = 0;
    [unroll] for (uint i = 0; i < COVERAGE_MAX_CLIPPED; ++i)
    {
        if (i >= n) break;
        const float2 a = p[i], b = p[(i + 1) % n];
        const float da = (a[axis] - limit) * sign, db = (b[axis] - limit) * sign;  // <= 0: inside
        if (da <= 0 && m < COVERAGE_MAX_CLIPPED) o[m++] = a;
        if ((da < 0 && db > 0) || (da > 0 && db < 0))
        {
            const float t = da / (da - db);
            if (m < COVERAGE_MAX_CLIPPED) o[m++] = lerp(a, b, t);
        }
    }
    n = m;
    [unroll] for (uint k = 0; k < COVERAGE_MAX_CLIPPED; ++k) p[k] = o[k];
}

float coverageTriangleArea(float2 a, float2 b, float2 c, float2 pixel)
{
    float2 p[COVERAGE_MAX_CLIPPED];
    p[0] = a - pixel;
    p[1] = b - pixel;
    p[2] = c - pixel;
    [unroll] for (uint k = 3; k < COVERAGE_MAX_CLIPPED; ++k) p[k] = 0;
    uint n = 3;
    coverageClip(p, n, 0, 0.0, -1.0);
    coverageClip(p, n, 0, 1.0, 1.0);
    coverageClip(p, n, 1, 0.0, -1.0);
    coverageClip(p, n, 1, 1.0, 1.0);
    float twice = 0;
    [unroll] for (uint i = 0; i < COVERAGE_MAX_CLIPPED; ++i)
    {
        if (i >= n) break;
        const float2 u = p[i], v = p[(i + 1) % n];
        twice += u.x * v.y - v.x * u.y;
    }
    return 0.5 * abs(twice);
}

uint coverageTriangleMask(float2 a, float2 b, float2 c, float2 pixel)
{
    const float orient = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
    const float s = orient >= 0 ? 1.0 : -1.0;
    uint mask = 0;
    [unroll] for (uint i = 0; i < COVERAGE_SAMPLES; ++i)
    {
        const float2 q = pixel + coverageSample(i);
        const float e0 = ((b.x - a.x) * (q.y - a.y) - (b.y - a.y) * (q.x - a.x)) * s;
        const float e1 = ((c.x - b.x) * (q.y - b.y) - (c.y - b.y) * (q.x - b.x)) * s;
        const float e2 = ((a.x - c.x) * (q.y - c.y) - (a.y - c.y) * (q.x - c.x)) * s;
        if (e0 >= 0 && e1 >= 0 && e2 >= 0) mask |= 1u << i;
    }
    return mask;
}

#endif
