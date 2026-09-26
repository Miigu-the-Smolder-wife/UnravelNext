// unx-kernel: cs_6_6 main
// unx-variants: JOBS=0,1
// Glass over V's translucent layer (A10, FEATURES_GAME 14.1; INTERFACES v1.67 translucentVis / translucentClass): the
// pixels whose one translucent surface covers them whole (class 1). Owner: M.
//   out = (R_p L_refl + sun specular) x exposure + T_p x behind
// behind = the pixel's composited exposed radiance under the glass (band A shading and the coverage records behind it).
// Thin pane (two-sided): R_p = F + (1 - F)^2 F t^2 / (1 - F^2 t^2), T_p = (1 - F)^2 t / (1 - F^2 t^2), F the exact
// unpolarised dielectric Fresnel (shDielectricFresnel), t the tint per pass; the transmitted direction is unchanged (a pane
// has no thickness in the geometry: exact for smooth panes). L_refl: the GI cache's radiance of the mirror lobe. Air: the
// behind value holds the air in front of it; the glass terms take S's air at the glass's depth. Class 2 pixels (records)
// go through the coverage composite (coverageSpecial, v1.73).
// JOBS=1 (R's FrameServices::traceRefractions present; rows [P[4].y, P[4].z) of the view, one band per dispatch): a
// mirror-smooth pixel (band-limited r < R's reflection.mirror_roughness_max, P[4].w: the M rule, where one ray is the
// lobe) appends a reflection job (R-1: medium 0xFF, the mirror direction) and, on a solid body (one-sided), a refraction
// job (R-2: medium 1, the refracted direction, sigma_a = the body's, 2 internal reflections), and a record for
// TranslucentApply: the weights (a solid body's front face reflects F and transmits (1 - F) / n^2: R's result is the
// radiance arriving inside the body at the entry point along -direction, the exit's (1 - F) n^2 and the absorption in it), the fallbacks (the GI cache's lobe, the straight path) for a job R did not
// trace, and the air in front of the glass. Rougher solid glass keeps the pane's weights and a straight path (counted in
// the statistics as solid glass without refraction, until the A10 rough-glass gather); JOBS=0 draws every solid body so.
// P[1].w (JOBS=1) = R's results UAV (raw); P[5] = { jobs UAV (raw: FrameServices::traceRefractions' format), records UAV (raw: header 32 B, then 48 B
// records, TranslucentApply), max jobs, max records }; each job's result is cleared here (R leaves an untraced job's).
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
#if JOBS
    const uint2 pixel = uint2(id.x, id.y + P[4].y);
    if (pixel.x >= g_viewWidth || pixel.y >= min(P[4].z, g_viewHeight)) return;
#else
    const uint2 pixel = id.xy;
    if (any(pixel >= uint2(g_viewWidth, g_viewHeight))) return;
#endif
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
#if JOBS
    const bool reflectJob = r < asfloat(P[4].w);
    const bool refractJob = reflectJob && !pane && sf.front;
    const float3 Rs = refractJob ? F.xxx : Rp;  // a solid body's front face alone; a pane's two faces
#else
    const bool reflectJob = false, refractJob = false;
    const float3 Rs = Rp;
#endif

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
    uint stat = pane || refractJob ? TRANSLUCENT_STAT_PANE : TRANSLUCENT_STAT_SOLID;
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
        radiance += shSunSpecular(1.0.xxx, r, max(r * r, 1e-4), 1.0.xxx, n, v, max(NoV, 1e-4), l0, E, shPixelAngle(D, Dx)) * Rs * sunVisibility;
    float3 lobe = 0;
    if (P[1].z != UNX_NONE)
    {
        GiSrvs gi;
        gi.cache = P[1].z;
        gi.hash = P[1].z;
        gi.pad0 = gi.pad1 = 0;
        lobe = Rs * giCacheRadiance(gi, worldPos, n, reflect(-v, n), reflectionLobeHalfAngle(r, NoV));
    }
    // (the in-scatter in front of the glass is inside the behind value)
    const float3 exposed = radiance * airTransmittance * g_exposure, lobeExposed = lobe * airTransmittance * g_exposure;

    RWTexture2D<float4> colour = ResourceDescriptorHeap[P[0].z];
    const float3 behind = colour[pixel].rgb;
    bool recorded = false;
#if JOBS
    if (reflectJob)
    {
        const uint count = refractJob ? 2u : 1u;
        RWByteAddressBuffer jobs = ResourceDescriptorHeap[P[5].x];
        RWByteAddressBuffer records = ResourceDescriptorHeap[P[5].y];
        uint first, record;
        jobs.InterlockedAdd(0, count, first);
        records.InterlockedAdd(0, 1u, record);
        if (first + count <= P[5].z && record < P[5].w)  // (a band holds at most max records pixels: never short)
        {
            const float eps = 1e-3 + 2e-4 * linearZ;
            const float3 dr = reflect(-v, n);
            uint at = 16 + first * 48;
            jobs.Store4(at, uint4(asuint(worldPos + n * eps), 0));
            jobs.Store4(at + 16, uint4(asuint(dr), 0xFFu));
            jobs.Store4(at + 32, uint4(0, 0, 0, asuint(1.0)));
            if (refractJob)
            {
                const float3 dt = refract(-v, n, 1.0 / max(m.ior, 1.0001));
                at += 48;
                jobs.Store4(at, uint4(asuint(worldPos - n * eps), 0));
                jobs.Store4(at + 16, uint4(asuint(dt), 1u | (2u << 8)));
                jobs.Store4(at + 32, uint4(asuint(m.hairAbsorption), asuint(max(m.ior, 1.0001))));
            }
            RWByteAddressBuffer results = ResourceDescriptorHeap[P[1].w];
            if (refractJob) results.Store4(first * 8, uint4(0, 0, 0, 0));
            else results.Store2(first * 8, uint2(0, 0));
            const float3 wRefract = (1 - F) / (m.ior * m.ior) * airTransmittance;  // R's result arrives inside (L / n^2 kept)
            const float3 fbRefract = refractJob ? Tp * behind : 0;
            const uint rat = 32 + record * 48;
            records.Store4(rat, uint4(pixel.x | (pixel.y << 16), first | (refractJob ? 1u << 31 : 0u), f32tof16(Rs.r * airTransmittance.r) | (f32tof16(Rs.g * airTransmittance.g) << 16), f32tof16(Rs.b * airTransmittance.b)));
            records.Store4(rat + 16, uint4(f32tof16(wRefract.r) | (f32tof16(wRefract.g) << 16), f32tof16(wRefract.b), f32tof16(lobeExposed.r) | (f32tof16(lobeExposed.g) << 16), f32tof16(lobeExposed.b)));
            const float3 front = refractJob ? airInscatter * g_exposure : 0;
            records.Store4(rat + 32, uint4(f32tof16(fbRefract.r) | (f32tof16(fbRefract.g) << 16), f32tof16(fbRefract.b), f32tof16(front.r) | (f32tof16(front.g) << 16), f32tof16(front.b)));
            colour[pixel] = float4(exposed + (refractJob ? 0 : Tp * behind), 1);
            recorded = true;
        }
    }
#endif
    if (!recorded) colour[pixel] = float4(exposed + lobeExposed + Tp * behind, 1);
    if (P[0].w != UNX_NONE)
    {
        RWByteAddressBuffer stats = ResourceDescriptorHeap[P[0].w];
        stats.InterlockedAdd(4 * stat, 1);
    }
}
