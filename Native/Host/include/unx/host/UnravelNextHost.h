#pragma once
// UnravelNext.dll C ABI (I track). Unity loads the DLL as a native plugin; C# calls these functions through P/Invoke
// (Assets/UnravelNextBridge in the old repository). Rules:
//   - Only fixed-size structs with a leading {size, version} pair, plain integers/floats, and opaque 64-bit handles cross
//     the boundary. No C++ objects, no STL, no exceptions: every function returns UNX_OK or an error code, and
//     UnxLastError() returns the message of the calling thread's last failure.
//   - A struct whose size or version does not match is refused (UNX_ERROR_ABI), so a stale managed side fails loudly.
//   - Render work happens on Unity's submission thread through the plugin event returned by UnxRenderEventFunc().
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define UNX_API __declspec(dllexport)
#define UNX_CALL __cdecl

enum UnxResult
{
    UNX_OK = 1,
    UNX_ERROR = 0,
    UNX_ERROR_ABI = -1,        // size/version mismatch between managed and native structs
    UNX_ERROR_NO_DEVICE = -2,  // Unity's D3D12 device is not available (other graphics API, device lost)
    UNX_ERROR_BUFFER = -3,     // output buffer too small; *required holds the size
    UNX_DEVICE_REMOVED = -4,   // the D3D12 device was removed (TDR, driver reset): every renderer is unusable; destroy
                               // them (UnxRendererDestroy still works) and create new ones once the host has a device
};

#define UNX_ABI_VERSION 6u  // 1: probe; 2: + renderer; 3: + UnxFrameSetSkeletons, UnxSceneSave writes the current state;
                            // 4: + UnxFrameSetEnvironment; 5: UNX_DEVICE_REMOVED (the process survives a device removal);
                            // 6: + UnxFrameSetDiscontinuity, UnxFrameSetSimulation, UnxTransformUpdate::flags (teleport);
                            //    later additions within 6 (optional exports, bridges probe for them): UnxFrameGraphStatsLatest,
                            //    UnxSceneLoad, UnxVideoMemory, UnxSceneEditInstances, UnxSceneEditMaterials, UnxVfxStreamExecutor,
                            //    UnxRendererQualityOverride, UnxFrameSetLens, UnxSurfaceDelta, UnxSurfaceSetHalfLives, UnxSurfaceSetTime,
                            //    UnxDebugPrimitives, UnxDebugText, UnxDecalAdd, UnxDecalUpdate, UnxDecalRemove, UnxViewModelAdd,
                            //    UnxViewModelSetPose, UnxViewModelRemove, UnxPhotoBegin, UnxPhotoSave, UnxPhotoEnd, UnxPhotoGetStatus,
                            //    UnxAcquireGpuBridge, UnxReleaseGpuBridge, UnxGpuBridgeStatistics, UnxHairAddBody, UnxHairTick,
                            //    UnxHairSetFrameFraction, UnxHairRemoveBody, UnxFrameSetFluids,
                            //    UnxSceneAddBlendShape, UnxSceneSetVertexAnimation, UnxFrameSetMorphs (C4),
                            //    UnxFrameSetOriginShift (C9), UnxSceneReserveRuntime, UnxFrameAddRuntimeMesh,
                            //    UnxFrameRemoveRuntimeMesh, UnxFrameAddRuntimeInstance, UnxFrameRemoveRuntimeInstance,
                            //    UnxFrameSetRuntimeTransforms (C2b), UnxFrameSetTerrainDeformation (C5), UnxFrameSetOcean (B7),
                            //    UnxSceneSetTerrainLayers (C5 terrain material, v1.74), UnxFrameSetClouds (B5, v1.77),
                            //    UnxFrameSetPools, UnxFrameAddPoolSources (W2, v1.78), UnxPoolStatsLatest (W2, v1.90),
                            //    UnxFrameSetWhiteBalance (v1.91), UnxFrameSetFog, UnxFrameSetFogVolumes (the height fog, local fog volumes),
                            //    UnxSceneSetCharacterShading (skin, eye and cloth parameters of a material),
                            //    UnxMaterialInputsDefaults, UnxSceneSetMaterialInputs (uv transform, second uv set, detail
                            //    maps, height, emissive scale and mask, vertex colour, dithered opacity),
                            //    UnxSceneSetMeshAttributes (a mesh's second uv set and vertex colours),
                            //    UnxLightComponentsDefaults, UnxSceneSetLightComponents (a light's scales, source texture,
                            //    barn doors, lighting channels, draw distance, colour temperature, falloff exponent),
                            //    UnxSceneSetInstanceLightingChannels,
                            //    UnxFrameSetColorGrading, UnxFrameSetPost, UnxFrameSetDisplayEncoding (the picture's settings a game
                            //    changes while it runs), UnxFrameSetFog2, UnxFrameSetFogVolumes2, UnxFrameSetClouds2,
                            //    UnxFrameSetWeather, UnxFrameSetLightning, UnxFrameSetPoolWeather (the weather)
UNX_API uint32_t UNX_CALL UnxAbiVersion(void);
// Message of the calling thread's last failure (UTF-8, empty when none). Valid until the next failing call.
UNX_API const char* UNX_CALL UnxLastError(void);
// Unity plugin event callback and the first of its reserved event ids (UnxEvent + base).
UNX_API void* UNX_CALL UnxRenderEventFunc(void);
UNX_API int32_t UNX_CALL UnxEventBase(void);

enum UnxEvent
{
    UNX_EVENT_PROBE_BEFORE = 0,  // probe: timestamp on Unity's queue before the frame
    UNX_EVENT_PROBE_WORK = 1,    // probe: the frame workload through the selected boundary
    UNX_EVENT_PROBE_AFTER = 2,   // probe: timestamp on Unity's queue after Unity consumed the output
    UNX_EVENT_PROBE_FACTS = 3,   // probe: collect device/queue facts on the submission thread
    UNX_EVENT_RENDER = 4,        // renderer: record and execute the frame queued with UnxFrameQueue (data = ticket)
    UNX_EVENT_COUNT = 5,
};

// ---- Host boundary probe (ARCHITECTURE 7.1-5) ----------------------------------------------------------------------

enum UnxProbeMode
{
    UNX_PROBE_UNITY_QUEUE = 1,  // frame list executed on Unity's queue (IUnityGraphicsD3D12v8::ExecuteCommandList)
    UNX_PROBE_OWN_QUEUE = 2,    // frame on the renderer's own DIRECT queue of Unity's device, fences to Unity's queue
};

typedef struct UnxProbeSettings
{
    uint32_t size;       // sizeof(UnxProbeSettings)
    uint32_t version;    // 1
    uint32_t mode;       // UnxProbeMode
    uint32_t width, height;
    uint32_t frames;     // measured frames after warm-up
    float warmupSeconds;
    uint32_t raiseUnityQueuePriority;  // 1: ID3D12CommandQueue1::SetProcessPriority(HIGH) on Unity's queue for the run
    uint64_t outputs[2];  // ID3D12Resource* of two Unity RenderTextures (R10G10B10A2, random write), used alternately
    char shaderDirectory[512];  // UTF-8, folder holding Probe/*.dxil
} UnxProbeSettings;
#ifdef __cplusplus
static_assert(sizeof(UnxProbeSettings) == 560, "UnxProbeSettings is part of the ABI (Assets/UnravelNextBridge/Runtime/Native/UnravelNextNative.cs)");
#endif

// Facts about Unity's device and queue (JSON). Valid after one UNX_EVENT_PROBE_FACTS event has run.
UNX_API int32_t UNX_CALL UnxProbeFacts(char* json, uint32_t capacity, uint32_t* required);
UNX_API int32_t UNX_CALL UnxProbeStart(const UnxProbeSettings* settings);
// Frames measured so far in the running probe; *finished = 1 when the requested count is reached.
UNX_API int32_t UNX_CALL UnxProbeProgress(uint32_t* measured, uint32_t* finished);
// Result of the finished probe (JSON), then the probe releases its resources (after the GPU is done with them).
UNX_API int32_t UNX_CALL UnxProbeResults(char* json, uint32_t capacity, uint32_t* required);
UNX_API int32_t UNX_CALL UnxProbeStop(void);

// ---- Renderer --------------------------------------------------------------------------------------------------------
// One renderer per scene. Load time: create, add scene content, commit (validates, builds the cluster hierarchy, uploads
// the GPU scene). Per frame: queue a frame (camera, time, output texture) on the main thread and issue
// UNX_EVENT_RENDER with the returned ticket in the same frame's command buffer; the frame is recorded and executed on
// Unity's submission thread. Units and conventions are the renderer's (INTERFACES_KO.md 6.1): right-handed, +Y up,
// metres, matrices row-major 3x4 acting on column vectors (object -> world). The bridge converts from Unity's
// left-handed space by mirroring Z.

typedef uint64_t UnxRenderer;  // opaque, 0 = none
#define UNX_NONE 0xFFFFFFFFu

enum UnxRendererFlags
{
    UNX_RENDERER_STANDALONE = 1u << 0,  // own device and queue (tests, tools); otherwise Unity's device (plugin loaded by Unity)
};

typedef struct UnxRendererDesc
{
    uint32_t size, version;     // sizeof, 1
    uint32_t flags;             // UnxRendererFlags
    uint32_t framesInFlight;    // Unity QualitySettings.maxQueuedFrames (>= 1)
    char shaderDirectory[512];  // UTF-8: folder of the renderer's compiled kernels (build/<track>/bin/shaders)
    char qualityDirectory[512]; // UTF-8: folder of the quality files (Config/quality)
} UnxRendererDesc;

UNX_API int32_t UNX_CALL UnxRendererCreate(const UnxRendererDesc* desc, UnxRenderer* renderer);
UNX_API int32_t UNX_CALL UnxRendererDestroy(UnxRenderer renderer);

// Scene content (before UnxSceneCommit). Indices returned through *index are dense and in call order per kind.
enum UnxTextureFormat  // scene::TextureFormat
{
    UNX_TEXTURE_RGBA8_SRGB = 0,
    UNX_TEXTURE_RGBA8_LINEAR = 1,
    UNX_TEXTURE_RG8_NORMAL = 2,
    UNX_TEXTURE_RG8_ROUGH_METAL = 3,
    UNX_TEXTURE_R8_LINEAR = 4,
    UNX_TEXTURE_RGBA16_FLOAT = 5,
};

typedef struct UnxTextureDesc
{
    uint32_t size, version;     // sizeof, 1
    uint32_t width, height;
    uint32_t format;            // UnxTextureFormat
    uint32_t wrap;              // 1 repeat, 0 clamp
    const void* texels;         // mip 0, rows top to bottom, tightly packed
    uint64_t byteCount;         // width * height * bytes per texel
    char name[64];
} UnxTextureDesc;

enum UnxMaterialClass  // scene::MaterialClass
{
    UNX_MATERIAL_STANDARD = 0,
    UNX_MATERIAL_FOLIAGE = 1,
    UNX_MATERIAL_HAIR = 2,
    UNX_MATERIAL_WATER = 3,
    UNX_MATERIAL_GLASS = 4,
    UNX_MATERIAL_SUBSURFACE = 5,
    UNX_MATERIAL_CUT = 6,       // destruction cut faces (A11; default cutScale 1, damage width 0.01 m)
    UNX_MATERIAL_TERRAIN = 7,   // terrain layer blending (v1.74): its layers come from UnxSceneSetTerrainLayers
};

