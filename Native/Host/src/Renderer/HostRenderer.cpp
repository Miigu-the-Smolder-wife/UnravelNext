#include "Renderer/HostRenderer.h"

#include "unx/render/Device.h"
#include "unx/render/FrameRenderer.h"
#include "unx/render/GpuProfiler.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"

#if __has_include("unx/clusterbuilder/ClusterBuilder.h")
#include "unx/clusterbuilder/ClusterBuilder.h"
#define UNX_HOST_HAS_CLUSTERBUILDER 1
#endif

#if __has_include("unx/cook/TextureCook.h")
#include "unx/cook/TextureCook.h"
#include "unx/material/TextureSystem.h"
#define UNX_HOST_HAS_COOK 1
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

namespace unx::host
{
using namespace unx::render;

struct HostRenderer::Standalone
{
    ComPtr<ID3D12Resource> output, readback;
    uint32_t width = 0, height = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    uint64_t readbackBytes = 0;
};

HostRenderer::HostRenderer(const HostRendererOptions& options) : m_options(options)
{
#if UNX_HOST_HAS_COOK
    // C1: textures are uploaded in their cooked form (block-compressed within the error bound, memory and disk caches under
    // UNX_COOK_CACHE, which the Unity editor sets to Library/UnravelNextCook).
    material::setChainProvider(&cook::textureChain);
#endif
    if (options.framesInFlight == 0) fail("framesInFlight must be at least 1");
    if (!options.standalone && (!options.hostDevice || !options.hostQueue)) fail("a host renderer needs the host's device and graphics queue");
    m_quality = QualityConfig::loadDirectory(options.qualityDirectory);
    DeviceOptions d;
    if (options.standalone)
    {
        d.debugLayer = options.debugLayer;
        d.gpuValidation = options.gpuValidation;
    }
    else
    {
        if (options.debugLayer || options.gpuValidation) fail("the debug layer is for standalone renderers (enabling it removes the host's device)");
        d.externalDevice = options.hostDevice;
        d.externalGraphicsQueue = options.hostQueue;
    }
    m_device = std::make_unique<Device>(d);
    m_shaders = std::make_unique<ShaderLibrary>(*m_device, options.shaderDirectory);
    m_slotFence.assign(options.framesInFlight, std::array<uint64_t, 3>{});
    m_slotHostFrame.assign(options.framesInFlight, UINT64_MAX);
    m_slotGraph.assign(options.framesInFlight, {});
    m_standalone = std::make_unique<Standalone>();
}

HostRenderer::~HostRenderer()
{
    if (m_device) m_device->waitIdle();
    m_profiler.reset();
    m_graph.reset();
    m_frameRenderer.reset();
    m_gpuScene.reset();
    m_standalone.reset();
}

void HostRenderer::requireOpen() const
{
    if (m_committed) fail("the scene is committed; content can no longer be added");
}

void HostRenderer::requireCommitted() const
{
    if (!m_committed) fail("commit the scene first");
}

SceneCommitInfo HostRenderer::commit()
{
    requireOpen();
    const auto t0 = std::chrono::steady_clock::now();
    scene::validate(m_scene);
    SceneCommitInfo info;
    for (const scene::Mesh& m : m_scene.meshes) info.triangles += m.indices.size() / 3;
#if UNX_HOST_HAS_CLUSTERBUILDER
    ClusterData clusters = clusterbuilder::build(m_scene, clusterbuilder::Settings::fromQuality(m_quality));
    info.clusters = clusters.clusters.size();
#else
    fail("this build has no cluster builder (track V): build with Tools/CI/Build.ps1 -Track I");
#endif
    m_gpuScene = std::make_unique<GpuScene>(*m_device);
    m_gpuScene->upload(m_scene);
    m_gpuScene->setClusters(std::move(clusters));
    m_frameRenderer = std::make_unique<FrameRenderer>(*m_device, *m_shaders, m_quality, *m_gpuScene, m_options.framesInFlight);
    m_graph = std::make_unique<RenderGraph>(*m_device);
    m_profiler = std::make_unique<GpuProfiler>(*m_device, m_options.framesInFlight, 1024);
    m_committed = true;
    for (const scene::Instance& i : m_scene.instances) m_applied.transforms.push_back(i.transform);
    for (const scene::Skeleton& k : m_scene.skeletons) m_applied.poses.push_back(std::make_shared<const std::vector<float3x4>>(k.jointToModel));
    m_applied.visible.assign(m_scene.instances.size(), 1);
    m_applied.sun = m_scene.sun;
    m_applied.atmosphere = m_scene.atmosphere;
    m_applied.wind = { m_scene.windDirection, m_scene.windSpeed };
    m_gpuTransforms = m_applied.transforms;
    m_gpuPoses = m_applied.poses;
    m_hostInstances = (uint32_t)m_scene.instances.size();
    m_hostMaterials = (uint32_t)m_scene.materials.size();
    for (const scene::Instance& i : m_scene.instances) m_hostSkinned.push_back((i.flags & scene::InstanceSkinned) ? 1 : 0);
    info.contentHash = scene::contentHash(m_scene);
    info.buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    logf("UnravelNext host: scene committed, %zu meshes, %zu instances, %llu triangles, %llu clusters, %.1f ms, hash %s\n", m_scene.meshes.size(),
         m_scene.instances.size(), (unsigned long long)info.triangles, (unsigned long long)info.clusters, info.buildMs, info.contentHash.substr(0, 16).c_str());
    return info;
}

void HostRenderer::setTransforms(std::span<const InstanceTransformUpdate> updates)
{
    requireCommitted();
    std::lock_guard lock(m_mutex);
    for (const InstanceTransformUpdate& u : updates)
        if (u.instance >= m_hostInstances) fail("transform update for instance %u of %u", u.instance, m_hostInstances);
    m_pending.transforms.insert(m_pending.transforms.end(), updates.begin(), updates.end());
}

uint32_t HostRenderer::instanceCount() const
{
    std::lock_guard lock(m_mutex);
    return m_hostInstances;
}

uint32_t HostRenderer::materialCount() const
{
    std::lock_guard lock(m_mutex);
    return m_hostMaterials;
}

void HostRenderer::editInstances(std::span<const std::pair<uint32_t, scene::Instance>> edits)
{
    requireCommitted();
    std::lock_guard lock(m_mutex);
    // Meshes and textures are fixed after commit, so they can be read here; instances and materials change on the
    // submission thread, so the host's own counts decide.
    uint32_t count = m_hostInstances;
    for (const auto& [i, inst] : edits)
    {
        if (i > count) fail("instance edit %u of %u (append in order)", i, count);
        if (inst.mesh >= m_scene.meshes.size()) fail("instance edit %u: mesh %u of %zu (a new mesh needs a new renderer)", i, inst.mesh, m_scene.meshes.size());
        if (inst.flags & scene::InstanceSkinned) fail("instance edit %u: a skinned instance needs a new renderer (its palette slot is fixed at commit)", i);
        if (i < count && i < m_hostSkinned.size() && m_hostSkinned[i]) fail("instance edit %u replaces a skinned instance", i);
        const size_t submeshes = m_scene.meshes[inst.mesh].submeshes.size();
        if (!inst.materialOverrides.empty() && inst.materialOverrides.size() != submeshes)
            fail("instance edit %u: %zu material overrides for %zu submeshes", i, inst.materialOverrides.size(), submeshes);
        for (uint32_t m : inst.materialOverrides)
            if (m >= m_hostMaterials) fail("instance edit %u: material %u of %u", i, m, m_hostMaterials);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                if (!std::isfinite(inst.transform.m[r][c])) fail("instance edit %u: transform is not finite", i);
        if (i == count) ++count;
    }
    for (const auto& e : edits)
    {
        if (e.first == m_hostInstances)
        {
            ++m_hostInstances;
            m_hostSkinned.push_back(0);
        }
        m_pending.instanceEdits.push_back(e);
    }
}

void HostRenderer::editMaterials(std::span<const std::pair<uint32_t, scene::Material>> edits)
{
    requireCommitted();
    std::lock_guard lock(m_mutex);
    uint32_t count = m_hostMaterials;
    const uint32_t textures = (uint32_t)m_scene.textures.size();
    for (const auto& [i, m] : edits)
    {
        if (i > count) fail("material edit %u of %u (append in order)", i, count);
        for (uint32_t t : { m.baseColorTexture, m.normalTexture, m.roughMetalTexture, m.emissiveTexture, m.occlusionTexture })
            if (t != scene::kNone && t >= textures) fail("material edit %u: texture %u of %u (textures are fixed at commit)", i, t, textures);
        if (i == count) ++count;
    }
    for (const auto& e : edits)
    {
        if (e.first == m_hostMaterials) ++m_hostMaterials;
        m_pending.materialEdits.push_back(e);
    }
}

void HostRenderer::applyEdits(const FramePacket& p, scene::Scene& s)
{
    for (const auto& [i, m] : p.materialEdits)
        if (i == s.materials.size()) s.materials.push_back(m);
        else s.materials[i] = m;
    for (const auto& [i, inst] : p.instanceEdits)
        if (i == s.instances.size()) s.instances.push_back(inst);
        else s.instances[i] = inst;
}

void HostRenderer::setSkeleton(uint32_t skeleton, std::vector<float3x4> jointToModel)
{
    requireCommitted();
    if (skeleton >= m_scene.skeletons.size()) fail("skeleton %u of %zu", skeleton, m_scene.skeletons.size());
    if (jointToModel.size() != m_scene.skeletons[skeleton].jointToModel.size())
        fail("skeleton %u has %zu joints, pose has %zu", skeleton, m_scene.skeletons[skeleton].jointToModel.size(), jointToModel.size());
    auto pose = std::make_shared<const std::vector<float3x4>>(std::move(jointToModel));
    std::lock_guard lock(m_mutex);
    m_pending.skeletons.push_back({ skeleton, std::move(pose) });
}

uint32_t HostRenderer::jointCount(uint32_t skeleton) const
{
    if (skeleton >= m_scene.skeletons.size()) fail("skeleton %u of %zu", skeleton, m_scene.skeletons.size());
    return (uint32_t)m_scene.skeletons[skeleton].jointToModel.size();
}

void HostRenderer::overlay(const FramePacket& p, HostState& state)
{
    if (p.sun) state.sun = *p.sun;
    if (p.atmosphere) state.atmosphere = *p.atmosphere;
    if (p.wind) state.wind = *p.wind;
    for (const auto& [i, inst] : p.instanceEdits)  // before the packet's transforms and visibility
    {
        if (i == state.transforms.size())
        {
            state.transforms.push_back(inst.transform);
            state.visible.push_back(1);
        }
        state.transforms[i] = inst.transform;
        state.visible[i] = 1;
    }
    for (const InstanceTransformUpdate& u : p.transforms) state.transforms[u.instance] = u.objectToWorld;
    for (const SkeletonPose& s : p.skeletons) state.poses[s.skeleton] = s.jointToModel;
    for (const auto& [instance, visible] : p.visibility) state.visible[instance] = visible ? 1 : 0;
}

scene::Scene HostRenderer::currentScene() const
{
    requireCommitted();
    scene::Scene s;
    HostState state;
    {
        std::lock_guard lock(m_mutex);
        {
            std::lock_guard applied(m_appliedMutex);
            s = m_scene;
            state = m_applied;
        }
        for (const FramePacket& p : m_packets)
        {
            applyEdits(p, s);
            overlay(p, state);
        }
        applyEdits(m_pending, s);
        overlay(m_pending, state);
    }
    s.sun = state.sun;
    s.atmosphere = state.atmosphere;
    s.windDirection = state.wind.direction;
    s.windSpeed = state.wind.speed;
    std::vector<scene::Instance> shown;
    for (size_t i = 0; i < s.instances.size(); ++i)
    {
        if (!state.visible[i]) continue;
        shown.push_back(std::move(s.instances[i]));
        shown.back().transform = state.transforms[i];
    }
    s.instances = std::move(shown);
    for (size_t k = 0; k < s.skeletons.size(); ++k) s.skeletons[k].jointToModel = *state.poses[k];
    return s;
}

void HostRenderer::setInstanceVisible(uint32_t instance, bool visible)
{
    requireCommitted();
    std::lock_guard lock(m_mutex);
    if (instance >= m_hostInstances) fail("visibility of instance %u of %u", instance, m_hostInstances);
    m_pending.visibility.push_back({ instance, visible });
}

void HostRenderer::setSun(const scene::Sun& sun)
{
    std::lock_guard lock(m_mutex);
    m_pending.sun = sun;
}

void HostRenderer::setDiscontinuity(uint32_t flags)
{
    requireCommitted();
    if (flags & ~(kDiscontinuityRestore | kDiscontinuityCut)) fail("unknown discontinuity bits 0x%x", flags);
    std::lock_guard lock(m_mutex);
    m_pending.discontinuity |= flags;
}

void HostRenderer::setSimulation(uint32_t gpuSimulation)
{
    requireCommitted();
    if (gpuSimulation & ~(kGpuSimulationSoft | kGpuSimulationVfx | kGpuSimulationRigid)) fail("unknown GPU simulation bits 0x%x", gpuSimulation);
    std::lock_guard lock(m_mutex);
    m_pending.gpuSimulation |= gpuSimulation;
}

void HostRenderer::setEnvironment(const scene::Sun& sun, const scene::Atmosphere& atmosphere, std::optional<FramePacket::Wind> wind)
{
    requireCommitted();
    std::lock_guard lock(m_mutex);
    m_pending.sun = sun;
    m_pending.atmosphere = atmosphere;
    if (wind) m_pending.wind = wind;
}

uint64_t HostRenderer::queueFrame(FramePacket packet)
{
    requireCommitted();
    if (packet.width == 0 || packet.height == 0) fail("frame output size is zero");
    std::lock_guard lock(m_mutex);
    const uint64_t ticket = m_nextTicket++;
    packet.ticket = ticket;
    packet.sun = std::move(m_pending.sun);
    packet.atmosphere = std::move(m_pending.atmosphere);
    packet.wind = std::move(m_pending.wind);
    packet.discontinuity = m_pending.discontinuity;
    packet.gpuSimulation = m_pending.gpuSimulation;
    packet.transforms = std::move(m_pending.transforms);
    packet.skeletons = std::move(m_pending.skeletons);
    packet.visibility = std::move(m_pending.visibility);
    packet.instanceEdits = std::move(m_pending.instanceEdits);
    packet.materialEdits = std::move(m_pending.materialEdits);
    m_pending = FramePacket{};
    m_packets.push_back(std::move(packet));
    // A frame the host never renders (camera disabled, event dropped) must not hold its updates back from later
    // frames: its scene changes move into the next queued packet when it is dropped.
    while (m_packets.size() > m_options.framesInFlight + 2)
    {
        FramePacket dropped = std::move(m_packets.front());
        m_packets.pop_front();
        FramePacket& next = m_packets.front();
        if (!next.sun) next.sun = dropped.sun;
        if (!next.atmosphere) next.atmosphere = dropped.atmosphere;
        if (!next.wind) next.wind = dropped.wind;
        next.discontinuity |= dropped.discontinuity;
        next.gpuSimulation |= dropped.gpuSimulation;
        next.transforms.insert(next.transforms.begin(), dropped.transforms.begin(), dropped.transforms.end());
        next.skeletons.insert(next.skeletons.begin(), dropped.skeletons.begin(), dropped.skeletons.end());
        next.visibility.insert(next.visibility.begin(), dropped.visibility.begin(), dropped.visibility.end());
        next.instanceEdits.insert(next.instanceEdits.begin(), std::make_move_iterator(dropped.instanceEdits.begin()), std::make_move_iterator(dropped.instanceEdits.end()));
        next.materialEdits.insert(next.materialEdits.begin(), std::make_move_iterator(dropped.materialEdits.begin()), std::make_move_iterator(dropped.materialEdits.end()));
    }
    return ticket;
}

std::optional<FramePacket> HostRenderer::takePacket(uint64_t ticket)
{
    std::lock_guard lock(m_mutex);
    // Older packets were never rendered: their updates carry into the one rendered now (applied first).
    FramePacket carried;
    bool haveCarried = false;
    while (!m_packets.empty() && m_packets.front().ticket < ticket)
    {
        FramePacket& old = m_packets.front();
        if (old.sun) carried.sun = old.sun;
        if (old.atmosphere) carried.atmosphere = old.atmosphere;
        if (old.wind) carried.wind = old.wind;
        carried.discontinuity |= old.discontinuity;
        carried.gpuSimulation |= old.gpuSimulation;
        carried.transforms.insert(carried.transforms.end(), old.transforms.begin(), old.transforms.end());
        carried.skeletons.insert(carried.skeletons.end(), std::make_move_iterator(old.skeletons.begin()), std::make_move_iterator(old.skeletons.end()));
        carried.visibility.insert(carried.visibility.end(), old.visibility.begin(), old.visibility.end());
        carried.instanceEdits.insert(carried.instanceEdits.end(), std::make_move_iterator(old.instanceEdits.begin()), std::make_move_iterator(old.instanceEdits.end()));
        carried.materialEdits.insert(carried.materialEdits.end(), std::make_move_iterator(old.materialEdits.begin()), std::make_move_iterator(old.materialEdits.end()));
        haveCarried = true;
        m_packets.pop_front();
    }
    if (m_packets.empty() || m_packets.front().ticket != ticket) return std::nullopt;
    FramePacket p = std::move(m_packets.front());
    m_packets.pop_front();
    if (haveCarried)
    {
        if (!p.sun) p.sun = carried.sun;
        if (!p.atmosphere) p.atmosphere = carried.atmosphere;
        if (!p.wind) p.wind = carried.wind;
        p.discontinuity |= carried.discontinuity;
        p.gpuSimulation |= carried.gpuSimulation;
        p.transforms.insert(p.transforms.begin(), carried.transforms.begin(), carried.transforms.end());
        p.skeletons.insert(p.skeletons.begin(), std::make_move_iterator(carried.skeletons.begin()), std::make_move_iterator(carried.skeletons.end()));
        p.visibility.insert(p.visibility.begin(), carried.visibility.begin(), carried.visibility.end());
        p.instanceEdits.insert(p.instanceEdits.begin(), std::make_move_iterator(carried.instanceEdits.begin()), std::make_move_iterator(carried.instanceEdits.end()));
        p.materialEdits.insert(p.materialEdits.begin(), std::make_move_iterator(carried.materialEdits.begin()), std::make_move_iterator(carried.materialEdits.end()));
    }
    // Leaving the queue: its updates join the applied state here, under the queue's lock, so currentScene never misses
    // a packet between the queue and the GPU. Its scene edits join the scene (the GPU scene's source) at the same point;
    // beginFrame hands them to the GPU scene.
    std::lock_guard applied(m_appliedMutex);
    applyEdits(p, m_scene);
    overlay(p, m_applied);
    return p;
}

uint32_t HostRenderer::debugErrors() { return m_device->drainDebugMessages(); }

void HostRenderer::removeDeviceForTest()
{
    ComPtr<ID3D12Device5> device;
    check(m_device->d3d()->QueryInterface(IID_PPV_ARGS(&device)), "ID3D12Device5 for RemoveDevice");
    device->RemoveDevice();
}

FrameStats HostRenderer::latestStats() const
{
    std::lock_guard lock(m_mutex);
    return m_stats;
}

uint32_t HostRenderer::beginFrame(const FramePacket& p)
{
    const uint64_t frame = m_recordedFrames;
    const uint32_t slot = (uint32_t)(frame % m_options.framesInFlight);
    for (uint32_t q = 0; q < kQueueTypeCount; ++q) m_device->queue((QueueType)q).waitCpu(m_slotFence[slot][q]);
    m_profiler->beginFrame(frame);
    if (const FrameTiming* t = m_profiler->lastCompleted())
    {
        std::lock_guard lock(m_mutex);
        m_stats.frameIndex = m_slotHostFrame[t->frame % m_options.framesInFlight];
        m_stats.graph = m_slotGraph[t->frame % m_options.framesInFlight];
        for (uint32_t q = 0; q < 2; ++q) m_stats.queues[q] = { t->queues[q].lists, t->queues[q].headMs, t->queues[q].tailMs, t->queues[q].gapMs };
        m_stats.gpuMs = t->gpuFrameMs;
        m_stats.passes = (uint32_t)t->passes.size();
        m_stats.passMs.clear();
        for (const PassTiming& pt : t->passes) m_stats.passMs.emplace_back(pt.name, pt.durationMs());
    }
    m_slotHostFrame[slot] = p.frameIndex;
    // Scene changes of this frame (GpuScene uploads them at the start of FrameRenderer::record).
    if (p.sun || p.atmosphere || p.wind)
    {
        std::lock_guard lock(m_appliedMutex);
        if (p.sun) m_scene.sun = *p.sun;
        if (p.atmosphere) m_scene.atmosphere = *p.atmosphere;
        if (p.wind)
        {
            m_scene.windDirection = p.wind->direction;
            m_scene.windSpeed = p.wind->speed;
        }
    }
    // Scene edits (the scene already has them, takePacket): materials first, since new instances may override with them.
    if (!p.materialEdits.empty() || !p.instanceEdits.empty())
    {
        std::vector<uint32_t> materials, instances;
        for (const auto& e : p.materialEdits) materials.push_back(e.first);
        for (const auto& e : p.instanceEdits) instances.push_back(e.first);
        auto unique = [](std::vector<uint32_t>& v) {
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
        };
        unique(materials);
        unique(instances);
        if (!materials.empty()) m_gpuScene->setMaterials(materials);
        if (!instances.empty())
        {
            m_gpuScene->setInstances(instances);
            m_gpuTransforms.resize(m_gpuScene->instances().size());
            for (uint32_t i : instances) m_gpuTransforms[i] = m_scene.instances[i].transform;
        }
    }
    // Only changes reach the GPU scene. A host sends every visible instance and pose each frame (a body at rest included),
    // and the GPU scene counts every update as motion (transformRevision / deformRevision += 1): the caster would be
    // listed for the local lights' shadow pages and the static TLAS rebuilt every frame. A bit-identical update changes
    // nothing else: without it the settling rule (GpuScene.h) gives prev = current, as the update itself would, and a
    // teleport to where the instance already is has no motion either way.
    m_changedTransforms.clear();
    for (const InstanceTransformUpdate& u : p.transforms)
    {
        if (std::memcmp(&m_gpuTransforms[u.instance], &u.objectToWorld, sizeof(float3x4)) == 0)
        {
            ++m_droppedTransforms;
            continue;
        }
        m_gpuTransforms[u.instance] = u.objectToWorld;
        m_changedTransforms.push_back(u);
    }
    if (!m_changedTransforms.empty()) m_gpuScene->updateTransforms(frame, m_changedTransforms);
    for (const SkeletonPose& s : p.skeletons)
    {
        std::shared_ptr<const std::vector<float3x4>>& last = m_gpuPoses[s.skeleton];
        if (last == s.jointToModel || std::memcmp(last->data(), s.jointToModel->data(), last->size() * sizeof(float3x4)) == 0)
        {
            ++m_droppedPoses;
            continue;
        }
        last = s.jointToModel;
        m_gpuScene->updateSkeleton(frame, s.skeleton, *s.jointToModel);
    }
    for (const auto& [instance, visible] : p.visibility) m_gpuScene->setInstanceVisible(instance, visible);
    return slot;
}

void HostRenderer::recordFrame(const FramePacket& p, TextureRef output)
{
    FrameContext fc;
    fc.frameIndex = m_recordedFrames;
    fc.time = p.time;
    fc.deltaTime = p.deltaTime;
    const ViewDesc current = ViewDesc::fromCamera(p.camera, p.width, p.height, {});
    // Either discontinuity bit: the main view has no previous view (INTERFACES 5.5.2).
    const bool previous = m_havePrev && p.discontinuity == 0;
    fc.mainView = ViewDesc::fromCamera(p.camera, p.width, p.height, previous ? m_prevViewProj : current.viewProj);
    fc.discontinuity = p.discontinuity;
    fc.gpuSimulation = p.gpuSimulation;
    m_lastDiscontinuity = p.discontinuity;
    m_lastGpuSimulation = p.gpuSimulation;
    m_prevViewProj = fc.mainView.viewProj;
    m_havePrev = true;
    m_frameRenderer->record(*m_graph, fc, output);
}

ID3D12Device* HostRenderer::d3dDevice() const { return m_device ? m_device->d3d() : nullptr; }

void HostRenderer::endFrame(uint32_t slot, uint64_t)
{
    for (uint32_t q = 0; q < kQueueTypeCount; ++q) m_slotFence[slot][q] = m_graph->lastFence((QueueType)q);
    const RenderGraphStats& g = m_graph->stats();
    m_slotGraph[slot] = { g.livePasses, g.commandLists, g.barrierBatches, g.barriers, g.crossQueueSyncs, g.transientResources, g.planReused,
                          g.transientBytesAliased, g.cpuCompileMs, m_gpuScene->revision() };
    ++m_recordedFrames;
    std::lock_guard lock(m_mutex);
    m_stats.cpuRecordMs = m_graph->stats().cpuRecordMs;
    m_stats.cpuSubmitMs = m_graph->stats().cpuSubmitMs;
}

fx::ParticleSystem& HostRenderer::fxModule()
{
    requireCommitted();
    fx::ParticleSystem& p = fx::particles(m_frameRenderer->trackState(), *m_device, m_quality);
    p.setExplicitQueue(QueueType::Compute);  // main-thread copies never touch the host's graphics queue
    return p;
}

void HostRenderer::fxRunPending()
{
    fx::ParticleSystem& p = fxModule();
    if (p.pendingTicks() == 0) return;
    Queue& graphics = m_device->queue(QueueType::Graphics);
    Queue& compute = m_device->queue(QueueType::Compute);
    // After every frame submitted so far (they read the state these ticks overwrite), before any later frame.
    compute.waitGpu(graphics, graphics.lastSignaled());
    if (!m_simGraph)
    {
        m_simGraph = std::make_unique<RenderGraph>(*m_device);
        m_simGraph->setAsyncCompute(true);  // (off, every pass would go to the graphics queue)
    }
    m_vfxImmediateTicks += p.pendingTicks();
    p.record(*m_simGraph, *m_shaders, m_simIndex++, QueueType::Compute);
    m_simGraph->execute(nullptr);
    m_simFence = compute.lastSignaled();
}

void HostRenderer::fxFrameWait()
{
    if (m_simFence <= m_simWaited) return;
    m_device->queue(QueueType::Graphics).waitGpu(m_device->queue(QueueType::Compute), m_simFence);
    m_simWaited = m_simFence;
}

void HostRenderer::vfxSubmit(const uint8_t* packet, uint64_t bytes)
{
    std::lock_guard lock(m_fxMutex);
    try
    {
        // A frame records every pending tick into one graph, and a tick reuses the readback slot of the tick one ring
        // before it: more pending ticks than the ring holds would wait on a slot of the same unexecuted graph. Ticks
        // submitted without frames or readbacks in between (a hitch, a long fixed-step catch-up) run at once on the
        // compute queue before the ring fills.
        fx::ParticleSystem& p = fxModule();
        if (p.pendingTicks() + 1 >= p.readbackSlots()) fxRunPending();
        p.submit(packet, bytes);
    }
    catch (const std::exception& e)
    {
        // The commit must not fail (NV_StreamExecutor contract): the next readback reports it.
        if (m_vfxError.empty()) m_vfxError = e.what();
    }
}

const fx::TickReadback& HostRenderer::vfxReadback(uint64_t stream, uint64_t generation, uint64_t tick)
{
    std::lock_guard lock(m_fxMutex);
    if (!m_vfxError.empty()) fail("FX stream executor: %s", m_vfxError.c_str());
    fxRunPending();
    m_vfxReadback = fxModule().readback(stream, generation, tick);
    return m_vfxReadback;
}

const std::vector<NV_StreamParticle>& HostRenderer::vfxCheckpoint(uint64_t stream, uint64_t generation, uint64_t tick)
{
    std::lock_guard lock(m_fxMutex);
    if (!m_vfxError.empty()) fail("FX stream executor: %s", m_vfxError.c_str());
    fxRunPending();
    fx::ParticleSystem& p = fxModule();
    if (p.latestTick() != tick) fail("FX stream executor: checkpoint of tick %llu, the latest is %llu", (unsigned long long)tick, (unsigned long long)p.latestTick());
    (void)stream;
    (void)generation;
    m_vfxCheckpoint = p.checkpoint(*m_shaders);
    return m_vfxCheckpoint;
}

void HostRenderer::renderOnHost(uint64_t ticket, const HostExecute& execute)
{
    if (m_options.standalone) fail("renderOnHost on a standalone renderer");
    requireCommitted();
    std::lock_guard fxLock(m_fxMutex);  // the frame's C0 ticks and particle pass use the module
    std::optional<FramePacket> packet = takePacket(ticket);
    if (!packet) return;  // an older ticket already rendered, or dropped: nothing to draw
    const FramePacket& p = *packet;
    if (!p.output) fail("frame %llu has no output texture", (unsigned long long)p.frameIndex);
    const D3D12_RESOURCE_DESC od = p.output->GetDesc();
    if (od.Width != p.width || od.Height != p.height || !(od.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
        fail("output texture is %llux%u flags 0x%x; the frame needs %ux%u with random write", (unsigned long long)od.Width, od.Height, (unsigned)od.Flags, p.width,
             p.height);
    const uint32_t slot = beginFrame(p);
    TextureDesc desc;
    desc.name = "host output";
    desc.width = p.width;
    desc.height = p.height;
    desc.format = DXGI_FORMAT_R10G10B10A2_UNORM;
    // The host executes every list with the output declared UNORDERED_ACCESS before and after, so the graph sees it in
    // that layout at frame start and leaves it there.
    const TextureRef output = m_graph->importTexture(p.output, desc, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    recordFrame(p, output);
    Queue& graphics = m_device->queue(QueueType::Graphics);
    ID3D12Resource* hostOutput = p.output;
    graphics.setExecuteHook([&](ID3D12CommandList* list) { execute(list, hostOutput); });
    try
    {
        fxFrameWait();  // on the host's queue, in its render event
        m_graph->execute(m_profiler.get());
    }
    catch (...)
    {
        graphics.setExecuteHook({});
        throw;
    }
    graphics.setExecuteHook({});
    endFrame(slot, p.frameIndex);
}

void HostRenderer::ensureStandaloneOutput(uint32_t width, uint32_t height)
{
    Standalone& s = *m_standalone;
    if (s.output && s.width == width && s.height == height) return;
    m_device->waitIdle();
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(m_device->d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr, nullptr, 0, nullptr,
                                                   IID_PPV_ARGS(&s.output)),
          "standalone output");
    s.output->SetName(L"UnravelNext host output");
    const D3D12_RESOURCE_DESC legacy = s.output->GetDesc();
    m_device->d3d()->GetCopyableFootprints(&legacy, 0, 1, 0, &s.footprint, nullptr, nullptr, &s.readbackBytes);
    D3D12_HEAP_PROPERTIES rb{ D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC b{};
    b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    b.Width = s.readbackBytes;
    b.Height = 1;
    b.DepthOrArraySize = 1;
    b.MipLevels = 1;
    b.SampleDesc.Count = 1;
    b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    check(m_device->d3d()->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &b, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s.readback)),
          "standalone readback");
    s.width = width;
    s.height = height;
}

void HostRenderer::renderStandalone(uint64_t ticket, void* readback, size_t readbackBytes)
{
    std::lock_guard fxLock(m_fxMutex);
    if (!m_options.standalone) fail("renderStandalone on a renderer bound to the host's device");
    requireCommitted();
    std::optional<FramePacket> packet = takePacket(ticket);
    if (!packet) fail("frame ticket %llu is not queued (dropped or already rendered)", (unsigned long long)ticket);
    const FramePacket& p = *packet;
    if (readback && readbackBytes < (size_t)p.width * p.height * 4) fail("readback buffer holds %zu bytes, needs %u", readbackBytes, p.width * p.height * 4);
    const void* previousOutput = m_standalone->output.Get();
    if (m_recreateOutput && m_standalone->output)
    {
        m_device->waitIdle();  // the host releases a render target only after the GPU is done with it
        m_standalone->output.Reset();
        m_standalone->width = m_standalone->height = 0;
    }
    ensureStandaloneOutput(p.width, p.height);
    if (m_recreateOutput && previousOutput)
    {
        ++m_outputRecreations;
        if (m_standalone->output.Get() == previousOutput) ++m_outputReuses;
    }
    const uint32_t slot = beginFrame(p);

    Standalone& s = *m_standalone;
    TextureDesc od;
    od.name = "host output";
    od.width = p.width;
    od.height = p.height;
    od.format = DXGI_FORMAT_R10G10B10A2_UNORM;
    const TextureRef output = m_graph->importTexture(s.output.Get(), od, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    recordFrame(p, output);
    if (readback)
    {
        BufferDesc bd;
        bd.name = "host readback";
        bd.size = s.readbackBytes;
        const BufferRef rb = m_graph->importBuffer(s.readback.Get(), bd);
        m_graph->addPass(
            "host.readback", QueueType::Graphics,
            [&](PassBuilder& b) {
                b.use(output, Use::CopySrc);
                b.use(rb, Use::CopyDst);
                b.keep();
            },
            [&s](PassContext& c) {
                D3D12_TEXTURE_COPY_LOCATION dst{};
                dst.pResource = s.readback.Get();
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint = s.footprint;
                D3D12_TEXTURE_COPY_LOCATION src{};
                src.pResource = s.output.Get();
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src.SubresourceIndex = 0;
                c.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            });
    }
    fxFrameWait();
    m_graph->execute(m_profiler.get());
    endFrame(slot, p.frameIndex);
    if (readback)
    {
        for (uint32_t q = 0; q < kQueueTypeCount; ++q) m_device->queue((QueueType)q).waitCpu(m_slotFence[slot][q]);
        const uint8_t* mapped = nullptr;
        D3D12_RANGE range{ 0, (SIZE_T)s.readbackBytes };
        check(s.readback->Map(0, &range, (void**)&mapped), "readback Map");
        for (uint32_t y = 0; y < p.height; ++y)
            std::memcpy((uint8_t*)readback + (size_t)y * p.width * 4, mapped + s.footprint.Offset + (size_t)y * s.footprint.Footprint.RowPitch, (size_t)p.width * 4);
        D3D12_RANGE none{ 0, 0 };
        s.readback->Unmap(0, &none);
    }
}
} // namespace unx::host
