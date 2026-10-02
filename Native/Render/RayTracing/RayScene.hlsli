// Ray-traced scene (R track, ARCHITECTURE 2.12): records that map a TLAS hit back to scene data, the two-TLAS trace
// (static world + dynamic objects), and surface reconstruction at a hit. Mirror of unx::render::rt (RtLayout.h).
// Included by R's ray libraries (GI, reflections); other tracks do not trace rays.
//
// Hit identity: InstanceID() indexes RtInstance, GeometryIndex() is the submesh within the BLAS, PrimitiveIndex() the
// triangle within that geometry. Static BLASes use the scene's own vertices and indices (object space, instance
// transform in the TLAS). Deformed BLASes (skinned characters: proxy or original, wind-deformed exact-set foliage) hold
// world-space positions written by RayTracing/Deform with deformVertex() (INTERFACES 6.4) and an identity transform.
#ifndef UNX_RT_RAYSCENE_HLSLI
#define UNX_RT_RAYSCENE_HLSLI
#include "Bindless.hlsli"
#include "Scene.hlsli"
#include "Passes/Material/MaterialTextures.hlsli"

// Instance masks. Rays of a consumer select what they may see; the exact set swaps proxies for originals (2.6).
#define RT_MASK_GI 0x1u          // GI cache rays and visibility rays of the cache
#define RT_MASK_REFLECTION 0x2u  // reflection M/G rays
#define RT_MASK_EMITTER 0x4u     // the analytic area lights (RayScene's emitter instance): GI and reflection rays only,
                                 // never visibility rays (the lights have no body; LTC ignores their occlusion)
#define RT_MASK_SHADOW 0x10u     // instances that cast shadows (INSTANCE_CAST_SHADOW): the mask of lights' shadow rays
                                 // (shading.mega_lights); every other ray's mask leaves it out
#define RT_MASK_ALL 0xFFu
// The mask of a shadow ray shot from a ray hit or a card texel toward the sun or a light: the casters, as the view's
// shadow maps draw them and as the sampled lights' rays see them (RT_MASK_SHADOW). With RT_MASK_GI here (the rule until
// 2026-10-03) a mesh that casts no shadow in the view - a lamp's shade, a pane the game marked so - still shadowed the
// hit, and the bounce light of what it "hid" was missing.
#ifndef RT_MASK_HIT_SHADOW
#define RT_MASK_HIT_SHADOW RT_MASK_SHADOW
#endif
// InstanceID of the emitter instance (RtHit.instance; RtHit.primitive = the light's index in the scene's light buffer).
#define RT_INSTANCE_EMITTER 0xFFFFFEu

// RtInstance.flags
#define RT_INSTANCE_DEFORMED 0x1u  // vertices come from the deformed pool (world space), not the scene vertex pool

// RtGeometry.flags
#define RT_GEOMETRY_PROXY_INDICES 0x1u  // indices come from R's index pool (proxy cuts) instead of the scene indices

struct RtInstance  // 16 B
{
    uint sceneInstance;   // index into the scene instance buffer
    uint geometryBase;    // first RtGeometry of this instance's BLAS (one per geometry = submesh)
    uint vertexBase;      // deformed instances: first vertex in the deformed pool; else UNX_NONE
    uint flags;           // RT_INSTANCE_*
};

struct RtGeometry  // 16 B
{
    uint indexOffset;     // first index of this geometry (uint32 units) in the scene index pool or R's index pool
    uint submesh;         // submesh index within the mesh (material slot)
    uint flags;           // RT_GEOMETRY_*
    uint vertexMap;       // proxy cuts: first entry in R's vertex map (compact -> mesh vertex); UNX_NONE = identity
};

struct RtDeformedVertex  // 24 B, world space
{
    float3 position;
    uint normalOct;
    uint2 motion;  // world - prevWorld (deformVertex, previous tick), fp16 x 3: hit motion without re-deforming
};

// Bindless indices of the ray scene, passed by consumers as two uint4 root constants (R-internal convention).
struct RtSceneSrvs
{
    uint tlasStatic, tlasDynamic, instances, geometries;
    uint indices, vertexMap, deformed, pad;
};

RtSceneSrvs rtSceneSrvs(uint4 a, uint4 b)
{
    RtSceneSrvs s;
    s.tlasStatic = a.x; s.tlasDynamic = a.y; s.instances = a.z; s.geometries = a.w;
    s.indices = b.x; s.vertexMap = b.y; s.deformed = b.z; s.pad = b.w;
    return s;
}

