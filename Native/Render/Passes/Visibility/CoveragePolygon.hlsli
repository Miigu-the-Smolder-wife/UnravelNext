// V internal (shared by V's coverage kernels and the depth raster service's coverage mode): exact evaluation of one
// primitive inside one pixel. The primitive is its triangle clipped to the view's near plane (CoverageRaster.ms): 3 or
// 4 vertices, each (x, y in render-target pixels, device depth z / w, 1 / w), and the vertices' uvs. Owner: V.
//   - area: the exact area of the polygon inside the pixel square (Coverage.hlsli clipping), in pixels;
//   - depth: device depth at the covered region's centroid (z / w is affine in screen space over the plane);
//   - mask: the 32 subsamples (coverageSample) inside the polygon;
//   - alpha-tested materials: subsamples whose texture alpha fails are cleared and the area is scaled by the passing
//     fraction. The uv is perspective-correct (uv / w and 1 / w are affine) and the footprint is one subsample's cell
//     (the pixel's gradients x 1 / sqrt(32)); a polygon covering no subsample takes the test at its centroid.
#ifndef UNX_COVERAGE_POLYGON_HLSLI
#define UNX_COVERAGE_POLYGON_HLSLI
#include "Frame.hlsli"
#include "Passes/Visibility/Coverage.hlsli"
#include "Passes/Material/MaterialTextures.hlsli"

struct CoveragePolygon
{
    float4 a, b, c, d;  // d = c unless quad
    float2 ta, tb, tc, td;
    bool quad;
};

struct CoverageSample
{
    float area;   // 0 = nothing of the primitive in this pixel (or everything cut out)
    float depth;
    uint mask;
};

// Barycentric weights of q with respect to (a, b, c) (screen pixels); affine, so valid outside the triangle.
float3 coveragePolygonBarycentric(float2 a, float2 b, float2 c, float2 q)
{
    const float2 e1 = b - a, e2 = c - a, d = q - a;
    const float det = e1.x * e2.y - e1.y * e2.x;
    if (abs(det) < 1e-20) return float3(1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0);
    const float s = (d.x * e2.y - d.y * e2.x) / det, t = (e1.x * d.y - e1.y * d.x) / det;
    return float3(1 - s - t, s, t);
}

// Perspective-correct uv at screen point s of triangle (a, b, c).
float2 coveragePolygonUv(float4 a, float4 b, float4 c, float2 ta, float2 tb, float2 tc, float2 s)
{
    const float3 w = coveragePolygonBarycentric(a.xy, b.xy, c.xy, s);
    const float3 q = w * float3(a.w, b.w, c.w);
    return (ta * q.x + tb * q.y + tc * q.z) / max(q.x + q.y + q.z, 1e-30);
}

// Area and centroid depth of the polygon in pixel 'pixel' (integer corner); the mask is coveragePolygonMask (after any
// occlusion test, which needs only the depth); no alpha test.
CoverageSample coveragePolygonGeometry(CoveragePolygon p, float2 pixel, out float2 mid)
{
    CoverageSample s;
    s.area = coverageTriangleAreaCentroid(p.a.xy, p.b.xy, p.c.xy, pixel, mid);
    if (p.quad)
    {
        float2 c2;
        const float area2 = coverageTriangleAreaCentroid(p.a.xy, p.c.xy, p.d.xy, pixel, c2);
        if (area2 > 0) mid = (mid * s.area + c2 * area2) / (s.area + area2);
        s.area += area2;
    }
    s.depth = 0;
    s.mask = 0;
    if (!(s.area > 0)) return s;
    // The plane's affine depth from the better-conditioned of the two triangles.
    const bool second = p.quad && abs(cross(float3(p.c.xy - p.a.xy, 0), float3(p.d.xy - p.a.xy, 0)).z) > abs(cross(float3(p.b.xy - p.a.xy, 0), float3(p.c.xy - p.a.xy, 0)).z);
    const float4 pb = second ? p.c : p.b, pc = second ? p.d : p.c;
    s.depth = dot(coveragePolygonBarycentric(p.a.xy, pb.xy, pc.xy, mid), float3(p.a.z, pb.z, pc.z));
    return s;
}

uint coveragePolygonMask(CoveragePolygon p, float2 pixel)
{
    // The frame's coverage mask LUT (v1.34): coverageTriangleMask's bits at a fraction of the edge evaluations.
    StructuredBuffer<uint2> lut = ResourceDescriptorHeap[g_coverageMaskLut];
    uint mask = coverageTriangleMaskLut(p.a.xy, p.b.xy, p.c.xy, pixel, lut);
    if (p.quad) mask |= coverageTriangleMaskLut(p.a.xy, p.c.xy, p.d.xy, pixel, lut);
    return mask;
}

// The alpha test of 'material' over a sample from coveragePolygonGeometry: clears failing subsamples, scales the area;
// area 0 when everything is cut out.
void coveragePolygonAlpha(CoveragePolygon p, uint material, float2 pixel, float2 mid, inout CoverageSample s)
{
    const bool second = p.quad && abs(cross(float3(p.c.xy - p.a.xy, 0), float3(p.d.xy - p.a.xy, 0)).z) > abs(cross(float3(p.b.xy - p.a.xy, 0), float3(p.c.xy - p.a.xy, 0)).z);
    const float4 pb = second ? p.c : p.b, pc = second ? p.d : p.c;
    const float2 ub = second ? p.tc : p.tb, uc = second ? p.td : p.tc;
    const GpuMaterial m = loadMaterial(material);
    const float2 uv0 = coveragePolygonUv(p.a, pb, pc, p.ta, ub, uc, mid);
    const float footprint = 0.17677670;  // 1 / sqrt(COVERAGE_SAMPLES)
    const float2 duvdx = (coveragePolygonUv(p.a, pb, pc, p.ta, ub, uc, mid + float2(1, 0)) - uv0) * footprint;
    const float2 duvdy = (coveragePolygonUv(p.a, pb, pc, p.ta, ub, uc, mid + float2(0, 1)) - uv0) * footprint;
    const uint before = countbits(s.mask);
    if (before == 0)
    {
        if (materialBaseColorGrad(m, uv0, duvdx, duvdy).a < m.alphaCutoff) s.area = 0;
        return;
    }
    uint pass = s.mask;
    for (uint bits = s.mask; bits != 0; bits &= bits - 1)
    {
        const uint i = firstbitlow(bits);
        const float2 uv = coveragePolygonUv(p.a, pb, pc, p.ta, ub, uc, pixel + coverageSample(i));
        if (materialBaseColorGrad(m, uv, duvdx, duvdy).a < m.alphaCutoff) pass &= ~(1u << i);
    }
    s.area = pass == 0 ? 0 : s.area * (float)countbits(pass) / (float)before;
    s.mask = pass;
}

#endif