// INTERFACES_KO.md 8.1 material v1.
typedef struct UnxMaterialDesc
{
    uint32_t size, version;     // sizeof, 7 (6: up to waterAnisotropy, 232 B; 5: up to substrateExtinction; 4: up to anisotropyRotation; 3 and 2: up to attenuationDistance, 2 without
                                // its sheen and attenuation fields; 1: up to name)
    uint32_t materialClass;     // UnxMaterialClass
    uint32_t twoSided;
    float baseColor[3];         // linear albedo (dielectric) or f0 (metal)
    float roughness;            // perceptual
    float metallic, specular;   // dielectric f0 = 0.08 * specular
    float alphaCutoff;          // 0 = opaque
    float transmission;         // foliage diffuse transmission; Subsurface: the strength of the light through thin parts
    float emissive[3];          // nits
    float ior;
    uint32_t baseColorTexture, normalTexture, roughMetalTexture, emissiveTexture, occlusionTexture;  // UNX_NONE = none
    uint32_t reserved;
    char name[64];
    // version 2 (A9 clearcoat, INTERFACES v1.76; Standard class): cover [0, 1] (0 = none), perceptual roughness, index
    // (a tabulated coat: 1.5 glaze / varnish, 1.33 the water film of wet surfaces).
    float clearcoat, clearcoatRoughness, clearcoatIor;
    // version 3 (A9 sheen, MATERIAL_LAYERS 1.4; Standard class, not with a clearcoat): colour (linear, [0, 1]; 0 = none) and
    // perceptual roughness [0.1, 1]; (A10) a Glass solid body's attenuation distance in m (baseColor = its transmittance
    // over it; 0 = the default 0.01 m).
    float sheenColor[3], sheenRoughness;
    float attenuationDistance;
    // version 4 (A9 anisotropy, MATERIAL_LAYERS 1.5; Standard class; the meshes using it need tangents): strength [0, 1]
    // (0 = isotropic; alpha_t = alpha + (1 - alpha) s^2, alpha_b = alpha) and the direction's rotation from the tangent
    // towards the bitangent (radians).
    float anisotropy, anisotropyRotation;
    // version 5 (A9 thin film, MATERIAL_LAYERS 1.2; Standard class, not with a clearcoat, sheen or anisotropy): thickness in
    // nm (0 = none, up to 5000), film index [1, 3], cover [0, 1], substrate (0: n + ik = substrateIor + i substrateExtinction;
    // 1..5 the spectral metals gold, copper, silver, aluminium, iron).
    float thinFilmThickness, thinFilmIor, thinFilmCoverage;
    uint32_t thinFilmSubstrate;
    float substrateIor, substrateExtinction;
    // version 6 (v1.92, defect queue 13 (75); Water class): the water's scattering coefficient (1/m, linear rgb; 0 = clear)
    // and the Henyey-Greenstein asymmetry of its phase function (-1 < g < 1)
    float waterScattering[3], waterAnisotropy;
    // version 7 (defect queue 13 (76)): 1 = the emissive surface is seen by primary and reflection rays only - the GI
    // update rays and the emissive cache take 0 from it (its lamp's analytic light lights the scene once); 0 = as before
    uint32_t emissiveVisibleOnly;
} UnxMaterialDesc;

typedef struct UnxSubmesh
{
    uint32_t indexOffset, indexCount, material, reserved;
} UnxSubmesh;

typedef struct UnxMeshDesc
{
    uint32_t size, version;     // sizeof, 1
    uint32_t vertexCount, indexCount, submeshCount, jointCount;
    const float* positions;     // 3 per vertex
    const float* normals;       // 3 per vertex, unit
    const float* tangents;      // 4 per vertex (xyz unit, w = bitangent sign) or null; required with a normal texture
    const float* uv0;           // 2 per vertex or null
    const uint32_t* indices;    // triangle list, counter-clockwise front faces (renderer space)
    const UnxSubmesh* submeshes;
    const uint16_t* joints;     // 4 per vertex when jointCount > 0
    const float* weights;       // 4 per vertex, sum 1
    const float* inverseBind;   // 12 per joint (row-major 3x4)
    char name[64];
} UnxMeshDesc;

enum UnxInstanceFlags  // scene::InstanceFlags
{
    UNX_INSTANCE_CAST_SHADOW = 1u << 0,
    UNX_INSTANCE_DYNAMIC = 1u << 1,
    UNX_INSTANCE_SKINNED = 1u << 2,
    UNX_INSTANCE_WIND = 1u << 3,
    // bits 4..6: the instance's lighting channels (UNX_INSTANCE_LIGHTING_CHANNELS below); 0 = channel 0 alone
};
// The lighting channels of an instance as its flags' bits: channels = a mask of the three channels (bit 0, 1, 2) the
// instance is in - a light lights the instances that share a channel with it (UnxLightComponentsDesc::lightingChannels).
// Or it into UnxInstanceDesc::flags (UnxSceneAddInstance, UnxSceneEditInstances); a description without it is in
// channel 0, as every light is by default.
#define UNX_INSTANCE_LIGHTING_CHANNELS(channels) (((((uint32_t)(channels)) & 7u) ^ 1u) << 4)

typedef struct UnxInstanceDesc
{
    uint32_t size, version;     // sizeof, 1
    uint32_t mesh, flags;       // UnxInstanceFlags
    uint32_t skeleton;          // UNX_NONE unless skinned
    uint32_t materialOverrideCount;  // 0 or the mesh's submesh count
    const uint32_t* materialOverrides;
    float transform[12];        // object -> world: rotation, uniform scale, translation
    float windStiffness, windPhase, windAnchorHeight, reserved;
} UnxInstanceDesc;

enum UnxLightType  // scene::LightType
{
    UNX_LIGHT_POINT = 0,
    UNX_LIGHT_SPOT = 1,
    UNX_LIGHT_RECT = 2,
    UNX_LIGHT_DISK = 3,
    UNX_LIGHT_SPHERE = 4,
    UNX_LIGHT_TUBE = 5,
};

typedef struct UnxLightDesc
{
    uint32_t size, version;     // sizeof, 2 (1: without rayEndBias - the same size, that field was reserved and is not read)
    uint32_t type, castShadow;
    float position[3], intensity;   // candela (point/spot) or luminance nits (area)
    float forward[3], range;
    float right[3], spotInner;
    float color[3], spotOuter;
    float areaSize[2];
    float rayEndBias;           // version 2 (v1.93): where the light's shadow rays end, metres before the light (its housing
                                // casts no shadow of it); negative: the engine's default (shading.mega_lights_ray_end_bias_m)
    float reserved;
} UnxLightDesc;

// Sun, sky and wind (INTERFACES_KO.md 8.3). atmosphere* fields are the physical model of scene::Atmosphere.
typedef struct UnxEnvironmentDesc
{
    uint32_t size, version;     // sizeof, 1
    float sunDirection[3], sunIlluminance;  // unit, ground -> sun; lux at the top of the atmosphere
    float sunColor[3], sunAngularRadius;
    float windDirection[3], windSpeed;
    float bottomRadius, topRadius, rayleighScaleHeight, mieScaleHeight;
    float rayleighScattering[3], mieG;
    float mieScattering[3], ozoneCenter;
    float mieAbsorption[3], ozoneWidth;
    float ozoneAbsorption[3], reserved0;
    float groundAlbedo[3], reserved1;
} UnxEnvironmentDesc;

UNX_API int32_t UNX_CALL UnxSceneAddTexture(UnxRenderer r, const UnxTextureDesc* desc, uint32_t* index);
UNX_API int32_t UNX_CALL UnxSceneAddMaterial(UnxRenderer r, const UnxMaterialDesc* desc, uint32_t* index);
UNX_API int32_t UNX_CALL UnxSceneAddMesh(UnxRenderer r, const UnxMeshDesc* desc, uint32_t* index);
// C4 (before UnxSceneCommit): a blend shape of a mesh: vertexCount strictly ascending mesh vertices, their position
// offsets (3 floats each, renderer space) and normal offsets (3 floats each, or null). Shapes keep the order of the calls.
UNX_API int32_t UNX_CALL UnxSceneAddBlendShape(UnxRenderer r, uint32_t mesh, const char* name, uint32_t vertexCount, const uint32_t* vertices,
                                               const float* deltaPositions, const float* deltaNormals);
// C4 (before UnxSceneCommit): the mesh's vertex animation: frameCount x vertex count positions (3 floats each, frame-major)
// and normals (same layout, or null) at framesPerSecond, looping or held at the ends. The mesh then has no skin and no
// blend shapes.
UNX_API int32_t UNX_CALL UnxSceneSetVertexAnimation(UnxRenderer r, uint32_t mesh, float framesPerSecond, uint32_t frameCount, uint32_t loop,
                                                    const float* positions, const float* normals);
