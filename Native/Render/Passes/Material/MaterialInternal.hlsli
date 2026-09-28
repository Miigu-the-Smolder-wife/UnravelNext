// M-internal per-pixel formats and tables (read only by M's kernels: Passes/Material, Passes/Shading). Owner: M.
//
//   material word  R32_UINT per pixel, written by the resolve:
//                    bits  0..15  scene material index (M_MATERIAL_SKY = no surface)
//                    bits 16..23  metallic unorm8 (material x roughMetal map)
//                    bits 24..31  reserved (0)
//   emissive       RGBA16F per pixel, written only for materials with an emissive texture (nits, before exposure)
//   tile lists     raw buffer: for shade class c, tiles[c * tileCount + i] = tile x | tile y << 16, i < count(c)
//   tile args      raw buffer: for shade class c, D3D12_DISPATCH_ARGUMENTS at byte 12 c = (count(c), 1, 1)
//   tile flags     raw buffer, one uint per tile: 0 after the resolve; the shading kernels set bit 0 when the tile has
//                  an edge pixel, and the first to set it appends the tile to the edge list (ShadingSystem.cpp)
//   word bit 24..  reserved (0)
//   texture table  StructuredBuffer<MTextureSet>, one entry per scene material (TextureSystem.cpp)
#ifndef UNX_M_MATERIAL_INTERNAL_HLSLI
#define UNX_M_MATERIAL_INTERNAL_HLSLI
#include "Bindless.hlsli"
#include "Scene.hlsli"

#define M_MATERIAL_SKY 0xFFFFu
#define M_TILE 8u

// Shade classes: one tile list and one shading kernel each (ARCHITECTURE 2.11: tile classification, no uber kernel).
// Order = bit in the tile class mask; MaterialSystem.h ShadeClass mirrors it.
#define M_CLASS_SKY 0u
#define M_CLASS_OPAQUE 1u      // Standard and Foliage (INTERFACES 8.1)
#define M_CLASS_SUBSURFACE 2u
#define M_CLASS_WATER 3u
#define M_CLASS_LAYERED 4u     // A9: Standard materials with a clearcoat (MATERIAL_LAYERED): ShadeOpaque LAYERED=1
#define M_CLASS_SHEEN 5u       // A9: Standard materials with a sheen (MATERIAL_LAYERED | MATERIAL_SHEEN): ShadeOpaque LAYERED=2
#define M_CLASS_COUNT 6u

uint mShadeClass(uint materialClass)
{
    if (materialClass == MATERIAL_SUBSURFACE) return M_CLASS_SUBSURFACE;
    if (materialClass == MATERIAL_WATER) return M_CLASS_WATER;
    return M_CLASS_OPAQUE;
}

// The material's shade class (A9: a layered Standard material shades in the layered class).
uint mShadeClassOf(GpuMaterial m)
{
    if ((m.classFlags & MATERIAL_LAYERED) != 0 && materialClass(m) == MATERIAL_STANDARD) return (m.classFlags & MATERIAL_SHEEN) != 0 ? M_CLASS_SHEEN : M_CLASS_LAYERED;
    return mShadeClass(materialClass(m));
}

// Material word: material index (bits 0..15), metallic (16..23), A9 the coat's footprint-filtered perceptual roughness
// (24..31; MATERIAL_LAYERS 3.4: alpha_c'^2 = alpha_c^2 + the footprint's slope variance, the base's rule).
uint mPackMaterialWord(uint material, float metallic, float coatRoughness = 0)
{
    return (material & 0xFFFFu) | (uint(round(saturate(metallic) * 255.0)) << 16) | (uint(round(saturate(coatRoughness) * 255.0)) << 24);
}
float mWordCoatRoughness(uint word) { return (word >> 24) / 255.0; }
uint mWordMaterial(uint word) { return word & 0xFFFFu; }
float mWordMetallic(uint word) { return ((word >> 16) & 0xFFu) / 255.0; }

// Scene texture set of a material (TextureSystem.cpp packs it; 32 B). Texture fields are bindless SRV indices or UNX_NONE.
//   baseColor   RGBA8 sRGB view (alpha = coverage), mips alpha-weighted, coverage-preserving alpha for alpha-tested use
//   moments     RGBA16_UNORM normal-map slope moments (LEAN family), see mNormalMoments
//   roughMetal  RG8_UNORM: r = perceptual roughness factor, g = metallic factor
//   emissive    RGBA8 sRGB or RGBA16F: multiplies the material's emissive (nits)
//   slopeRange  S of the moments encoding (max |slope| of the source map)
//   flags       clamp addressing per texture (M_TEX_* bits = gpu::MaterialTextureBit)
//   coverage    R8_UNORM cut-out coverage of the base colour for alpha-tested materials (fraction of base texels whose
//               alpha passes the cutoff, box mips), UNX_NONE otherwise
struct MTextureSet
{
    uint baseColor, moments, roughMetal, emissive;
    uint occlusion;
    float slopeRange;
    uint flags, coverage;
};

// Scale of M's texture footprints: the output pixel's share of the view's pixel (g_upscaleRatio: an upscaled main view's
// internal / output height; 1 when the view renders at its output resolution). The upscale reconstructs output pixels
// from jittered internal samples, so the footprint of an output pixel gives the native resolution's texture detail.
float mFootprintScale() { return g_upscaleRatio > 0 ? g_upscaleRatio : 1.0; }
float4 mSampleGradRaw(Texture2D<float4> t, bool clampAddress, float2 uv, float2 duvdx, float2 duvdy)
{
    return clampAddress ? t.SampleGrad(g_anisoClamp, uv, duvdx, duvdy) : t.SampleGrad(g_anisoWrap, uv, duvdx, duvdy);
}
// Footprint-filtered sample with the texture's addressing (MTextureSet.flags / gpu::Material.textureClamp bit); duvdx /
// duvdy per view pixel (mFootprintScale applied here).
float4 mSampleGrad(Texture2D<float4> t, bool clampAddress, float2 uv, float2 duvdx, float2 duvdy)
{
    const float k = mFootprintScale();
    return mSampleGradRaw(t, clampAddress, uv, duvdx * k, duvdy * k);
}

