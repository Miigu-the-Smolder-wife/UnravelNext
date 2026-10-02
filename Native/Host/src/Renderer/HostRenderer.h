#pragma once
// The renderer behind UnravelNext.dll's C ABI (I track): builds a scene::Scene from the host's content, commits it
// (validate, V's cluster builder, GpuScene upload), and records frames through FrameRenderer.
//
// Threads: the host's main thread adds content, commits, and queues frame packets (camera, time, output, per-frame
// scene updates); frames are recorded later on the host's submission thread (Unity's render event), so the main thread
// prepares frame N+1 while N is recorded. The packet is the only thing the two threads share.
//
// Devices (host boundary decision, Docs/Status/I_STATUS_KO.md 1.4): on Unity, the Device is built on Unity's device and
// graphics queue (DeviceOptions::externalDevice / externalGraphicsQueue) and each frame's lists execute through the
// host's ExecuteCommandList, which declares the output texture's state to Unity. Standalone (tests, tools): own device.
#include "unx/fx/MeshParticles.h"
#include "unx/fx/Particles.h"
#include "unx/core/Config.h"
#include "unx/debug/DebugDraw.h"
#include "unx/decal/Decals.h"
#include "unx/decal/SurfaceState.h"
#include "unx/viewmodel/ViewModel.h"
#include "unx/hair/Hair.h"
#include "unx/water/Pool.h"
#include "unx/render/Frame.h"
#include "unx/render/GpuScene.h"
#include "unx/scene/SceneData.h"

#include <array>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_set>
#include <unordered_map>

namespace unx::render
{
class Device;
class ShaderLibrary;
class FrameRenderer;
class RenderGraph;
class GpuProfiler;
} // namespace unx::render

