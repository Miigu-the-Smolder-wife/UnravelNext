// unx-kernel: cs_6_6 main
// Mesh particles (A3, FEATURES_GAME 0.A 7b): every live particle of a mesh program (FX_OUTPUT_MESH) becomes one scene
// instance in C's GPU-written instance range (GpuScene::gpuInstanceRange, INTERFACES v1.58), before V's culling. From there
// it is a dynamic instance like any other (two-phase HiZ, LOD, bands, S shadows); R's TLAS is a CPU list and omits it.
//
// Per render thread of the latest tick (the particle render pass's layout, ParticleLayerPass.hlsli RenderRange):
//   position  fxParticleAt at the frame time (FxParticleAt.hlsli: Hermite across the tick, births and deaths extrapolated),
//             relative to the frame origin (LayerConstants offsets = stream anchor - frame origin);
//   rotation  the stream's orientation state (NV_StreamParticleOrientation, executor version 4): across the tick
//             q(w) = r^w (x) exp(w0 w dt / 2) q0, r = q1 (x) conj(exp(w0 dt / 2) q0) the tick's transport (identity without
//             one), so q(0) = q0 and q(1) = q1 exactly (continuous at every tick boundary, impacts included: they damp the
//             spin after the advance); a birth of the tick exp(-w1 (1 - w) dt / 2) q1; a death of the tick exp(w0 w dt / 2) q0;
//   scale     program size x emitter sizeScale x size curve at the age (the mesh's own size is 1);
//   axes      the stream's rotation R_s mapped to the renderer's axes R_r = M R_s M, M = diag(streamAxes).
// The previous transform is the one this particle was drawn with in the previous frame: the same rules at that frame's w
// when the latest tick did not change (mode 1), that frame's drawn record (MeshDrawn, indexed by the latest layout) when
// exactly one tick followed it (mode 2); without either (a birth since, a RESET, the first frame) it is the particle
// moved back over the frame time with its velocity and spin.
// The mesh is the program's 64-bit asset key (StreamProgram.reserved6.xy = mesh_asset lo/hi) looked up in the host's
// sorted table (UnxVfxMapMeshAsset); a key without a mesh is not drawn and counted.
#include "Passes/FX/FxParticleAt.hlsli"

static uint s_curveKeys;
float4 fxMeshCurveKey(uint i)
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
#define NV_CURVE_KEY(i) fxMeshCurveKey(i)
#include "Passes/FX/Stream/shaders/VfxParticleMath.hlsli"

#define FX_MESH_PROGRAM_ORIENTATION 256u  // NV_STREAM_PROGRAM_ORIENTATION (MeshOrientation.hlsli FX_PROGRAM_ORIENTATION)
#define FX_MESH_NONE 0xFFFFFFFFu
// counters[]
#define FX_MESH_COUNTER_INSTANCES 0u  // instances written
#define FX_MESH_COUNTER_OVERFLOW 1u   // live mesh particles past the range's capacity (not drawn)
#define FX_MESH_COUNTER_UNMAPPED 2u   // live mesh particles whose program's asset key has no mesh (not drawn)
#define FX_MESH_COUNTER_STATUS 3u
#define FX_MESH_STATUS_NO_ORIENTATION 1u  // a mesh program without NV_STREAM_PROGRAM_ORIENTATION state (not drawn)

struct MeshConstants
{
    LayerConstants layer;  // 256 B: the fields fxParticleAt and renderRange read; offsets relative to the frame origin
    uint orientationCur, orientationPrev, drawnIn, drawnOut;
    uint instances, count, countOffset, first;  // raw UAVs of the scene's instance buffer and of the count's buffer
    uint capacity, assets, assetCount, counters;
    uint mode; float wHistory; float frameDt; uint revision;  // mode: 0 none, 1 same tick (w of the previous frame), 2 next tick
    float3 originDelta; uint pad0;  // previous frame origin - this frame's (renderer axes)
};
struct ParticleOrientation { float4 rotation; float3 spin; float reserved0; };  // 32 B (NV_StreamParticleOrientation)
struct MeshDrawn { float3 position; float scale; float4 rotation; };            // 32 B: stream rotation; scale <= 0: not drawn

MeshConstants meshConstants()
{
    StructuredBuffer<MeshConstants> c = ResourceDescriptorHeap[P[0].x];
    return c[0];
}
void meshCount(MeshConstants mc, uint word, uint n)
{
    RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[mc.counters];
    InterlockedAdd(counters[word], n);
}

float meshCurve1(uint first, uint count, float u) { return count >= 2u ? nv_curve(first, count, u).y : 1.0f; }
float meshScale(StreamProgram p, StreamEmitter e, float age) { return p.size * e.sizeScale * meshCurve1(p.sizeKeys, p.sizeCount, saturate(age / p.lifetime)); }

