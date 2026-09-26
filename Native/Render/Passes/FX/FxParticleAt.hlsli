// The particle render pass's particle at the frame time (render rules request 2), shared by the sprite/ribbon setup
// (FxLayerSetup) and the mesh particle instance writer (FxMeshInstances): the render range of a render thread and the
// particle's camera-relative position and age by the tick interpolation rules below.
#ifndef FX_PARTICLE_AT_HLSLI
#define FX_PARTICLE_AT_HLSLI
#include "Passes/FX/ParticleLayerPass.hlsli"

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

// The particle at the frame time, relative to the camera in the renderer's axes (the stream's positions and the anchor
// offsets are in stream space; c.streamAxes maps the result), and its age (false: not alive then). A particle of both ticks by
// cubic Hermite of the two ends' positions and velocities; one born in the latest tick by p_n - v_n (1 - w) dt; one that died
// in it by p_(n-1) + v_(n-1) w dt while w dt < lifetime - age_(n-1). 'dying' = a particle of the previous state only.
bool fxParticleAt(LayerConstants c, RenderRange rr, uint k, uint birth, uint row, StreamProgram p, out float3 pos, out float age, out bool dying)
{
    pos = 0;
    age = 0;
    dying = (rr.prevCountFlags & 0x80000000u) != 0u;
    const float wdt = c.w * c.dt, rest = (1.0f - c.w) * c.dt;
    if (dying)
    {
        StructuredBuffer<float4> posAge = ResourceDescriptorHeap[c.posAgePrev];
        StructuredBuffer<float4> velocity = ResourceDescriptorHeap[c.velocityPrev];
        StructuredBuffer<EmitterDynamic> dynamic = ResourceDescriptorHeap[c.dynamicPrev];
        const float4 pa = posAge[rr.stateBase + k];
        if (!(wdt < p.lifetime - pa.w)) return false;  // dead by the frame time
        pos = (c.offsetPrev + dynamic[row].originAnchor + pa.xyz + velocity[rr.stateBase + k].xyz * wdt) * c.streamAxes;
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
            pos = ((2 * w3 - 3 * w2 + 1) * p0 + (w3 - 2 * w2 + w) * c.dt * v0 + (3 * w2 - 2 * w3) * p1 + (w3 - w2) * c.dt * v1) * c.streamAxes;
        }
        else
        {
            if (age < 0) return false;  // born after the frame time
            pos = (p1 - v1 * rest) * c.streamAxes;
        }
    }
    return true;
}
#endif
