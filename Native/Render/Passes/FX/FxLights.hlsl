// unx-kernel: cs_6_6 main
// unx-variants: STEP=0,1,2,3,4,5,6
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
// Per-particle lights (fx.particles.particle_lights_max = N > 0; P[7].w): Unreal's Niagara light renderer makes every
// particle a light; here the N particles that matter most at the camera are lights of their own, after the rows', and
// the rest stay in their row's light (the row's sums leave the chosen ones out, so no light is counted twice). A
// particle's importance is its intensity's luminance over its squared distance from the camera (not below its
// radius): what it can add to a surface beside the viewer. The N most important are found without a sort, the same
// set whatever the threads' order: a histogram of the importance in quarter octaves (STEP 3 clears it, STEP 2 fills
// it), the highest bins that together hold at most N particles (STEP 4: the cut bin; a bin that would pass N is left
// out whole), then STEP 0 as above with each chunk's count of chosen particles, STEP 5 the chunks' first light (a
// running sum in chunk order: the lights' order is the chunks', then the threads') and F = slots + chosen, and STEP 6
// the chosen particles' lights: the particle's own intensity, colour and position, its radius as the light's size,
// the row light's range rule, no shadow.
// P[7] = { chunk count, histogram (STEP 2, 3: UAV; 4: SRV; raw, 256 words), cut (raw: word 0 the cut bin, 1 the chosen
//          count, 2.. each chunk's first light; STEP 4, 5: UAV; 0, 6: SRV), N }
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