RtInstance rtLoadInstance(RtSceneSrvs s, uint id) { StructuredBuffer<RtInstance> b = ResourceDescriptorHeap[s.instances]; return b[id]; }
RtGeometry rtLoadGeometry(RtSceneSrvs s, uint id) { StructuredBuffer<RtGeometry> b = ResourceDescriptorHeap[s.geometries]; return b[id]; }

// Hit identity carried out of the closest-hit shader. t < 0 = miss.
struct RtHit
{
    float t;
    uint instance;   // RtInstance index
    uint geometry;   // GeometryIndex()
    uint primitive;  // PrimitiveIndex()
    float2 barycentrics;
    uint frontFace;  // 1 = HIT_KIND_TRIANGLE_FRONT_FACE
    uint pad;
};

RtHit rtMiss()
{
    RtHit h;
    h.t = -1;
    h.instance = h.geometry = h.primitive = 0;
    h.barycentrics = 0;
    h.frontFace = 0;
    h.pad = 0;
    return h;
}

// Mesh-relative vertex indices of the hit triangle and the mesh vertex indices for attribute fetch.
struct RtTriangle
{
    uint3 poolIndex;   // indices into the vertex source of the BLAS (scene mesh vertices or the instance's deformed range)
    uint3 meshVertex;  // mesh-relative vertex indices (uv, rest-pose attributes)
};

RtTriangle rtTriangle(RtSceneSrvs s, RtGeometry g, uint primitive)
{
    RtTriangle r;
    const uint base = g.indexOffset + 3 * primitive;
    if ((g.flags & RT_GEOMETRY_PROXY_INDICES) != 0)
    {
        StructuredBuffer<uint> idx = ResourceDescriptorHeap[s.indices];
        r.poolIndex = uint3(idx[base], idx[base + 1], idx[base + 2]);
        if (g.vertexMap != UNX_NONE)
        {
            StructuredBuffer<uint> map = ResourceDescriptorHeap[s.vertexMap];
            r.meshVertex = uint3(map[g.vertexMap + r.poolIndex.x], map[g.vertexMap + r.poolIndex.y], map[g.vertexMap + r.poolIndex.z]);
        }
        else
        {
            r.meshVertex = r.poolIndex;
        }
    }
    else
    {
        StructuredBuffer<uint> idx = ResourceDescriptorHeap[g_indices];
        r.poolIndex = uint3(idx[base], idx[base + 1], idx[base + 2]);
        r.meshVertex = r.poolIndex;
    }
    return r;
}

// Material of a hit (instance overrides first).
// The instance record of a hit and its geometry's record.
RtInstance rtResolve(RtSceneSrvs s, RtHit h, out RtGeometry g)
{
    const RtInstance ri = rtLoadInstance(s, h.instance);
    g = rtLoadGeometry(s, ri.geometryBase + h.geometry);
    return ri;
}

uint rtMaterial(RtSceneSrvs s, RtHit h, out GpuInstance inst, out GpuMesh mesh, out RtGeometry g)
{
    const RtInstance ri = rtResolve(s, h, g);
    inst = loadInstance(ri.sceneInstance);
    mesh = loadMesh(inst.mesh);
    const GpuSubmesh sub = loadSubmesh(mesh.submeshOffset + g.submesh);
    return instanceMaterial(inst, sub, g.submesh);
}

float3 rtBary(float2 b) { return float3(1 - b.x - b.y, b.x, b.y); }

// uv of a hit triangle (always from the scene vertex pool: deformation does not move texture coordinates).
float2 rtUv(GpuMesh mesh, uint3 meshVertex, float2 barycentrics)
{
    const float3 w = rtBary(barycentrics);
    return loadVertex(mesh, meshVertex.x).uv * w.x + loadVertex(mesh, meshVertex.y).uv * w.y + loadVertex(mesh, meshVertex.z).uv * w.z;
}

