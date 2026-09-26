// Vertex deformation shared by every consumer (INTERFACES_KO.md 6.4): V's mesh shaders (main views and S's shadow
// pages through V's raster service) and R's BLAS refit call the same function, so raster, shadows and rays see the
// same geometry. Owner: core. The wind model may improve (P3) without changing this signature.
#ifndef UNX_DEFORMATION_HLSLI
#define UNX_DEFORMATION_HLSLI
#include "Scene.hlsli"

struct DeformedVertex
{
    float3 world;
    float3 prevWorld;  // previous tick (motion vectors, VSM/BLAS change tests)
    float3 normal;     // world, unit
    float3 tangent;    // world, unit
};

float3x4 loadJoint(uint palette, uint joint)
{
    StructuredBuffer<float4> rows = ResourceDescriptorHeap[palette];
    return float3x4(rows[3 * joint], rows[3 * joint + 1], rows[3 * joint + 2]);
}

// Normal transform of a joint's 3x3 A: the cofactor matrix det(A) A^-T (columns cross products of A's columns), times
// sign(det A) so a mirroring joint keeps the normal on the outer side. Exact for any invertible joint (non-uniform
// scale, shear: squash and stretch, the host's part scales folded into joints); for rotation + uniform scale it is a
// positive multiple of A, so the normalised result is unchanged.
float3 cofactorNormal(float3x3 a, float3 n)
{
    const float3x3 at = transpose(a);  // rows = columns of a
    const float3 c0 = cross(at[1], at[2]), c1 = cross(at[2], at[0]), c2 = cross(at[0], at[1]);
    const float det = dot(at[0], c0);
    return (det < 0 ? -1.0 : 1.0) * (n.x * c0 + n.y * c1 + n.z * c2);
}

// Linear blend skinning in object space (jointToModel * inverseBind are pre-multiplied into the palette). Positions and
// tangents by the joint matrices, normals by their cofactors (cofactorNormal), each blended by the weights.
void skin(GpuMesh mesh, GpuInstance inst, uint meshVertex, uint palette, inout float3 p, inout float3 n, inout float3 t)
{
    StructuredBuffer<GpuSkinVertex> sv = ResourceDescriptorHeap[g_skinVertices];
    const GpuSkinVertex s = sv[mesh.skinOffset + meshVertex];
    const uint j[4] = { s.joints01 & 0xFFFFu, s.joints01 >> 16, s.joints23 & 0xFFFFu, s.joints23 >> 16 };
    const float w[4] = { (s.weights01 & 0xFFFFu) / 65535.0, (s.weights01 >> 16) / 65535.0, (s.weights23 & 0xFFFFu) / 65535.0, (s.weights23 >> 16) / 65535.0 };
    float3 sp = 0, sn = 0, st = 0;
    [unroll] for (uint k = 0; k < 4; ++k)
    {
        const float3x4 m = loadJoint(palette, inst.bonePalette + j[k]);
        sp += w[k] * mul(m, float4(p, 1));
        sn += w[k] * cofactorNormal((float3x3)m, n);
        st += w[k] * mul((float3x3)m, t);
    }
    p = sp;
    n = normalize(sn);
    t = normalize(st);
}

// Wind displacement (object space) for a vertex at object position p, at time 'time'. v1: height-weighted sway along
// the scene wind direction; replaced by the P3 wind model behind the same signature. The scene wind may change between
// frames (the host changes it before recording, v1.23). windOffset is memoryless: a function of the time and of the
// wind at that time only, never of the wind's history; the P3 model keeps this property (otherwise windChangeBound
// must take the history), because page and BLAS reuse compares only the two endpoints.
float3 windOffset(GpuInstance inst, float3 p, float time)
{
    if (inst.windStiffness <= 0 || g_windSpeed <= 0) return 0;
    const float h = max(p.y - inst.windAnchor, 0.0);
    const float amplitude = g_windSpeed * g_windSpeed * 0.002 / inst.windStiffness * h * h;
    // World -> object direction: transpose of the object -> world rotation (rows of objectToWorld weighted by the world
    // components; rotation + uniform scale, so the normalised result is R^-1 * dirWorld).
    const float3 dirWorld = g_windDirection;
    const float3 dirObject = normalize(inst.objectToWorld[0].xyz * dirWorld.x + inst.objectToWorld[1].xyz * dirWorld.y + inst.objectToWorld[2].xyz * dirWorld.z);
    return dirObject * amplitude * (0.6 + 0.4 * sin(time * 1.7 + inst.windPhase));
}

