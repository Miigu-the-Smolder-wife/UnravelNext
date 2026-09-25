#include "unx/rt/RayScene.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace unx::render::rt
{
namespace
{
constexpr uint64_t kAsAlign = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BYTE_ALIGNMENT;  // 256
constexpr uint64_t kScratchBatchBytes = 256ull << 20;  // scratch reused between load-time build batches

uint64_t alignUp(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }

// Load-time and test-path barriers (outside the frame graph, on the R track's own command lists).
void globalBarrier(ID3D12GraphicsCommandList7* cmd, D3D12_BARRIER_SYNC syncBefore, D3D12_BARRIER_ACCESS accessBefore, D3D12_BARRIER_SYNC syncAfter,
                   D3D12_BARRIER_ACCESS accessAfter)
{
    D3D12_GLOBAL_BARRIER g{ syncBefore, syncAfter, accessBefore, accessAfter };
    D3D12_BARRIER_GROUP group{};
    group.Type = D3D12_BARRIER_TYPE_GLOBAL;
    group.NumBarriers = 1;
    group.pGlobalBarriers = &g;
    cmd->Barrier(1, &group);
}

constexpr D3D12_BARRIER_SYNC kSyncBuild = D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE;
constexpr D3D12_BARRIER_SYNC kSyncTrace = D3D12_BARRIER_SYNC_ALL_SHADING | D3D12_BARRIER_SYNC_RAYTRACING | D3D12_BARRIER_SYNC_COMPUTE_SHADING;
constexpr D3D12_BARRIER_ACCESS kAsRead = D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ;
constexpr D3D12_BARRIER_ACCESS kAsWrite = D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE;

std::mutex g_sceneMutex;
std::map<std::pair<Device*, GpuScene*>, std::unique_ptr<RayScene>> g_scenes;

struct RaySceneSlot
{
    std::unique_ptr<RayScene> scene;
};
} // namespace

RayScene* RayScene::find(TrackState& state) { return state.get<RaySceneSlot>("R.rayScene").scene.get(); }

RayScene& RayScene::get(FramePassContext& fc)
{
    RaySceneSlot& slot = fc.state<RaySceneSlot>("R.rayScene");
    if (slot.scene && slot.scene->sceneRevision() != fc.scene.revision()) slot.scene.reset();  // re-uploaded scene
    if (!slot.scene) slot.scene = std::make_unique<RayScene>(fc.device, fc.shaders, fc.scene, fc.quality);
    return *slot.scene;
}

RayScene& RayScene::get(Device& device, ShaderLibrary& shaders, GpuScene& scene, const QualityConfig& quality)
{
    std::lock_guard lock(g_sceneMutex);
    auto& slot = g_scenes[{ &device, &scene }];
    if (slot && slot->sceneRevision() != scene.revision()) slot.reset();  // re-uploaded scene: rebuild (streaming boundary)
    if (!slot) slot = std::make_unique<RayScene>(device, shaders, scene, quality);
    return *slot;
}

void RayScene::release(Device& device, GpuScene& scene)
{
    device.waitIdle();
    std::lock_guard lock(g_sceneMutex);
    g_scenes.erase({ &device, &scene });
}

void RayScene::releaseDevice(Device& device)
{
    device.waitIdle();
    std::lock_guard lock(g_sceneMutex);
    std::erase_if(g_scenes, [&](const auto& e) { return e.first.first == &device; });
}

RayScene::Buffer RayScene::createBuffer(uint64_t bytes, bool uav, bool accelerationStructure, const wchar_t* name)
{
    Buffer b;
    b.bytes = std::max<uint64_t>(bytes, 256);
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = b.bytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (uav || accelerationStructure) d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (accelerationStructure) d.Flags |= D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE;
    check(m_device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&b.resource)),
          "RayScene buffer");
    b.resource->SetName(name);
    return b;
}

void RayScene::upload(Buffer& target, const void* data, uint64_t bytes)
{
    if (bytes == 0) return;
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> staging;
    check(m_device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&staging)),
          "RayScene staging");
    void* mapped = nullptr;
    D3D12_RANGE none{ 0, 0 };
    check(staging->Map(0, &none, &mapped), "map RayScene staging");
    std::memcpy(mapped, data, (size_t)bytes);
    staging->Unmap(0, nullptr);
    CommandList cl = m_device.acquireCommandList(QueueType::Graphics);
    cl.list->CopyBufferRegion(target.resource.Get(), 0, staging.Get(), 0, bytes);
    m_device.queue(QueueType::Graphics).waitCpu(m_device.submit(cl));
}

RayScene::Buffer RayScene::createStructured(const void* data, uint32_t stride, uint32_t count, const wchar_t* name)
{
    std::vector<uint8_t> zero;
    if (count == 0)
    {
        zero.assign(stride, 0);
        data = zero.data();
        count = 1;
    }
    Buffer b = createBuffer((uint64_t)stride * count, false, false, name);
    upload(b, data, (uint64_t)stride * count);
    DescriptorHeaps& h = m_device.descriptors();
    b.srv = h.allocateResource();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Format = DXGI_FORMAT_UNKNOWN;
    sd.Buffer.NumElements = count;
    sd.Buffer.StructureByteStride = stride;
    m_device.d3d()->CreateShaderResourceView(b.resource.Get(), &sd, h.resourceCpu(b.srv));
    return b;
}

uint32_t RayScene::tlasSrv(D3D12_GPU_VIRTUAL_ADDRESS address)
{
    DescriptorHeaps& h = m_device.descriptors();
    const uint32_t index = h.allocateResource();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.RaytracingAccelerationStructure.Location = address;
    m_device.d3d()->CreateShaderResourceView(nullptr, &sd, h.resourceCpu(index));
    return index;
}

void RayScene::release(Buffer& b)
{
    if (b.resource) m_device.deferRelease(b.resource);
    if (b.srv != gpu::kNone)
    {
        DescriptorHeaps* h = &m_device.descriptors();
        const uint32_t srv = b.srv;
        m_device.deferCall([h, srv] { h->freeResource(srv); });
    }
    b = {};
}

RayScene::~RayScene()
{
    for (Buffer* b : { &m_meshBlasPool, &m_deformedPool, &m_deformedBlasPool, &m_deformedScratch, &m_deformJobs, &m_deformGroups, &m_instanceBuffer, &m_geometryBuffer,
                       &m_indexPool, &m_vertexMap, &m_tlasStatic, &m_tlasDynamic, &m_tlasScratch, &m_staticDescBuffer, &m_dynamicDescBuffer, &m_staticScratch,
                       &m_exactCounts, &m_exactZero })
        release(*b);
    if (m_exactReadback) m_exactReadback->Unmap(0, nullptr);
    m_device.deferRelease(m_exactReadback);
    if (m_patchRing) m_patchRing->Unmap(0, nullptr);
    m_device.deferRelease(m_patchRing);
    if (m_vsmRing)
    {
        m_vsmRing->Unmap(0, nullptr);
        m_device.deferRelease(m_vsmRing);
        DescriptorHeaps* vh = &m_device.descriptors();
        for (uint32_t srv : m_vsmRingSrv) m_device.deferCall([vh, srv] { vh->freeResource(srv); });
    }
    if (m_descRing)
    {
        m_descRing->Unmap(0, nullptr);
        m_device.deferRelease(m_descRing);
    }
    DescriptorHeaps* h = &m_device.descriptors();
    for (uint32_t srv : { m_tlasStaticSrv, m_tlasDynamicSrv, m_deformedPoolUav, m_exactCountsUav })
        if (srv != gpu::kNone) m_device.deferCall([h, srv] { h->freeResource(srv); });
}