// Surface at a hit, world space.
#ifndef RT_SURFACE_EXTRAS
#define RT_SURFACE_EXTRAS 1
#endif
struct RtSurface
{
    float3 position;
    float3 normal;         // shading normal (interpolated), facing the ray origin side for two-sided materials
    float3 geometricNormal;
    float2 uv;
    float uvPerWorldArea;  // the triangle's uv area / world area: texture level of detail at hits (ray cones)
    uint material;
    uint sceneInstance;
    bool frontFace;
#if RT_SURFACE_EXTRAS
    // The hit triangle (HitShading.hlsli rtHitEye): its rest-pose edges (object space), its edges as hit (world) and its
    // uv edges; and the vertex colour at the hit (white for a mesh without colours). RT_SURFACE_EXTRAS 0 (a kernel at the
    // DXIL limit, which reads none of them): the surface has no such fields.
    float3 restE1, restE2, worldE1, worldE2;
    float2 uvE1, uvE2;
    float4 color;
#endif
};
// (The material inputs at a hit - the uv transform in its textures and its alpha test, the emissive mask, the vertex
// tint - compile out with UNX_MATERIAL_INPUTS 0, Scene.hlsli.)

// With the records it read (a caller needing more of the hit, e.g. its motion, does not load them again).
RtSurface rtSurfaceParts(RtSceneSrvs s, RtHit h, float3 origin, float3 direction, out GpuInstance inst, out GpuMesh mesh, out RtInstance ri,
                         out RtTriangle tri)
{
    RtGeometry g;
    RtSurface o;
    ri = rtResolve(s, h, g);
    inst = loadInstance(ri.sceneInstance);
    mesh = loadMesh(inst.mesh);
    o.material = instanceMaterial(inst, loadSubmesh(mesh.submeshOffset + g.submesh), g.submesh);  // rtMaterial
    o.sceneInstance = ri.sceneInstance;
    tri = rtTriangle(s, g, h.primitive);
    const float3 w = rtBary(h.barycentrics);
    float3 p0, p1, p2, n;
    if ((ri.flags & RT_INSTANCE_DEFORMED) != 0)
    {
        StructuredBuffer<RtDeformedVertex> d = ResourceDescriptorHeap[s.deformed];
        const RtDeformedVertex a = d[ri.vertexBase + tri.poolIndex.x], b = d[ri.vertexBase + tri.poolIndex.y], c = d[ri.vertexBase + tri.poolIndex.z];
        p0 = a.position; p1 = b.position; p2 = c.position;
        n = normalize(octDecode(a.normalOct) * w.x + octDecode(b.normalOct) * w.y + octDecode(c.normalOct) * w.z);
#if RT_SURFACE_EXTRAS
        const float3 rest0 = loadVertex(mesh, tri.meshVertex.x).position;
        o.restE1 = loadVertex(mesh, tri.meshVertex.y).position - rest0;
        o.restE2 = loadVertex(mesh, tri.meshVertex.z).position - rest0;
#endif
    }
    else
    {
        const VertexData a = loadVertex(mesh, tri.meshVertex.x), b = loadVertex(mesh, tri.meshVertex.y), c = loadVertex(mesh, tri.meshVertex.z);
        p0 = transformPoint(inst.objectToWorld, a.position);
        p1 = transformPoint(inst.objectToWorld, b.position);
        p2 = transformPoint(inst.objectToWorld, c.position);
        n = normalize(transformVector(inst.objectToWorld, a.normal * w.x + b.normal * w.y + c.normal * w.z));
#if RT_SURFACE_EXTRAS
        o.restE1 = b.position - a.position;
        o.restE2 = c.position - a.position;
#endif
    }
#if RT_SURFACE_EXTRAS
    o.color = 1;
    o.worldE1 = p1 - p0;
    o.worldE2 = p2 - p0;
    const uint attributeBase = meshAttributeBase(inst.mesh);
    if (attributeBase != 0)
    {
        float2 unusedUv;
        float4 c0, c1, c2;
        loadVertexAttributes(attributeBase, tri.meshVertex.x, unusedUv, c0);
        loadVertexAttributes(attributeBase, tri.meshVertex.y, unusedUv, c1);
        loadVertexAttributes(attributeBase, tri.meshVertex.z, unusedUv, c2);
        o.color = c0 * w.x + c1 * w.y + c2 * w.z;
    }
#endif
    o.position = origin + direction * h.t;
    o.geometricNormal = normalize(cross(p1 - p0, p2 - p0));
    const float2 uv0 = loadVertex(mesh, tri.meshVertex.x).uv, uv1 = loadVertex(mesh, tri.meshVertex.y).uv, uv2 = loadVertex(mesh, tri.meshVertex.z).uv;
    o.uv = uv0 * w.x + uv1 * w.y + uv2 * w.z;
    const float2 du = uv1 - uv0, dv = uv2 - uv0;
#if RT_SURFACE_EXTRAS
    o.uvE1 = du;
    o.uvE2 = dv;
#endif
    o.uvPerWorldArea = abs(du.x * dv.y - du.y * dv.x) / max(length(cross(p1 - p0, p2 - p0)), 1e-20);
    o.frontFace = h.frontFace != 0;
    if (!o.frontFace)
    {
        o.normal = -n;
        o.geometricNormal = -o.geometricNormal;
    }
    else
    {
        o.normal = n;
    }
    return o;
}
RtSurface rtSurface(RtSceneSrvs s, RtHit h, float3 origin, float3 direction)
{
    GpuInstance inst;
    GpuMesh mesh;
    RtInstance ri;
    RtTriangle tri;
    return rtSurfaceParts(s, h, origin, direction, inst, mesh, ri, tri);
}