// jointToModel: 12 floats per joint (row-major 3x4), the skeleton's current pose in model space.
UNX_API int32_t UNX_CALL UnxSceneAddSkeleton(UnxRenderer r, const float* jointToModel, uint32_t jointCount, uint32_t* index);
UNX_API int32_t UNX_CALL UnxSceneAddInstance(UnxRenderer r, const UnxInstanceDesc* desc, uint32_t* index);
// Scene edits after commit (optional exports within ABI 6; INTERFACES 6.3 v1.44, render A item A2): the instance at each
// index takes its description (index == the current instance count appends, in order; the new instance is visible and has
// no motion). Meshes and textures are fixed at commit, and a skinned instance cannot be added or replaced (a new renderer).
// Materials likewise (index == the material count appends). Edits reach the GPU scene with the next queued frame; remove an
// instance with UnxFrameSetInstanceVisible(0) and reuse its slot in a later edit.
UNX_API int32_t UNX_CALL UnxSceneEditInstances(UnxRenderer r, const uint32_t* indices, const UnxInstanceDesc* descs, uint32_t count);
UNX_API int32_t UNX_CALL UnxSceneEditMaterials(UnxRenderer r, const uint32_t* indices, const UnxMaterialDesc* descs, uint32_t count);
// V3 (optional export within ABI 6; WORLD_VFX 3.7): fills 'executor' (NV_StreamExecutor of NativeVfxStream.h, 48 B) with
// the renderer's FX particle module as a VFX stream executor, for nv_stream_attach. Committed renderers only; the
// executor's user pointer is the renderer, so detach the stream (nv_stream_detach) before UnxRendererDestroy. Its
// callbacks run on the caller's thread (the host's main thread): a readback of a tick no frame has recorded yet runs
// that tick at once on the renderer's compute queue, so the caller never waits for the render thread.
UNX_API int32_t UNX_CALL UnxVfxStreamExecutor(UnxRenderer r, void* executor);
// Quality override before commit (optional export within ABI 6): "section.key=value" in TOML syntax, e.g. a game's
// "shading.post_bloom_strength=0.04". Refused after commit.
UNX_API int32_t UNX_CALL UnxRendererQualityOverride(UnxRenderer r, const char* utf8Assignment);
// The camera's lens for the frames queued from now on (optional export within ABI 6; INTERFACES v1.51): aperture diameter
// in metres (0 = pinhole: no depth of field) and focus distance in metres along the view axis. Unity: focal length /
// f-number of a physical camera, and its focus distance.
UNX_API int32_t UNX_CALL UnxFrameSetLens(UnxRenderer r, float apertureMetres, float focusMetres);
// The camera's white balance for the frames queued from now on (optional export within ABI 6; INTERFACES v1.91, defect
// queue 6 / game request 83): the illuminant the camera is set to as a correlated colour temperature in kelvin (0 = D65:
// no adaptation; else 1000..40000; the CIE daylight locus from 4000 K, the Planckian locus below) and a tint as Duv in
// the CIE 1960 uv diagram (+ towards green, - towards magenta; |tint| <= 0.1). The post chain adapts the exposed image
// from that white to the display's D65 (Bradford) when the quality key shading.post_white_balance is on (the game sets
// it as an override before commit); off, or at D65, the output is bit-identical to before.
UNX_API int32_t UNX_CALL UnxFrameSetWhiteBalance(UnxRenderer r, float kelvin, float tint);
// Surface state field (A7, E's SurfaceField; optional exports within ABI 6, INTERFACES v1.52): after each VFX commit the
// host passes NativeVfx nv_surface_delta(previous publication, publication): `changed` NV_SurfaceBrickV2 records (1560 B:
// int32 key[3], uint32, double t0, float value[384]) and `removedKeys` (3 int32 per key). Half-lives: 6 doubles (s, 0 = no
// decay; wet, scorch, frost, dust, blood, snow; nv_surface_state_v2). Time: the VFX context seconds of the frames queued
// from now on (interpolated like the frame's transforms). All three take effect with the next queued frame, in call order.
UNX_API int32_t UNX_CALL UnxSurfaceDelta(UnxRenderer r, const void* changed, uint64_t changedCount, const int32_t* removedKeys, uint64_t removedCount);
UNX_API int32_t UNX_CALL UnxSurfaceSetHalfLives(UnxRenderer r, const double* halfLives6);
UNX_API int32_t UNX_CALL UnxSurfaceSetTime(UnxRenderer r, double seconds);
// Debug drawing (A15, E's Passes/Debug; optional exports within ABI 6, INTERFACES v1.53) for the next queued frame only
// (immediate mode: a frame that is never rendered drops its primitives). Records as DebugDraw.h lays them out:
//   line 32 B     { float a[3]; uint32 rgba; float b[3]; uint32 widthFlags }   (width px in 8.8 fixed point | flags << 16;
//                                                                              a point is a line with a == b)
//   triangle 48 B { float a[3]; uint32 rgba; float b[3]; uint32 flags; float c[3]; uint32 0 }
// rgba: R in the low byte, sRGB, a = opacity. Flags: 1 depth test, 2 screen (positions are pixels), 4 x-ray, 8 text shadow.
// Text: printable ASCII at a world anchor (or pixel with flag 2), cells sizePx high, offset in px.
UNX_API int32_t UNX_CALL UnxDebugPrimitives(UnxRenderer r, const void* lines, uint32_t lineCount, const void* triangles, uint32_t triangleCount);
UNX_API int32_t UNX_CALL UnxDebugText(UnxRenderer r, const float* anchor3, const char* utf8, uint32_t rgba, float sizePx, uint32_t flags, float offsetX,
                                      float offsetY);
// Projected decals (A7, E's decal::DecalSet; optional exports within ABI 6, INTERFACES v1.53): an oriented box (the unit
// cube [-1, 1]^3 -> space, rows of a 3 x 4 matrix: columns = half-extent axes X, Y, Z, then the centre; world space, or the
// object space of 'instance'), a scene material painted on surfaces facing the box's +Z (angle fade from fadeStart to
// fadeEnd degrees), priority then creation order, opacity, soft edge fraction of the box depth. Ids are returned by
// UnxDecalAdd; changes reach the frames queued after the call.
typedef struct UnxDecalDesc
{
    float box[12];
    uint32_t material;
    uint32_t instance;  // 0xFFFFFFFF: world space
    int32_t priority;
    float opacity, fadeStartDegrees, fadeEndDegrees, edge;
    uint32_t reserved;  // 0
} UnxDecalDesc;
#ifdef __cplusplus
static_assert(sizeof(UnxDecalDesc) == 80, "UnxDecalDesc is part of the ABI (Assets/UnravelNextBridge/Runtime/Native/UnravelNextRendererNative.cs)");
#endif
UNX_API int32_t UNX_CALL UnxDecalAdd(UnxRenderer r, const UnxDecalDesc* desc, uint32_t* id);
UNX_API int32_t UNX_CALL UnxDecalUpdate(UnxRenderer r, uint32_t id, const UnxDecalDesc* desc);
UNX_API int32_t UNX_CALL UnxDecalRemove(UnxRenderer r, uint32_t id);
// First-person view models (A12, E's Passes/ViewModel; optional exports within ABI 6, INTERFACES v1.54): a scene instance
// posed in the camera's frame - cameraLocal12 is its object -> view space transform (rows of a 3 x 4 matrix; view space:
// x right, y up, looking down -z; the renderer's axes, i.e. Unity camera space with z negated) - composed with the camera
// of every rendered frame (so it follows the camera at the display rate; the host sends no world transform for it, and
// one it sends is overridden). Pose it every frame from the animation's view-model rig. Changes reach the frames queued
// after the call.
UNX_API int32_t UNX_CALL UnxViewModelAdd(UnxRenderer r, uint32_t instance, const float* cameraLocal12, uint32_t* id);
UNX_API int32_t UNX_CALL UnxViewModelSetPose(UnxRenderer r, uint32_t id, const float* cameraLocal12);
UNX_API int32_t UNX_CALL UnxViewModelRemove(UnxRenderer r, uint32_t id);
UNX_API int32_t UNX_CALL UnxSceneAddLight(UnxRenderer r, const UnxLightDesc* desc, uint32_t* index);
UNX_API int32_t UNX_CALL UnxSceneSetEnvironment(UnxRenderer r, const UnxEnvironmentDesc* desc);
// The renderer's defaults for the environment (scene::Sun, scene::Atmosphere), for callers that set only some fields.
UNX_API int32_t UNX_CALL UnxEnvironmentDefaults(UnxEnvironmentDesc* desc);

typedef struct UnxSceneInfo
{
    uint32_t size, version;     // sizeof, 1
    uint32_t textures, materials, meshes, instances, lights, skeletons;
    uint64_t triangles;         // source triangles over all meshes
    uint64_t clusters;          // cluster hierarchy size (V's builder)
    double buildMs;             // validate + cluster build + GPU upload
    char contentHash[65];       // scene::contentHash (SHA-256 hex)
    char reserved[7];
} UnxSceneInfo;

// Validates the scene, builds the cluster hierarchy and uploads it. Blocking; call outside rendering.
UNX_API int32_t UNX_CALL UnxSceneCommit(UnxRenderer r, UnxSceneInfo* info);
// scene::contentHash of the content added so far (SHA-256 hex, 65 bytes with the terminator): the scene's identity,
// equal to the hash of the same scene built natively (exporter checks, reference-image cache keys).
UNX_API int32_t UNX_CALL UnxSceneContentHash(UnxRenderer r, char hash[65]);

typedef struct UnxCameraDesc
{
    float position[3], verticalFov;  // radians
    float forward[3], nearPlane;
    float up[3], ev100;
} UnxCameraDesc;

// Saves the scene as a .unxscene file (INTERFACES 6.2), with 'camera' (nullable) as its camera 0 and 'name' as the scene
// name: host scenes become test scenes for every track and for standalone gates. Before UnxSceneCommit: the content
// added so far. After: the scene as the host shows it now (the latest transforms, poses, sun and visibility it set,
// including the ones not yet rendered), hidden instances left out.
UNX_API int32_t UNX_CALL UnxSceneSave(UnxRenderer r, const char* utf8Path, const char* utf8Name, const UnxCameraDesc* camera);
// B11 photo mode (FEATURES_GAME 17; optional exports within ABI 6, INTERFACES v1.65): E's GPU reference path tracer on
// the renderer's device renders a snapshot of the scene as the host shows it now - the committed content with the latest
// transforms, poses, sun and visibility, the runtime geometry and terrain patches, and the view models at the latest
// frame camera - from 'camera' with the host's lens (UnxFrameSetLens), progressively: every frame rendered after
// UnxPhotoBegin adds halfSamplesPerFrame samples per pixel and half until samplesPerPixel (both halves) and shows the
// image through the post chain (the frame's camera is not used; its output size is the photo's, a new size restarts).
// Pause the game first; UnxPhotoBegin again restarts (a moved photo camera), UnxPhotoEnd returns to the scene (its
// first frame is a history cut). A camera EV100 that is not finite takes automatic exposure's last choice.
typedef struct UnxPhotoDesc
{
    uint32_t size, version;          // sizeof, 1
    uint32_t samplesPerPixel;        // the target, both halves together (>= 2)
    uint32_t halfSamplesPerFrame;    // per rendered frame and half (>= 1; each dispatch is also kept near 25 ms)
    UnxCameraDesc camera;
} UnxPhotoDesc;
typedef struct UnxPhotoStatus
{
    uint32_t size, version;          // sizeof, 1 (set by the caller)
    uint32_t active;                 // the frames show the photo
    uint32_t width, height, samples, target;
    uint32_t saves;                  // saves completed since UnxPhotoBegin
    double relMse;                   // the image's relMSE against the converged one at the last save (-1 before one)
    double startSeconds;             // snapshot upload and acceleration structures on the render thread
    uint64_t generation;             // counts UnxPhotoBegin / UnxPhotoEnd calls
    char error[256];                 // the last start or save failure (UTF-8, empty when none)
} UnxPhotoStatus;
#ifdef __cplusplus
static_assert(sizeof(UnxPhotoDesc) == 64 && sizeof(UnxPhotoStatus) == 312, "UnxPhoto* are part of the ABI (UnravelNextRendererNative.cs)");
#endif
UNX_API int32_t UNX_CALL UnxPhotoBegin(UnxRenderer r, const UnxPhotoDesc* desc);
// Saved when the next frame renders: 'utf8Exr' (nullable) linear radiance x exposure (OpenEXR, 32-bit float), 'utf8Png'
// (nullable) the post chain's SDR display encoding as 16-bit RGB. Completion and failures show in UnxPhotoGetStatus.
UNX_API int32_t UNX_CALL UnxPhotoSave(UnxRenderer r, const char* utf8Exr, const char* utf8Png);
UNX_API int32_t UNX_CALL UnxPhotoEnd(UnxRenderer r);
UNX_API int32_t UNX_CALL UnxPhotoGetStatus(UnxRenderer r, UnxPhotoStatus* status);

