// unx-kernel: ps_6_6 main
// V test kernel: the depth raster service's coverage-mode pixel contract compiles (DepthRaster.hlsli with
// DEPTH_RASTER_COVERAGE 1). Writes area, mask and depth of each texel to a raw UAV (P[4].x, row pitch P[4].y words).
#define DEPTH_RASTER_COVERAGE 1
#include "Passes/Visibility/DepthRaster.hlsli"

void main(DepthRasterPixel p)
{
    const DepthRasterCoverage c = depthRasterCoverage(p);
    if (!(c.area > 0)) return;
    RWByteAddressBuffer dst = ResourceDescriptorHeap[P[4].x];
    const uint2 t = uint2(p.position.xy);
    dst.Store3(12 * (t.y * P[4].y + t.x), uint3(asuint(c.area), c.mask, asuint(c.depth)));
}