RayScene::RayScene(Device& device, ShaderLibrary& shaders, GpuScene& scene, const QualityConfig& quality) : m_device(device), m_shaders(shaders), m_scene(scene)
{
    const auto t0 = std::chrono::steady_clock::now();
    if (device.caps().raytracingTier < D3D12_RAYTRACING_TIER_1_1) fail("RayScene: DXR tier 1.1 required");
    const scene::Scene* src = scene.source();
    if (!src) fail("RayScene: GpuScene has no uploaded scene");
    m_sceneRevision = scene.revision();
    const uint32_t proxyBudget = (uint32_t)quality.integer("raytracing.character_proxy_triangles");
    m_proxyErrorPx = (float)quality.number("raytracing.proxy_error_px");
    m_experiment = (uint32_t)quality.integer("raytracing.experiment_disable");
    const uint64_t dynamicMax = (uint64_t)quality.integer("raytracing.dynamic_tlas_instances_max");

    const auto& instances = scene.instances();
    const auto& meshes = scene.meshes();
    m_meshBlas.assign(meshes.size(), {});

    // Geometry records: one per non-empty submesh, shared by every BLAS of the mesh (material comes from the instance).
    std::vector<uint32_t> meshGeometryBase(meshes.size(), gpu::kNone);
    auto geometryBase = [&](uint32_t m) {
        if (meshGeometryBase[m] != gpu::kNone) return meshGeometryBase[m];
        meshGeometryBase[m] = (uint32_t)m_geometries.size();
        const scene::Mesh& sm = src->meshes[m];
        for (uint32_t s = 0; s < (uint32_t)sm.submeshes.size(); ++s)
            if (sm.submeshes[s].indexCount > 0) m_geometries.push_back({ meshes[m].indexOffset + sm.submeshes[s].indexOffset, s, 0, gpu::kNone });
        return meshGeometryBase[m];
    };

    // Classify instances (INTERFACES 6.2 flags): deformed = skinned with a palette and skin stream; dynamic rigid =
    // InstanceDynamic; the rest is static (wind-affected foliage uses its rest pose in RT, ARCHITECTURE 2.7).
    std::vector<uint32_t> staticList, dynamicRigid, deformed;
    for (uint32_t i = 0; i < (uint32_t)instances.size(); ++i)
    {
        const gpu::Instance& in = instances[i];
        const gpu::Mesh& m = meshes[in.mesh];
        if (m.triangleCount == 0) continue;
        if ((in.flags & scene::InstanceSkinned) && in.bonePalette != gpu::kNone && m.skinOffset != gpu::kNone) deformed.push_back(i);
        else if (in.flags & scene::InstanceDynamic) dynamicRigid.push_back(i);
        else staticList.push_back(i);
    }
    if (dynamicRigid.size() + deformed.size() > dynamicMax)
        fail("RayScene: %zu dynamic instances exceed raytracing.dynamic_tlas_instances_max %llu", dynamicRigid.size() + deformed.size(), (unsigned long long)dynamicMax);

    auto materialAlpha = [&](uint32_t material) { return material < src->materials.size() && src->materials[material].alphaCutoff > 0; };
    // Per-submesh alpha of an instance (overrides first); FORCE_NON_OPAQUE is needed when an override turns a submesh the
    // mesh BLAS built OPAQUE into an alpha-tested one (the any-hit shader accepts opaque materials, so the reverse is exact).
    auto instanceFlags = [&](uint32_t i) -> UINT {
        const scene::Instance& in = src->instances[i];
        const scene::Mesh& sm = src->meshes[in.mesh];
        for (uint32_t s = 0; s < (uint32_t)sm.submeshes.size() && s < (uint32_t)in.materialOverrides.size(); ++s)
            if (materialAlpha(in.materialOverrides[s]) && !materialAlpha(sm.submeshes[s].material)) return D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE;
        return 0;
    };

    // RtInstance records: static TLAS instances first, then the dynamic TLAS (rigid, then deformed).
    for (uint32_t i : staticList) m_meshBlas[instances[i].mesh].geometryBase = geometryBase(instances[i].mesh);
    for (uint32_t i : dynamicRigid) m_meshBlas[instances[i].mesh].geometryBase = geometryBase(instances[i].mesh);
    buildMeshBlas();

    auto transformOf = [&](const gpu::Instance& in, D3D12_RAYTRACING_INSTANCE_DESC& d) {
        for (int r = 0; r < 3; ++r)
        {
            d.Transform[r][0] = in.objectToWorld[r].x;
            d.Transform[r][1] = in.objectToWorld[r].y;
            d.Transform[r][2] = in.objectToWorld[r].z;
            d.Transform[r][3] = in.objectToWorld[r].w;
        }
    };
    const D3D12_GPU_VIRTUAL_ADDRESS meshPool = m_meshBlasPool.address();
    auto rigidDesc = [&](uint32_t i) {
        const gpu::Instance& in = instances[i];
        D3D12_RAYTRACING_INSTANCE_DESC d{};
        transformOf(in, d);
        d.InstanceID = (UINT)m_instances.size();
        d.InstanceMask = (in.flags & gpu::kInstanceHidden) ? 0 : kRtMaskAll;
        d.InstanceContributionToHitGroupIndex = 0;
        d.Flags = instanceFlags(i);  // DXR's default winding = CCW front in our right-handed frame (verified by Tests/RayScene)
        d.AccelerationStructure = meshPool + m_meshBlas[in.mesh].offset;
        m_instances.push_back({ i, m_meshBlas[in.mesh].geometryBase, gpu::kNone, 0 });
        return d;
    };
    for (uint32_t i : staticList)
    {
        m_staticDescs.push_back(rigidDesc(i));
        m_staticScene.push_back(i);
        m_staticKeys.push_back(staticKey(instances[i]));
    }
    for (uint32_t i : dynamicRigid)
    {
        m_dynamicRecord.push_back((uint32_t)m_instances.size());
        m_dynamicDescs.push_back(rigidDesc(i));
    }

    // Deformed instances: per-instance BLAS over the mesh's RT proxy (ProxyMesh: V's LOD cut within
    // raytracing.character_proxy_triangles, ARCHITECTURE 2.8), deformed through the proxy's vertex map.
    m_proxyBudget = proxyBudget;
    m_proxyLevels.assign(meshes.size(), {});
    m_proxyLevelsBuilt.assign(meshes.size(), 0);
    uint32_t vertexBase = 0;
    for (uint32_t i : deformed)
    {
        const gpu::Instance& in = instances[i];
        const ProxyMesh& p = proxyLevels(in.mesh)[0];  // the finest cut: the vertex region fits every coarser one
        Deformed d;
        d.sceneInstance = i;
        d.vertexBase = vertexBase;
        d.vertexCount = p.vertexCount;
        d.geometryBase = p.geometryBase;
        d.mesh = in.mesh;
        d.originalGeometryBase = geometryBase(in.mesh);
        vertexBase += p.vertexCount;
        if (p.triangles > proxyBudget) ++m_stats.deformedAboveProxyBudget;
        m_stats.deformedTriangles += p.triangles;
        m_deformed.push_back(std::move(d));
    }
    m_stats.deformedVertices = vertexBase;
    // Reflection exact set slots after the proxies in the deformed pool (ARCHITECTURE 2.6).
    m_exactMinHits = (uint32_t)quality.integer("raytracing.exact_set_min_hits");
    {
        const uint32_t slots = std::min<uint32_t>((uint32_t)quality.integer("raytracing.exact_set_max"), (uint32_t)m_deformed.size());
        uint32_t capacity = 0;
        for (const Deformed& d : m_deformed) capacity = std::max(capacity, meshes[d.mesh].vertexCount);
        m_exact.resize(slots);
        for (uint32_t s = 0; s < slots; ++s)
        {
            m_exact[s].vertexBase = vertexBase;
            vertexBase += capacity;
        }
        m_stats.exactSlots = slots;
        m_poolVertices = std::max<uint64_t>(vertexBase, 1);  // proxies, then the exact slots
    }
    // R's index pool and vertex map (proxy cuts) are needed by the deformed BLAS geometry below.
    m_indexPool = createStructured(m_indexPoolData.data(), sizeof(uint32_t), (uint32_t)m_indexPoolData.size(), L"RT proxy indices");
    m_vertexMap = createStructured(m_vertexMapData.data(), sizeof(uint32_t), (uint32_t)m_vertexMapData.size(), L"RT vertex map");
    buildDeformed();
    for (size_t k = 0; k < m_deformed.size(); ++k)
    {
        Deformed& d = m_deformed[k];
        d.record = (uint32_t)m_instances.size();
        D3D12_RAYTRACING_INSTANCE_DESC desc{};
        desc.Transform[0][0] = desc.Transform[1][1] = desc.Transform[2][2] = 1;  // world-space vertices
        desc.InstanceID = (UINT)m_instances.size();
        desc.InstanceMask = (instances[d.sceneInstance].flags & gpu::kInstanceHidden) ? 0 : kRtMaskAll;
        desc.Flags = instanceFlags(d.sceneInstance);
        desc.AccelerationStructure = m_deformedBlasPool.address() + d.blasOffset;
        m_dynamicRecord.push_back((uint32_t)m_instances.size());
        m_instances.push_back({ d.sceneInstance, d.geometryBase, d.vertexBase, kRtInstanceDeformed | ((uint32_t)k << 8) });
        m_dynamicDescs.push_back(desc);
    }
    // Exact set slot records (patched when a slot changes owner).
    for (ExactSlot& e : m_exact)
    {
        e.record = (uint32_t)m_instances.size();
        m_instances.push_back({ 0, 0, e.vertexBase, kRtInstanceDeformed | (0xFFFFFFu << 8) });
    }
    m_stats.staticInstances = (uint32_t)m_staticDescs.size();
    m_stats.dynamicInstances = (uint32_t)m_dynamicDescs.size();
    m_stats.deformedInstances = (uint32_t)m_deformed.size();

    m_instanceBuffer = createStructured(m_instances.data(), sizeof(RtInstance), (uint32_t)m_instances.size(), L"RT instances");
    if (!m_exact.empty())
    {
        const uint64_t bytes = (uint64_t)m_deformed.size() * 4;
        m_exactCounts = createBuffer(bytes, true, false, L"RT exact set hit counts");
        const std::vector<uint32_t> zeros(m_deformed.size(), 0);
        m_exactZero = createBuffer(bytes, false, false, L"RT exact set zeros");
        upload(m_exactZero, zeros.data(), bytes);
        DescriptorHeaps& h = m_device.descriptors();
        m_exactCountsUav = h.allocateResource();
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = (UINT)m_deformed.size();
        ud.Buffer.StructureByteStride = 4;
        m_device.d3d()->CreateUnorderedAccessView(m_exactCounts.resource.Get(), nullptr, &ud, h.resourceCpu(m_exactCountsUav));
        D3D12_HEAP_PROPERTIES readback{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC1 d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = kDescSlots * bytes;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(m_device.d3d()->CreateCommittedResource3(&readback, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_exactReadback)),
              "RT exact set readback");
        D3D12_RANGE all{ 0, (SIZE_T)d.Width };
        check(m_exactReadback->Map(0, &all, reinterpret_cast<void**>(const_cast<uint32_t**>(&m_exactReadbackMapped))), "map exact readback");
        m_exactSlotFrame.assign(kDescSlots, UINT64_MAX);
    }
    // Patch ring (exact set records and proxy cut switches): whenever there are deformed instances.
    if (!m_deformed.empty())
    {
        D3D12_HEAP_PROPERTIES upHeap{ D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC1 d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = kDescSlots * kPatchSlotBytes;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(m_device.d3d()->CreateCommittedResource3(&upHeap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_patchRing)),
              "RT patch ring");
        D3D12_RANGE none{ 0, 0 };
        check(m_patchRing->Map(0, &none, reinterpret_cast<void**>(&m_patchRingMapped)), "map patch ring");
    }
    m_geometryBuffer = createStructured(m_geometries.data(), sizeof(RtGeometry), (uint32_t)m_geometries.size(), L"RT geometries");

    buildStaticTlas();
    {
        m_descSlotBytes = (uint64_t)(m_dynamicDescs.size() + m_staticDescs.size() + 1) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
        m_descSlotBytes = (m_descSlotBytes + 255) / 256 * 256;
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC1 d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = m_descSlotBytes * kDescSlots;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(m_device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_descRing)),
              "RT instance descriptor ring");
        m_descRing->SetName(L"RT instance descriptor ring");
        D3D12_RANGE none{ 0, 0 };
        check(m_descRing->Map(0, &none, reinterpret_cast<void**>(&m_descRingMapped)), "map RT descriptor ring");
    }
    // Dynamic TLAS and deformed BLASes: first full build at load.
    {
        CommandList cl = m_device.acquireCommandList(QueueType::Graphics);
        gpu::FrameConstants fcData{};
        m_scene.fill(fcData);
        Buffer constants = createBuffer(1024, false, false, L"RT load frame constants");
        upload(constants, &fcData, sizeof fcData);
        cl.list->SetComputeRootConstantBufferView(1, constants.address());
        updateDynamic(cl.list.Get(), false);
        m_device.queue(QueueType::Graphics).waitCpu(m_device.submit(cl));
        release(constants);
    }
    m_tlasStaticSrv = tlasSrv(m_tlasStatic.address());
    m_tlasDynamicSrv = tlasSrv(m_tlasDynamic.address());
    m_stats.loadMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    logf("RayScene: %u static + %u dynamic instances (%u deformed), %u mesh BLAS (%llu tris, %.1f MB compacted from %.1f MB), deformed %llu tris / %llu verts "
         "(%u above proxy budget), TLAS static %.1f MB dynamic %.2f MB, load %.0f ms\n",
         m_stats.staticInstances, m_stats.dynamicInstances, m_stats.deformedInstances, m_stats.meshBlas, (unsigned long long)m_stats.meshBlasTriangles,
         m_stats.meshBlasBytes / 1048576.0, m_stats.meshBlasBytesBeforeCompaction / 1048576.0, (unsigned long long)m_stats.deformedTriangles,
         (unsigned long long)m_stats.deformedVertices, m_stats.deformedAboveProxyBudget, m_stats.tlasStaticBytes / 1048576.0, m_stats.tlasDynamicBytes / 1048576.0,
         m_stats.loadMs);
}