namespace unx::host
{
class GpuBridgeHost;

struct HostRendererOptions
{
    bool standalone = true;                        // own device and queue (tests, tools)
    ID3D12Device* hostDevice = nullptr;            // Unity's device (standalone = false)
    ID3D12CommandQueue* hostQueue = nullptr;       // Unity's graphics queue (standalone = false)
    uint32_t framesInFlight = 2;
    bool debugLayer = false;                       // standalone only: D3D12 debug layer (correctness runs)
    bool gpuValidation = false;                    // standalone only: GPU-based validation (implies debugLayer)
    ID3D12Device* standaloneDevice = nullptr;      // standalone on this device with queues of its own (tests on WARP; no debug layer)
    std::filesystem::path shaderDirectory;
    std::filesystem::path qualityDirectory;
    std::vector<std::string> qualityOverrides;     // "section.key=value" applied after the directory (a game's settings)
    // Particle stream space -> renderer world axis signs (FrameContext::streamAxes): {1, 1, -1} for Unity's World
    // (UnxRendererCreate), identity for native callers that stream in renderer axes.
    float streamAxes[3] = { 1, 1, 1 };
};

struct SceneCommitInfo
{
    uint64_t triangles = 0, clusters = 0;
    double buildMs = 0;
    std::string contentHash;
};

// Shared, immutable once set: the queued packet and the renderer's latest-state record (currentScene) hold the same
// joints without a copy.
struct SkeletonPose
{
    uint32_t skeleton = 0;
    std::shared_ptr<const std::vector<float3x4>> jointToModel;
};

// Character shading of a material (UnxSceneSetCharacterShading): scene::Material's skin, eye and cloth fields, which the
// material description of the ABI does not carry. Defaults are scene::Material's.
struct CharacterShading
{
    float3 subsurfaceMeanFreePath{ 0.00130f, 0.00095f, 0.00067f };
    float subsurfaceLobeMix = 0.85f;
    float2 subsurfaceLobeRoughness{ 0.75f, 1.30f };
    float cloth = 0;
    float eyeIrisRadius = 0, eyeIrisDepth = 0.45f, eyeLimbusWidth = 0.12f, eyeLimbusDarkening = 0.6f, eyePupilScale = 1, eyeIrisConcavity = 1, eyeIor = 1.336f;
    float3 eyeAxis{ 0, 0, 1 };
};

// Material inputs of a material (UnxSceneSetMaterialInputs): scene::Material's fields of that name, which the material
// description of the ABI does not carry. Defaults are scene::Material's.
struct MaterialInputs
{
    float2 uvScale{ 1, 1 }, uvOffset{ 0, 0 };
    float uvRotation = 0;
    uint32_t occlusionUvSet = 0;
    uint32_t detailColorTexture = scene::kNone, detailNormalTexture = scene::kNone;
    float2 detailScale{ 1, 1 }, detailOffset{ 0, 0 };
    uint32_t detailUvSet = 0;
    float detailColorStrength = 1, detailNormalScale = 1;
    uint32_t heightTexture = scene::kNone;
    float heightScale = 0;
    float emissiveScale = 1;
    uint32_t emissiveMaskTexture = scene::kNone;
    bool vertexColorTint = false, vertexAlphaBlend = false, alphaDither = false;
};

// Everything one frame needs, copied on the main thread.
struct FramePacket
{
    uint64_t ticket = 0;
    uint64_t frameIndex = 0;                       // the host's frame number (reports)
    double time = 0;
    float deltaTime = 0;
    uint32_t width = 0, height = 0;
    scene::Camera camera;
    ID3D12Resource* output = nullptr;              // host-owned RGB10A2 (RGBA16F with displayPeak) random-write texture;
                                                   // null standalone
    float displayPeak = 0;                         // FrameContext::displayPeak: 0 SDR, else HDR peak / paper white
    float lensAperture = 0, lensFocus = 0;         // FrameContext::lensAperture / lensFocus (the host's current lens)
    float whiteBalanceKelvin = 0, whiteBalanceTint = 0;  // FrameContext::whiteBalance* (v1.91; 0 = D65)
    // A3 mesh particles (render C): the host's asset -> mesh table when it changed (mesh = committed mesh index, a runtime
    // mesh id with bit 31, or 0xFFFFFFFF = unmapped); resolved on the render thread (fx::meshAssets)
    std::optional<std::vector<std::pair<uint64_t, uint32_t>>> meshAssets;
    std::optional<scene::Sun> sun;                 // changed sun (time of day)
    std::optional<scene::Atmosphere> atmosphere;   // changed atmosphere (weather); the atmosphere track rebuilds its LUTs
    struct Wind
    {
        float3 direction;
        float speed = 0;
    };
    std::optional<Wind> wind;                      // changed scene wind (INTERFACES 6.4 v1.23: memoryless model, endpoint bound)
    uint32_t discontinuity = 0;                    // FrameContext::discontinuity (v1.35): kDiscontinuityRestore | Cut
    uint32_t gpuSimulation = 0;                    // FrameContext::gpuSimulation (v1.35): kGpuSimulation* bits
    std::vector<render::InstanceTransformUpdate> transforms;
    std::vector<SkeletonPose> skeletons;
    std::vector<std::pair<uint32_t, bool>> visibility;
    // C4: blend shape weights and vertex animation time per instance (in order; later ones win).
    struct Morph
    {
        uint32_t instance = 0;
        std::vector<float> weights;
        float time = 0;
    };
    std::vector<Morph> morphs;
    // C9: origin shift applied before this packet's transforms (sum when packets merge; the older packets' transforms
    // are moved into the newer coordinates when they merge).
    float3 originShift{};
    float3 worldOrigin{};  // the host's accumulated origin shift at this packet (FrameContext::worldOrigin)
    // C2b runtime geometry, in call order (ids are the host's; HostRenderer maps them to GPU scene indices).
    struct RuntimeOp
    {
        enum Kind : uint32_t { AddMesh, RemoveMesh, AddInstance, RemoveInstance, Transform, Patch } kind = AddMesh;
        uint32_t id = 0, mesh = 0, flags = 0;  // Patch: id = the terrain tile's scene instance
        float3x4 transform;
        std::shared_ptr<const scene::Mesh> meshData;
        std::shared_ptr<const render::ClusterData> clusters;
        std::shared_ptr<const render::GpuScene::PatchRegion> region;  // Patch (C5)
    };
    std::vector<RuntimeOp> runtime;
    // Scene edits after commit (INTERFACES 6.3 v1.44, GpuScene::setInstances / setMaterials), in the order the host made
    // them: index == the count at that point appends. Applied before this packet's transforms.
    std::vector<std::pair<uint32_t, scene::Instance>> instanceEdits;
    std::vector<std::pair<uint32_t, scene::Material>> materialEdits;
    // Character shading set after commit (setCharacterShading), applied after this packet's material edits.
    std::vector<std::pair<uint32_t, CharacterShading>> characterEdits;
    // Material inputs set after commit (setMaterialInputs), applied after this packet's material edits.
    std::vector<std::pair<uint32_t, MaterialInputs>> inputEdits;
    // A7 surface state field (E's surface::SurfaceField, fed from NativeVfx nv_surface_delta): delta batches in the host's
    // order (each: removed keys, then changed bricks), changed half-lives, and the frame's VFX context time.
    struct SurfaceDelta
    {
        std::vector<surface::BrickInput> changed;
        std::vector<int32_t> removed;  // 3 per key
    };
    std::vector<SurfaceDelta> surfaceDeltas;
    std::optional<std::array<double, surface::kChannels>> surfaceHalfLives;
    double surfaceTime = 0;
    // A15 debug primitives of this frame (E's DrawList records; immediate mode: a frame that is never rendered drops them).
    std::vector<debug::Line> debugLines;
    std::vector<debug::Triangle> debugTriangles;
    std::vector<debug::Glyph> debugGlyphs;
    // A7 projected decals: the host's decal set when it changed (a later snapshot replaces an earlier one).
    std::shared_ptr<const decal::DecalSet> decals;
    // A12 view models (E's viewmodel::ViewModels): the host's operations in call order, replayed on the render thread
    // (the same id allocation as the host's mirror).
    struct ViewModelOp
    {
        enum Kind : uint8_t { Add, SetPose, Remove } kind;
        uint32_t id, instance;
        float3x4 pose;
    };
    std::vector<ViewModelOp> viewModelOps;
    // B10 strand hair (E's hair::HairSystem): the host's operations in call order, replayed on the render thread (the
    // same body ids as the host's mirror), and the frame's time within the latest tick when set.
    struct HairOp
    {
        enum Kind : uint8_t { AddBody, Tick, RemoveBody } kind;
        uint32_t body = 0;
        std::shared_ptr<const hair::BodyDesc> desc;  // AddBody
        std::vector<float3x4> joints;                // Tick: the joints' world transforms at the tick's end
        std::vector<hair::Capsule> capsules;         // Tick: world, at the tick's end
        float3 wind{};
        float dt = 0;
    };
    std::vector<HairOp> hairOps;
    std::optional<float> hairFraction;
    // B8 GPU fluids of this frame (the latest set replaces an earlier one): FrameContext::fluids plus what the GPU bridge
    // admits (the resources' bridge ids and the tick's World stamp).
    struct Fluid
    {
        render::FluidFrame frame;
        uint64_t currentResource = 0, startResource = 0;  // bridge resource ids (0: a standalone device)
    };
    std::shared_ptr<const std::vector<Fluid>> fluids;
    std::array<uint64_t, 6> fluidStamp{};  // NRC_GpuWorldStamp of their tick (world, generation, epoch, tick, branch, phase)
    std::optional<render::OceanFrame> ocean;  // B7: the sea in this frame's coordinates (FrameContext::ocean)
    render::CloudLayerDesc clouds;            // B5: the cloud layer (FrameContext::clouds)
    render::FogDesc fog;                      // the height fog (FrameContext::fog)
    std::vector<render::FogVolumeDesc> fogVolumes;  // local fog volumes (FrameContext::fogVolumes; world coordinates)
    // W2 closed basins (v1.78): the basins every frame takes (this frame's coordinates; the sources pointers are set when
    // the frame is recorded) and this frame's sources (each handed to one frame; a dropped frame's carry into the next).
    std::vector<render::PoolFrame> pools;
    struct PoolSource
    {
        uint32_t pool = 0;
        render::PoolSourceFrame source;  // this frame's coordinates
    };
    std::vector<PoolSource> poolSources;
};

// The render graph of one recorded frame (RenderGraphStats, the fields the host reports).
struct GraphFrameStats
{
    uint32_t livePasses = 0, commandLists = 0, barrierBatches = 0, barriers = 0, crossQueueSyncs = 0, transientResources = 0;
    bool planReused = false;
    uint64_t transientBytesAliased = 0;
    double cpuCompileMs = 0;
    uint32_t sceneRevision = 0;  // GpuScene::revision() the frame recorded with (GI lighting epochs follow it)
};

struct FrameStats
{
    uint64_t frameIndex = UINT64_MAX;              // host frame number the GPU numbers belong to
    double gpuMs = 0, cpuRecordMs = 0, cpuSubmitMs = 0;
    uint32_t passes = 0;
    GraphFrameStats graph;                         // the same frame's render graph
    struct QueueGaps
    {
        uint32_t lists = 0;
        double headMs = 0, tailMs = 0, gapMs = 0;
    } queues[2];                                   // graphics, compute: time outside the passes (profiler list marks)
    std::vector<std::pair<std::string, double>> passMs;  // the same frame's passes in graph order
};

// B11 photo mode (FEATURES_GAME 17): E's GPU reference tracer on the renderer's device renders a snapshot of the scene
// progressively; while it is on, every rendered frame shows its image through M's post chain.
struct PhotoSettings
{
    uint32_t samplesPerPixel = 4096;   // the accumulation's target (both halves together)
    uint32_t halfSamplesPerFrame = 1;  // samples per pixel and half added per rendered frame (the tracer also keeps each
                                       // dispatch near GpuPathTracer::kTargetDispatchMs)
};

struct PhotoStatus
{
    bool active = false;               // a photo is being shown
    uint64_t generation = 0;           // the photoBegin / photoEnd call this status belongs to (1, 2, ...)
    uint32_t width = 0, height = 0;
    uint32_t samples = 0, target = 0;  // samples per pixel done (both halves), the target
    double relMse = -1;                // the image's relMSE against the converged image (halvesRelMse / 4) at the last
                                       // save; -1 before one
    double startSeconds = 0;           // the tracer's creation: snapshot upload, BLAS/TLAS, tables (render thread)
    uint32_t saves = 0;                // saves completed for this generation
    std::string error;                 // the last failure (start or save); empty when none
};

// Executes one of the frame's command lists on the host queue; 'output' is the host texture the list may touch.
using HostExecute = std::function<void(ID3D12CommandList* list, ID3D12Resource* output)>;

class HostRenderer
{
public:
    explicit HostRenderer(const HostRendererOptions& options);
    ~HostRenderer();
    HostRenderer(const HostRenderer&) = delete;
    HostRenderer& operator=(const HostRenderer&) = delete;

