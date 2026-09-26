// unx-kernel: cs_6_6 main
// unx-variants: STEP=0,1
// A3 FX particle lights (render A's contract with engine 2, NativeVfxStream.h NV_STREAM_PROGRAM_LIGHT; S and R read the
// tail, S_STATUS 10): every active emitter row whose program has the flag (emissive sprites, material 0) becomes one point
// light of this frame, derived from its particles at the frame time (the particle render pass's interpolation, FxParticleAt):
//   intensity per channel I = sum alpha L pi (s/2)^2 (radiance L in nit x the sprite's disc area: the flat disc's
//   intensity along its normal, which faces every viewer), luminance Y = the Rec.709 luminance of I;
//   position = the Y-weighted centre; colour = I / Y; size.x (spread) = the Y-weighted RMS distance from the centre plus
//   the Y-weighted mean particle radius; range = the distance where I / d^2 falls to one display code at this frame's
//   exposure for a white Lambertian surface (pi 2^-10 / exposure lux), so the window drops less than one code; no shadow.
//   STEP=0: one group per chunk (<= 2048 particles of one render range, ParticleSystem::lightTables): 8 particles per
//           thread, then a fixed-order tree reduction in group memory -> one partial per chunk (deterministic, no atomics).
//   STEP=1: one thread per slot sums its chunks in order and writes gpu::Light at lights[first + slot]; thread 0 writes
//           F = the slot count to the count word. Slots past F up to the capacity are not touched (readers stop at F).
// P[0] = { posAge cur, velocity cur, posAge prev, velocity prev }, P[1] = { dynamic cur, dynamic prev, emitters, programs },
// P[2] = { curve keys, render ranges, chunks (uint4), slots (uint2) }, P[3] = { partials UAV (float4 x 3 per chunk), lights
// UAV (raw), count UAV (raw), first light }, P[4] = { offsetCur.xyz, w }, P[5] = { offsetPrev.xyz, dt },
// P[6] = { streamAxes.xyz, slot count }, P[7] = { chunk count, 0, 0, 0 }; b1 = the main view (camera, exposure).
#include "Passes/FX/ParticleLayerPass.hlsli"
#include "Passes/FX/FxParticleAt.hlsli"
#include "Scene.hlsli"

static uint s_curveKeys;
float4 fxLightCurveKey(uint i)
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
#define NV_CURVE_KEY(i) fxLightCurveKey(i)
#include "Passes/FX/Stream/shaders/VfxParticleMath.hlsli"

float lightCurve1(uint first, uint count, float u) { return count >= 2u ? nv_curve(first, count, u).y : 1.0f; }
float3 lightCurve3(uint first, uint count, float u) { return count >= 2u ? nv_curve(first, count, u).yzw : float3(1, 1, 1); }

LayerConstants lightConstants()
{
    LayerConstants c = (LayerConstants)0;
    c.posAgeCur = P[0].x, c.velocityCur = P[0].y, c.posAgePrev = P[0].z, c.velocityPrev = P[0].w;
    c.dynamicCur = P[1].x, c.dynamicPrev = P[1].y, c.emitters = P[1].z, c.programs = P[1].w;
    c.curveKeys = P[2].x, c.ranges = P[2].y;
    c.offsetCur = asfloat(P[4].xyz), c.w = asfloat(P[4].w);
    c.offsetPrev = asfloat(P[5].xyz), c.dt = asfloat(P[5].w);
    c.streamAxes = asfloat(P[6].xyz);
    return c;
}

// Sums of a set of particles: i = sum I (rgb), y = sum Y, yp = sum Y p, yp2 = sum Y |p|^2, yr = sum Y r.
struct LightSums { float3 i; float y; float3 yp; float yp2; float yr; };
LightSums lightZero() { LightSums s = (LightSums)0; return s; }
LightSums lightAdd(LightSums a, LightSums b)
{
    a.i += b.i, a.y += b.y, a.yp += b.yp, a.yp2 += b.yp2, a.yr += b.yr;
    return a;
}

#if STEP == 0
groupshared float4 gs0[256], gs1[256], gs2[256];

