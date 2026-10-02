// unx-kernel: lib_6_6 main
// unx-variants: SKY=0,1
// r.card.radiosity.trace (CardLighting.hlsli): one ray generation thread per trace texel of the frame's radiosity list -
// 64 of a listed tile: 2 x 2 probes (CL_PROBE_SPACING texels apart) x 4 x 4 rays over the probe's hemisphere, uniform in
// solid angle, jittered per probe and update. The probe stands on one texel of its 4 x 4, chosen by the page's temporal
// index. The ray's radiance: the final lighting atlas at the hit (clReadCards; the back of a one-sided surface: 0), the
// sky where it escapes; its largest channel held to P[4].y in exposed units (the reference's MaxRayIntensity).
// A hit without a card (the reference: 0) takes its direct light - the sun by one shadow ray to the disk's centre and one
// local-light sample with its shadow ray (HitLocalSample.hlsli) - through the material's constants (no texture, no
// indirect light): a fifth to a third of the rays' hits read no card (lobby, 2026-10-02) and every bounce lost that
// share. A thread traces at most 3 rays (the ray and those two) and nothing loops around them: dispatches of at most
// 87,381 threads (CardLighting.cpp). (The reference retraces a ray whose first hit is a back face within 5 cm of its
// origin - AvoidSelfIntersections; not here.)
// The radiance goes to the trace atlas at the probe's rays' texels: tile origin + probe x 4 + ray.
// P[0] = { card frame SRV, select SRV, frame index, 0 }
// P[1], P[2], P[3] = sky and sun (GiSky.hlsli; P[1].w = ray length)
// P[4] = { trace atlas UAV, asuint(ray intensity cap, exposed units; 0: none), the dispatch's first thread, page capacity }
// P[5] = { direct list capacity, 0, radiosity list capacity, page light SRV (raw) }
// P[6], P[7] = RtSceneSrvs
#include "RayTracing/RayShaders.hlsli"
#include "RayTracing/HitShading.hlsli"
#include "RayTracing/HitLocalLights.hlsli"
#include "Passes/GI/GiSky.hlsli"
#include "Passes/SurfaceCache/CardLighting.hlsli"
#include "RayTracing/HitLocalSample.hlsli"

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
    const RtHit hit = rtTraceClosest(scene, ray, RAY_FLAG_NONE, RT_MASK_GI);
    if (hit.t < 0) radiance = giSkyRadiance(ray.Direction);
    else if (hit.instance != RT_INSTANCE_EMITTER)  // (a light's own surface: the direct light carries it)
    {
        const RtSurface s = rtSurface(scene, hit, ray.Origin, ray.Direction);
        // (the back of a one-sided surface: no light. rtSurface turns the normals of a back-face hit toward the ray's side)
        GpuMaterial m = loadMaterial(s.material);
        if (s.frontFace || (m.classFlags & MATERIAL_TWO_SIDED) != 0)
        {
            const ClSample cards = clReadCards(f, s.sceneInstance, s.position, s.geometricNormal, CL_READ_FINAL);
            if (cards.valid) radiance = cards.final;
            else
            {
                // no card: the hit's direct light through the material's constants
                if ((m.classFlags & MATERIAL_EMISSIVE_VISIBLE_ONLY) != 0) m.emissive = 0;
                g_rtHitCone = 0.37;  // (a ray of the 4 x 4 hemisphere map: a cone of about 20 degrees half angle)
                const float bias = 1e-3 + 2e-4 * distance(s.position, g_cameraPosition);
                RtHitLighting L = (RtHitLighting)0;
                const float3 l = normalize(g_sunDirection);
                if (dot(s.normal, l) > 0 || (m.classFlags & 0xFFu) == MATERIAL_FOLIAGE)
                {
                    const float3 e0 = giSunIlluminance(s.position);
                    if (any(e0 > 0))
                    {
                        RayDesc sr;
                        sr.Origin = s.position + (dot(s.geometricNormal, l) > 0 ? 1.0 : -1.0) * s.geometricNormal * bias;
                        sr.Direction = l;
                        sr.TMin = 0;
                        sr.TMax = giRayLength();
                        L.sunIlluminance = e0;
                        L.sunVisibility = rtVisible(scene, sr, RT_MASK_GI) ? 1.0 : 0.0;
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