    // Scene content (main thread, before commit). The returned index is the element's position in its scene array.
    scene::Scene& scene() { return m_scene; }
    template <typename T>
    uint32_t add(std::vector<T>& list, T value)
    {
        requireOpen();
        list.push_back(std::move(value));
        return (uint32_t)(list.size() - 1);
    }
    // C5 terrain material (before commit): the layers and splat textures of a Terrain-class material (the rest is checked
    // by scene::validate at commit).
    void setTerrainLayers(uint32_t material, uint32_t splat0, uint32_t splat1, const std::vector<scene::TerrainLayer>& layers)
    {
        requireOpen();
        if (material >= m_scene.materials.size() || m_scene.materials[material].cls != scene::MaterialClass::Terrain)
            fail("terrain layers: material %u is not a Terrain-class material", material);
        if (layers.empty() || layers.size() > 8) fail("terrain layers: %zu layers (1..8)", layers.size());
        scene::Material& m = m_scene.materials[material];
        m.terrainSplat[0] = splat0;
        m.terrainSplat[1] = splat1;
        m.terrainLayers = layers;
    }
    // Character shading of a material of the scene (main thread; the values are checked here). Before commit the scene
    // material takes it; after commit it is an edit of the next queued frame. The groups that are not defined on the
    // material's class are ignored (applyCharacter); a later editMaterials of the material keeps it (keepCharacter).
    void setCharacterShading(uint32_t material, const CharacterShading& c);
    // Material inputs of a material of the scene (main thread; the values and the textures' indices and formats are
    // checked here). Before commit the scene material takes them; after commit they are an edit of the next queued frame.
    // Ignored on the Cut and Terrain classes; the dither without an alpha cutoff (applyInputs). A later editMaterials of
    // the material keeps them (keepInputs).
    void setMaterialInputs(uint32_t material, const MaterialInputs& in);
    // A mesh's second uv set and vertex colours (before commit; either may be empty, each as long as the mesh's vertices).
    void setMeshAttributes(uint32_t mesh, std::vector<float2> uv1, std::vector<uint32_t> colors);
    SceneCommitInfo commit();
    // Quality override before commit ("section.key=value", QualityConfig::applyOverride): a game's post terms, for example.
    void overrideQuality(const std::string& assignment);
    bool committed() const { return m_committed; }

