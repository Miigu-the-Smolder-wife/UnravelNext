// unx-kernel: cs_6_6 main
// unx-variants: STEP=0,1
// Particle render pass, per particle of the latest tick's render ranges (ParticleLayerPass.hlsli RenderRange):
//   STEP=0: the particle at the frame time (render rules request 2: a particle of both ticks by cubic Hermite of the two
//           ends' positions and velocities, one born in the latest tick by p_n - v_n (1 - w) dt from age_n - (1 - w) dt >= 0,
//           one that died in it by p_(n-1) + v_(n-1) w dt while w dt < lifetime - age_(n-1)), its appearance at that age
//           (size, colour, opacity: pure functions of program, emitter and age), the projection, the pixel-footprint
//           prefilter and the record; then the tile counts of the record's square.
//   STEP=1: the tile entries (depth bits, record index) at tile start + atomic fill (the tile kernel sorts them).
// Sprites only for now (M0); other outputs write an undrawn record. Lighting (request 3, stage 2): a program with material
// 1 is lit (colour = albedo): the sun (S's air visibility, the atmosphere's transmittance), R's GI cache (isotropic: the
// mean irradiance of the six axes / pi) and the froxel list's local lights (S's direct visibility where they cast
// shadows), each with the program's phase function (Henyey-Greenstein, g = medium_phase), once at the particle centre;
// material 0 is emissive (colour = radiance, nit). Every particle then takes S's air between the camera and its centre:
// premultiplied colour = alpha (T_air L + inscatter) (request 4: the surface behind already carries the full path).
#include "Passes/FX/ParticleLayerPass.hlsli"
#include "Passes/Shading/ShadingCommon.hlsli"
#include "Passes/Shading/AreaLight.hlsli"
#include "Passes/Atmosphere/Atmosphere.hlsli"
#include "Passes/Shadow/ShadowVisibility.hlsli"
#include "Passes/GI/GiCache.hlsli"
#include "Passes/Atmosphere/Froxel.hlsli"

static uint s_curveKeys;
float4 fxLayerCurveKey(uint i)
{
    StructuredBuffer<float4> keys = ResourceDescriptorHeap[s_curveKeys];
    return keys[i];
}
#define NV_PARTICLE_MATH_TYPES_ONLY
#include "Passes/FX/Stream/shaders/VfxParticleMath.hlsli"
#undef NV_PARTICLE_MATH_TYPES_ONLY
#define NV_FIELD_COUNT 0u
#define NV_FIELD(i) ((NvField)0)
#define NV_WORLD_FIELD_COUNT 0u
#define NV_WORLD_FIELD(i) ((NvWorldField)0)
#define NV_SURFACE_COUNT 0u
#define NV_SURFACE(i) ((NvSurface)0)
#define NV_CURVE_KEY(i) fxLayerCurveKey(i)
#include "Passes/FX/Stream/shaders/VfxParticleMath.hlsli"

float curve1(uint first, uint count, float u) { return count >= 2u ? nv_curve(first, count, u).y : 1.0f; }

float fxPhase(float cosTheta, float g)  // Henyey-Greenstein, normalised over the sphere; g = 0: 1 / 4 pi
{
    const float g2 = g * g;
    return (1.0f - g2) / (4.0f * SH_PI * pow(max(1.0f + g2 - 2.0f * g * cosTheta, 1e-6f), 1.5f));
}

