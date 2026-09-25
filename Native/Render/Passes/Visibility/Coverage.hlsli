// Exact coverage of a triangle inside a pixel (INTERFACES_KO.md 5.5.1, 7.1). Owner: V. Shared by V's coverage layer
// (band B fragments) and M's edge composite (band A edge pixels), so both see the same geometry.
//   coverageTriangleArea: area of triangle (a, b, c) inside the pixel square [pixel, pixel + 1)^2, exact up to float
//                         rounding (Green's theorem on the boundary clamped to the square, streamed per edge). Screen
//                         pixels, either winding.
//   coverageTriangleAreaCentroid: the same area and the clipped polygon's centroid (coverage-layer fragment depth).
//   coverageTriangleMask: which of the 32 subsamples (kCoverageSamples, stratified in x and y) lie inside the triangle;
//                         orders overlapping fragments inside one pixel (ARCHITECTURE 2.1: error <= 1/32 of the pixel).
//   coverageTriangleMaskLut: the same bits from a 64 angle x 64 distance table of conservative edge masks
//                         (FrameConstants::coverageMaskLut, GpuScene coverageMaskTable): one 8 B load per edge, and the
//                         exact edge function only for the few subsamples the table leaves open (V test
//                         coverage_mask_lut_matches_exact: identical to coverageTriangleMask).
#ifndef UNX_COVERAGE_HLSLI
#define UNX_COVERAGE_HLSLI

#define COVERAGE_SAMPLES 32u

// Subsample i: x = (i + 0.5) / 32, y = bit-reversed i (base-2 radical inverse) + 1/64: one sample per row and column
// of the 32 x 32 grid.
float2 coverageSample(uint i)
{
    return float2((i + 0.5) / 32.0, (reversebits(i) >> 27) / 32.0 + 1.0 / 64.0);
}

// Area (and first moments) of a triangle inside the unit pixel square by Green's theorem on its boundary clamped to the
// square: clamping every point of a closed curve to a convex box keeps the winding number of every interior point of
// the box (the segment from a point to its clamp never crosses the interior), so the clamped curve's signed area is
// the area of the triangle inside the box, and its first moments are the region's. Each edge is split where it crosses
// x = 0, 1 (clamping x), each piece where it crosses y = 0, 1 (clamping y), and the pieces add shoelace terms: no
// vertex arrays, registers only (V's microbenchmark: 0.121 vs 0.246 ns per fragment for the Sutherland-Hodgman clip).
struct CoverageGreen
{
    float twice;    // 2 x signed area
    float2 moment;  // 6 x first moments (sum (u + v)(u x v))
};

void coverageGreenSegment(float2 u, float2 v, inout CoverageGreen g)
{
    const float cr = u.x * v.y - v.x * u.y;
    g.twice += cr;
    g.moment += (u + v) * cr;
}

// Segment p -> q with x already in [0, 1]: split at y = 0, 1 and clamp y.
void coverageGreenClipY(float2 p, float2 q, inout CoverageGreen g)
{
    const float dy = q.y - p.y;
    if (abs(dy) < 1e-7)
    {
        const float y = clamp(p.y, 0.0, 1.0);
        coverageGreenSegment(float2(p.x, y), float2(q.x, y), g);
        return;
    }
    const float t0 = (0.0 - p.y) / dy, t1 = (1.0 - p.y) / dy;
    const float ta = clamp(min(t0, t1), 0.0, 1.0), tb = clamp(max(t0, t1), 0.0, 1.0);
    float2 a = lerp(p, q, ta), b = lerp(p, q, tb);
    a.y = clamp(a.y, 0.0, 1.0);
    b.y = clamp(b.y, 0.0, 1.0);
    const float2 pa = float2(p.x, clamp(p.y, 0.0, 1.0)), qb = float2(q.x, clamp(q.y, 0.0, 1.0));
    coverageGreenSegment(pa, a, g);
    coverageGreenSegment(a, b, g);
    coverageGreenSegment(b, qb, g);
}

// Segment p -> q: split at x = 0, 1 and clamp x, then each piece in y.
void coverageGreenClip(float2 p, float2 q, inout CoverageGreen g)
{
    const float dx = q.x - p.x;
    if (abs(dx) < 1e-7)
    {
        const float x = clamp(p.x, 0.0, 1.0);
        coverageGreenClipY(float2(x, p.y), float2(x, q.y), g);
        return;
    }
    const float t0 = (0.0 - p.x) / dx, t1 = (1.0 - p.x) / dx;
    const float ta = clamp(min(t0, t1), 0.0, 1.0), tb = clamp(max(t0, t1), 0.0, 1.0);
    float2 a = lerp(p, q, ta), b = lerp(p, q, tb);
    a.x = clamp(a.x, 0.0, 1.0);
    b.x = clamp(b.x, 0.0, 1.0);
    const float2 pa = float2(clamp(p.x, 0.0, 1.0), p.y), qb = float2(clamp(q.x, 0.0, 1.0), q.y);
    coverageGreenClipY(pa, a, g);
    coverageGreenClipY(a, b, g);
    coverageGreenClipY(b, qb, g);
}