    // Per-frame changes (main thread) collected into the next queued frame.
    void setTransforms(std::span<const render::InstanceTransformUpdate> updates);
    void setSkeleton(uint32_t skeleton, std::vector<float3x4> jointToModel);
    uint32_t jointCount(uint32_t skeleton) const;
    void setInstanceVisible(uint32_t instance, bool visible);
    // C4 (before commit): a blend shape or the vertex animation of a mesh; (after commit) an instance's weights and time.
    void addBlendShape(uint32_t mesh, scene::BlendShape shape);
    void setVertexAnimation(uint32_t mesh, scene::VertexAnimation animation);
    void setMorph(uint32_t instance, std::vector<float> weights, float time);
    // C9: the next frame's origin shift (1024 m grid); transforms queued earlier for that frame move with it.
    void setOriginShift(float3 shift);
    uint32_t meshVertexCount(uint32_t mesh) const;
    // C2b runtime geometry (see UnravelNextHost.h).
    void reserveRuntime(const render::RuntimeCapacity& capacity);
    uint32_t addRuntimeMesh(scene::Mesh mesh);
    void removeRuntimeMesh(uint32_t id);
    uint32_t addRuntimeInstance(uint32_t mesh, const float3x4& transform, uint32_t flags);
    void removeRuntimeInstance(uint32_t id);
    void setRuntimeTransform(uint32_t id, const float3x4& transform);
    // C5 terrain deformation D (scene/TerrainPatch.h, UnravelNextHost.h UnxFrameSetTerrainDeformation): the whole current
    // window, world xz of texel (0, 0) in this frame's coordinates; 'tiles' = the cooked terrain tile instances it may
    // touch. Blocks whose inputs changed are rebuilt on the calling thread as runtime meshes; tiles no longer listed lose
    // their patches.
    void setTerrainDeformation(double originX, double originZ, float spacing, uint32_t texels, const float* heights, std::span<const uint32_t> tiles);
    uint32_t blendShapeCount(uint32_t instance) const;  // of the instance's mesh
    // Scene edits after commit (main thread; A2, INTERFACES 6.3 v1.44): each instance at its index takes the new value
    // (index == instanceCount() appends, in order; its transform has no motion and it is visible); skinned instances and
    // new meshes need a new renderer. Materials likewise (their textures must already be in the scene). They reach the
    // GPU scene with the next queued frame; a removal is setInstanceVisible(false), the slot reused by a later edit.
    void editInstances(std::span<const std::pair<uint32_t, scene::Instance>> edits);
    void editMaterials(std::span<const std::pair<uint32_t, scene::Material>> edits);
    uint32_t instanceCount() const;
    uint32_t materialCount() const;
    void setSun(const scene::Sun& sun);
    // History discontinuity and GPU simulation steps of the next queued frame (INTERFACES 5.5.2, v1.35); bits of a
    // dropped packet are ORed into the next one.
    void setDiscontinuity(uint32_t flags);
    // The camera's lens for the following frames (depth of field): aperture diameter (m, 0 = pinhole) and focus distance (m).
    void setLens(float aperture, float focus);
    // The camera's white balance for the following frames (v1.91): the illuminant the camera is set to as a correlated
    // colour temperature (K; 0 = D65, no adaptation; else 1000..40000) and a tint (Duv, |tint| <= 0.1).
    void setWhiteBalance(float kelvin, float tint);
    // A3 mesh particles (render C): the scene mesh a program's mesh_asset draws (committed mesh index or runtime mesh id;
    // 0xFFFFFFFF removes the mapping: its particles are not drawn and counted unmapped)
    void mapMeshAsset(uint64_t asset, uint32_t mesh);
    // A7 surface state (E's SurfaceField): a NativeVfx nv_surface_delta between two publications (changed bricks in the
    // NV_SurfaceBrickV2 layout, removed keys as 3 int32 each), the channel half-lives (s, 0 = no decay; wet, scorch,
    // frost, dust, blood, snow) and the VFX context time of the following frames. Applied in this order on the render
    // thread before the frame they are queued with (a dropped frame's batches go to the next).
    void surfaceDelta(const surface::BrickInput* changed, size_t changedCount, const int32_t* removedKeys, size_t removedCount);
    void setSurfaceHalfLives(const std::array<double, surface::kChannels>& halfLife);
    void setSurfaceTime(double seconds);
    // A15 debug drawing (E's debug::DrawList) for the next queued frame only: primitive records as DebugDraw.h lays them
    // out, and text (printable ASCII) at an anchor.
    void debugPrimitives(std::span<const debug::Line> lines, std::span<const debug::Triangle> triangles, std::span<const debug::Glyph> glyphs);
    void debugText(float3 anchor, std::string_view text, uint32_t color, float sizePx, uint32_t flags, float2 offsetPx);
    // A7 projected decals (E's decal::DecalSet, mirrored here: ids are the set's, the render thread gets a snapshot with
    // the next queued frame after a change). Materials and instances are the host's current counts.
    uint32_t decalAdd(const decal::Decal& d);
    void decalUpdate(uint32_t id, const decal::Decal& d);
    void decalRemove(uint32_t id);
    // A12 first-person view models (E's viewmodel::ViewModels, mirrored here): a scene instance posed in the camera's
    // frame (object -> view space: x right, y up, looking down -z), composed with each rendered frame's camera.
    uint32_t viewModelAdd(uint32_t instance, const float3x4& cameraLocal);
    void viewModelSetPose(uint32_t id, const float3x4& cameraLocal);
    void viewModelRemove(uint32_t id);
    // B10 strand hair (E's hair::HairSystem, mirrored here: a body's id is the system's; validated on the calling thread):
    // a body of guide strands bound to joints, one World tick of it (the joints and capsules at the tick's end, the wind,
    // the tick interval) and the rendered frames' time within the latest tick (0 = the previous tick's end, 1 = the
    // latest's). Order per World step: ticks, then the fraction, then the frame.
    uint32_t hairAddBody(const hair::BodyDesc& desc);
    void hairTick(uint32_t body, std::span<const float3x4> joints, std::span<const hair::Capsule> capsules, float3 wind, float dt);
    void hairSetFrameFraction(float fraction);
    void hairRemoveBody(uint32_t body);
    // B8 GPU fluids (engine 1's shared-mode physics fluids) for the frames queued from now on until the next call: each
    // one's NP_FluidGpuView (96 B) or NP_FluidGpuView2 (136 B, an anchored domain), as np_fluid_gpu_view filled it, the
    // frame's time within their tick and its domain in
    // cells; stamp = the tick's NRC_GpuWorldStamp (6 x 64 bit). An empty list: no fluids.
    struct FluidInput
    {
        const void* view = nullptr;  // NP_FluidGpuView or NP_FluidGpuView2
        float alpha = 1;
        uint32_t domainCells[3] = {};
        uint32_t material = 0;       // the scene material of its surface (Water class)
    };
    void setFluids(std::span<const FluidInput> fluids, const uint64_t (&stamp)[6]);
    // B7 the sea for the frames queued from now on until the next call (null: none). World coordinates; each frame takes
    // it in its own coordinates (level and lake centre less the origin shifts applied by then).
    struct OceanInput
    {
        float windSpeed = 0, windDirection = 0, fetch = 0, spread = 0;
        uint32_t seed = 0;
        double level = 0;
        float horizontalBound = 0, verticalBound = 0;
        bool lake = false;
        double lakeCentre[2] = {};
        float lakeRadius = 0;
    };
    void setOcean(const OceanInput* ocean);
    // W2 closed basins (v1.78): held until the next call (empty: none); world coordinates, each queued frame takes them
    // in its own. Validated here (ids unique and nonzero, sizes, film 0 or 1).
    struct PoolInput
    {
        uint32_t id = 0, material = 0;
        uint32_t shape = 0;  // v1.92: 0 rectangle, 1 round (sizeX = diameter)
        float sizeX = 0, sizeZ = 0, depth = 0, surfaceFilm = 0;
        double centre[3] = {};
        float yaw = 0;
    };
    void setPools(std::span<const PoolInput> pools);
    // Sources for the next queued frame (world coordinates; the basin must be in the current set, the centre inside it).
    void addPoolSources(std::span<const FramePacket::PoolSource> sources);
    // The basins and sources the next queued frame takes, in that frame's coordinates (tests).
    std::pair<std::vector<render::PoolFrame>, std::vector<FramePacket::PoolSource>> queuedPools();
    // The latest completed surface statistics of a basin (water::PoolStats; UnxPoolStatsLatest): false until a record of
    // that basin completed on the GPU (framesInFlight records after its first), or when it is not in the set.
    bool poolStats(uint32_t id, water::PoolStats& out) const;
    // B5 clouds (v1.77): held until changed; every queued frame takes the current layer.
    void setClouds(const render::CloudLayerDesc& clouds);
    // The height fog: held until changed; every queued frame takes the current medium.
    void setFog(const render::FogDesc& fog);
    // Local fog volumes: the current set, held until changed.
    void setFogVolumes(const std::vector<render::FogVolumeDesc>& volumes);
    render::CloudLayerDesc clouds()
    {
        std::lock_guard lock(m_mutex);
        return m_clouds;
    }
    // The sea the next queued frame takes, in that frame's coordinates (tests).
    std::optional<render::OceanFrame> queuedOcean();
    void setSimulation(uint32_t gpuSimulation);
    // Sun, atmosphere and (when set) wind of the following frames.
    void setEnvironment(const scene::Sun& sun, const scene::Atmosphere& atmosphere, std::optional<FramePacket::Wind> wind);
    uint64_t queueFrame(FramePacket packet);

