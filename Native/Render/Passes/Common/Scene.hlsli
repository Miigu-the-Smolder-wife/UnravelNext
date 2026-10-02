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
    float3 hairAbsorption;  // Hair class (v1.66): sigma_a, beta_N, cuticle tilt (beta_M = roughness, eta = ior); Glass: the
                            // solid body's sigma_a (1/m, A10 R-2); Water (v1.92): the medium's scattering sigma_s (1/m) and
                            // hairBetaN its Henyey-Greenstein g (turbid baths, defect queue 13 (75)); Subsurface: the mean
                            // free path (m, rgb; the screen-space scattering pass's) and hairBetaN the specular lobes' mix
    float hairBetaN;
    float hairTilt;
    float cutScale;          // Cut class (v1.66): triplanar repeats per metre, damage band width (m); Subsurface: the two
    float cutDamageWidth;    // specular lobes' roughness scales (MaterialModel.hlsli modelSubsurfaceOf)
    uint terrainLayers;      // Terrain class (v1.74): first layer | count << 24 in g_terrainLayers
};

struct GpuMaterialLayers  // v1.76 (A9): gpu::MaterialLayers, 64 B
{
    float clearcoat, clearcoatRoughness;
    uint coat;
    float coatEta;
    float3 sheenColor;  // A9 sheen (MATERIAL_SHEEN): colour C, perceptual roughness
    float sheenRoughness;
    float anisotropy;       // A9 anisotropy (MATERIAL_ANISOTROPIC; Passes/Material/Aniso.hlsli): strength, rotation
    float2 anisotropyRotation;  // (cos, sin)
    float filmCoverage;     // A9 thin film (MATERIAL_THIN_FILM; MaterialModel.hlsli modelFilmBegin): cover w, F table
    uint filmTable;         // offset in g_coatTable (scene::model::filmTable: MODEL_FILM_MU RGB points)
    float cloth;            // cloth blend (with MATERIAL_SHEEN): the share of the base's specular lobe the fuzz replaces
    float2 reserved;
};
GpuMaterialLayers loadMaterialLayers(uint i) { StructuredBuffer<GpuMaterialLayers> b = ResourceDescriptorHeap[g_materialLayers]; return b[i]; }

