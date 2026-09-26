// GPU scene records and accessors (INTERFACES_KO.md 6.3). Mirror of unx::render::gpu (GpuSceneLayout.h).
// Every track reads scene data only through these accessors, so storage formats can change behind them.
#ifndef UNX_SCENE_HLSLI
#define UNX_SCENE_HLSLI
#include "Frame.hlsli"

struct GpuInstance
{
    float4 objectToWorld[3];
    float4 prevObjectToWorld[3];
    uint mesh;
    uint flags;
    uint materialRemap;
    uint bonePalette;
    uint transformRevision;
    uint deformRevision;
    float windStiffness, windPhase, windAnchor;
    float3 breakCentre;  // INSTANCE_MOTION_BREAK: world centre of the bounding sphere in the previous rendered frame
    uint morph;          // C4: first morph record row, UNX_NONE = no blend shapes / vertex animation
    float morphRadius;   // C4: object-space bound of the morph offset (culling inflation)
    uint patch;          // C5: terrain patch slot (g_patchData), UNX_NONE = none
    uint morphPad;
};

struct GpuMesh
{
    float4 boundsSphere;
    float3 boundsMin;
    uint vertexOffset;
    float3 boundsMax;
    uint vertexCount;
    uint indexOffset, triangleCount, submeshOffset, submeshCount;
    uint clusterOffset, clusterCount, lodLevelOffset, skinOffset;
};

struct GpuSubmesh
{
    uint indexOffset, indexCount, material, pad0;
};

struct GpuVertexRecord
{
    float3 position;
    uint normalOct;
    uint tangentOct;
    float2 uv;
    uint tangentSign;
};

struct GpuSkinVertex
{
    uint joints01, joints23, weights01, weights23;
};

struct GpuCluster
{
    float4 boundsSphere;
    float4 normalCone;
    uint vertexOffset, triangleOffset, counts, material;
    float lodError, parentLodError, minFeatureWidth;
    uint brick;
};

struct GpuLodLevel
{
    uint clusterOffset, clusterCount;
    float error;
    uint triangleCount;
};

struct GpuMaterial
{
    float3 baseColor;
    float roughness;
    float3 emissive;
    float metallic;
    float specular, alphaCutoff, transmission, ior;
    uint classFlags, baseColorTexture, normalTexture, roughMetalTexture;
    uint emissiveTexture, occlusionTexture, revision, textureClamp;  // textureClamp: bit per texture, 1 = g_anisoClamp
    float3 hairAbsorption;  // Hair class (v1.66): sigma_a, beta_N, cuticle tilt (beta_M = roughness, eta = ior)
    float hairBetaN;
    float hairTilt;
    float cutScale;          // Cut class (v1.66): triplanar repeats per metre, damage band width (m)
    float cutDamageWidth;
    uint terrainLayers;      // Terrain class (v1.74): first layer | count << 24 in g_terrainLayers
};

struct GpuMaterialLayers  // v1.76 (A9): gpu::MaterialLayers, 64 B
{
    float clearcoat, clearcoatRoughness;
    uint coat;
    float coatEta;
    float4 reserved[3];
};
GpuMaterialLayers loadMaterialLayers(uint i) { StructuredBuffer<GpuMaterialLayers> b = ResourceDescriptorHeap[g_materialLayers]; return b[i]; }

struct GpuTerrainLayer  // v1.74: a Standard material read at terrain uv0 x scale + offset
{
    uint material;
    float2 scale, offset;
    uint2 splatSize;  // the material's splats 0 and 1: width | height << 16 (0: none)
    uint pad;
};
GpuTerrainLayer loadTerrainLayer(uint i) { StructuredBuffer<GpuTerrainLayer> b = ResourceDescriptorHeap[g_terrainLayers]; return b[i]; }

struct GpuLight
{
    float3 position;
    uint typeFlags;
    float3 forward;
    float range;
    float3 right;
    float intensity;
    float3 color;
    float spotScale;
    float2 size;
    float spotOffset;
    uint revision;
};

struct GpuVisibleCluster
{
    uint instance, cluster;
};

// scene::InstanceFlags
#define INSTANCE_CAST_SHADOW (1u << 0)
#define INSTANCE_DYNAMIC (1u << 1)
#define INSTANCE_SKINNED (1u << 2)
#define INSTANCE_WIND (1u << 3)
#define INSTANCE_HIDDEN (1u << 31)  // gpu::kInstanceHidden: skipped by every reader (GpuScene::setInstanceVisible)
// gpu::kInstanceMotionBreak (v1.45): a teleport or restore in this frame; prev* = current (zero motion), breakCentre = where
// its bounding sphere was in the previous rendered frame (caches keyed by the old place invalidate from it).
#define INSTANCE_MOTION_BREAK (1u << 30)
#define INSTANCE_VIEW_MODEL (1u << 29)  // gpu::kInstanceViewModel: a first-person view model (Passes/ViewModel)
// scene::MaterialClass
#define MATERIAL_STANDARD 0u
#define MATERIAL_FOLIAGE 1u
#define MATERIAL_HAIR 2u
#define MATERIAL_WATER 3u
#define MATERIAL_GLASS 4u
#define MATERIAL_SUBSURFACE 5u
#define MATERIAL_CUT 6u  // destruction cut faces (A11): Standard shading, triplanar textures
#define MATERIAL_TERRAIN 7u  // terrain layer blending (C5): Standard shading of up to 8 splat-weighted layers
// gpu::MaterialFlags (bits 8..)
#define MATERIAL_TWO_SIDED (1u << 8)
#define MATERIAL_ALPHA_TESTED (1u << 9)
#define MATERIAL_LAYERED (1u << 10)  // v1.76 A9: layer record index in classFlags bits 16..31 (loadMaterialLayers)
// scene::LightType
#define LIGHT_POINT 0u
#define LIGHT_SPOT 1u
#define LIGHT_RECT 2u
#define LIGHT_DISK 3u
#define LIGHT_SPHERE 4u
#define LIGHT_TUBE 5u