    // Submission thread (Unity's render event): records the queued frame and executes it through 'execute'.
    void renderOnHost(uint64_t ticket, const HostExecute& execute);
    // Standalone: records and executes on the renderer's own queue into its own output; optional blocking readback of
    // the RGB10A2 pixels.
    void renderStandalone(uint64_t ticket, void* readback, size_t readbackBytes);
    FrameStats latestStats() const;
    // Test hook: the main view's EV100 of the last recorded frame (automatic exposure's choice when the camera asked).
    float lastEv100ForTest() const;
    // The committed renderer's track state (standalone tests: E's decal set; the host ABI for decals is E's).
    render::TrackState& trackStateForTest();
    // Debug-layer errors reported so far (standalone renderers with debugLayer; 0 otherwise).
    uint32_t debugErrors();
    // Test hook (standalone): every following frame gets a newly created output texture, the old one released after the
    // GPU finishes with it, as a host recreates its render target on resize. The allocator tends to hand the new texture
    // the old one's address, so views cached by resource pointer would point at a destroyed texture.
    void setRecreateStandaloneOutput(bool recreate) { m_recreateOutput = recreate; }
    uint32_t outputAddressReuses() const { return m_outputReuses; }
    // Test hooks: the flags of the last recorded frame, and the GPU scene's CPU mirror of an instance.
    std::pair<uint32_t, uint32_t> lastFrameFlags() const { return { m_lastDiscontinuity, m_lastGpuSimulation }; }
    const render::gpu::Instance& gpuInstanceForTest(uint32_t instance) const { return m_gpuScene->instances().at(instance); }
    // The D3D12 device the renderer records on (Unity's inside Unity).
    ID3D12Device* d3dDevice() const;
    // Engine 1's shared GPU bridge on this renderer's device (src/GpuBridge; NativePhysics / NativeVfx GPU work runs on its
    // queues, graphics reads of their buffers go through prepareGraphics / commitGraphics). One per renderer device.
    GpuBridgeHost& gpuBridge();
    // Test hook: transform and pose updates that matched the GPU scene bit for bit and were not applied (beginFrame).
    std::pair<uint64_t, uint64_t> droppedUpdatesForTest() const { return { m_droppedTransforms, m_droppedPoses }; }
    // Test hook (C5): terrain patch blocks built so far.
    uint64_t patchBuildsForTest() const { return m_patchBuilds; }
    // Test hook: removes this renderer's D3D12 device (ID3D12Device5::RemoveDevice: this process only, no GPU reset), as a
    // TDR would, so the device-removal path can be exercised.
    void removeDeviceForTest();
    uint32_t outputRecreations() const { return m_outputRecreations; }

