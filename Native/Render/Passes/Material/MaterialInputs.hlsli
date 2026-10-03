// Material inputs in the material resolve (scene::Material's uv transform, second uv set, detail maps, height, emissive
// mask, vertex colour; the record is Scene.hlsli GpuMaterialInputs, at GpuMaterial::inputs). Owner: M.
// They belong to the classes whose textures are read at the mesh's uv (every class but Cut and Terrain). Per pixel:
//   uv         the material's own textures (base colour, normal, roughness / metallic, emissive and its mask, occlusion,
//              height) are read at uv' = M uv + t (the record's rows), with the footprint through M; a normal map's
//              slopes come back to the mesh's tangent frame through M's transpose with the scale taken out (a rotated
//              texture's bumps turn with it, a mirrored one's flip);
//   streams    the second uv set and the vertex colour of the pixel's triangle (mVertexStreams: loaded only for a
//              material that reads one), interpolated with the surface's barycentrics; a mesh without them gives its
//              uv0 and white;
//   parallax   (a height map) the view ray is marched through the height field under the surface in the texture's uv
//              (mParallax); every texture on uv set 0 is then read where the ray meets the field. The pixel's position,
//              depth and geometric normal stay the surface's. With material.parallax_shadow the field's shadow of the
//              sun at that point goes to the class word (one byte; the shading kernel multiplies the sun's visibility);
//   detail     a tiled colour and normal over the base (mDetail): the colour multiplies by lerp(1, detail x 2^2.2, w
//              strength) (neutral at sRGB 0.5), the normal's mean slope x w scale adds to the base normal in the detail
//              uv's frame - the mesh's tangent frame on uv set 0, the frame of the set's own screen derivatives on set 1 -
//              and its slope variance x (w scale)^2 joins the footprint's, as mNormalMoments gives the base map's;
//              w = 1, or the vertex colour's alpha;
//   emission   the emissive texture and the mask multiply the material's emissive (which already holds emissiveScale).
// Beyond the resolve - so that a detailed wall is the same wall under a leaf, in a mirror and in the bounce light:
//   coverage fragments  (CoverageShade.hlsli covFragmentMaterial) everything above but the occlusion map and the
//                       parallax's own shadow of the sun (a fragment has no class word);
//   ray hits            (HitShading.hlsli rtHitMaterialAt) the uv transform, the emissive mask, the vertex tint and the
//                       detail colour on its uv set (the second set from RtSurface::uv1); no detail normal (a hit shades
//                       its interpolated normal), no parallax;
//   card capture        (CardCaptureMaterial.hlsli ccMaterial) the same as hits, and the detail normal's slopes on uv
//                       set 0; no parallax (a card looks along its own axis, close to the surface's normal: the ray
//                       meets the field where it enters). The capture through the cluster hierarchy has no streams
//                       (V's raster service gives a pixel one uv): its second set is the first, its colour white.
#ifndef UNX_M_MATERIAL_INPUTS_HLSLI
#define UNX_M_MATERIAL_INPUTS_HLSLI
#include "Passes/Material/MaterialInternal.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"

// The second uv set (with its screen derivatives) and the vertex colour at a surface point.
struct MVertexStreams
{
    float2 uv1, duv1dx, duv1dy;
    float4 color;
};
MVertexStreams mVertexStreams(uint visId, uint visibleClustersSrv, MSurface s)
{
    MVertexStreams o;
    o.uv1 = s.uv;
    o.duv1dx = s.duvdx;
    o.duv1dy = s.duvdy;
    o.color = 1;
    if (g_meshAttributes == UNX_NONE) return o;
    const GpuVisibleCluster vc = loadVisibleCluster(visibleClustersSrv, visVisibleCluster(visId));
    const uint base = meshAttributeBase(loadInstance(vc.instance).mesh);
    if (base == 0) return o;
    const uint3 tri = loadClusterTriangle(loadCluster(vc.cluster), visTriangle(visId));
    float2 u0, u1, u2;
    float4 c0, c1, c2;
    loadVertexAttributes(base, tri.x, u0, c0);
    loadVertexAttributes(base, tri.y, u1, c1);
    loadVertexAttributes(base, tri.z, u2, c2);
    o.uv1 = s.bary.x * u0 + s.bary.y * u1 + s.bary.z * u2;
    o.duv1dx = s.baryDx.x * u0 + s.baryDx.y * u1 + s.baryDx.z * u2;
    o.duv1dy = s.baryDy.x * u0 + s.baryDy.y * u1 + s.baryDy.z * u2;
    o.color = s.bary.x * c0 + s.bary.y * c1 + s.bary.z * c2;
    return o;
}