CoverageGreen coverageGreenTriangle(float2 a, float2 b, float2 c, float2 pixel)
{
    CoverageGreen g;
    g.twice = 0;
    g.moment = 0;
    a -= pixel;
    b -= pixel;
    c -= pixel;
    coverageGreenClip(a, b, g);
    coverageGreenClip(b, c, g);
    coverageGreenClip(c, a, g);
    return g;
}

float coverageTriangleArea(float2 a, float2 b, float2 c, float2 pixel) { return 0.5 * abs(coverageGreenTriangle(a, b, c, pixel).twice); }

// Area and centroid (screen pixels) of the same region: the covered region's centre of mass, where the coverage layer
// evaluates a fragment's depth (always inside the triangle, unlike the pixel centre).
float coverageTriangleAreaCentroid(float2 a, float2 b, float2 c, float2 pixel, out float2 regionCentre)
{
    const CoverageGreen g = coverageGreenTriangle(a, b, c, pixel);
    regionCentre = pixel + (abs(g.twice) > 1e-12 ? g.moment / (3.0 * g.twice) : float2(0.5, 0.5));
    return 0.5 * abs(g.twice);
}

#define COVERAGE_LUT_ANGLES 64u
#define COVERAGE_LUT_DISTANCES 64u
#define COVERAGE_LUT_REACH 0.70710678  // sqrt(2) / 2: no subsample is farther from the pixel centre along any direction
#define COVERAGE_LUT_MARGIN (1.0 / 128)  // GpuScene.h kCoverageLutMargin

// Sure-inside and ambiguous subsamples of edge a -> b of a triangle (screen pixels; s = +-1 makes it counter-clockwise):
// the edge's table bin (inward normal folded into the upper half plane by complementing, pseudo-angle and distance)
// gives the subsamples inside or outside for every edge of the bin; the others are ambiguous.
void coverageEdgeLut(float2 a, float2 b, float2 pixel, float s, StructuredBuffer<uint2> lut, out uint inside, out uint ambiguous)
{
    const float2 e = (b - a) * s;
    const float len2 = dot(e, e);
    if (len2 == 0)
    {
        inside = 0;
        ambiguous = 0xFFFFFFFFu;  // the exact test decides (a zero edge function counts as inside)
        return;
    }
    float2 n = float2(-e.y, e.x);
    float h = dot(n, float2(0.5, 0.5) - (a - pixel)) * rsqrt(len2);
    const bool flip = n.y < 0 || (n.y == 0 && n.x < 0);
    if (flip)
    {
        n = -n;
        h = -h;
    }
    uint2 t;
    if (h >= COVERAGE_LUT_REACH + COVERAGE_LUT_MARGIN) t = uint2(0xFFFFFFFFu, 0);
    else if (h < -COVERAGE_LUT_REACH - COVERAGE_LUT_MARGIN) t = uint2(0, 0xFFFFFFFFu);
    else
    {
        const uint k = min((uint)((1 - n.x / (abs(n.x) + n.y)) * (COVERAGE_LUT_ANGLES / 2)), COVERAGE_LUT_ANGLES - 1);
        const uint j = min((uint)max((h + COVERAGE_LUT_REACH) * (COVERAGE_LUT_DISTANCES / (2 * COVERAGE_LUT_REACH)), 0.0), COVERAGE_LUT_DISTANCES - 1);
        t = lut[k * COVERAGE_LUT_DISTANCES + j];
    }
    if (flip) t = t.yx;
    inside = t.x;
    ambiguous = ~(t.x | t.y);
}

// coverageTriangleMask's result from the table: subsamples surely inside all three edges are set, surely outside one
// are dropped, and only the ambiguous ones that can still be covered run coverageTriangleMask's edge function (the
// same expression, so the result is the same bits; about two subsamples per edge that crosses the pixel).
uint coverageTriangleMaskLut(float2 a, float2 b, float2 c, float2 pixel, StructuredBuffer<uint2> lut)
{
    const float orient = (b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y);
    const float s = orient >= 0 ? 1.0 : -1.0;
    uint in0, am0, in1, am1, in2, am2;
    coverageEdgeLut(a, b, pixel, s, lut, in0, am0);
    coverageEdgeLut(b, c, pixel, s, lut, in1, am1);
    coverageEdgeLut(c, a, pixel, s, lut, in2, am2);
    uint mask = (in0 | am0) & (in1 | am1) & (in2 | am2);
    uint test = mask & (am0 | am1 | am2);
    while (test != 0)
    {
        const uint i = firstbitlow(test);
        test &= test - 1;
        const float2 q = pixel + coverageSample(i);
        const bool e0 = (am0 >> i & 1) == 0 || ((b.x - a.x) * (q.y - a.y) - (b.y - a.y) * (q.x - a.x)) * s >= 0;
        const bool e1 = (am1 >> i & 1) == 0 || ((c.x - b.x) * (q.y - b.y) - (c.y - b.y) * (q.x - b.x)) * s >= 0;
        const bool e2 = (am2 >> i & 1) == 0 || ((a.x - c.x) * (q.y - c.y) - (a.y - c.y) * (q.x - c.x)) * s >= 0;
        if (!(e0 && e1 && e2)) mask &= ~(1u << i);
    }
    return mask;
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