// Particle k of a render range as a light source at the frame time: its camera-relative position, intensity I (rgb),
// its luminance Y and radius. false: not alive, or it emits nothing.
bool lightParticle(LayerConstants c, RenderRange rr, StreamEmitter e, StreamProgram p, uint k, out float3 pos, out float3 I, out float Y, out float radius)
{
    I = 0;
    Y = 0;
    radius = 0;
    float age;
    bool dying;
    if (!fxParticleAt(c, rr, k, rr.first + k, rr.row, p, pos, age, dying)) return false;
    const float u = saturate(age / p.lifetime);
    const float size = max(p.size * e.sizeScale * lightCurve1(p.sizeKeys, p.sizeCount, u), 0.0f);
    const float alpha = saturate(p.color.a * e.colorScale.a * lightCurve1(p.alphaKeys, p.alphaCount, u));
    const float3 radiance = max(p.color.rgb * e.colorScale.rgb * lightCurve3(p.colorKeys, p.colorCount, u), 0.0f);
    I = radiance * (alpha * 0.25f * 3.14159265f * size * size);
    Y = dot(I, float3(0.2126f, 0.7152f, 0.0722f));
    radius = 0.5f * size;
    return Y > 0;
}
// The importance bin of a particle's light (quarter octaves of Y / max(distance^2, radius^2); 0..255).
uint lightBin(float Y, float3 pos, float radius)
{
    const float importance = Y / max(max(dot(pos, pos), radius * radius), 1e-12f);
    return (uint)clamp(floor(log2(max(importance, 1e-30f)) * 4.0f) + 128.0f, 0.0f, 255.0f);
}
// The cut bin of this frame (particles of this bin and above are lights of their own); no per-particle lights: none.
uint lightCut()
{
    if (P[7].w == 0u) return 0xFFFFFFFFu;
    ByteAddressBuffer cut = ResourceDescriptorHeap[P[7].z];
    return cut.Load(0);
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
    const uint cutBin = lightCut();
    float chosen = 0;  // the chunk's particles that are lights of their own (STEP 6 writes them)
    [loop] for (uint j = 0; j < 8u; ++j)  // 2048 = 8 x 256 particles at most (ParticleSystem::kLightChunk)
    {
        const uint i = tid + j * 256u;
        if (!live || i >= chunk.z) break;
        float3 pos, I;
        float Y, radius;
        if (!lightParticle(c, rr, e, p, chunk.y + i, pos, I, Y, radius)) continue;
        if (lightBin(Y, pos, radius) >= cutBin)
        {
            chosen += 1;
            continue;
        }
        LightSums one;
        one.i = I, one.y = Y, one.yp = Y * pos, one.yp2 = Y * dot(pos, pos), one.yr = Y * radius;
        s = lightAdd(s, one);
    }
    gs0[tid] = float4(s.i, s.y);
    gs1[tid] = float4(s.yp, s.yp2);
    gs2[tid] = float4(s.yr, chosen, 0, 0);
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
#elif STEP == 2
// the histogram of the particles' importance bins
[numthreads(256, 1, 1)]
void main(uint tid : SV_GroupIndex, uint3 gid : SV_GroupID)
{
    const LayerConstants c = lightConstants();
    s_curveKeys = c.curveKeys;
    StructuredBuffer<uint4> chunks = ResourceDescriptorHeap[P[2].z];
    const uint4 chunk = chunks[gid.x];
    StructuredBuffer<RenderRange> ranges = ResourceDescriptorHeap[c.ranges];
    const RenderRange rr = ranges[chunk.x];
    StructuredBuffer<StreamEmitter> emitters = ResourceDescriptorHeap[c.emitters];
    StructuredBuffer<StreamProgram> programs = ResourceDescriptorHeap[c.programs];
    const StreamEmitter e = emitters[rr.row];
    const StreamProgram p = programs[e.program];
    if ((e.flags & FX_EMITTER_KILLED) != 0u || !(p.lifetime > 0)) return;
    RWByteAddressBuffer histogram = ResourceDescriptorHeap[P[7].y];
    [loop] for (uint j = 0; j < 8u; ++j)
    {
        const uint i = tid + j * 256u;
        if (i >= chunk.z) break;
        float3 pos, I;
        float Y, radius;
        if (lightParticle(c, rr, e, p, chunk.y + i, pos, I, Y, radius)) histogram.InterlockedAdd(4u * lightBin(Y, pos, radius), 1u);
    }
}
#elif STEP == 3
[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    RWByteAddressBuffer histogram = ResourceDescriptorHeap[P[7].y];
    histogram.Store(4u * id.x, 0u);
}
#elif STEP == 4
// the cut bin: the highest bins that together hold at most N particles
[numthreads(1, 1, 1)]
void main()
{
    ByteAddressBuffer histogram = ResourceDescriptorHeap[P[7].y];
    RWByteAddressBuffer cut = ResourceDescriptorHeap[P[7].z];
    uint total = 0u, bin = 256u;
    [loop] for (uint b = 256u; b > 0u; --b)
    {
        const uint n = histogram.Load(4u * (b - 1u));
        if (total + n > P[7].w) break;
        total += n;
        bin = b - 1u;
    }
    cut.Store2(0, uint2(bin, total));
}
#elif STEP == 5
// each chunk's first particle light (the chunks' chosen counts summed in chunk order), and F
[numthreads(1, 1, 1)]
void main()
{
    StructuredBuffer<float4> partials = ResourceDescriptorHeap[P[3].x];
    RWByteAddressBuffer cut = ResourceDescriptorHeap[P[7].z];
    uint running = 0u;
    [loop] for (uint chunk = 0u; chunk < P[7].x; ++chunk)
    {
        cut.Store(8u + 4u * chunk, running);
        running += (uint)partials[chunk * 3u + 2u].y;
    }
    RWByteAddressBuffer count = ResourceDescriptorHeap[P[3].z];
    count.Store(0, P[6].w + min(running, P[7].w));
}
#elif STEP == 6
// the chosen particles' lights
groupshared uint gs_first[256];

[numthreads(256, 1, 1)]
void main(uint tid : SV_GroupIndex, uint3 gid : SV_GroupID)
{
    const LayerConstants c = lightConstants();
    s_curveKeys = c.curveKeys;
    StructuredBuffer<uint4> chunks = ResourceDescriptorHeap[P[2].z];
    const uint4 chunk = chunks[gid.x];
    StructuredBuffer<RenderRange> ranges = ResourceDescriptorHeap[c.ranges];
    const RenderRange rr = ranges[chunk.x];
    StructuredBuffer<StreamEmitter> emitters = ResourceDescriptorHeap[c.emitters];
    StructuredBuffer<StreamProgram> programs = ResourceDescriptorHeap[c.programs];
    const StreamEmitter e = emitters[rr.row];
    const StreamProgram p = programs[e.program];
    const bool live = (e.flags & FX_EMITTER_KILLED) == 0u && p.lifetime > 0;
    ByteAddressBuffer cut = ResourceDescriptorHeap[P[7].z];
    const uint cutBin = cut.Load(0), base = cut.Load(8u + 4u * gid.x);
    // this thread's chosen particles, and how many the threads before it have (an exclusive scan over the group)
    float3 positions[8], intensities[8];
    float radii[8];
    uint mine = 0u;
    [loop] for (uint j = 0; j < 8u; ++j)
    {
        const uint i = tid + j * 256u;
        if (!live || i >= chunk.z) break;
        float3 pos, I;
        float Y, radius;
        if (!lightParticle(c, rr, e, p, chunk.y + i, pos, I, Y, radius) || lightBin(Y, pos, radius) < cutBin) continue;
        positions[mine] = pos;
        intensities[mine] = I;
        radii[mine] = radius;
        ++mine;
    }
    gs_first[tid] = mine;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint stride = 1u; stride < 256u; stride <<= 1)
    {
        const uint before = tid >= stride ? gs_first[tid - stride] : 0u;
        GroupMemoryBarrierWithGroupSync();
        gs_first[tid] += before;
        GroupMemoryBarrierWithGroupSync();
    }
    const uint first = base + gs_first[tid] - mine;
    RWByteAddressBuffer lights = ResourceDescriptorHeap[P[3].y];
    [loop] for (uint n = 0u; n < mine; ++n)
    {
        if (first + n >= P[7].w) break;
        const float Y = dot(intensities[n], float3(0.2126f, 0.7152f, 0.0722f));
        GpuLight l = (GpuLight)0;
        l.position = g_cameraPosition + positions[n];
        l.typeFlags = LIGHT_POINT | (0xFFFFu << 16);  // no shadow
        l.forward = float3(0, 0, 1);
        l.right = float3(1, 0, 0);
        l.intensity = Y;
        l.color = intensities[n] / Y;
        l.range = sqrt(Y * g_exposure / (3.14159265f * (1.0f / 1024.0f)));
        l.size = float2(radii[n], 0);
        l.spotScale = 1;
        l.spotOffset = 0;
        l.revision = 0;
        lights.Store<GpuLight>((P[3].w + P[6].w + first + n) * 112u, l);
    }
}
#else
[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint slot = id.x, slots = P[6].w;
    if (slot == 0 && P[7].w == 0u)
    {
        // (with per-particle lights STEP 5 writes F: the slots and the chosen particles)
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
    lights.Store<GpuLight>((P[3].w + slot) * 112u, l);  // (sizeof(gpu::Light); the light components stay 0: a plain light)
}
#endif