// Speed-independent part of the bound (v1.23): windOffsetBound = windOffsetScale x speed^2 for the frame's speed. A
// consumer that reuses wind-moved results (S's pages, R's BLAS) stores it with the wind it drew them with.
float windOffsetScale(GpuInstance inst, float3 centre, float radius)
{
    if ((inst.flags & INSTANCE_WIND) == 0 || inst.windStiffness <= 0) return 0;
    const float h = max(centre.y + radius - inst.windAnchor, 0.0);
    return 0.002 / inst.windStiffness * h * h;
}

// Upper bound of |windOffset| (object space) for any point of an object-space sphere and any time, at this frame's
// wind. Culling inflates bounds by it so geometry moved by the wind is never culled. Changes together with windOffset.
float windOffsetBound(GpuInstance inst, float3 centre, float radius) { return g_windSpeed > 0 ? windOffsetScale(inst, centre, radius) * g_windSpeed * g_windSpeed : 0; }

// |windOffset(t1) - windOffset(t0)| <= windOffsetBound(...) * windChangeFactor(t0, t1) for every instance and vertex,
// while the scene wind (direction, speed) and the instance transform are unchanged (S request: page-side VSM dirty
// rule). v1: amplitude x (0.6 + 0.4 sin(1.7 t + phase)), and |sin a - sin b| <= min(2, |a - b|).
float windChangeFactor(float t0, float t1) { return 0.4 * min(2.0, 1.7 * abs(t1 - t0)); }

// Across wind changes (v1.23; I request, S review): for an unchanged instance transform and every point of the sphere
// windOffsetScale was taken over,
//   |windOffset(t1; s1, d1) - windOffset(t0; s0, d0)| <= windChangeBound(windOffsetScale(...), t0, s0, d0, t1, s1, d1)
// for times t0, t1, wind speeds s0, s1 (m/s) and unit world directions d0, d1, however the wind changed in between
// (windOffset is memoryless). v1: windOffset = d K s^2 m(t), m = 0.6 + 0.4 sin(1.7 t + phase) in [0.2, 1], so
//   |d1 K s1^2 m1 - d0 K s0^2 m0| <= K [s1^2 |m1 - m0| + |s1^2 - s0^2| m0 + |d1 - d0| s0^2 m0]
//                                 <= K [s1^2 0.4 min(2, 1.7 |t1 - t0|) + |s1^2 - s0^2| + 2 sin(dtheta / 2) s0^2],
// |d1 - d0| = 2 sin(dtheta / 2) (the object-space directions are the same rotation of d0, d1). Unchanged wind gives
// windOffsetBound x windChangeFactor.
float windChangeBound(float scale, float t0, float s0, float3 d0, float t1, float s1, float3 d1)
{
    return scale * (s1 * s1 * 0.4 * min(2.0, 1.7 * abs(t1 - t0)) + abs(s1 * s1 - s0 * s0) + length(d1 - d0) * s0 * s0);
}

// C4 (render C) morphs before skinning: scene::evaluateMorph is the definition (same additions in the same order, so the
// GPU position equals the CPU reference bit for bit for blend shapes). Mesh block in g_morphData (words):
//   (StructuredBuffer<uint>) h0 = { kind (1 blend shapes, 2 vertex animation), vertex count, shape / frame count, flags (1 loop, 2 normals) }
//   h1 = { frames per second (float bits), rows | positions offset, records | normals offset, 0 }
//   blend: rows[vertexCount + 1] = first record of each vertex; record = 8 words { shape, dp.xyz, dn.xyz, 0 }, per vertex
//          in ascending shape order. vertex animation: float3 positions (and normals) frame-major.
// Record rows (g_morphRecords): { mesh block (bits), weights row (bits), time, previous time }; weights rows: current
// weights (4 per row), then the previous frame's.
#define MORPH_MAX_SHAPES_PER_VERTEX 256u

