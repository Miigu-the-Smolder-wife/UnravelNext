// Pixel-kernel contract of V's depth raster service (INTERFACES_KO.md 5.3, FrameServices::rasterizeDepth). Owner: V.
// The requester's pixel kernel (DepthRasterRequest::pixelKernel) takes DepthRasterPixel as its only input and calls
// depthRasterCovered(p) before writing anything, so alpha-tested materials cut out the same shape as in the main view:
//
//   #include "Passes/Visibility/DepthRaster.hlsli"
//   void main(DepthRasterPixel p) { if (!depthRasterCovered(p)) discard; ... }
//
// Root constants 0..15 belong to V, 16..31 to the requester (DepthRasterRequest::pixelConstants -> P[4..7]).
#ifndef UNX_DEPTH_RASTER_HLSLI
#define UNX_DEPTH_RASTER_HLSLI
#include "Bindless.hlsli"
#include "Scene.hlsli"
#include "Passes/Visibility/AlphaTest.hlsli"

struct DepthRasterPixel
{
    float4 position : SV_Position;           // viewport pixel centre, z = device depth of the view
    float2 uv : TEXCOORD0;                   // material uv (alpha test)
    nointerpolation uint userData : USERDATA; // RasterView::userData of the view being rasterised
    nointerpolation uint material : MATERIAL; // scene material of the triangle (instance overrides applied)
    nointerpolation uint instance : INSTANCE; // scene instance of the triangle (loadInstance)
};

// False where an alpha-tested material is transparent (baseColor texture alpha < alphaCutoff), as in the main view and
// the reference (INTERFACES_KO.md 8.1).
bool depthRasterCovered(DepthRasterPixel p) { return alphaTestCovered(p.material, p.uv); }

#endif
