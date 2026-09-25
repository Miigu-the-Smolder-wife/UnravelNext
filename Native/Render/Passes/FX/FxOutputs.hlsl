// unx-kernel: cs_6_6 main
// unx-variants: STEP=0,1
// Geometry outputs of the tick (WORLD_VFX_DESIGN_KO.md 3.5), after compaction. The live births of an emitter are the
// contiguous range [death_birth, next_birth), so a live particle's output index is output_base + (birth - death_birth)
// (NativeVfxStream.h): no sort.
//   STEP=0 thread per alive-list entry:
//     NV_RIBBON: ribbon point {origin-space position, width = size (full), age} at its index;
//     NV_VOLUME: grid^3 medium cells (NV_MediumCell layout, 96 B) at cell index x grid^3 (output_base counts medium
//                cells: first cell = output_base + (birth - death_birth) x grid^3): cuboid of side = size around the
//                particle, separable tent mass over the grid, coefficients = program coefficients x density (colour alpha
//                scales mass, RGB tints emission) divided by the published support volume (the old VfxMediaShader rules,
//                in float). Cell coordinates: integer 1024 m cells of the anchor space + float offsets inside the cell.
//   STEP=1 thread per emitter row with a NV_RIBBON program: walks its points in birth order and writes two vertices
//     per point (NV_RibbonVertex: origin-space position, normal, uv) with the transported side frame, and a link per
//     point (the previous point of its strip, or NONE at a strip start / isolated point), as the old VfxRibbonShader.
// P[0] = (points SRV/UAV, links UAV, vertices UAV, cells UAV); P[1].x ribbon point capacity, .y medium cell capacity
#include "Passes/FX/Particles.hlsli"

#define FX_OUTPUT_RIBBON 2u
#define FX_OUTPUT_VOLUME 3u
struct RibbonPoint { float3 position; float width; float age; uint valid; uint pad0, pad1; };  // 32 B
struct RibbonVertex { float3 position; float3 normal; float2 uv; };                             // 32 B (NV_RibbonVertex)
struct MediumCell { int4 cell; float4 low, high, absorption, scattering, emission; };           // 96 B (NV_MediumCell)

float fxCurve1(uint first, uint count, float u) { return count >= 2u ? nv_curve(first, count, u).y : 1.0f; }
float3 fxCurve3(uint first, uint count, float u) { return count >= 2u ? nv_curve(first, count, u).yzw : float3(1, 1, 1); }