GpuInstance loadInstance(uint i) { StructuredBuffer<GpuInstance> b = ResourceDescriptorHeap[g_instances]; return b[i]; }
GpuMesh loadMesh(uint i) { StructuredBuffer<GpuMesh> b = ResourceDescriptorHeap[g_meshes]; return b[i]; }
GpuSubmesh loadSubmesh(uint i) { StructuredBuffer<GpuSubmesh> b = ResourceDescriptorHeap[g_submeshes]; return b[i]; }
GpuCluster loadCluster(uint i) { StructuredBuffer<GpuCluster> b = ResourceDescriptorHeap[g_clusters]; return b[i]; }
GpuLodLevel loadLodLevel(uint i) { StructuredBuffer<GpuLodLevel> b = ResourceDescriptorHeap[g_lodLevels]; return b[i]; }
GpuMaterial loadMaterial(uint i) { StructuredBuffer<GpuMaterial> b = ResourceDescriptorHeap[g_materials]; return b[i]; }
GpuLight loadLight(uint i) { StructuredBuffer<GpuLight> b = ResourceDescriptorHeap[g_lights]; return b[i]; }

uint materialClass(GpuMaterial m) { return m.classFlags & 0xFFu; }
uint clusterVertexCount(GpuCluster c) { return c.counts & 0xFFu; }
uint clusterTriangleCount(GpuCluster c) { return (c.counts >> 8) & 0xFFu; }
uint clusterSubmesh(GpuCluster c) { return c.counts >> 16; }  // index within the mesh
uint lightType(GpuLight l) { return l.typeFlags & 0xFFu; }
bool lightCastsShadow(GpuLight l) { return (l.typeFlags & 0x100u) != 0; }
uint lightShadowIndex(GpuLight l) { return l.typeFlags >> 16; }

// Instance material for a submesh (instance overrides first).
uint instanceMaterial(GpuInstance inst, GpuSubmesh sub, uint submeshIndexInMesh)
{
    if (inst.materialRemap == UNX_NONE) return sub.material;
    StructuredBuffer<uint> remap = ResourceDescriptorHeap[g_materialRemap];
    return remap[inst.materialRemap + submeshIndexInMesh];
}

// Material of a cluster drawn for an instance (instance overrides first).
uint clusterMaterial(GpuInstance inst, GpuCluster c)
{
    if (inst.materialRemap == UNX_NONE) return c.material;
    StructuredBuffer<uint> remap = ResourceDescriptorHeap[g_materialRemap];
    return remap[inst.materialRemap + clusterSubmesh(c)];
}

float3 transformPoint(float4 rows[3], float3 p) { return float3(dot(rows[0].xyz, p) + rows[0].w, dot(rows[1].xyz, p) + rows[1].w, dot(rows[2].xyz, p) + rows[2].w); }
float3 transformVector(float4 rows[3], float3 v) { return float3(dot(rows[0].xyz, v), dot(rows[1].xyz, v), dot(rows[2].xyz, v)); }

float2 octWrap(float2 v) { return (1.0 - abs(v.yx)) * select(v.xy >= 0.0, 1.0, -1.0); }
float3 octDecode(uint packed)
{
    const float2 e = float2(int2(packed << 16, packed) >> 16) / 32767.0;  // snorm16 x 2
    float3 n = float3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0) n.xy = octWrap(n.xy);
    return normalize(n);
}
uint octEncode(float3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    float2 e = n.z >= 0 ? n.xy : octWrap(n.xy);
    const int2 q = int2(round(clamp(e, -1.0, 1.0) * 32767.0));
    return (uint(q.x) & 0xFFFFu) | (uint(q.y) << 16);
}

struct VertexData
{
    float3 position;  // object space, before deformation
    float3 normal;
    float3 tangent;
    float tangentSign;
    float2 uv;
};

// meshVertex is mesh-relative (0 .. vertexCount-1).
VertexData loadVertex(GpuMesh mesh, uint meshVertex)
{
    StructuredBuffer<GpuVertexRecord> b = ResourceDescriptorHeap[g_vertices];
    const GpuVertexRecord r = b[mesh.vertexOffset + meshVertex];
    VertexData v;
    v.position = r.position;
    v.normal = octDecode(r.normalOct);
    v.tangent = octDecode(r.tangentOct);
    v.tangentSign = r.tangentSign ? -1.0 : 1.0;
    v.uv = r.uv;
    return v;
}

// Source triangle t of a mesh (mesh-relative vertex indices), as used by ray tracing and the reference.
uint3 loadTriangle(GpuMesh mesh, uint t)
{
    StructuredBuffer<uint> idx = ResourceDescriptorHeap[g_indices];
    const uint base = mesh.indexOffset + 3 * t;
    return uint3(idx[base], idx[base + 1], idx[base + 2]);
}

// Triangle t of a cluster (mesh-relative vertex indices).
uint3 loadClusterTriangle(GpuCluster c, uint t)
{
    StructuredBuffer<uint> tris = ResourceDescriptorHeap[g_clusterTriangles];
    StructuredBuffer<uint> verts = ResourceDescriptorHeap[g_clusterVertexIndices];
    const uint packed = tris[c.triangleOffset + t];
    return uint3(verts[c.vertexOffset + (packed & 0xFFu)], verts[c.vertexOffset + ((packed >> 8) & 0xFFu)], verts[c.vertexOffset + ((packed >> 16) & 0xFFu)]);
}

#endif