// Engine 1's shared GPU bridge (optional exports within ABI 6; coordination decision (a), 84922cc): the renderer's device
// runs NativePhysics / NativeVfx GPU work through an NRC_GpuBridge table (Unravel Native/RuntimeCommon/GpuExecutionAbi.h,
// 136 B) - the same shape as the old TnrAcquireGpuBridge / TnrReleaseGpuBridge / TnrGetSharedGpuStatistics. Acquire
// leases a table (size = sizeof(NRC_GpuBridge)); release gives it back (calls its release, then zeroes it).
typedef struct NRC_GpuBridge NRC_GpuBridge;
typedef struct NRC_GpuStatistics NRC_GpuStatistics;
UNX_API int32_t UNX_CALL UnxAcquireGpuBridge(UnxRenderer r, NRC_GpuBridge* bridge, uint32_t size);
UNX_API int32_t UNX_CALL UnxReleaseGpuBridge(NRC_GpuBridge* bridge, uint32_t size);
UNX_API int32_t UNX_CALL UnxGpuBridgeStatistics(UnxRenderer r, NRC_GpuStatistics* statistics, uint32_t size);

// B10 strand hair (optional exports within ABI 6, INTERFACES v1.69; E's hair::HairSystem, Passes/Hair/Hair.h): a body of
// guide strands (nodesPerStrand nodes each, the root bound to a joint) with follow strands around them, simulated per
// World tick on the GPU and drawn in V's coverage layer with the Hair-class material. Per World step: UnxHairTick for
// every body (the joints' world transforms and the body's capsules at the tick's end, the wind at the body, the tick
// interval), then UnxHairSetFrameFraction with the rendered frame's time within the latest tick (0 = the previous tick's
// end, 1 = the latest's), then the frame. Body ids are reused after removal (last freed first).
typedef struct UnxHairSimulation
{
    float gravity[3], damping;          // m/s^2; fraction of the Verlet velocity removed per substep
    float globalStiffness, globalRange; // pull per substep towards the rest pose; fraction of the strand it acts on
    float localStiffness;               // pull per sweep towards the rest vector in the carried frame
    uint32_t localIterations;           // <= 16
    float dftlDamping, collisionMargin; // DFTL velocity correction; m
    uint32_t substeps;                  // >= 1
    float windDrag;                     // wind acceleration per (m/s) of relative air speed
} UnxHairSimulation;
typedef struct UnxHairFollow
{
    uint32_t guide;
    float offset[3];                    // at the root, in the guide's rest frame (x along the root segment)
    float tipSpread;                    // offset scale at the tip (1: parallel)
} UnxHairFollow;
typedef struct UnxHairBodyDesc
{
    uint32_t size, version;             // sizeof, 1
    uint32_t nodesPerStrand, joints;    // 2..32 nodes; joints the roots bind to
    uint32_t guides, follows;
    const float* restPositions;         // guides x nodesPerStrand x 3, in the space of each guide's joint
    const uint32_t* guideJoint;         // guides
    const UnxHairFollow* followStrands; // follows
    float rootRadius, tipRadius;        // m
    uint32_t material, instance;        // a Hair-class scene material; the scene instance it belongs to (0xFFFFFFFF: none)
    UnxHairSimulation simulation;
} UnxHairBodyDesc;
typedef struct UnxHairCapsule
{
    float a[3], radius;                 // world
    float b[3];
    uint32_t reserved;                  // 0
} UnxHairCapsule;
#ifdef __cplusplus
static_assert(sizeof(UnxHairSimulation) == 48 && sizeof(UnxHairFollow) == 20 && sizeof(UnxHairBodyDesc) == 112 && sizeof(UnxHairCapsule) == 32,
              "UnxHair* are part of the ABI");
#endif
UNX_API int32_t UNX_CALL UnxHairAddBody(UnxRenderer r, const UnxHairBodyDesc* desc, uint32_t* body);
UNX_API int32_t UNX_CALL UnxHairTick(UnxRenderer r, uint32_t body, const float* joints12, uint32_t jointCount, const UnxHairCapsule* capsules,
                                     uint32_t capsuleCount, const float wind[3], float dt);
UNX_API int32_t UNX_CALL UnxHairSetFrameFraction(UnxRenderer r, float fraction);
UNX_API int32_t UNX_CALL UnxHairRemoveBody(UnxRenderer r, uint32_t body);

// B8 GPU fluids (optional export within ABI 6, INTERFACES v1.70; engine 1's shared-mode physics fluids on the renderer's
// device): the frames queued from now on draw these fluids until the next call (count 0: none). view = the
// NP_FluidGpuView (96 B) or NP_FluidGpuView2 (136 B: an anchored domain's start origin and frame velocity) np_fluid_gpu_view
// filled for the tick, alpha = the frame's time within that tick, domainCells = the
// fluid's domain in cells; stamp = the tick's NRC_GpuWorldStamp (world, world_generation, epoch, tick, branch, phase).
// The renderer admits the reads through the GPU bridge before each frame's lists and commits them with its fence.
typedef struct UnxFluidInput
{
    const void* view;
    float alpha;
    uint32_t domainCells[3];
    uint32_t material;                  // the scene material of its surface (Water class)
    uint32_t reserved;                  // 0
} UnxFluidInput;
#ifdef __cplusplus
static_assert(sizeof(UnxFluidInput) == 32, "UnxFluidInput is part of the ABI");
#endif
UNX_API int32_t UNX_CALL UnxFrameSetFluids(UnxRenderer r, const UnxFluidInput* fluids, uint32_t count, const uint64_t stamp[6]);

// B7 the sea (optional export within ABI 6, INTERFACES v1.72; engine 1's W: FFT spectrum and view grid): the frames queued
// from now on draw this sea until the next call (null: none). World coordinates (the renderer applies its origin shifts
// to the level and the lake centre). windDirection in radians; horizontalBound / verticalBound: the roughest sea's
// horizontal and vertical displacement bounds R and A (m); lake: 0 the open sea, 1 a circular lake.
typedef struct UnxOceanDesc
{
    uint32_t size, version;             // sizeof (72), 1
    float windSpeed, windDirection, fetch, spread;
    uint32_t seed, lake;
    double level;                       // still water height, world (m)
    double lakeCentre[2];               // world (x, z), m
    float horizontalBound, verticalBound, lakeRadius;
    uint32_t reserved;                  // 0
} UnxOceanDesc;
#ifdef __cplusplus
static_assert(sizeof(UnxOceanDesc) == 72, "UnxOceanDesc is part of the ABI");
#endif
UNX_API int32_t UNX_CALL UnxFrameSetOcean(UnxRenderer r, const UnxOceanDesc* ocean);

// W2 closed basins (optional exports within ABI 6, INTERFACES v1.78; FEATURES_GAME 1.10): the baths and pools the frames
// queued from now on draw, until the next call (count 0: none). World coordinates (the renderer applies its origin
// shifts); id: stable and nonzero (the basin's ripples live under it; a basin left out of a later call is released);
// centre: the still surface's centre, y = the level with no bodies in the water; yaw about +y (rad, the renderer's
// right-handed world: local x -> (cos, 0, -sin)); surfaceFilm 0 (clean) or 1 (inextensible: bathers, soap).
typedef struct UnxPoolDesc
{
    uint32_t size, version;             // sizeof (64), 1
    uint32_t id, material;              // material: the scene material of the surface (Water class)
    float sizeX, sizeZ, depth, surfaceFilm;
    double centre[3];                   // world, m
    float yaw;
    uint32_t shape;                     // 0 rectangle (sizeX x sizeZ); 1 round (v1.92, W2-R: sizeX = the diameter, sizeZ ignored;
                                        // this word was 'reserved = 0' before, so older bridges describe rectangles)
} UnxPoolDesc;
#ifdef __cplusplus
static_assert(sizeof(UnxPoolDesc) == 64, "UnxPoolDesc is part of the ABI");
#endif
UNX_API int32_t UNX_CALL UnxFrameSetPools(UnxRenderer r, const UnxPoolDesc* pools, uint32_t count);
// Disturbances of the basins (physics contacts, bodies entering and moving: FEATURES_GAME 1.10): each one goes to exactly
// one frame, the next queued (with a frame that is never drawn, to the one after it). World coordinates, inside its basin.
// impulse: vertical impulse on the water (N s, + downward); volume: the change of the volume the body displaces there (m^3,
// + water pushed out); radius: the footprint's Gaussian sigma (m, > 0).
typedef struct UnxPoolSource
{
    double x, z;
    float radius, impulse, volume;
    uint32_t pool;                      // UnxPoolDesc::id
} UnxPoolSource;
#ifdef __cplusplus
static_assert(sizeof(UnxPoolSource) == 32, "UnxPoolSource is part of the ABI");
#endif
UNX_API int32_t UNX_CALL UnxFrameAddPoolSources(UnxRenderer r, const UnxPoolSource* sources, uint32_t count);
// The surface statistics of a basin (optional export within ABI 6, INTERFACES v1.90; FEATURES_GAME 1.10): over the
// basin's 257^2 samples of the record framesInFlight records before the latest, relative to the still level: the mean
// height, the RMS of height - mean, and max |height - mean| (m). valid = 0 (UNX_OK) until a record of the basin has
// completed on the GPU, or when the basin is not in the current set; the other fields are then 0.
typedef struct UnxPoolStats
{
    uint32_t size, version;             // sizeof (48), 1
    uint32_t pool, valid;               // UnxPoolDesc::id (in), 0 / 1 (out)
    uint64_t frameIndex;                // the record's frame
    double time;                        // the basin's time at that record (s)
    float mean, rms, maxDeviation;      // m
    uint32_t reserved;                  // 0
} UnxPoolStats;
#ifdef __cplusplus
static_assert(sizeof(UnxPoolStats) == 48, "UnxPoolStats is part of the ABI");
#endif
UNX_API int32_t UNX_CALL UnxPoolStatsLatest(UnxRenderer r, UnxPoolStats* stats);

// B5 clouds (optional export within ABI 6, INTERFACES v1.77; after commit, any time): the frame's cloud layer, weather
// content held until changed (render::CloudLayerDesc). Null or coverage 0 = no clouds.
typedef struct UnxCloudDesc
{
    uint32_t size, version;             // sizeof (48), 1
    float coverage;                     // [0, 1]
    float baseAltitude, topAltitude;    // world metres, base < top
    float sigmaMax, albedo;             // peak extinction (1/m) > 0, single-scattering albedo [0, 1]
    float windX, windZ;                 // m/s (advection in World time)
    uint32_t seed;
    uint32_t reserved[2];               // 0
} UnxCloudDesc;
#ifdef __cplusplus
static_assert(sizeof(UnxCloudDesc) == 48, "UnxCloudDesc is part of the ABI");
#endif
UNX_API int32_t UNX_CALL UnxFrameSetClouds(UnxRenderer r, const UnxCloudDesc* clouds);

