// unx-kernel: cs_6_6 main
// Glass over V's translucent layer (A10, FEATURES_GAME 14.1; INTERFACES v1.67 translucentVis / translucentClass): the
// pixels whose one translucent surface covers them whole (class 1). Owner: M.
//   out = (R_p L_refl + sun specular) x exposure + T_p x behind
// behind = the pixel's composited exposed radiance under the glass (band A shading and the coverage records behind it).
// Thin pane (two-sided): R_p = F + (1 - F)^2 F t^2 / (1 - F^2 t^2), T_p = (1 - F)^2 t / (1 - F^2 t^2), F the exact
// unpolarised dielectric Fresnel (shDielectricFresnel), t the tint per pass; the transmitted direction is unchanged (a pane
// has no thickness in the geometry: exact for smooth panes). Solid bodies (one-sided) refract twice; until R's
// refraction rays (request R-2) they are drawn with the pane's weights and a straight path (counted in the statistics as
// solid glass without refraction). L_refl: the GI cache's radiance of the mirror lobe until R's reflection jobs for the
// translucent layer (R-1). Air: the behind value holds the air in front of it; the glass terms take S's air at the
// glass's depth. Class 2 pixels (records) go through the coverage composite (coverageSpecial, v1.73).
// P[0] = { translucentVis, translucentClass, colour (exposed linear, read-write), statistics (raw; UNX_NONE: none) }
// P[1] = { visible clusters, M texture table, R's GI cache (UNX_NONE: none), 0 }
// P[2] = { atmosphere transmittance, multi-scatter, air volume, 0 } (UNX_NONE: none)
// P[3] = { VSM page table, blocks, search bound, VSM constants CBV } (UNX_NONE page table: no sun shadow)
// P[4] = { VSM transmittance layers, 0, 0, 0 }
#include "Bindless.hlsli"
#include "Passes/Common/VisBuffer.hlsli"
#include "Passes/Material/MaterialInternal.hlsli"
#include "Passes/Material/MaterialSurface.hlsli"
#include "Passes/Shading/ShadingCommon.hlsli"
#include "Passes/Atmosphere/Atmosphere.hlsli"
#include "Passes/Shadow/ShadowVisibility.hlsli"
#include "Passes/GI/GiCache.hlsli"
#include "Passes/Reflection/Reflection.hlsli"