// Alpha test of a hit candidate (any-hit). INTERFACES 8.1: opaque when baseColor texture alpha >= alphaCutoff.
bool rtAlphaOpaque(RtSceneSrvs s, uint instance, uint geometry, uint primitive, float2 barycentrics)
{
    RtHit h = rtMiss();
    h.instance = instance;
    h.geometry = geometry;
    h.primitive = primitive;
    h.barycentrics = barycentrics;
    GpuInstance inst;
    GpuMesh mesh;
    RtGeometry g;
    const GpuMaterial m = loadMaterial(rtMaterial(s, h, inst, mesh, g));
    if (m.alphaCutoff <= 0 || m.baseColorTexture == UNX_NONE) return true;
    const RtTriangle tri = rtTriangle(s, g, primitive);
    const float2 uv = rtUv(mesh, tri.meshVertex, barycentrics);
    // The raster's alpha test (V AlphaTest.hlsli) at the texture's level 0 (the scene texture unchanged, M keeps coverage
    // equal across levels): M's published texture and addressing (MaterialTextures.hlsli, INTERFACES v1.11).
    return materialBaseColorLevel(m, uv, 0).a >= m.alphaCutoff;
}

// Whether an area light's specular belongs to the ray paths this frame (COVERAGE 12.4 structure 2): M's stable bits
// (word 11 of RayScene's local-light data; 0xFFFFFFFF: no bits, every light), else M shades it with LTC and a ray hit on
// its emitter must add nothing (the geometry behind the light stays occluded).
bool rtEmitterCounts(uint lightData, uint light)
{
    if (lightData == 0xFFFFFFFFu) return true;
    ByteAddressBuffer b = ResourceDescriptorHeap[lightData];
    const uint maskSrv = b.Load(44);
    if (maskSrv == 0xFFFFFFFFu) return true;
    ByteAddressBuffer mask = ResourceDescriptorHeap[maskSrv];
    return ((mask.Load(4u * (light >> 5)) >> (light & 31u)) & 1u) != 0;
}

// Radiance of an area light seen from 'receiver' (the ray origin): L = intensity x colour x the light's range window
// (INTERFACES 8.3; the same w(distance to the light's centre) as M's shAreaWindow, so LTC and rays see one light).
float3 rtEmitterRadiance(uint light, float3 receiver)
{
    StructuredBuffer<GpuLight> lights = ResourceDescriptorHeap[g_lights];
    const GpuLight l = lights[light];
    return l.intensity * l.color * lightWindow(l, length(l.position - receiver));
}
// The same where the ray met the emitter at hitPoint: a rect that shows an image (scene::Light::sourceTexture) has the
// image's colour there (its finest level, as the reference's path tracer shades a rect light's hit).
float3 rtEmitterRadiance(uint light, float3 receiver, float3 hitPoint)
{
    StructuredBuffer<GpuLight> lights = ResourceDescriptorHeap[g_lights];
    const GpuLight l = lights[light];
    float3 L = l.intensity * l.color * lightWindow(l, length(l.position - receiver));
    if (l.sourceTexture != 0 && lightType(l) == LIGHT_RECT)
    {
        Texture2D<float4> image = ResourceDescriptorHeap[l.sourceTexture - 1];
        const float3 q = hitPoint - l.position;
        const float2 uv = float2(dot(q, l.right) / max(l.size.x, 1e-6), -dot(q, cross(l.forward, l.right)) / max(l.size.y, 1e-6)) + 0.5;
        L *= image.SampleLevel(g_linearClamp, uv, 0).rgb;
    }
    return L;
}

#endif
