#pragma once
// GPU scene records (INTERFACES_KO.md 6.3). Byte-for-byte mirror of Passes/Common/Scene.hlsli and Frame.hlsli.
// Every buffer is a StructuredBuffer read through bindless indices published in FrameConstants.
// Layout changes follow the interface-change procedure (INTERFACES_KO.md 0); sizes are checked here and in HLSL.
#include "unx/core/Math.h"
#include "unx/render/ViewKind.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>

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
constexpr uint32_t kCompactVertexOffset = 1u << 31;
inline uint32_t vertexStride(const Mesh& m) { return (m.vertexOffset & kCompactVertexOffset) ? 28u : 32u; }
inline uint64_t vertexByteOffset(const Mesh& m) { return (uint64_t)(m.vertexOffset & ~kCompactVertexOffset) * vertexStride(m); }

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
    uint32_t inputs;         // the material's MaterialInputs record in FrameConstants::materialInputs, kNone: none (until the
                             // material inputs this was an occlusion texture slot that M never published: always kNone)
    uint32_t revision;
    uint32_t textureClamp;   // bit per texture (MaterialTextureBit): 1 = clamp addressing (g_anisoClamp), 0 = wrap
    // Hair class (v1.66): sigma_a (scene::model::hairAbsorption), beta_N, cuticle tilt; beta_M = roughness, eta = ior.
    // Glass class (A10 R-2): hairAbsorption holds the solid body's sigma_a = -ln(baseColor) / attenuationDistance (1/m).
    // Subsurface class: hairAbsorption holds the mean free path (m, rgb; the screen-space scattering pass's) and hairBetaN
    // the specular lobes' mix (scene::Material::subsurfaceLobeMix).
    float3 hairAbsorption;
    float hairBetaN;
    float hairTilt;
    // Cut class (v1.66): triplanar texture repeats per metre, damage band width (m).
    // Subsurface class: the two specular lobes' roughness scales (scene::Material::subsurfaceLobeRoughness).
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
    float cloth;                // cloth blend (with MaterialSheen): the share of the base's specular lobe the fuzz replaces
    float reserved[2];
};
static_assert(sizeof(MaterialLayers) == 64);

// The record of an eye material (MaterialEye: a Subsurface material with an iris; scene::model::Eye) - one of the layer
// buffer's records, at the index in classFlags bits 16..31 (a Subsurface material has no layers).
struct MaterialEyeRecord  // 64 B
{
    float irisRadius, irisDepth, limbusWidth, limbusDarkening;
    float pupilScale, concavity, eta, reserved0;
    float axis[3];  // the optical axis in the mesh's object space (unit)
    float reserved1;
    float reserved2[4];
};
static_assert(sizeof(MaterialEyeRecord) == sizeof(MaterialLayers));

// Material inputs (scene::Material's uv transform, second uv set, detail maps, height, emissive mask, vertex colour and
// dithered opacity; Passes/Common/Scene.hlsli GpuMaterialInputs): the record of a material that has any, at
// Material::inputs in FrameConstants::materialInputs. Texture fields are M's SRVs (GpuScene::setMaterialTextures).
struct MaterialInputs  // 80 B
{
    float uvU[3];            // the material's uv: u' = uvU.xy . uv + uvU.z,
    float detailScaleU;      //   the detail maps': uv(set) x detailScale + detailOffset
    float uvV[3];            //   v' = uvV.xy . uv + uvV.z
    float detailScaleV;
    float detailOffset[2];
    float detailColor;       // strength of the detail colour [0, 1]
    float detailNormal;      // scale of the detail normal's slopes
    uint32_t flags;          // MaterialInputFlags
    uint32_t detailColorTexture, detailNormalTexture;  // RGBA8 sRGB; M's slope moments (RGBA16_UNORM)
    float detailSlopeRange;  // S of the detail moments' encoding
    uint32_t heightTexture;  // R8
    float heightScale;       // m
    uint32_t emissiveMaskTexture;  // R8
    uint32_t textureClamp;   // clamp addressing: 1 detail colour, 2 detail normal, 4 height, 8 emissive mask
};
static_assert(sizeof(MaterialInputs) == 80);

enum MaterialInputFlags : uint32_t
{
    MaterialInputUv = 1u << 0,            // the uv transform is not the identity
    MaterialInputOcclusionUv1 = 1u << 1,  // the occlusion map is read on the second uv set
    MaterialInputDetailUv1 = 1u << 2,     // the detail maps are read on the second uv set
    MaterialInputVertexTint = 1u << 3,    // the vertex colour's rgb multiplies the base colour
    MaterialInputVertexBlend = 1u << 4,   // the vertex colour's alpha weighs the detail maps
    MaterialInputDither = 1u << 5,        // dithered opacity (the views' alpha test)
};