void RayScene::buildMeshBlas()
{
    const scene::Scene* src = m_scene.source();
    const auto& meshes = m_scene.meshes();
    const D3D12_GPU_VIRTUAL_ADDRESS vertices = m_scene.buffer("vertices")->GetGPUVirtualAddress();
    const D3D12_GPU_VIRTUAL_ADDRESS indices = m_scene.buffer("indices")->GetGPUVirtualAddress();
    auto materialAlpha = [&](uint32_t material) { return material < src->materials.size() && src->materials[material].alphaCutoff > 0; };

    struct Build
    {
        uint32_t mesh;
        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO sizes{};
        uint64_t offset = 0, scratchOffset = 0;
    };
    std::vector<Build> builds;
    for (uint32_t m = 0; m < (uint32_t)meshes.size(); ++m)
    {
        if (m_meshBlas[m].geometryBase == gpu::kNone) continue;
        const gpu::Mesh& gm = meshes[m];
        const scene::Mesh& sm = src->meshes[m];
        Build b;
        b.mesh = m;
        for (uint32_t s = 0; s < (uint32_t)sm.submeshes.size(); ++s)
        {
            if (sm.submeshes[s].indexCount == 0) continue;
            const bool alpha = materialAlpha(sm.submeshes[s].material);
            m_meshBlas[m].anyAlpha |= alpha;
            D3D12_RAYTRACING_GEOMETRY_DESC g{};
            g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            g.Flags = alpha ? D3D12_RAYTRACING_GEOMETRY_FLAG_NONE : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            g.Triangles.VertexBuffer = { vertices + (uint64_t)gm.vertexOffset * sizeof(gpu::Vertex), sizeof(gpu::Vertex) };
            g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            g.Triangles.VertexCount = gm.vertexCount;
            g.Triangles.IndexBuffer = indices + ((uint64_t)gm.indexOffset + sm.submeshes[s].indexOffset) * sizeof(uint32_t);
            g.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
            g.Triangles.IndexCount = sm.submeshes[s].indexCount;
            b.geometries.push_back(g);
        }
        m_stats.meshBlasTriangles += gm.triangleCount;
        builds.push_back(std::move(b));
    }
    m_stats.meshBlas = (uint32_t)builds.size();
    if (builds.empty())
    {
        m_meshBlasPool = createBuffer(kAsAlign, false, true, L"RT mesh BLAS pool (empty)");
        return;
    }

    uint64_t poolBytes = 0;
    for (Build& b : builds)
    {
        b.inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        b.inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_COMPACTION;
        b.inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        b.inputs.NumDescs = (UINT)b.geometries.size();
        b.inputs.pGeometryDescs = b.geometries.data();
        m_device.d3d()->GetRaytracingAccelerationStructurePrebuildInfo(&b.inputs, &b.sizes);
        if (b.sizes.ResultDataMaxSizeInBytes == 0) fail("RayScene: empty BLAS prebuild size for mesh %u", b.mesh);
        b.offset = poolBytes;
        poolBytes += alignUp(b.sizes.ResultDataMaxSizeInBytes, kAsAlign);
    }
    m_stats.meshBlasBytesBeforeCompaction = poolBytes;
    Buffer buildPool = createBuffer(poolBytes, false, true, L"RT mesh BLAS build pool");
    Buffer sizes = createBuffer(builds.size() * 8, true, false, L"RT BLAS compacted sizes");

    // Build in batches whose scratch fits kScratchBatchBytes; one scratch buffer reused across batches.
    uint64_t maxScratch = 0;
    for (const Build& b : builds) maxScratch = std::max(maxScratch, alignUp(b.sizes.ScratchDataSizeInBytes, kAsAlign));
    Buffer scratch = createBuffer(std::max(maxScratch, std::min<uint64_t>(kScratchBatchBytes, [&] {
                                      uint64_t t = 0;
                                      for (const Build& b : builds) t += alignUp(b.sizes.ScratchDataSizeInBytes, kAsAlign);
                                      return t;
                                  }())),
                                  true, false, L"RT BLAS scratch");
    CommandList cl = m_device.acquireCommandList(QueueType::Graphics);
    uint64_t scratchUsed = 0;
    for (Build& b : builds)
    {
        const uint64_t need = alignUp(b.sizes.ScratchDataSizeInBytes, kAsAlign);
        if (scratchUsed + need > scratch.bytes)
        {
            globalBarrier(cl.list.Get(), kSyncBuild, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, kSyncBuild, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
            scratchUsed = 0;
        }
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
        d.Inputs = b.inputs;
        d.DestAccelerationStructureData = buildPool.address() + b.offset;
        d.ScratchAccelerationStructureData = scratch.address() + scratchUsed;
        cl.list->BuildRaytracingAccelerationStructure(&d, 0, nullptr);
        scratchUsed += need;
    }
    globalBarrier(cl.list.Get(), kSyncBuild, kAsWrite, D3D12_BARRIER_SYNC_EMIT_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO, kAsRead);
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> sources;
    for (const Build& b : builds) sources.push_back(buildPool.address() + b.offset);
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_DESC post{ sizes.address(), D3D12_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO_COMPACTED_SIZE };
    cl.list->EmitRaytracingAccelerationStructurePostbuildInfo(&post, (UINT)sources.size(), sources.data());
    globalBarrier(cl.list.Get(), D3D12_BARRIER_SYNC_EMIT_RAYTRACING_ACCELERATION_STRUCTURE_POSTBUILD_INFO, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_SYNC_COPY,
                  D3D12_BARRIER_ACCESS_COPY_SOURCE);
    D3D12_HEAP_PROPERTIES rb{ D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC1 rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = builds.size() * 8;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    check(m_device.d3d()->CreateCommittedResource3(&rb, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&readback)),
          "RT compaction readback");
    cl.list->CopyBufferRegion(readback.Get(), 0, sizes.resource.Get(), 0, builds.size() * 8);
    m_device.queue(QueueType::Graphics).waitCpu(m_device.submit(cl));

    std::vector<uint64_t> compacted(builds.size());
    void* mapped = nullptr;
    D3D12_RANGE all{ 0, builds.size() * 8 };
    check(readback->Map(0, &all, &mapped), "map compaction sizes");
    std::memcpy(compacted.data(), mapped, builds.size() * 8);
    D3D12_RANGE none{ 0, 0 };
    readback->Unmap(0, &none);

    uint64_t compactBytes = 0;
    std::vector<uint64_t> compactOffset(builds.size());
    for (size_t k = 0; k < builds.size(); ++k)
    {
        if (compacted[k] == 0 || compacted[k] > builds[k].sizes.ResultDataMaxSizeInBytes) fail("RayScene: invalid compacted BLAS size %llu", (unsigned long long)compacted[k]);
        compactOffset[k] = compactBytes;
        compactBytes += alignUp(compacted[k], kAsAlign);
    }
    m_meshBlasPool = createBuffer(compactBytes, false, true, L"RT mesh BLAS pool");
    CommandList copy = m_device.acquireCommandList(QueueType::Graphics);
    for (size_t k = 0; k < builds.size(); ++k)
    {
        copy.list->CopyRaytracingAccelerationStructure(m_meshBlasPool.address() + compactOffset[k], buildPool.address() + builds[k].offset,
                                                      D3D12_RAYTRACING_ACCELERATION_STRUCTURE_COPY_MODE_COMPACT);
        m_meshBlas[builds[k].mesh].offset = compactOffset[k];
    }
    m_device.queue(QueueType::Graphics).waitCpu(m_device.submit(copy));
    m_stats.meshBlasBytes = compactBytes;
    release(buildPool);
    release(scratch);
    release(sizes);
}

const std::vector<RayScene::ProxyMesh>& RayScene::proxyLevels(uint32_t mesh)
{
    std::vector<ProxyMesh>& levels = m_proxyLevels[mesh];
    if (m_proxyLevelsBuilt[mesh]) return levels;
    m_proxyLevelsBuilt[mesh] = 1;
    const ClusterData& cd = m_scene.clusters();
    if (mesh < cd.meshes.size() && cd.meshes[mesh].lodLevelCount > 0)
    {
        const auto& range = cd.meshes[mesh];
        // Levels run finest to coarsest: every cut within the budget, else the coarsest alone.
        for (uint32_t l = range.lodLevelOffset; l < range.lodLevelOffset + range.lodLevelCount; ++l)
            if (cd.lodLevels[l].triangleCount <= m_proxyBudget) levels.push_back(buildProxy(mesh, l));
        if (levels.empty()) levels.push_back(buildProxy(mesh, range.lodLevelOffset + range.lodLevelCount - 1));
    }
    else
        levels.push_back(buildProxy(mesh, gpu::kNone));
    return levels;
}

RayScene::ProxyMesh RayScene::buildProxy(uint32_t mesh, uint32_t lodLevel)
{
    ProxyMesh p;
    const scene::Mesh& sm = m_scene.source()->meshes[mesh];
    const ClusterData& cd = m_scene.clusters();
    // Per-submesh index lists in mesh vertex indices, from the cut or the full mesh.
    std::vector<std::vector<uint32_t>> perSubmesh(sm.submeshes.size());
    if (lodLevel != gpu::kNone)
    {
        const gpu::LodLevel& level = cd.lodLevels[lodLevel];
        p.reduced = level.error > 0;
        p.error = level.error;
        for (uint32_t k = level.clusterOffset; k < level.clusterOffset + level.clusterCount; ++k)
        {
            const gpu::Cluster& c = cd.clusters[cd.lodLevelClusters[k]];
            const uint32_t triangles = (c.counts >> 8) & 0xFFu, submesh = c.counts >> 16;
            for (uint32_t t = 0; t < triangles; ++t)
            {
                const uint32_t packed = cd.clusterTriangles[c.triangleOffset + t];
                for (uint32_t v : { packed & 0xFFu, (packed >> 8) & 0xFFu, (packed >> 16) & 0xFFu })
                    perSubmesh[submesh].push_back(cd.clusterVertexIndices[c.vertexOffset + v]);
            }
        }
    }
    else
        for (uint32_t s = 0; s < (uint32_t)sm.submeshes.size(); ++s)
            perSubmesh[s].assign(sm.indices.begin() + sm.submeshes[s].indexOffset, sm.indices.begin() + sm.submeshes[s].indexOffset + sm.submeshes[s].indexCount);
    // Compact vertices (first use order) and R's pools.
    std::unordered_map<uint32_t, uint32_t> compact;
    p.vertexMap = (uint32_t)m_vertexMapData.size();
    p.geometryBase = (uint32_t)m_geometries.size();
    for (uint32_t s = 0; s < (uint32_t)perSubmesh.size(); ++s)
    {
        if (perSubmesh[s].empty()) continue;
        p.submesh.push_back(s);
        p.indexOffset.push_back((uint32_t)m_indexPoolData.size());
        p.indexCount.push_back((uint32_t)perSubmesh[s].size());
        m_geometries.push_back({ p.indexOffset.back(), s, kRtGeometryProxyIndices, p.vertexMap });
        for (uint32_t v : perSubmesh[s])
        {
            auto [it, added] = compact.try_emplace(v, (uint32_t)(m_vertexMapData.size() - p.vertexMap));
            if (added) m_vertexMapData.push_back(v);
            m_indexPoolData.push_back(it->second);
        }
        p.triangles += (uint32_t)perSubmesh[s].size() / 3;
    }
    p.vertexCount = (uint32_t)(m_vertexMapData.size() - p.vertexMap);
    return p;
}

// BLAS geometry of a deformed instance's proxy cut (positions from the deformed pool at vertexBase).
std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> RayScene::proxyGeometry(uint32_t sceneInstance, uint32_t mesh, const ProxyMesh& p, uint32_t vertexBase) const
{
    const scene::Scene* src = m_scene.source();
    const scene::Mesh& sm = src->meshes[mesh];
    const scene::Instance& si = src->instances[sceneInstance];
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> out;
    for (size_t k = 0; k < p.submesh.size(); ++k)
    {
        const uint32_t s = p.submesh[k];
        const uint32_t material = s < si.materialOverrides.size() ? si.materialOverrides[s] : sm.submeshes[s].material;
        const bool alpha = material < src->materials.size() && src->materials[material].alphaCutoff > 0;
        D3D12_RAYTRACING_GEOMETRY_DESC g{};
        g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        g.Flags = alpha ? D3D12_RAYTRACING_GEOMETRY_FLAG_NONE : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        g.Triangles.VertexBuffer = { m_deformedPool.address() + (uint64_t)vertexBase * sizeof(RtDeformedVertex), sizeof(RtDeformedVertex) };
        g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        g.Triangles.VertexCount = p.vertexCount;
        g.Triangles.IndexBuffer = m_indexPool.address() + (uint64_t)p.indexOffset[k] * sizeof(uint32_t);
        g.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        g.Triangles.IndexCount = p.indexCount[k];
        out.push_back(g);
    }
    return out;
}

uint32_t RayScene::maxMeshVertices() const
{
    uint32_t v = 0;
    for (const Deformed& d : m_deformed) v = std::max(v, m_scene.meshes()[d.mesh].vertexCount);
    return v;
}

// The deformed instance's full mesh as BLAS geometry (scene indices, positions from the deformed pool at vertexBase).
std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> RayScene::originalGeometry(const Deformed& d, uint32_t vertexBase) const
{
    const scene::Scene* src = m_scene.source();
    const gpu::Mesh& gm = m_scene.meshes()[d.mesh];
    const scene::Mesh& sm = src->meshes[d.mesh];
    const scene::Instance& si = src->instances[d.sceneInstance];
    const D3D12_GPU_VIRTUAL_ADDRESS indices = m_scene.buffer("indices")->GetGPUVirtualAddress();
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> out;
    for (uint32_t s = 0; s < (uint32_t)sm.submeshes.size(); ++s)
    {
        if (sm.submeshes[s].indexCount == 0) continue;
        const uint32_t material = s < si.materialOverrides.size() ? si.materialOverrides[s] : sm.submeshes[s].material;
        const bool alpha = material < src->materials.size() && src->materials[material].alphaCutoff > 0;
        D3D12_RAYTRACING_GEOMETRY_DESC g{};
        g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        g.Flags = alpha ? D3D12_RAYTRACING_GEOMETRY_FLAG_NONE : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        g.Triangles.VertexBuffer = { (m_deformedPool.resource ? m_deformedPool.address() : 0) + (uint64_t)vertexBase * sizeof(RtDeformedVertex), sizeof(RtDeformedVertex) };
        g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        g.Triangles.VertexCount = gm.vertexCount;
        g.Triangles.IndexBuffer = indices + ((uint64_t)gm.indexOffset + sm.submeshes[s].indexOffset) * sizeof(uint32_t);
        g.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        g.Triangles.IndexCount = sm.submeshes[s].indexCount;
        out.push_back(g);
    }
    return out;
}

void RayScene::buildDeformed()
{
    m_deformedPool = createBuffer(m_poolVertices * sizeof(RtDeformedVertex), true, false, L"RT deformed vertices");
    {
        DescriptorHeaps& h = m_device.descriptors();
        m_deformedPool.srv = h.allocateResource();
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements = (UINT)m_poolVertices;
        sd.Buffer.StructureByteStride = sizeof(RtDeformedVertex);
        m_device.d3d()->CreateShaderResourceView(m_deformedPool.resource.Get(), &sd, h.resourceCpu(m_deformedPool.srv));
    }
    if (m_deformed.empty())
    {
        m_deformedBlasPool = createBuffer(kAsAlign, false, true, L"RT deformed BLAS pool (empty)");
        return;
    }
    std::vector<DeformJob> jobs;
    std::vector<uint32_t> groups;  // uint2 pairs
    uint64_t poolBytes = 0, scratchBytes = 0;
    for (Deformed& d : m_deformed)
    {
        const std::vector<ProxyMesh>& levels = m_proxyLevels[d.mesh];
        // BLAS and scratch ranges fit every cut of the mesh (the instance switches between them).
        uint64_t result = 0, scratch = 0;
        for (const ProxyMesh& p : levels)
        {
            const std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometry = proxyGeometry(d.sceneInstance, d.mesh, p, d.vertexBase);
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
            inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
            inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            inputs.NumDescs = (UINT)geometry.size();
            inputs.pGeometryDescs = geometry.data();
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO sizes{};
            m_device.d3d()->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &sizes);
            result = std::max(result, sizes.ResultDataMaxSizeInBytes);
            scratch = std::max(scratch, std::max(sizes.ScratchDataSizeInBytes, sizes.UpdateScratchDataSizeInBytes));
        }
        d.geometries = proxyGeometry(d.sceneInstance, d.mesh, levels[d.level], d.vertexBase);
        d.blasOffset = poolBytes;
        poolBytes += alignUp(result, kAsAlign);
        d.scratchOffset = scratchBytes;
        scratchBytes += alignUp(scratch, kAsAlign);

        // One job per deformed instance (its index = the deformed index); groups cover the finest cut's vertices.
        jobs.push_back({ d.sceneInstance, d.vertexBase, levels[d.level].vertexMap, d.vertexCount });
        const uint32_t job = (uint32_t)jobs.size() - 1;
        for (uint32_t first = 0; first < levels[0].vertexCount; first += 64) groups.insert(groups.end(), { job, first });
    }
    // Exact slots: BLAS and scratch for the largest full skinned mesh, a job (idle until owned) and groups for its capacity.
    {
        uint64_t result = 0, scratch = 0;
        for (const Deformed& d : m_deformed)
        {
            const std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> full = originalGeometry(d, 0);
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
            inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
            inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            inputs.NumDescs = (UINT)full.size();
            inputs.pGeometryDescs = full.data();
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO sizes{};
            m_device.d3d()->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &sizes);
            result = std::max(result, sizes.ResultDataMaxSizeInBytes);
            scratch = std::max(scratch, std::max(sizes.ScratchDataSizeInBytes, sizes.UpdateScratchDataSizeInBytes));
        }
        const uint32_t capacity = maxMeshVertices();
        for (ExactSlot& e : m_exact)
        {
            e.blasOffset = poolBytes;
            poolBytes += alignUp(result, kAsAlign);
            e.scratchOffset = scratchBytes;
            scratchBytes += alignUp(scratch, kAsAlign);
            e.job = (uint32_t)jobs.size();
            jobs.push_back({ 0, e.vertexBase, gpu::kNone, 0 });
            for (uint32_t first = 0; first < capacity; first += 64) groups.insert(groups.end(), { e.job, first });
        }
        m_exactRebuild.assign(m_exact.size(), 0);
    }
    m_deformedBlasPool = createBuffer(poolBytes, false, true, L"RT deformed BLAS pool");
    m_deformedScratch = createBuffer(scratchBytes, true, false, L"RT deformed BLAS scratch");
    m_deformJobs = createStructured(jobs.data(), sizeof(DeformJob), (uint32_t)jobs.size(), L"RT deform jobs");
    m_deformGroups = createStructured(groups.data(), 8, (uint32_t)(groups.size() / 2), L"RT deform groups");
    m_deformGroupCount = (uint32_t)(groups.size() / 2);
    m_stats.deformedBlasBytes = poolBytes;
    // UAV of the deformed pool for the deform kernel.
    DescriptorHeaps& h = m_device.descriptors();
    m_deformedPoolUav = h.allocateResource();
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = (UINT)m_poolVertices;
    ud.Buffer.StructureByteStride = sizeof(RtDeformedVertex);
    m_device.d3d()->CreateUnorderedAccessView(m_deformedPool.resource.Get(), nullptr, &ud, h.resourceCpu(m_deformedPoolUav));
}

