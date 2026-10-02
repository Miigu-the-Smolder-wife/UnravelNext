// unx-kernel: cs_6_6 main
// unx-variants: STEP=0,1 ML=0,1 GIV=0,1
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
#include "Passes/FX/FxParticleAt.hlsli"
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
    // indirect (GiSource.hlsli): the Lumen translucency volume's light through the phase function (band 0, and band 1 x
    // g), or R's GI cache, isotropic (the mean irradiance over the six axes / pi = fluence / 4 pi)
    // (GIV: the source's kind picks the kernel, ParticleLayer.cpp - with the lighting inlined twice both reads in one
    // kernel passed the DXIL limit; it stands once now, and the four variants are 73 to 94 KB)
#if GIV
    if (giSourceIsVolume(c.giCache)) L += ltvInscatter(giSourceVolume(c.giCache), worldPos, D, g);
#else
    if (giSourceIsCache(c.giCache))
    {
        ByteAddressBuffer cache = ResourceDescriptorHeap[c.giCache];
        const GiHeader h = giHeader(cache);
        float3 sum = 0;
        float n = 0;
        // (a loop: one copy of the cache lookup in the kernel - six stood for 7,500 instructions, 31 KB)
        [loop] for (uint a = 0; a < 6u; ++a)
        {
            const float side = (a & 1u) ? -1.0 : 1.0;  // +x, -x, +y, -y, +z, -z
            const float3 axis = float3((a >> 1) == 0 ? side : 0.0, (a >> 1) == 1 ? side : 0.0, (a >> 1) == 2 ? side : 0.0);
            float w;
            const float3 e = giCacheIrradianceAt(cache, h, worldPos, axis, 0, w);
            if (w > 0) { sum += e; n += 1; }
        }
        if (n > 0) L += sum / (n * SH_PI);
    }
#endif
    // ML = 1 (its own variant, as GIV above; ParticleLayer.cpp picks it when the volumes exist):
    // shading.mega_lights (render A; P[1].xy = the froxel grid's sampled local light, MegaLightsVolume.hlsl - as Unreal's
    // MegaLights lights translucency through its lit volume): the local lights' visible fluence F and luminance-weighted
    // direction moment M at the particle's froxel (trilinear), with the phase function's first two SH bands:
    // L = F (1 + 3 g (M . D) / lum(F)) / 4 pi. The loop over the list with S's shadow maps is then not run (S assigns no
    // local shadow maps under mega_lights).
#if ML
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
#else
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
                El = lightMeanColor(light) * (light.intensity * lightDiffuseScale(light) * area * lightBarnDoorFar(light, -toLight) * shAreaWindow(light, p) / d2);
            }
            float v = 1;
            if (lightCastsShadow(light) && c.shadowPageTable != UNX_NONE && c.shadowLights != UNX_NONE) v = shadowVisibilityDirect(sh, index, worldPos, -D);
            L += El * (v * fxPhase(dot(toLight, D), g));
        }
    }
#endif
    return albedo * L;
}
float3 curve3(uint first, uint count, float u) { return count >= 2u ? nv_curve(first, count, u).yzw : float3(1, 1, 1); }

// What a particle of either output asks of the lighting. The kernel holds fxLitRadiance once (setup): each output's code is
// two halves around that one call. With a call in the sprite's path and another in the ribbon point's the kernel carried
// the whole lighting twice - 19,600 instructions each, four fifths of a kernel at the size limit.
struct FxLitPoint
{
    bool lit;        // material 1 (colour = albedo); else the radiance is the colour
    float3 colour, pos;
    float phase, footprint, linearZ;
    uint2 pixel;
};