// The program's asset key -> the scene mesh (table sorted by (hi, lo); FX_MESH_NONE when absent).
uint meshOfAsset(MeshConstants mc, uint2 key)
{
    if (mc.assetCount == 0u) return FX_MESH_NONE;
    StructuredBuffer<uint4> table = ResourceDescriptorHeap[mc.assets];
    uint lo = 0u, hi = mc.assetCount;
    [loop] for (uint guard = 0u; guard < 32u && lo < hi; ++guard)
    {
        const uint mid = (lo + hi) >> 1;
        const uint4 a = table[mid];
        if (a.y < key.y || (a.y == key.y && a.x < key.x)) lo = mid + 1u; else hi = mid;
    }
    if (lo >= mc.assetCount) return FX_MESH_NONE;
    const uint4 a = table[lo];
    return all(a.xy == key) ? a.z : FX_MESH_NONE;
}

// r^w for a unit quaternion r (the shorter arc): a fraction w of its rotation about the same axis.
float4 quatPower(float4 r, float w)
{
    if (r.w < 0) r = -r;
    const float s = length(r.xyz);
    const float half = atan2(s, r.w) * w;
    return float4(r.xyz * (s > 1e-12f ? sin(half) / s : w), cos(half));
}

// The particle's rotation (stream space, local -> anchor) at frame fraction w and its spin then (rad/s).
float4 meshRotation(MeshConstants mc, RenderRange rr, uint k, uint birth, float w, out float3 spin)
{
    const float dt = mc.layer.dt;
    StructuredBuffer<ParticleOrientation> cur = ResourceDescriptorHeap[mc.orientationCur];
    StructuredBuffer<ParticleOrientation> prev = ResourceDescriptorHeap[mc.orientationPrev];
    if ((rr.prevCountFlags & 0x80000000u) != 0u)  // died in the latest tick
    {
        const ParticleOrientation o = prev[rr.stateBase + k];
        spin = o.spin;
        return nv_orientation_advance(o.rotation, o.spin, w * dt);
    }
    const ParticleOrientation o1 = cur[rr.stateBase + k];
    spin = o1.spin;
    const uint rel = birth - rr.prevFirst;
    if (rel < (rr.prevCountFlags & 0x7FFFFFFFu))
    {
        const ParticleOrientation o0 = prev[rr.prevBase + rel];
        const float4 end = nv_orientation_advance(o0.rotation, o0.spin, dt);
        const float4 transport = nv_quat_mul(o1.rotation, float4(-end.xyz, end.w));
        return nv_quat_normalize(nv_quat_mul(quatPower(transport, w), nv_orientation_advance(o0.rotation, o0.spin, w * dt)));
    }
    // born in the latest tick: back from its state over the rest of the tick (a positive interval with the reversed spin)
    return nv_orientation_advance(o1.rotation, -o1.spin, (1.0f - w) * dt);
}

// objectToWorld rows in the renderer's axes: R_r = M R_s M (M = diag(axes)) times the scale, then the position.
void meshTransform(float4 q, float scale, float3 position, float3 axes, out float4 rows[3])
{
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    const float3x3 R = float3x3(1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y),
                                2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
                                2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y));
    [unroll] for (uint i = 0u; i < 3u; ++i) rows[i] = float4(R[i] * axes * (axes[i] * scale), position[i]);
}

