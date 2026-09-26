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
#include "unx/fx/Particles.h"
#include "unx/core/Config.h"
#include "unx/render/Frame.h"
#include "unx/render/GpuScene.h"
#include "unx/scene/SceneData.h"

#include <array>
#include <deque>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

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
struct HostRendererOptions
{
    bool standalone = true;                        // own device and queue (tests, tools)
    ID3D12Device* hostDevice = nullptr;            // Unity's device (standalone = false)
    ID3D12CommandQueue* hostQueue = nullptr;       // Unity's graphics queue (standalone = false)
    uint32_t framesInFlight = 2;
    bool debugLayer = false;                       // standalone only: D3D12 debug layer (correctness runs)
    bool gpuValidation = false;                    // standalone only: GPU-based validation (implies debugLayer)
    std::filesystem::path shaderDirectory;
    std::filesystem::path qualityDirectory;
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

// Everything one frame needs, copied on the main thread.
struct FramePacket
{
    uint64_t ticket = 0;
    uint64_t frameIndex = 0;                       // the host's frame number (reports)
    double time = 0;
    float deltaTime = 0;
    uint32_t width = 0, height = 0;
    scene::Camera camera;
    ID3D12Resource* output = nullptr;              // host-owned RGB10A2 random-write texture; null standalone
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
    // Scene edits after commit (INTERFACES 6.3 v1.44, GpuScene::setInstances / setMaterials), in the order the host made
    // them: index == the count at that point appends. Applied before this packet's transforms.
    std::vector<std::pair<uint32_t, scene::Instance>> instanceEdits;
    std::vector<std::pair<uint32_t, scene::Material>> materialEdits;
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
    SceneCommitInfo commit();
    bool committed() const { return m_committed; }

    // Per-frame changes (main thread) collected into the next queued frame.
    void setTransforms(std::span<const render::InstanceTransformUpdate> updates);
    void setSkeleton(uint32_t skeleton, std::vector<float3x4> jointToModel);
    uint32_t jointCount(uint32_t skeleton) const;
    void setInstanceVisible(uint32_t instance, bool visible);
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
    // Test hook: transform and pose updates that matched the GPU scene bit for bit and were not applied (beginFrame).
    std::pair<uint64_t, uint64_t> droppedUpdatesForTest() const { return { m_droppedTransforms, m_droppedPoses }; }
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
    // Ticks run at once on the compute queue (claimed by a readback or checkpoint) so far (tests, statistics).
    uint64_t vfxImmediateTicks() const { return m_vfxImmediateTicks; }

    // The scene as the host shows it now: the content with the latest transforms, poses, sun and visibility the host
    // set (rendered, queued and pending updates, newest last); hidden instances are left out. Main thread (UnxSceneSave).
    scene::Scene currentScene() const;

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
        scene::Sun sun;
        scene::Atmosphere atmosphere;
        FramePacket::Wind wind;
    };
    static void overlay(const FramePacket& p, HostState& state);
    static void applyEdits(const FramePacket& p, scene::Scene& s);
    void ensureStandaloneOutput(uint32_t width, uint32_t height);
    // Paces the frame slot, applies the packet's scene updates, declares the frame; returns the frame slot.
    uint32_t beginFrame(const FramePacket& packet);
    void recordFrame(const FramePacket& packet, render::TextureRef output);
    void endFrame(uint32_t slot, uint64_t hostFrameIndex);
    fx::ParticleSystem& fxModule();  // (m_fxMutex held) created on first use, explicit copies on the compute queue
    void fxRunPending();              // (m_fxMutex held) the claimed ticks on the compute queue
    void fxFrameWait();               // (m_fxMutex held, frame submission) the graphics queue waits for them

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

    mutable std::mutex m_mutex;  // packets, pending updates, stats
    std::deque<FramePacket> m_packets;
    FramePacket m_pending;       // updates for the next queued frame
    uint32_t m_hostInstances = 0, m_hostMaterials = 0;  // counts with every edit the host made (m_mutex)
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
    std::unique_ptr<render::RenderGraph> m_simGraph;  // the claimed ticks' graph (compute queue)
    uint64_t m_simIndex = 1ull << 48;                // its import index (apart from frame indices)
    uint64_t m_simFence = 0, m_simWaited = 0;        // compute fence of the last claimed tick; the frames waited up to
    uint64_t m_vfxImmediateTicks = 0;
    std::string m_vfxError;                          // a submit that failed (reported by the next readback/checkpoint)
    fx::TickReadback m_vfxReadback;
    std::vector<NV_StreamParticle> m_vfxCheckpoint;
};
} // namespace unx::host
