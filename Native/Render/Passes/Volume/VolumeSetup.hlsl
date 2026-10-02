// unx-kernel: cs_6_6 main
// unx-variants: STEP=0,1,2,3,4
// Particle media and heat haze, records and tile lists (VolumeCommon.hlsli):
//   STEP=0 clear: the media and haze tile counts and fills, the counters (thread per tile of the larger grid).
//   STEP=1 setup, thread per render thread: the particle at the frame time (FxLayerSetup.hlsl's interpolation: cubic
//          Hermite of the two ticks' ends; born in the latest tick p_n - v_n (1 - w) dt; died in it p_(n-1) + v_(n-1) w dt),
//          its appearance at that age; a volume-output particle -> media record (lit once: fxLitRadiance's law with
//          albedo 1, i.e. L_in per unit scattering) and the froxel tiles its bounding sphere (tent cube half-width r x
//          sqrt 3) covers; a distortion-output particle -> haze record and the 1/4-resolution tiles its support covers.
//   STEP=4 count: the record's loose-quadtree cells (at most K x K, VolumeCommon.hlsli), thread per record and cell row.
//   STEP=2 scan (P[0].y = 0 media, 1 haze): one group, exclusive prefix of the cell counts -> cell starts, entry total.
//   STEP=3 scatter: the entries (record, z0 | z1 as halves (media), tile rectangle) at cell start + atomic fill.
#include "Passes/Volume/VolumeCommon.hlsli"
#include "Passes/FX/ParticleLayerPass.hlsli"
#include "Passes/Shading/ShadingCommon.hlsli"
#include "Passes/Shading/AreaLight.hlsli"
#include "Passes/Atmosphere/Atmosphere.hlsli"
#include "Passes/Shadow/ShadowVisibility.hlsli"
#include "Passes/GI/GiCache.hlsli"
#include "Passes/GI/GiSource.hlsli"
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
float3 curve3(uint first, uint count, float u) { return count >= 2u ? nv_curve(first, count, u).yzw : float3(1, 1, 1); }
float volumePhase(float cosTheta, float g)  // Henyey-Greenstein over the sphere (the lit particles' fxPhase)
{
    const float g2 = g * g;
    return (1.0f - g2) / (4.0f * SH_PI * pow(max(1.0f + g2 - 2.0f * g * cosTheta, 1e-6f), 1.5f));
}

