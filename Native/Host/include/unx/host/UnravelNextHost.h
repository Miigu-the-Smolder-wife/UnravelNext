#pragma once
// UnravelNext.dll C ABI (I track). Unity loads the DLL as a native plugin; C# calls these functions through P/Invoke
// (Assets/UnravelNextBridge in the old repository). Rules:
//   - Only fixed-size structs with a leading {size, version} pair, plain integers/floats, and opaque 64-bit handles cross
//     the boundary. No C++ objects, no STL, no exceptions: every function returns UNX_OK or an error code, and
//     UnxLastError() returns the message of the calling thread's last failure.
//   - A struct whose size or version does not match is refused (UNX_ERROR_ABI), so a stale managed side fails loudly.
//   - Render work happens on Unity's submission thread through the plugin event returned by UnxRenderEventFunc().
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
};

#define UNX_ABI_VERSION 2u  // 1: probe; 2: + renderer
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
// jointToModel: 12 floats per joint (row-major 3x4), the skeleton's current pose in model space.
UNX_API int32_t UNX_CALL UnxSceneAddSkeleton(UnxRenderer r, const float* jointToModel, uint32_t jointCount, uint32_t* index);
UNX_API int32_t UNX_CALL UnxSceneAddInstance(UnxRenderer r, const UnxInstanceDesc* desc, uint32_t* index);
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

typedef struct UnxFrameDesc
{
    uint32_t size, version;     // sizeof, 1
    uint64_t frameIndex;
    double time;                // seconds (wind, animation clocks)
    float deltaTime;
    uint32_t outputWidth, outputHeight;
    uint32_t reserved;
    UnxCameraDesc camera;
    uint64_t output;            // ID3D12Resource* of the output (R10G10B10A2 UNORM, random write), Unity leaves it in
                                // UNORDERED_ACCESS for the frame's list and in its own state otherwise
} UnxFrameDesc;

// Per-frame scene changes (after UnxSceneCommit), collected into the next UnxFrameQueue. Transforms and poses are the
// values to show in that frame (the host interpolates between committed World ticks; the renderer keeps the previous
// frame's for motion).
typedef struct UnxTransformUpdate
{
    uint32_t instance, reserved;
    float transform[12];        // object -> world, row-major 3x4: rotation, uniform scale, translation
} UnxTransformUpdate;
UNX_API int32_t UNX_CALL UnxFrameSetTransforms(UnxRenderer r, const UnxTransformUpdate* updates, uint32_t count);
// jointToModel: 12 floats per joint, the skeleton's model-space joints (the palette is jointToModel x inverseBind).
UNX_API int32_t UNX_CALL UnxFrameSetSkeleton(UnxRenderer r, uint32_t skeleton, const float* jointToModel, uint32_t jointCount);
UNX_API int32_t UNX_CALL UnxFrameSetInstanceVisible(UnxRenderer r, uint32_t instance, uint32_t visible);
// Sun of the following frames (time of day): unit direction ground -> sun, lux at the top of the atmosphere.
UNX_API int32_t UNX_CALL UnxFrameSetSun(UnxRenderer r, const float direction[3], float illuminance, const float color[3], float angularRadius);

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
static_assert(sizeof(UnxTransformUpdate) == 56);
static_assert(sizeof(UnxPassTiming) == 56);
#endif