// Radiance a lit particle of albedo 'albedo' scatters towards the camera (D: camera -> particle, unit).
float3 fxLitRadiance(LayerConstants c, float3 albedo, float3 offset, float3 D, float g, float footprint, uint2 pixel, float linearZ)
{
    const float3 worldPos = g_cameraPosition + offset;
    float3 L = 0;
    AtmosphereSrvs atm;
    atm.transmittance = c.transmittance;
    atm.multiScatter = c.multiScatter;
    atm.skyView = UNX_NONE;
    atm.aerial = c.airVolume;
    ShadowSrvs sh;
    sh.pageTable = c.shadowPageTable;
    sh.pool = c.shadowPool;
    sh.blocks = c.shadowBlocks;
    sh.searchBound = c.shadowSearchBound;
    sh.constants = c.shadowConstants;
    sh.lights = c.shadowLights;
    sh.pad0 = c.shadowSlotOfLight;
    sh.layers = c.shadowLayers;
    // sun: illuminance at the particle (the atmosphere's transmittance), S's visibility in the air
    float3 E = g_sunIlluminance * g_sunColor;
    if (c.transmittance != UNX_NONE) E = atmosphereSunIlluminance(atm, worldPos);
    float visibility = 1;
    if (c.shadowPageTable != UNX_NONE)
    {
        bool resident;
        const float v = shadowSunVisibilityInAir(sh, worldPos, footprint, resident);
        if (resident) visibility = v;
    }
    const float3 l = normalize(g_sunDirection);
    L += E * (visibility * fxPhase(dot(l, D), g));
    // indirect: R's GI cache, isotropic (the mean irradiance over the six axes / pi = fluence / 4 pi)
    if (c.giCache != UNX_NONE)
    {
        ByteAddressBuffer cache = ResourceDescriptorHeap[c.giCache];
        const GiHeader h = giHeader(cache);
        float3 sum = 0;
        float n = 0;
        const float3 axes[6] = { float3(1, 0, 0), float3(-1, 0, 0), float3(0, 1, 0), float3(0, -1, 0), float3(0, 0, 1), float3(0, 0, -1) };
        [unroll] for (uint a = 0; a < 6u; ++a)
        {
            float w;
            const float3 e = giCacheIrradianceAt(cache, h, worldPos, axes[a], 0, w);
            if (w > 0) { sum += e; n += 1; }
        }
        if (n > 0) L += sum / (n * SH_PI);
    }
    // local lights of the froxel list at the particle (punctual exactly; area lights as their centre's point, exact
    // when the light is small against its distance)
    if (c.froxelLights != UNX_NONE)
    {
        FroxelSrvs froxels;
        froxels.lights = c.froxelLights;
        froxels.lightIndices = c.froxelLights;
        froxels.scattering = UNX_NONE;
        froxels.pad = 0;
        const uint2 range = froxelLightRange(froxels, pixel, linearZ);
        for (uint i = 0; i < range.y; ++i)
        {
            const uint index = froxelLight(froxels, range.x + i);
            const GpuLight light = loadLight(index);
            float3 toLight;
            float3 El = shPunctualIlluminance(light, (light.position - g_cameraPosition) - offset, toLight);
            if (lightType(light) > LIGHT_SPOT)
            {
                const float3 p = (light.position - g_cameraPosition) - offset;
                const float d2 = max(dot(p, p), 1e-4f);
                toLight = p * rsqrt(d2);
                // radiance x the light's projected area seen from the particle (small-source limit): rect w x h and disk
                // pi r^2 times their emitting side's cosine, sphere pi r^2, tube (capsule) 2 r l + pi r^2 broadside
                const uint type = lightType(light);
                const float facing = max(0.0f, dot(light.forward, -toLight));
                const float area = type == LIGHT_RECT ? light.size.x * light.size.y * facing
                                 : type == LIGHT_DISK ? SH_PI * light.size.x * light.size.x * facing
                                 : type == LIGHT_SPHERE ? SH_PI * light.size.x * light.size.x
                                                        : 2.0f * light.size.y * light.size.x + SH_PI * light.size.y * light.size.y;
                El = light.color * (light.intensity * area * shAreaWindow(light, p) / d2);
            }
            float v = 1;
            if (lightCastsShadow(light) && c.shadowPageTable != UNX_NONE && c.shadowLights != UNX_NONE) v = shadowVisibilityDirect(sh, index, worldPos, -D);
            L += El * (v * fxPhase(dot(toLight, D), g));
        }
    }
    return albedo * L;
}
float3 curve3(uint first, uint count, float u) { return count >= 2u ? nv_curve(first, count, u).yzw : float3(1, 1, 1); }

RenderRange renderRange(LayerConstants c, uint t, uint group)
{
    StructuredBuffer<uint> blocks = ResourceDescriptorHeap[c.blocks];
    StructuredBuffer<RenderRange> ranges = ResourceDescriptorHeap[c.ranges];
    uint lo = blocks[group], hi = min(blocks[group + 1u] + 1u, c.rangeCount);
    [loop] for (uint guard = 0u; guard < 32u && hi - lo > 1u; ++guard)
    {
        const uint mid = (lo + hi) >> 1;
        if (ranges[mid].thread <= t) lo = mid; else hi = mid;
    }
    return ranges[lo];
}

