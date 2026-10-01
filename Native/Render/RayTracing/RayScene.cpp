#include "unx/rt/RayScene.h"
#if defined(UNX_HAS_MATERIAL)
#include "unx/material/MaterialSystem.h"  // M's texture table for decals at hits (recordDecals)
#endif

#include <algorithm>
#include <cfloat>
#include <limits>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
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
    if (slot.scene && slot.scene->sceneRevision() != fc.scene.revision())
    {
        // A new revision: rebuilt from the previous object (its mesh BLASes and static TLAS reused where unchanged, B3).
        std::unique_ptr<RayScene> previous = std::move(slot.scene);
        slot.scene = std::make_unique<RayScene>(fc.device, fc.shaders, fc.scene, fc.quality, previous.get());
    }
    if (!slot.scene) slot.scene = std::make_unique<RayScene>(fc.device, fc.shaders, fc.scene, fc.quality);
    return *slot.scene;
}

RayScene& RayScene::get(Device& device, ShaderLibrary& shaders, GpuScene& scene, const QualityConfig& quality)
{
    std::lock_guard lock(g_sceneMutex);
    auto& slot = g_scenes[{ &device, &scene }];
    if (slot && slot->sceneRevision() != scene.revision())
    {
        std::unique_ptr<RayScene> previous = std::move(slot);  // a new revision: rebuilt from the previous object (B3)
        slot = std::make_unique<RayScene>(device, shaders, scene, quality, previous.get());
    }
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
    // No CPU wait (B3: each upload's wait added up to milliseconds per rebuild): later work on the graphics queue runs
    // after the copy, and the constructor's last build waits for the queue before the object is used; the staging buffer
    // is released when the GPU is done with it.
    CommandList cl = m_device.acquireCommandList(QueueType::Graphics);
    cl.list->CopyBufferRegion(target.resource.Get(), 0, staging.Get(), 0, bytes);
    m_device.submit(cl);
    m_device.deferRelease(staging);
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
    for (Buffer& b : m_inheritedPools) release(b);
    for (Buffer* b : { &m_meshBlasPool, &m_emitterAabbs, &m_emitterBlas, &m_deformedPool, &m_deformedBlasPool, &m_deformedScratch, &m_deformJobs, &m_deformGroups, &m_instanceBuffer, &m_geometryBuffer,
                       &m_indexPool, &m_vertexMap, &m_tlasStatic, &m_tlasDynamic, &m_tlasScratch, &m_staticDescBuffer, &m_dynamicDescBuffer, &m_staticScratch,
                       &m_exactCounts, &m_exactZero, &m_runtimePool, &m_runtimeScratch, &m_decalAabbs, &m_decalBlas, &m_decalBlasScratch, &m_decalTlas, &m_decalTlasScratch, &m_decalDesc })
        release(*b);
    if (m_decalTlasSrv != 0xFFFFFFFFu)
    {
        DescriptorHeaps* dh = &m_device.descriptors();
        const uint32_t srv = m_decalTlasSrv;
        m_device.deferCall([dh, srv] { dh->freeResource(srv); });
    }
    if (m_exactReadback) m_exactReadback->Unmap(0, nullptr);
    m_device.deferRelease(m_exactReadback);
    if (m_patchRing) m_patchRing->Unmap(0, nullptr);
    if (m_runtimeRing) m_runtimeRing->Unmap(0, nullptr);
    m_device.deferRelease(m_runtimeRing);
    m_device.deferRelease(m_patchRing);
    static_assert(sizeof(m_lightRingSrv) / sizeof(uint32_t) == kDescSlots && sizeof(m_lightSlotVersion) / sizeof(uint64_t) == kDescSlots);
    if (m_emissive)
    {
        m_device.deferRelease(m_emissive);
        DescriptorHeaps* eh = &m_device.descriptors();
        const uint32_t srv = m_emissiveSrv;
        m_device.deferCall([eh, srv] { eh->freeResource(srv); });
    }
    if (m_lightRing)
    {
        m_lightRing->Unmap(0, nullptr);
        m_device.deferRelease(m_lightRing);
        DescriptorHeaps* lh = &m_device.descriptors();
        for (uint32_t srv : m_lightRingSrv) m_device.deferCall([lh, srv] { lh->freeResource(srv); });
    }
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

namespace
{
// World AABB of a mesh's bounding sphere under a row-major affine transform (uniform scale: INTERFACES 6.2).
std::pair<float3, float3> sphereBounds(const float4 rows[3], const float4& sphere)
{
    const float3 c{ rows[0].x * sphere.x + rows[0].y * sphere.y + rows[0].z * sphere.z + rows[0].w,
                    rows[1].x * sphere.x + rows[1].y * sphere.y + rows[1].z * sphere.z + rows[1].w,
                    rows[2].x * sphere.x + rows[2].y * sphere.y + rows[2].z * sphere.z + rows[2].w };
    const float s = std::sqrt(rows[0].x * rows[0].x + rows[1].x * rows[1].x + rows[2].x * rows[2].x), r = sphere.w * s;
    return { c - float3{ r, r, r }, c + float3{ r, r, r } };
}
std::pair<float3, float3> descBounds(const D3D12_RAYTRACING_INSTANCE_DESC& d, const float4& sphere)
{
    float4 rows[3];
    for (int i = 0; i < 3; ++i) rows[i] = { d.Transform[i][0], d.Transform[i][1], d.Transform[i][2], d.Transform[i][3] };
    return sphereBounds(rows, sphere);
}
} // namespace

uint32_t RayScene::alphaMaskOf(uint32_t mesh) const
{
    const scene::Scene* src = m_scene.source();
    const scene::Mesh& sm = src->meshes[mesh];
    uint32_t mask = 0;
    for (uint32_t s = 0; s < (uint32_t)sm.submeshes.size() && s < 32; ++s)
    {
        const uint32_t mat = sm.submeshes[s].material;
        if (mat < src->materials.size() && src->materials[mat].alphaCutoff > 0) mask |= 1u << s;
    }
    return mask;
}

RayScene::RayScene(Device& device, ShaderLibrary& shaders, GpuScene& scene, const QualityConfig& quality, RayScene* previous)
    : m_device(device), m_shaders(shaders), m_scene(scene)
{
    const auto t0 = std::chrono::steady_clock::now();
    if (device.caps().raytracingTier < D3D12_RAYTRACING_TIER_1_1) fail("RayScene: DXR tier 1.1 required");
    const scene::Scene* src = scene.source();
    if (!src) fail("RayScene: GpuScene has no uploaded scene");
    m_sceneRevision = scene.revision();
    const uint32_t proxyBudget = (uint32_t)quality.integer("raytracing.character_proxy_triangles");
    m_proxyErrorPx = (float)quality.number("raytracing.proxy_error_px");
    m_proxySkinWeight = (float)quality.number("raytracing.proxy_skin_weight");
    m_proxyPosedFactor = (float)quality.number("raytracing.proxy_posed_factor");
    m_emittersEnabled = quality.boolean("raytracing.emitters");
    {
        const std::string model = quality.string("raytracing.proxy_error_model");
        if (model != "measured" && model != "bound") fail("raytracing.proxy_error_model must be \"measured\" or \"bound\" (got \"%s\")", model.c_str());
        m_proxyErrorBound = model == "bound";
    }
    {
        const std::string cuts = quality.string("raytracing.skinned_proxy_cuts");
        if (cuts != "skin_aware" && cuts != "cluster_lod") fail("raytracing.skinned_proxy_cuts must be \"skin_aware\" or \"cluster_lod\" (got \"%s\")", cuts.c_str());
        m_skinAwareCuts = cuts == "skin_aware";
    }
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
    m_meshRecords = meshes;
    m_materialRecords = scene.materials();
    m_lightRecords = scene.lights();
    m_instanceRecords = instances;
    for (uint32_t m = 0; m < (uint32_t)meshes.size(); ++m) m_meshBlas[m].alphaMask = alphaMaskOf(m);
    m_proxyCache.assign(meshes.size(), {});
    if (previous)
    {
        for (uint32_t m = 0; m < (uint32_t)std::min(meshes.size(), previous->m_meshRecords.size()); ++m)
            if (m < previous->m_proxyCache.size() && std::memcmp(&previous->m_meshRecords[m], &meshes[m], sizeof(gpu::Mesh)) == 0)
                m_proxyCache[m] = std::move(previous->m_proxyCache[m]);
        // B3: inherit the previous object's BLAS of every mesh whose record and alpha layout are unchanged.
        m_incremental = true;
        m_materialsChanged = previous->m_materialRecords.size() != m_materialRecords.size() ||
                             (m_materialRecords.size() && std::memcmp(previous->m_materialRecords.data(), m_materialRecords.data(), m_materialRecords.size() * sizeof(gpu::Material)) != 0);
        m_lightsChanged = previous->m_lightRecords.size() != m_lightRecords.size() ||
                          (m_lightRecords.size() && std::memcmp(previous->m_lightRecords.data(), m_lightRecords.data(), m_lightRecords.size() * sizeof(gpu::Light)) != 0);
        for (uint32_t m = 0; m < (uint32_t)std::min(meshes.size(), previous->m_meshRecords.size()); ++m)
        {
            const MeshBlas& old = previous->m_meshBlas[m];
            if (!old.built || old.alphaMask != m_meshBlas[m].alphaMask || std::memcmp(&previous->m_meshRecords[m], &meshes[m], sizeof(gpu::Mesh)) != 0) continue;
            m_meshBlas[m].address = old.address;
            m_meshBlas[m].anyAlpha = old.anyAlpha;
            m_meshBlas[m].built = true;
        }
        m_inheritedPools = std::move(previous->m_inheritedPools);
        if (previous->m_meshBlasPool.resource) m_inheritedPools.push_back(previous->m_meshBlasPool);
        previous->m_meshBlasPool = {};
        // Geometry that changed: instances added, removed (hidden) or changed in mesh, flags or transform.
        const auto& oldInst = previous->m_instanceRecords;
        const auto& oldMeshes = previous->m_meshRecords;
        for (uint32_t i = 0; i < (uint32_t)std::max(instances.size(), oldInst.size()); ++i)
        {
            const bool had = i < oldInst.size() && (oldInst[i].flags & gpu::kInstanceHidden) == 0;
            const bool has = i < instances.size() && (instances[i].flags & gpu::kInstanceHidden) == 0;
            if (had && has && oldInst[i].mesh == instances[i].mesh && oldInst[i].flags == instances[i].flags &&
                std::memcmp(oldInst[i].objectToWorld, instances[i].objectToWorld, sizeof(instances[i].objectToWorld)) == 0)
                continue;
            if (had) m_buildChanges.push_back(sphereBounds(oldInst[i].objectToWorld, oldMeshes[oldInst[i].mesh].boundsSphere));
            if (has) m_buildChanges.push_back(sphereBounds(instances[i].objectToWorld, meshes[instances[i].mesh].boundsSphere));
        }
    }
    buildMeshBlas();
    const auto tMeshBlas = std::chrono::steady_clock::now();

    auto transformOf = [&](const gpu::Instance& in, D3D12_RAYTRACING_INSTANCE_DESC& d) {
        for (int r = 0; r < 3; ++r)
        {
            d.Transform[r][0] = in.objectToWorld[r].x;
            d.Transform[r][1] = in.objectToWorld[r].y;
            d.Transform[r][2] = in.objectToWorld[r].z;
            d.Transform[r][3] = in.objectToWorld[r].w;
        }
    };
    auto rigidDesc = [&](uint32_t i) {
        const gpu::Instance& in = instances[i];
        D3D12_RAYTRACING_INSTANCE_DESC d{};
        transformOf(in, d);
        d.InstanceID = (UINT)m_instances.size();
        d.InstanceMask = rtInstanceMask(in.flags);
        d.InstanceContributionToHitGroupIndex = 0;
        d.Flags = instanceFlags(i);  // DXR's default winding = CCW front in our right-handed frame (verified by Tests/RayScene)
        d.AccelerationStructure = m_meshBlas[in.mesh].address;
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
    const auto tProxies = std::chrono::steady_clock::now();
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
    const auto tDeformed = std::chrono::steady_clock::now();
    for (size_t k = 0; k < m_deformed.size(); ++k)
    {
        Deformed& d = m_deformed[k];
        d.record = (uint32_t)m_instances.size();
        D3D12_RAYTRACING_INSTANCE_DESC desc{};
        desc.Transform[0][0] = desc.Transform[1][1] = desc.Transform[2][2] = 1;  // world-space vertices
        desc.InstanceID = (UINT)m_instances.size();
        desc.InstanceMask = rtInstanceMask(instances[d.sceneInstance].flags);
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
    buildEmitters();
    m_stats.staticInstances = (uint32_t)m_staticDescs.size();
    m_stats.dynamicInstances = (uint32_t)m_dynamicDescs.size();
    m_stats.deformedInstances = (uint32_t)m_deformed.size();

    setupRuntime();  // runtime records after the load-time ones (placeholders), before the record buffers are made
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

    // The static TLAS: inherited when the static set (descriptors, their BLASes and keys) is the previous one's.
    if (previous && previous->m_tlasStatic.resource && previous->m_staticDescs.size() == m_staticDescs.size() && previous->m_staticKeys == m_staticKeys &&
        (m_staticDescs.empty() ||
         std::memcmp(previous->m_staticDescs.data(), m_staticDescs.data(), m_staticDescs.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC)) == 0))
    {
        m_tlasStatic = previous->m_tlasStatic;
        m_staticDescBuffer = previous->m_staticDescBuffer;
        m_staticScratch = previous->m_staticScratch;
        previous->m_tlasStatic = previous->m_staticDescBuffer = previous->m_staticScratch = {};
        m_stats.tlasStaticBytes = m_tlasStatic.bytes;
        m_stats.staticTlasInherited = 1;
    }
    else
        buildStaticTlas();
    {
        m_descSlotBytes = (uint64_t)(m_dynamicDescs.size() + m_runtimeInstanceCap + kMaxTriangleStreams + m_staticDescs.size() + 1) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
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
    const auto tRecords = std::chrono::steady_clock::now();
    {
        CommandList cl = m_device.acquireCommandList(QueueType::Graphics);
        gpu::FrameConstants fcData{};
        m_scene.fill(fcData);
        Buffer constants = createBuffer(1024, false, false, L"RT load frame constants");
        upload(constants, &fcData, sizeof fcData);
        cl.list->SetComputeRootConstantBufferView(1, constants.address());
        updateDynamic(cl.list.Get(), false);
        // No CPU wait (B3: rebuilds at scene edits must not stall the frame): the other queues wait on the GPU for this
        // build and the uploads before it (graphics queue order), so every later frame's work sees them.
        const uint64_t fence = m_device.submit(cl);
        for (uint32_t q = 0; q < kQueueTypeCount; ++q)
            if (q != (uint32_t)QueueType::Graphics) m_device.queue((QueueType)q).waitGpu(m_device.queue(QueueType::Graphics), fence);
        release(constants);
    }
    m_tlasStaticSrv = tlasSrv(m_tlasStatic.address());
    m_tlasDynamicSrv = tlasSrv(m_tlasDynamic.address());
    const auto tEnd = std::chrono::steady_clock::now();
    m_stats.loadMs = std::chrono::duration<double, std::milli>(tEnd - t0).count();
    auto ms = [](auto a, auto b) { return std::chrono::duration<double, std::milli>(b - a).count(); };
    logf("RayScene: %u static + %u dynamic instances (%u deformed), %u mesh BLAS (%llu tris, %.1f MB compacted from %.1f MB), deformed %llu tris / %llu verts "
         "(%u above proxy budget), TLAS static %.1f MB dynamic %.2f MB, load %.0f ms (records and mesh BLAS %.1f, proxies %.1f, deformed setup %.1f, "
         "static TLAS and records %.1f, first dynamic build %.1f)\n",
         m_stats.staticInstances, m_stats.dynamicInstances, m_stats.deformedInstances, m_stats.meshBlas, (unsigned long long)m_stats.meshBlasTriangles,
         m_stats.meshBlasBytes / 1048576.0, m_stats.meshBlasBytesBeforeCompaction / 1048576.0, (unsigned long long)m_stats.deformedTriangles,
         (unsigned long long)m_stats.deformedVertices, m_stats.deformedAboveProxyBudget, m_stats.tlasStaticBytes / 1048576.0, m_stats.tlasDynamicBytes / 1048576.0,
         m_stats.loadMs, ms(t0, tMeshBlas), ms(tMeshBlas, tProxies), ms(tProxies, tDeformed), ms(tDeformed, tRecords), ms(tRecords, tEnd));
    {
        // Resident video memory by use (the buffers this object keeps; upload rings are in system memory).
        auto mb = [](std::initializer_list<const Buffer*> list) {
            uint64_t bytes = 0;
            for (const Buffer* b : list)
                if (b->resource) bytes += b->bytes;
            return bytes / 1e6;
        };
        logf("RayScene resident (MB): static BLAS+TLAS %.1f (BLAS %.1f, TLAS %.2f, descs+scratch %.2f), dynamic AS %.1f (deformed BLAS %.1f, scratch %.1f, "
             "TLAS+scratch+descs %.2f, emitters %.3f), deformed vertices %.1f, records %.1f (instances, geometries, proxy indices, vertex map, deform jobs)\n",
             mb({ &m_meshBlasPool, &m_tlasStatic, &m_staticDescBuffer, &m_staticScratch }), mb({ &m_meshBlasPool }), mb({ &m_tlasStatic }),
             mb({ &m_staticDescBuffer, &m_staticScratch }),
             mb({ &m_deformedBlasPool, &m_deformedScratch, &m_tlasDynamic, &m_tlasScratch, &m_dynamicDescBuffer, &m_emitterBlas, &m_emitterAabbs }),
             mb({ &m_deformedBlasPool }), mb({ &m_deformedScratch }), mb({ &m_tlasDynamic, &m_tlasScratch, &m_dynamicDescBuffer }),
             mb({ &m_emitterBlas, &m_emitterAabbs }), mb({ &m_deformedPool }),
             mb({ &m_instanceBuffer, &m_geometryBuffer, &m_indexPool, &m_vertexMap, &m_deformJobs, &m_deformGroups, &m_exactCounts, &m_exactZero }));
    }
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
        if (m_meshBlas[m].geometryBase == gpu::kNone || m_meshBlas[m].built) continue;  // unused, or inherited (B3)
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
        m_meshBlas[builds[k].mesh].address = m_meshBlasPool.address() + compactOffset[k];
        m_meshBlas[builds[k].mesh].built = true;
    }
    m_device.submit(copy);  // later graphics work is ordered after it; the build pool below is released when the GPU is done
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
    if (m_proxySkeletons.size() < m_proxyLevels.size()) m_proxySkeletons.resize(m_proxyLevels.size());
    ProxyCache& cache = m_proxyCache[mesh];
    if (cache.valid)
    {
        m_proxySkeletons[mesh] = cache.skeleton;
        for (const ProxyRecipe& r : cache.levels)
        {
            ProxyMesh p = buildProxyFromLists(mesh, r.lists, r.reduced, &r.pose);
            p.error = r.error;
            levels.push_back(std::move(p));
        }
        return levels;
    }
    m_proxySkeletons[mesh] = proxyPoseSkeleton(m_scene.source()->meshes[mesh]);
    cache.skeleton = m_proxySkeletons[mesh];
    cache.valid = true;
    auto record = [&](const std::vector<std::vector<uint32_t>>& lists, const ProxyMesh& p) { cache.levels.push_back({ lists, p.reduced, p.error, p.pose }); };
    // Skinned meshes: R's skin-aware cuts (ProxyPoseBound.h) unless raytracing.skinned_proxy_cuts = "cluster_lod".
    if (m_skinAwareCuts)
    {
        const scene::Mesh& sm = m_scene.source()->meshes[mesh];
        const auto cuts = skinAwareCuts(sm, m_proxySkeletons[mesh], m_proxyBudget, m_proxySkinWeight);
        for (size_t l = 0; l < cuts.size(); ++l)
        {
            levels.push_back(buildProxyFromLists(mesh, cuts[l], !(l == 0 && sm.indices.size() / 3 <= m_proxyBudget)));
            record(cuts[l], levels.back());
        }
        if (!levels.empty()) return levels;
    }
    const ClusterData& cd = m_scene.clusters();
    if (mesh < cd.meshes.size() && cd.meshes[mesh].lodLevelCount > 0)
    {
        const auto& range = cd.meshes[mesh];
        // Levels run finest to coarsest: every cut within the budget, else the coarsest alone.
        std::vector<std::vector<uint32_t>> lists;
        for (uint32_t l = range.lodLevelOffset; l < range.lodLevelOffset + range.lodLevelCount; ++l)
            if (cd.lodLevels[l].triangleCount <= m_proxyBudget)
            {
                levels.push_back(buildProxy(mesh, l, &lists));
                record(lists, levels.back());
            }
        if (levels.empty())
        {
            levels.push_back(buildProxy(mesh, range.lodLevelOffset + range.lodLevelCount - 1, &lists));
            record(lists, levels.back());
        }
    }
    else
    {
        std::vector<std::vector<uint32_t>> lists;
        levels.push_back(buildProxy(mesh, gpu::kNone, &lists));
        record(lists, levels.back());
    }
    return levels;
}

RayScene::ProxyMesh RayScene::buildProxy(uint32_t mesh, uint32_t lodLevel, std::vector<std::vector<uint32_t>>* listsOut)
{
    const scene::Mesh& sm = m_scene.source()->meshes[mesh];
    const ClusterData& cd = m_scene.clusters();
    // Per-submesh index lists in mesh vertex indices, from the cut or the full mesh.
    std::vector<std::vector<uint32_t>> perSubmesh(sm.submeshes.size());
    bool reduced = false;
    if (lodLevel != gpu::kNone)
    {
        const gpu::LodLevel& level = cd.lodLevels[lodLevel];
        reduced = level.error > 0;
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
    ProxyMesh p = buildProxyFromLists(mesh, perSubmesh, reduced);
    if (lodLevel != gpu::kNone) p.error = cd.lodLevels[lodLevel].error;
    if (listsOut) *listsOut = std::move(perSubmesh);
    return p;
}

// A proxy from per-submesh triangle lists (mesh vertex indices); 'reduced' = not the source mesh. Its error is the
// measured bind-pose Hausdorff distance (ProxyPoseCoefficients::bindError), its pose terms bound it in any pose.
RayScene::ProxyMesh RayScene::buildProxyFromLists(uint32_t mesh, const std::vector<std::vector<uint32_t>>& perSubmesh, bool reduced, const ProxyPoseCoefficients* pose)
{
    ProxyMesh p;
    p.reduced = reduced;
    const scene::Mesh& sm = m_scene.source()->meshes[mesh];
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
    if (p.reduced)
    {
        if (pose)
            p.pose = *pose;  // cached (ProxyCache)
        else
        {
            std::vector<uint32_t> cut;
            for (const std::vector<uint32_t>& list : perSubmesh) cut.insert(cut.end(), list.begin(), list.end());
            p.pose = proxyPoseCoefficients(sm, m_proxySkeletons[mesh], cut);
        }
        p.error = p.pose.bindError;
    }
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

void RayScene::buildEmitters()
{
    const scene::Scene* src = m_scene.source();
    if (!src || src->lights.empty()) return;
    std::vector<D3D12_RAYTRACING_AABB> boxes;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (const scene::Light& l : src->lights)
    {
        const float3 c = l.position, f = l.forward, r = l.right, u = cross(l.forward, l.right);
        float3 lo{ nan, nan, nan }, hi{ nan, nan, nan };  // NaN: an inactive primitive (point, spot)
        auto grow = [&](float3 p, float pad) {
            if (std::isnan(lo.x)) lo = hi = p;
            lo = { std::min(lo.x, p.x - pad), std::min(lo.y, p.y - pad), std::min(lo.z, p.z - pad) };
            hi = { std::max(hi.x, p.x + pad), std::max(hi.y, p.y + pad), std::max(hi.z, p.z + pad) };
        };
        switch (l.type)
        {
        case scene::LightType::Rect:
            for (float a : { -0.5f, 0.5f })
                for (float b : { -0.5f, 0.5f }) grow(c + r * (a * l.size.x) + u * (b * l.size.y), 1e-3f * std::max(l.size.x, l.size.y) + 1e-5f);
            break;
        case scene::LightType::Disk:
        {
            // A disk's box: per axis the radius times the in-plane extent sqrt(1 - n_axis^2).
            const float R = l.size.x;
            const float3 e{ R * std::sqrt(std::max(0.0f, 1 - f.x * f.x)), R * std::sqrt(std::max(0.0f, 1 - f.y * f.y)), R * std::sqrt(std::max(0.0f, 1 - f.z * f.z)) };
            grow(c - e, 1e-3f * R + 1e-5f);
            grow(c + e, 1e-3f * R + 1e-5f);
            break;
        }
        case scene::LightType::Sphere:
            grow(c, l.size.x);
            break;
        case scene::LightType::Tube:
            grow(c - r * (0.5f * l.size.x), l.size.y);
            grow(c + r * (0.5f * l.size.x), l.size.y);
            break;
        default:
            break;
        }
        if (!std::isnan(lo.x)) ++m_emitterLights;
        boxes.push_back({ lo.x, lo.y, lo.z, hi.x, hi.y, hi.z });
    }
    if (m_emitterLights == 0) return;
    m_emitterAabbs = createBuffer(boxes.size() * sizeof(D3D12_RAYTRACING_AABB), false, false, L"RT emitter AABBs");
    upload(m_emitterAabbs, boxes.data(), boxes.size() * sizeof(D3D12_RAYTRACING_AABB));
    D3D12_RAYTRACING_GEOMETRY_DESC g{};
    g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
    g.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    g.AABBs.AABBCount = boxes.size();
    g.AABBs.AABBs = { m_emitterAabbs.address(), sizeof(D3D12_RAYTRACING_AABB) };
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
    inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    inputs.NumDescs = 1;
    inputs.pGeometryDescs = &g;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO sizes{};
    m_device.d3d()->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &sizes);
    m_emitterBlas = createBuffer(sizes.ResultDataMaxSizeInBytes, false, true, L"RT emitter BLAS");
    Buffer scratch = createBuffer(sizes.ScratchDataSizeInBytes, true, false, L"RT emitter BLAS scratch");
    CommandList cl = m_device.acquireCommandList(QueueType::Graphics);
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
    d.Inputs = inputs;
    d.DestAccelerationStructureData = m_emitterBlas.address();
    d.ScratchAccelerationStructureData = scratch.address();
    cl.list->BuildRaytracingAccelerationStructure(&d, 0, nullptr);
    m_device.queue(QueueType::Graphics).waitCpu(m_device.submit(cl));
    D3D12_RAYTRACING_INSTANCE_DESC desc{};
    desc.Transform[0][0] = desc.Transform[1][1] = desc.Transform[2][2] = 1;  // world-space boxes
    desc.InstanceID = kRtInstanceEmitter;
    desc.InstanceMask = m_emittersEnabled ? kRtMaskEmitter : 0;  // raytracing.emitters (off until structure 2 is gated)
    desc.InstanceContributionToHitGroupIndex = 1;  // RtEmitterGroup
    desc.AccelerationStructure = m_emitterBlas.address();
    m_dynamicRecord.push_back(0xFFFFFFFFu);
    m_dynamicDescs.push_back(desc);
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
    std::fill(m_exactRebuild.begin(), m_exactRebuild.end(), uint8_t{ 0 });
    m_stats.exactBuilds = 0;
    if (m_exact.empty()) return;
    const uint32_t n = (uint32_t)m_deformed.size();
    const uint32_t oldSlot = (uint32_t)((fc.frame.frameIndex + kDescSlots - fc.framesInFlight) % kDescSlots);
    std::vector<uint32_t> wanted;
    if (m_exactSlotFrame[oldSlot] != UINT64_MAX && fc.frame.frameIndex >= m_exactSlotFrame[oldSlot] + fc.framesInFlight)
    {
        const uint32_t* counts = m_exactReadbackMapped + (size_t)oldSlot * n;
        // Only instances whose finest proxy cut exceeds the error bound where the rays meet them: a reflection ray's
        // footprint at its hit is coneWidth + t x spread >= pixelAngle x (eye-to-reflector + t) >= pixelAngle x the
        // direct distance (triangle inequality; flat and convex reflectors, ReflectionShade.hlsli), and the cut is chosen
        // so its error is at most raytracing.proxy_error_px of those footprints at the direct distance. So a proxy that
        // meets its bound is within the bound along every such reflection path, and its original mesh would change
        // nothing above it. Members stay until the ratio falls below 0.8 (the proxies' hysteresis).
        for (uint32_t k = 0; k < n; ++k)
        {
            if (counts[k] < m_exactMinHits) continue;
            const bool member = std::any_of(m_exact.begin(), m_exact.end(), [&](const ExactSlot& e) { return e.owner == k; });
            if ((m_experiment & 2) != 0 || m_deformed[k].exactNeed > (member ? 0.8f : 1.0f)) wanted.push_back(k);  // 2: hit count alone
            else ++m_stats.exactWithinBoundTotal;
        }
        m_stats.exactWantedTotal += wanted.size();
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
    m_stats.posedErrorOverBindMax = 0;
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
            // Error model (raytracing.proxy_error_model): "measured" = the cut's measured bind-pose Hausdorff x
            // raytracing.proxy_posed_factor (skin-aware cuts: posed / bind measured 0.93-1.32 at attribute weight 4, see
            // ProxyPoseBound.h); "bound" = the certified posed bound (sound, but 50-170x the measured error: it counts
            // tangential sliding of corresponding points, which Hausdorff does not). Experiment bit 16: V's claim alone.
            const bool certified = m_proxyErrorBound && (m_experiment & 16) == 0;
            if (certified) proxyPoseTerms(m_proxySkeletons[d.mesh], m_scene.palette(d.sceneInstance), m_poseTerms);
            auto cutError = [&](uint32_t l) {
                if (!levels[l].reduced) return 0.0f;
                if (m_experiment & 16) return levels[l].error;
                return certified ? proxyPoseError(levels[l].pose, m_proxySkeletons[d.mesh], m_poseTerms) : levels[l].error * m_proxyPosedFactor;
            };
            // The reflection exact set's criterion (selectExactSet): the finest cut's error over this bound.
            const float finest = cutError(0) * scale;
            d.exactNeed = finest > 0 ? (bound > 0 ? finest / bound : FLT_MAX) : 0.0f;
            m_stats.posedErrorOverBindMax = std::max(m_stats.posedErrorOverBindMax, levels[0].error > 0 ? cutError(0) / levels[0].error : 0.0f);
            if (levels.size() < 2 || patches.size() + 2 > patchCapacity)
            {
                triangles += levels[d.level].triangles;
                continue;
            }
            uint32_t level = d.level;
            while (level > 0 && cutError(level) * scale > bound) --level;  // finer at once
            while (level + 1 < levels.size() && cutError(level + 1) * scale <= 0.8f * bound) ++level;  // coarser with margin
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
    for (const BufferRef& r : { v.pageTable, v.blocks, v.searchBound }) b.use(r, Use::SrvGraphics);
    b.use(v.atlas, Use::SrvGraphics);
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
    const uint32_t words[8] = { c.srv(v.pageTable), c.srv(v.atlas), c.srv(v.blocks), c.srv(v.searchBound), v.constants, 0xFFFFFFFFu, 0,
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
    d.InstanceMask = rtInstanceMask(in.flags);
}

void RayScene::record(FramePassContext& fc)
{
    RenderGraph& g = fc.graph;
    m_frame = {};
    m_changes = std::move(m_buildChanges);
    m_buildChanges.clear();
    updateLightGrid(fc);
    recordLightFunctions(fc);  // after the grid slot exists (word 20)
    recordFxLights(fc);        // word 21
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
        if (m_dynamicRecord[k] == 0xFFFFFFFFu)  // the emitter instance: boxes at the lights' source positions
        {
            slotDescs[k] = m_dynamicDescs[k];
            const float3 origin = m_scene.originOffset();  // C9: moved with the frame's origin (its lights are)
            slotDescs[k].Transform[0][3] = -origin.x, slotDescs[k].Transform[1][3] = -origin.y, slotDescs[k].Transform[2][3] = -origin.z;
            continue;
        }
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
    recordRuntime(fc, slotDescs);  // runtime geometry after the load-time dynamic instances
    recordStreams(fc, slotDescs);  // W's triangle streams after those (R-W2)
    // Diagnostics (every 64 frames): what the dynamic TLAS builder is given.
    if (fc.frame.frameIndex % 64 == 0)
    {
        DynamicTlasCensus& c = m_stats.dynamicCensus;
        c = {};
        std::vector<uint64_t> blases;
        float lo[3] = { 1e30f, 1e30f, 1e30f }, hi[3] = { -1e30f, -1e30f, -1e30f };
        for (size_t k = 0; k < m_dynamicDescs.size(); ++k)
        {
            const D3D12_RAYTRACING_INSTANCE_DESC& d = slotDescs[k];
            bool finite = true;
            float scale = 0;
            for (int r = 0; r < 3; ++r)
            {
                for (int col = 0; col < 4; ++col) finite = finite && std::isfinite(d.Transform[r][col]);
                scale = std::max(scale, std::fabs(d.Transform[r][0]) + std::fabs(d.Transform[r][1]) + std::fabs(d.Transform[r][2]));
            }
            if (!finite) ++c.nonFinite;
            if (d.InstanceMask == 0) ++c.maskZero;
            if (d.AccelerationStructure == 0) ++c.nullBlas;
            c.maxScale = std::max(c.maxScale, scale);
            for (int r = 0; r < 3; ++r)
                if (finite)
                {
                    lo[r] = std::min(lo[r], d.Transform[r][3]);
                    hi[r] = std::max(hi[r], d.Transform[r][3]);
                }
            blases.push_back(d.AccelerationStructure);
        }
        std::sort(blases.begin(), blases.end());
        c.distinctBlas = (uint32_t)(std::unique(blases.begin(), blases.end()) - blases.begin());
        c.instances = (uint32_t)m_dynamicDescs.size();
        for (int r = 0; r < 3; ++r) c.extent[r] = hi[r] - lo[r];
    }
    D3D12_GPU_VIRTUAL_ADDRESS dynamicDescs = m_descRing->GetGPUVirtualAddress() + slotOffset;
    std::optional<BufferRef> dynamicDescCopy;
    if ((m_experiment & 4) != 0 && m_dynamicDescBuffer.resource && !m_dynamicDescs.empty())
    {
        // Attribution: the builder reads the descriptors from video memory (copied from the upload ring first).
        const BufferRef copy = g.importBuffer(m_dynamicDescBuffer.resource.Get(), { "RT dynamic instance descs", m_dynamicDescBuffer.bytes, 0 });
        ID3D12Resource* ring = m_descRing.Get();
        const uint64_t bytes = m_dynamicDescs.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
        g.addPass("r.as.tlas.descs", QueueType::Graphics, [&](PassBuilder& b) { b.use(copy, Use::CopyDst); },
                  [copy, ring, slotOffset, bytes](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(copy), 0, ring, slotOffset, bytes); });
        dynamicDescCopy = copy;
        dynamicDescs = m_dynamicDescBuffer.address();
    }
    bool staticChanged = false;
    const float3 shift = fc.frame.originShift;
    const bool rebase = shift.x != 0 || shift.y != 0 || shift.z != 0;
    for (size_t k = 0; k < m_staticDescs.size(); ++k)
    {
        const gpu::Instance& in = sceneInstances[m_staticScene[k]];
        // A rebase moves every transform without a revision (GpuScene::rebase): all static instances are refreshed.
        if (staticKey(in) == m_staticKeys[k] && !rebase) continue;
        m_staticKeys[k] = staticKey(in);
        const float4 sphere = m_scene.meshes()[in.mesh].boundsSphere;
        // An origin rebase (C9) moves every instance by -delta with the frame: nothing changed in the world, so GI keeps
        // its cells (GiShift moves them) and only the TLAS is rebuilt.
        const bool change = !rebase;
        if (change && m_staticDescs[k].InstanceMask != 0) m_changes.push_back(descBounds(m_staticDescs[k], sphere));  // where it was (B3)
        refreshDesc(m_staticDescs[k], in, false);
        if (change && m_staticDescs[k].InstanceMask != 0) m_changes.push_back(descBounds(m_staticDescs[k], sphere));  // where it is now
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
        // After the dynamic region (load-time dynamic, runtime and stream instances), which the same slot holds.
        const uint64_t staticOffset = slotOffset + (m_dynamicDescs.size() + m_runtimeInstanceCap + kMaxTriangleStreams) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC);
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
    // The barrier into the build (refit writes -> TLAS reads) is taken by this empty pass, so its time (waiting for the
    // deformed BLAS builds to drain) is not charged to r.as.tlas.dynamic, which then times the build alone.
    g.addPass("r.as.tlas.sync", QueueType::Compute,
              [&](PassBuilder& b) {
                  if (frame.deformedBlas.valid()) b.use(frame.deformedBlas, Use::AccelerationStructureRead);
                  if (frame.runtimePool.valid()) b.use(frame.runtimePool, Use::AccelerationStructureRead);
                  if (frame.streamPool.valid()) b.use(frame.streamPool, Use::AccelerationStructureRead);
                  b.use(frame.tlasDynamic, Use::AccelerationStructureWrite);
                  b.use(scratch, Use::AccelerationStructureScratch);
                  if (dynamicDescCopy) b.use(*dynamicDescCopy, Use::AccelerationStructureInput);
              },
              [](PassContext&) {});
    g.addPass("r.as.tlas.dynamic", QueueType::Compute,
              [&](PassBuilder& b) {
                  if (frame.deformedBlas.valid()) b.use(frame.deformedBlas, Use::AccelerationStructureRead);
                  b.use(frame.tlasDynamic, Use::AccelerationStructureWrite);
                  b.use(scratch, Use::AccelerationStructureScratch);
                  if (dynamicDescCopy) b.use(*dynamicDescCopy, Use::AccelerationStructureInput);
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
    if (m_frame.runtimePool.valid()) b.use(m_frame.runtimePool, Use::AccelerationStructureRead);  // runtime geometry BLASes
    if (m_frame.geometries.valid()) b.use(m_frame.geometries, Use::SrvGraphics);
    if (m_frame.lightFunctions.valid()) b.use(m_frame.lightFunctions, Use::SrvGraphics);  // the hits' local lights (A8)
    if (m_frame.fxCdf.valid()) b.use(m_frame.fxCdf, Use::SrvGraphics);  // A3 FX lights: their distribution and records
    if (m_frame.fxLights.valid()) b.use(m_frame.fxLights, Use::SrvGraphics);
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
    inputs.NumDescs = (UINT)(descs ? m_dynamicCountNow : (uint32_t)m_dynamicDescs.size());  // the ring has the runtime ones too
    if (!m_tlasDynamic.resource)
    {
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO sizes{};
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS capacity = inputs;
        capacity.NumDescs = (UINT)(m_dynamicDescs.size() + m_runtimeInstanceCap + kMaxTriangleStreams);  // room for every runtime instance and stream
        m_device.d3d()->GetRaytracingAccelerationStructurePrebuildInfo(&capacity, &sizes);
        m_tlasDynamic = createBuffer(sizes.ResultDataMaxSizeInBytes, false, true, L"RT dynamic TLAS");
        m_tlasScratch = createBuffer(sizes.ScratchDataSizeInBytes, true, false, L"RT dynamic TLAS scratch");
        m_dynamicDescBuffer = createBuffer(std::max<size_t>(m_dynamicDescs.size(), 1) * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), false, false, L"RT dynamic instance descs");
        m_stats.tlasDynamicBytes = m_tlasDynamic.bytes;
        // Load-time descriptors for the out-of-graph path (tests); frames use the per-frame ring (record).
        upload(m_dynamicDescBuffer, m_dynamicDescs.data(), m_dynamicDescs.size() * sizeof(D3D12_RAYTRACING_INSTANCE_DESC));
    }
    inputs.InstanceDescs = descs ? descs : m_dynamicDescBuffer.address();
    if ((m_experiment & 8) != 0)  // attribution: fast build instead of fast trace
        inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
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
    out[7] = m_lightSrvNow;  // HitLocalLights.hlsli (0xFFFFFFFF: no local lights)
}

namespace
{
// Mirror of the shared RtLight (Reference/GpuTracer/shared/Lights.hlsli), 96 B.
struct RtLightRecord
{
    float3 position;
    uint32_t type;
    float3 forward;
    float intensity;
    float3 right;
    float range;
    float3 up;
    float spotScale;
    float3 color;
    float spotOffset;
    float size[2];
    uint32_t castShadow, pad;
};
static_assert(sizeof(RtLightRecord) == 96);
// RtLightGrid (48 B) + offsets of the lights, cell starts and cell lights (HitLocalLights.hlsli).
struct RtLightHeader
{
    float3 minCorner;
    uint32_t count;
    float3 cell;
    uint32_t dim[3], pad[2];
    uint32_t lightsOffset, cellStartOffset, cellLightsOffset, pad2;
    uint32_t decal[4];  // words 16..19, per frame (RayScene::recordDecals; HitDecals.hlsli): TLAS, frames, texture table, count
    uint32_t functions[4];  // word 20, per frame: E's light functions (FrameResources::lightFunctions, A8), 0xFFFFFFFF none;
                            // word 21, per frame: the FX lights' groups (recordFxLights, A3), 0xFFFFFFFF none
};
static_assert(sizeof(RtLightHeader) == 96);
constexpr uint32_t kLightDecalOffset = 64, kLightFunctionOffset = 80;
constexpr uint32_t kLightCellsMax = 1u << 18;  // the grid's cell size grows past this many cells (262,144 x 4 B starts)
} // namespace

// The reference LightSet's normalisation and grid (Reference/PathTracer/src/Lights.cpp: forward unit, right
// orthogonalised, up = forward x right, spot scale / offset, cells of half the median range within [0.5, 16] m, a light
// listed in every cell its range sphere touches), with the cell size grown until at most kLightCellsMax cells.
void RayScene::updateEmissive(FramePassContext& fc)
{
    if (fc.scene.revision() == m_emissiveRevision) return;
    m_emissiveRevision = fc.scene.revision();
    if (m_emissive)
    {
        m_device.deferRelease(m_emissive);
        DescriptorHeaps* h = &m_device.descriptors();
        const uint32_t srv = m_emissiveSrv;
        m_device.deferCall([h, srv] { h->freeResource(srv); });
        m_emissive.Reset();
        m_emissiveSrv = 0xFFFFFFFFu;
    }
    const scene::Scene* src = fc.scene.source();
    if (!src) return;
    std::vector<uint32_t> entries;  // 4 words each: instance, mesh triangle, submesh, running weight (float bits)
    std::vector<uint32_t> base(src->instances.size(), 0xFFFFFFFFu);
    double total = 0;
    for (uint32_t i = 0; i < (uint32_t)src->instances.size(); ++i)
    {
        const scene::Instance& in = src->instances[i];
        if (in.flags & scene::InstanceSkinned) continue;  // deformed emitters: their emission still reaches the cache by rays
        const scene::Mesh& m = src->meshes[in.mesh];
        std::vector<float> lum(m.submeshes.size(), 0.0f);
        bool any = false;
        for (size_t k = 0; k < m.submeshes.size(); ++k)
        {
            const uint32_t mat = in.materialOverrides.empty() ? m.submeshes[k].material : in.materialOverrides[k];
            const float3 e = src->materials[mat].emissive;
            lum[k] = 0.2126f * e.x + 0.7152f * e.y + 0.0722f * e.z;
            any = any || lum[k] > 0;
        }
        if (!any) continue;
        base[i] = (uint32_t)(entries.size() / 4);
        const uint32_t triangles = (uint32_t)(m.indices.size() / 3);
        for (uint32_t t = 0; t < triangles; ++t)
        {
            uint32_t sub = 0;
            for (uint32_t k = 0; k < (uint32_t)m.submeshes.size(); ++k)
                if (t * 3 >= m.submeshes[k].indexOffset && t * 3 < m.submeshes[k].indexOffset + m.submeshes[k].indexCount) sub = k;
            double w = 0;
            if (lum[sub] > 0)
            {
                const float3 p0 = in.transform.transformPoint(m.positions[m.indices[3 * t]]), p1 = in.transform.transformPoint(m.positions[m.indices[3 * t + 1]]),
                             p2 = in.transform.transformPoint(m.positions[m.indices[3 * t + 2]]);
                w = 0.5 * length(cross(p1 - p0, p2 - p0)) * lum[sub];
            }
            total += w;
            float cum = (float)total;
            uint32_t cumBits;
            std::memcpy(&cumBits, &cum, 4);
            entries.insert(entries.end(), { i, t, sub, cumBits });
        }
    }
    if (entries.empty() || !(total > 0)) return;
    // Raw buffer: { entries, instances, total (float bits), base-table offset }, entries (16 B), base table (4 B each).
    const uint32_t count = (uint32_t)(entries.size() / 4), baseOffset = 16 + count * 16;
    std::vector<uint32_t> image(4 + entries.size() + base.size());
    const float totalF = (float)total;
    image[0] = count;
    image[1] = (uint32_t)base.size();
    std::memcpy(&image[2], &totalF, 4);
    image[3] = baseOffset;
    std::copy(entries.begin(), entries.end(), image.begin() + 4);
    std::copy(base.begin(), base.end(), image.begin() + 4 + entries.size());
    D3D12_HEAP_PROPERTIES up{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = image.size() * 4;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    check(m_device.d3d()->CreateCommittedResource3(&up, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_emissive)),
          "RT emissive triangles");
    m_emissive->SetName(L"RT emissive triangles");
    void* mapped = nullptr;
    D3D12_RANGE nothing{ 0, 0 };
    check(m_emissive->Map(0, &nothing, &mapped), "map RT emissive triangles");
    std::memcpy(mapped, image.data(), image.size() * 4);
    m_emissive->Unmap(0, nullptr);
    DescriptorHeaps& h = m_device.descriptors();
    m_emissiveSrv = h.allocateResource();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Format = DXGI_FORMAT_R32_TYPELESS;
    sd.Buffer.NumElements = (UINT)image.size();
    sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    m_device.d3d()->CreateShaderResourceView(m_emissive.Get(), &sd, h.resourceCpu(m_emissiveSrv));
}

void RayScene::updateLightGrid(FramePassContext& fc)
{
    updateEmissive(fc);
    const scene::Scene* src = fc.scene.source();
    const std::vector<scene::Light> none;
    const std::vector<scene::Light>& lights = src ? src->lights : none;
    uint64_t hash = 1469598103934665603ull;
    auto mix = [&](const void* p, size_t n) {
        const uint8_t* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) hash = (hash ^ b[i]) * 1099511628211ull;
    };
    for (const scene::Light& l : lights)
    {
        const uint32_t type = (uint32_t)l.type, shadow = l.castShadow ? 1u : 0u;
        mix(&type, 4); mix(&l.position, 12); mix(&l.forward, 12); mix(&l.right, 12); mix(&l.color, 12); mix(&l.intensity, 4);
        mix(&l.range, 4); mix(&l.spotInner, 4); mix(&l.spotOuter, 4); mix(&l.size, 8); mix(&shadow, 4);
    }
    const size_t n = lights.size();
    mix(&n, sizeof n);
    const float3 origin = m_scene.originOffset();  // C9: positions relative to the frame's origin
    mix(&origin, sizeof origin);
    mix(&m_emissiveSrv, 4);  // the header carries it
    if (hash != m_lightHash || m_lightImage.empty())
    {
        m_lightHash = hash;
        ++m_lightVersion;
        std::vector<RtLightRecord> rec(n);
        RtLightHeader head{};
        float3 lo{ 1e30f, 1e30f, 1e30f }, hi{ -1e30f, -1e30f, -1e30f };
        std::vector<float> ranges;
        for (size_t i = 0; i < n; ++i)
        {
            const scene::Light& l = lights[i];
            RtLightRecord& r = rec[i];
            r.position = l.position - origin;  // the frame's origin (C9 rebase: GpuScene's lights move the same way)
            r.type = (uint32_t)l.type;
            r.forward = normalize(l.forward);
            r.right = normalize(l.right - r.forward * dot(l.right, r.forward));
            r.up = cross(r.forward, r.right);
            const float ci = std::cos(l.spotInner), co = std::cos(l.spotOuter);
            r.spotScale = 1.0f / std::max(ci - co, 1e-4f);
            r.spotOffset = -co * r.spotScale;
            r.intensity = l.intensity;
            r.range = std::max(l.range, 1e-3f);
            r.color = l.color;
            r.size[0] = l.size.x;
            r.size[1] = l.size.y;
            r.castShadow = l.castShadow ? 1u : 0u;
            lo = { std::min(lo.x, r.position.x - r.range), std::min(lo.y, r.position.y - r.range), std::min(lo.z, r.position.z - r.range) };
            hi = { std::max(hi.x, r.position.x + r.range), std::max(hi.y, r.position.y + r.range), std::max(hi.z, r.position.z + r.range) };
            ranges.push_back(r.range);
        }
        std::vector<uint32_t> cellStart(2, 0), cellLights;
        head.count = (uint32_t)n;
        head.dim[0] = head.dim[1] = head.dim[2] = 1;
        head.cell = { 1, 1, 1 };
        if (n > 0)
        {
            std::nth_element(ranges.begin(), ranges.begin() + ranges.size() / 2, ranges.end());
            float cellSize = std::clamp(ranges[ranges.size() / 2] * 0.5f, 0.5f, 16.0f);
            const float3 ext = hi - lo;
            const float e[3] = { ext.x, ext.y, ext.z };
            for (;;)
            {
                uint64_t cells = 1;
                for (int k = 0; k < 3; ++k) cells *= std::clamp((uint32_t)std::ceil(e[k] / cellSize), 1u, 512u);
                if (cells <= kLightCellsMax) break;
                cellSize *= 1.25f;
            }
            float cs[3];
            for (int k = 0; k < 3; ++k)
            {
                head.dim[k] = std::clamp((uint32_t)std::ceil(e[k] / cellSize), 1u, 512u);
                cs[k] = e[k] / head.dim[k];
            }
            head.minCorner = lo;
            head.cell = { cs[0], cs[1], cs[2] };
            const size_t cells = (size_t)head.dim[0] * head.dim[1] * head.dim[2];
            std::vector<std::vector<uint32_t>> lists(cells);
            for (uint32_t i = 0; i < (uint32_t)n; ++i)
            {
                const RtLightRecord& l = rec[i];
                const float3 a = l.position - float3{ l.range, l.range, l.range } - lo, b = l.position + float3{ l.range, l.range, l.range } - lo;
                const int x0 = std::max(0, (int)(a.x / cs[0])), x1 = std::min((int)head.dim[0] - 1, (int)(b.x / cs[0]));
                const int y0 = std::max(0, (int)(a.y / cs[1])), y1 = std::min((int)head.dim[1] - 1, (int)(b.y / cs[1]));
                const int z0 = std::max(0, (int)(a.z / cs[2])), z1 = std::min((int)head.dim[2] - 1, (int)(b.z / cs[2]));
                for (int z = z0; z <= z1; ++z)
                    for (int y = y0; y <= y1; ++y)
                        for (int x = x0; x <= x1; ++x)
                        {
                            const float3 cmin = lo + float3{ x * cs[0], y * cs[1], z * cs[2] }, cmax = cmin + head.cell;
                            const float3 q{ std::max(cmin.x, std::min(l.position.x, cmax.x)), std::max(cmin.y, std::min(l.position.y, cmax.y)),
                                            std::max(cmin.z, std::min(l.position.z, cmax.z)) };
                            const float3 dd = q - l.position;
                            if (dot(dd, dd) <= l.range * l.range) lists[((size_t)z * head.dim[1] + y) * head.dim[0] + x].push_back(i);
                        }
            }
            cellStart.assign(cells + 1, 0);
            for (size_t c = 0; c < cells; ++c)
            {
                cellStart[c] = (uint32_t)cellLights.size();
                cellLights.insert(cellLights.end(), lists[c].begin(), lists[c].end());
            }
            cellStart[cells] = (uint32_t)cellLights.size();
        }
        if (cellLights.empty()) cellLights.push_back(0);
        head.pad2 = m_emissiveSrv;  // word 15: emissive triangles (0xFFFFFFFF: none)
        head.decal[0] = 0xFFFFFFFFu;  // no decals until recordDecals writes them this frame
        head.decal[3] = 0;
        head.functions[0] = 0xFFFFFFFFu;
        head.lightsOffset = sizeof(RtLightHeader);
        head.cellStartOffset = head.lightsOffset + (uint32_t)(std::max<size_t>(n, 1) * sizeof(RtLightRecord));
        head.cellLightsOffset = head.cellStartOffset + (uint32_t)(cellStart.size() * 4);
        m_lightImage.assign(head.cellLightsOffset + cellLights.size() * 4, 0);
        std::memcpy(m_lightImage.data(), &head, sizeof head);
        if (n) std::memcpy(m_lightImage.data() + head.lightsOffset, rec.data(), n * sizeof(RtLightRecord));
        std::memcpy(m_lightImage.data() + head.cellStartOffset, cellStart.data(), cellStart.size() * 4);
        std::memcpy(m_lightImage.data() + head.cellLightsOffset, cellLights.data(), cellLights.size() * 4);
    }
    if (n == 0 && m_emissiveSrv == 0xFFFFFFFFu && !m_emittersEnabled)
    {
        m_lightSrvNow = 0xFFFFFFFFu;
        return;
    }
    publishLightSlot(fc);
}

void RayScene::publishLightSlot(FramePassContext& fc)
{
    const uint64_t need = (m_lightImage.size() + 255) & ~255ull;
    if (!m_lightRing || need > m_lightSlotBytes)
    {
        if (m_lightRing)
        {
            m_lightRing->Unmap(0, nullptr);
            m_device.deferRelease(m_lightRing);
            DescriptorHeaps* vh = &m_device.descriptors();
            for (uint32_t srv : m_lightRingSrv) m_device.deferCall([vh, srv] { vh->freeResource(srv); });
        }
        m_lightSlotBytes = std::max<uint64_t>(need * 2, 65536);
        D3D12_HEAP_PROPERTIES up{ D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC1 d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = kDescSlots * m_lightSlotBytes;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(m_device.d3d()->CreateCommittedResource3(&up, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr,
                                                       IID_PPV_ARGS(&m_lightRing)),
              "RT light grid ring");
        m_lightRing->SetName(L"RT local-light grid ring");
        D3D12_RANGE nothing{ 0, 0 };
        check(m_lightRing->Map(0, &nothing, reinterpret_cast<void**>(&m_lightRingMapped)), "map RT light grid ring");
        DescriptorHeaps& h = m_device.descriptors();
        for (uint32_t k = 0; k < kDescSlots; ++k)
        {
            m_lightRingSrv[k] = h.allocateResource();
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Format = DXGI_FORMAT_R32_TYPELESS;
            sd.Buffer.FirstElement = k * m_lightSlotBytes / 4;
            sd.Buffer.NumElements = (UINT)(m_lightSlotBytes / 4);
            sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            m_device.d3d()->CreateShaderResourceView(m_lightRing.Get(), &sd, h.resourceCpu(m_lightRingSrv[k]));
            m_lightSlotVersion[k] = 0;
        }
    }
    // Slot of frame % kDescSlots: the frame kDescSlots before has completed (frames in flight <= kDescSlots).
    const uint32_t slot = (uint32_t)(fc.frame.frameIndex % kDescSlots);
    if (m_lightSlotVersion[slot] != m_lightVersion)
    {
        std::memcpy(m_lightRingMapped + slot * m_lightSlotBytes, m_lightImage.data(), m_lightImage.size());
        m_lightSlotVersion[slot] = m_lightVersion;
    }
    // Word 11 (RtLightGrid.pad1), every frame: M's stable-area-light bits (COVERAGE 12.4 structure 2; FrameResources::
    // areaLightStable, published by prepareScene before this record). An emitter hit counts only for stable lights; M
    // shades the others' specular with LTC. UINT32_MAX (no M, or emitters off): every emitter counts.
    std::memcpy(m_lightRingMapped + slot * m_lightSlotBytes + 44, &fc.resources.areaLightStable, 4);
    // Words 16..19, every frame: no decals unless recordDecals writes them after this (a slot keeps an earlier frame's).
    const uint32_t noDecals[4] = { 0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0 };
    std::memcpy(m_lightRingMapped + slot * m_lightSlotBytes + kLightDecalOffset, noDecals, sizeof noDecals);
    const uint32_t noFunctions[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu };  // word 20 (light functions), word 21 (FX lights, recordFxLights)
    std::memcpy(m_lightRingMapped + slot * m_lightSlotBytes + kLightFunctionOffset, noFunctions, sizeof noFunctions);
    m_lightSrvNow = m_lightRingSrv[slot];
}

uint8_t* RayScene::lightSlot(FramePassContext& fc)
{
    if (m_lightSrvNow == 0xFFFFFFFFu) publishLightSlot(fc);  // an empty grid (record() had no lights to publish)
    return m_lightRingMapped + (fc.frame.frameIndex % kDescSlots) * m_lightSlotBytes;
}

void RayScene::recordDecals(FramePassContext& fc, const ViewResources& main)
{
    if (m_decalFrame == fc.frame.frameIndex) return;
    m_decalFrame = fc.frame.frameIndex;
    m_decalFrames = {};
    m_decalTlasRef = {};
    if (!main.decalFrames.valid()) return;  // record() left words 16..19 at "no decals"
    RenderGraph& g = fc.graph;
    constexpr uint32_t kFrameBytes = 128;  // DecalFrame (Decal.hlsli)
    const uint32_t count = (uint32_t)(g.desc(main.decalFrames).size / kFrameBytes);
    if (count == 0) return;
    if (count > m_decalCapacity)
    {
        for (Buffer* b : { &m_decalAabbs, &m_decalBlas, &m_decalBlasScratch, &m_decalTlas, &m_decalTlasScratch, &m_decalDesc }) release(*b);
        if (m_decalTlasSrv != 0xFFFFFFFFu)
        {
            DescriptorHeaps* h = &m_device.descriptors();
            const uint32_t srv = m_decalTlasSrv;
            m_device.deferCall([h, srv] { h->freeResource(srv); });
        }
        m_decalCapacity = std::max(count, 2 * m_decalCapacity);
        m_decalAabbs = createBuffer((uint64_t)m_decalCapacity * sizeof(D3D12_RAYTRACING_AABB), true, false, L"RT decal AABBs");
        D3D12_RAYTRACING_GEOMETRY_DESC geo{};
        geo.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
        geo.AABBs.AABBCount = m_decalCapacity;
        geo.AABBs.AABBs = { m_decalAabbs.address(), sizeof(D3D12_RAYTRACING_AABB) };
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
        in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
        in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        in.NumDescs = 1;
        in.pGeometryDescs = &geo;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO sizes{};
        m_device.d3d()->GetRaytracingAccelerationStructurePrebuildInfo(&in, &sizes);
        m_decalBlas = createBuffer(sizes.ResultDataMaxSizeInBytes, false, true, L"RT decal BLAS");
        m_decalBlasScratch = createBuffer(sizes.ScratchDataSizeInBytes, true, false, L"RT decal BLAS scratch");
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS top{};
        top.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
        top.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
        top.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        top.NumDescs = 1;
        m_device.d3d()->GetRaytracingAccelerationStructurePrebuildInfo(&top, &sizes);
        m_decalTlas = createBuffer(sizes.ResultDataMaxSizeInBytes, false, true, L"RT decal TLAS");
        m_decalTlasScratch = createBuffer(sizes.ScratchDataSizeInBytes, true, false, L"RT decal TLAS scratch");
        D3D12_RAYTRACING_INSTANCE_DESC desc{};
        desc.Transform[0][0] = desc.Transform[1][1] = desc.Transform[2][2] = 1;  // world-space boxes
        desc.InstanceMask = 0xFF;
        desc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_NON_OPAQUE;
        desc.AccelerationStructure = m_decalBlas.address();
        m_decalDesc = createBuffer(sizeof desc, false, false, L"RT decal instance");
        upload(m_decalDesc, &desc, sizeof desc);
        m_decalTlasSrv = tlasSrv(m_decalTlas.address());
    }
    const BufferRef frames = main.decalFrames;
    const BufferRef aabbs = g.importBuffer(m_decalAabbs.resource.Get(), { "RT decal AABBs", m_decalAabbs.bytes, 0 });
    const BufferRef blas = g.importBuffer(m_decalBlas.resource.Get(), { "RT decal BLAS", m_decalBlas.bytes, 0 });
    const BufferRef blasScratch = g.importBuffer(m_decalBlasScratch.resource.Get(), { "RT decal BLAS scratch", m_decalBlasScratch.bytes, 0 });
    const BufferRef tlas = g.importBuffer(m_decalTlas.resource.Get(), { "RT decal TLAS", m_decalTlas.bytes, 0 });
    const BufferRef tlasScratch = g.importBuffer(m_decalTlasScratch.resource.Get(), { "RT decal TLAS scratch", m_decalTlasScratch.bytes, 0 });
    ID3D12PipelineState* boxes = fc.shaders.compute("RayTracing/DecalBoxes");
    const D3D12_GPU_VIRTUAL_ADDRESS constants = fc.frameConstantsFor(fc.frame.mainView);  // the decal frames' camera
    g.addPass("r.decals.boxes", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(frames, Use::SrvCompute);
                  b.use(aabbs, Use::UavCompute);
              },
              [=](PassContext& c) {
                  const uint32_t k[4] = { c.srv(frames), c.uav(aabbs), count, 0 };
                  c.cmd->SetPipelineState(boxes);
                  c.bindFrameConstants(constants);
                  c.computeConstants(k, 4);
                  c.cmd->Dispatch((count + 63) / 64, 1, 1);
              });
    const D3D12_GPU_VIRTUAL_ADDRESS aabbAddress = m_decalAabbs.address(), blasAddress = m_decalBlas.address(), blasScratchAddress = m_decalBlasScratch.address();
    g.addPass("r.decals.blas", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(aabbs, Use::AccelerationStructureInput);
                  b.use(blas, Use::AccelerationStructureWrite);
                  b.use(blasScratch, Use::AccelerationStructureScratch);
              },
              [=](PassContext& c) {
                  D3D12_RAYTRACING_GEOMETRY_DESC geo{};
                  geo.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_PROCEDURAL_PRIMITIVE_AABBS;
                  geo.AABBs.AABBCount = count;
                  geo.AABBs.AABBs = { aabbAddress, sizeof(D3D12_RAYTRACING_AABB) };
                  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
                  d.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
                  d.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
                  d.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
                  d.Inputs.NumDescs = 1;
                  d.Inputs.pGeometryDescs = &geo;
                  d.DestAccelerationStructureData = blasAddress;
                  d.ScratchAccelerationStructureData = blasScratchAddress;
                  c.cmd->BuildRaytracingAccelerationStructure(&d, 0, nullptr);
              });
    // The header words (words 16..19 of this frame's light-grid slot) are written when this pass executes: the frames'
    // SRV exists then (the ring slot is the frame's own, frames in flight <= kDescSlots).
    uint8_t* slot = lightSlot(fc);
#if defined(UNX_HAS_MATERIAL)
    const uint32_t textures = material::textureTable(fc);
#else
    const uint32_t textures = 0xFFFFFFFFu;  // no M in this build: decal material constants only
#endif
    const D3D12_GPU_VIRTUAL_ADDRESS descAddress = m_decalDesc.address(), tlasAddress = m_decalTlas.address(), tlasScratchAddress = m_decalTlasScratch.address();
    const uint32_t tlasSrvIndex = m_decalTlasSrv;
    g.addPass("r.decals.tlas", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(blas, Use::AccelerationStructureRead);
                  b.use(tlas, Use::AccelerationStructureWrite);
                  b.use(tlasScratch, Use::AccelerationStructureScratch);
                  b.use(frames, Use::SrvCompute);
              },
              [=](PassContext& c) {
                  D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
                  d.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
                  d.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
                  d.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
                  d.Inputs.NumDescs = 1;
                  d.Inputs.InstanceDescs = descAddress;
                  d.DestAccelerationStructureData = tlasAddress;
                  d.ScratchAccelerationStructureData = tlasScratchAddress;
                  c.cmd->BuildRaytracingAccelerationStructure(&d, 0, nullptr);
                  const uint32_t words[4] = { tlasSrvIndex, c.srv(frames), textures, count };
                  std::memcpy(slot + kLightDecalOffset, words, sizeof words);
              });
    m_decalFrames = frames;
    m_decalTlasRef = tlas;
}

void RayScene::setupRuntime()
{
    const RuntimeCapacity& cap = m_scene.runtimeCapacity();
    m_dynamicCountNow = (uint32_t)m_dynamicDescs.size();
    if (cap.instances == 0 || cap.meshes == 0 || cap.indices < 3) return;
    m_runtimeInstanceCap = cap.instances;
    m_runtimeGeometryCap = std::max(cap.submeshes, 1u);
    m_runtimeRecordBase = (uint32_t)m_instances.size();
    m_runtimeGeometryBase = (uint32_t)m_geometries.size();
    m_instances.resize(m_instances.size() + m_runtimeInstanceCap, RtInstance{});
    m_geometries.resize(m_geometries.size() + m_runtimeGeometryCap, RtGeometry{});
    m_runtimeRecords.assign(m_runtimeInstanceCap, RtInstance{});
    m_runtimeGeometries.assign(m_runtimeGeometryCap, RtGeometry{});
    m_runtimeGeometryFree = { { 0u, m_runtimeGeometryCap } };
    m_runtimeBlas.assign(cap.meshes, RuntimeBlas{});
    // Pool and scratch: the prebuild sizes of every runtime triangle in one BLAS, twice (per-mesh BLAS overheads, first-fit
    // fragmentation), plus a page per mesh; a frame's builds never need more scratch than all runtime triangles at once.
    D3D12_RAYTRACING_GEOMETRY_DESC g{};
    g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    g.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    g.Triangles.VertexCount = std::max(cap.vertices, 3u);
    g.Triangles.VertexBuffer.StrideInBytes = sizeof(gpu::Vertex);
    g.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
    g.Triangles.IndexCount = cap.indices / 3 * 3;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
    in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    in.NumDescs = 1;
    in.pGeometryDescs = &g;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO sizes{};
    m_device.d3d()->GetRaytracingAccelerationStructurePrebuildInfo(&in, &sizes);
    const uint64_t pool = alignUp(2 * sizes.ResultDataMaxSizeInBytes + (uint64_t)cap.meshes * 4096, kAsAlign);
    const uint64_t scratch = alignUp(2 * sizes.ScratchDataSizeInBytes + (uint64_t)cap.meshes * kAsAlign, kAsAlign);
    m_runtimePool = createBuffer(pool, false, true, L"RT runtime BLAS pool");
    m_runtimeScratch = createBuffer(scratch, true, false, L"RT runtime BLAS scratch");
    m_runtimePoolFree = { { 0ull, pool } };
    D3D12_HEAP_PROPERTIES up{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = (uint64_t)kDescSlots * (m_runtimeInstanceCap + m_runtimeGeometryCap) * 16;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    check(m_device.d3d()->CreateCommittedResource3(&up, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_runtimeRing)),
          "RT runtime record ring");
    m_runtimeRing->SetName(L"RT runtime record ring");
    D3D12_RANGE none{ 0, 0 };
    check(m_runtimeRing->Map(0, &none, reinterpret_cast<void**>(&m_runtimeRingMapped)), "map RT runtime record ring");
}

namespace
{
// First fit in a sorted free list of (first, count); returns UINT64_MAX when nothing fits.
template <class T>
uint64_t takeRange(std::vector<std::pair<T, T>>& free, T count, T align)
{
    for (size_t k = 0; k < free.size(); ++k)
    {
        const T first = (free[k].first + align - 1) / align * align, end = free[k].first + free[k].second;
        if (first + count > end) continue;
        const T before = first - free[k].first, after = end - (first + count);
        const T start = free[k].first;
        free.erase(free.begin() + (std::ptrdiff_t)k);
        if (after) free.insert(free.begin() + (std::ptrdiff_t)k, { first + count, after });
        if (before) free.insert(free.begin() + (std::ptrdiff_t)k, { start, before });
        return (uint64_t)first;
    }
    return UINT64_MAX;
}
template <class T>
void giveRange(std::vector<std::pair<T, T>>& free, T first, T count)
{
    if (count == 0) return;
    auto it = std::lower_bound(free.begin(), free.end(), std::pair<T, T>{ first, 0 });
    it = free.insert(it, { first, count });
    const size_t k = (size_t)(it - free.begin());
    if (k + 1 < free.size() && free[k].first + free[k].second == free[k + 1].first)
    {
        free[k].second += free[k + 1].second;
        free.erase(free.begin() + (std::ptrdiff_t)k + 1);
    }
    if (k > 0 && free[k - 1].first + free[k - 1].second == free[k].first)
    {
        free[k - 1].second += free[k].second;
        free.erase(free.begin() + (std::ptrdiff_t)k);
    }
}
} // namespace

void RayScene::recordStreams(FramePassContext& fc, D3D12_RAYTRACING_INSTANCE_DESC* slot)
{
    m_streamsNow.clear();
    m_frame.streamPool = {};
    struct StreamBuild
    {
        uint32_t slot;
        BufferRef vertices;
        uint32_t triangles;
        uint64_t offset;
    };
    std::vector<StreamBuild> builds;
    // FrameResources::triangleStreams is the list the producers append to (slot = index, at most kMaxTriangleStreams).
    const uint32_t streamCount = (uint32_t)std::min<size_t>(fc.resources.triangleStreams.size(), kMaxTriangleStreams);
    for (uint32_t k = 0; k < streamCount; ++k)
    {
        const TriangleStream& ts = fc.resources.triangleStreams[k];
        if (!ts.vertices.valid() || ts.maxTriangles == 0) continue;
        builds.push_back({ k, ts.vertices, ts.maxTriangles, 0 });
    }
    if (builds.empty()) return;
    // One pool for every stream's capacity (it grows with the streams; the old pool is released after its frames).
    uint64_t total = 0;
    for (StreamBuild& b : builds)
    {
        if (m_streamTriangles[b.slot] != b.triangles)
        {
            D3D12_RAYTRACING_GEOMETRY_DESC gd{};
            gd.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            gd.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            gd.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            gd.Triangles.VertexCount = b.triangles * 3;
            gd.Triangles.VertexBuffer.StrideInBytes = 32;
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS in{};
            in.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            in.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
            in.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            in.NumDescs = 1;
            in.pGeometryDescs = &gd;
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
            m_device.d3d()->GetRaytracingAccelerationStructurePrebuildInfo(&in, &info);
            m_streamBytes[b.slot] = (info.ResultDataMaxSizeInBytes + 255) / 256 * 256;
            m_streamTriangles[b.slot] = b.triangles;
            m_streamScratchBytes = std::max<uint64_t>(m_streamScratchBytes, (info.ScratchDataSizeInBytes + 255) / 256 * 256);
        }
        b.offset = total;
        m_streamOffset[b.slot] = total;
        total += m_streamBytes[b.slot];
    }
    if (m_streamPool.bytes < total)
    {
        if (m_streamPool.resource) m_device.deferRelease(m_streamPool.resource);
        m_streamPool = createBuffer(total, true, true, L"RT stream BLAS pool");
    }
    const uint64_t scratchStride = std::max<uint64_t>(m_streamScratchBytes, 256);
    if (m_streamScratch.bytes < scratchStride * builds.size())
    {
        if (m_streamScratch.resource) m_device.deferRelease(m_streamScratch.resource);
        m_streamScratch = createBuffer(scratchStride * builds.size(), true, false, L"RT stream BLAS scratch");
    }
    // Descriptors: after the load-time dynamic and runtime instances; identity transform (the streams are in world space).
    for (uint32_t j = 0; j < builds.size(); ++j)
    {
        D3D12_RAYTRACING_INSTANCE_DESC d{};
        d.Transform[0][0] = d.Transform[1][1] = d.Transform[2][2] = 1;
        d.InstanceID = kRtInstanceStreamBase + builds[j].slot;
        d.InstanceMask = kRtMaskFluid;
        d.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE | D3D12_RAYTRACING_INSTANCE_FLAG_FORCE_OPAQUE;
        d.AccelerationStructure = m_streamPool.address() + builds[j].offset;
        slot[m_dynamicCountNow + j] = d;
        m_streamsNow.push_back({ builds[j].slot, builds[j].vertices });
    }
    m_dynamicCountNow += (uint32_t)builds.size();
    RenderGraph& g = fc.graph;
    const BufferRef pool = g.importBuffer(m_streamPool.resource.Get(), { "RT stream BLAS pool", m_streamPool.bytes, 0 });
    const BufferRef scratch = g.importBuffer(m_streamScratch.resource.Get(), { "RT stream BLAS scratch", m_streamScratch.bytes, 0 });
    m_frame.streamPool = pool;
    g.addPass("r.as.streams", QueueType::Compute,
              [&](PassBuilder& b) {
                  for (const StreamBuild& s : builds) b.use(s.vertices, Use::AccelerationStructureInput);
                  b.use(pool, Use::AccelerationStructureWrite);
                  b.use(scratch, Use::AccelerationStructureScratch);
              },
              [builds, pool, scratch, scratchStride](PassContext& c) {
                  const D3D12_GPU_VIRTUAL_ADDRESS poolAddress = c.resource(pool)->GetGPUVirtualAddress(), scratchAddress = c.resource(scratch)->GetGPUVirtualAddress();
                  for (size_t j = 0; j < builds.size(); ++j)
                  {
                      D3D12_RAYTRACING_GEOMETRY_DESC gd{};
                      gd.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                      gd.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
                      gd.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
                      gd.Triangles.VertexCount = builds[j].triangles * 3;
                      gd.Triangles.VertexBuffer.StartAddress = c.resource(builds[j].vertices)->GetGPUVirtualAddress();
                      gd.Triangles.VertexBuffer.StrideInBytes = 32;
                      D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
                      d.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
                      d.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
                      d.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
                      d.Inputs.NumDescs = 1;
                      d.Inputs.pGeometryDescs = &gd;
                      d.DestAccelerationStructureData = poolAddress + builds[j].offset;
                      d.ScratchAccelerationStructureData = scratchAddress + j * scratchStride;
                      c.cmd->BuildRaytracingAccelerationStructure(&d, 0, nullptr);
                  }
              });
}

void RayScene::recordRuntime(FramePassContext& fc, D3D12_RAYTRACING_INSTANCE_DESC* slot)
{
    m_dynamicCountNow = (uint32_t)m_dynamicDescs.size();
    if (m_runtimeInstanceCap == 0) return;
    const uint64_t frame = fc.frame.frameIndex;
    // Ranges of replaced meshes return once the frames that could still trace them have completed.
    for (size_t k = 0; k < m_runtimeFrees.size();)
    {
        const RuntimeFree& f = m_runtimeFrees[k];
        if (frame < f.frame + fc.framesInFlight + 1)
        {
            ++k;
            continue;
        }
        giveRange(m_runtimePoolFree, f.offset, f.bytes);
        giveRange(m_runtimeGeometryFree, f.geometryBase, f.geometryCount);
        m_runtimeFrees.erase(m_runtimeFrees.begin() + (std::ptrdiff_t)k);
    }
    const scene::Scene* src = m_scene.source();
    auto materialAlpha = [&](uint32_t material) { return src && material < src->materials.size() && src->materials[material].alphaCutoff > 0; };
    const auto& instances = m_scene.instances();
    const auto& meshes = m_scene.meshes();
    const uint32_t staticCount = m_scene.staticInstanceCount(), staticMeshes = m_scene.staticMeshCount();
    const D3D12_GPU_VIRTUAL_ADDRESS vertices = m_scene.buffer("vertices")->GetGPUVirtualAddress();
    const D3D12_GPU_VIRTUAL_ADDRESS indices = m_scene.buffer("indices")->GetGPUVirtualAddress();
    struct Build
    {
        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries;
        D3D12_GPU_VIRTUAL_ADDRESS dest = 0, scratch = 0;
    };
    auto builds = std::make_shared<std::vector<Build>>();
    uint64_t scratchUsed = 0;
    if (m_runtimeSeen.size() < instances.size()) m_runtimeSeen.resize(instances.size(), 0);
    uint32_t live = 0;
    for (uint32_t i = staticCount; i < (uint32_t)instances.size(); ++i)
    {
        const gpu::Instance& in = instances[i];
        D3D12_RAYTRACING_INSTANCE_DESC desc{};
        for (int r = 0; r < 3; ++r)
        {
            desc.Transform[r][0] = in.objectToWorld[r].x;
            desc.Transform[r][1] = in.objectToWorld[r].y;
            desc.Transform[r][2] = in.objectToWorld[r].z;
            desc.Transform[r][3] = in.objectToWorld[r].w;
        }
        const bool runtimeMesh = in.mesh >= staticMeshes && in.mesh - staticMeshes < (uint32_t)m_runtimeBlas.size();
        const bool visible = (in.flags & gpu::kInstanceHidden) == 0 && runtimeMesh && in.mesh < meshes.size();
        if (!visible)
        {
            // Gone this frame: where it was changed (B3: GI invalidates the cells whose rays crossed it).
            if (m_runtimeSeen[i] && in.mesh < meshes.size()) m_changes.push_back(descBounds(desc, meshes[in.mesh].boundsSphere));
            m_runtimeSeen[i] = 0;
            continue;
        }
        RuntimeBlas& rb = m_runtimeBlas[in.mesh - staticMeshes];
        const uint64_t generation = m_scene.runtimeMeshGeneration(in.mesh);
        if (rb.generation != generation)
        {
            if (rb.generation != 0) m_runtimeFrees.push_back({ frame, rb.offset, rb.bytes, rb.geometryBase, rb.geometryCount });
            rb = RuntimeBlas{};
            const gpu::Mesh& gm = meshes[in.mesh];
            const std::vector<gpu::Submesh> subs = m_scene.runtimeSubmeshes(in.mesh);
            Build b;
            std::vector<uint32_t> submeshOf;
            for (uint32_t s = 0; s < (uint32_t)subs.size(); ++s)
            {
                if (subs[s].indexCount == 0) continue;
                const bool alpha = materialAlpha(subs[s].material);
                rb.anyAlpha |= alpha;
                D3D12_RAYTRACING_GEOMETRY_DESC g{};
                g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
                g.Flags = alpha ? D3D12_RAYTRACING_GEOMETRY_FLAG_NONE : D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
                g.Triangles.VertexBuffer = { vertices + (uint64_t)gm.vertexOffset * sizeof(gpu::Vertex), sizeof(gpu::Vertex) };
                g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
                g.Triangles.VertexCount = gm.vertexCount;
                g.Triangles.IndexBuffer = indices + ((uint64_t)gm.indexOffset + subs[s].indexOffset) * sizeof(uint32_t);
                g.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
                g.Triangles.IndexCount = subs[s].indexCount;
                b.geometries.push_back(g);
                submeshOf.push_back(s);
            }
            if (b.geometries.empty()) continue;  // nothing to trace
            D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS inputs{};
            inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
            inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
            inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
            inputs.NumDescs = (UINT)b.geometries.size();
            inputs.pGeometryDescs = b.geometries.data();
            D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO sizes{};
            m_device.d3d()->GetRaytracingAccelerationStructurePrebuildInfo(&inputs, &sizes);
            const uint64_t bytes = alignUp(sizes.ResultDataMaxSizeInBytes, kAsAlign), scratch = alignUp(sizes.ScratchDataSizeInBytes, kAsAlign);
            if (scratchUsed + scratch > m_runtimeScratch.bytes) continue;  // this frame's scratch is spent: built next frame
            const uint64_t offset = takeRange<uint64_t>(m_runtimePoolFree, bytes, kAsAlign);
            const uint64_t geometryBase = offset == UINT64_MAX ? UINT64_MAX : takeRange<uint32_t>(m_runtimeGeometryFree, (uint32_t)b.geometries.size(), 1u);
            if (offset == UINT64_MAX || geometryBase == UINT64_MAX)
            {
                if (offset != UINT64_MAX) giveRange(m_runtimePoolFree, offset, bytes);
                static bool warned = false;
                if (!warned) logf("RayScene: runtime BLAS pool or geometry records full (mesh %u, %llu B): not traced until space returns\n", in.mesh, (unsigned long long)bytes);
                warned = true;
                continue;
            }
            rb.generation = generation;
            rb.offset = offset;
            rb.bytes = bytes;
            rb.geometryBase = (uint32_t)geometryBase;
            rb.geometryCount = (uint32_t)b.geometries.size();
            for (uint32_t k = 0; k < rb.geometryCount; ++k)
                m_runtimeGeometries[rb.geometryBase + k] = { gm.indexOffset + subs[submeshOf[k]].indexOffset, submeshOf[k], 0, gpu::kNone };
            b.dest = m_runtimePool.address() + offset;
            b.scratch = m_runtimeScratch.address() + scratchUsed;
            scratchUsed += scratch;
            builds->push_back(std::move(b));
        }
        if (live >= m_runtimeInstanceCap) break;  // GpuScene's own capacity: never reached
        desc.InstanceID = m_runtimeRecordBase + live;
        desc.InstanceMask = kRtMaskAll;
        desc.InstanceContributionToHitGroupIndex = 0;
        desc.AccelerationStructure = m_runtimePool.address() + rb.offset;
        slot[m_dynamicDescs.size() + live] = desc;
        m_runtimeRecords[live] = { i, m_runtimeGeometryBase + rb.geometryBase, gpu::kNone, 0 };
        if (!m_runtimeSeen[i]) m_changes.push_back(descBounds(desc, meshes[in.mesh].boundsSphere));  // appeared (B3)
        m_runtimeSeen[i] = 1;
        ++live;
    }
    m_dynamicCountNow = (uint32_t)m_dynamicDescs.size() + live;

    // Records of this frame (runtime instances, then the runtime geometries) through the ring into the record buffers.
    RenderGraph& g = fc.graph;
    const uint64_t slotBytes = (uint64_t)(m_runtimeInstanceCap + m_runtimeGeometryCap) * 16, slotOffset = (frame % kDescSlots) * slotBytes;
    std::memcpy(m_runtimeRingMapped + slotOffset, m_runtimeRecords.data(), (size_t)m_runtimeInstanceCap * 16);
    std::memcpy(m_runtimeRingMapped + slotOffset + (uint64_t)m_runtimeInstanceCap * 16, m_runtimeGeometries.data(), (size_t)m_runtimeGeometryCap * 16);
    const BufferRef records = m_frame.instances;
    const BufferRef geometries = g.importBuffer(m_geometryBuffer.resource.Get(), { "RT geometries", m_geometryBuffer.bytes, sizeof(RtGeometry) });
    m_frame.geometries = geometries;  // declareTraversal orders the hit passes' reads after this copy
    ID3D12Resource* ring = m_runtimeRing.Get();
    const uint64_t recordDst = (uint64_t)m_runtimeRecordBase * 16, geometryDst = (uint64_t)m_runtimeGeometryBase * 16;
    const uint64_t recordBytes = (uint64_t)m_runtimeInstanceCap * 16, geometryBytes = (uint64_t)m_runtimeGeometryCap * 16;
    g.addPass("r.as.runtime.records", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(records, Use::CopyDst);
                  b.use(geometries, Use::CopyDst);
              },
              [=](PassContext& c) {
                  c.cmd->CopyBufferRegion(c.resource(records), recordDst, ring, slotOffset, recordBytes);
                  c.cmd->CopyBufferRegion(c.resource(geometries), geometryDst, ring, slotOffset + recordBytes, geometryBytes);
              });
    const BufferRef pool = g.importBuffer(m_runtimePool.resource.Get(), { "RT runtime BLAS pool", m_runtimePool.bytes, 0 });
    m_frame.runtimePool = pool;
    if (builds->empty()) return;
    const BufferRef scratch = g.importBuffer(m_runtimeScratch.resource.Get(), { "RT runtime BLAS scratch", m_runtimeScratch.bytes, 0 });
    g.addPass("r.as.runtime.blas", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(pool, Use::AccelerationStructureWrite);
                  b.use(scratch, Use::AccelerationStructureScratch);
              },
              [builds](PassContext& c) {
                  for (Build& b : *builds)
                  {
                      D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
                      d.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
                      d.Inputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
                      d.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
                      d.Inputs.NumDescs = (UINT)b.geometries.size();
                      d.Inputs.pGeometryDescs = b.geometries.data();
                      d.DestAccelerationStructureData = b.dest;
                      d.ScratchAccelerationStructureData = b.scratch;
                      c.cmd->BuildRaytracingAccelerationStructure(&d, 0, nullptr);
                  }
              });
}