void RayScene::buildStaticTlas()
{
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs = (UINT)m_staticDescs.size();
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO sizes{};
    m_device.d3d()->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &sizes);
    m_tlasStatic = createBuffer(sizes.ResultDataMaxSizeInBytes, false, true, L"RT static TLAS");
    m_stats.tlasStaticBytes = m_tlasStatic.bytes;
    Buffer scratch = createBuffer(sizes.ScratchDataSizeInBytes, true, false, L"RT static TLAS scratch");
    m_staticDescBuffer = createBuffer(m_staticDescs.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), false, false, L"RT static instance descs");
    upload(m_staticDescBuffer, m_staticDescs.data(), m_staticDescs.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
    inputs.InstanceDescs = m_staticDescBuffer.address();
    CommandList cl = m_device.acquireCommandList(QueueType::Graphics);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
    d.Inputs = inputs;
    d.DestAccelerationStructureData = m_tlasStatic.address();
    d.ScratchAccelerationStructureData = scratch.address();
    cl.list->BuildRaytracingAccelerationStructure(&d, 0, nullptr);
    m_device.queue(QueueType::Graphics).waitCpu(m_device.submit(cl));
    m_staticScratch = scratch;  // kept for in-frame rebuilds (visibility or transform changes of static instances)
}

void RayScene::recordDeform(ID3D12GraphicsCommandList7* cmd) const
{
    // The caller bound the frame constants (root CBV b1): deformVertex reads time, palettes and scene buffers.
    cmd->SetPipelineState(m_shaders.compute("RayTracing/Deform"));
    const uint32_t width = std::min<uint32_t>(m_deformGroupCount, 65535u);
    const uint32_t constants[8] = { m_deformJobs.srv, m_deformGroups.srv, m_deformedPoolUav, m_deformGroupCount, m_vertexMap.srv, width, 0, 0 };
    cmd->SetComputeRoot32BitConstants(0, 8, constants, 0);
    cmd->Dispatch(width, (m_deformGroupCount + width - 1) / width, 1);
}