// The height fog (optional export within ABI 6; after commit, any time): weather content held until changed
// (render::FogDesc). Null: the quality file's atmosphere.fog decides (off unless the file turns it on).
typedef struct UnxFogDesc
{
    uint32_t size, version;             // sizeof (56), 1
    float density;                      // extinction (1/m) at 'height', >= 0 (0: no fog)
    float heightFalloff;                // the density halves every 1 / this metres of height, >= 0
    float height;                       // world metres
    float albedo[3];                    // scattering / extinction, [0, 1]
    float phaseG;                       // Henyey-Greenstein asymmetry, (-1, 1)
    float startDistance;                // m, >= 0
    float skyAmount;                    // [0, 1]: how much of the fog sky pixels take (1: as every pixel; 0: opaque pixels only)
    float noiseAmount;                  // [0, 1]: the density's variation about its mean (0: a uniform medium)
    float noiseScale;                   // m, >= 1: the variation's largest features
    uint32_t reserved;                  // 0
} UnxFogDesc;
#ifdef __cplusplus
static_assert(sizeof(UnxFogDesc) == 56, "UnxFogDesc is part of the ABI");
#endif
UNX_API int32_t UNX_CALL UnxFrameSetFog(UnxRenderer r, const UnxFogDesc* fog);
// Local fog volumes (optional export within ABI 6; after commit, any time): the current set, held until changed
// (render::FogVolumeDesc; at most 16 take effect). Extra fog inside an ellipsoid or a box - steam over a bath, mist in a
// hollow - lit as the fog is (the sun and its shadows, the local lights, the indirect light); seen within the fog's
// near volume (80 m by default). count 0: none.
typedef struct UnxFogVolumeDesc
{
    uint32_t size, version;             // sizeof (80), 1
    double centre[3];                   // world, m
    float halfSize[3];                  // m, > 0: the ellipsoid's radii or the box's half extents along its axes
    float yaw;                          // rad, about the up axis
    uint32_t shape;                     // 0 ellipsoid, 1 box
    float density;                      // extinction (1/m) at the volume's bottom, away from its boundary, >= 0
    float heightFalloff;                // the density halves this many times from the bottom to the top, >= 0 (0: uniform)
    float edge;                         // (0, 1]: the outer share of the volume over which the density fades to 0
    float albedo[3];                    // scattering / extinction, [0, 1]
    uint32_t reserved;                  // 0
} UnxFogVolumeDesc;
#ifdef __cplusplus
static_assert(sizeof(UnxFogVolumeDesc) == 80, "UnxFogVolumeDesc is part of the ABI");
#endif
UNX_API int32_t UNX_CALL UnxFrameSetFogVolumes(UnxRenderer r, const UnxFogVolumeDesc* volumes, uint32_t count);

// C5 terrain material (optional export within ABI 6, INTERFACES v1.74; before UnxSceneCommit): the layers of a material
// added with UNX_MATERIAL_TERRAIN - 1..8 Standard materials read at layer uv = uv0 x scale + offset, weighted by channel
// i % 4 of splat texture i / 4 (UNX_TEXTURE_RGBA8_LINEAR; splat1 = UNX_NONE with at most 4 layers).
typedef struct UnxTerrainLayer
{
    uint32_t material;                  // a Standard-class material of the scene
    float scale[2], offset[2];
    uint32_t reserved[3];               // 0
} UnxTerrainLayer;
#ifdef __cplusplus
static_assert(sizeof(UnxTerrainLayer) == 32, "UnxTerrainLayer is part of the ABI");
#endif
UNX_API int32_t UNX_CALL UnxSceneSetTerrainLayers(UnxRenderer r, uint32_t material, uint32_t splat0, uint32_t splat1, const UnxTerrainLayer* layers, uint32_t count);

// Character shading (optional export within ABI 6; before or after UnxSceneCommit): what skin, eyes and cloth need beyond
// UnxMaterialDesc, for a material already in the scene. One description holds the three groups; each applies to the
// materials it is defined on and is ignored on the others:
//   skin   UNX_MATERIAL_SUBSURFACE: the medium's mean free path per colour channel - the screen-space scattering spreads
//          the diffuse light by it -, and the two specular lobes (roughness x scale each, lobe 0 weighing the mix). The
//          light through thin parts (ears, fingers) is the material's own 'transmission'.
//   eye    UNX_MATERIAL_SUBSURFACE with eyeIrisRadius > 0: one sphere-like mesh is sclera, iris and cornea. Its uv
//          (0.5, 0.5) is where the optical axis leaves the eye and eyeAxis that axis in the mesh's object space (renderer
//          space, as the mesh's positions); the base colour texture is the whole eye's with the iris painted around the uv
//          centre, eyeIrisRadius its radius in uv units. The iris is seen through the cornea (refraction onto the iris
//          plane, eyeIrisDepth under the cornea's apex), darkened at the limbus, lit on its own plane with the light of a
//          grazing lamp gathered on its far side; the sclera scatters with the mean free path; the specular lobe is the
//          cornea's, at the material's roughness and specular (the two skin lobes and 'transmission' are not used).
//          eyePupilScale may change every frame (a pupil that follows the light): call again with the new value.
//   cloth  UNX_MATERIAL_STANDARD with a sheen (sheenColor: the fuzz): the share of the base's specular lobe the fuzz
//          replaces. The reference's Cloth shading model (FuzzColor, Cloth) is sheenColor = Cloth x FuzzColor, cloth = Cloth.
// Values of 0 marked "default" take the engine's default, so a zeroed description with one group filled in is valid.
// Before commit the material is changed in place; after commit the change reaches the GPU scene with the next queued
// frame. UnxSceneEditMaterials describes a material anew without these fields: the material keeps them while its class
// stays the same (cloth: while it has a sheen).
typedef struct UnxCharacterShadingDesc
{
    uint32_t size, version;             // sizeof (80), 1
    float subsurfaceMeanFreePath[3];    // m (1 / sigma_t), >= 0; all 0: default (skin: 0.00130, 0.00095, 0.00067)
    float subsurfaceLobeMix;            // [0, 1]: the weight of lobe 0 (read with the scales)
    float subsurfaceLobeRoughness[2];   // >= 0: the lobes' roughness scales; both 0: default (mix 0.85, scales 0.75 and 1.30)
    float cloth;                        // [0, 1]; 0 = the sheen over the whole base
    float eyeIrisRadius;                // (0, 0.5] uv units; 0 = not an eye
    float eyeIrisDepth;                 // (0, 2] iris radii: the cornea's apex above the iris plane; 0: default 0.45
    float eyeLimbusWidth;               // [0.01, 1] iris radii: the band where the iris gives way to the sclera; 0: default 0.12
    float eyeLimbusDarkening;           // [0, 1]: the limbal ring (0 = none)
    float eyePupilScale;                // (0, 8]: the iris texture's radius 1 - (1 - r) x this (> 1: a wider pupil); 0: default 1
    float eyeIrisConcavity;             // [0, 1]: how far a grazing light gathers on the iris's far side (0 = evenly lit)
    float eyeIor;                       // [1, 2]: the aqueous humour's index; 0: default 1.336
    float eyeAxis[3];                   // unit, the mesh's object space, out of the eye; all 0: default (0, 0, 1)
    uint32_t reserved;                  // 0
} UnxCharacterShadingDesc;
#ifdef __cplusplus
static_assert(sizeof(UnxCharacterShadingDesc) == 80, "UnxCharacterShadingDesc is part of the ABI");
#endif
UNX_API int32_t UNX_CALL UnxSceneSetCharacterShading(UnxRenderer r, uint32_t material, const UnxCharacterShadingDesc* desc);

// Material inputs (optional exports within ABI 6): what a game's materials carry beyond UnxMaterialDesc, for a material
// already in the scene - every class but UNX_MATERIAL_CUT and UNX_MATERIAL_TERRAIN (ignored there).
//   uv        the transform of the material's own textures (base colour, normal, roughness / metallic, emissive and its
//             mask, occlusion on uv set 0, height): uv' = R(uvRotation) (uv x uvScale) + uvOffset - Unity's tiling and
//             offset are uvScale and uvOffset. The alpha test cuts through it in every view, in the shadows and at ray hits.
//   uv sets   occlusionUvSet / detailUvSet 1: that map is read on the mesh's second uv set (UnxSceneSetMeshAttributes; a
//             mesh without one: its uv0), without the transform.
//   detail    a tiled colour (UNX_TEXTURE_RGBA8_SRGB; multiplies the base colour, neutral at sRGB 0.5 - Unity's detail
//             albedo x2) and normal (UNX_TEXTURE_RG8_NORMAL; its slopes add to the base normal's) at uv(detailUvSet) x
//             detailScale + detailOffset, weighted by their strengths and, with UNX_MATERIAL_INPUT_VERTEX_BLEND, by the
//             vertex colour's alpha.
//   height    UNX_TEXTURE_R8_LINEAR, 1 = the surface, 0 = heightScale metres under it: parallax occlusion mapping in the
//             main and planar views (the quality file's material.parallax_steps; the pixel's depth stays the surface's).
//   emission  emissiveScale multiplies the material's emissive (the intensity apart from the colour); the mask
//             (UNX_TEXTURE_R8_LINEAR, on the material's uv) multiplies it per texel.
//   flags     UNX_MATERIAL_INPUT_VERTEX_TINT: the vertex colour's rgb multiplies the base colour;
//             UNX_MATERIAL_INPUT_ALPHA_DITHER: an alpha-tested material's cut is dithered around its cutoff in the views
//             (soft edges under the temporal upscale; ignored without an alpha cutoff).
// Fill the description with UnxMaterialInputsDefaults first (a zeroed one names texture 0 four times and has no scale).
// Before commit the material is changed in place; after commit the change reaches the GPU scene with the next queued
// frame (textures must already be in the scene). UnxSceneEditMaterials describes a material anew without these fields:
// the material keeps them.
#define UNX_MATERIAL_INPUT_VERTEX_TINT 1u
#define UNX_MATERIAL_INPUT_VERTEX_BLEND 2u
#define UNX_MATERIAL_INPUT_ALPHA_DITHER 4u
typedef struct UnxMaterialInputsDesc
{
    uint32_t size, version;             // sizeof (96), 1
    float uvScale[2];                   // finite, != 0 (default 1, 1)
    float uvOffset[2];
    float uvRotation;                   // radians, counter-clockwise in uv
    uint32_t occlusionUvSet;            // 0 or 1
    uint32_t detailColorTexture;        // a scene texture or UNX_NONE
    uint32_t detailNormalTexture;       // a scene texture or UNX_NONE
    float detailScale[2];               // finite, != 0 (default 1, 1)
    float detailOffset[2];
    uint32_t detailUvSet;               // 0 or 1
    float detailColorStrength;          // [0, 1] (default 1)
    float detailNormalScale;            // [0, 4] (default 1)
    uint32_t heightTexture;             // a scene texture or UNX_NONE
    float heightScale;                  // m, [0, 1] (0: no parallax)
    float emissiveScale;                // >= 0 (default 1)
    uint32_t emissiveMaskTexture;       // a scene texture or UNX_NONE
    uint32_t flags;                     // UNX_MATERIAL_INPUT_*
    uint32_t reserved[2];               // 0
} UnxMaterialInputsDesc;
#ifdef __cplusplus
static_assert(sizeof(UnxMaterialInputsDesc) == 96, "UnxMaterialInputsDesc is part of the ABI");
#endif
UNX_API int32_t UNX_CALL UnxMaterialInputsDefaults(UnxMaterialInputsDesc* desc);
UNX_API int32_t UNX_CALL UnxSceneSetMaterialInputs(UnxRenderer r, uint32_t material, const UnxMaterialInputsDesc* desc);
// A mesh's optional vertex streams (before UnxSceneCommit; a mesh added with UnxSceneAddMesh): uv1 = 2 floats per vertex
// (the second uv set) or null, colors = one RGBA8 per vertex with r in the low byte (linear values; Unity's Color32) or
// null; vertexCount must be the mesh's.
UNX_API int32_t UNX_CALL UnxSceneSetMeshAttributes(UnxRenderer r, uint32_t mesh, const float* uv1, const uint32_t* colors, uint32_t vertexCount);