// In-scattered radiance per unit scattering towards the camera at a camera-relative point (D: camera -> point, unit):
// the sun (atmosphere transmittance, S's visibility in the air at 'footprint'), R's GI cache (isotropic) and the froxel
// list's local lights, each with the phase function; the law of FxLayerSetup.hlsl fxLitRadiance with albedo 1.
float3 volumeLit(VolumeConstants c, float3 offset, float3 D, float g, float footprint, uint2 pixel, float linearZ)
{
    const float3 worldPos = g_cameraPosition + offset;
    float3 L = 0;
    AtmosphereSrvs atm;
    atm.transmittance = c.transmittance;
    atm.multiScatter = c.multiScatter;
    atm.skyView = UNX_NONE;
    atm.aerial = c.airVolume;
    ShadowSrvs sh;
    sh.pageTable = c.shadow[0];
    sh.pool = c.shadow[1];
    sh.blocks = c.shadow[2];
    sh.searchBound = c.shadow[3];
    sh.constants = c.shadow[4];
    sh.lights = c.shadow[5];
    sh.pad0 = c.shadow[6];
    sh.layers = c.shadow[7];
    float3 E = g_sunIlluminance * g_sunColor;
    if (c.transmittance != UNX_NONE) E = atmosphereSunIlluminance(atm, worldPos);
    float visibility = 1;
    if (c.shadow[0] != UNX_NONE)
    {
        bool resident;
        const float v = shadowSunVisibilityInAir(sh, worldPos, footprint, resident);
        if (resident) visibility = v;
    }
    L += E * (visibility * volumePhase(dot(normalize(g_sunDirection), D), g));
    // indirect (GiSource.hlsli): the Lumen translucency volume's light through the phase function, or R's GI cache
    if (giSourceIsVolume(c.giCache)) L += ltvInscatter(giSourceVolume(c.giCache), worldPos, D, g);
    else if (c.giCache != UNX_NONE)
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
    // shading.mega_lights (render A; P[1].xy = the froxel grid's sampled local light, MegaLightsVolume.hlsl; as the lit
    // sprites, FxLayerSetup.hlsl ML = 1): the local lights' visible fluence F and direction moment M at the particle's
    // froxel with the phase function's first two SH bands, in place of the loop over the list with S's shadow maps (S
    // assigns no local shadow maps under mega_lights).
    if (c.froxelLights != UNX_NONE && P[1].x != UNX_NONE)
    {
        const FroxelGrid grid = froxelGrid(c.froxelLights);
        const float3 uvw = float3((float2(pixel) + 0.5) / (float2(grid.gridX, grid.gridY) * grid.tilePx), max(froxelSliceCoord(grid, linearZ), 0.5) / grid.slices);
        Texture3D<float4> fluenceVolume = ResourceDescriptorHeap[P[1].x];
        Texture3D<float4> momentVolume = ResourceDescriptorHeap[P[1].y];
        const float3 F = fluenceVolume.SampleLevel(g_linearClamp, uvw, 0).rgb / g_exposure;
        const float3 M = momentVolume.SampleLevel(g_linearClamp, uvw, 0).rgb / g_exposure;
        const float lumF = dot(F, float3(0.2126, 0.7152, 0.0722));
        if (lumF > 0) L += F * (max(0.0f, 1.0f + 3.0f * g * dot(M, D) / lumF) / (4.0f * SH_PI));
    }
    else if (c.froxelLights != UNX_NONE)
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
                const uint type = lightType(light);
                const float facing = max(0.0f, dot(light.forward, -toLight));
                const float area = type == LIGHT_RECT ? light.size.x * light.size.y * facing
                                 : type == LIGHT_DISK ? SH_PI * light.size.x * light.size.x * facing
                                 : type == LIGHT_SPHERE ? SH_PI * light.size.x * light.size.x
                                                        : 2.0f * light.size.y * light.size.x + SH_PI * light.size.y * light.size.y;
                El = light.color * (light.intensity * area * shAreaWindow(light, p) / d2);
            }
            float v = 1;
            if (lightCastsShadow(light) && c.shadow[0] != UNX_NONE && c.shadow[5] != UNX_NONE) v = shadowVisibilityDirect(sh, index, worldPos, -D);
            L += El * (v * volumePhase(dot(toLight, D), g));
        }
    }
    return L;
}

RenderRange volumeRange(VolumeConstants c, uint t, uint group)
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

// The particle of render thread t at the frame time (camera-relative, renderer axes: computed in stream space - the
// anchor offsets, origins, positions and velocities are - and mapped by c.streamAxes), with its age; false when not alive then.
bool volumeParticleAt(VolumeConstants c, RenderRange rr, uint k, StreamProgram p, out float3 pos, out float age)
{
    const uint birth = rr.first + k, row = rr.row;
    const float wdt = c.w * c.dt, rest = (1.0f - c.w) * c.dt;
    pos = 0;
    age = 0;
    if ((rr.prevCountFlags & 0x80000000u) != 0u)
    {
        StructuredBuffer<float4> posAge = ResourceDescriptorHeap[c.posAgePrev];
        StructuredBuffer<float4> velocity = ResourceDescriptorHeap[c.velocityPrev];
        StructuredBuffer<EmitterDynamic> dynamic = ResourceDescriptorHeap[c.dynamicPrev];
        const float4 pa = posAge[rr.stateBase + k];
        if (!(wdt < p.lifetime - pa.w)) return false;
        pos = (c.offsetPrev + dynamic[row].originAnchor + pa.xyz + velocity[rr.stateBase + k].xyz * wdt) * c.streamAxes;
        age = pa.w + wdt;
        return true;
    }
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
        pos = ((2 * w3 - 3 * w2 + 1) * p0 + (w3 - 2 * w2 + w) * c.dt * v0 + (3 * w2 - 2 * w3) * p1 + (w3 - w2) * c.dt * v1) * c.streamAxes;
        return true;
    }
    if (age < 0) return false;
    pos = (p1 - v1 * rest) * c.streamAxes;
    return true;
}