// The material's uv and footprint from the mesh's (the identity for a material without a record).
struct MInputUv
{
    bool on;              // the material has a record
    GpuMaterialInputs r;  // (zero without one, its textures UNX_NONE)
    float2 uv, duvdx, duvdy;
};
MInputUv mInputUv(GpuMaterial m, float2 uv, float2 duvdx, float2 duvdy)
{
    MInputUv o;
    o.on = m.inputs != UNX_NONE;
    o.r = (GpuMaterialInputs)0;
    o.r.detailColorTexture = o.r.detailNormalTexture = o.r.heightTexture = o.r.emissiveMaskTexture = UNX_NONE;
    o.uv = uv;
    o.duvdx = duvdx;
    o.duvdy = duvdy;
    if (o.on)
    {
        o.r = loadMaterialInputs(m.inputs);
        o.uv = materialInputsUv(o.r, uv);
        o.duvdx = materialInputsUvStep(o.r, duvdx);
        o.duvdy = materialInputsUvStep(o.r, duvdy);
    }
    return o;
}
// A normal map's mean slope, measured along the texture's axes, in the mesh's tangent frame: the transform's transpose
// with each axis's scale taken out (the identity without a transform).
float2 mInputSlope(GpuMaterialInputs r, float2 slope)
{
    if ((r.flags & MATERIAL_INPUT_UV) == 0) return slope;
    const float2 c0 = float2(r.uvU.x, r.uvV.x), c1 = float2(r.uvU.y, r.uvV.y);
    return float2(dot(c0, slope) * rsqrt(max(dot(c0, c0), 1e-20)), dot(c1, slope) * rsqrt(max(dot(c1, c1), 1e-20)));
}
// The emissive mask at the material's uv (1 without one).
float mInputEmissiveMask(MInputUv i)
{
    if (i.r.emissiveMaskTexture == UNX_NONE) return 1;
    Texture2D<float4> t = ResourceDescriptorHeap[i.r.emissiveMaskTexture];
    return mSampleGrad(t, (i.r.textureClamp & 8u) != 0, i.uv, i.duvdx, i.duvdy).x;
}

// World positions' derivatives by a uv set from the pixel's screen derivatives of both (dP/du, dP/dv on the triangle's
// plane). False: the set is degenerate at the pixel.
bool mUvTangents(float3 dpdx, float3 dpdy, float2 duvdx, float2 duvdy, out float3 Pu, out float3 Pv)
{
    const float det = duvdx.x * duvdy.y - duvdx.y * duvdy.x;
    Pu = Pv = 0;
    if (!(abs(det) > 1e-30)) return false;
    Pu = (dpdx * duvdy.y - dpdy * duvdx.y) / det;
    Pv = (dpdy * duvdx.x - dpdx * duvdy.x) / det;
    return true;
}
// The uv step of a world step w in the surface (the least-squares solve of Pu a + Pv b = w).
float2 mWorldToUv(float3 w, float3 Pu, float3 Pv)
{
    const float uu = dot(Pu, Pu), vv = dot(Pv, Pv), uv = dot(Pu, Pv);
    const float det = uu * vv - uv * uv;
    if (!(det > 1e-30)) return 0;
    const float wu = dot(w, Pu), wv = dot(w, Pv);
    return float2(vv * wu - uv * wv, uu * wv - uv * wu) / det;
}