// A3 FX particle lights (S_STATUS_KO.md 10): the groups of the scene light buffer's tail for the hits' choice
// (FxLightGroups.hlsl; HitLocalLights.hlsli rtFxWeight / rtFxChoose), one pass per frame after the FX writer (the tail and its count are
// graph resources: FrameResources::fxLights / fxLightCount); its SRV goes into the frame's header slot at execution
// (word 21), as the light functions' (word 20).
void RayScene::recordFxLights(FramePassContext& fc)
{
    const GpuScene::FxLightRange range = m_scene.fxLightRange();
    if (range.capacity == 0 || !fc.resources.fxLights.valid() || !fc.resources.fxLightCount.valid()) return;
    uint8_t* word = lightSlot(fc) + kLightFunctionOffset + 4;
    RenderGraph& g = fc.graph;
    const BufferRef cdf = g.createBuffer({ "RT FX light groups", 16ull + 32ull * ((range.capacity + 31) / 32), 0 });
    const BufferRef lights = fc.resources.fxLights, count = fc.resources.fxLightCount;
    m_frame.fxCdf = cdf;
    m_frame.fxLights = lights;
    ID3D12PipelineState* pso = m_shaders.compute("RayTracing/FxLightGroups");
    g.addPass("r.lights.fxgroups", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(lights, Use::SrvCompute);
                  b.use(count, Use::SrvCompute);
                  b.use(cdf, Use::UavCompute);
              },
              [word, pso, cdf, constants = fc.frameConstantsFor(fc.frame.mainView), first = range.first, capacity = range.capacity](PassContext& c) {
                  const uint32_t srv = c.srv(cdf);
                  std::memcpy(word, &srv, 4);
                  const uint32_t k[8] = { c.uav(cdf), 0, 0, first, capacity, 0, 0, 0 };
                  c.cmd->SetPipelineState(pso);
                  c.computeConstants(k, 8);
                  c.bindFrameConstants(constants);
                  c.cmd->Dispatch(1, 1, 1);
              });
}