#define M_TEX_BASE_COLOR 1u
#define M_TEX_NORMAL 2u
#define M_TEX_ROUGH_METAL 4u
#define M_TEX_EMISSIVE 8u

MTextureSet mLoadTextureSet(uint tableSrv, uint material)
{
    StructuredBuffer<MTextureSet> t = ResourceDescriptorHeap[tableSrv];
    return t[material];
}

// Normal-map slope moments at a footprint (LEAN family, Olano & Baker 2010). Texel k of mip L stores, over the base
// texels it covers, the mean slope mu = E[(x/z, y/z)], the internal variance trace E|s|^2 - |mu|^2 and |mu|^2, encoded
// as unorm16: mu -> (mu / S + 1) / 2, internal trace -> v / (2 S^2), |mu|^2 -> q / (2 S^2).
// Minified (footprint >= 2 base texels along its major axis, rho): the hardware filter averages the channels over the
// footprint, so its slope variance trace is V = sum w v + (sum w q - |sum w mu|^2), exact for the filter's weights.
// Magnified (rho <= 1): the footprint lies inside one bilinear cell of mip 0, where the normal field is the bilinear
// interpolant; its variance over the pixel box is (|J tx|^2 + |J ty|^2) / 12 (J = the interpolant's slope Jacobian per
// texel from the cell's four texels, tx / ty = the pixel's steps in texels). The LEAN between-texel term would instead
// add w(1 - w)|d mu|^2, the texel-grid pattern of a coarser footprint. Between 1 and 2 texels the two blend linearly.
struct MSlopeMoments
{
    float2 mean;     // mean slope (tangent space)
    float variance;  // trace of the slope covariance over the footprint
};

MSlopeMoments mNormalMoments(Texture2D<float4> t, float2 uv, float2 duvdx, float2 duvdy, float S, bool clampAddress)
{
    duvdx *= mFootprintScale();  // (per view pixel in, over the output pixel from here: mSampleGrad)
    duvdy *= mFootprintScale();
    const float4 m = mSampleGradRaw(t, clampAddress, uv, duvdx, duvdy);
    uint w, h;
    t.GetDimensions(w, h);
    const float2 size = float2(w, h);
    const float2 tx = duvdx * size, ty = duvdy * size;
    const float rho = sqrt(max(dot(tx, tx), dot(ty, ty)));
    MSlopeMoments r;
    r.mean = (m.xy * 2.0 - 1.0) * S;
    const float S2 = 2.0 * S * S;
    const float inner = m.z * S2;
    float between = 0;
    if (rho > 1.0)
    {
        // unorm16 rounding bounds the error of q (S^2 / 65535) and of |mu|^2 (4 S^2 / 65535): subtract it so rounding
        // never shows up as variance.
        between = max(m.w * S2 - dot(r.mean, r.mean) - 5.0 * S * S / 65535.0, 0.0);
    }
    if (rho < 2.0)
    {
        // The bilinear cell around uv on mip 0, chosen here (not by Gather, whose fixed-point cell choice near a texel
        // boundary can differ from the fraction computed in float) and read with wrapped loads.
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
            i1 = int2(uint2(i0 + 1) % uint2(w, h));
        }
        const float2 m00 = (t.Load(int3(i0.x, i0.y, 0)).xy * 2.0 - 1.0) * S, m10 = (t.Load(int3(i1.x, i0.y, 0)).xy * 2.0 - 1.0) * S;
        const float2 m01 = (t.Load(int3(i0.x, i1.y, 0)).xy * 2.0 - 1.0) * S, m11 = (t.Load(int3(i1.x, i1.y, 0)).xy * 2.0 - 1.0) * S;
        const float2 dxu = lerp(m10 - m00, m11 - m01, f.y);  // d mu / du (per texel)
        const float2 dxv = lerp(m01 - m00, m11 - m10, f.x);  // d mu / dv
        const float2 jx = dxu * tx.x + dxv * tx.y, jy = dxu * ty.x + dxv * ty.y;
        const float gradient = (dot(jx, jx) + dot(jy, jy)) / 12.0;
        between = lerp(gradient, between, saturate(rho - 1.0));
    }
    r.variance = inner + between;
    return r;
}

// The reference's shading-normal rules (Reference/PathTracer/src/RtScene.cpp; INTERFACES 8.1, v1.65). A mapped normal
// stays on the geometric side of the surface it belongs to (mirrored across the triangle's plane; ng: the geometric
// normal of the side being shaded). On a surface seen from its shaded side (front, or a two-sided material's back), a
// normal facing away from the viewer is bent towards it just enough for n.v = 1e-4: the BRDF is defined for n.v > 0 only,
// and without the bend such texels (normal-mapped detail at grazing views) reflected no light at all (U2's black dots).
float3 mNormalOnGeometricSide(float3 n, float3 ng)
{
    const float g = dot(n, ng);
    return g < 0 ? n - ng * (2 * g) : n;
}
float3 mNormalTowardsViewer(float3 n, float3 v)
{
    const float nv = dot(n, v);
    return nv < 1e-4 ? normalize(n + v * (1e-4 - nv)) : n;
}

#endif
