// unx-kernel: cs_6_6 main
// unx-variants: STEP=0,1
// Particle render pass, per particle of the latest tick's render ranges (ParticleLayerPass.hlsli RenderRange):
//   STEP=0: the particle at the frame time (render rules request 2: a particle of both ticks by cubic Hermite of the two
//           ends' positions and velocities, one born in the latest tick by p_n - v_n (1 - w) dt from age_n - (1 - w) dt >= 0,
//           one that died in it by p_(n-1) + v_(n-1) w dt while w dt < lifetime - age_(n-1)), its appearance at that age
//           (size, colour, opacity: pure functions of program, emitter and age), the projection, the pixel-footprint
//           prefilter and the record; then the tile counts of the record's square.
//   STEP=1: the tile entries (depth bits, record index) at tile start + atomic fill (the tile kernel sorts them).
// Sprites only for now (M0); other outputs write an undrawn record. Emission only (lighting follows: request 3).
#include "Passes/FX/ParticleLayerPass.hlsli"

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
    rec.radianceAlpha = fxPackHalf4(float4(colour * g_exposure, alpha * r2 / rEff2));
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