// A ribbon particle's point of this frame in the render pass's ribbon layout (ParticleSystem: per ribbon row the births
// [dying_birth, next_birth) of the latest tick, the ones that died in it first; FxRibbon then builds the strips over each
// range's valid window): camera-relative position at the frame time, width after the
// pixel-footprint prefilter across it (the strip's profile widened to h' = sqrt(h^2 + 1/4) px, h its half width, at the same
// integrated opacity: alpha h / h'), and its appearance before the air (the strip samples the air at each hit):
// radiance x exposure (material 1 lit at the point like a sprite, 0 emissive), opacity. A point not alive at the frame time
// is written invalid: born after it (the newest births, the range's tail) or already dead (the oldest dying ones, its head);
// the valid points are one window. A killed emitter or a refused material writes invalid points (nothing drawn).
// ribbonPointBegin: the point up to its lighting request (false: nothing to light - no place in the layout, or an invalid
// point, written); ribbonPointEnd: the point and its appearance with the radiance.
struct FxRibbonPending
{
    uint index;
    FxRibbonPoint record;
    float alpha;
};
bool ribbonPointBegin(LayerConstants c, RenderRange rr, uint k, uint birth, uint row, StreamEmitter e, StreamProgram p, bool drawn, out FxRibbonPending pending,
                      out FxLitPoint lp)
{
    pending = (FxRibbonPending)0;
    lp = (FxLitPoint)0;
    float3 pos;
    float age;
    bool dying;
    const bool alive = drawn && fxParticleAt(c, rr, k, birth, row, p, pos, age, dying);
    if (c.ribbonRows == UNX_NONE) return false;
    StructuredBuffer<uint2> rows = ResourceDescriptorHeap[c.ribbonRows];
    const uint2 place = rows[row];
    if (place.x == FX_NONE) return false;
    const uint index = place.x + (birth - place.y);
    if (index >= c.ribbonCapacity) { fxLayerStatus(c, FX_LAYER_STATUS_RANGE); return false; }
    FxRibbonPoint rp = (FxRibbonPoint)0;
    if (!alive)
    {
        RWStructuredBuffer<FxRibbonPoint> points = ResourceDescriptorHeap[c.ribbonPoints];
        RWStructuredBuffer<uint2> appearance = ResourceDescriptorHeap[c.ribbonAppearance];
        points[index] = rp;  // valid = 0
        appearance[index] = uint2(0, 0);
        return false;
    }
    const float u = saturate(age / p.lifetime);
    const float size = p.size * e.sizeScale * curve1(p.sizeKeys, p.sizeCount, u);
    const float3 colour = p.color.rgb * e.colorScale.rgb * curve3(p.colorKeys, p.colorCount, u);
    float alpha = saturate(p.color.a * e.colorScale.a * curve1(p.alphaKeys, p.alphaCount, u));
    const float3 v = mul((float3x3)g_view, pos);
    const float distance = -v.z;
    float width = max(size, 0.0f);
    float2 pixel = float2(0.5f * g_viewWidth, 0.5f * g_viewHeight);
    if (distance > g_nearPlane && width > 0)
    {
        const float h = 0.5f * width * g_proj[1][1] * 0.5f * g_viewHeight / distance;
        const float hEff = sqrt(h * h + 0.25f);
        width *= hEff / h;
        alpha *= h / hEff;
        const float4 clip = mul(g_proj, float4(v, 1));
        pixel = clamp(float2((clip.x / clip.w + 1) * 0.5f * g_viewWidth, (1 - clip.y / clip.w) * 0.5f * g_viewHeight), 0.0f,
                      float2(g_viewWidth - 1, g_viewHeight - 1));
    }
    const float linearZ = max(distance, g_nearPlane);
    const float footprint = 2.0f * linearZ / (g_proj[1][1] * g_viewHeight);
    lp.lit = p.material == 1u;
    lp.colour = colour;
    lp.pos = pos;
    lp.phase = p.mediumPhase;
    lp.footprint = max(size * 0.5f, footprint);
    lp.linearZ = linearZ;
    lp.pixel = (uint2)pixel;
    rp.position = pos * c.streamAxes;  // (stream axes: FxRibbon builds the strip frame there and maps its vertices)
    rp.width = width;
    rp.age = age;
    rp.valid = 1u;
    pending.index = index;
    pending.record = rp;
    pending.alpha = alpha;
    return true;
}
void ribbonPointEnd(LayerConstants c, FxRibbonPending pending, float3 radiance)
{
    RWStructuredBuffer<FxRibbonPoint> points = ResourceDescriptorHeap[c.ribbonPoints];
    RWStructuredBuffer<uint2> appearance = ResourceDescriptorHeap[c.ribbonAppearance];
    points[pending.index] = pending.record;
    appearance[pending.index] = fxPackHalf4(float4(radiance * g_exposure, pending.alpha));
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
    // The particle material contract (0 emissive: colour = nit; 1 lit: colour = albedo): any other value is refused, not
    // guessed (an old material-table index drew as emissive at the wrong scale).
    const bool badMaterial = p.output == FX_OUTPUT_SPRITE && p.material > 1u;
    if (WaveActiveAnyTrue(badMaterial) && WaveIsFirstLane()) fxLayerStatus(c, FX_LAYER_STATUS_MATERIAL);
    const bool badRibbon = p.output == FX_OUTPUT_RIBBON && p.material > 1u;
    if (WaveActiveAnyTrue(badRibbon) && WaveIsFirstLane()) fxLayerStatus(c, FX_LAYER_STATUS_MATERIAL);
    // Either output's first half, up to its lighting request (FxLitPoint): a ribbon point, or a sprite
    const bool ribbon = p.output == FX_OUTPUT_RIBBON;
    FxRibbonPending pending = (FxRibbonPending)0;
    FxLitPoint lp = (FxLitPoint)0;
    float2 centre = 0;
    float alpha = 0, r2 = 0, rEff2 = 1, rEff = 0, linearZ = 0;
    if (ribbon)
    {
        if (!ribbonPointBegin(c, rr, k, birth, row, e, p, !badRibbon && (e.flags & FX_EMITTER_KILLED) == 0u && p.lifetime > 0, pending, lp)) return rec;
    }
    else
    {
        if (badMaterial || p.output != FX_OUTPUT_SPRITE || (e.flags & FX_EMITTER_KILLED) != 0u || !(p.lifetime > 0)) return rec;

        float3 pos;
        float age;
        bool dying;
        if (!fxParticleAt(c, rr, k, birth, row, p, pos, age, dying)) return rec;

        // appearance at that age
        const float u = saturate(age / p.lifetime);
        const float size = p.size * e.sizeScale * curve1(p.sizeKeys, p.sizeCount, u);
        const float3 colour = p.color.rgb * e.colorScale.rgb * curve3(p.colorKeys, p.colorCount, u);
        alpha = saturate(p.color.a * e.colorScale.a * curve1(p.alphaKeys, p.alphaCount, u));
        if (!(size > 0) || !(alpha > 0)) return rec;

        // projection (camera-relative: the view's rotation, then its projection)
        const float3 v = mul((float3x3)g_view, pos);
        const float distance = -v.z;
        if (!(distance > g_nearPlane)) return rec;
        const float4 clip = mul(g_proj, float4(v, 1));
        const float2 ndc = clip.xy / clip.w;
        centre = float2((ndc.x + 1) * 0.5f * g_viewWidth, (1 - ndc.y) * 0.5f * g_viewHeight);
        const float radius = 0.5f * size * g_proj[1][1] * 0.5f * g_viewHeight / distance;
        // Pixel-footprint prefilter: the pixel box filter widens the profile to r' = sqrt(r^2 + 1/4) (radius of a half pixel)
        // at the same integrated opacity (alpha r^2 / r'^2): a sprite below a pixel keeps its energy instead of being missed
        // by pixel centres.
        r2 = radius * radius;
        rEff2 = r2 + 0.25f;
        rEff = sqrt(rEff2);
        if (centre.x + rEff < 0 || centre.y + rEff < 0 || centre.x - rEff > g_viewWidth || centre.y - rEff > g_viewHeight) return rec;
        rec.centre = centre;
        rec.radius = rEff;
        rec.depth = g_nearPlane / distance;
        linearZ = distance;  // (v.z along the view axis: the view depth)
        const float footprint = 2.0f * distance / (g_proj[1][1] * g_viewHeight);
        lp.lit = p.material == 1u;
        lp.colour = colour;
        lp.pos = pos;
        lp.phase = p.mediumPhase;
        lp.footprint = max(size * 0.5f, footprint);
        lp.linearZ = linearZ;
        lp.pixel = (uint2)clamp(centre, 0, float2(g_viewWidth - 1, g_viewHeight - 1));
    }
    // lighting (material 1: lit; 0: emissive) - the kernel's one call
    float3 radiance = lp.colour;
    if (lp.lit) radiance = fxLitRadiance(c, lp.colour, lp.pos, normalize(lp.pos), lp.phase, lp.footprint, lp.pixel, lp.linearZ);
    if (ribbon)
    {
        ribbonPointEnd(c, pending, radiance);
        return rec;
    }
    // the sprite's second half: the air between the camera and the particle (S's air volume)
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
#if STEP == 0
    if (t >= c.threads) return;
#else
    if (t >= c.recordCapacity) return;
#endif
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
    // (the thread range covers the strip records too: FxLayerStrips at stripBase + point)
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
