#pragma once
// GPU scene records (INTERFACES_KO.md 6.3). Byte-for-byte mirror of Passes/Common/Scene.hlsli and Frame.hlsli.
// Every buffer is a StructuredBuffer read through bindless indices published in FrameConstants.
// Layout changes follow the interface-change procedure (INTERFACES_KO.md 0); sizes are checked here and in HLSL.
#include "unx/core/Math.h"
#include "unx/render/ViewKind.h"

#include <cstdint>

namespace unx::render::gpu
{
constexpr uint32_t kNone = 0xFFFFFFFFu;
constexpr uint32_t kInstanceHidden = 1u << 31;  // Instance::flags, GPU scene only (GpuScene::setInstanceVisible): every reader skips it
// Instance::flags, GPU scene only, set for one frame (v1.45): the instance's motion history broke in this frame (a teleport,
// UNX_TRANSFORM_TELEPORT, or a restore discontinuity: prevObjectToWorld = objectToWorld and the previous palette = the
// current one, so its motion is zero). Caches keyed by where the instance was drawn before (VSM pages) cannot measure the
// jump from prevObjectToWorld: breakCentre holds the world centre of its bounding sphere in the previous rendered frame.
constexpr uint32_t kInstanceMotionBreak = 1u << 30;
// v1.53 (A12, E): a first-person view model (GPU scene only, GpuScene::setInstanceViewModel): a camera-attached instance
// E places at the frame's camera every rendered frame (Passes/ViewModel). V remaps its main-view projection by
// FrameConstants::viewModelScale (ViewModel.hlsli); M leaves its pixels out of the camera-rotation blur.
constexpr uint32_t kInstanceViewModel = 1u << 29;

struct Instance  // 160 B
{
    float4 objectToWorld[3];      // rows of the affine object -> world transform (this tick)
    float4 prevObjectToWorld[3];  // previous rendered frame's objectToWorld (motion, HiZ phase 1, VSM invalidation); equal when static
    uint32_t mesh;
    uint32_t flags;               // scene::InstanceFlags | kInstanceHidden
    uint32_t materialRemap;       // first entry in the material remap buffer (one per submesh) or kNone = mesh materials
    uint32_t bonePalette;         // first joint in the bone palettes, kNone when rigid
    uint32_t transformRevision;   // increments when objectToWorld changes
    uint32_t deformRevision;      // increments when skinning/wind changes this instance's vertices beyond a tick
    float windStiffness, windPhase, windAnchor;
    float3 breakCentre;           // kInstanceMotionBreak: world centre of the bounding sphere in the previous rendered frame
    // C4 (render C): blend shapes / vertex animation. morph = first row of the instance's record in the morph records
    // (FrameConstants::morphRecords), kNone when its mesh has neither; morphRadius = object-space bound of the offset
    // from the bind pose at the current and previous weights (scene::morphBound), added to every culling sphere.
    uint32_t morph;
    float morphRadius;
    // C5 (render C): terrain deformation patches. patch = slot in FrameConstants::patchData (kPatchSlotElements uint4 per
    // slot), kNone when no block of this instance is replaced. Only V reads it (forced source clusters in the replaced
    // rectangle, source triangles of replaced blocks dropped).
    uint32_t patch;
    uint32_t morphPad;
};
static_assert(sizeof(Instance) == 160);

// C5 terrain patch slot (FrameConstants::patchData, StructuredBuffer<uint4>): element 0 = float4 (block grid origin x,
// z, block size x, z; object space, signed), 1 = float4 rectangle of the replaced blocks (min x, min z, max x, max z),
// 2 = uint4 (blocks per side, replaced count, 0, 0), 3..34 = the replaced-block bit mask (block bj * side + bi), 4096 bits.
constexpr uint32_t kPatchSlotElements = 35;
constexpr uint32_t kPatchMaxBlocksPerSide = 64;
constexpr uint32_t kPatchSlots = 64;
// GPU-written instances (A3 mesh particles, GpuScene::gpuInstanceRange): their live count is element
// kGpuInstanceCountElement of patchData (.x), zeroed by the scene update every frame before the writer runs.
constexpr uint32_t kGpuInstanceCountElement = kPatchSlots * kPatchSlotElements;

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
    float4 boundsSphere;     // object space, tight (culling)
    float4 normalCone;       // axis xyz; w = cutoff (meshoptimizer cone_cutoff = sin of the normal spread half-angle).
                             // Every triangle faces away from p when dot(c - p, axis) >= w * |c - p| + radius (c, radius:
                             // boundsSphere). w >= 1: no cone
    uint32_t vertexOffset;   // first entry in the cluster vertex-index pool (uint32 mesh-relative vertex indices)
    uint32_t triangleOffset; // first entry in the cluster triangle pool (uint32 = 3 x uint8 local indices)
    uint32_t counts;         // vertexCount | triangleCount << 8 | submesh << 16 (index within the mesh; a cluster never
                             // spans submeshes). Material with instance overrides: clusterMaterial() (Scene.hlsli)
    uint32_t material;       // scene material of the submesh (before instance overrides)
    float lodError;          // object-space error of this cluster's geometry (0 = source geometry)
    float parentLodError;    // error of the group that replaces it (FLT_MAX: never replaced)
    float minFeatureWidth;   // object-space metres, narrowest feature (ARCHITECTURE 2.1 bands A/B/C). > 0: solid
                             // (projected width is view independent); < 0: flat sheet of width |w| whose projected width
                             // shrinks with |cos| between the view direction and the sheet normals (normalCone)
    uint32_t brick;          // band C voxel brick, kNone if none
};
static_assert(sizeof(Cluster) == 64);