// Light components (optional exports within ABI 6; before UnxSceneCommit: lights are fixed at commit): what a light
// carries beyond UnxLightDesc (scene::Light's light components; the engine's local light and rect light components).
//   scales       the light's specular lobes and its diffuse light on the surfaces it lights directly, its in-scattering in
//                the air and fog, and its share in the indirect light (what GI and reflections carry on); >= 0.
//   source       rect lights: the image the emitter shows and emits (a scene texture, UNX_TEXTURE_RGBA8_SRGB or
//                UNX_TEXTURE_RGBA16_FLOAT; UNX_NONE: uniform); the light's colour multiplies it.
//   barn doors   rect lights: four flaps of barnDoorLength metres along the emitter's edges, opened by barnDoorAngle
//                radians from the emitter's normal (0: straight walls, pi / 2: flat - no effect). Length 0: none.
//   channels     a mask of the three lighting channels the light is in (default 1: channel 0): it lights the instances
//                that share one (UNX_INSTANCE_LIGHTING_CHANNELS, UnxSceneSetInstanceLightingChannels).
//   distance     the light is not drawn past maxDrawDistance metres from the camera (0: always) and fades out over the
//                last maxDistanceFadeRange metres before it (0: a cut).
//   temperature  kelvin (1,000..15,000; 0: not used): the light's colour is UnxLightDesc::color times the black body's
//                chromaticity at it, at the colour's own luminance (6,500 K: about white).
//   falloff      point and spot lights: 0 = the inverse-square falloff with the range's window (intensity in candela);
//                > 0: (1 - (d / range)^2)^exponent without the inverse square, the intensity then the illuminance (lux)
//                at the light.
// Fill the description with UnxLightComponentsDefaults first (a zeroed one has no scales and names texture 0). The
// values are checked at UnxSceneCommit with the rest of the scene.
typedef struct UnxLightComponentsDesc
{
    uint32_t size, version;             // sizeof (64), 1
    float specularScale, diffuseScale, volumetricScattering, indirectIntensity;  // >= 0 (default 1)
    uint32_t sourceTexture;             // a scene texture or UNX_NONE
    float barnDoorAngle;                // radians, [0, pi / 2] (default pi / 2)
    float barnDoorLength;               // m, >= 0 (default 0: none)
    uint32_t lightingChannels;          // 3 bits (default 1)
    float maxDrawDistance;              // m, >= 0 (default 0: always drawn)
    float maxDistanceFadeRange;         // m, >= 0
    float temperature;                  // K (default 0: not used)
    float falloffExponent;              // >= 0 (default 0: inverse square)
    uint32_t reserved[2];               // 0
} UnxLightComponentsDesc;
#ifdef __cplusplus
static_assert(sizeof(UnxLightComponentsDesc) == 64, "UnxLightComponentsDesc is part of the ABI");
#endif
UNX_API int32_t UNX_CALL UnxLightComponentsDefaults(UnxLightComponentsDesc* desc);
UNX_API int32_t UNX_CALL UnxSceneSetLightComponents(UnxRenderer r, uint32_t light, const UnxLightComponentsDesc* desc);
// The lighting channels of an instance already added (before UnxSceneCommit; channels: a mask of the three channels, as
// UNX_INSTANCE_LIGHTING_CHANNELS takes it). After commit, describe the instance anew with the flag bits
// (UnxSceneEditInstances).
UNX_API int32_t UNX_CALL UnxSceneSetInstanceLightingChannels(UnxRenderer r, uint32_t instance, uint32_t channels);

// Loads a .unxscene file (INTERFACES 6.2) as the renderer's content: textures, materials, meshes, skeletons, instances
// (their flags included), lights, sun, atmosphere and wind, with the file's indices. Only before any content was added and
// before UnxSceneCommit; content added afterwards appends. 'camera0' (nullable) receives the file's camera 0 (fails when
// the file has none), 'instances' / 'skeletons' (nullable) the counts. Optional export within ABI 6.
UNX_API int32_t UNX_CALL UnxSceneLoad(UnxRenderer r, const char* utf8Path, UnxCameraDesc* camera0, uint32_t* instances, uint32_t* skeletons);

typedef struct UnxFrameDesc
{
    uint32_t size, version;     // sizeof, 1
    uint64_t frameIndex;
    double time;                // seconds (wind, animation clocks)
    float deltaTime;
    uint32_t outputWidth, outputHeight;
    float displayPeak;          // HDR display (A4): its peak over paper-white luminance (>= 1); the output is then
                                // R16G16B16A16 FLOAT holding display-referred linear Rec.709 light, 1 = paper white (the
                                // host encodes it for the swap chain: scRGB or PQ). 0 = SDR: R10G10B10A2, sRGB-encoded.
                                // (This was 'reserved', always 0: older hosts get SDR.)
    UnxCameraDesc camera;
    uint64_t output;            // ID3D12Resource* of the output (R10G10B10A2 UNORM, or R16G16B16A16 FLOAT with a
                                // displayPeak; random write), Unity leaves it in UNORDERED_ACCESS for the frame's list
                                // and in its own state otherwise
} UnxFrameDesc;

// Per-frame scene changes (after UnxSceneCommit), collected into the next UnxFrameQueue. Transforms and poses are the
// values to show in that frame (the host interpolates between committed World ticks; the renderer keeps the previous
// frame's for motion).
enum UnxTransformFlags
{
    UNX_TRANSFORM_TELEPORT = 1u << 0,  // the instance jumped: no motion this frame (prevObjectToWorld = objectToWorld)
};

typedef struct UnxTransformUpdate
{
    uint32_t instance, flags;   // UnxTransformFlags
    float transform[12];        // object -> world, row-major 3x4: rotation, uniform scale, translation
} UnxTransformUpdate;
UNX_API int32_t UNX_CALL UnxFrameSetTransforms(UnxRenderer r, const UnxTransformUpdate* updates, uint32_t count);
// jointToModel: 12 floats per joint, the skeleton's model-space joints (the palette is jointToModel x inverseBind).
UNX_API int32_t UNX_CALL UnxFrameSetSkeleton(UnxRenderer r, uint32_t skeleton, const float* jointToModel, uint32_t jointCount);
// Every listed skeleton's pose in one call: jointToModel holds the poses back to back in list order, each with its
// skeleton's joint count (12 floats per joint); jointCount is the buffer's total, checked before anything is recorded.
UNX_API int32_t UNX_CALL UnxFrameSetSkeletons(UnxRenderer r, uint32_t count, const uint32_t* skeletons, const float* jointToModel, uint64_t jointCount);
UNX_API int32_t UNX_CALL UnxFrameSetInstanceVisible(UnxRenderer r, uint32_t instance, uint32_t visible);
// C4: blend shape weights and vertex animation times of the next queued frame: weights holds each listed instance's
// weights back to back (its mesh's blend shape count each; weightCount is the total, checked first); times one per
// instance (null = 0). An instance not listed keeps its last values.
UNX_API int32_t UNX_CALL UnxFrameSetMorphs(UnxRenderer r, uint32_t count, const uint32_t* instances, const float* weights, uint64_t weightCount,
                                           const float* times);
// C9: origin rebase of the next queued frame: its coordinates are the previous frame's minus shift (whole multiples of
// 1024 m per axis). Send the frame's transforms and camera in the new coordinates (transforms already queued for the frame
// are moved).
UNX_API int32_t UNX_CALL UnxFrameSetOriginShift(UnxRenderer r, const double shift[3]);

// C2b runtime geometry (CARVE destruction fragments, generated meshes): meshes and instances added and removed between
// frames without rebuilding the scene. Room is reserved before UnxSceneCommit; ids are the host's (a runtime mesh id has
// bit 31 set and can be used as the mesh of UnxFrameAddRuntimeInstance, as can any committed mesh index). Adds and removals
// take effect with the next queued frame, in call order; a removal waits until no frame in flight draws it. Raster,
// shadows and materials see runtime geometry; ray tracing (reflections, GI) does not yet (FEATURES_GAME 2.1 event BLAS).
typedef struct UnxRuntimeCapacity
{
    uint32_t size, version;  // sizeof, 1
    uint32_t meshes, submeshes, vertices, indices, clusters, clusterVertexIndices, clusterTriangles, nodes, instances;
} UnxRuntimeCapacity;
UNX_API int32_t UNX_CALL UnxSceneReserveRuntime(UnxRenderer r, const UnxRuntimeCapacity* capacity);
UNX_API int32_t UNX_CALL UnxFrameAddRuntimeMesh(UnxRenderer r, const UnxMeshDesc* desc, uint32_t* id);
UNX_API int32_t UNX_CALL UnxFrameRemoveRuntimeMesh(UnxRenderer r, uint32_t id);
UNX_API int32_t UNX_CALL UnxFrameAddRuntimeInstance(UnxRenderer r, uint32_t mesh, const float objectToWorld[12], uint32_t flags, uint32_t* id);
UNX_API int32_t UNX_CALL UnxFrameRemoveRuntimeInstance(UnxRenderer r, uint32_t id);
UNX_API int32_t UNX_CALL UnxFrameSetRuntimeTransforms(UnxRenderer r, uint32_t count, const uint32_t* ids, const float* objectToWorld);

// C5 terrain deformation D (FEATURES_GAME 9, PHYSICS 9.7): footprints, wheel ruts, craters as geometry. Each call gives
// the whole current window (physics' D after its tick). Texel (i, j) is at world (originX + i spacing, originZ + j spacing)
// in this frame's coordinates (after origin shifts); texels lie on the terrain grid (cell size / spacing an integer) and
// D is 0 at the window's edge. 'tiles' are the scene instances of cooked terrain tiles (Unravel UnravelNextTerrainCook:
// placed by translation) the window may touch; tiles patched before and not listed lose their patches. Every 4 x 4 cell
// block whose closure has non-zero D is replaced by a mesh at D's resolution (runtime geometry: reserve room with
// UnxSceneReserveRuntime, per block (4 cells / spacing + 1)^2 vertices); unchanged blocks are kept. Rays (GI,
// reflections) see the tile without D (condition: D depth <= 10 cm).
typedef struct UnxTerrainDeformation
{
    uint32_t size, version;   // sizeof, 1
    uint32_t texels;          // per side (0: no deformation)
    float spacing;            // metres per texel
    double originX, originZ;  // world xz of texel (0, 0)
    const float* heights;     // texels^2, rows along +z, metres added to the terrain height (negative = pressed in)
    uint32_t tileCount;
    const uint32_t* tiles;
} UnxTerrainDeformation;
UNX_API int32_t UNX_CALL UnxFrameSetTerrainDeformation(UnxRenderer r, const UnxTerrainDeformation* d);