[numthreads(256, 1, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 gid : SV_GroupID)
{
    const MeshConstants mc = meshConstants();
    const LayerConstants c = mc.layer;
    const uint t = id.x;
    if (t >= c.threads) return;
    s_curveKeys = c.curveKeys;
    const RenderRange rr = renderRange(c, t, gid.x);
    const uint k = t - rr.thread, birth = rr.first + k, row = rr.row;
    StructuredBuffer<StreamEmitter> emitters = ResourceDescriptorHeap[c.emitters];
    StructuredBuffer<StreamProgram> programs = ResourceDescriptorHeap[c.programs];
    const StreamEmitter e = emitters[row];
    const StreamProgram p = programs[e.program];
    if (p.output != FX_OUTPUT_MESH || (e.flags & FX_EMITTER_KILLED) != 0u || !(p.lifetime > 0)) return;
    if ((p.flags & FX_MESH_PROGRAM_ORIENTATION) == 0u || mc.orientationCur == FX_MESH_NONE)
    {
        RWStructuredBuffer<uint> counters = ResourceDescriptorHeap[mc.counters];
        InterlockedOr(counters[FX_MESH_COUNTER_STATUS], FX_MESH_STATUS_NO_ORIENTATION);
        return;
    }
    const bool dying = (rr.prevCountFlags & 0x80000000u) != 0u;

    float3 pos;
    float age;
    bool d;
    bool alive = fxParticleAt(c, rr, k, birth, row, p, pos, age, d);
    MeshDrawn drawn;
    drawn.position = pos;
    drawn.scale = alive ? meshScale(p, e, age) : 0.0f;
    float3 spin = 0;
    drawn.rotation = alive ? meshRotation(mc, rr, k, birth, c.w, spin) : float4(0, 0, 0, 1);
    const uint mesh = meshOfAsset(mc, p.reserved6.xy);
    if (alive && mesh == FX_MESH_NONE)
    {
        meshCount(mc, FX_MESH_COUNTER_UNMAPPED, 1u);
        alive = false;
    }
    if (!(drawn.scale > 0)) alive = false;
    if (!alive) drawn.scale = 0.0f;
    // this frame's record at the latest layout index (the next frame's mode 2 reads it; a death of the latest tick is not
    // in the next tick's layout)
    if (!dying)
    {
        RWStructuredBuffer<MeshDrawn> drawnOut = ResourceDescriptorHeap[mc.drawnOut];
        drawnOut[rr.stateBase + k] = drawn;
    }
    if (!alive) return;

    // the previous frame's transform
    MeshDrawn before = (MeshDrawn)0;
    if (mc.mode == 1u)
    {
        LayerConstants h = c;
        h.w = mc.wHistory;
        float ageBefore;
        if (fxParticleAt(h, rr, k, birth, row, p, before.position, ageBefore, d))
        {
            before.scale = meshScale(p, e, ageBefore);
            float3 unused;
            before.rotation = meshRotation(mc, rr, k, birth, mc.wHistory, unused);
        }
    }
    else if (mc.mode == 2u)
    {
        const uint rel = birth - rr.prevFirst;
        const uint index = dying ? rr.stateBase + k : rel < (rr.prevCountFlags & 0x7FFFFFFFu) ? rr.prevBase + rel : FX_MESH_NONE;
        if (index != FX_MESH_NONE)
        {
            StructuredBuffer<MeshDrawn> drawnIn = ResourceDescriptorHeap[mc.drawnIn];
            before = drawnIn[index];
            before.position += mc.originDelta;
        }
    }
    if (!(before.scale > 0))
    {
        // no drawn history: moved back over the frame time with its velocity and spin
        StructuredBuffer<float4> velocity = ResourceDescriptorHeap[dying ? c.velocityPrev : c.velocityCur];
        before.position = pos - velocity[rr.stateBase + k].xyz * c.streamAxes * mc.frameDt;
        before.scale = drawn.scale;
        before.rotation = nv_orientation_advance(drawn.rotation, -spin, mc.frameDt);
    }

    // one instance record (gpu::Instance, 160 B) at the range's next slot
    const uint lanes = WaveActiveCountBits(true), lane = WavePrefixCountBits(true);
    uint base = 0u;
    RWByteAddressBuffer countBuffer = ResourceDescriptorHeap[mc.count];
    if (WaveIsFirstLane()) countBuffer.InterlockedAdd(mc.countOffset, lanes, base);
    const uint slot = WaveReadLaneFirst(base) + lane;
    if (slot >= mc.capacity)
    {
        meshCount(mc, FX_MESH_COUNTER_OVERFLOW, 1u);
        return;
    }
    meshCount(mc, FX_MESH_COUNTER_INSTANCES, 1u);
    float4 now[3], then[3];
    meshTransform(drawn.rotation, drawn.scale, drawn.position, c.streamAxes, now);
    meshTransform(before.rotation, before.scale, before.position, c.streamAxes, then);
    RWByteAddressBuffer instances = ResourceDescriptorHeap[mc.instances];
    const uint at = (mc.first + slot) * 160u;
    [unroll] for (uint i = 0u; i < 3u; ++i)
    {
        instances.Store4(at + 16u * i, asuint(now[i]));
        instances.Store4(at + 48u + 16u * i, asuint(then[i]));
    }
    // mesh, flags (InstanceCastShadow | InstanceDynamic), materialRemap, bonePalette; transformRevision, deformRevision,
    // wind stiffness, phase; wind anchor, breakCentre; morph, morphRadius, patch, pad
    instances.Store4(at + 96u, uint4(mesh, 3u, FX_MESH_NONE, FX_MESH_NONE));
    instances.Store4(at + 112u, uint4(mc.revision, 0u, 0u, 0u));
    instances.Store4(at + 128u, uint4(0u, 0u, 0u, 0u));
    instances.Store4(at + 144u, uint4(FX_MESH_NONE, 0u, FX_MESH_NONE, 0u));
}
