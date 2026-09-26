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
                            //    UnxViewModelSetPose, UnxViewModelRemove,
                            //    UnxSceneAddBlendShape, UnxSceneSetVertexAnimation, UnxFrameSetMorphs (C4),
                            //    UnxFrameSetOriginShift (C9), UnxSceneReserveRuntime, UnxFrameAddRuntimeMesh,
                            //    UnxFrameRemoveRuntimeMesh, UnxFrameAddRuntimeInstance, UnxFrameRemoveRuntimeInstance,
                            //    UnxFrameSetRuntimeTransforms (C2b), UnxFrameSetTerrainDeformation (C5)
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
};

// INTERFACES_KO.md 8.1 material v1.
typedef struct UnxMaterialDesc
{
    uint32_t size, version;     // sizeof, 1
    uint32_t materialClass;     // UnxMaterialClass
    uint32_t twoSided;
    float baseColor[3];         // linear albedo (dielectric) or f0 (metal)
    float roughness;            // perceptual
    float metallic, specular;   // dielectric f0 = 0.08 * specular
    float alphaCutoff;          // 0 = opaque
    float transmission;         // foliage diffuse transmission
    float emissive[3];          // nits
    float ior;
    uint32_t baseColorTexture, normalTexture, roughMetalTexture, emissiveTexture, occlusionTexture;  // UNX_NONE = none
    uint32_t reserved;
    char name[64];
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
};

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
    uint32_t size, version;     // sizeof, 1
    uint32_t type, castShadow;
    float position[3], intensity;   // candela (point/spot) or luminance nits (area)
    float forward[3], range;
    float right[3], spotInner;
    float color[3], spotOuter;
    float areaSize[2], reserved[2];
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

#ifdef __cplusplus
}
// The managed bridge (Assets/UnravelNextBridge/Runtime/Native/UnravelNextNative.cs) checks the same sizes at start.
static_assert(sizeof(UnxRendererDesc) == 1040);
static_assert(sizeof(UnxTextureDesc) == 104);
static_assert(sizeof(UnxMaterialDesc) == 152);
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