LayerRecord setup(LayerConstants c, uint t, uint group)
{
    LayerRecord rec = (LayerRecord)0;
    const RenderRange rr = renderRange(c, t, group);
    const uint k = t - rr.thread, birth = rr.first + k, row = rr.row;
    StructuredBuffer<StreamEmitter> emitters = ResourceDescriptorHeap[c.emitters];
    StructuredBuffer<StreamProgram> programs = ResourceDescriptorHeap[c.programs];
    const StreamEmitter e = emitters[row];
    const StreamProgram p = programs[e.program];
    if (p.output != FX_OUTPUT_SPRITE || (e.flags & FX_EMITTER_KILLED) != 0u || !(p.lifetime > 0)) return rec;

    // the particle at the frame time, relative to the camera
    const float wdt = c.w * c.dt, rest = (1.0f - c.w) * c.dt;
    float3 pos;
    float age;
    if ((rr.prevCountFlags & 0x80000000u) != 0u)
    {
        StructuredBuffer<float4> posAge = ResourceDescriptorHeap[c.posAgePrev];
        StructuredBuffer<float4> velocity = ResourceDescriptorHeap[c.velocityPrev];
        StructuredBuffer<EmitterDynamic> dynamic = ResourceDescriptorHeap[c.dynamicPrev];
        const float4 pa = posAge[rr.stateBase + k];
        if (!(wdt < p.lifetime - pa.w)) return rec;  // dead by the frame time
        pos = c.offsetPrev + dynamic[row].originAnchor + pa.xyz + velocity[rr.stateBase + k].xyz * wdt;
        age = pa.w + wdt;
    }
    else
    {
        StructuredBuffer<float4> posAge = ResourceDescriptorHeap[c.posAgeCur];
        StructuredBuffer<float4> velocity = ResourceDescriptorHeap[c.velocityCur];
        StructuredBuffer<EmitterDynamic> dynamic = ResourceDescriptorHeap[c.dynamicCur];
        const float4 pa1 = posAge[rr.stateBase + k];
        const float3 v1 = velocity[rr.stateBase + k].xyz;
        const float3 p1 = c.offsetCur + dynamic[row].originAnchor + pa1.xyz;
        age = pa1.w - rest;
        const uint rel = birth - rr.prevFirst;
        if (rel < (rr.prevCountFlags & 0x7FFFFFFFu))
        {
            StructuredBuffer<float4> posAge0 = ResourceDescriptorHeap[c.posAgePrev];
            StructuredBuffer<float4> velocity0 = ResourceDescriptorHeap[c.velocityPrev];
            StructuredBuffer<EmitterDynamic> dynamic0 = ResourceDescriptorHeap[c.dynamicPrev];
            const float3 p0 = c.offsetPrev + dynamic0[row].originAnchor + posAge0[rr.prevBase + rel].xyz;
            const float3 v0 = velocity0[rr.prevBase + rel].xyz;
            const float w = c.w, w2 = w * w, w3 = w2 * w;
            pos = (2 * w3 - 3 * w2 + 1) * p0 + (w3 - 2 * w2 + w) * c.dt * v0 + (3 * w2 - 2 * w3) * p1 + (w3 - w2) * c.dt * v1;
        }
        else
        {
            if (age < 0) return rec;  // born after the frame time
            pos = p1 - v1 * rest;
        }
    }

    // appearance at that age
    const float u = saturate(age / p.lifetime);
    const float size = p.size * e.sizeScale * curve1(p.sizeKeys, p.sizeCount, u);
    const float3 colour = p.color.rgb * e.colorScale.rgb * curve3(p.colorKeys, p.colorCount, u);
    const float alpha = saturate(p.color.a * e.colorScale.a * curve1(p.alphaKeys, p.alphaCount, u));
    if (!(size > 0) || !(alpha > 0)) return rec;

    // projection (camera-relative: the view's rotation, then its projection)
    const float3 v = mul((float3x3)g_view, pos);
    const float distance = -v.z;
    if (!(distance > g_nearPlane)) return rec;
    const float4 clip = mul(g_proj, float4(v, 1));
    const float2 ndc = clip.xy / clip.w;
    const float2 centre = float2((ndc.x + 1) * 0.5f * g_viewWidth, (1 - ndc.y) * 0.5f * g_viewHeight);
    const float radius = 0.5f * size * g_proj[1][1] * 0.5f * g_viewHeight / distance;
    // Pixel-footprint prefilter: the pixel box filter widens the profile to r' = sqrt(r^2 + 1/4) (radius of a half pixel)
    // at the same integrated opacity (alpha r^2 / r'^2): a sprite below a pixel keeps its energy instead of being missed
    // by pixel centres.
    const float r2 = radius * radius, rEff2 = r2 + 0.25f;
    const float rEff = sqrt(rEff2);
    if (centre.x + rEff < 0 || centre.y + rEff < 0 || centre.x - rEff > g_viewWidth || centre.y - rEff > g_viewHeight) return rec;
    rec.centre = centre;
    rec.radius = rEff;
    rec.depth = g_nearPlane / distance;
    // lighting (material 1: lit; 0: emissive), then the air between the camera and the particle (S's air volume)
    const float3 D = pos / distance;
    const float linearZ = distance;  // (v.z along the view axis: the view depth)
    const float footprint = 2.0f * distance / (g_proj[1][1] * g_viewHeight);
    float3 radiance = p.material == 1u ? fxLitRadiance(c, colour, pos, normalize(pos), p.mediumPhase, max(size * 0.5f, footprint), (uint2)clamp(centre, 0, float2(g_viewWidth - 1, g_viewHeight - 1)), linearZ)
                                       : colour;
    if (c.airVolume != UNX_NONE && c.transmittance != UNX_NONE)
    {
        AtmosphereSrvs atm;
        atm.transmittance = c.transmittance;
        atm.multiScatter = c.multiScatter;
        atm.skyView = UNX_NONE;
        atm.aerial = c.airVolume;
        float3 inscatter, transmittance, sunAtDepth;
        atmosphereAirView(atm, centre / float2(g_viewWidth, g_viewHeight), linearZ, inscatter, transmittance, sunAtDepth);
        radiance = radiance * transmittance + inscatter;
    }
    rec.radianceAlpha = fxPackHalf4(float4(radiance * g_exposure, alpha * r2 / rEff2));
    rec.flags = rEff < FX_LAYER_MIN_RADIUS ? FX_LAYER_RECORD_SMALL : 0u;
    rec.program = e.program;
    return rec;
}