// A mesh's optional vertex streams (scene::Mesh::uv1, colors), one record per vertex of a mesh that has either
// (FrameConstants::vertexAttributes; the mesh's first record + 1 in FrameConstants::meshAttributes, 0: none).
struct VertexAttributes  // 16 B
{
    float2 uv1;              // the second uv set (a mesh with colours only: its uv0)
    uint32_t color;          // RGBA8, r in the low byte (a mesh with uv1 only: white)
    uint32_t pad;
};
static_assert(sizeof(VertexAttributes) == 16);

// Textures of one material as M's texture system publishes them (GpuScene::setMaterialTextures, INTERFACES 6.3 v1.10):
// bindless SRV indices (kNone = none; the SRVs belong to M) and clamp bits (MaterialTextureBit). The material inputs'
// textures go to the material's MaterialInputs record.
struct MaterialTextures
{
    uint32_t baseColor = kNone, normal = kNone, roughMetal = kNone, emissive = kNone;
    uint32_t clamp = 0;
    uint32_t detailColor = kNone, detailNormal = kNone, height = kNone, emissiveMask = kNone;
    float detailSlopeRange = 0;
    uint32_t inputClamp = 0;  // MaterialInputs::textureClamp
};

enum MaterialTextureBit : uint32_t
{
    MaterialTextureBaseColor = 1u << 0,
    MaterialTextureNormal = 1u << 1,
    MaterialTextureRoughMetal = 1u << 2,
    MaterialTextureEmissive = 1u << 3,
    MaterialTextureOcclusion = 1u << 4,
    MaterialTextureEmissiveMask = 1u << 5,  // M's table: the material has an emissive mask (its emission is per pixel)
};

enum MaterialFlags : uint32_t
{
    MaterialTwoSided = 1u << 0,
    MaterialAlphaTested = 1u << 1,
    MaterialLayered = 1u << 2,  // A9: a MaterialLayers record, index in classFlags bits 16..31
    MaterialSheen = 1u << 3,    // A9: the record's layer is a sheen (shade class Sheen; else a clearcoat)
    MaterialAnisotropic = 1u << 4,  // A9: the record's anisotropy is used (with MaterialLayered; a coat may also be present)
    MaterialThinFilm = 1u << 5,     // A9: the record's film is used (with MaterialLayered; no coat, sheen or anisotropy)
    MaterialEmissiveVisibleOnly = 1u << 6,  // v1.92 (defect queue 13 (76)): emissive for primary and reflection rays only (GI: 0)
    MaterialEye = 1u << 7,          // a Subsurface material with an iris: a MaterialEyeRecord, index in classFlags bits 16..31
};

// A float as a half float's 16 bits (round toward zero; magnitudes past the half range saturate, below 2^-14: 0).
inline uint32_t halfFloatBits(float value)
{
    uint32_t u;
    std::memcpy(&u, &value, 4);
    const uint32_t sign = (u >> 16) & 0x8000u;
    const int32_t e = (int32_t)((u >> 23) & 0xFF) - 127 + 15;
    return sign | (e <= 0 ? 0u : (e >= 31 ? 0x7BFFu : (((uint32_t)e << 10) | ((u >> 13) & 0x3FFu))));
}

// A linear colour as an RGB9E5 word (three 9-bit mantissas under one 5-bit exponent, DXGI's R9G9B9E5_SHAREDEXP layout:
// channel = mantissa x 2^(exponent - 24)), and back.
inline uint32_t rgb9e5(float3 c)
{
    const float limit = 65408.0f;  // (511 / 512) x 2^16
    const float r = std::min(std::max(c.x, 0.0f), limit), g = std::min(std::max(c.y, 0.0f), limit), b = std::min(std::max(c.z, 0.0f), limit);
    const float top = std::max(r, std::max(g, b));
    int exponent = std::max(-16, top > 0 ? (int)std::floor(std::log2(top)) : -16) + 1 + 15;
    float unit = std::exp2((float)(exponent - 24));
    if ((int)std::floor(top / unit + 0.5f) == 512)
    {
        unit *= 2;
        ++exponent;
    }
    return (uint32_t)std::floor(r / unit + 0.5f) | ((uint32_t)std::floor(g / unit + 0.5f) << 9) | ((uint32_t)std::floor(b / unit + 0.5f) << 18) | ((uint32_t)exponent << 27);
}
inline float3 rgb9e5ToFloat(uint32_t v)
{
    const float unit = std::exp2((float)(int)(v >> 27) - 24.0f);
    return { (float)(v & 0x1FFu) * unit, (float)((v >> 9) & 0x1FFu) * unit, (float)((v >> 18) & 0x1FFu) * unit };
}