void RayScene::recordRefit(ID3D12GraphicsCommandList7* cmd, bool refit, const std::vector<uint8_t>* rebuild) const
{
    // Refit (or first build) of every deformed BLAS; each has its own scratch range, so the builds run concurrently. An
    // instance whose proxy cut changed is built (its triangles changed).
    for (size_t k = 0; k < m_deformed.size(); ++k)
    {
        const Deformed& dd = m_deformed[k];
        const bool update = refit && (m_experiment & 1) == 0 && !(rebuild && k < rebuild->size() && (*rebuild)[k]);
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
        inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
        inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        inputs.NumDescs = (UINT)dd.geometries.size();
        inputs.pGeometryDescs = dd.geometries.data();
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
        d.Inputs = inputs;
        d.DestAccelerationStructureData = m_deformedBlasPool.address() + dd.blasOffset;
        if (update)
        {
            d.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
            d.SourceAccelerationStructureData = d.DestAccelerationStructureData;
        }
        d.ScratchAccelerationStructureData = m_deformedScratch.address() + dd.scratchOffset;
        cmd->BuildRaytracingAccelerationStructure(&d, 0, nullptr);
    }
}

void RayScene::recordExactBuilds(ID3D12GraphicsCommandList7* cmd, const std::vector<uint8_t>& rebuild) const
{
    // Occupied exact slots: full build for a new owner, refit otherwise (each its own scratch range).
    for (size_t k = 0; k < m_exact.size(); ++k)
    {
        const ExactSlot& e = m_exact[k];
        if (e.owner == 0xFFFFFFFFu) continue;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
        d.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        d.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
        d.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        d.Inputs.NumDescs = (UINT)e.geometries.size();
        d.Inputs.pGeometryDescs = e.geometries.data();
        d.DestAccelerationStructureData = m_deformedBlasPool.address() + e.blasOffset;
        if (!rebuild[k])
        {
            d.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
            d.SourceAccelerationStructureData = d.DestAccelerationStructureData;
        }
        d.ScratchAccelerationStructureData = m_deformedScratch.address() + e.scratchOffset;
        cmd->BuildRaytracingAccelerationStructure(&d, 0, nullptr);
    }
}