struct LodLevel  // 16 B: one uniform-error cut of a mesh's hierarchy (ray tracing geometry, diagnostics)
{
    uint32_t clusterOffset, clusterCount;  // range in lodLevelClusters (FrameConstants), which holds cluster indices
    float error;             // object-space error of this cut (0 = source geometry)
    uint32_t triangleCount;
};
static_assert(sizeof(LodLevel) == 16);

struct Material  // 112 B
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
    uint32_t textureClamp;   // bit per texture (MaterialTextureBit): 1 = clamp addressing (g_anisoClamp), 0 = wrap
    // Hair class (v1.66): sigma_a (scene::model::hairAbsorption), beta_N, cuticle tilt; beta_M = roughness, eta = ior.
    // Glass class (A10 R-2): hairAbsorption holds the solid body's sigma_a = -ln(baseColor) / attenuationDistance (1/m).
    float3 hairAbsorption;
    float hairBetaN;
    float hairTilt;
    // Cut class (v1.66): triplanar texture repeats per metre, damage band width (m).
    float cutScale;
    float cutDamageWidth;
    // Terrain class (v1.74): its layers in FrameConstants::terrainLayers, first | count << 24 (count 1..8).
    uint32_t terrainLayers;
};
static_assert(sizeof(Material) == 112);

// One layer of a Terrain-class material (v1.74): a Standard material read at terrain uv0 x scale + offset.
struct TerrainLayer  // 32 B
{
    uint32_t material;
    float scaleU, scaleV, offsetU, offsetV;
    uint32_t splatSize[2];  // the material's splats 0 and 1: width | height << 16 (0: none), in every record of the material
    uint32_t pad;
};
static_assert(sizeof(TerrainLayer) == 32);

// A9 material layers (MATERIAL_LAYERS 1.3): the layer parameters of a layered material, in their own buffer
// (FrameConstants::materialLayers) so the hot 112 B record is unchanged; a layered material has MaterialLayered and
// its record index in classFlags bits 16..31.
struct MaterialLayers  // 64 B
{
    float clearcoat;            // cover c (0 = none)
    float clearcoatRoughness;   // perceptual r_c
    uint32_t coat;              // tabulated coat (scene::model::coatIndex: 0 eta 1.5, 1 eta 1.33)
    float coatEta;
    float sheenColor[3];        // A9 sheen (MaterialSheen; MATERIAL_LAYERS 1.4): colour C (0 = none), perceptual r_sh
    float sheenRoughness;
    float anisotropy;           // A9 anisotropy (MaterialAnisotropic; MATERIAL_LAYERS 1.5): strength s, the rotation's
    float anisotropyCos;        // cos and sin (from the cooked tangent towards the bitangent)
    float anisotropySin;
    float filmCoverage;         // A9 thin film (MaterialThinFilm; MATERIAL_LAYERS 1.2): cover w and the film's F table
    uint32_t filmTable;         // (scene::model::filmTable, kFilmTableMu RGB points) as a float offset into coatTable
    float reserved[3];
};
static_assert(sizeof(MaterialLayers) == 64);

