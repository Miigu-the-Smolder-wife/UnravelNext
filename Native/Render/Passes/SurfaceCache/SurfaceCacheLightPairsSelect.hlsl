// unx-kernel: cs_6_6 main
// unx-variants: SKY=0,1
// r.sc.pairs.select (SurfaceCacheLightPairs.hlsli): one thread per cell of the frame's direct-light budget. No ray.
// P[0] = { cache UAV, budget, frame, flags (SurfaceCacheLight.hlsl: bit 0 local lights and sun, bit 5 feedback order,
//          bit 7 no local lights, bit 8 no sun, bit 10 lights without their shadow rays) }
// P[1], P[2], P[3] = sky and sun (GiSky.hlsli; P[1].w = ray length)
// P[4] = { cells UAV (raw), pairs UAV (raw), 0, asuint(default ray end bias, m: shading.mega_lights_ray_end_bias_m) }
// P[6], P[7] = RtSceneSrvs (the light grid: word 7)
#include "RayTracing/HitLocalLights.hlsli"
#include "Passes/GI/GiSky.hlsli"
#include "Passes/Shading/MegaLightsSampling.hlsli"
#include "Passes/SurfaceCache/SurfaceCacheLightPairs.hlsli"

void scpStore(RWByteAddressBuffer pairs, uint cell, uint k, ScpPair p)
{
    const uint at = scpPairOffset(cell, k);
    pairs.Store4(at, uint4(asuint(p.direction), asuint(p.tMax)));
    pairs.Store4(at + 16, uint4(asuint(p.irradiance), p.flags));
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint cell = id.x;
    if (cell >= P[0].y) return;
    RWByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    RWByteAddressBuffer cells = ResourceDescriptorHeap[P[4].x];
    RWByteAddressBuffer pairs = ResourceDescriptorHeap[P[4].y];
    ScpPair none;
    none.direction = float3(0, 0, 1);
    none.tMax = 0;
    none.irradiance = 0;
    none.flags = 0;
    uint k;
    for (k = 0; k < SCP_PAIRS; ++k) scpStore(pairs, cell, k, none);
    cells.Store(cell * 4, SC_NONE);

    const ScLayout l = scLayout(b);
    const uint n = l.entries;
    if (n == 0) return;
    const uint slot = scpPickCell(b, cell, P[0].y, P[0].z, n, (P[0].w & 32u) != 0);
    if (slot == SC_NONE) return;
    const uint4 data = b.Load4(scDataOffset(n, slot));
    const float3 position = asfloat(data.xyz), normal = scUnpackOct(data.w);
    if (!scpRayOk(position, normal)) return;  // (a cell without a valid point: no light and no rays; it is not stored)
    cells.Store(cell * 4, slot);
    if ((P[0].w & 1u) == 0) return;
    const float bias = scpBias(l, position);
    const RtSceneSrvs scene = rtSceneSrvs(P[6], P[7]);

    // ---- the lights of the cell's place in the light grid, the strongest SCP_LIGHTS kept (SurfaceCacheCellsGen's rule)
    if (scene.pad != 0xFFFFFFFFu && (P[0].w & 128u) == 0)
    {
        g_rtLightData = scene.pad;
        ByteAddressBuffer lights = ResourceDescriptorHeap[scene.pad];
        const RtLightGrid grid = lights.Load<RtLightGrid>(0);
        const uint gridCell = rtLightCell(grid, position);
        uint chosen[SCP_LIGHTS];
        float weight[SCP_LIGHTS];
        uint held = 0;
        if (gridCell != ~0u)
        {
            const uint k0 = rtLightCellStart(gridCell), k1 = rtLightCellStart(gridCell + 1);
            [loop] for (uint g = k0; g < k1; ++g)
            {
                const uint li = rtLightCellLight(g);
                const float w = rtLightOrientedImportance(rtLightFetch(li), position, normal, false);
                if (!(w > 0)) continue;
                uint at = held;
                if (held == SCP_LIGHTS)
                {
                    if (w <= weight[SCP_LIGHTS - 1]) continue;
                    at = SCP_LIGHTS - 1;
                }
                else ++held;
                [loop] while (at > 0 && weight[at - 1] < w)
                {
                    weight[at] = weight[at - 1];
                    chosen[at] = chosen[at - 1];
                    --at;
                }
                weight[at] = w;
                chosen[at] = li;
            }
        }
        const MlPoint lambert = mlPointLambert(position, normal, float3(1, 1, 1));  // (its radiance is irradiance / pi)
        const float defaultEndBias = asfloat(P[4].w);
        [loop] for (k = 0; k < held; ++k)
        {
            const GpuLight g = loadLight(chosen[k]);
            float3 e = 3.14159265 * mlLightUnshadowed(lambert, g, chosen[k], UNX_NONE);
            if (!all(e >= 0) || !all(e < 1e30)) e = 0;  // (NaN, infinite: no light)
            if (!any(e > 0)) continue;
            ScpPair p = none;
            p.irradiance = e;
            if (!lightCastsShadow(g) || (P[0].w & 1024u) != 0) p.flags = SCP_VISIBLE;
            else
            {
                // the ray toward the light's centre, ending before its surface by the light's end bias (scene::Light::rayEndBias,
                // else the default)
                const float3 d = g.position - position;
                const float dist = length(d);
                if (!(dist > 0)) p.flags = SCP_VISIBLE;  // (the cell at the light's centre: SurfaceCacheLight.hlsl counts it lit)
                else
                {
                    const uint type = lightType(g);
                    const float radius = type == LIGHT_SPHERE ? g.size.x : type == LIGHT_TUBE ? g.size.y : 0.0;
                    const float reach = dist - radius - lightRayEndBias(g, defaultEndBias);
                    p.direction = d / dist;
                    p.tMax = reach > bias ? reach : bias;
                    const float3 origin = position + normal * (dot(normal, p.direction) < 0 ? -bias : bias);
                    // (a ray that may not be launched: the pair stays without its visible bit - no light, as scCentreRay)
                    if (scpRayOk(origin, p.direction) && scpIntervalOk(bias, p.tMax)) p.flags = SCP_NEEDS_RAY;
                }
            }
            scpStore(pairs, cell, k, p);
        }
    }

    // ---- the sun: one ray into its disk
    const uint seed = giRandom(slot * 9781u + P[0].z * 6271u + 17u);
    const float3 toSun = giSunDirection(seed + 3);
    const float muS = dot(normal, toSun);
    if (muS > 0 && (P[0].w & 256u) == 0)
    {
        float3 e = giSunIlluminance(position) * muS;
        if (!all(e >= 0) || !all(e < 1e30)) e = 0;
        if (any(e > 0))
        {
            ScpPair p = none;
            p.irradiance = e;
            p.direction = toSun;
            p.tMax = giRayLength();
            if (scpRayOk(position + normal * bias, p.direction) && scpIntervalOk(0, p.tMax)) p.flags = SCP_NEEDS_RAY | SCP_SUN;
            scpStore(pairs, cell, SCP_LIGHTS, p);
        }
    }
}