// Reads back the hit counts of the frame framesInFlight ago and assigns the exact slots (current owners keep theirs, so
// a slot is rebuilt only when its owner changes). Patches go through a small upload ring copied in the frame.
void RayScene::selectExactSet(FramePassContext& fc)
{
    std::fill(m_exactRebuild.begin(), m_exactRebuild.end(), 0);
    m_stats.exactBuilds = 0;
    if (m_exact.empty()) return;
    const uint32_t n = (uint32_t)m_deformed.size();
    const uint32_t oldSlot = (uint32_t)((fc.frame.frameIndex + kDescSlots - fc.framesInFlight) % kDescSlots);
    std::vector<uint32_t> wanted;
    if (m_exactSlotFrame[oldSlot] != UINT64_MAX && fc.frame.frameIndex >= m_exactSlotFrame[oldSlot] + fc.framesInFlight)
    {
        const uint32_t* counts = m_exactReadbackMapped + (size_t)oldSlot * n;
        for (uint32_t k = 0; k < n; ++k)
            if (counts[k] >= m_exactMinHits) wanted.push_back(k);
        std::sort(wanted.begin(), wanted.end(), [&](uint32_t a, uint32_t b) { return counts[a] != counts[b] ? counts[a] > counts[b] : a < b; });
        if (wanted.size() > m_exact.size()) wanted.resize(m_exact.size());
    }
    else
        for (const ExactSlot& e : m_exact)
            if (e.owner != 0xFFFFFFFFu) wanted.push_back(e.owner);  // no new counts yet: keep the set
    // Keep owners still wanted; free the others; give free slots to the new ones.
    std::vector<uint8_t> placed(wanted.size(), 0);
    for (ExactSlot& e : m_exact)
    {
        const auto it = std::find(wanted.begin(), wanted.end(), e.owner);
        if (e.owner != 0xFFFFFFFFu && it != wanted.end()) placed[it - wanted.begin()] = 1;
        else e.owner = 0xFFFFFFFFu;
    }
    std::vector<std::pair<uint32_t, uint32_t>> changed;  // slot, new owner
    for (size_t w = 0; w < wanted.size(); ++w)
    {
        if (placed[w]) continue;
        for (size_t k = 0; k < m_exact.size(); ++k)
            if (m_exact[k].owner == 0xFFFFFFFFu && std::none_of(changed.begin(), changed.end(), [&](const auto& c) { return c.first == k; }))
            {
                changed.push_back({ (uint32_t)k, wanted[w] });
                break;
            }
    }
    m_stats.exactOccupied = 0;
    m_stats.exactVertices = 0;
    // Patch records: RtInstance (16 B) and DeformJob (16 B) of each changed slot; a slot freed this frame keeps its data.
    struct Patch
    {
        uint64_t dst;
        bool job;
        uint32_t words[4];
    };
    std::vector<Patch> patches;
    for (const auto& [slot, owner] : changed)
    {
        ExactSlot& e = m_exact[slot];
        const Deformed& d = m_deformed[owner];
        e.owner = owner;
        e.geometries = originalGeometry(d, e.vertexBase);
        m_exactRebuild[slot] = 1;
        ++m_stats.exactBuilds;
        patches.push_back({ (uint64_t)e.record * sizeof(RtInstance), false, { d.sceneInstance, d.originalGeometryBase, e.vertexBase, kRtInstanceDeformed | (owner << 8) } });
        patches.push_back({ (uint64_t)e.job * 16, true, { d.sceneInstance, e.vertexBase, gpu::kNone, m_scene.meshes()[d.mesh].vertexCount } });
    }
    for (const ExactSlot& e : m_exact)
        if (e.owner != 0xFFFFFFFFu)
        {
            ++m_stats.exactOccupied;
            m_stats.exactVertices += m_scene.meshes()[m_deformed[e.owner].mesh].vertexCount;
        }
    // Freed slots stop deforming: their job's vertex count is set to 0.
    for (size_t k = 0; k < m_exact.size(); ++k)
        if (m_exact[k].owner == 0xFFFFFFFFu) patches.push_back({ (uint64_t)m_exact[k].job * 16, true, { 0, m_exact[k].vertexBase, gpu::kNone, 0 } });
    if (patches.empty()) return;
    const uint64_t ringOffset = (fc.frame.frameIndex % kDescSlots) * kPatchSlotBytes;
    if (patches.size() * 16 > kPatchSlotBytes) fail("RayScene: exact set patches exceed the patch ring");
    for (size_t k = 0; k < patches.size(); ++k) std::memcpy(m_patchRingMapped + ringOffset + k * 16, patches[k].words, 16);
    const BufferRef instancesRef = m_frame.instances, jobsRef = m_frame.jobs;
    ID3D12Resource* ring = m_patchRing.Get();
    fc.graph.addPass("r.as.exact.patch", QueueType::Graphics,
                     [&](PassBuilder& b) {
                         b.use(instancesRef, Use::CopyDst);
                         b.use(jobsRef, Use::CopyDst);
                     },
                     [patches, ring, ringOffset, instancesRef, jobsRef](PassContext& c) {
                         for (size_t k = 0; k < patches.size(); ++k)
                             c.cmd->CopyBufferRegion(c.resource(patches[k].job ? jobsRef : instancesRef), patches[k].dst, ring, ringOffset + k * 16, 16);
                     });
}