struct Light  // 112 B
{
    float3 position;
    uint32_t typeFlags;      // LightType | castShadow << 8 | (lighting channels ^ 1) << 9 (3 bits; 0: channel 0 alone) |
                             // shadowIndex << 16 (S: VSM light slot, 0xFFFF = none)
    float3 forward;
    float range;
    float3 right;
    float intensity;         // candela (point/spot) or nits (area)
    float3 color;
    float spotScale;         // 1 / max(cos(inner) - cos(outer), 1e-4)
    float2 size;
    float spotOffset;        // -cos(outer) * spotScale
    uint32_t revision;       // bits 0..15: the record's change count (wraps); bits 16..31 (v1.93): the light's shadow-ray end
                             // bias in metres as a half float, sign bit set = none of its own (Scene.hlsli lightRayEndBias)
    // ---- the light components (scene::Light; Scene.hlsli's accessors). Every word is 0 for a light that sets none, so a
    // zeroed record (the FX particle lights, tests) is a plain light.
    uint32_t scales;         // half(specularScale - 1) | half(diffuseScale - 1) << 16
    uint32_t scales2;        // half(volumetricScattering - 1) | half(indirectIntensity - 1) << 16
    float drawDistance;      // m from the camera past which the light is not drawn; 0: no limit
    float fadeRange;         // m before drawDistance over which it fades; 0: cut
    float falloffExponent;   // point, spot: 0 = inverse square; > 0: (1 - (d / range)^2)^exponent, no inverse square
    uint32_t barnDoor;       // rect: half(flap height = length x cos angle) | half(flap spread = length x sin angle) << 16; 0: none
    uint32_t sourceTexture;  // rect: SRV of the emitter's image + 1 (M's TextureSystem publishes it; 0: uniform)
    uint32_t sourceMean;     // with sourceTexture: the image's mean colour (rgb9e5) - what a consumer that takes the light
                             // as a point takes for the image (the air and fog, particles, the FAR tile terms, ray hits)
};
static_assert(sizeof(Light) == 112);
// Light::revision from a change count and scene::Light::rayEndBias (negative: none).
inline uint32_t lightRevisionWord(uint32_t count, float rayEndBias)
{
    uint32_t half = 0xBC00u;  // -1.0
    if (rayEndBias >= 0)
    {
        uint32_t u;
        std::memcpy(&u, &rayEndBias, 4);
        const int32_t e = (int32_t)((u >> 23) & 0xFF) - 127 + 15;
        half = e <= 0 ? 0u : (e >= 31 ? 0x7BFFu : (((uint32_t)e << 10) | ((u >> 13) & 0x3FFu)));  // (below 2^-14 m: 0)
    }
    return (count & 0xFFFFu) | (half << 16);
}

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
    // blueNoise: SRV of the blue-noise tile (unx/render/BlueNoise.h: 64 x 64 RGBA16_UNORM, four patterns; Passes/Common/
    // BlueNoise.hlsli blueNoise4); kNone = none (the readers fall back to a hash).
    // fog: the main view's fog parameters (Passes/Atmosphere/FogVolume.hlsli FogParams; a raw buffer's SRV + 1), 0 = no
    // fog in this view. The air lookups of every layer (atmosphereAerial / atmosphereAirView) take the fog with it.
    // materialInputs: StructuredBuffer<MaterialInputs> (kNone: no material has one). meshAttributes: StructuredBuffer<uint>
    // per mesh, 1 + the mesh's first record in vertexAttributes (StructuredBuffer<VertexAttributes>), 0: the mesh has
    // neither a second uv set nor vertex colours (kNone: no mesh has).
    // clusterStream: the SRV + 1 of the compressed cluster vertices (ClusterData::named "clusterStream", Tools/
    // ClusterBuilder ClusterStream.h; Passes/Common/ClusterStream.hlsli loadClusterVertex), 0 = the scene has none.
    // clusterPages: the SRV + 1 of the frame's cluster page table (visibility.cluster_streaming: tracks::
    // clusterPageTable), 0 = no streaming this frame. Frame.hlsli declares the two only with UNX_CLUSTER_STREAM.
    // A zero-initialized frame may be built by isolated tracks and host diagnostics. Descriptor 0
    // is a real buffer, not an absent optional texture: use the shared absent-resource sentinel.
    uint32_t blueNoise = kNone;
    uint32_t fog = 0;  // SRV + 1: zero is the absent fog convention
    uint32_t materialInputs = kNone;
    uint32_t meshAttributes, vertexAttributes, clusterStream, clusterPages;
    uint32_t vertexSigns = kNone;
    uint32_t vertexPadding[3] = {};
};
static_assert(sizeof(FrameConstants) == 624);
} // namespace unx::render::gpu