[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID)
{
    const LayerConstants c = fxLayerConstants();
    const uint t = id.x;
    if (t >= c.threads) return;
    RWStructuredBuffer<LayerRecord> records = ResourceDescriptorHeap[c.records];
#if STEP == 0
    s_curveKeys = c.curveKeys;
    const LayerRecord rec = setup(c, t, gid.x);
    records[t] = rec;
    uint2 t0, t1;
    if (!fxLayerTiles(c, rec, t0, t1)) return;
    RWStructuredBuffer<uint> counts = ResourceDescriptorHeap[c.tileCounts];
    [loop] for (uint y = t0.y; y <= t1.y; ++y)
        [loop] for (uint x = t0.x; x <= t1.x; ++x) InterlockedAdd(counts[y * c.tilesX + x], 1u);
    const uint n = WaveActiveCountBits(true);
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[c.counters];
    if (WaveIsFirstLane()) InterlockedAdd(counters[FX_LAYER_COUNTER_DRAWN], n);
#else
    const LayerRecord rec = records[t];
    uint2 t0, t1;
    if (!fxLayerTiles(c, rec, t0, t1)) return;
    RWStructuredBuffer<uint> fill = ResourceDescriptorHeap[c.tileFill];
    RWStructuredBuffer<uint> starts = ResourceDescriptorHeap[c.tileStarts];
    RWStructuredBuffer<uint2> entries = ResourceDescriptorHeap[c.entries];
    [loop] for (uint y = t0.y; y <= t1.y; ++y)
        [loop] for (uint x = t0.x; x <= t1.x; ++x)
        {
            const uint tile = y * c.tilesX + x;
            uint slot;
            InterlockedAdd(fill[tile], 1u, slot);
            const uint at = starts[tile] + slot;
            if (at < c.entryCapacity) entries[at] = uint2(asuint(rec.depth), t);
            else fxLayerStatus(c, FX_LAYER_STATUS_ENTRY_OVERFLOW);
        }
#endif
}
