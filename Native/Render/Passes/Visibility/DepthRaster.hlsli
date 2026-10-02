// Pixel-kernel contract of V's depth raster service (INTERFACES_KO.md 5.3, FrameServices::rasterizeDepth). Owner: V.
// The requester's pixel kernel (DepthRasterRequest::pixelKernel) takes DepthRasterPixel as its only input and calls
// depthRasterCovered(p) before writing anything, so alpha-tested materials cut out the same shape as in the main view:
//
//   #include "Passes/Visibility/DepthRaster.hlsli"
//   void main(DepthRasterPixel p) { if (!depthRasterCovered(p)) discard; ... }
//
// Root constants 0..15 belong to V, 16..31 to the requester (DepthRasterRequest::pixelConstants -> P[4..7]).
//
// Coverage mode (DepthRasterRequest::coverage, v1.26; S's VSM transmittance layer): the kernel defines
// DEPTH_RASTER_COVERAGE 1 before the include, the raster is conservative, and the kernel runs once per texel the
// primitive touches. depthRasterCoverage(p) gives the exact area of the primitive inside the texel (x the alpha-tested
// material's cutout coverage), the 32-subsample mask (Coverage.hlsli coverageSample) and the device depth at the covered
// region's centroid; area 0 = nothing of it in this texel (return without writing). Only clusters of band B in the
// request's views are drawn (DepthRasterRequest::bands); depthRasterCovered is not needed (the alpha test is inside).
//
//   #define DEPTH_RASTER_COVERAGE 1
//   #include "Passes/Visibility/DepthRaster.hlsli"
//   void main(DepthRasterPixel p) { const DepthRasterCoverage c = depthRasterCoverage(p); if (!(c.area > 0)) return; ... }
//
// Surface frame (DepthRasterRequest::pixelNormals, v2): the kernel defines DEPTH_RASTER_NORMALS 1 before the include and
// reads p.normal and p.tangent - the vertices' world normal and tangent, deformed like their positions (skin, wind,
// morphs: deformVertex) and interpolated (not unit); tangent.w is the bitangent's sign (bitangent = w cross(normal,
// tangent)). A request with render targets (DepthRasterRequest::colorTargets) returns their values (SV_Target0 ..).
#ifndef UNX_DEPTH_RASTER_HLSLI
#define UNX_DEPTH_RASTER_HLSLI
#include "Bindless.hlsli"
#include "Scene.hlsli"
#include "Passes/Visibility/AlphaTest.hlsli"

struct DepthRasterPixel
{
    float4 position : SV_Position;           // viewport pixel centre, z = device depth of the view
    float2 uv : TEXCOORD0;                   // material uv (alpha test)
#if DEPTH_RASTER_NORMALS
    float3 normal : NORMAL;                  // world, interpolated: normalise before use
    float4 tangent : TANGENT;                // world xyz, interpolated; w = the bitangent's sign
#endif
    nointerpolation uint userData : USERDATA; // RasterView::userData of the view being rasterised
    nointerpolation uint material : MATERIAL; // scene material of the triangle (instance overrides applied)
    nointerpolation uint instance : INSTANCE; // scene instance of the triangle (loadInstance)
#if DEPTH_RASTER_COVERAGE
    // The triangle clipped to the view's near plane (3 or 4 vertices): render-target pixels x, y, device depth, 1/w.
    nointerpolation float4 covA : COVA;
    nointerpolation float4 covB : COVB;
    nointerpolation float4 covC : COVC;
    nointerpolation float4 covD : COVD;       // = covC unless covFlags bit 1
    nointerpolation float4 covUvAB : COVUVAB;
    nointerpolation float4 covUvCD : COVUVCD;
    nointerpolation uint covFlags : COVFLAGS; // bit 0 alpha-tested material, bit 1 quad
#endif
};

// False where an alpha-tested material is transparent (baseColor texture alpha < alphaCutoff), as in the main view and
// the reference (INTERFACES_KO.md 8.1).
bool depthRasterCovered(DepthRasterPixel p) { return alphaTestCovered(p.material, p.uv); }

#if DEPTH_RASTER_COVERAGE
#include "Passes/Visibility/CoveragePolygon.hlsli"

struct DepthRasterCoverage
{
    float area;   // exact area of the primitive inside the texel, in texels, x cutout coverage; 0 = none
    uint mask;    // 32-subsample mask
    float depth;  // device depth at the covered region's centroid
};

DepthRasterCoverage depthRasterCoverage(DepthRasterPixel p)
{
    CoveragePolygon poly;
    poly.a = p.covA;
    poly.b = p.covB;
    poly.c = p.covC;
    poly.d = p.covD;
    poly.ta = p.covUvAB.xy;
    poly.tb = p.covUvAB.zw;
    poly.tc = p.covUvCD.xy;
    poly.td = p.covUvCD.zw;
    poly.quad = (p.covFlags & 2u) != 0;
    const float2 texel = floor(p.position.xy);
    float2 mid;
    CoverageSample s = coveragePolygonGeometry(poly, texel, mid);
    DepthRasterCoverage c;
    c.area = 0;
    c.mask = 0;
    c.depth = s.depth;
    if (!(s.area > 0)) return c;
    s.mask = coveragePolygonMask(poly, texel);
    if ((p.covFlags & 1u) != 0) coveragePolygonAlpha(poly, p.material, texel, mid, s);
    c.area = s.area;
    c.mask = s.mask;
    return c;
}
#endif

#endif