// Screen-space bound of a camera-relative sphere (full-resolution pixel rectangle, conservative: the projection of the
// 8 corners of its axis-aligned cube, or the whole view when the cube reaches the near plane) and its view depth range.
bool volumeSphereBound(float3 centre, float R, out float2 lo, out float2 hi, out float z0, out float z1)
{
    const float3 v = mul((float3x3)g_view, centre);
    const float z = -v.z;
    z0 = max(z - R, g_nearPlane);
    z1 = z + R;
    lo = 0;
    hi = float2(g_viewWidth, g_viewHeight);
    if (!(z1 > g_nearPlane)) return false;
    lo = float2(1e30f, 1e30f);
    hi = -lo;
    [unroll] for (uint k = 0; k < 8u; ++k)
    {
        const float3 corner = centre + R * float3((k & 1u) ? 1 : -1, (k & 2u) ? 1 : -1, (k & 4u) ? 1 : -1);
        if (!(-mul((float3x3)g_view, corner).z > g_nearPlane))
        {
            lo = 0;
            hi = float2(g_viewWidth, g_viewHeight);
            return true;
        }
        const float2 px = volumePixelOf(corner);
        lo = min(lo, px);
        hi = max(hi, px);
    }
    return true;
}

groupshared uint gs_partial[1024];

