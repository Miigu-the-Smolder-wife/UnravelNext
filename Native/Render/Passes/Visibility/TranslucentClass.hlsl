// unx-kernel: cs_6_6 main
// unx-variants: MODE=0,1
// MODE=0: clears the count (P[0].w: the count UAV) before the count raster.
// MODE=1: the A6 translucent layer class per pixel (v1.67; A's rule: the nearest translucent surface is one sample per pixel, and
// only edge pixels and pixels where translucent surfaces overlap go to coverage records):
//   0  no translucent surface in front of band A at the pixel centre nor at any centre of its 3 x 3 block;
//   1  the layer's sample (translucentVis, translucentDepth) is the pixel's only translucent surface and covers the whole
//      pixel: count 1, and every pixel of its 3 x 3 block (clamped) has count 1 and the same surface (instance and
//      material) with the depth not bending (second difference along x and y within 1e-3 of the depth);
//   2  the pixel's translucent surfaces come from coverage records (exact areas, masks and depths; CoverageRaster over
//      the translucent lists keeps only these pixels): count >= 2, or an edge by the rule above (an outline, a seam
//      between surfaces, a band A occluder's edge, a fold, a partly covering surface behind the nearest one), including
//      a pixel with count 0 next to one with a surface (an outline's outer side: the surface may cover part of the pixel
//      without its centre; the sample there is none).
//   P[0] count SRV (Texture2D<uint>), vis SRV (Texture2D<uint>), depth SRV (Texture2D<float>, linear, +inf = none),
//        class UAV (RWTexture2D<uint>, R8_UINT)
//   P[1] visible clusters SRV (uint2), width, height, unused
#include "Bindless.hlsli"
#include "Scene.hlsli"
#include "VisBuffer.hlsli"

uint2 translucentSurface(uint visId)
{
    StructuredBuffer<uint2> visible = ResourceDescriptorHeap[P[1].x];
    const uint2 e = visible[visVisibleCluster(visId)];
    return uint2(e.x, clusterMaterial(loadInstance(e.x), loadCluster(e.y & 0xFFFFFFu)));
}

[numthreads(8, 8, 1)]
void main(uint2 pixel : SV_DispatchThreadID)
{
    const uint width = P[1].y, height = P[1].z;
    if (pixel.x >= width || pixel.y >= height) return;
#if MODE == 0
    RWTexture2D<uint> clear = ResourceDescriptorHeap[P[0].w];
    clear[pixel] = 0;
#else
    Texture2D<uint> counts = ResourceDescriptorHeap[P[0].x];
    Texture2D<uint> vis = ResourceDescriptorHeap[P[0].y];
    Texture2D<float> depth = ResourceDescriptorHeap[P[0].z];
    RWTexture2D<uint> cls = ResourceDescriptorHeap[P[0].w];
    const uint count = counts[pixel], self = vis[pixel];
    const int2 hi = int2(width, height) - 1;
    if (count == 0)
    {
        bool near = false;
        for (int y = -1; y <= 1; ++y)
            for (int x = -1; x <= 1; ++x) near = near || counts[clamp(int2(pixel) + int2(x, y), 0, hi)] != 0;
        cls[pixel] = near ? 2u : 0u;
        return;
    }
    bool records = count >= 2 || self == VIS_NONE;
    if (!records)
    {
        const uint2 surface = translucentSurface(self);
        float d[3][3] = { { 0, 0, 0 }, { 0, 0, 0 }, { 0, 0, 0 } };
        for (int y = -1; y <= 1 && !records; ++y)
            for (int x = -1; x <= 1; ++x)
            {
                const int2 q = clamp(int2(pixel) + int2(x, y), 0, hi);
                const uint v = vis[q];
                if (counts[q] != 1 || v == VIS_NONE || (v != self && any(translucentSurface(v) != surface)))
                {
                    records = true;
                    break;
                }
                d[y + 1][x + 1] = depth[q];
            }
        if (!records)
        {
            const float c = d[1][1];
            records = abs(d[1][0] + d[1][2] - 2 * c) > 1e-3 * c || abs(d[0][1] + d[2][1] - 2 * c) > 1e-3 * c;
        }
    }
    cls[pixel] = records ? 2u : 1u;
#endif
}
