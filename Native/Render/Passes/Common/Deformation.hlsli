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

// Linear blend skinning in object space (jointToModel * inverseBind are pre-multiplied into the palette).
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
        sn += w[k] * mul((float3x3)m, n);
        st += w[k] * mul((float3x3)m, t);
    }
    p = sp;
    n = normalize(sn);
    t = normalize(st);
}

// Wind displacement (object space) for a vertex at object position p, at time 'time'. v1: height-weighted sway along
// the scene wind direction; replaced by the P3 wind model behind the same signature.
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

// Upper bound of |windOffset| (object space) for any point of an object-space sphere and any time. Culling inflates
// bounds by it so geometry moved by the wind is never culled. Changes together with windOffset.
float windOffsetBound(GpuInstance inst, float3 centre, float radius)
{
    if ((inst.flags & INSTANCE_WIND) == 0 || inst.windStiffness <= 0 || g_windSpeed <= 0) return 0;
    const float h = max(centre.y + radius - inst.windAnchor, 0.0);
    return g_windSpeed * g_windSpeed * 0.002 / inst.windStiffness * h * h;
}

DeformedVertex deformVertex(GpuInstance inst, GpuMesh mesh, uint meshVertex)
{
    const VertexData v = loadVertex(mesh, meshVertex);
    float3 p = v.position, n = v.normal, t = v.tangent;
    float3 pp = p, pn = n, pt = t;
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