// Parallax occlusion mapping. The height field lies r.heightScale metres deep under the surface (texture 1 = the
// surface, 0 = the floor), along the shading side's interpolated normal. The view ray enters at the pixel's surface point
// and runs to the floor in 'steps' equal depth steps (material.parallax_steps: 1..64); the first step at or under the
// field is refined by one secant step between it and the step before. The ray's cosine to the normal is held above 1/8
// (at most 8 depths of travel along the surface). Each height read takes the pixel's own footprint (the level of detail
// does not change along the ray).
// 'uv' in: the material's uv at the surface point; out: where the ray meets the field. sunVisibility (shadow): 1 - 16 x
// the deepest the ray from that point towards the sun's centre runs under the field, in field depths, over max(steps /
// 2, 4) steps; 1 with the sun behind the surface or shadow = false.
void mParallax(GpuMaterialInputs r, MSurface s, inout float2 uv, float2 duvdx, float2 duvdy, uint steps, bool shadow, out float sunVisibility)
{
    sunVisibility = 1;
    float3 Pu, Pv;
    if (steps == 0 || !mUvTangents(s.dpdx, s.dpdy, duvdx, duvdy, Pu, Pv)) return;
    Texture2D<float4> heights = ResourceDescriptorHeap[r.heightTexture];
    const bool clampAddress = (r.textureClamp & 4u) != 0;
    const float3 nn = normalize(s.normal);
    const float3 N = dot(nn, s.view) < 0 ? -nn : nn;  // (the side the viewer is on)
    const float cosV = max(dot(N, s.view), 0.125);
    // the uv travelled from the surface to the floor: the ray's part along the surface per unit depth x the depth
    const float2 run = mWorldToUv((N * dot(N, s.view) - s.view) * (r.heightScale / cosV), Pu, Pv);
    float before = 0, fieldBefore = 1 - mSampleGrad(heights, clampAddress, uv, duvdx, duvdy).x;  // the field's depth under the surface point
    float hit = 0;
    if (fieldBefore > 0)
    {
        hit = 1;
        [loop] for (uint i = 1; i <= steps; ++i)
        {
            const float depth = (float)i / (float)steps;
            const float field = 1 - mSampleGrad(heights, clampAddress, uv + run * depth, duvdx, duvdy).x;
            if (depth >= field)
            {
                // the ray is above the field by a at the step before and under it by b here
                const float a = fieldBefore - before, b = depth - field;
                hit = lerp(before, depth, a / max(a + b, 1e-6));
                break;
            }
            before = depth;
            fieldBefore = field;
        }
    }
    uv += run * hit;
    const float3 l = normalize(g_sunDirection);
    const float cosL = dot(N, l);
    if (!shadow || !(cosL > 0) || !(hit > 0)) return;
    const float2 runL = mWorldToUv((l - N * cosL) * (r.heightScale / max(cosL, 0.125)), Pu, Pv);  // (per unit depth, upwards)
    const uint shadowSteps = max(steps / 2, 4u);
    float under = 0;
    [loop] for (uint j = 1; j < shadowSteps; ++j)
    {
        const float depth = hit * (1 - (float)j / (float)shadowSteps);
        const float field = 1 - mSampleGrad(heights, clampAddress, uv + runL * (hit - depth), duvdx, duvdy).x;
        under = max(under, depth - field);
    }
    sunVisibility = saturate(1 - 16 * under);
}

// The detail maps at a pixel: the base colour's factor, the world-space term to add to the unnormalised shading normal
// and the slope variance to add to the footprint's.
struct MDetail
{
    float3 colorFactor;
    float3 normalTerm;
    float variance;
};
// uv0 / streams: the mesh's first set (untransformed, with its footprint) and the pixel's streams; T, B: the mesh's
// tangent frame at the pixel (the base normal map's, unnormalised as the resolve uses them).
MDetail mDetail(GpuMaterialInputs r, MSurface s, float2 uv0, MVertexStreams streams, float3 T, float3 B)
{
    MDetail o;
    o.colorFactor = 1;
    o.normalTerm = 0;
    o.variance = 0;
    const bool set1 = (r.flags & MATERIAL_INPUT_DETAIL_UV1) != 0;
    const float2 scale = float2(r.detailScaleU, r.detailScaleV);
    const float2 uv = (set1 ? streams.uv1 : uv0) * scale + r.detailOffset;
    const float2 dx = (set1 ? streams.duv1dx : s.duvdx) * scale, dy = (set1 ? streams.duv1dy : s.duvdy) * scale;
    const float w = (r.flags & MATERIAL_INPUT_VERTEX_BLEND) != 0 ? saturate(streams.color.a) : 1.0;
    if (r.detailColorTexture != UNX_NONE)
    {
        Texture2D<float4> t = ResourceDescriptorHeap[r.detailColorTexture];
        o.colorFactor = lerp(1.0, mSampleGrad(t, (r.textureClamp & 1u) != 0, uv, dx, dy).rgb * 4.59479380, w * r.detailColor);
    }
    if (r.detailNormalTexture != UNX_NONE)
    {
        Texture2D<float4> t = ResourceDescriptorHeap[r.detailNormalTexture];
        const MSlopeMoments mm = mNormalMoments(t, uv, dx, dy, r.detailSlopeRange, (r.textureClamp & 2u) != 0);
        const float k = w * r.detailNormal;
        float3 Td = T * sign(scale.x), Bd = B * sign(scale.y);
        if (set1)
        {
            // the second set's own frame: dP/du and dP/dv of the detail uv in the plane of the interpolated normal
            const float3 N = normalize(s.normal);
            float3 Pu, Pv;
            if (mUvTangents(s.dpdx, s.dpdy, dx, dy, Pu, Pv))
            {
                Pu -= N * dot(N, Pu);
                Pv -= N * dot(N, Pv);
                Td = Pu * rsqrt(max(dot(Pu, Pu), 1e-30)) * length(s.normal);
                Bd = Pv * rsqrt(max(dot(Pv, Pv), 1e-30)) * length(s.normal);
            }
            else Td = Bd = 0;
        }
        o.normalTerm = (Td * mm.mean.x + Bd * mm.mean.y) * k;
        o.variance = mm.variance * k * k;
    }
    return o;
}

#endif
