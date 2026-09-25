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

EdgePixel edgePixel(uint2 q, Texture2D<uint> words, Texture2D<float> depth, Texture2D<uint2> gbuffer)
{
    EdgePixel e;
    const uint word = words[q];
    e.material = mWordMaterial(word);
    e.sky = e.material == M_MATERIAL_SKY;
    e.position = 0;
    e.normal = 0;
    e.footprint = 0;
    if (!e.sky)
    {
        float3 D, Dx, Dy;
        mPixelRay(float2(q) + 0.5, D, Dx, Dy);
        const float z = linearDepth(depth[q]);
        e.position = D * z;
        e.normal = octDecode(gbuffer[q].x);
        e.footprint = length(Dx) * z;
    }
    return e;
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

// The centre's sample from what the shading kernel already holds: material, pixel ray D (unit view depth) and its x
// derivative, linear depth, shading normal.
EdgePixel edgeCenter(uint material, float3 D, float3 Dx, float linearZ, float3 normal)
{
    EdgePixel e;
    e.material = material;
    e.sky = material == M_MATERIAL_SKY;
    e.position = e.sky ? 0 : D * linearZ;
    e.normal = e.sky ? 0 : normal;
    e.footprint = e.sky ? 0 : length(Dx) * linearZ;
    return e;
}

// Is 'pixel' (centre sample c, pixel ray D, Dx, Dy) an edge pixel: some in-view 3 x 3 neighbour sees another surface.
// A neighbour on the same triangle (same vis id) is the same surface and one with another material is not, before any
// geometry is read. A neighbour's ray is the centre's plus its offset times the ray's screen derivatives (the pixel ray
// is affine in pixel position), so only its depth and normal are loaded.
bool edgeIsEdge(uint2 pixel, EdgePixel c, float3 D, float3 Dx, float3 Dy, Texture2D<uint> visIds, Texture2D<uint> words, Texture2D<float> depth,
                Texture2D<uint2> gbuffer, EdgeParams p)
{
    const uint vc = visIds[pixel];
    [unroll] for (uint k = 0; k < 9; ++k)
    {
        if (k == 4) continue;
        const int2 o = int2(int(k % 3) - 1, int(k / 3) - 1);
        const int2 q = int2(pixel) + o;
        if (any(q < 0) || q.x >= int(g_viewWidth) || q.y >= int(g_viewHeight)) continue;
        if (visIds[uint2(q)] == vc) continue;
        EdgePixel e;
        e.material = mWordMaterial(words[uint2(q)]);
        if (e.material != c.material) return true;
        e.sky = c.sky;
        if (e.sky) continue;  // the sky has one vis id (VIS_NONE); kept for completeness
        const float z = linearDepth(depth[uint2(q)]);
        e.position = (D + float(o.x) * Dx + float(o.y) * Dy) * z;
        e.normal = octDecode(gbuffer[uint2(q)].x);
        e.footprint = length(Dx) * z;
        if (!edgeSameSurface(c, e, p)) return true;
    }
    return false;
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

// Appends the wave's edge pixels to the edge pixel list (one atomic per wave). Edge args layout (raw, 24 B): bytes 0-11
// the composite's D3D12_DISPATCH_ARGUMENTS (EdgeArgs.hlsl fills x), byte 12 the pixel count. The list holds the count in
// entry 0 (EdgeArgs.hlsl copies it) and the pixels from entry 1.
void edgeAppendPixel(uint2 pixel, bool isEdge, uint listUav, uint argsUav)
{
    const uint n = WaveActiveCountBits(isEdge);
    if (n == 0) return;
    uint base = 0;
    if (WaveIsFirstLane())
    {
        RWByteAddressBuffer args = ResourceDescriptorHeap[argsUav];
        args.InterlockedAdd(12, n, base);
    }
    base = WaveReadLaneFirst(base);
    if (isEdge)
    {
        RWByteAddressBuffer list = ResourceDescriptorHeap[listUav];
        list.Store(4 * (1 + base + WavePrefixCountBits(isEdge)), pixel.x | (pixel.y << 16));
    }
}

#endif