void RayScene::selectProxyLevels(FramePassContext& fc)
{
    m_deformedRebuild.assign(m_deformed.size(), 0);
    m_stats.proxySwitches = 0;
    if (m_deformed.empty()) return;
    const ViewDesc& view = fc.frame.mainView;
    const float pixelAngle = 2 * std::tan(view.verticalFov * 0.5f) / (float)std::max(view.height, 1u);
    const auto& instances = m_scene.instances();
    const auto& meshes = m_scene.meshes();
    struct Patch
    {
        uint64_t dst;
        bool job;
        uint32_t words[4];
    };
    std::vector<Patch> patches;
    const size_t patchCapacity = (kPatchSlotBytes - kProxyPatchOffset) / 16;
    uint64_t triangles = 0;
    for (size_t k = 0; k < m_deformed.size(); ++k)
    {
        Deformed& d = m_deformed[k];
        const std::vector<ProxyMesh>& levels = m_proxyLevels[d.mesh];
        if (levels.size() > 1 && patches.size() + 2 <= patchCapacity)
        {
            const gpu::Instance& in = instances[d.sceneInstance];
            const gpu::Mesh& gm = meshes[d.mesh];
            float scale = 0;
            auto at = [&](int r, int c) { const float4& v = in.objectToWorld[r]; return c == 0 ? v.x : c == 1 ? v.y : v.z; };
            for (int c = 0; c < 3; ++c)
            {
                const float x = at(0, c), y = at(1, c), z = at(2, c);
                scale = std::max(scale, std::sqrt(x * x + y * y + z * z));
            }
            const float3 centre{ in.objectToWorld[0].x * gm.boundsSphere.x + in.objectToWorld[0].y * gm.boundsSphere.y + in.objectToWorld[0].z * gm.boundsSphere.z + in.objectToWorld[0].w,
                                 in.objectToWorld[1].x * gm.boundsSphere.x + in.objectToWorld[1].y * gm.boundsSphere.y + in.objectToWorld[1].z * gm.boundsSphere.z + in.objectToWorld[1].w,
                                 in.objectToWorld[2].x * gm.boundsSphere.x + in.objectToWorld[2].y * gm.boundsSphere.y + in.objectToWorld[2].z * gm.boundsSphere.z + in.objectToWorld[2].w };
            const float3 toEye{ centre.x - view.position.x, centre.y - view.position.y, centre.z - view.position.z };
            // A skinned pose can leave the bind-pose sphere: the distance is from its doubled radius (conservative).
            const float distance = std::max(std::sqrt(toEye.x * toEye.x + toEye.y * toEye.y + toEye.z * toEye.z) - 2 * gm.boundsSphere.w * scale, 0.0f);
            const float bound = m_proxyErrorPx * pixelAngle * distance;
            uint32_t level = d.level;
            while (level > 0 && levels[level].error * scale > bound) --level;  // finer at once
            while (level + 1 < levels.size() && levels[level + 1].error * scale <= 0.8f * bound) ++level;  // coarser with margin
            if (level != d.level)
            {
                const ProxyMesh& p = levels[level];
                d.level = level;
                d.vertexCount = p.vertexCount;
                d.geometryBase = p.geometryBase;
                d.geometries = proxyGeometry(d.sceneInstance, d.mesh, p, d.vertexBase);
                m_instances[d.record].geometryBase = p.geometryBase;
                m_deformedRebuild[k] = 1;
                ++m_stats.proxySwitches;
                patches.push_back({ (uint64_t)d.record * sizeof(RtInstance), false, { d.sceneInstance, p.geometryBase, d.vertexBase, m_instances[d.record].flags } });
                patches.push_back({ (uint64_t)k * 16, true, { d.sceneInstance, d.vertexBase, p.vertexMap, p.vertexCount } });
            }
        }
        triangles += levels[d.level].triangles;
    }
    m_stats.deformedTriangles = triangles;
    m_stats.deformedVertices = 0;
    for (const Deformed& d : m_deformed) m_stats.deformedVertices += d.vertexCount;
    if (patches.empty()) return;
    const uint64_t ringOffset = (fc.frame.frameIndex % kDescSlots) * kPatchSlotBytes + kProxyPatchOffset;
    for (size_t k = 0; k < patches.size(); ++k) std::memcpy(m_patchRingMapped + ringOffset + k * 16, patches[k].words, 16);
    const BufferRef instancesRef = m_frame.instances, jobsRef = m_frame.jobs;
    ID3D12Resource* ring = m_patchRing.Get();
    fc.graph.addPass("r.as.proxy.patch", QueueType::Graphics,
                     [&](PassBuilder& b) {
                         b.use(instancesRef, Use::CopyDst);
                         b.use(jobsRef, Use::CopyDst);
                     },
                     [patches, ring, ringOffset, instancesRef, jobsRef](PassContext& c) {
                         for (size_t k = 0; k < patches.size(); ++k)
                             c.cmd->CopyBufferRegion(c.resource(patches[k].job ? jobsRef : instancesRef), patches[k].dst, ring, ringOffset + k * 16, 16);
                     });
}

void RayScene::declareVsm(PassBuilder& b, const VsmRefs& v)
{
    if (!v.valid()) return;
    for (const BufferRef& r : { v.pageTable, v.pool, v.blocks, v.searchBound }) b.use(r, Use::SrvGraphics);
    if (v.layers.valid()) b.use(v.layers, Use::SrvGraphics);
}

uint32_t RayScene::vsmSrvs(PassContext& c, const VsmRefs& v, uint64_t frame, uint32_t user)
{
    static_assert(sizeof(m_vsmRingSrv) / sizeof(uint32_t) == kDescSlots * 2);
    if (!v.valid()) return 0xFFFFFFFFu;
    if (!m_vsmRing)
    {
        D3D12_HEAP_PROPERTIES up{ D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC1 d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = kDescSlots * 2 * 32;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(m_device.d3d()->CreateCommittedResource3(&up, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_vsmRing)),
              "RT VSM ring");
        D3D12_RANGE none{ 0, 0 };
        check(m_vsmRing->Map(0, &none, reinterpret_cast<void**>(&m_vsmRingMapped)), "map RT VSM ring");
        DescriptorHeaps& h = m_device.descriptors();
        for (uint32_t k = 0; k < kDescSlots * 2; ++k)
        {
            m_vsmRingSrv[k] = h.allocateResource();
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Format = DXGI_FORMAT_R32_TYPELESS;
            sd.Buffer.FirstElement = k * 8;
            sd.Buffer.NumElements = 8;
            sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            m_device.d3d()->CreateShaderResourceView(m_vsmRing.Get(), &sd, h.resourceCpu(m_vsmRingSrv[k]));
        }
    }
    // Slot of frame % kDescSlots: frame f - kDescSlots has completed (frames in flight <= kDescSlots, record()).
    const uint32_t slot = (uint32_t)(frame % kDescSlots) * 2 + user;
    // ShadowSrvs: { page table, pool, blocks, search bound, constants, lights (none), 0, transmittance layer (UNX_NONE: T = 1) }.
    const uint32_t words[8] = { c.srv(v.pageTable), c.srv(v.pool), c.srv(v.blocks), c.srv(v.searchBound), v.constants, 0xFFFFFFFFu, 0,
                                v.layers.valid() ? c.srv(v.layers) : 0xFFFFFFFFu };
    std::memcpy(m_vsmRingMapped + slot * 32, words, sizeof words);
    return m_vsmRingSrv[slot];
}

void RayScene::recordExactReadback(FramePassContext& fc)
{
    if (m_exact.empty()) return;
    const uint32_t slot = (uint32_t)(fc.frame.frameIndex % kDescSlots);
    const uint64_t bytes = (uint64_t)m_deformed.size() * 4;
    m_exactSlotFrame[slot] = fc.frame.frameIndex;
    const BufferRef counts = m_frame.exactCounts;
    const BufferRef readback = fc.graph.importBuffer(m_exactReadback.Get(), { "RT exact set readback", kDescSlots * bytes, 0 });
    fc.graph.addPass("r.as.exact.readback", QueueType::Graphics,
                     [&](PassBuilder& b) {
                         b.use(counts, Use::CopySrc);
                         b.use(readback, Use::CopyDst);
                         b.keep();
                     },
                     [counts, readback, slot, bytes](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(readback), slot * bytes, c.resource(counts), 0, bytes); });
}

