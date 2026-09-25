// Edge (E) pixels and their surface groups (ARCHITECTURE 2.10; INTERFACES 5.5.1). Owner: M.
//
// Two pixels see the same surface when both are sky, or both show the same material and each lies on the other's
// tangent plane (shading normal) and their normals agree:
//   |(P_q - P_p) . n_p| and |(P_p - P_q) . n_q| <= max(k_f footprint, k_d |P_q - P_p|),  n_p . n_q >= cos(angle).
// The relation is symmetric, so a pixel an edge pixel borrows radiance from is itself an edge pixel. The distance term
// grows with the pixels' separation, which keeps a bumpy (normal-mapped) surface seen at a grazing angle continuous while
// a depth gap (silhouette) exceeds it; the normal term finds hard creases. A pixel is an edge pixel (E) when a 3 x 3
// neighbour sees another surface. Triangle boundaries inside one smooth surface are not edges.
#ifndef UNX_M_EDGE_HLSLI
#define UNX_M_EDGE_HLSLI
#include "GBuffer.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"
#include "Passes/Shading/ShadingCommon.hlsli"

struct EdgeParams
{
    float cosAngle;           // shading.edge_normal_angle_deg
    float footprintTolerance; // shading.edge_footprint_tolerance (pixel footprints)
    float distanceTolerance;  // shading.edge_distance_tolerance (fraction of the pixels' separation)
};

struct EdgePixel
{
    bool sky;
    uint material;
    float3 position;  // camera-relative
    float3 normal;
    float footprint;  // lateral size of one pixel at the surface (m)
};

// A pixel's edge sample from its material, linear depth and shading normal. Every consumer (a shading kernel for its
// pixel and the neighbours it compares, the composite) builds a pixel's sample with this one function from the same
// inputs, so the same-surface relation is exactly symmetric: whoever a pixel sees as another surface sees it back, and
// every surface an edge pixel's composite borrows radiance from is itself an edge pixel (its radiance was kept).
EdgePixel edgeSample(uint2 q, uint material, float linearZ, float3 normal)
{
    EdgePixel e;
    e.material = material;
    e.sky = material == M_MATERIAL_SKY;
    e.position = 0;
    e.normal = 0;
    e.footprint = 0;
    if (!e.sky)
    {
        float3 D, Dx, Dy;
        mPixelRay(float2(q) + 0.5, D, Dx, Dy);
        e.position = D * linearZ;
        e.normal = normal;
        e.footprint = length(Dx) * linearZ;
    }
    return e;
}

EdgePixel edgePixel(uint2 q, Texture2D<uint> words, Texture2D<float> depth, Texture2D<uint2> gbuffer)
{
    const uint material = mWordMaterial(words[q]);
    if (material == M_MATERIAL_SKY) return edgeSample(q, material, 0, 0);
    return edgeSample(q, material, linearDepth(depth[q]), octDecode(gbuffer[q].x));
}

bool edgeSameSurface(EdgePixel a, EdgePixel b, EdgeParams p)
{
    if (a.sky || b.sky) return a.sky && b.sky;
    if (a.material != b.material) return false;
    if (dot(a.normal, b.normal) < p.cosAngle) return false;
    const float3 d = b.position - a.position;
    const float tol = max(p.footprintTolerance * max(a.footprint, b.footprint), p.distanceTolerance * length(d));
    return abs(dot(d, a.normal)) <= tol && abs(dot(d, b.normal)) <= tol;
}

// Mean of clamp(y, 0, 1) over a linear ramp between ya and yb (either order): the ramp spends t0 below 0, t1 - t0
// inside [0, 1] (linear there between the clamped end values) and 1 - t1 above 1.
float edgeRampClampMean(float ya, float yb)
{
    const float lo = min(ya, yb), hi = max(ya, yb), s = max(hi - lo, 1e-30);
    const float t0 = saturate(-lo / s), t1 = saturate((1 - lo) / s);
    return (t1 - t0) * 0.5 * (saturate(lo) + saturate(hi)) + (1 - t1);
}

// Signed contribution of the directed edge p -> q (pixel-corner relative) to the area inside the unit square: its
// overlap with the square's x range times the mean of clamp(y, 0, 1) along that part, signed by the x direction.
float edgeColumnIntegral(float2 p, float2 q)
{
    const float dx = q.x - p.x, inv = dx != 0 ? 1 / dx : 0;
    const float x0 = max(min(p.x, q.x), 0.0), x1 = min(max(p.x, q.x), 1.0);
    const float ya = lerp(p.y, q.y, saturate((x0 - p.x) * inv)), yb = lerp(p.y, q.y, saturate((x1 - p.x) * inv));
    return (dx > 0 ? 1.0 : -1.0) * max(x1 - x0, 0.0) * edgeRampClampMean(ya, yb);
}