// History discontinuity of the next queued frame (INTERFACES 5.5.2): RESTORE for a World snapshot restore, save load
// or branch change (every temporal state resets), CUT for a camera cut (view-bound histories reset, world-space caches
// kept). Set for the first frame after the event; bits of a frame that is never rendered carry into the next one.
enum UnxDiscontinuity
{
    UNX_DISCONTINUITY_RESTORE = 1u << 0,
    UNX_DISCONTINUITY_CUT = 1u << 1,
};
UNX_API int32_t UNX_CALL UnxFrameSetDiscontinuity(UnxRenderer r, uint32_t flags);
// GPU simulation steps submitted in the next queued frame (FrameContext::gpuSimulation): R's GI spreads its rays by it.
enum UnxGpuSimulation
{
    UNX_GPU_SIMULATION_SOFT = 1u << 0,
    UNX_GPU_SIMULATION_VFX = 1u << 1,
    UNX_GPU_SIMULATION_RIGID = 1u << 2,
};
UNX_API int32_t UNX_CALL UnxFrameSetSimulation(UnxRenderer r, uint32_t gpuSimulation);
// Sun of the following frames (time of day): unit direction ground -> sun, lux at the top of the atmosphere.
UNX_API int32_t UNX_CALL UnxFrameSetSun(UnxRenderer r, const float direction[3], float illuminance, const float color[3], float angularRadius);
// Sun, atmosphere and wind of the following frames (time of day, weather); the atmosphere track rebuilds its LUTs when
// they change. Wind changes follow INTERFACES 6.4 (v1.23): the wind model is memoryless and the tracks bound a change by
// its endpoints, so a host may change the wind every frame.
UNX_API int32_t UNX_CALL UnxFrameSetEnvironment(UnxRenderer r, const UnxEnvironmentDesc* desc);

// Snapshot of one frame's inputs and the changes set since the previous queue; *ticket goes into UNX_EVENT_RENDER's
// data. A ticket that is never rendered is dropped after framesInFlight + 2 newer ones; its changes move to the next.
UNX_API int32_t UNX_CALL UnxFrameQueue(UnxRenderer r, const UnxFrameDesc* desc, uint64_t* ticket);
// Standalone renderers only (UNX_RENDERER_STANDALONE): records and executes a queued frame on the renderer's own
// queue into its own output (desc->output ignored); blocks until the GPU finishes when readback is non-null and then
// copies the RGB10A2 pixels (width * height * 4 bytes).
UNX_API int32_t UNX_CALL UnxFrameRenderStandalone(UnxRenderer r, uint64_t ticket, void* readback, uint64_t readbackBytes);

typedef struct UnxFrameStats
{
    uint32_t size, version;     // sizeof, 1
    uint64_t frameIndex;        // frame these numbers belong to (completed on the GPU)
    double gpuMs;               // whole frame, GPU timestamps
    double cpuRecordMs, cpuSubmitMs;
    uint32_t passes, reserved;
} UnxFrameStats;
UNX_API int32_t UNX_CALL UnxFrameStatsLatest(UnxRenderer r, UnxFrameStats* stats);

// The frame's time on one queue outside its passes (the profiler's list marks, INTERFACES v1.39): frame span on that
// queue = head + passes + tail + gap.
typedef struct UnxQueueTiming
{
    uint32_t lists, reserved;   // command lists of the frame on this queue (boundaries = lists - 1)
    double headMs;              // the frame's first timestamp to this queue's first list
    double tailMs;              // per list, last pass end to list end (the closing barriers), summed
    double gapMs;               // list end to the next list's begin (other work on the queue, or idle), summed
} UnxQueueTiming;

// What the renderer submitted for the frame UnxFrameStatsLatest reports (its render graph): passes, command lists,
// barriers, queue synchronisation, transient memory, plan compile time, and (version 2) per queue the time outside the
// passes. Optional export within ABI 6 (a bridge probes for it). Version 1 (64 B, without 'queues') is still accepted.
typedef struct UnxFrameGraphStats
{
    uint32_t size, version;     // sizeof, 2
    uint64_t frameIndex;        // the same frame as UnxFrameStats::frameIndex
    uint32_t livePasses, commandLists, barrierBatches, barriers;
    uint32_t crossQueueSyncs, transientResources, planReused;
    uint32_t sceneRevision;     // the GPU scene's revision the frame recorded with: it changes on uploads and on published
                                // material textures, and every change resets the GI cache's history (a new lighting epoch)
    uint64_t transientBytesAliased;
    double cpuCompileMs;        // plan build (0 when the cached plan was reused)
    UnxQueueTiming queues[2];   // graphics, compute (version 2)
} UnxFrameGraphStats;
UNX_API int32_t UNX_CALL UnxFrameGraphStatsLatest(UnxRenderer r, UnxFrameGraphStats* stats);

// This process's video memory on the renderer's adapter (DXGI QueryVideoMemoryInfo, bytes): local (VRAM) and non-local
// (system memory the GPU maps). 'r' names the renderer whose device's adapter is asked; 0 asks Unity's device (inside
// Unity). Usage is the whole process: Unity's own resources, every renderer's, and any other D3D12 user in the process.
// Optional export within ABI 6.
typedef struct UnxVideoMemoryInfo
{
    uint32_t size, version;     // sizeof, 1
    uint64_t localBudget, localUsage, localReservation, localAvailableForReservation;
    uint64_t nonLocalBudget, nonLocalUsage, nonLocalReservation, nonLocalAvailableForReservation;
} UnxVideoMemoryInfo;
UNX_API int32_t UNX_CALL UnxVideoMemory(UnxRenderer r, UnxVideoMemoryInfo* info);

// Per-pass GPU time of the same completed frame (pass names as the render graph declares them, e.g. "v.raster.bandA").
typedef struct UnxPassTiming
{
    char name[48];              // UTF-8, truncated to 47 bytes
    double ms;
} UnxPassTiming;
// Writes min(capacity, passes) entries; *count receives the frame's pass count.
UNX_API int32_t UNX_CALL UnxFramePassTimingsLatest(UnxRenderer r, UnxPassTiming* passes, uint32_t capacity, uint32_t* count);

// ---- The picture's settings a game changes while it runs (optional exports within ABI 6; after commit, any time). Each
// is held until changed: every frame queued afterwards takes the current values. What a run keeps fixed stays in the
// quality file (UnxRendererQualityOverride before commit); these are for what changes with the scene or the moment.

// The colour grading before the tone curve (render::ColorGradingDesc; the quality file's shading.post_grading_* hold
// the same values for a whole run). Null: the quality file's. Each of a range's five values is r, g, b and a master
// that multiplies them (offset: adds to them); the shadows', midtones' and highlights' values combine with the global
// ones by their share of the pixel's luma range. All 1 (offset 0, temperature 6500, tint 0): the picture is unchanged
// and the chain does not build its grading table.
typedef struct UnxColorGradingRange
{
    float saturation[4];        // about the luma (0: grey)
    float contrast[4];          // about scene grey 0.18
    float gamma[4];             // the value to the power 1 / gamma
    float gain[4];
    float offset[4];
} UnxColorGradingRange;
typedef struct UnxColorGradingDesc
{
    uint32_t size, version;     // sizeof, 1
    float temperature;          // K, 1667 .. 25000: the scene's white the picture is balanced from (6500: none)
    float tint;                 // across the temperature's line (+ green, - magenta)
    UnxColorGradingRange global, shadows, midtones, highlights;
    float shadowsMax;           // the luma below which the shadows' values weigh in (> 0; the default 0.09)
    float highlightsMin;        // ... from which the highlights' weigh in (the default 0.5), fully from
    float highlightsMax;        // highlightsMax (> highlightsMin; the default 1)
    float reserved;             // 0
} UnxColorGradingDesc;
#ifdef __cplusplus
static_assert(sizeof(UnxColorGradingDesc) == 352, "UnxColorGradingDesc is part of the ABI");
#endif
UNX_API int32_t UNX_CALL UnxFrameSetColorGrading(UnxRenderer r, const UnxColorGradingDesc* grading);

// The post settings (render::PostSettingsDesc and FrameContext::exposureCompensation). Null: all of them the quality
// file's, no exposure compensation. A NaN (diaphragmBlades: -1) leaves that one value to the quality file's key named
// beside it. The depth of field's aperture and focus distance are UnxFrameSetLens's.
typedef struct UnxPostSettingsDesc
{
    uint32_t size, version;     // sizeof, 1
    float exposureCompensation; // stops over the automatic exposure, + brighter (0: none; finite)
    float exposureMinEv100;     // the automatic exposure's metering range: the EV100 it settles at stays inside
    float exposureMaxEv100;     // (min < max); shading.exposure_min_ev / exposure_max_ev
    float bloomIntensity;       // the share of the light the bloom spreads, 0 .. 1; shading.post_bloom_strength
    float vignette;             // the natural vignetting's strength, 0 .. 1; shading.post_vignette
    float motionBlurAmount;     // the shutter as a fraction of the frame interval, 0 .. 1 (0: none, 0.5: 180 degrees);
                                // shading.motion_blur_shutter
    int32_t diaphragmBlades;    // depth of field: the diaphragm's blades, 0 (a disc) or 4 .. 16; shading.dof_diaphragm_blades
    float lensFullAperture;     // ... and the lens's widest aperture (diameter, m; 0: straight blades);
                                // shading.dof_diaphragm_full_aperture
    float reserved[2];          // 0
} UnxPostSettingsDesc;
#ifdef __cplusplus
static_assert(sizeof(UnxPostSettingsDesc) == 48, "UnxPostSettingsDesc is part of the ABI");
#endif
UNX_API int32_t UNX_CALL UnxFrameSetPost(UnxRenderer r, const UnxPostSettingsDesc* post);

// How an HDR frame (UnxFrameDesc::displayPeak >= 1) is written for the display (FrameContext::displayEncoding):
//   -1  the quality file's output.hdr_encoding (the default);
//    0  linear Rec.709 light with 1 = paper white into R16G16B16A16 FLOAT: the host encodes it for its swap chain;
//    1  scRGB: linear Rec.709 with 1 = 80 cd/m2 into R16G16B16A16 FLOAT - what a swap chain of that format in
//       DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 shows;
//    2  HDR10: Rec.2020 primaries under the SMPTE ST 2084 curve (DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020); the frame's
//       output texture may then be R10G10B10A2 UNORM (the HDR10 swap chain's format; dithered) or R16G16B16A16 FLOAT.
// paperWhiteNits: paper white's luminance in cd/m2 for encodings 1 and 2 (40 .. 1000; 0: the quality file's
// output.hdr_paper_white_nits). The display's peak is displayPeak x paper white.
UNX_API int32_t UNX_CALL UnxFrameSetDisplayEncoding(UnxRenderer r, int32_t encoding, float paperWhiteNits);

// ---- Weather (optional exports within ABI 6; after commit, any time): UnxFrameSetFog2, UnxFrameSetFogVolumes2,
// UnxFrameSetClouds2, UnxFrameSetWeather, UnxFrameSetLightning, UnxFrameSetPoolWeather. The first three take the frozen descriptions' fields
// (UnxFogDesc, UnxFogVolumeDesc, UnxCloudDesc: the same names, units and limits) and what the renderer's frame has
// gained since; each sets the same state as its first function - the latest call of either decides, and the first
// function leaves the added fields at "none".