    // V3 (WORLD_VFX 3.7 (f), I track): the renderer's FX particle module as the VFX stream executor (NV_StreamExecutor),
    // called on the host's main thread. submit keeps the committed packet; the next frame's C0 records it on the frame's
    // queue. readback and checkpoint first claim the ticks no frame has recorded yet (two fixed steps in one Unity frame:
    // the second step's prepare reads the first) and run them at once on the device's compute queue, after the last
    // submitted frame on the GPU; the next frame waits for them on the GPU. So every tick runs exactly once and the main
    // thread never waits for the render thread. Every use of the module, frames included, holds m_fxMutex.
    void vfxSubmit(const uint8_t* packet, uint64_t bytes);
    const fx::TickReadback& vfxReadback(uint64_t stream, uint64_t generation, uint64_t tick);  // valid until the next call
    const std::vector<NV_StreamParticle>& vfxCheckpoint(uint64_t stream, uint64_t generation, uint64_t tick);
    // executor version 4: the orientations of the last vfxCheckpoint (same records, same order)
    const std::vector<NV_StreamParticleOrientation>& vfxCheckpointOrientations(uint64_t stream, uint64_t generation, uint64_t tick);
    // Ticks run at once on the compute queue (claimed by a readback or checkpoint) so far (tests, statistics).
    uint64_t vfxImmediateTicks() const { return m_vfxImmediateTicks; }

    // The scene as the host shows it now: the content with the latest transforms, poses, sun and visibility the host
    // set (rendered, queued and pending updates, newest last); hidden instances are left out. Main thread (UnxSceneSave).
    scene::Scene currentScene() const;
    // Photo mode's snapshot (main thread): currentScene() with what the frames draw beyond the committed content - the
    // C2b runtime meshes and instances, the C5 terrain patches (a patched tile's replaced triangles left out, V's centroid
    // rule) and the A12 view models at the host's latest frame camera.
    scene::Scene photoScene() const;

    // B11 photo mode (main thread; FEATURES_GAME 17). photoBegin snapshots the scene now (photoScene), with 'camera' (an
    // EV100 that is not finite takes automatic exposure's last choice) and the host's lens, and every frame rendered
    // after it shows the tracer's image at the frame's output size (a new size restarts the accumulation) through M's
    // post chain. Calling it again restarts with a new snapshot and camera (a moved photo camera). photoSave writes the
    // image when the next frame renders: 'exr' (optional) the linear radiance x exposure (metrics::writeExr), 'png'
    // (optional) the post chain's SDR encoding as 16-bit RGB. photoEnd returns to the scene's frames.
    void photoBegin(const scene::Camera& camera, const PhotoSettings& settings);
    void photoSave(std::filesystem::path exr, std::filesystem::path png);
    void photoEnd();
    PhotoStatus photoStatus() const;