// The record of an eye material (MATERIAL_EYE: a Subsurface material with an iris; gpu::MaterialEye, 64 B), in the layer
// records' buffer and at the same index bits (MaterialModel.hlsli "Eye", Passes/Material/MaterialEye.hlsli).
struct GpuMaterialEye
{
    float irisRadius;       // uv units around (0.5, 0.5): where the iris ends (the limbus)
    float irisDepth;        // the cornea's apex above the iris plane, in iris radii
    float limbusWidth;      // in iris radii: the band over which the iris gives way to the sclera
    float limbusDarkening;  // [0, 1]: the limbal ring
    float pupilScale;       // the iris texture's radial remap (1 = as painted, > 1 a wider pupil)
    float concavity;        // the caustic normal's tilt at the limbus (0 = the flat iris plane)
    float eta;              // the aqueous humour's refractive index
    float reserved0;
    float3 axis;            // the optical axis in the mesh's object space (unit, out of the eye)
    float reserved1;
    float4 reserved2;
};
GpuMaterialEye loadMaterialEye(uint i) { StructuredBuffer<GpuMaterialEye> b = ResourceDescriptorHeap[g_materialLayers]; return b[i]; }

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
    // the light components (GpuSceneLayout.h Light; every word 0: a plain light) - read through the accessors below
    uint scales, scales2;
    float drawDistance, fadeRange, falloffExponent;
    uint barnDoor, sourceTexture, lightPad;
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
// scene::InstanceLightingChannels...: bits 4..6 = the instance's lighting channels ^ 1 (instanceLightingChannels below)
#define INSTANCE_HIDDEN (1u << 31)  // gpu::kInstanceHidden: skipped by every reader (GpuScene::setInstanceVisible)
// gpu::kInstanceMotionBreak (v1.45): a teleport or restore in this frame; prev* = current (zero motion), breakCentre = where
// its bounding sphere was in the previous rendered frame (caches keyed by the old place invalidate from it).
#define INSTANCE_MOTION_BREAK (1u << 30)
#define INSTANCE_VIEW_MODEL (1u << 29)  // gpu::kInstanceViewModel: a first-person view model (Passes/ViewModel)
// The instance's flags and deformation inputs let its geometry be elsewhere in another frame of the same scene revision:
// Dynamic, Skinned, Wind or a view model, a bone palette, morph or terrain patch (gpu::instanceMovable, GpuScene.h). Readers
// that split instances in two sets by it (V's RasterView::instanceSet, S's static / dynamic shadow pages) also count every
// instance past the scene's uploaded ones (run-time and GPU-written instances) as movable. The others keep their place
// and shape while their transform revision holds.
#define INSTANCE_MOVABLE_FLAGS (INSTANCE_DYNAMIC | INSTANCE_SKINNED | INSTANCE_WIND | INSTANCE_VIEW_MODEL)
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
#define MATERIAL_SHEEN (1u << 11)    // A9: the record's layer is a sheen (shade class Sheen), else a clearcoat
#define MATERIAL_ANISOTROPIC (1u << 12)  // A9: the record's anisotropy is used (with MATERIAL_LAYERED)
#define MATERIAL_THIN_FILM (1u << 13)    // A9: the record's thin film is used (with MATERIAL_LAYERED)
#define MATERIAL_EMISSIVE_VISIBLE_ONLY (1u << 14)  // v1.92 (defect queue 13 (76)): the emission is seen by primary and reflection rays only; GI update rays and the emissive cache take 0 (rtEmissionAt's GI callers)
#define MATERIAL_EYE (1u << 15)  // a Subsurface material with an iris: its GpuMaterialEye record's index in classFlags bits 16..31 (loadMaterialEye)
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
// The light's own shadow-ray end bias (m; revision bits 16..31 as a half float), 'fallback' when it has none (sign bit).
float lightRayEndBias(GpuLight l, float fallback) { return (l.revision & 0x80000000u) != 0 ? fallback : f16tof32(l.revision >> 16); }