// The height fog with its second layer (render::FogDesc::density2): another exponential layer of the same medium - a low
// ground fog under a thin haze -, with its own density, falloff and height; the albedo and the phase function are the
// fog's. density2 0: none (while it rains the rain's veil takes the layer: UnxFrameSetWeather). Null: the quality file's
// atmosphere.fog decides, as UnxFrameSetFog(null).
typedef struct UnxFogDesc2
{
    uint32_t size, version;             // sizeof (64), 1
    float density;                      // extinction (1/m) at 'height', >= 0
    float heightFalloff;                // the density halves every 1 / this metres of height, >= 0
    float height;                       // world metres
    float albedo[3];                    // scattering / extinction, [0, 1]
    float phaseG;                       // Henyey-Greenstein asymmetry, (-1, 1)
    float startDistance;                // m, >= 0
    float skyAmount;                    // [0, 1]
    float noiseAmount;                  // [0, 1]
    float noiseScale;                   // m, >= 1
    float density2;                     // the second layer's extinction (1/m) at 'height2', >= 0 (0: no second layer)
    float heightFalloff2;               // its density halves every 1 / this metres of height, >= 0 (0: uniform in height)
    float height2;                      // world metres
} UnxFogDesc2;
#ifdef __cplusplus
static_assert(sizeof(UnxFogDesc2) == 64, "UnxFogDesc2 is part of the ABI");
#endif
UNX_API int32_t UNX_CALL UnxFrameSetFog2(UnxRenderer r, const UnxFogDesc2* fog);

// Local fog volumes with rising steam and a density grid (render::FogVolumeDesc): the current set, held until changed,
// as UnxFrameSetFogVolumes.
//   steam   sourcePlane: the height inside the volume (0 bottom .. 0.95) the medium rises from - no density under it,
//           heightFalloff counts from it (the water's surface in a volume that reaches under it). riseSpeed: the
//           density's own variation moves up the volume's axis at this speed. turbulence: the variation's share of the
//           density (0.5: from nothing to twice the mean; 1: wisps with gaps), a quarter of it at the source plane and all
//           of it from a third of the height up. turbulenceScale: its largest features. All 0 (turbulenceScale any):
//           the volume as UnxFrameSetFogVolumes gives it.
//   grid    a density grid of the game's own (a simulation, authored wisps): gridSize texels per side (each 1 .. 32),
//           one byte per texel, x fastest then y then z, over the volume's box [-1, 1]^3 along its axes - texel (0, 0, 0)
//           at the corner (-1, -1, -1); read between texels; a texel is the density's factor (255: 1) on everything
//           above. Copied by the call. grid null (gridSize 0, 0, 0): none.
typedef struct UnxFogVolumeDesc2
{
    uint32_t size, version;             // sizeof (112), 1
    double centre[3];                   // world, m
    float halfSize[3];                  // m, > 0
    float yaw;                          // rad, about the up axis
    uint32_t shape;                     // 0 ellipsoid, 1 box
    float density;                      // extinction (1/m) at the volume's bottom (the source plane), >= 0
    float heightFalloff;                // >= 0 (0: uniform)
    float edge;                         // (0, 1]
    float albedo[3];                    // [0, 1]
    float sourcePlane;                  // [0, 0.95]
    float riseSpeed;                    // m/s
    float turbulence;                   // [0, 1]
    float turbulenceScale;              // m, > 0 where turbulence > 0
    uint32_t gridSize[3];               // texels per side, each 1 .. 32 (0, 0, 0 without a grid)
    const uint8_t* grid;                // gridSize[0] x gridSize[1] x gridSize[2] bytes, or null
} UnxFogVolumeDesc2;
#ifdef __cplusplus
static_assert(sizeof(UnxFogVolumeDesc2) == 112, "UnxFogVolumeDesc2 is part of the ABI");
#endif
UNX_API int32_t UNX_CALL UnxFrameSetFogVolumes2(UnxRenderer r, const UnxFogVolumeDesc2* volumes, uint32_t count);

// The cloud layer with the cirrus sheet (render::CloudLayerDesc::cirrus*): thin ice cloud at one altitude far above the
// layer, with its own coverage map and drift, lit by the same sun and sky. A frame may have the sheet without the layer
// (coverage 0, cirrusCoverage > 0). Null, or both coverages 0: no clouds.
typedef struct UnxCloudDesc2
{
    uint32_t size, version;             // sizeof (64), 1
    float coverage;                     // [0, 1] (0: no layer)
    float baseAltitude, topAltitude;    // world metres, base < top
    float sigmaMax, albedo;             // peak extinction (1/m) > 0, single-scattering albedo [0, 1]
    float windX, windZ;                 // m/s
    uint32_t seed;
    float cirrusCoverage;               // [0, 1]: the share of the sheet's map that holds cirrus (0: no sheet)
    float cirrusAltitude;               // world metres, > 0 (9000)
    float cirrusOpticalDepth;           // vertical, where the map is full, > 0 (thin cirrus 0.03 .. 0.3)
    float cirrusWindX, cirrusWindZ;     // m/s: the sheet's own drift
    uint32_t reserved;                  // 0
} UnxCloudDesc2;
#ifdef __cplusplus
static_assert(sizeof(UnxCloudDesc2) == 64, "UnxCloudDesc2 is part of the ABI");
#endif
UNX_API int32_t UNX_CALL UnxFrameSetClouds2(UnxRenderer r, const UnxCloudDesc2* clouds);

// The weather record (render::WeatherFrame; held until changed): the one row the sky, the air and the surfaces read.
//   cloudCover  the cloud layer's coverage in frames whose cloud description has no layer of its own (the layer's other
//               values are then the defaults);
//   rainRate    the rain's veil in the air (extinction 0.248 R^0.67 per km, in the fog's second layer where the frame
//               sets none) and the rain shadow map: what is under cover stays dry;
//   wetness     surfaces exposed to the rain darken and smooth by it (and by the surface state field's wet channel);
//   snowRate, snowDepth  the snow cover on exposed surfaces;
//   rainDirection        the direction the drops fall (the wind tilts it), any length > 0.
// Null: no weather (every value 0).
typedef struct UnxWeatherDesc
{
    uint32_t size, version;             // sizeof (48), 1
    float rainRate;                     // mm/h, >= 0
    float wetness;                      // [0, 1]
    float snowRate;                     // mm/h water equivalent, >= 0
    float snowDepth;                    // m, >= 0
    float cloudCover;                   // [0, 1]
    float rainDirection[3];             // (0, -1, 0): straight down
    uint32_t reserved[2];               // 0
} UnxWeatherDesc;
#ifdef __cplusplus
static_assert(sizeof(UnxWeatherDesc) == 48, "UnxWeatherDesc is part of the ABI");
#endif
UNX_API int32_t UNX_CALL UnxFrameSetWeather(UnxRenderer r, const UnxWeatherDesc* weather);

// A lightning flash (render::LightningDesc), for the next queued frame only (as the debug primitives: a frame that is
// never rendered drops it; call it for every frame of a flash with that frame's intensity): a source in or under the
// cloud layer that lights the cloud around it - in the layer's picture, in the cloud in front of surfaces and in the sky
// the rays read. The light on the ground is a scene light the game places for the flash. Null or intensity 0: no flash.
typedef struct UnxLightningDesc
{
    uint32_t size, version;             // sizeof (56), 1
    double position[3];                 // world, m: the channel's brightest stretch
    float intensity;                    // luminous intensity (cd), the frame's mean over its exposure, >= 0
    float color[3];                     // linear, >= 0 (0.8, 0.87, 1)
    float radius;                       // m, > 0: the lit channel's extent (30)
    uint32_t reserved;                  // 0
} UnxLightningDesc;
#ifdef __cplusplus
static_assert(sizeof(UnxLightningDesc) == 56, "UnxLightningDesc is part of the ABI");
#endif
UNX_API int32_t UNX_CALL UnxFrameSetLightning(UnxRenderer r, const UnxLightningDesc* lightning);

// The basins' weather (render::PoolFrame::steam*, rainExposure; held until changed, like the basins): per basin of the
// set UnxFrameSetPools gave, by its id.
//   steam  over hot water: a local fog volume over the basin (as UnxFogVolumeDesc2's rising steam: from the still
//          surface up to steamHeight, fading toward the rim), lit as the fog is; among the frame's 16 fog volumes, after
//          the ones UnxFrameSetFogVolumes gave. steamDensity 0: none.
//   rain   the share of the weather record's rain (UnxFrameSetWeather: rainRate) that reaches the surface - 1 under the
//          open sky, 0 under a roof (the renderer does not find the roof itself): the drops disturb the surface.
// A basin not named has neither. count 0: none.
typedef struct UnxPoolWeatherDesc
{
    uint32_t size, version;             // sizeof (32), 1
    uint32_t pool;                      // UnxPoolDesc::id
    float steamDensity;                 // extinction (1/m) of the steam at the surface, >= 0
    float steamHeight;                  // m above the still surface, > 0 with steam (1.5)
    float steamRiseSpeed;               // m/s (0.3)
    float steamTurbulence;              // [0, 1] (0.6)
    float rainExposure;                 // [0, 1]
} UnxPoolWeatherDesc;
#ifdef __cplusplus
static_assert(sizeof(UnxPoolWeatherDesc) == 32, "UnxPoolWeatherDesc is part of the ABI");
#endif
UNX_API int32_t UNX_CALL UnxFrameSetPoolWeather(UnxRenderer r, const UnxPoolWeatherDesc* pools, uint32_t count);

#ifdef __cplusplus
}
// The managed bridge (Assets/UnravelNextBridge/Runtime/Native/UnravelNextNative.cs) checks the same sizes at start.
static_assert(sizeof(UnxRendererDesc) == 1040);
static_assert(sizeof(UnxTextureDesc) == 104);
static_assert(sizeof(UnxMaterialDesc) == 236);  // version 5 (A9 thin film); version 4 = 192 (A9 anisotropy); versions 3 and 2 = 184 (A9 layers); version 1 = 152
static_assert(sizeof(UnxSubmesh) == 16);
static_assert(sizeof(UnxMeshDesc) == 160);
static_assert(sizeof(UnxInstanceDesc) == 96);
static_assert(sizeof(UnxLightDesc) == 96);
static_assert(sizeof(UnxEnvironmentDesc) == 152);
static_assert(sizeof(UnxSceneInfo) == 128);
static_assert(sizeof(UnxCameraDesc) == 48);
static_assert(sizeof(UnxFrameDesc) == 96);
static_assert(sizeof(UnxFrameStats) == 48);
static_assert(sizeof(UnxQueueTiming) == 32);
static_assert(sizeof(UnxVideoMemoryInfo) == 72);
static_assert(sizeof(UnxFrameGraphStats) == 128);
static_assert(offsetof(UnxFrameGraphStats, queues) == 64);  // version 1 is the first 64 bytes
static_assert(sizeof(UnxTransformUpdate) == 56);
static_assert(sizeof(UnxPassTiming) == 56);
#endif