[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID, uint3 gtid : SV_GroupThreadID)
{
    const VolumeConstants c = volumeConstants();
#if STEP == 0
    const uint mediaTiles = c.mediaTiles, hazeTiles = c.hazeCells;  // list cells (VolumeCommon.hlsli)
    if (id.x < mediaTiles)
    {
        RWStructuredBuffer<uint> counts = ResourceDescriptorHeap[c.mediaCounts];
        RWStructuredBuffer<uint> fill = ResourceDescriptorHeap[c.mediaFill];
        counts[id.x] = 0u;
        fill[id.x] = 0u;
    }
    if (id.x < hazeTiles)
    {
        RWStructuredBuffer<uint> counts = ResourceDescriptorHeap[c.hazeCounts];
        RWStructuredBuffer<uint> fill = ResourceDescriptorHeap[c.hazeFill];
        counts[id.x] = 0u;
        fill[id.x] = 0u;
    }
    if (id.x < 8u)
    {
        RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[c.counters];
        counters[id.x] = 0u;
    }
#elif STEP == 1 || STEP == 3 || STEP == 4
    const uint t = id.x;
    if (t >= c.threads) return;
    RWStructuredBuffer<VolumeRecord> records = ResourceDescriptorHeap[c.records];
    VolumeRecord rec = (VolumeRecord)0;
#if STEP == 1
    s_curveKeys = c.curveKeys;
    if ((c.mode & 4u) != 0u) return;  // given records (VolumePass::recordRecords): nothing to evaluate
    {
        const RenderRange rr = volumeRange(c, t, gid.x);
        StructuredBuffer<StreamEmitter> emitters = ResourceDescriptorHeap[c.emitters];
        StructuredBuffer<StreamProgram> programs = ResourceDescriptorHeap[c.programs];
        const StreamEmitter e = emitters[rr.row];
        const StreamProgram p = programs[e.program];
        const bool media = p.output == FX_OUTPUT_VOLUME && (c.mode & 2u) != 0u;
        const bool haze = p.output == FX_OUTPUT_DISTORTION && (c.mode & 1u) != 0u;
        float3 pos;
        float age;
        if ((media || haze) && (e.flags & FX_EMITTER_KILLED) == 0u && p.lifetime > 0 && volumeParticleAt(c, rr, t - rr.thread, p, pos, age))
        {
            const float u = saturate(age / p.lifetime);
            const float size = p.size * e.sizeScale * curve1(p.sizeKeys, p.sizeCount, u);
            const float alpha = p.color.a * e.colorScale.a * curve1(p.alphaKeys, p.alphaCount, u);
            if (size > 0 && alpha > 0)
            {
                rec.centre = pos;
                if (media)
                {
                    const float3 colour = p.color.rgb * e.colorScale.rgb * curve3(p.colorKeys, p.colorCount, u);
                    const float r = 0.5f * size;
                    const float3 v = mul((float3x3)g_view, pos);
                    const float z = max(-v.z, g_nearPlane);
                    const float2 px = clamp(volumePixelOf(pos), 0, float2(g_viewWidth - 1, g_viewHeight - 1));
                    const float footprint = 2.0f * z / (g_proj[1][1] * g_viewHeight);
                    const float3 lin = volumeLit(c, pos, normalize(pos), p.mediumPhase, max(r, footprint), (uint2)px, z);
                    rec.radius = r;
                    rec.mass = alpha;
                    rec.a = p.mediumAbsorption + p.mediumScattering;
                    rec.b = p.mediumScattering * lin + p.mediumEmission * colour;
                    rec.kind = VOLUME_KIND_MEDIA;
                }
                else
                {
                    rec.radius = size;
                    rec.a = float3(p.refractionAmplitude * alpha, p.refractionWidth, (float)p.refractionProfile);
                    rec.mass = volumeHazeSupport(size, rec.a);
                    rec.kind = VOLUME_KIND_HAZE;
                }
            }
        }
        // non-finite inputs (a defect upstream, e.g. an unset program or light term) are isolated and reported, never
        // binned: one NaN channel would otherwise saturate the slices' extinction (min(NaN, 65504) = 65504, T = 0)
        const float check = rec.centre.x + rec.centre.y + rec.centre.z + rec.radius + rec.mass + rec.a.x + rec.a.y + rec.a.z + rec.b.x + rec.b.y + rec.b.z;
        if (rec.kind != VOLUME_KIND_NONE && !isfinite(check))
        {
            volumeStatus(c, VOLUME_STATUS_NONFINITE);
            rec = (VolumeRecord)0;
        }
        records[t] = rec;
    }
    return;  // binning: STEP 4 (counts) and STEP 3 (entries), one thread per record and cell row
#else
    rec = records[t];
#endif
    if (rec.kind == VOLUME_KIND_NONE) return;
    const float R = rec.kind == VOLUME_KIND_MEDIA ? rec.radius * 1.7320508f : rec.mass;
    float2 plo, phi;
    float z0, z1;
    if (!volumeSphereBound(rec.centre, R, plo, phi, z0, z1)) return;
    uint tilesX, tilesY, span, countsIndex, fillIndex, startsIndex, entriesIndex, capacity, overflowBit;
    if (rec.kind == VOLUME_KIND_MEDIA)
    {
        ByteAddressBuffer lists = ResourceDescriptorHeap[c.froxelLights];
        const FroxelGrid g = lists.Load<FroxelGrid>(0);
        tilesX = g.gridX; tilesY = g.gridY; span = g.tilePx;
        countsIndex = c.mediaCounts; fillIndex = c.mediaFill; startsIndex = c.mediaStarts; entriesIndex = c.mediaEntries;
        capacity = c.mediaEntryCapacity; overflowBit = VOLUME_STATUS_MEDIA_OVERFLOW;
    }
    else
    {
        tilesX = c.hazeTilesX; tilesY = c.hazeTilesY; span = VOLUME_HAZE_TILE * VOLUME_HAZE_SCALE;
        countsIndex = c.hazeCounts; fillIndex = c.hazeFill; startsIndex = c.hazeStarts; entriesIndex = c.hazeEntries;
        capacity = c.hazeEntryCapacity; overflowBit = VOLUME_STATUS_HAZE_OVERFLOW;
    }
    if (tilesX == 0u || tilesY == 0u || span == 0u) return;  // no grid: tilesX - 1 would wrap and the loops below never end
    const float2 lo = floor(plo / span), hi = floor(phi / span);
    if (any(hi < 0.0f) || lo.x > (float)(tilesX - 1) || lo.y > (float)(tilesY - 1)) return;
    const uint2 t0 = (uint2)clamp(lo, 0.0f, float2(tilesX - 1, tilesY - 1)), t1 = (uint2)clamp(hi, 0.0f, float2(tilesX - 1, tilesY - 1));
    // The loose quadtree level where the rectangle spans at most K x K cells (VolumeCommon.hlsli): one cell row per thread
    // (dispatch Y = K), at most K atomics each whatever the particle's size.
    const uint2 tiles = uint2(tilesX, tilesY);
    const uint L = volumeLevelOf(t0, t1);
    const uint2 c0 = t0 >> L, c1 = t1 >> L, dims = volumeLevelDims(tiles, L);
    const uint base = volumeLevelBase(tiles, L);
    const uint y = c0.y + gid.y;
    if (y > c1.y) return;
#if STEP == 4
    RWStructuredBuffer<uint> counts = ResourceDescriptorHeap[countsIndex];
    [loop] for (uint x = c0.x; x <= c1.x; ++x) InterlockedAdd(counts[base + y * dims.x + x], 1u);
    if (gid.y == 0u)
    {
        RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[c.counters];
        InterlockedAdd(counters[VOLUME_COUNTER_RECORDS], 1u);
    }
#else
    RWStructuredBuffer<uint> fill = ResourceDescriptorHeap[fillIndex];
    RWStructuredBuffer<uint> starts = ResourceDescriptorHeap[startsIndex];
    RWStructuredBuffer<uint4> entries = ResourceDescriptorHeap[entriesIndex];
    const uint zz = f32tof16(z0 * 0.999f) | (f32tof16(z1 * 1.001f) << 16);  // widened past half-float rounding (the density is 0 there)
    const uint4 entry = uint4(t, zz, t0.x | (t0.y << 16), t1.x | (t1.y << 16));
    [loop] for (uint x = c0.x; x <= c1.x; ++x)
    {
        const uint tile = base + y * dims.x + x;
        uint slot;
        InterlockedAdd(fill[tile], 1u, slot);
        const uint at = starts[tile] + slot;
        if (at < capacity) entries[at] = entry;
        else volumeStatus(c, overflowBit);
    }
#endif
#else  // STEP == 2: one group of 256 threads, each a run of consecutive tiles
    const bool haze = P[0].y != 0u;
    uint tiles;
    RWStructuredBuffer<uint> counts = ResourceDescriptorHeap[haze ? c.hazeCounts : c.mediaCounts];
    RWStructuredBuffer<uint> starts = ResourceDescriptorHeap[haze ? c.hazeStarts : c.mediaStarts];
    tiles = haze ? c.hazeCells : c.mediaTiles;  // list cells of every level (VolumeCommon.hlsli)
    const uint t = gtid.x, per = (tiles + 255u) / 256u;
    uint local = 0u;
    for (uint k = 0u; k < per; ++k)
    {
        const uint at = t * per + k;
        if (at < tiles) local += counts[at];
    }
    gs_partial[t] = local;
    GroupMemoryBarrierWithGroupSync();
    for (uint s = 1u; s < 256u; s <<= 1)
    {
        const uint o = t >= s ? gs_partial[t - s] : 0u;
        GroupMemoryBarrierWithGroupSync();
        gs_partial[t] += o;
        GroupMemoryBarrierWithGroupSync();
    }
    uint run = gs_partial[t] - local;
    for (uint k2 = 0u; k2 < per; ++k2)
    {
        const uint at = t * per + k2;
        if (at < tiles)
        {
            starts[at] = run;
            run += counts[at];
        }
    }
    if (t == 255u)
    {
        RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[c.counters];
        const uint total = gs_partial[255];
        counters[haze ? VOLUME_COUNTER_HAZE_ENTRIES : VOLUME_COUNTER_MEDIA_ENTRIES] = total;
        if (total > (haze ? c.hazeEntryCapacity : c.mediaEntryCapacity)) InterlockedOr(counters[VOLUME_COUNTER_STATUS], haze ? VOLUME_STATUS_HAZE_OVERFLOW : VOLUME_STATUS_MEDIA_OVERFLOW);
    }
#endif
}
