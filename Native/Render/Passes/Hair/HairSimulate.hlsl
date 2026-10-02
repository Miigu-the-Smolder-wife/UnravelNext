// unx-kernel: cs_6_6 main
// unx-variants: STEP=0,1,2
// Strand hair (E, B10; include/unx/hair/Hair.h).
//   STEP 0: one tick of one body's guides, one thread per guide (sequential over its nodes: a structural bound of
//           substeps x (nodes x (1 + localIterations) + 2 x nodes x capsules) per thread)
//   STEP 1: per guide, its nodes at the frame time (tick states interpolated) and twist-free frames transported from the
//           root frame along the strand
//   STEP 2: per follow strand the LOD keeps, its nodes (guide node + the rest offset in the guide node's frame, spread
//           to the tip) and its segments (camera-relative end points and radii). Thread k writes the k-th kept strand's
//           segments at place k: every strand in its own order when all are kept, else the k-th of the body's LOD order
//           (the follows by rising hash; a keep threshold leaves the first 'kept' of them)
// Constants (raw SRV, HairConstants below).
#include "Bindless.hlsli"

struct HairConstants
{
    uint guides, nodes, follows, capsules;
    uint rest, state, tickPrev, tickCur;          // rest float4 (xyz joint space, w segment length to the next node);
                                                  // state float4 x 2 per node (position, previous position); tick
                                                  // snapshots float4 per node (world)
    uint guideJoint, inputs, jointsPrev, jointsCur; // inputs: StructuredBuffer<float4>; the joint rows (3 per joint) of
                                                  // the tick's start and end at these element offsets
    uint capsuleOffset, frameNodes, frameRotations, followBuffer;  // capsules: 2 float4 each (a, radius), (b, 0)
    uint segments, localIterations, substeps, lodKeep;  // lodKeep: follows with hash < lodKeep are drawn (2^32 fraction;
                                                  // 0xFFFFFFFF = all)
    uint segmentBase, kept, pad1, pad2;           // kept: the strands drawn this frame (the first of the LOD order)
    float3 gravity; float damping;
    float3 wind; float dt;
    float globalStiffness, globalRange, localStiffness, dftlDamping;
    float collisionMargin, windDrag, frameFraction, radiusScale;
    float3 cameraPosition; float rootRadius;
    float tipRadius, pad3, pad4, pad5;
};

// P[0] = { constants SRV (raw), byte offset of this dispatch's HairConstants }
HairConstants hairConstants()
{
    ByteAddressBuffer b = ResourceDescriptorHeap[P[0].x];
    return b.Load<HairConstants>(P[0].y);
}