#if STEP == 0
float tentMass(uint k, uint n)
{
    const float a = (float)k / (float)n * 2.0f - 1.0f, b = (float)(k + 1u) / (float)n * 2.0f - 1.0f;
    const float ca = a <= 0 ? 0.5f * (a + 1) * (a + 1) : 1 - 0.5f * (1 - a) * (1 - a);
    const float cb = b <= 0 ? 0.5f * (b + 1) * (b + 1) : 1 - 0.5f * (1 - b) * (1 - b);
    return cb - ca;
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    FX_RWBUFFER(uint, counters, g_counters);
    if (id.x >= counters[FX_COUNTER_ALIVE]) return;
    FX_RWBUFFER(uint, aliveList, g_aliveList);
    FX_RWBUFFER(float4, posAge, g_posAge);
    FX_RWBUFFER(uint2, meta, g_meta);
    FX_BUFFER(StreamEmitter, emitters, g_emitters);
    FX_BUFFER(StreamProgram, programs, g_programs);
    FX_RWBUFFER(EmitterDynamic, dynamic, g_emitterDynamic);
    const uint slot = aliveList[id.x];
    const uint2 m = meta[slot];
    if (m.x >= g_emitterCount) return;
    const StreamEmitter e = emitters[m.x];
    const StreamProgram p = programs[e.program];
    if (p.output != FX_OUTPUT_RIBBON && p.output != FX_OUTPUT_VOLUME) return;
    const float4 pa = posAge[slot];
    const float u = saturate(pa.w / p.lifetime);
    const float size = p.size * e.sizeScale * fxCurve1(p.sizeKeys, p.sizeCount, u);
    uint index = e.outputBase + (m.y - e.deathBirth);
    if (p.output == FX_OUTPUT_RIBBON)
    {
        if (index >= P[1].x) { fxStatus(FX_STATUS_RANGE); return; }
        FX_RWBUFFER(RibbonPoint, points, P[0].x);
        RibbonPoint rp;
        rp.position = pa.xyz; rp.width = size; rp.age = pa.w; rp.valid = 1u; rp.pad0 = rp.pad1 = 0u;
        points[index] = rp;
        return;
    }
    const uint n = clamp(p.mediumGrid, 1u, 32u), cells = n * n * n;
    const uint first = e.outputBase + (m.y - e.deathBirth) * cells;  // output_base counts medium cells (NativeVfxStream.h)
    if (cells > P[1].y || first > P[1].y - cells) { fxStatus(FX_STATUS_RANGE); return; }
    const float4 colour = float4(p.color.rgb * e.colorScale.rgb * fxCurve3(p.colorKeys, p.colorCount, u),
                                 p.color.a * e.colorScale.a * fxCurve1(p.alphaKeys, p.alphaCount, u));
    const float3 q = dynamic[m.x].originAnchor + pa.xyz;
    const int3 cell = (int3)floor(q / 1024.0f);
    const float3 centre = q - (float3)cell * 1024.0f;
    FX_RWBUFFER(MediumCell, cellsOut, P[0].w);
    for (uint k = 0u; k < cells; ++k)
    {
        const uint3 c = uint3(k % n, (k / n) % n, k / (n * n));
        const float3 lo = centre + size * ((float3)c / (float)n - 0.5f), hi = centre + size * ((float3)(c + 1u) / (float)n - 0.5f);
        const float volume = (hi.x - lo.x) * (hi.y - lo.y) * (hi.z - lo.z);
        const float density = colour.a * tentMass(c.x, n) * tentMass(c.y, n) * tentMass(c.z, n) / max(volume, 1e-30f);
        MediumCell mc;
        mc.cell = int4(cell, 0);
        mc.low = float4(lo, 0);
        mc.high = float4(hi, 0);
        mc.absorption = float4(p.mediumAbsorption * density, p.mediumPhase);
        mc.scattering = float4(p.mediumScattering * density, 0);
        mc.emission = float4(p.mediumEmission * density * colour.rgb, 0);
        cellsOut[first + k] = mc;
    }
}
#else
// STEP == 1: ribbon strips (one thread walks one emitter's points in birth order)
float3 unitOr(float3 v, float3 fallback) { const float l = length(v); return l > 1e-12f ? v / l : fallback; }
float3 initialSide(float3 tangent, float3 normal)
{
    float3 side = cross(tangent, normal);
    if (length(side) < 1e-6f)
    {
        const float3 a = abs(tangent);
        const float3 alt = a.x <= a.y && a.x <= a.z ? float3(1, 0, 0) : (a.y <= a.z ? float3(0, 1, 0) : float3(0, 0, 1));
        side = cross(tangent, alt);
    }
    return unitOr(side, float3(1, 0, 0));
}
float3 transportSide(float3 before, float3 after, float3 side)
{
    const float c = clamp(dot(before, after), -1.0f, 1.0f);
    if (c > -1.0f + 1e-6f)
    {
        const float3 axis = cross(before, after), first = cross(axis, side);
        side += first + cross(axis, first) / (1.0f + c);
    }
    side -= after * dot(side, after);
    return length(side) > 1e-6f ? normalize(side) : initialSide(after, before);
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_emitterCount) return;
    FX_BUFFER(StreamEmitter, emitters, g_emitters);
    FX_BUFFER(StreamProgram, programs, g_programs);
    const StreamEmitter e = emitters[id.x];
    if ((e.flags & FX_EMITTER_ACTIVE) == 0u) return;
    const StreamProgram p = programs[e.program];
    if (p.output != FX_OUTPUT_RIBBON) return;
    FX_RWBUFFER(RibbonPoint, points, P[0].x);
    FX_RWBUFFER(uint, links, P[0].y);
    FX_RWBUFFER(RibbonVertex, vertices, P[0].z);
    const uint count = e.nextBirth - e.deathBirth, first = e.outputBase;
    if (count > P[1].x || first > P[1].x - count) { fxStatus(FX_STATUS_RANGE); return; }
    const float3 normal = p.ribbonNormal;
    const float limit = p.ribbonBreak, uvScale = p.ribbonUv > 0 ? p.ribbonUv : 1.0f;
    float3 priorTangent = normal, side = normal;
    float distance = 0;
    uint previous = FX_NONE;
    for (uint j = 0u; j < count; ++j)
    {
        const uint at = first + j;
        const RibbonPoint pt = points[at];
        links[at] = FX_NONE;
        RibbonVertex v;
        v.position = pt.position; v.normal = normal; v.uv = float2(0, 0);
        vertices[at * 2u] = v;
        v.uv.y = 1;
        vertices[at * 2u + 1u] = v;
        // a run of coincident points is represented by its last point (the others keep their default vertices)
        if (j + 1u < count && all(points[at + 1u].position == pt.position)) continue;
        const bool hasNext = j + 1u < count;
        const float3 nextPos = hasNext ? points[at + 1u].position : pt.position;
        bool before = previous != FX_NONE, after = hasNext;
        float3 incoming = float3(0, 0, 0), outgoing = float3(0, 0, 0);
        float segment = 0;
        if (before)
        {
            incoming = pt.position - points[previous].position;
            segment = length(incoming);
            if (segment > limit) before = false; else incoming /= segment;
        }
        if (after)
        {
            outgoing = nextPos - pt.position;
            const float span = length(outgoing);
            if (span > limit) after = false; else outgoing /= span;
        }
        if (before || after)
        {
            const float3 tangent = before ? (after ? unitOr(incoming + outgoing, outgoing) : incoming) : outgoing;
            if (before) { side = transportSide(priorTangent, tangent, side); distance += segment; links[at] = previous; }
            else { distance = 0; side = initialSide(tangent, normal); }
            priorTangent = tangent;
            const float3 n = unitOr(cross(side, tangent), normal);
            const float halfWidth = 0.5f * pt.width;
            v.position = pt.position - side * halfWidth; v.normal = n; v.uv = float2(distance / uvScale, 0);
            vertices[at * 2u] = v;
            v.position = pt.position + side * halfWidth; v.uv.y = 1;
            vertices[at * 2u + 1u] = v;
        }
        previous = at;
    }
}
#endif