void RayScene::updateDynamic(ID3D12GraphicsCommandList7* cmd, bool refit)
{
    if (!m_deformed.empty())
    {
        recordDeform(cmd);
        globalBarrier(cmd, D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, kSyncBuild, D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
        recordRefit(cmd, refit);
        globalBarrier(cmd, kSyncBuild, kAsWrite, kSyncBuild, kAsRead);
    }
    if (!m_tlasDynamic.resource) recordDynamicTlas(cmd, 0);  // sizes the TLAS and uploads the load-time descriptors
    else recordDynamicTlas(cmd, m_dynamicDescBuffer.address());
    globalBarrier(cmd, kSyncBuild, kAsWrite, kSyncTrace | kSyncBuild, kAsRead);
}

void RayScene::refreshDesc(D3D12_RAYTRACING_INSTANCE_DESC& d, const gpu::Instance& in, bool worldSpace) const
{
    if (!worldSpace)
        for (int r = 0; r < 3; ++r)
        {
            d.Transform[r][0] = in.objectToWorld[r].x;
            d.Transform[r][1] = in.objectToWorld[r].y;
            d.Transform[r][2] = in.objectToWorld[r].z;
            d.Transform[r][3] = in.objectToWorld[r].w;
        }
    d.InstanceMask = (in.flags & gpu::kInstanceHidden) ? 0 : kRtMaskAll;
}

void RayScene::record(FramePassContext& fc)
{
    RenderGraph& g = fc.graph;
    m_frame = {};
    m_frame.tlasStatic = g.importBuffer(m_tlasStatic.resource.Get(), { "RT static TLAS", m_tlasStatic.bytes, 0 });
    m_frame.tlasDynamic = g.importBuffer(m_tlasDynamic.resource.Get(), { "RT dynamic TLAS", m_tlasDynamic.bytes, 0 });
    fc.resources.tlasStatic = m_frame.tlasStatic;
    fc.resources.tlasDynamic = m_frame.tlasDynamic;
    const BufferRef scratch = g.importBuffer(m_tlasScratch.resource.Get(), { "RT dynamic TLAS scratch", m_tlasScratch.bytes, 0 });
    m_frame.instances = g.importBuffer(m_instanceBuffer.resource.Get(), { "RT instances", m_instanceBuffer.bytes, sizeof(RtInstance) });
    if (!m_deformed.empty()) m_frame.jobs = g.importBuffer(m_deformJobs.resource.Get(), { "RT deform jobs", m_deformJobs.bytes, 16 });
    if (!m_exact.empty())
    {
        // Reflection exact set: this frame's hit counts start at 0; the slots follow the counts read back now.
        const BufferRef counts = g.importBuffer(m_exactCounts.resource.Get(), { "RT exact set hit counts", m_exactCounts.bytes, 4 });
        const BufferRef zero = g.importBuffer(m_exactZero.resource.Get(), { "RT exact set zeros", m_exactZero.bytes, 4 });
        m_frame.exactCounts = counts;
        const uint64_t bytes = (uint64_t)m_deformed.size() * 4;
        g.addPass("r.as.exact.clear", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(zero, Use::CopySrc);
                      b.use(counts, Use::CopyDst);
                  },
                  [zero, counts, bytes](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(counts), 0, c.resource(zero), 0, bytes); });
        selectExactSet(fc);
    }
    selectProxyLevels(fc);
    ++m_stats.framesRecorded;
    m_stats.exactBuildsTotal += m_stats.exactBuilds;
    m_stats.exactOccupiedTotal += m_stats.exactOccupied;
    m_stats.exactVerticesTotal += m_stats.exactVertices;
    m_stats.proxySwitchesTotal += m_stats.proxySwitches;
    // This frame's instance descriptors (GpuScene's CPU mirror is current, INTERFACES 6.3 v1.8).
    if (fc.framesInFlight > kDescSlots) fail("RayScene: %u frames in flight exceed %u descriptor slots", fc.framesInFlight, kDescSlots);
    const uint64_t slotOffset = (fc.frame.frameIndex % kDescSlots) * m_descSlotBytes;
    auto* slotDescs = reinterpret_cast<D3D12_RAYTRACING_INSTANCE_DESC*>(m_descRingMapped + slotOffset);
    const auto& sceneInstances = m_scene.instances();
    for (size_t k = 0; k < m_dynamicDescs.size(); ++k)
    {
        const RtInstance& ri = m_instances[m_dynamicRecord[k]];
        refreshDesc(m_dynamicDescs[k], sceneInstances[ri.sceneInstance], (ri.flags & kRtInstanceDeformed) != 0);
        slotDescs[k] = m_dynamicDescs[k];
        if ((ri.flags & kRtInstanceDeformed) == 0) continue;
        // A skinned instance in the reflection exact set is traced with its original mesh (its slot's BLAS and record).
        const uint32_t deformedIndex = ri.flags >> 8;
        for (const ExactSlot& e : m_exact)
            if (e.owner == deformedIndex)
            {
                slotDescs[k].InstanceID = e.record;
                slotDescs[k].AccelerationStructure = m_deformedBlasPool.address() + e.blasOffset;
            }
    }
    const D3D12_GPU_VIRTUAL_ADDRESS dynamicDescs = m_descRing->GetGPUVirtualAddress() + slotOffset;
    bool staticChanged = false;
    for (size_t k = 0; k < m_staticDescs.size(); ++k)
    {
        const gpu::Instance& in = sceneInstances[m_staticScene[k]];
        if (staticKey(in) == m_staticKeys[k]) continue;
        m_staticKeys[k] = staticKey(in);
        refreshDesc(m_staticDescs[k], in, false);
        staticChanged = true;
    }
    if (!m_deformed.empty())
    {
        const BufferRef pool = g.importBuffer(m_deformedPool.resource.Get(), { "RT deformed vertices", m_deformedPool.bytes, 0 });
        const BufferRef blas = g.importBuffer(m_deformedBlasPool.resource.Get(), { "RT deformed BLAS pool", m_deformedBlasPool.bytes, 0 });
        const BufferRef blasScratch = g.importBuffer(m_deformedScratch.resource.Get(), { "RT deformed BLAS scratch", m_deformedScratch.bytes, 0 });
        m_frame.deformedBlas = blas;
        m_frame.deformedVertices = pool;
        const D3D12_GPU_VIRTUAL_ADDRESS constants = fc.frameConstantsFor(fc.frame.mainView);
        const BufferRef jobs = m_frame.jobs;
        g.addPass("r.as.deform", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(pool, Use::UavCompute);
                      b.use(jobs, Use::SrvCompute);
                  },
                  [this, constants](PassContext& c) {
                      c.bindFrameConstants(constants);
                      recordDeform(c.cmd);
                  });
        g.addPass("r.as.refit", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(pool, Use::AccelerationStructureInput);
                      b.use(blas, Use::AccelerationStructureWrite);
                      b.use(blas, Use::AccelerationStructureRead);
                      b.use(blasScratch, Use::AccelerationStructureScratch);
                  },
                  [this, rebuild = m_exactRebuild, deformedRebuild = m_deformedRebuild](PassContext& c) {
                      recordRefit(c.cmd, true, &deformedRebuild);
                      recordExactBuilds(c.cmd, rebuild);
                  });
    }
    const Frame frame = m_frame;
    if (staticChanged)
    {
        const uint64_t staticOffset = slotOffset + m_dynamicDescs.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
        std::memcpy(m_descRingMapped + staticOffset, m_staticDescs.data(), m_staticDescs.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
        const D3D12_GPU_VIRTUAL_ADDRESS staticDescs = m_descRing->GetGPUVirtualAddress() + staticOffset;
        const BufferRef staticScratch = g.importBuffer(m_staticScratch.resource.Get(), { "RT static TLAS scratch", m_staticScratch.bytes, 0 });
        g.addPass("r.as.tlas.static", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(frame.tlasStatic, Use::AccelerationStructureWrite);
                      b.use(staticScratch, Use::AccelerationStructureScratch);
                  },
                  [this, staticDescs](PassContext& c) { recordStaticTlas(c.cmd, staticDescs); });
    }
    g.addPass("r.as.tlas.dynamic", QueueType::Compute,
              [&](PassBuilder& b) {
                  if (frame.deformedBlas.valid()) b.use(frame.deformedBlas, Use::AccelerationStructureRead);
                  b.use(frame.tlasDynamic, Use::AccelerationStructureWrite);
                  b.use(scratch, Use::AccelerationStructureScratch);
              },
              [this, dynamicDescs](PassContext& c) { recordDynamicTlas(c.cmd, dynamicDescs); });
}

void RayScene::declareTraversal(PassBuilder& b) const
{
    if (m_frame.instances.valid()) b.use(m_frame.instances, Use::SrvGraphics);  // after this frame's exact set patches
    b.use(m_frame.tlasStatic, Use::AccelerationStructureRead);
    b.use(m_frame.tlasDynamic, Use::AccelerationStructureRead);
    if (m_frame.deformedBlas.valid()) b.use(m_frame.deformedBlas, Use::AccelerationStructureRead);
    if (m_frame.deformedVertices.valid()) b.use(m_frame.deformedVertices, Use::SrvGraphics);
}

void RayScene::recordStaticTlas(ID3D12GraphicsCommandList7* cmd, D3D12_GPU_VIRTUAL_ADDRESS descs)
{
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
    d.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    d.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    d.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    d.Inputs.NumDescs = (UINT)m_staticDescs.size();
    d.Inputs.InstanceDescs = descs;
    d.DestAccelerationStructureData = m_tlasStatic.address();
    d.ScratchAccelerationStructureData = m_staticScratch.address();
    cmd->BuildRaytracingAccelerationStructure(&d, 0, nullptr);
}

void RayScene::recordDynamicTlas(ID3D12GraphicsCommandList7* cmd, D3D12_GPU_VIRTUAL_ADDRESS descs)
{
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs = (UINT)m_dynamicDescs.size();
    if (!m_tlasDynamic.resource)
    {
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO sizes{};
        m_device.d3d()->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &sizes);
        m_tlasDynamic = createBuffer(sizes.ResultDataMaxSizeInBytes, false, true, L"RT dynamic TLAS");
        m_tlasScratch = createBuffer(sizes.ScratchDataSizeInBytes, true, false, L"RT dynamic TLAS scratch");
        m_dynamicDescBuffer = createBuffer(std::max<size_t>(m_dynamicDescs.size(), 1) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), false, false, L"RT dynamic instance descs");
        m_stats.tlasDynamicBytes = m_tlasDynamic.bytes;
        // Load-time descriptors for the out-of-graph path (tests); frames use the per-frame ring (record).
        upload(m_dynamicDescBuffer, m_dynamicDescs.data(), m_dynamicDescs.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
    }
    inputs.InstanceDescs = descs ? descs : m_dynamicDescBuffer.address();
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
    d.Inputs = inputs;
    d.DestAccelerationStructureData = m_tlasDynamic.address();
    d.ScratchAccelerationStructureData = m_tlasScratch.address();
    cmd->BuildRaytracingAccelerationStructure(&d, 0, nullptr);
}

void RayScene::rootConstants(uint32_t out[8]) const
{
    out[0] = m_tlasStaticSrv;
    out[1] = m_tlasDynamicSrv;
    out[2] = m_instanceBuffer.srv;
    out[3] = m_geometryBuffer.srv;
    out[4] = m_indexPool.srv;
    out[5] = m_vertexMap.srv;
    out[6] = m_deformedPool.srv;
    out[7] = 0;
}
} // namespace unx::render::rt