#define TRANSLUCENT_STAT_PANE 0u    // pane pixels composited
#define TRANSLUCENT_STAT_SOLID 1u   // solid glass pixels drawn without refraction (awaiting R-2)
#define TRANSLUCENT_STAT_UNLIT 2u   // pixels whose sun visibility had no resident page (lit: awaiting S's marking)

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint2 pixel = id.xy;
    if (any(pixel >= uint2(g_viewWidth, g_viewHeight))) return;
    Texture2D<uint> classes = ResourceDescriptorHeap[P[0].y];
    if (classes[pixel] != 1u) return;
    Texture2D<uint> vis = ResourceDescriptorHeap[P[0].x];
    const uint visId = vis[pixel];
    if (visId == VIS_NONE) return;

    const float2 centre = float2(pixel) + 0.5;
    const MTriangleIdentity tri = mTriangleIdentity(visId, P[1].x);
    const MVertex v0 = mTriangleVertex(visId, P[1].x, 0), v1 = mTriangleVertex(visId, P[1].x, 1), v2 = mTriangleVertex(visId, P[1].x, 2);
    const MSurface sf = mSurfaceFromVertices(tri, v0, v1, v2, centre);
    const GpuMaterial m = loadMaterial(sf.material);
    if (materialClass(m) != MATERIAL_GLASS) return;  // (water: W's pass)
    const MTextureSet ts = mLoadTextureSet(P[1].y, sf.material);
    float3 tint = m.baseColor;
    if (ts.baseColor != UNX_NONE)
    {
        Texture2D<float4> t = ResourceDescriptorHeap[ts.baseColor];
        tint *= mSampleGrad(t, (ts.flags & M_TEX_BASE_COLOR) != 0, sf.uv, sf.duvdx, sf.duvdy).rgb;
    }
    float roughness = m.roughness;
    if (ts.roughMetal != UNX_NONE)
    {
        Texture2D<float4> t = ResourceDescriptorHeap[ts.roughMetal];
        roughness *= mSampleGrad(t, (ts.flags & M_TEX_ROUGH_METAL) != 0, sf.uv, sf.duvdx, sf.duvdy).x;
    }
    float variance = (dot(sf.dndx, sf.dndx) + dot(sf.dndy, sf.dndy)) / 12.0;
    float3 n;
    if (ts.moments != UNX_NONE)
    {
        Texture2D<float4> t = ResourceDescriptorHeap[ts.moments];
        const MSlopeMoments mm = mNormalMoments(t, sf.uv, sf.duvdx, sf.duvdy, ts.slopeRange, (ts.flags & M_TEX_NORMAL) != 0);
        const float3 B = sf.tangentSign * cross(sf.normal, sf.tangent);
        n = normalize(sf.tangent * mm.mean.x + B * mm.mean.y + sf.normal);
        variance += mm.variance;
    }
    else n = normalize(sf.normal);
    const bool pane = (m.classFlags & MATERIAL_TWO_SIDED) != 0;
    const bool backSide = !sf.front && pane;
    if (backSide) n = -n;
    n = mNormalOnGeometricSide(n, backSide ? -sf.geometricNormal : sf.geometricNormal);
    if (sf.front || backSide) n = mNormalTowardsViewer(n, sf.view);
    const float alpha = sqrt(max(roughness * roughness, 1e-4) * max(roughness * roughness, 1e-4) + variance);  // band-limited
    const float r = min(sqrt(alpha), 1.0);

    const float3 v = sf.view;
    const float NoV = saturate(dot(n, v));
    const float F = shDielectricFresnel(NoV, 1.0 / max(m.ior, 1.0001));
    const float3 t = saturate(tint);
    const float3 denom = 1 - F * F * t * t;
    const float3 Rp = F + (1 - F) * (1 - F) * F * t * t / denom;
    const float3 Tp = (1 - F) * (1 - F) * t / denom;

    // reflected light: the sun's disk through the GGX lobe (F = 1 there, the pane's R_p at the mirror direction: exact as the
    // lobe narrows), the GI cache's radiance of the mirror lobe
    const float3 offset = sf.offset;
    const float3 worldPos = g_cameraPosition + offset;
    const float linearZ = dot(offset, -g_view[2].xyz);
    float3 D, Dx, Dy;
    mPixelRay(centre, D, Dx, Dy);
    AtmosphereSrvs atm;
    atm.transmittance = P[2].x;
    atm.multiScatter = P[2].y;
    atm.skyView = UNX_NONE;
    atm.aerial = P[2].z;
    float3 E = g_sunIlluminance * g_sunColor, airInscatter = 0, airTransmittance = 1;
    if (atm.transmittance != UNX_NONE)
    {
        if (atm.aerial != UNX_NONE) atmosphereAirView(atm, centre / float2(g_viewWidth, g_viewHeight), linearZ, airInscatter, airTransmittance, E);
        else E = atmosphereSunIlluminance(atm, worldPos);
    }
    float sunVisibility = 1;
    uint stat = pane ? TRANSLUCENT_STAT_PANE : TRANSLUCENT_STAT_SOLID;
    if (P[3].x != UNX_NONE)
    {
        ShadowSrvs sh;
        sh.pageTable = P[3].x;
        sh.pool = UNX_NONE;
        sh.blocks = P[3].y;
        sh.searchBound = P[3].z;
        sh.constants = P[3].w;
        sh.lights = UNX_NONE;
        sh.pad0 = UNX_NONE;
        sh.layers = P[4].x;
        bool resident;
        sunVisibility = shadowSunVisibilityAt(sh, worldPos, sf.geometricNormal * (backSide ? -1.0 : 1.0), linearZ * shPixelAngle(D, Dx), resident);
        if (!resident) sunVisibility = 1;
        if (!resident && P[0].w != UNX_NONE)
        {
            RWByteAddressBuffer stats = ResourceDescriptorHeap[P[0].w];
            stats.InterlockedAdd(4 * TRANSLUCENT_STAT_UNLIT, 1);
        }
    }
    const float3 l0 = normalize(g_sunDirection);
    float3 radiance = 0;
    if (sunVisibility > 0 && dot(n, l0) > 0)
        radiance += shSunSpecular(1.0.xxx, r, max(r * r, 1e-4), 1.0.xxx, n, v, max(NoV, 1e-4), l0, E, shPixelAngle(D, Dx)) * Rp * sunVisibility;
    if (P[1].z != UNX_NONE)
    {
        GiSrvs gi;
        gi.cache = P[1].z;
        gi.hash = P[1].z;
        gi.pad0 = gi.pad1 = 0;
        radiance += Rp * giCacheRadiance(gi, worldPos, n, reflect(-v, n), reflectionLobeHalfAngle(r, NoV));
    }
    radiance = radiance * airTransmittance;  // (the in-scatter in front of the glass is inside the behind value)

    RWTexture2D<float4> colour = ResourceDescriptorHeap[P[0].z];
    const float3 behind = colour[pixel].rgb;
    colour[pixel] = float4(radiance * g_exposure + Tp * behind, 1);
    if (P[0].w != UNX_NONE)
    {
        RWByteAddressBuffer stats = ResourceDescriptorHeap[P[0].w];
        stats.InterlockedAdd(4 * stat, 1);
    }
}
