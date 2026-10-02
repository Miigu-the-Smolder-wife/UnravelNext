// unx-kernel: lib_6_6 main
// unx-variants: SKY=0,1
// r.card.radiosity.trace (CardLighting.hlsli): one ray generation thread per trace texel of the frame's radiosity list -
// 64 of a listed tile: 2 x 2 probes (CL_PROBE_SPACING texels apart) x 4 x 4 rays over the probe's hemisphere, uniform in
// solid angle, jittered per probe and update. The probe stands on one texel of its 4 x 4, chosen by the page's temporal
// index. The ray's radiance: the final lighting atlas at the hit (clReadCardsHiRes: the cards' highest mapped level
// there, and the hit's feedback - the reference's radiosity samples the high-resolution pages; the back of a one-sided
// surface: 0), the sky where it escapes; its largest channel held to P[4].y in exposed units (the reference's
// MaxRayIntensity).
// A hit without a card (the reference: 0) takes its direct light - the sun by one shadow ray to the disk's centre and one
// local-light sample with its shadow ray (HitLocalSample.hlsli) - and the indirect light the card frame names
// (LumenHitIndirect.hlsli: the previous frame's translucency volume), through the material's constants (no texture):
// a fifth to a third of the rays' hits read no card (lobby, 2026-10-02) and every bounce lost that share.
// The re-shoot (the reference's AvoidSelfIntersections in its retrace mode, LumenHardwareRayTracingCommon.ush
// TraceSurfaceCacheRay): the ray geometry is not the captured surface texel for texel, so a ray can start under it. A
// first hit on the back of a one-sided surface nearer than P[0].w (SkipBackFaceHitDistance) is shot again from that
// distance, one on a two-sided surface nearer than P[5].y (SkipTwoSidedHitDistance) again from just past the hit; once
// (no loop). A hit nearer than P[3].w (MinTraceDistanceToSampleSurfaceCache) still blocks the ray and reads no light: at
// that distance a card texel would light itself.
// Hair (RayTracing/HitHair.hlsli; raytracing.hair): the grooms are not in the ray scene, so the ray's first fibre in E's
// density volume, where it lies before the hit, ends the ray - the groom's proxy lit by the sun's shadow ray and one
// local-light sample, as the screen probes', the radiance cache's and the volume's rays take it. The grooms the texel
// lies inside are left out (a card under the hair). A floor under a head of hair is then shadowed by it in the bounce.
// A thread traces at most 4 rays (the ray, its re-shoot, the sun's and the light sample's shadow rays - of the surface
// hit or of the groom's proxy, never both) and nothing loops around them: dispatches of at most 65,536 threads
// (CardLighting.cpp).
// The radiance goes to the trace atlas at the probe's rays' texels: tile origin + probe x 4 + ray.
// P[0] = { card frame SRV, select SRV, frame index, asuint(back-face skip distance, m; 0: no re-shoot) }
// P[1], P[2], P[3].xyz = sky and sun (GiSky.hlsli; P[1].w = ray length), P[3].w = asuint(the least hit distance that
// reads light, m)
// P[4] = { trace atlas UAV, asuint(ray intensity cap, exposed units; 0: none), the dispatch's first thread, page capacity }
// P[5] = { direct list capacity, asuint(two-sided skip distance, m), radiosity list capacity, page light SRV (raw) }
// P[6], P[7] = RtSceneSrvs
#define RT_SHADOW_TRANSMITTANCE  // (the hits' shadow rays take what the Glass they cross leaves of the light: RayShaders.hlsli)
#include "RayTracing/RayShaders.hlsli"
#include "RayTracing/HitShading.hlsli"
#include "RayTracing/HitLocalLights.hlsli"
#include "Passes/GI/GiSky.hlsli"
#include "Passes/SurfaceCache/CardLighting.hlsli"
#include "Passes/GI/LumenHitIndirect.hlsli"
#include "RayTracing/HitLocalSample.hlsli"
#include "RayTracing/HitHair.hlsli"