// Textures of one material as M's texture system publishes them (GpuScene::setMaterialTextures, INTERFACES 6.3 v1.10):
// bindless SRV indices (kNone = none; the SRVs belong to M) and clamp bits (MaterialTextureBit).
struct MaterialTextures
{
    uint32_t baseColor = kNone, normal = kNone, roughMetal = kNone, emissive = kNone, occlusion = kNone;
    uint32_t clamp = 0;
};

enum MaterialTextureBit : uint32_t
{
    MaterialTextureBaseColor = 1u << 0,
    MaterialTextureNormal = 1u << 1,
    MaterialTextureRoughMetal = 1u << 2,
    MaterialTextureEmissive = 1u << 3,
    MaterialTextureOcclusion = 1u << 4,
};

enum MaterialFlags : uint32_t
{
    MaterialTwoSided = 1u << 0,
    MaterialAlphaTested = 1u << 1,
    MaterialLayered = 1u << 2,  // A9: a MaterialLayers record, index in classFlags bits 16..31
    MaterialSheen = 1u << 3,    // A9: the record's layer is a sheen (shade class Sheen; else a clearcoat)
    MaterialAnisotropic = 1u << 4,  // A9: the record's anisotropy is used (with MaterialLayered; a coat may also be present)
    MaterialThinFilm = 1u << 5,     // A9: the record's film is used (with MaterialLayered; no coat, sheen or anisotropy)
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
    uint32_t materialCount, sceneRevision, lodLevelClusters, specularAlbedoLut;  // lodLevelClusters: cluster indices of LodLevel cuts;
                                                                                 // specularAlbedoLut: (A, B) table (8.1, v1.25)
    // v1.34: coverageMaskLut: edge half-plane masks for coverageTriangleMaskLut (Coverage.hlsli, 5.5.1); giRaysThisFrame:
    // this frame's share of gi.rays_per_tick (10.3 of the design revision; 0 until the renderer hands it out).
    // v1.50 (A15, E): debugDraw: UAV of this frame's debug primitive buffer (Passes/Debug/DebugDraw.hlsli: debugLine,
    // debugTriangle, debugText append to it from any kernel); 0xFFFFFFFF = debug drawing off (appends are no-ops).
    // v1.53 (A12, E): viewModelScale: the main view's projection remap of view-model instances (clip.xy x it; 1 = none,
    // the default: the view model is drawn with the world's camera; viewmodel.fov_override_degrees sets it).
    uint32_t coverageMaskLut, giRaysThisFrame, debugDraw;
    float viewModelScale;
    // C4 (render C): morphRecords (StructuredBuffer<float4>: per morph instance {mesh block, weights row, time, previous
    // time}, then weight rows) and morphData (raw: per morph mesh a block, Passes/Common/Deformation.hlsli morphVertex).
    // C5: patchData (StructuredBuffer<uint4>, kPatchSlots slots of kPatchSlotElements, then the GPU-written instance count;
    // kNone without a runtime pool).
    uint32_t morphRecords, morphData, patchData;
    uint32_t terrainLayers;  // v1.74: StructuredBuffer<TerrainLayer> of the Terrain-class materials (kNone: none)
    // v1.76 (A9): materialLayers: StructuredBuffer<MaterialLayers> (kNone: no layered material); coatTable:
    // StructuredBuffer<float> scene::model::coatTable() (kCoatTableStride floats per tabulated coat).
    // v1.79 (A3 FX particle lights, S_STATUS 10): fxLightCount: StructuredBuffer<uint> SRV whose element 0 is F, the FX
    // lights the GPU wrote this frame at lights[lightCount, lightCount + F) (kNone: no FX light tail); fxLightCapacity:
    // F_max, the tail's size (readers use min(F, fxLightCapacity)). lightCount + fxLightCapacity <= 32768.
    uint32_t materialLayers, coatTable, fxLightCount, fxLightCapacity;
    // Temporal upscale (output.render_scale, FrameContext::Upscale): upscaleRatio = the main view's internal / output
    // height while it renders below the output (0 = native; every other view 0). M's texture footprints are taken over
    // the output pixel (x upscaleRatio, the temporal upsamplers' mip bias log2 ratio): the upscale accumulates the
    // jittered internal samples into output pixels, which then show the native resolution's texture detail.
    float upscaleRatio;
    uint32_t pad0, pad1, pad2;
};
static_assert(sizeof(FrameConstants) == 592);
} // namespace unx::render::gpu