float4 quatFromMatrix(float3x3 m)  // rows of a rotation matrix -> quaternion (x, y, z, w)
{
    const float t = m[0][0] + m[1][1] + m[2][2];
    float4 q;
    if (t > 0)
    {
        const float s = sqrt(t + 1) * 2;
        q = float4((m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s, 0.25f * s);
    }
    else if (m[0][0] > m[1][1] && m[0][0] > m[2][2])
    {
        const float s = sqrt(1 + m[0][0] - m[1][1] - m[2][2]) * 2;
        q = float4(0.25f * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s, (m[2][1] - m[1][2]) / s);
    }
    else if (m[1][1] > m[2][2])
    {
        const float s = sqrt(1 + m[1][1] - m[0][0] - m[2][2]) * 2;
        q = float4((m[0][1] + m[1][0]) / s, 0.25f * s, (m[1][2] + m[2][1]) / s, (m[0][2] - m[2][0]) / s);
    }
    else
    {
        const float s = sqrt(1 + m[2][2] - m[0][0] - m[1][1]) * 2;
        q = float4((m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25f * s, (m[1][0] - m[0][1]) / s);
    }
    return normalize(q);
}
float3 quatRotate(float4 q, float3 v)
{
    const float3 t = 2 * cross(q.xyz, v);
    return v + q.w * t + cross(q.xyz, t);
}
float4 quatMul(float4 a, float4 b) { return float4(a.w * b.xyz + b.w * a.xyz + cross(a.xyz, b.xyz), a.w * b.w - dot(a.xyz, b.xyz)); }
// The shortest rotation taking unit a to unit b.
float4 quatFromTo(float3 a, float3 b)
{
    const float c = dot(a, b);
    if (c < -0.999999f)
    {
        float3 axis = cross(float3(1, 0, 0), a);
        if (dot(axis, axis) < 1e-12f) axis = cross(float3(0, 1, 0), a);
        return float4(normalize(axis), 0);
    }
    return normalize(float4(cross(a, b), 1 + c));
}
float3x4 loadJointRows(uint buffer, uint offset, uint joint)
{
    StructuredBuffer<float4> rows = ResourceDescriptorHeap[buffer];
    return float3x4(rows[offset + 3 * joint], rows[offset + 3 * joint + 1], rows[offset + 3 * joint + 2]);
}
float3 hairTransform(float3x4 m, float3 p) { return float3(dot(m[0].xyz, p) + m[0].w, dot(m[1].xyz, p) + m[1].w, dot(m[2].xyz, p) + m[2].w); }
// Pushes p out of every capsule (radius + margin).
float3 hairCollide(HairConstants c, float3 p)
{
    StructuredBuffer<float4> caps = ResourceDescriptorHeap[c.inputs];
    for (uint k = 0; k < c.capsules; ++k)
    {
        const float4 a = caps[c.capsuleOffset + 2 * k], b = caps[c.capsuleOffset + 2 * k + 1];
        const float3 ab = b.xyz - a.xyz;
        const float t = saturate(dot(p - a.xyz, ab) / max(dot(ab, ab), 1e-12f));
        const float3 q = a.xyz + ab * t, d = p - q;
        const float r = a.w + c.collisionMargin, len = length(d);
        if (len < r) p = q + (len > 1e-9f ? d / len : float3(0, 1, 0)) * r;
    }
    return p;
}

[numthreads(64, 1, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    const HairConstants c = hairConstants();
#if STEP == 0
    const uint g = id.x;
    if (g >= c.guides) return;
    StructuredBuffer<float4> rest = ResourceDescriptorHeap[c.rest];
    RWStructuredBuffer<float4> state = ResourceDescriptorHeap[c.state];
    RWStructuredBuffer<float4> tickPrev = ResourceDescriptorHeap[c.tickPrev];
    RWStructuredBuffer<float4> tickCur = ResourceDescriptorHeap[c.tickCur];
    StructuredBuffer<uint> guideJoint = ResourceDescriptorHeap[c.guideJoint];
    const uint base = g * c.nodes, joint = guideJoint[g];
    const float3x4 j0 = loadJointRows(c.inputs, c.jointsPrev, joint), j1 = loadJointRows(c.inputs, c.jointsCur, joint);
    // the previous tick's end state is the render interpolation's start
    for (uint i = 0; i < c.nodes; ++i) tickPrev[base + i] = tickCur[base + i];
    const float h = c.dt / c.substeps;
    for (uint sub = 0; sub < c.substeps; ++sub)
    {
        const float t = float(sub + 1) / c.substeps;
        const float3x4 jm = j0 * (1 - t) + j1 * t;  // O(theta^2) off a rotation over one tick's substep
        const float4 q = quatFromMatrix(float3x3(normalize(jm[0].xyz), normalize(jm[1].xyz), normalize(jm[2].xyz)));
        // 1. Verlet (root pinned to its joint)
        for (uint i = 0; i < c.nodes; ++i)
        {
            const float3 target = hairTransform(jm, rest[base + i].xyz);
            float4 x = state[2 * (base + i)], xp = state[2 * (base + i) + 1];
            if (i == 0)
            {
                xp.xyz = x.xyz;
                x.xyz = target;
            }
            else
            {
                const float3 v = (x.xyz - xp.xyz) * (1 - c.damping);
                const float3 air = (c.wind * h - (x.xyz - xp.xyz)) * c.windDrag * h;  // drag towards the wind velocity
                xp.xyz = x.xyz;
                x.xyz += v + c.gravity * h * h + air;
                // 2. global shape
                if (float(i) <= c.globalRange * (c.nodes - 1)) x.xyz += c.globalStiffness * (target - x.xyz);
            }
            state[2 * (base + i)] = x;
            state[2 * (base + i) + 1] = xp;
        }
        // 3. local shape: the rest vector of segment i in the frame the chain carries (rotated by the actual bend)
        for (uint it = 0; it < c.localIterations; ++it)
        {
            float4 frame = q;
            for (uint i = 0; i + 1 < c.nodes; ++i)
            {
                const float3 restVec = rest[base + i + 1].xyz - rest[base + i].xyz;
                float3 a = state[2 * (base + i)].xyz, b = state[2 * (base + i + 1)].xyz;
                const float3 expected = quatRotate(frame, restVec);
                const float3 d = c.localStiffness * 0.5f * (a + expected - b);
                if (i > 0) a -= d;
                b += (i > 0) ? d : 2 * d;
                state[2 * (base + i)].xyz = a;
                state[2 * (base + i + 1)].xyz = b;
                const float3 actual = b - a;
                if (dot(actual, actual) > 1e-20f && dot(expected, expected) > 1e-20f) frame = normalize(quatMul(quatFromTo(normalize(expected), normalize(actual)), frame));
            }
        }
        // 4. collision, 5. DFTL (exact lengths root to tip, velocity correction -s d_{i+1}), 6. collision
        for (uint i = 1; i < c.nodes; ++i) state[2 * (base + i)].xyz = hairCollide(c, state[2 * (base + i)].xyz);
        for (uint i = 1; i < c.nodes; ++i)
        {
            const float3 parent = state[2 * (base + i - 1)].xyz;
            float4 x = state[2 * (base + i)];
            const float len = rest[base + i - 1].w;
            const float3 dir = x.xyz - parent;
            const float l = length(dir);
            const float3 fixedPos = parent + (l > 1e-12f ? dir / l : normalize(quatRotate(q, rest[base + i].xyz - rest[base + i - 1].xyz))) * len;
            const float3 correction = fixedPos - x.xyz;
            x.xyz = fixedPos;
            state[2 * (base + i)] = x;
            // velocity correction of node i - 1 by the next node's correction: previous position moved by s d_i
            if (i > 1) state[2 * (base + i - 1) + 1].xyz += c.dftlDamping * correction;
        }
        for (uint i = 1; i < c.nodes; ++i) state[2 * (base + i)].xyz = hairCollide(c, state[2 * (base + i)].xyz);
    }
    for (uint i = 0; i < c.nodes; ++i) tickCur[base + i] = float4(state[2 * (base + i)].xyz, 0);
#elif STEP == 1
    const uint g = id.x;
    if (g >= c.guides) return;
    StructuredBuffer<float4> tickPrev = ResourceDescriptorHeap[c.tickPrev];
    StructuredBuffer<float4> tickCur = ResourceDescriptorHeap[c.tickCur];
    StructuredBuffer<float4> rest = ResourceDescriptorHeap[c.rest];
    StructuredBuffer<uint> guideJoint = ResourceDescriptorHeap[c.guideJoint];
    RWStructuredBuffer<float4> nodes = ResourceDescriptorHeap[c.frameNodes];
    RWStructuredBuffer<float4> rotations = ResourceDescriptorHeap[c.frameRotations];
    const uint base = g * c.nodes;
    const float w = c.frameFraction;
    // root frame: the joint's rotation at the frame time, then transported (minimal rotation) along the strand
    const float3x4 j0 = loadJointRows(c.inputs, c.jointsPrev, guideJoint[g]), j1 = loadJointRows(c.inputs, c.jointsCur, guideJoint[g]);
    const float3x4 jm = j0 * (1 - w) + j1 * w;
    float4 frame = quatFromMatrix(float3x3(normalize(jm[0].xyz), normalize(jm[1].xyz), normalize(jm[2].xyz)));
    for (uint i = 0; i < c.nodes; ++i)
    {
        const float3 p = lerp(tickPrev[base + i].xyz, tickCur[base + i].xyz, w);
        nodes[base + i] = float4(p - c.cameraPosition, 0);
        if (i + 1 < c.nodes)
        {
            const float3 pn = lerp(tickPrev[base + i + 1].xyz, tickCur[base + i + 1].xyz, w);
            const float3 restVec = rest[base + i + 1].xyz - rest[base + i].xyz;
            const float3 expected = quatRotate(frame, restVec), actual = pn - p;
            if (dot(actual, actual) > 1e-20f && dot(expected, expected) > 1e-20f) frame = normalize(quatMul(quatFromTo(normalize(expected), normalize(actual)), frame));
        }
        rotations[base + i] = frame;
    }
#else
    const uint k = id.x;
    if (k >= c.kept) return;
    // (offset xyz, guide), (tipSpread, hash, the k-th follow of the LOD order, 0)
    StructuredBuffer<float4> follow = ResourceDescriptorHeap[c.followBuffer];
    StructuredBuffer<float4> nodes = ResourceDescriptorHeap[c.frameNodes];
    StructuredBuffer<float4> rotations = ResourceDescriptorHeap[c.frameRotations];
    RWStructuredBuffer<float4> segments = ResourceDescriptorHeap[c.segments];
    const uint f = c.kept == c.follows ? k : asuint(follow[2 * k + 1].z);
    const float4 f0 = follow[2 * f], f1 = follow[2 * f + 1];
    const uint guide = asuint(f0.w), base = guide * c.nodes;
    const uint firstSegment = c.segmentBase + k * (c.nodes - 1);
    float3 previous = 0;
    for (uint i = 0; i < c.nodes; ++i)
    {
        const float s = float(i) / float(c.nodes - 1);
        const float3 p = nodes[base + i].xyz + quatRotate(rotations[base + i], f0.xyz * lerp(1.0f, f1.x, s));
        if (i > 0)
        {
            const float r0 = lerp(c.rootRadius, c.tipRadius, float(i - 1) / float(c.nodes - 1)) * c.radiusScale;
            const float r1 = lerp(c.rootRadius, c.tipRadius, s) * c.radiusScale;
            segments[2 * (firstSegment + i - 1)] = float4(previous, r0);
            segments[2 * (firstSegment + i - 1) + 1] = float4(p, r1);
        }
        previous = p;
    }
#endif
}