    const HostRendererOptions& options() const { return m_options; }
    const QualityConfig& quality() const { return m_quality; }

private:
    void requireOpen() const;
    void requireCommitted() const;
    std::optional<FramePacket> takePacket(uint64_t ticket);
    // Per-instance and per-skeleton values the host set (poses shared with the packets, not copied).
    struct HostState
    {
        std::vector<float3x4> transforms;
        std::vector<std::shared_ptr<const std::vector<float3x4>>> poses;
        std::vector<uint8_t> visible;
        std::vector<std::pair<std::vector<float>, float>> morphs;  // C4: per instance (empty weights: not set)
        scene::Sun sun;
        scene::Atmosphere atmosphere;
        FramePacket::Wind wind;
    };
    static void overlay(const FramePacket& p, HostState& state);
    static void applyEdits(const FramePacket& p, scene::Scene& s);
    static void applyCharacter(const CharacterShading& c, scene::Material& m);
    static void keepCharacter(const scene::Material& old, scene::Material& next);
    static void applyInputs(const MaterialInputs& in, scene::Material& m);
    static void keepInputs(const scene::Material& old, scene::Material& next);
    void ensureStandaloneOutput(uint32_t width, uint32_t height, DXGI_FORMAT format);
    // Paces the frame slot, applies the packet's scene updates, declares the frame; returns the frame slot.
    uint32_t beginFrame(const FramePacket& packet);
    void recordFrame(const FramePacket& packet, render::TextureRef output);
    void endFrame(uint32_t slot, uint64_t hostFrameIndex);
    fx::ParticleSystem& fxModule();  // (m_fxMutex held) created on first use, explicit copies on the compute queue
    void fxRunPending();              // (m_fxMutex held) the claimed ticks on the compute queue
    void fxFrameWait();               // (m_fxMutex held, frame submission) the graphics queue waits for them
    scene::Scene snapshot(bool photo) const;
    // Submission thread: photo mode's part of a frame. photoFrame records it (false: no photo, render the scene);
    // photoAfterExecute finishes a save once the frame's lists were submitted.
    bool photoFrame(render::FrameContext& fc, const FramePacket& p, render::TextureRef output);
    void photoAfterExecute();
    // Submission thread: B8 fluids - the GPU bridge admits the frame's reads (queue waits on the graphics queue) before its
    // lists run, and takes the frame's fence after them.
    void fluidsBeforeExecute(const FramePacket& p);
    void fluidsAfterExecute();

    HostRendererOptions m_options;
    QualityConfig m_quality;
    scene::Scene m_scene;
    bool m_committed = false;
    std::unique_ptr<render::Device> m_device;
    std::unique_ptr<render::ShaderLibrary> m_shaders;
    std::unique_ptr<render::GpuScene> m_gpuScene;
    std::unique_ptr<render::FrameRenderer> m_frameRenderer;
    std::unique_ptr<render::RenderGraph> m_graph;
    std::unique_ptr<render::GpuProfiler> m_profiler;
    std::shared_ptr<GpuBridgeHost> m_gpuBridge;  // quiesced and released before the device

    mutable std::mutex m_mutex;  // packets, pending updates, stats
    std::deque<FramePacket> m_packets;
    FramePacket m_pending;       // updates for the next queued frame
    // C2b: host ids (main thread) and their GPU scene indices (render thread, at apply time).
    render::RuntimeCapacity m_runtimeCapacity;
    uint32_t m_nextRuntimeMesh = 0, m_nextRuntimeInstance = 0;
    std::unordered_map<uint32_t, uint32_t> m_runtimeMeshIndex, m_runtimeInstanceIndex;
    std::unordered_set<uint32_t> m_runtimeMeshLive;                  // main-thread view (validation)
    std::unordered_map<uint32_t, uint32_t> m_runtimeInstanceMesh;    // live runtime instance id -> its mesh (validation)
    // B11: the host's live runtime geometry (m_mutex; photo snapshots): mesh data by id, instance placement by id (in
    // the host's current coordinates: origin shifts move them).
    std::unordered_map<uint32_t, std::shared_ptr<const scene::Mesh>> m_runtimeMeshData;
    struct RuntimePlacement
    {
        float3x4 transform;
        uint32_t flags = 0;
    };
    std::unordered_map<uint32_t, RuntimePlacement> m_runtimeInstancePlacement;
    std::unordered_map<uint32_t, std::shared_ptr<const render::GpuScene::PatchRegion>> m_patchRegions;  // (m_mutex) per tile
    scene::Camera m_lastCamera;                  // (m_mutex) the camera of the latest queued frame (view models)
    double m_lastTime = 0;                       // (m_mutex) its time
    bool m_haveLastCamera = false;
    // C5 terrain patches (main thread): per tile instance, its replaced blocks' runtime mesh / instance and input hash.
    struct PatchBlock
    {
        uint32_t mesh = 0, instance = 0;
        uint64_t hash = 0;
    };
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, PatchBlock>> m_terrainPatches;
    float3 m_mainOriginOffset{};
    uint64_t m_patchBuilds = 0;  // sum of setOriginShift calls (main thread): committed transforms minus this = now
    uint32_t m_hostInstances = 0, m_hostMaterials = 0;  // counts with every edit the host made (m_mutex)
    // Each material's class with every edit the host made (m_mutex): setFluids checks a material appended or changed by an
    // edit, which m_scene (the committed scene) does not hold (engine 1's D0 finding, 2026-09-27).
    std::vector<scene::MaterialClass> m_hostMaterialClasses;
    std::vector<uint8_t> m_hostSkinned;                 // per instance, with the host's edits (m_mutex)
    // Latest state of every packet taken for rendering (takePacket, under m_mutex then m_appliedMutex): with the queued
    // packets and m_pending on top it is the host's current scene. m_appliedMutex also guards m_scene.sun,
    // m_scene.atmosphere and the scene wind, which the submission thread writes (the renderer reads them from
    // GpuScene::source()).
    mutable std::mutex m_appliedMutex;
    HostState m_applied;
    uint64_t m_nextTicket = 1;
    FrameStats m_stats;

    // Submission thread only.
    std::vector<std::array<uint64_t, 3>> m_slotFence;
    std::vector<uint64_t> m_slotHostFrame;
    std::vector<GraphFrameStats> m_slotGraph;  // the render graph of the frame recorded in each slot
    uint64_t m_recordedFrames = 0;
    float4x4 m_prevViewProj{};
    bool m_havePrev = false;

