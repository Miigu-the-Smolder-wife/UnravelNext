#pragma once
// GPU scene records (INTERFACES_KO.md 6.3). Byte-for-byte mirror of Passes/Common/Scene.hlsli and Frame.hlsli.
// Every buffer is a StructuredBuffer read through bindless indices published in FrameConstants.
// Layout changes follow the interface-change procedure (INTERFACES_KO.md 0); sizes are checked here and in HLSL.
#include "unx/core/Math.h"

#include <cstdint>

namespace unx::render::gpu
{
constexpr uint32_t kNone = 0xFFFFFFFFu;

struct Instance  // 144 B
{
    float4 objectToWorld[3];      // rows of the affine object -> world transform (this tick)
    float4 prevObjectToWorld[3];  // previous tick (motion, VSM invalidation); equal when static
    uint32_t mesh;
    uint32_t flags;               // scene::InstanceFlags
    uint32_t materialRemap;       // first entry in the material remap buffer (one per submesh) or kNone = mesh materials
    uint32_t bonePalette;         // first joint in the bone palettes, kNone when rigid
    uint32_t transformRevision;   // increments when objectToWorld changes
    uint32_t deformRevision;      // increments when skinning/wind changes this instance's vertices beyond a tick
    float windStiffness, windPhase, windAnchor;
    uint32_t pad0, pad1, pad2;
};
static_assert(sizeof(Instance) == 144);

struct Mesh  // 80 B
{
    float4 boundsSphere;     // object space centre (xyz) and radius (w)
    float3 boundsMin;
    uint32_t vertexOffset;   // first vertex in the vertex pool
    float3 boundsMax;
    uint32_t vertexCount;
    uint32_t indexOffset;    // source triangle list in the index pool (mesh-relative vertex indices)
    uint32_t triangleCount;
    uint32_t submeshOffset;  // first Submesh record
    uint32_t submeshCount;
    uint32_t clusterOffset;  // V: first cluster of this mesh's hierarchy
    uint32_t clusterCount;
    uint32_t lodLevelOffset; // V: first LodLevel record
    uint32_t skinOffset;     // first SkinVertex, kNone when rigid
};
static_assert(sizeof(Mesh) == 80);

struct Submesh  // 16 B
{
    uint32_t indexOffset;    // relative to Mesh::indexOffset
    uint32_t indexCount;
    uint32_t material;
    uint32_t pad0;
};
static_assert(sizeof(Submesh) == 16);

// Vertex pool, format v1 (uncompressed, 32 B). Read only through loadVertex() (Scene.hlsli) so the storage format can
// be compressed by V/M without an interface change.
struct Vertex  // 32 B
{
    float3 position;         // object space
    uint32_t normalOct;      // octahedral, snorm16 x 2
    uint32_t tangentOct;     // octahedral, snorm16 x 2
    float2 uv;
    uint32_t tangentSign;    // 0 or 1 (bitangent = sign * cross(n, t))
};
static_assert(sizeof(Vertex) == 32);

struct SkinVertex  // 16 B
{
    uint32_t joints01, joints23;   // uint16 x 4
    uint32_t weights01, weights23; // unorm16 x 4
};
static_assert(sizeof(SkinVertex) == 16);

// Cluster (meshlet) of the LOD hierarchy. Written by V's cluster builder; read by V (cull, raster), M (resolve), R (RT
// geometry from a LOD level) and S through V's raster service.
struct Cluster  // 64 B
{
    float4 boundsSphere;     // object space
    float4 normalCone;       // axis xyz, cos(cutoff) in w (w = -1: no cone culling)
    uint32_t vertexOffset;   // first entry in the cluster vertex-index pool (uint32 mesh-relative vertex indices)
    uint32_t triangleOffset; // first entry in the cluster triangle pool (uint32 = 3 x uint8 local indices)
    uint32_t counts;         // vertexCount | triangleCount << 8 | lodLevel << 16 | clusterFlags << 24
    uint32_t material;       // scene material; a cluster never spans submeshes
    float lodError;          // object-space error of this cluster
    float parentLodError;    // error of the group that replaces it (render when parent > threshold >= own)
    float minFeatureWidth;   // object-space metres: narrowest feature (ARCHITECTURE 2.1 bands A/B/C)
    uint32_t brick;          // band C voxel brick, kNone if none
};
static_assert(sizeof(Cluster) == 64);

struct LodLevel  // 16 B: one cut of the hierarchy, used for ray tracing geometry and diagnostics
{
    uint32_t clusterOffset, clusterCount;
    float error;             // object-space error of this level
    uint32_t triangleCount;
};
static_assert(sizeof(LodLevel) == 16);

struct Material  // 80 B
{
    float3 baseColor;
    float roughness;         // perceptual
    float3 emissive;         // nits
    float metallic;
    float specular;          // dielectric f0 = 0.08 * specular
    float alphaCutoff;       // 0 = opaque
    float transmission;
    float ior;
    uint32_t classFlags;     // MaterialClass | flags << 8 (MaterialFlags)
    uint32_t baseColorTexture;   // bindless SRV index or kNone
    uint32_t normalTexture;
    uint32_t roughMetalTexture;
    uint32_t emissiveTexture;
    uint32_t occlusionTexture;
    uint32_t revision;
    uint32_t pad0;
};
static_assert(sizeof(Material) == 80);

enum MaterialFlags : uint32_t
{
    MaterialTwoSided = 1u << 0,
    MaterialAlphaTested = 1u << 1,
};

struct Light  // 80 B
{
    float3 position;
    uint32_t typeFlags;      // LightType | castShadow << 8 | shadowIndex << 16 (S: VSM light slot, 0xFFFF = none)
    float3 forward;
    float range;
    float3 right;
    float intensity;         // candela (point/spot) or nits (area)
    float3 color;
    float spotScale;         // 1 / max(cos(inner) - cos(outer), 1e-4)
    float2 size;
    float spotOffset;        // -cos(outer) * spotScale
    uint32_t revision;
};
static_assert(sizeof(Light) == 80);

// One visible cluster of one instance in one view (V writes the list per view; vis id indexes it).
struct VisibleCluster  // 8 B
{
    uint32_t instance;
    uint32_t cluster;
};
static_assert(sizeof(VisibleCluster) == 8);

enum class ViewKind : uint32_t
{
    Main = 0,
    PlanarReflection = 1,   // rendered through FrameServices::renderView by R (INTERFACES_KO.md 5.4)
};

// Root CBV b1 (Frame.hlsli). One per view per frame, 1 KB slots.
struct FrameConstants
{
    float4x4 viewProj, prevViewProj, invViewProj, view, proj;  // row_major in HLSL
    float3 cameraPosition;
    float nearPlane;
    float4 clipPlane;        // world plane; keep dot(p, xyz) + w >= 0; all zero = none
    uint32_t viewWidth, viewHeight, viewKind, frameIndex;
    float time, deltaTime, exposure, tanHalfFovY;   // exposure = 1 / (1.2 * 2^ev100)
    float3 sunDirection;     // towards the sun
    float sunIlluminance;    // lux at the top of the atmosphere
    float3 sunColor;
    float sunAngularRadius;
    float3 windDirection;
    float windSpeed;
    // Scene buffers (bindless SRV indices).
    uint32_t instances, meshes, submeshes, vertices;
    uint32_t indices, clusters, clusterVertexIndices, clusterTriangles;
    uint32_t lodLevels, materials, materialRemap, lights;
    uint32_t skinVertices, bonePalette, prevBonePalette, materialModelLut;  // materialModelLut: E(mu, r) table (8.1)
    uint32_t instanceCount, meshCount, clusterCount, lightCount;
    uint32_t materialCount, sceneRevision, pad0, pad1;
};
static_assert(sizeof(FrameConstants) == 528);
} // namespace unx::render::gpu