// Exact area of triangle (a, b, c) (screen pixels, either winding) inside the pixel square [pixel, pixel + 1)^2, by
// Green's theorem: on every vertical line through the square the boundary's crossings, signed by their x direction,
// sum to the inside length clamp(y_upper, 0, 1) - clamp(y_lower, 0, 1), so the area is the sum over the three edges of
// their column integrals (closed form above). Registers only, no clipping; the same quantity as V's
// coverageTriangleArea (Coverage.hlsli) up to float rounding.
float edgeTriangleArea(float2 a, float2 b, float2 c, float2 pixel)
{
    a -= pixel;
    b -= pixel;
    c -= pixel;
    return abs(edgeColumnIntegral(a, b) + edgeColumnIntegral(b, c) + edgeColumnIntegral(c, a));
}

// Fraction of the pixel square an alpha-tested (cut-out) surface covers at uv (screen gradients dx, dy): the part where
// the base colour's alpha passes the cutoff, as V's alpha test decides at a pixel centre (MaterialTextures.hlsli).
//   Minified (footprint >= 2 base texels along its major axis): M's cut-out coverage mips (fraction of base texels
//   passing the cutoff, box mips) filtered over the footprint.
//   Magnified (<= 1 texel): the bilinear alpha of mip 0 is linear to first order across the pixel, so the passing
//   region is a half-plane; its exact area in the pixel square (shHalfPlaneCoverage) with the alpha's screen gradient.
//   Between 1 and 2 texels the two blend linearly.
float edgeCutoutCoverage(MTextureSet ts, float cutoff, float2 uv, float2 dx, float2 dy)
{
    const bool clampAddress = (ts.flags & M_TEX_BASE_COLOR) != 0;
    Texture2D<float4> base = ResourceDescriptorHeap[ts.baseColor];
    uint w, h;
    base.GetDimensions(w, h);
    const float2 size = float2(w, h);
    const float rho = max(length(dx * size), length(dy * size));
    float minified = 0, magnified = 0;
    if (rho > 1)
    {
        Texture2D<float> coverage = ResourceDescriptorHeap[ts.coverage];
        minified = clampAddress ? coverage.SampleGrad(g_anisoClamp, uv, dx, dy) : coverage.SampleGrad(g_anisoWrap, uv, dx, dy);
    }
    if (rho < 2)
    {
        // The bilinear cell around uv on mip 0 (as mNormalMoments: chosen here, read with the texture's addressing).
        const float2 p = uv * size - 0.5;
        const float2 c0 = floor(p);
        const float2 f = p - c0;
        int2 i0, i1;
        if (clampAddress)
        {
            i0 = clamp(int2(c0), int2(0, 0), int2(w - 1, h - 1));
            i1 = clamp(int2(c0) + 1, int2(0, 0), int2(w - 1, h - 1));
        }
        else
        {
            i0 = int2(c0 - size * floor(c0 / size));
            i1 = int2((c0 + 1) - size * floor((c0 + 1) / size));
        }
        const float a00 = base.Load(int3(i0.x, i0.y, 0)).a, a10 = base.Load(int3(i1.x, i0.y, 0)).a;
        const float a01 = base.Load(int3(i0.x, i1.y, 0)).a, a11 = base.Load(int3(i1.x, i1.y, 0)).a;
        const float alpha = lerp(lerp(a00, a10, f.x), lerp(a01, a11, f.x), f.y);
        const float2 perTexel = float2(lerp(a10 - a00, a11 - a01, f.y), lerp(a01 - a00, a11 - a10, f.x));
        const float2 g = float2(dot(perTexel, dx * size), dot(perTexel, dy * size));  // d alpha per pixel step
        const float gl = length(g);
        magnified = gl > 1e-8 ? shHalfPlaneCoverage(g / gl, (alpha - cutoff) / gl) : (alpha >= cutoff ? 1.0 : 0.0);
    }
    return rho <= 1 ? magnified : (rho >= 2 ? minified : lerp(magnified, minified, rho - 1));
}

// Edge pixel list (raw; EdgeDetect.hlsl fills it with one atomic per tile): entry 0 the pixel count (EdgeArgs.hlsl copies
// it from the edge args), entries from 1 the pixels (x | y << 16). Edge args (raw, 24 B): bytes 0-11 the composite's
// D3D12_DISPATCH_ARGUMENTS (EdgeArgs.hlsl fills x), byte 12 the pixel count (ShadeBegin zeroes it).

#endif