// ---- light components (scene::Light; Unreal's local light components). One place for what every consumer of a light
// must agree on: the opaque and coverage shading, the sampled lights, the air and fog, the surface cache, the ray hits.
//   lighting channels   3 bits on a light and on an instance: a light lights the instances that share one. A kernel
//                       that shades a point of an instance sets g_lightChannels to the instance's channels before it
//                       evaluates lights (the light samples' weights of the opaque surface: MegaLightsSample.hlsl);
//                       lightWindow is then 0 for a light in none of them. Unset (7): no test - the coverage layer's
//                       fragments and the hair (their records name no instance), the air and the fog, the ray hits,
//                       the surface cache's cards (Unreal tests neither fog nor hit lighting).
//   scales              specular, diffuse (direct shading), volumetric (the air's and fog's in-scattering), indirect (the
//                       surface cache's direct light and the ray hits' light samples).
//   lightWindow         what multiplies intensity / d^2 at distance d from the light: the range's window
//                       saturate(1 - (d / range)^4)^2 of INTERFACES 8.2 - or, for a point or spot light with a falloff
//                       exponent n, (1 - (d / range)^2)^n x d^2 (no inverse square: the intensity is the illuminance at
//                       the light) -, times lightViewFade.
//   lightViewFade       1, or the fade towards the light's draw distance from this view's camera:
//                       saturate((drawDistance - |light - camera|) / fadeRange), a cut without a fade range.
// UNX_LIGHT_COMPONENTS 0 before this file compiles the scales, the exponent and the fade out (every light as a plain one):
// for a kernel at the DXIL size limit; such a kernel says so in its header.
#ifndef UNX_LIGHT_COMPONENTS
#define UNX_LIGHT_COMPONENTS 1
#endif
uint lightChannels(GpuLight l) { return ((l.typeFlags >> 9) & 7u) ^ 1u; }
uint instanceLightingChannels(uint instanceFlags) { return ((instanceFlags >> 4) & 7u) ^ 1u; }
static uint g_lightChannels = 7u;  // the receiver's channels
#if UNX_LIGHT_COMPONENTS
float lightSpecularScale(GpuLight l) { return 1 + f16tof32(l.scales & 0xFFFFu); }
float lightDiffuseScale(GpuLight l) { return 1 + f16tof32(l.scales >> 16); }
float lightVolumetricScale(GpuLight l) { return 1 + f16tof32(l.scales2 & 0xFFFFu); }
float lightIndirectScale(GpuLight l) { return 1 + f16tof32(l.scales2 >> 16); }
float lightViewFade(GpuLight l)
{
    if (!(l.drawDistance > 0)) return 1;
    const float d = distance(l.position, g_cameraPosition);
    return l.fadeRange > 0 ? saturate((l.drawDistance - d) / l.fadeRange) : (d < l.drawDistance ? 1.0 : 0.0);
}
float lightWindow(GpuLight l, float d)
{
    const float x = d / max(l.range, 1e-6), x2 = x * x;
    float w = saturate(1 - x2 * x2);
    w *= w;
    if (l.falloffExponent > 0) w = pow(saturate(1 - x2), l.falloffExponent) * (d * d);
    if ((lightChannels(l) & g_lightChannels) == 0) w = 0;
    return w * lightViewFade(l);
}
#else
float lightSpecularScale(GpuLight l) { return 1; }
float lightDiffuseScale(GpuLight l) { return 1; }
float lightVolumetricScale(GpuLight l) { return 1; }
float lightIndirectScale(GpuLight l) { return 1; }
float lightViewFade(GpuLight l) { return 1; }
float lightWindow(GpuLight l, float d)
{
    const float x = d / max(l.range, 1e-6), x2 = x * x;
    const float w = saturate(1 - x2 * x2);
    return w * w;
}
#endif
// The part of a rect light a point sees past its barn doors (scene::Light::barnDoorAngle / Length; Unreal's GetRect with
// bComputeVisibleRect): four flaps along the emitter's edges, 'height' in front of it and spread outwards by 'spread'.
// From a point outside a flap, the flap's top edge hides the strip of the emitter behind it - the emitter's near side
// moves in by height x (the point's offset past the edge) / (its distance in front of the top) - spread at the height;
// a point beside the housing below the flaps' top sees nothing. p: the light's centre relative to the point; up =
// forward x right. centre, halfSize: the visible rect (in: the emitter's). false: nothing is visible.
bool lightBarnDoorRect(GpuLight l, float3 p, float3 up, inout float3 centre, inout float2 halfSize)
{
    const float height = f16tof32(l.barnDoor & 0xFFFFu), spreadTop = f16tof32(l.barnDoor >> 16);
    const float3 s = float3(dot(-p, l.right), dot(-p, up), dot(-p, l.forward));  // the point in the light's frame
    const float depth = min(s.z, height);
    const float spread = spreadTop * depth / max(height, 1e-4);
    const float2 side = float2(s.x < 0 ? -1.0 : 1.0, s.y < 0 ? -1.0 : 1.0);
    const float2 edge = halfSize + spread;  // the flaps' edge at that depth, on the point's side
    const float2 shift = depth * (max(abs(s.xy), edge) - edge) / max(s.z - depth, 1e-3) - spread;
    const float2 lo = clamp(-halfSize + shift * max(0.0, -side), -halfSize, halfSize);
    const float2 hi = clamp(halfSize - shift * max(0.0, side), -halfSize, halfSize);
    if (any(hi - lo <= 0)) return false;
    const float2 mid = 0.5 * (lo + hi);
    centre = p + l.right * mid.x + up * mid.y;
    halfSize = 0.5 * (hi - lo);
    return true;
}

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

// Compressed cluster vertices (visibility.cluster_compression): loadClusterVertex, the fetch of a cluster's local vertex.
#include "ClusterStream.hlsli"

#endif
