// unx-kernel: ps_6_6 main
// Coverage layer pixel kernel (band B, CoverageLayer.hlsli), conservative rasterisation, no render target or depth:
//  1. the exact area of the primitive's polygon (triangle, or the near-clipped quad as two triangles) inside this pixel
//     and the covered region's centroid (Coverage.hlsli); zero area (conservative raster's touching pixels) writes
//     nothing;
//  2. depth at the centroid (z/w is affine in screen space over the plane of the triangle);
//  3. band A occlusion: dropped when every band A surface over the pixel's 3 x 3 neighbourhood is nearer (HiZ mip 0:
//     farthest depth of 2 x 2 blocks). A band A edge pixel keeps its band B fragments (the E composite needs them);
//  4. the 32-subsample mask; alpha-tested materials clear the subsamples whose texture alpha fails and scale the area by
//     the passing fraction. The uv is perspective-correct (uv/w and 1/w are affine in screen space) and the texture
//     footprint is one subsample's cell (the pixel's gradients x 1/sqrt(32)): 32 samples of the alpha estimate its
//     coverage inside the pixel without the pixel-wide blur a pixel footprint would threshold. A polygon covering no
//     subsample takes the test at its centroid with the same footprint for its whole area;
//  5. wave-aggregated append of the raw fragment (20 B), linked to the pixel's previous newest fragment through the
//     heads (atomic exchange); the pixel's first fragment puts it on the pixel list.
#include "Passes/Visibility/CoverageLayer.hlsli"
#include "Passes/Visibility/CoveragePolygon.hlsli"

bool coverageAboveBandA(uint2 pixel, float depth)
{
    if (COV_HIZ == UNX_NONE) return true;
    Texture2D<float> hiz = ResourceDescriptorHeap[COV_HIZ];
    const uint2 size = COV_HIZ_SIZE;
    const uint2 t0 = min(uint2(max(int2(pixel) - 1, 0)) >> 1, size - 1), t1 = min((pixel + 1) >> 1, size - 1);
    float farthest = 1;
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        const uint2 t = uint2((k & 1) ? t1.x : t0.x, (k & 2) ? t1.y : t0.y);
        farthest = min(farthest, hiz.Load(int3(t, 0)));
    }
    return depth >= farthest;  // reversed Z: nearer = larger
}

void main(float4 position : SV_Position, nointerpolation uint visId : VISID, nointerpolation uint flags : COVFLAGS, nointerpolation uint material : MATERIAL,
          nointerpolation float4 a : TRIA, nointerpolation float4 b : TRIB, nointerpolation float4 c : TRIC, nointerpolation float4 d : TRID,
          nointerpolation float4 tab : UVAB, nointerpolation float4 tcd : UVCD)
{
    if (IsHelperLane()) return;  // no derivatives are taken (explicit gradients)
    const uint2 pixel = uint2(position.xy);
    if (pixel.x >= COV_WIDTH || pixel.y >= COV_HEIGHT) return;
    CoveragePolygon poly;
    poly.a = a;
    poly.b = b;
    poly.c = c;
    poly.d = d;
    poly.ta = tab.xy;
    poly.tb = tab.zw;
    poly.tc = tcd.xy;
    poly.td = tcd.zw;
    poly.quad = (flags & COV_FLAG_QUAD) != 0;
    float2 mid;
    CoverageSample cs = coveragePolygonGeometry(poly, float2(pixel), mid);
    if (!(cs.area > 0)) return;
    if (!coverageAboveBandA(pixel, cs.depth)) return;
    cs.mask = coveragePolygonMask(poly, float2(pixel));
    if ((flags & COV_FLAG_ALPHA) != 0)
    {
        coveragePolygonAlpha(poly, material, float2(pixel), mid, cs);
        if (!(cs.area > 0)) return;
    }
    const float depth = cs.depth, area = cs.area;
    const uint mask = cs.mask;

    RWByteAddressBuffer state = ResourceDescriptorHeap[COV_STATE];
    const uint index = waveAppend(state, VS_COV_FRAGMENTS, 1, COV_CAP_FRAGMENTS, OVERFLOW_COVERAGE);
    uint previous = 1;  // "not the pixel's first fragment" for lanes whose fragment did not fit
    if (index < COV_CAP_FRAGMENTS)
    {
        RWTexture2D<uint> heads = ResourceDescriptorHeap[COV_HEADS];
        InterlockedExchange(heads[pixel], index + 1, previous);
        RWStructuredBuffer<CoverageRawFragment> raw = ResourceDescriptorHeap[COV_RAW];
        CoverageRawFragment f;
        f.visId = visId;
        f.depth = depth;
        f.mask = mask;
        f.area = area;
        f.next = previous;
        raw[index] = f;
    }
    const uint slot = waveAppend(state, VS_COV_PIXELS, previous == 0 ? 1 : 0, COV_CAP_PIXELS, OVERFLOW_COVERAGE);
    if (previous == 0 && slot < COV_CAP_PIXELS)
    {
        RWByteAddressBuffer pixels = ResourceDescriptorHeap[COV_PIXELS];
        pixels.Store(4 * (COV_PIXEL_LIST + slot), coveragePack(pixel));
    }
}
