#pragma once
// Scene data (INTERFACES_KO.md 6). The one scene description every consumer reads: the test scene generator (C) writes
// it, the CPU reference path tracer (C) renders it directly, the real-time renderer builds its GPU representation from it
// (clusters, bricks, BLAS). Conventions: unx/core/Math.h (right-handed, +Y up, metres).
//
// Units: lengths in metres; angles in radians; colours linear Rec.709 primaries; emitted radiance in cd/m^2 (nits);
// point/spot intensity in candela; sun illuminance in lux at the top of the atmosphere.
//
// Versioning: kSceneFormatVersion changes only through the interface-change procedure (INTERFACES_KO.md 0).
#include "unx/core/Math.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace unx::scene
{
constexpr uint32_t kSceneFormatVersion = 1;
constexpr uint32_t kNone = 0xFFFFFFFFu;

enum class TextureFormat : uint8_t
{
    Rgba8Srgb = 0,  // base colour (+ alpha coverage)
    Rgba8Linear = 1,
    Rg8Normal = 2,     // tangent-space normal XY in [0,1]; Z reconstructed
    Rg8RoughMetal = 3, // R = perceptual roughness, G = metallic (linear)
    R8Linear = 4,      // occlusion or single-channel masks
    Rgba16Float = 5,   // HDR (emission maps)
};

struct Texture
{
    std::string name;
    uint32_t width = 0, height = 0;
    TextureFormat format = TextureFormat::Rgba8Srgb;
    bool wrap = true;              // repeat (true) or clamp (false) addressing, both axes
    std::vector<uint8_t> texels;   // mip 0 only, rows top to bottom; consumers build their own mip chains
};

// Material classes select the shading model and the shading-kernel class (ARCHITECTURE 2.11). The reference path
// tracer implements the same model per class (INTERFACES_KO.md 8).
enum class MaterialClass : uint8_t
{
    Standard = 0,    // opaque/alpha-tested dielectric-metal (Lambert + GGX, 8.1)
    Foliage = 1,     // two-sided thin leaf: Standard + diffuse transmission
    Hair = 2,        // strands (P3)
    Water = 3,       // water surface (P4)
    Glass = 4,       // thin/solid dielectric (P4)
    Subsurface = 5,  // skin and similar (P4)
    Cut = 6,         // destruction cut faces (A11): Standard shading, textures by object-space triplanar projection
    Terrain = 7,     // terrain layer blending (C5, FEATURES_GAME 9): Standard shading of up to 8 splat-weighted layers
};

// A layer of a Terrain-class material: a Standard material read at layer uv = terrain uv0 x scale + offset.
struct TerrainLayer
{
    uint32_t material = kNone;  // a Standard-class material of the scene
    float2 scale{ 1, 1 };
    float2 offset{ 0, 0 };
};

struct Material
{
    std::string name;
    MaterialClass cls = MaterialClass::Standard;
    float3 baseColor{ 0.5f, 0.5f, 0.5f };  // linear albedo (dielectric) or f0 (metal)
    float roughness = 0.5f;                  // perceptual roughness r; GGX alpha = r*r
    float metallic = 0.0f;
    float specular = 0.5f;                   // dielectric f0 = 0.08 * specular (0.5 -> 0.04)
    float3 emissive{ 0, 0, 0 };              // nits
    float alphaCutoff = 0.0f;                // 0 = opaque; otherwise alpha-tested against baseColor texture alpha
    float transmission = 0.0f;               // Foliage: fraction of diffuse transmitted to the back side; Subsurface:
                                             // strength of the light coming through thin parts (0 = none)
    float ior = 1.5f;                        // Glass / Water
    // Water (v1.92, defect queue 13 (75)): the medium's scattering coefficient (1/m, rgb; 0 = clear: the absorption
    // -ln baseColor alone) and the Henyey-Greenstein asymmetry of its phase function (-1 < g < 1; bath water ~ 0.7-0.9)
    float3 waterScattering{ 0.0f, 0.0f, 0.0f };
    float waterAnisotropy = 0.0f;
    bool twoSided = false;
    // v1.92 (defect queue 13 (76), transitional until V2.5 14.1b lights every emissive face): an emissive surface that is
    // seen as emissive by primary and reflection rays (it shows in mirrors) but adds nothing to the GI update rays'
    // emissive samples and the emissive cache channel, so the analytic light it belongs to lights the scene once.
    bool emissiveVisibleOnly = false;
    uint32_t baseColorTexture = kNone;       // Rgba8Srgb; multiplies baseColor, alpha = coverage
    uint32_t normalTexture = kNone;          // Rg8Normal
    uint32_t roughMetalTexture = kNone;      // Rg8RoughMetal; multiplies roughness/metallic
    uint32_t emissiveTexture = kNone;        // Rgba16Float or Rgba8Srgb; multiplies emissive
    uint32_t occlusionTexture = kNone;       // R8Linear; ambient/specular occlusion (baked cavities only)
    // Hair class (strands, B10; Passes/Hair/HairBsdf.hlsli, INTERFACES 8.1 v1.66): longitudinal roughness beta_M =
    // roughness, eta = ior (keratin 1.55), absorption from melanin (d'Eon 2011) or, both concentrations 0, from baseColor as
    // the target colour (Chiang 2016): scene::model::hairAbsorption.
    float hairEumelanin = 0.0f, hairPheomelanin = 0.0f;  // concentrations (>= 0)
    float hairBetaN = 0.3f;                  // azimuthal roughness (0, 1]
    float hairTilt = 0.0349f;                // cuticle scale tilt (radians; 2 degrees)
    // Cut class (destruction cut faces, A11; Passes/Material/CutFace.hlsli, INTERFACES 8.1 v1.66): the textures are read
    // by an object-space triplanar projection at cutScale repeats per metre; the damage band along the triangle's
    // boundary edges is cutDamageWidth metres wide.
    float cutScale = 1.0f;                   // > 0
    float cutDamageWidth = 0.01f;            // >= 0
    // Subsurface class (skin and similar; MaterialModel.h Subsurface, Passes/Common/MaterialModel.hlsli): the specular is
    // two GGX lobes at the roughnesses saturate(roughness x subsurfaceLobeRoughness[i]), lobe 0 weighing subsurfaceLobeMix
    // and lobe 1 the rest; transmission is the light through thin parts. subsurfaceMeanFreePath (per colour channel) is the
    // medium's mean free path 1 / sigma_t: the screen-space scattering pass spreads the diffuse light with Burley's profile
    // at d = l / s(albedo) (MaterialModel.h "Subsurface class, stage B"; 0 in a channel: no scattering there).
    // The default is skin (measured reduced coefficients, Jensen et al. 2001: 1 / sigma_t' = 1.30 / 0.95 / 0.67 mm): the light
    // spreads a few millimetres, which shows in close-ups; wax, jade or marble are an order of magnitude longer.
    float3 subsurfaceMeanFreePath{ 0.00130f, 0.00095f, 0.00067f };  // m, >= 0
    float subsurfaceLobeMix = 0.85f;                               // [0, 1]
    float2 subsurfaceLobeRoughness{ 0.75f, 1.30f };                // >= 0
    // Eye (a Subsurface-class material with an iris: eyeIrisRadius > 0; MaterialModel.h "Eye"): one sphere-like mesh is
    // sclera, iris and cornea. Its uv (0.5, 0.5) is where the optical axis leaves the eye and eyeAxis that axis in the
    // mesh's object space; the base colour texture is the whole eye's, the iris painted around the uv centre. The specular
    // lobe is one lobe (the cornea and the tear film, at roughness), the sclera scatters with subsurfaceMeanFreePath, the
    // iris is seen through the cornea (refraction onto the iris plane) and lit on its own plane. The class's lobe
    // parameters and transmission are not used (validation: transmission 0).
    float eyeIrisRadius = 0.0f;       // uv units, (0, 0.5]: where the iris ends (the limbus); 0 = not an eye
    float eyeIrisDepth = 0.45f;       // (0, 2]: the cornea's apex above the iris plane, in iris radii (human: about 2.7 mm over 5.9 mm)
    float eyeLimbusWidth = 0.12f;     // [0.01, 1]: the band, in iris radii, over which the iris gives way to the sclera
    float eyeLimbusDarkening = 0.6f;  // [0, 1]: the limbal ring's depth
    float eyePupilScale = 1.0f;       // (0, 8]: the iris texture's radius 1 - (1 - rho) x this (1 = as painted, > 1 a wider pupil)
    float eyeIrisConcavity = 1.0f;    // [0, 1]: the caustic normal's tilt at the limbus (0 = the flat iris plane: no caustic side)
    float eyeIor = 1.336f;            // [1, 2]: the aqueous humour's refractive index
    float3 eyeAxis{ 0, 0, 1 };        // unit: the optical axis in the mesh's object space, out of the eye
    // Terrain class (C5 cooked terrain tiles, FEATURES_GAME 9 direct blending): layer i's weight is channel i % 4 of splat
    // map i / 4 (Rgba8Linear, over the terrain's uv0), normalised by the weights' sum; 1..8 layers (splat 1 needed above 4).
    uint32_t terrainSplat[2] = { kNone, kNone };
    std::vector<TerrainLayer> terrainLayers;
    // Clearcoat layer (A9, MATERIAL_LAYERS 1.1; Standard class): a clear dielectric film over the material,
    // covering the fraction clearcoat of it, with its own perceptual roughness. Its refractive index is one of the
    // tabulated coats (MaterialModel.h kCoatEtas): 1.5 (glaze, varnish, lacquer) or 1.33 (the water film of wet surfaces).
    float clearcoat = 0.0f;                  // [0, 1]; 0 = no layer
    float clearcoatRoughness = 0.05f;        // [0, 1]
    float clearcoatIor = 1.5f;               // 1.5 or 1.33
    // Sheen layer (A9, MATERIAL_LAYERS 1.4; Standard class, not with a clearcoat): cloth's grazing sheen, colour sheenColor
    // (linear, [0, 1]; 0 = none) and perceptual roughness sheenRoughness in [0.1, 1].
    float3 sheenColor{ 0, 0, 0 };
    float sheenRoughness = 0.5f;
    // Cloth blend (with a sheen; MaterialModel.h evaluateSheen): the share of the base's specular lobe the fuzz replaces -
    // 0 = the sheen over the whole base (as before), 1 = no GGX highlight under the fuzz (cotton, velvet). The reference's
    // Cloth shading model (FuzzColor, Cloth) is sheenColor = Cloth x FuzzColor, cloth = Cloth.
    float cloth = 0.0f;                      // [0, 1]
    // Anisotropy (A9, MATERIAL_LAYERS 1.5; Standard class; MaterialModel.h evaluateAnisotropic): the GGX lobe stretched
    // along the cooked tangent rotated by anisotropyRotation (radians, towards the bitangent), alpha_t = alpha + (1 - alpha)
    // anisotropy^2, alpha_b = alpha (KHR_materials_anisotropy). 0 = isotropic. Meshes using it need tangents.
    float anisotropy = 0.0f;                 // [0, 1]
    float anisotropyRotation = 0.0f;         // radians
    // Thin film (A9, MATERIAL_LAYERS 1.2; MaterialModel.h Film; Standard class, not with a clearcoat, sheen or anisotropy):
    // a film of index thinFilmIor and thinFilmThickness nm over a substrate - thinFilmSubstrate 0: n + ik = substrateIor +
    // i substrateExtinction (1.0 + 0i: a free film such as a soap bubble's), 1..5 the spectral metals gold, copper,
    // silver, aluminium, iron - over the fraction thinFilmCoverage of the base's specular Fresnel.
    float thinFilmThickness = 0.0f;          // nm, [0, 5000]; 0 = no film
    float thinFilmIor = 1.33f;               // [1, 3]
    float thinFilmCoverage = 1.0f;           // [0, 1]
    uint32_t thinFilmSubstrate = 0;          // model::FilmSubstrate
    float substrateIor = 1.5f;               // [1, 5] (Constant substrate)
    float substrateExtinction = 0.0f;        // [0, 20]
    // Glass solid bodies (one-sided; A10 R-2): baseColor is the body's transmittance over attenuationDistance metres, so
    // sigma_a = -ln(baseColor) / attenuationDistance (1/m). A pane (two-sided) takes baseColor per pass as before.
    float attenuationDistance = 0.01f;
};

struct Submesh
{
    uint32_t indexOffset = 0;  // into Mesh::indices
    uint32_t indexCount = 0;   // multiple of 3
    uint32_t material = 0;     // index into Scene::materials
};

// Skinning stream (optional): up to 4 influences per vertex, weights sum to 1.
struct SkinStream
{
    std::vector<uint16_t> joints;  // 4 per vertex
    std::vector<float> weights;    // 4 per vertex
    std::vector<float3x4> inverseBind;  // per joint
};

// Blend shape (C4, FEATURES_GAME 15): a sparse set of vertex offsets in the mesh's bind pose. With instance weights w_s,
// a vertex is  p = p0 + sum_s w_s dp_s,  n = normalize(n0 + sum_s w_s dn_s)  (before skinning; evaluateMorph is the
// definition every consumer follows: raster, shadows, rays and the reference).
struct BlendShape
{
    std::string name;
    std::vector<uint32_t> vertices;      // strictly ascending mesh vertex indices the shape moves
    std::vector<float3> deltaPositions;  // one per listed vertex, object space
    std::vector<float3> deltaNormals;    // one per listed vertex, or empty (the shape leaves normals)
};

// Vertex animation (VAT, C4): baked positions (and optionally normals) of every vertex per frame at a fixed rate. At
// time t the vertex is the linear interpolation of frames floor(t fps) and the next (wrapping when looping, held at the
// ends otherwise); it replaces the bind pose. A mesh with a vertex animation has no skin and no blend shapes.
struct VertexAnimation
{
    float framesPerSecond = 0;       // 0 = none
    uint32_t frameCount = 0;
    bool loop = true;
    std::vector<float3> positions;   // frameCount x vertex count, frame-major
    std::vector<float3> normals;     // same layout, or empty (the mesh normals)
};

struct Mesh
{
    std::string name;
    std::vector<float3> positions;
    std::vector<float3> normals;   // unit, per vertex
    std::vector<float4> tangents;  // xyz unit tangent, w = bitangent sign (+1/-1); required when any material of the
                                   // mesh has a normal texture (the generator/importer computes them once, so the
                                   // reference and the renderer use the same tangent frame)
    std::vector<float2> uv0;
    std::vector<uint32_t> indices; // triangle list, counter-clockwise front faces
    std::vector<Submesh> submeshes;
    SkinStream skin;               // empty = rigid
    std::vector<BlendShape> blendShapes;  // C4; empty = none
    VertexAnimation vertexAnimation;      // C4; framesPerSecond 0 = none
};

// Wind (ARCHITECTURE 2.3, 2.7): per-instance response to the scene wind field; the displacement function is shared
// by raster, shadow pages and ray tracing (Passes/Common/Deformation.hlsli).
struct WindParams
{
    float stiffness = 0;   // 0 = not affected
    float phase = 0;       // per-instance phase offset (radians)
    float anchorHeight = 0;  // object-space height below which vertices do not move
};

enum InstanceFlags : uint32_t
{
    InstanceCastShadow = 1u << 0,
    InstanceDynamic = 1u << 1,   // transform may change every tick (dynamic TLAS, VSM caster revision)
    InstanceSkinned = 1u << 2,   // uses Mesh::skin with Scene::skeletons[skeleton]
    InstanceWind = 1u << 3,      // uses WindParams
    // Lighting channels (bits 4..6, Unreal's LightingChannels): the three channels the instance is in, stored as
    // mask ^ 1 - so flags without these bits are channel 0 alone, the default. A local light lights an instance when
    // they share a channel (Light::lightingChannels); the sun lights every instance.
    InstanceLightingChannelsShift = 4,
    InstanceLightingChannelsMask = 7u << 4,
};
// The instance's lighting channels (3 bits) from its flags, and flags with them set.
constexpr uint32_t instanceLightingChannels(uint32_t flags) { return ((flags >> InstanceLightingChannelsShift) & 7u) ^ 1u; }
constexpr uint32_t withLightingChannels(uint32_t flags, uint32_t channels)
{
    return (flags & ~(uint32_t)InstanceLightingChannelsMask) | (((channels & 7u) ^ 1u) << InstanceLightingChannelsShift);
}

struct Instance
{
    uint32_t mesh = 0;
    float3x4 transform;            // object -> world: rotation, uniform scale, translation (no shear/non-uniform scale)
    uint32_t flags = InstanceCastShadow;
    uint32_t skeleton = kNone;     // index into Scene::skeletons when InstanceSkinned
    WindParams wind;
    std::vector<uint32_t> materialOverrides;  // per submesh; empty = mesh materials
    std::vector<float> blendWeights;          // C4: per blend shape of its mesh (initial weights; hosts set them per frame); empty = 0
    float vertexAnimationTime = 0;            // C4: seconds into the mesh's vertex animation (hosts set it per frame)
};

// A skeleton's current pose: joint matrices in model space (applied after inverseBind).
struct Skeleton
{
    std::string name;
    std::vector<float3x4> jointToModel;
};

enum class LightType : uint8_t
{
    Point = 0,
    Spot = 1,
    Rect = 2,    // one-sided rectangle, emits along +forward
    Disk = 3,    // one-sided disk
    Sphere = 4,
    Tube = 5,    // capsule (length along 'right', radius = size.y)
};

struct Light
{
    LightType type = LightType::Point;
    float3 position{};
    float3 forward{ 0, -1, 0 };    // emission axis (spot, rect, disk)
    float3 right{ 1, 0, 0 };       // rect/tube orientation
    float3 color{ 1, 1, 1 };       // linear tint, luminance-normalised to 1
    float intensity = 1000;        // point/spot: candela; area lights: luminance in nits
    float range = 30;              // influence radius (m); contribution is windowed to 0 at range (INTERFACES 8.3)
    float spotInner = 0.3f, spotOuter = 0.5f;  // half-angles (rad)
    float2 size{ 0, 0 };           // rect: width,height; disk/sphere: radius in x; tube: length,radius
    bool castShadow = false;
    // Where the light's shadow rays end, metres before their point on the light (shading.mega_lights; Unreal's per-light
    // Ray End Bias): geometry nearer than this to the light - its own housing, the trough it sits in - casts no shadow of
    // it. Negative: the engine's default (shading.mega_lights_ray_end_bias_m). File block "LEND" (lights with a value).
    float rayEndBias = -1.0f;
    // ---- light components (Unreal's ULocalLightComponent / URectLightComponent; file block "LCMP", lights that set one)
    // Scales of the light's parts: its specular lobes and its diffuse light on surfaces it lights directly, its
    // in-scattering in the air and fog, and its share in the indirect light (the surface cache's direct light and the
    // ray hits' light samples: what GI and reflections carry on).
    float specularScale = 1.0f, diffuseScale = 1.0f, volumetricScattering = 1.0f, indirectIntensity = 1.0f;
    // Rect: the image the emitter shows and emits (a scene texture, Rgba8Srgb or Rgba16Float; kNone: uniform). The light's
    // colour multiplies it.
    uint32_t sourceTexture = kNone;
    // Rect: barn doors - four flaps of this length (m) along the emitter's edges, opened by this angle (radians) from
    // the emitter's normal (0: straight walls, pi / 2: flat, no effect). Length 0: none.
    float barnDoorAngle = 1.5707963f, barnDoorLength = 0.0f;
    // The channels (3 bits) the light is in: it lights the instances that share one (InstanceLightingChannels...).
    uint32_t lightingChannels = 1;
    // The light is not drawn past this distance from the camera (m; 0: always drawn) and fades out over the last
    // fadeRange metres before it (0: cut).
    float maxDrawDistance = 0.0f, maxDistanceFadeRange = 0.0f;
    // Colour temperature (K; 0: not used): the light's colour is 'color' times the black body's chromaticity at this
    // temperature (6,500 K: about white), at the luminance of 'color' (lightColor below). An authoring input: the GPU
    // records hold the product.
    float temperature = 0.0f;
    // Point and spot: 0 = the inverse-square falloff with the range's window (intensity in candela). > 0: the stylised
    // falloff (1 - (d / range)^2)^exponent without the inverse square (Unreal's light falloff exponent with inverse
    // squared falloff off); 'intensity' is then the illuminance (lux) the light gives at its own position.
    float falloffExponent = 0.0f;
};
// The black body's chromaticity at 'kelvin' (1,000..15,000 K) as a linear Rec.709 colour of luminance 1 (Krystek's
// rational approximation of the Planckian locus; components below 0 are held at 0).
float3 colorTemperatureTint(float kelvin);
// The light's colour as the renderer takes it: 'color', times colorTemperatureTint(temperature) when a temperature is
// set, scaled back to the luminance of 'color'.
float3 lightColor(const Light& l);
// Whether the light sets a light component (the "LCMP" block holds it).
bool hasLightComponents(const Light& l);

// Sun and sky: the physical atmosphere of ARCHITECTURE 2.3, parameters from the previous engine (TitanNative
// Atmosphere.h, Hillaire 2020 / Bruneton defaults).
struct Sun
{
    float3 direction{ 0.3419f, 0.9117f, 0.2279f };  // unit, from the ground towards the sun
    float illuminance = 128000;             // lux at the top of the atmosphere
    float angularRadius = 0.004654f;        // radians (0.2667 deg)
    float3 color{ 1, 1, 1 };                // linear tint at the top of the atmosphere
};

struct Atmosphere
{
    float bottomRadius = 6360000;   // m (scene origin sits on the planet's surface, planet centre at (0,-R,0))
    float topRadius = 6460000;
    float rayleighScaleHeight = 8000;
    float mieScaleHeight = 1200;
    float mieG = 0.8f;
    float3 rayleighScattering{ 5.802e-6f, 13.558e-6f, 33.100e-6f };  // 1/m
    float3 mieScattering{ 3.996e-6f, 3.996e-6f, 3.996e-6f };
    float3 mieAbsorption{ 0.444e-6f, 0.444e-6f, 0.444e-6f };
    float3 ozoneAbsorption{ 0.650e-6f, 1.881e-6f, 0.085e-6f };
    float ozoneCenter = 25000, ozoneWidth = 15000;  // tent profile
    float3 groundAlbedo{ 0.1f, 0.1f, 0.1f };
};

// The scene's weather: a cloud layer, the height fog and local fog volumes, in the terms the renderer's frame takes them
// (unx/render/FrameContext.h CloudLayerDesc, FogDesc, FogVolumeDesc: the same fields and units; the kernels are
// Passes/Atmosphere/Cloud*.hlsl, Fog*.hlsl). A frame whose producer sets none of its own takes the scene's
// (FrameRenderer, FrameContext::sceneWeather). File blocks "CLDS" and "FOGS", written only when the scene has them: a
// scene without weather has the bytes and the content hash it had. The reference path tracer does not render them.
struct CloudLayer
{
    float coverage = 0;                             // [0, 1]: the share of the weather map that becomes cloud; 0: no layer
    float baseAltitude = 1500, topAltitude = 4000;  // m above the planet's surface (the scene's origin lies on it)
    float sigmaMax = 0.04f;                         // peak extinction (1/m)
    float albedo = 0.99f;                           // single-scattering albedo
    float windX = 0, windZ = 0;                     // m/s: the layer's drift
};
struct Fog
{
    bool enabled = false;
    float density = 0.002f;        // extinction (1/m) at 'height'
    float heightFalloff = 0.02f;   // the density halves every 1 / this metres of height
    float height = 0;              // m (scene y)
    float3 albedo{ 1, 1, 1 };      // scattering / extinction
    float phaseG = 0.2f;           // Henyey-Greenstein asymmetry, (-1, 1)
    float startDistance = 0;       // m from the camera: no fog nearer
    float skyAmount = 1;           // [0, 1]: how much of the fog sky pixels take
    float noiseAmount = 0.3f;      // [0, 1]: the density's variation about its mean
    float noiseScale = 20;         // m: the variation's largest features
};
// Extra fog inside an ellipsoid or a box (mist in a hollow, steam): seen within the fog's near volume.
struct FogVolume
{
    float3 centre{};               // m
    float3 halfSize{ 1, 1, 1 };    // m: the ellipsoid's radii or the box's half extents along its axes
    float yaw = 0;                 // rad about +y
    uint32_t shape = 0;            // 0 ellipsoid, 1 box
    float density = 0.05f;         // extinction (1/m) at the volume's bottom, away from its boundary
    float heightFalloff = 0;       // the density halves this many times from the volume's bottom to its top
    float edge = 0.3f;             // (0, 1]: the outer share of the volume over which the density fades to 0
    float3 albedo{ 1, 1, 1 };
    // rising steam (unx/render/FrameContext.h FogVolumeDesc: the same fields; file block "FVST", written only for
    // volumes that set any of them)
    float sourcePlane = 0;         // [0, 0.95]: the height inside the volume the medium rises from
    float riseSpeed = 0;           // m/s
    float turbulence = 0;          // [0, 1]
    float turbulenceScale = 0.5f;  // m
};

struct Camera
{
    std::string name;
    float3 position{};
    float3 forward{ 0, 0, -1 };
    float3 up{ 0, 1, 0 };
    float verticalFov = 1.0471976f;  // 60 degrees
    float nearPlane = 0.05f;
    float ev100 = 14.0f;             // exposure: radiance scale = 1 / (1.2 * 2^ev100)
};

// Camera path for gates (120 s RPP paths, P7) and temporal-stability tests: linear position, slerped orientation.
struct CameraKey
{
    float time = 0;
    float3 position{};
    float3 forward{ 0, 0, -1 };
    float3 up{ 0, 1, 0 };
};
struct CameraPath
{
    std::string name;
    std::vector<CameraKey> keys;
};

struct Scene
{
    std::string name;
    uint64_t seed = 0;
    std::vector<Texture> textures;
    std::vector<Material> materials;
    std::vector<Mesh> meshes;
    std::vector<Instance> instances;
    std::vector<Skeleton> skeletons;
    std::vector<Light> lights;
    Sun sun;
    Atmosphere atmosphere;
    float3 windDirection{ 1, 0, 0 };  // unit, world
    float windSpeed = 0;               // m/s
    std::vector<Camera> cameras;
    std::vector<CameraPath> paths;
    CloudLayer clouds;                 // coverage 0: none
    Fog fog;                           // enabled false: none
    std::vector<FogVolume> fogVolumes;
};

// .unxscene binary file (little endian): "UNXSCENE", u32 version, then the fields in declaration order with u64
// element counts before every array. Deterministic: the same Scene always yields the same bytes.
void save(const Scene& scene, const std::filesystem::path& path);
Scene load(const std::filesystem::path& path);
std::vector<uint8_t> serialize(const Scene& scene);
Scene deserialize(const std::vector<uint8_t>& bytes);
// SHA-256 of serialize(scene): the scene identity recorded next to reference images and gate reports.
std::string contentHash(const Scene& scene);
// Structural checks (index ranges, unit vectors, sizes); throws unx::Error with the first problem found.
void validate(const Scene& scene);

// C4 reference evaluation of vertex v of 'mesh' before skinning: blend shapes with 'weights' (per shape; missing = 0)
// and the vertex animation at 'time'. Returns the object-space position and unit normal.
void evaluateMorph(const Mesh& mesh, const std::vector<float>& weights, float time, uint32_t v, float3& position, float3& normal);
// Largest displacement from the bind pose any vertex can reach: blend shapes with |w_s| <= the given bounds (per shape),
// or the vertex animation over all frames (the culling inflation of an instance).
float morphBound(const Mesh& mesh, const std::vector<float>& weightBounds);
} // namespace unx::scene
