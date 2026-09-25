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
#include "Passes/Visibility/Coverage.hlsli"
#include "Passes/Material/MaterialTextures.hlsli"

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

// Perspective-correct uv at screen point s of triangle (a, b, c) (xy pixels, w = 1/w).
float2 perspectiveUv(float4 a, float4 b, float4 c, float2 ta, float2 tb, float2 tc, float2 s)
{
    const float3 w = coverageBarycentric(a.xy, b.xy, c.xy, s);
    const float3 q = w * float3(a.w, b.w, c.w);
    return (ta * q.x + tb * q.y + tc * q.z) / max(q.x + q.y + q.z, 1e-30);
}

void main(float4 position : SV_Position, nointerpolation uint visId : VISID, nointerpolation uint flags : COVFLAGS, nointerpolation uint material : MATERIAL,
          nointerpolation float4 a : TRIA, nointerpolation float4 b : TRIB, nointerpolation float4 c : TRIC, nointerpolation float4 d : TRID,
          nointerpolation float4 tab : UVAB, nointerpolation float4 tcd : UVCD)
{
    if (IsHelperLane()) return;  // no derivatives are taken (explicit gradients)
    const uint2 pixel = uint2(position.xy);
    if (pixel.x >= COV_WIDTH || pixel.y >= COV_HEIGHT) return;
    const bool quad = (flags & COV_FLAG_QUAD) != 0;
    float2 mid;
    float area = coverageTriangleAreaCentroid(a.xy, b.xy, c.xy, float2(pixel), mid);
    if (quad)
    {
        float2 c2;
        const float area2 = coverageTriangleAreaCentroid(a.xy, c.xy, d.xy, float2(pixel), c2);
        if (area2 > 0) mid = (mid * area + c2 * area2) / (area + area2);
        area += area2;
    }
    if (!(area > 0)) return;
    // The plane's affine functions from the better-conditioned of the two triangles.
    const bool useSecond = quad && abs(cross(float3(c.xy - a.xy, 0), float3(d.xy - a.xy, 0)).z) > abs(cross(float3(b.xy - a.xy, 0), float3(c.xy - a.xy, 0)).z);
    const float4 pa = a, pb = useSecond ? c : b, pc = useSecond ? d : c;
    const float2 ua = tab.xy, ub = useSecond ? tcd.xy : tab.zw, uc = useSecond ? tcd.zw : tcd.xy;
    const float depth = dot(coverageBarycentric(pa.xy, pb.xy, pc.xy, mid), float3(pa.z, pb.z, pc.z));
    if (!coverageAboveBandA(pixel, depth)) return;
    uint mask = coverageTriangleMask(a.xy, b.xy, c.xy, float2(pixel));
    if (quad) mask |= coverageTriangleMask(a.xy, c.xy, d.xy, float2(pixel));
    if ((flags & COV_FLAG_ALPHA) != 0)
    {
        const GpuMaterial m = loadMaterial(material);
        const float2 uv0 = perspectiveUv(pa, pb, pc, ua, ub, uc, mid);
        const float footprint = 0.17677670;  // 1 / sqrt(COVERAGE_SAMPLES)
        const float2 duvdx = (perspectiveUv(pa, pb, pc, ua, ub, uc, mid + float2(1, 0)) - uv0) * footprint;
        const float2 duvdy = (perspectiveUv(pa, pb, pc, ua, ub, uc, mid + float2(0, 1)) - uv0) * footprint;
        const uint before = countbits(mask);
        if (before == 0)
        {
            if (materialBaseColorGrad(m, uv0, duvdx, duvdy).a < m.alphaCutoff) return;
        }
        else
        {
            uint pass = mask;
            for (uint bits = mask; bits != 0; bits &= bits - 1)
            {
                const uint i = firstbitlow(bits);
                const float2 uv = perspectiveUv(pa, pb, pc, ua, ub, uc, float2(pixel) + coverageSample(i));
                if (materialBaseColorGrad(m, uv, duvdx, duvdy).a < m.alphaCutoff) pass &= ~(1u << i);
            }
            if (pass == 0) return;
            area *= (float)countbits(pass) / (float)before;
            mask = pass;
        }
    }

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