void morphVertex(GpuInstance inst, uint meshVertex, bool previous, inout float3 pOut, inout float3 nOut)
{
    // precise: one rounding per multiply and per add, as the CPU reference (no fused multiply-add).
    precise float3 p = pOut, n = nOut;
    StructuredBuffer<float4> records = ResourceDescriptorHeap[g_morphRecords];
    StructuredBuffer<uint> data = ResourceDescriptorHeap[g_morphData];
    const float4 r = records[inst.morph];
    const uint block = asuint(r.x), weightsRow = asuint(r.y);
    const uint4 h0 = uint4(data[block], data[block + 1], data[block + 2], data[block + 3]);
    const uint4 h1 = uint4(data[block + 4], data[block + 5], data[block + 6], data[block + 7]);
    if (h0.x == 1)
    {
        const uint shapes = h0.z;
        const uint row = weightsRow + (previous ? (shapes + 3) / 4 : 0);
        const uint first = data[h1.y + meshVertex], last = min(data[h1.y + meshVertex + 1], first + MORPH_MAX_SHAPES_PER_VERTEX);
        [loop] for (uint k = first; k < last; ++k)
        {
            const uint at = h1.z + 8 * k;
            const uint shape = data[at];
            const float w = records[row + shape / 4][shape % 4];
            if (w == 0) continue;
            p = p + asfloat(uint3(data[at + 1], data[at + 2], data[at + 3])) * w;
            n = n + asfloat(uint3(data[at + 4], data[at + 5], data[at + 6])) * w;
        }
    }
    else if (h0.x == 2)
    {
        const uint frames = h0.z, vertices = h0.y;
        const float fps = asfloat(h1.x), time = previous ? r.w : r.z;
        const float f = time * fps;
        float base = floor(f);
        const float a = f - base;
        uint f0, f1;
        if ((h0.w & 1u) != 0)
        {
            base = base - frames * floor(base / frames);
            f0 = min((uint)base, frames - 1);
            f1 = (f0 + 1) % frames;
        }
        else
        {
            f0 = (uint)clamp(base, 0.0, (float)frames - 1);
            f1 = (base < 0 || base >= (float)frames - 1) ? f0 : f0 + 1;
        }
        const uint a0 = h1.y + 3 * (f0 * vertices + meshVertex), a1 = h1.y + 3 * (f1 * vertices + meshVertex);
        const float3 p0 = asfloat(uint3(data[a0], data[a0 + 1], data[a0 + 2])), p1 = asfloat(uint3(data[a1], data[a1 + 1], data[a1 + 2]));
        p = p0 + (p1 - p0) * a;
        if ((h0.w & 2u) != 0)
        {
            const uint b0 = h1.z + 3 * (f0 * vertices + meshVertex), b1 = h1.z + 3 * (f1 * vertices + meshVertex);
            const float3 n0 = asfloat(uint3(data[b0], data[b0 + 1], data[b0 + 2])), n1 = asfloat(uint3(data[b1], data[b1 + 1], data[b1 + 2]));
            n = n0 + (n1 - n0) * a;
        }
    }
    pOut = p;
    nOut = normalize(n);
}

DeformedVertex deformVertex(GpuInstance inst, GpuMesh mesh, uint meshVertex)
{
    const VertexData v = loadVertex(mesh, meshVertex);
    float3 p = v.position, n = v.normal, t = v.tangent;
    float3 pp = p, pn = n, pt = t;
    if (inst.morph != UNX_NONE)
    {
        morphVertex(inst, meshVertex, false, p, n);
        morphVertex(inst, meshVertex, true, pp, pn);
    }
    if ((inst.flags & INSTANCE_SKINNED) != 0 && inst.bonePalette != UNX_NONE && mesh.skinOffset != UNX_NONE)
    {
        skin(mesh, inst, meshVertex, g_bonePalette, p, n, t);
        skin(mesh, inst, meshVertex, g_prevBonePalette, pp, pn, pt);
    }
    if ((inst.flags & INSTANCE_WIND) != 0)
    {
        p += windOffset(inst, p, g_time);
        pp += windOffset(inst, pp, g_time - g_deltaTime);
    }
    DeformedVertex d;
    d.world = transformPoint(inst.objectToWorld, p);
    d.prevWorld = transformPoint(inst.prevObjectToWorld, pp);
    d.normal = normalize(transformVector(inst.objectToWorld, n));  // uniform-scale transforms (INTERFACES 6.1)
    d.tangent = normalize(transformVector(inst.objectToWorld, t));
    return d;
}

#endif