[shader("raygeneration")]
void CardRadiosityTraceGen()
{
    const uint thread = DispatchRaysIndex().x + P[4].z;
    const uint index = thread >> 6, t = thread & 63u;
    ByteAddressBuffer select = ResourceDescriptorHeap[P[0].y];
    if (index >= min(select.Load(clSelectContext(1) + CL_SELECT_TILES), P[5].z)) return;
    const McFrame f = mcFrame(P[0].x);
    uint pageIndex;
    uint2 tile;
    clUnpackTile(select.Load(clTileListOffset(P[4].w, P[5].x, 1, index)), pageIndex, tile);
    const McCardPage page = mcLoadCardPage(f, pageIndex);
    const uint2 inTile = uint2(t & 7u, t >> 3);
    const uint2 traceCoord = tile * MC_TILE + inTile;
    if (any(float2(traceCoord) >= page.sizeInTexels)) return;
    const McCard card = mcLoadCard(f, page.card);
    ByteAddressBuffer pageLight = ResourceDescriptorHeap[P[5].w];
    const uint temporalIndex = pageLight.Load(pageIndex * CL_PAGE_LIGHT_BYTES + 12);
    const uint2 probe = traceCoord / CL_PROBE_SPACING;
    const McTexel texel = mcPageTexel(f, page, card, probe * CL_PROBE_SPACING + clProbeTexelOffset(temporalIndex));
    RWTexture2D<float3> trace = ResourceDescriptorHeap[P[4].x];
    const uint2 out2 = uint2(page.atlasRect.xy) + traceCoord;
    trace[out2] = 0;
    if (!texel.valid) return;
    RayDesc ray;
    ray.Direction = clProbeRayDirection(texel.normal, uint2(page.atlasRect.xy) / CL_PROBE_SPACING + probe, traceCoord % CL_PROBE_RAYS, temporalIndex);
    ray.Origin = texel.position + texel.normal * (1e-3 + 2e-4 * distance(texel.position, g_cameraPosition));
    ray.TMin = 0;
    ray.TMax = giRayLength();
    const float dd = dot(ray.Direction, ray.Direction);
    if (!(all(abs(ray.Origin) < 1e9) && dd > 0.98 && dd < 1.02 && ray.TMax > 0 && ray.TMax < 1e30)) return;
    const RtSceneSrvs scene = rtScene();
    float3 radiance = 0;
    RtHit hit = rtTraceClosest(scene, ray, RAY_FLAG_NONE, RT_MASK_GI);
    // the re-shoot: past a near back face, or just past a near two-sided surface
    const float skipBackFace = asfloat(P[0].w), skipTwoSided = asfloat(P[5].y);
    if (hit.t >= 0 && hit.instance != RT_INSTANCE_EMITTER && hit.t < max(skipBackFace, skipTwoSided))
    {
        GpuInstance hitInstance;
        GpuMesh hitMesh;
        RtGeometry hitGeometry;
        const bool twoSided = (loadMaterial(rtMaterial(scene, hit, hitInstance, hitMesh, hitGeometry)).classFlags & MATERIAL_TWO_SIDED) != 0;
        float skip = -1;
        if (twoSided)
        {
            if (hit.t < skipTwoSided) skip = hit.t + 1e-4;
        }
        else if (hit.frontFace == 0 && hit.t < skipBackFace)
            skip = skipBackFace;
        if (skip > 0)
        {
            RayDesc again = ray;
            again.TMin = skip;
            hit = rtTraceClosest(scene, again, RAY_FLAG_NONE, RT_MASK_GI);
        }
    }
    // the grooms on the ray: its first fibre before the hit
    const uint hairParams = rtHairParams(scene);
    const uint hairSeed = rtHairSeed(out2, P[0].z);
    RtHairHit hair;
    hair.t = -1;
    hair.body = hair.material = 0;
    if (hairParams != 0xFFFFFFFFu) hair = rtHairFirst(hairParams, ray.Origin, ray.Direction, hit.t < 0 ? ray.TMax : hit.t, hairSeed, true);
    if (hair.t >= 0)
    {
        g_rtHitCone = 0.37;
        radiance = rtHairRadiance(scene, hairParams, hair, ray.Origin, ray.Direction, hair.t * 0.74, 1e-3 + 2e-4 * distance(ray.Origin, g_cameraPosition), hairSeed, true, true);
    }
    else if (hit.t < 0) radiance = giSkyRadiance(ray.Direction);
    else if (hit.t < asfloat(P[3].w)) radiance = 0;  // (too near to read the cache: the texel's own light)
    else if (hit.instance != RT_INSTANCE_EMITTER)  // (a light's own surface: the direct light carries it)
    {
        const RtSurface s = rtSurface(scene, hit, ray.Origin, ray.Direction);
        // (the back of a one-sided surface: no light. rtSurface turns the normals of a back-face hit toward the ray's side)
        GpuMaterial m = loadMaterial(s.material);
        if (s.frontFace || (m.classFlags & MATERIAL_TWO_SIDED) != 0)
        {
            // (the cone of a ray of the 4 x 4 hemisphere map: about 20 degrees half angle)
            const ClSample cards = clReadCardsHiRes(f, s.sceneInstance, s.position, s.geometricNormal, CL_READ_FINAL, 0.37 * hit.t, traceCoord);
            // (a leaf: (1 - t) of its side's light and t of the other side's - LumenHitIndirect.hlsli lhiFoliageFinal)
            if (cards.valid) radiance = lhiFoliageFinal(lhiRules(P[0].x), f, m, s.sceneInstance, s.position, s.geometricNormal, cards.final);
            else
            {
                // no card: the hit's direct light through the material's constants
                if ((m.classFlags & MATERIAL_EMISSIVE_VISIBLE_ONLY) != 0) m.emissive = 0;
                g_rtHitCone = 0.37;  // (a ray of the 4 x 4 hemisphere map: a cone of about 20 degrees half angle)
                const float bias = 1e-3 + 2e-4 * distance(s.position, g_cameraPosition);
                RtHitLighting L = (RtHitLighting)0;
                const float4 e = lhiIrradiance(lhiSources(P[0].x), s.position, s.normal, thread * 9781u + P[0].z * 26699u);
                // (past the mesh cards' end, where no source answers: the sky's light on the hit - the far field)
                L.irradiance = e.a > 0 ? e.rgb : giFarSkyIrradiance(s.position, s.normal, lhiRules(P[0].x).farStart);
                L.specularRadiance = e.rgb / 3.14159265;
                const float3 l = normalize(g_sunDirection);
                if (dot(s.normal, l) > 0 || rtHitTransmits(m))
                {
                    const float3 e0 = giSunIlluminance(s.position);
                    if (any(e0 > 0))
                    {
                        RayDesc sr;
                        sr.Origin = s.position + (dot(s.geometricNormal, l) > 0 ? 1.0 : -1.0) * s.geometricNormal * bias;
                        sr.Direction = l;
                        sr.TMin = 0;
                        sr.TMax = giRayLength();
                        // (the sun through the Glass on the way: what the panes leave of it - RayShaders.hlsli rtShadowTransmittance)
                        const float3 through = rtShadowTransmittance(scene, sr, RT_MASK_HIT_SHADOW);
                        L.sunIlluminance = e0 * through;
                        L.sunVisibility = any(through > 0) ? 1.0 : 0.0;
                    }
                }
                L.local = rtHitLocalSample(scene, s, m, -ray.Direction, hit.t * 0.74, bias, thread * 9781u + P[0].z * 26699u);
                radiance = rtHitRadiance(m, s.normal, -ray.Direction, L, 0.74);
            }
        }
    }
    const float cap = asfloat(P[4].y);
    const float brightest = max(radiance.r, max(radiance.g, radiance.b)) * g_exposure;
    if (cap > 0 && brightest > cap) radiance *= cap / brightest;
    if (any(isnan(radiance)) || any(isinf(radiance))) radiance = 0;
    trace[out2] = max(radiance, 0.0) * CL_RADIANCE_SCALE;
}