void RayScene::recordLightFunctions(FramePassContext& fc)
{
    m_frame.lightFunctions = {};
    const BufferRef functions = fc.resources.lightFunctions;
    if (!functions.valid() || m_lightSrvNow == 0xFFFFFFFFu) return;  // none, or no local lights to apply them to
    m_frame.lightFunctions = functions;
    uint8_t* word = m_lightRingMapped + (fc.frame.frameIndex % kDescSlots) * m_lightSlotBytes + kLightFunctionOffset;
    // The table's SRV exists when a pass executes: this one writes it into the frame's header slot (frames in flight <=
    // kDescSlots) before the hit passes run.
    fc.graph.addPass("r.lights.functions", QueueType::Compute,
                     [&](PassBuilder& b) {
                         b.use(functions, Use::SrvGraphics);
                         b.keep();
                     },
                     [word, functions](PassContext& c) {
                         const uint32_t srv = c.srv(functions);
                         std::memcpy(word, &srv, 4);
                     });
}

void RayScene::declareDecals(PassBuilder& b) const
{
    if (!m_decalTlasRef.valid()) return;
    b.use(m_decalTlasRef, Use::AccelerationStructureRead);
    b.use(m_decalFrames, Use::SrvGraphics);  // as declareTraversal: ray and compute passes of R
}
} // namespace unx::render::rt