    struct Standalone;
    std::unique_ptr<Standalone> m_standalone;
    bool m_recreateOutput = false;
    uint32_t m_lastDiscontinuity = 0, m_lastGpuSimulation = 0;  // submission thread (test hook)
    // What the GPU scene last received per instance and per skeleton (submission thread): bit-identical updates stop here.
    std::vector<float3x4> m_gpuTransforms;
    std::vector<std::shared_ptr<const std::vector<float3x4>>> m_gpuPoses;
    std::vector<render::InstanceTransformUpdate> m_changedTransforms;
    uint64_t m_droppedTransforms = 0, m_droppedPoses = 0;
    uint32_t m_outputReuses = 0, m_outputRecreations = 0;
    // V3 stream executor state.
    std::mutex m_fxMutex;
    uint64_t m_fxRecorded = 0;  // (m_fxMutex held) packets written under UNX_FX_RECORD
    float m_lensAperture = 0, m_lensFocus = 0;  // (m_mutex) the lens every queued frame takes
    float m_whiteBalanceKelvin = 0, m_whiteBalanceTint = 0;  // (m_mutex) the white balance every queued frame takes (v1.91)
    std::map<uint64_t, uint32_t> m_meshAssetMap;  // (m_mutex) A3 mesh particles: asset -> mesh
    bool m_meshAssetsChanged = false;             // (m_mutex)
    std::vector<std::pair<uint64_t, uint32_t>> m_meshAssetsRender;  // render thread: the latest table received
    double m_surfaceTime = 0;                   // (m_mutex) the VFX time every queued frame takes
    decal::DecalSet m_decals;                   // (m_mutex) the host's decal set
    std::vector<uint8_t> m_decalLive;           // (m_mutex) per decal id: live
    bool m_decalsChanged = false;               // (m_mutex) a snapshot goes with the next queued frame
    viewmodel::ViewModels m_viewModels;         // (m_mutex) the host's mirror (ids, live entries)
    std::vector<uint32_t> m_hairJoints;         // (m_mutex) per hair body id: its joint count (0 = free)
    std::vector<uint32_t> m_hairFree;           // (m_mutex) free ids, reused last-freed first (HairSystem's rule)
    std::shared_ptr<const std::vector<FramePacket::Fluid>> m_fluids;  // (m_mutex) the fluids every queued frame takes
    std::array<uint64_t, 6> m_fluidStamp{};                          // (m_mutex)
    std::optional<OceanInput> m_ocean;                               // (m_mutex) the sea every queued frame takes
    render::FogDesc m_fog;                                           // (m_mutex) the height fog every queued frame takes
    std::vector<render::FogVolumeDesc> m_fogVolumes;                 // (m_mutex) the local fog volumes every queued frame takes
    render::CloudLayerDesc m_clouds;                                 // (m_mutex) B5 the cloud layer every queued frame takes
    std::optional<render::OceanFrame> oceanFrameLocked() const;      // (m_mutex held) m_ocean in the current coordinates
    std::vector<PoolInput> m_pools;                                  // (m_mutex) W2 basins every queued frame takes (world)
    std::vector<std::pair<uint32_t, water::PoolStats>> m_poolStats;  // (m_mutex) their latest statistics, copied after each record
    std::vector<FramePacket::PoolSource> m_pendingPoolSources;       // (m_mutex) for the next queued frame (world)
    void poolsLocked(FramePacket& packet);                           // (m_mutex held) basins and sources into the packet's coordinates
    std::vector<render::PoolFrame> m_poolFrames;                     // submission thread: FrameContext::pools of the frame being recorded
    std::vector<render::PoolSourceFrame> m_poolSourceFrames;         // submission thread: their sources, grouped by basin
    uint64_t m_fluidTicket = 0;                  // submission thread: the frame's bridge admission (0: none)
    std::vector<render::FluidFrame> m_fluidFrames;  // submission thread: FrameContext::fluids of the frame being recorded
    std::unique_ptr<render::RenderGraph> m_simGraph;  // the claimed ticks' graph (compute queue)
    uint64_t m_simIndex = 1ull << 48;                // its import index (apart from frame indices)
    uint64_t m_simFence = 0, m_simWaited = 0;        // compute fence of the last claimed tick; the frames waited up to
    uint64_t m_vfxImmediateTicks = 0;
    std::string m_vfxError;                          // a submit that failed (reported by the next readback/checkpoint)
    fx::TickReadback m_vfxReadback;
    std::vector<NV_StreamParticle> m_vfxCheckpoint;
    std::vector<NV_StreamParticleOrientation> m_vfxCheckpointOrientations;
    // B11 photo mode: the main thread's request (m_photoMutex) and the submission thread's tracer.
    struct PhotoRequest
    {
        scene::Scene scene;
        scene::Camera camera;
        float lensAperture = 0, lensFocus = 0;
        float whiteBalanceKelvin = 0, whiteBalanceTint = 0;
        double time = 0;
        PhotoSettings settings;
    };
    struct PhotoSaveRequest
    {
        std::filesystem::path exr, png;
    };
    struct Photo;
    mutable std::mutex m_photoMutex;
    std::shared_ptr<const PhotoRequest> m_photoRequest;  // null: off
    uint64_t m_photoGeneration = 0;
    std::vector<PhotoSaveRequest> m_photoSaves;
    PhotoStatus m_photoStatus;
    std::unique_ptr<Photo> m_photo;  // submission thread
};
} // namespace unx::host