[numthreads(256, 1, 1)]
void main(uint tid : SV_GroupIndex, uint3 gid : SV_GroupID)
{
    const LayerConstants c = lightConstants();
    s_curveKeys = c.curveKeys;
    StructuredBuffer<uint4> chunks = ResourceDescriptorHeap[P[2].z];
    const uint4 chunk = chunks[gid.x];  // (range, offset, count, slot)
    StructuredBuffer<RenderRange> ranges = ResourceDescriptorHeap[c.ranges];
    const RenderRange rr = ranges[chunk.x];
    StructuredBuffer<StreamEmitter> emitters = ResourceDescriptorHeap[c.emitters];
    StructuredBuffer<StreamProgram> programs = ResourceDescriptorHeap[c.programs];
    const StreamEmitter e = emitters[rr.row];
    const StreamProgram p = programs[e.program];
    LightSums s = lightZero();
    const bool live = (e.flags & FX_EMITTER_KILLED) == 0u && p.lifetime > 0;
    [loop] for (uint j = 0; j < 8u; ++j)  // 2048 = 8 x 256 particles at most (ParticleSystem::kLightChunk)
    {
        const uint i = tid + j * 256u;
        if (!live || i >= chunk.z) break;
        const uint k = chunk.y + i, birth = rr.first + k;
        float3 pos;
        float age;
        bool dying;
        if (!fxParticleAt(c, rr, k, birth, rr.row, p, pos, age, dying)) continue;
        const float u = saturate(age / p.lifetime);
        const float size = max(p.size * e.sizeScale * lightCurve1(p.sizeKeys, p.sizeCount, u), 0.0f);
        const float alpha = saturate(p.color.a * e.colorScale.a * lightCurve1(p.alphaKeys, p.alphaCount, u));
        const float3 radiance = max(p.color.rgb * e.colorScale.rgb * lightCurve3(p.colorKeys, p.colorCount, u), 0.0f);
        const float3 I = radiance * (alpha * 0.25f * 3.14159265f * size * size);
        const float Y = dot(I, float3(0.2126f, 0.7152f, 0.0722f));
        if (!(Y > 0)) continue;
        LightSums one;
        one.i = I, one.y = Y, one.yp = Y * pos, one.yp2 = Y * dot(pos, pos), one.yr = Y * 0.5f * size;
        s = lightAdd(s, one);
    }
    gs0[tid] = float4(s.i, s.y);
    gs1[tid] = float4(s.yp, s.yp2);
    gs2[tid] = float4(s.yr, 0, 0, 0);
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint stride = 128u; stride > 0u; stride >>= 1)
    {
        if (tid < stride)
        {
            gs0[tid] += gs0[tid + stride];
            gs1[tid] += gs1[tid + stride];
            gs2[tid] += gs2[tid + stride];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (tid == 0)
    {
        RWStructuredBuffer<float4> partials = ResourceDescriptorHeap[P[3].x];
        partials[gid.x * 3u] = gs0[0];
        partials[gid.x * 3u + 1u] = gs1[0];
        partials[gid.x * 3u + 2u] = gs2[0];
    }
}
#else
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint slot = id.x, slots = P[6].w;
    if (slot == 0)
    {
        RWByteAddressBuffer count = ResourceDescriptorHeap[P[3].z];
        count.Store(0, slots);
    }
    if (slot >= slots) return;
    StructuredBuffer<uint2> slotChunks = ResourceDescriptorHeap[P[2].w];
    StructuredBuffer<float4> partials = ResourceDescriptorHeap[P[3].x];
    const uint2 sc = slotChunks[slot];  // (first chunk, chunks)
    LightSums s = lightZero();
    [loop] for (uint k = 0; k < sc.y; ++k)  // bounded by the slot's chunks (a row's particles / 2048)
    {
        const float4 a = partials[(sc.x + k) * 3u], b = partials[(sc.x + k) * 3u + 1u], r = partials[(sc.x + k) * 3u + 2u];
        LightSums one;
        one.i = a.xyz, one.y = a.w, one.yp = b.xyz, one.yp2 = b.w, one.yr = r.x;
        s = lightAdd(s, one);
    }
    GpuLight l = (GpuLight)0;
    const float Y = s.y;
    const float3 centre = Y > 0 ? s.yp / Y : float3(0, 0, 0);  // camera-relative
    const float variance = Y > 0 ? max(s.yp2 / Y - dot(centre, centre), 0.0f) : 0.0f;
    l.position = g_cameraPosition + centre;
    l.typeFlags = LIGHT_POINT | (0xFFFFu << 16);  // no shadow
    l.forward = float3(0, 0, 1);
    l.right = float3(1, 0, 0);
    l.intensity = Y;
    l.color = Y > 0 ? s.i / Y : float3(0, 0, 0);
    l.range = Y > 0 ? sqrt(Y * g_exposure / (3.14159265f * (1.0f / 1024.0f))) : 0.0f;
    l.size = float2(sqrt(variance) + (Y > 0 ? s.yr / Y : 0.0f), 0);
    l.spotScale = 1;
    l.spotOffset = 0;
    l.revision = 0;
    RWByteAddressBuffer lights = ResourceDescriptorHeap[P[3].y];
    lights.Store<GpuLight>((P[3].w + slot) * 80u, l);
}
#endif
