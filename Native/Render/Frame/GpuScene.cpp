#include "unx/render/GpuScene.h"

#include "unx/render/Shaders.h"
#include "unx/scene/MaterialModel.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace unx::render
{
namespace
{
uint32_t octEncode(float3 n)
{
    float s = std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z);
    float ex = n.x / s, ey = n.y / s;
    if (n.z < 0)
    {
        const float ox = (1 - std::fabs(ey)) * (ex >= 0 ? 1.f : -1.f);
        const float oy = (1 - std::fabs(ex)) * (ey >= 0 ? 1.f : -1.f);
        ex = ox;
        ey = oy;
    }
    auto q = [](float v) { return (uint32_t)(int32_t)std::lround(std::clamp(v, -1.f, 1.f) * 32767.f) & 0xFFFFu; };
    return q(ex) | (q(ey) << 16);
}

void rows(const float3x4& m, float4 out[3])
{
    for (int r = 0; r < 3; ++r) out[r] = { m.m[r][0], m.m[r][1], m.m[r][2], m.m[r][3] };
}

float3x4 compose(const float3x4& a, const float3x4& b)  // a * b (affine)
{
    float3x4 r;
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            float s = (j == 3) ? a.m[i][3] : 0.f;
            for (int k = 0; k < 3; ++k) s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    }
    return r;
}

float3 anyTangent(float3 n)
{
    const float3 a = std::fabs(n.y) < 0.99f ? float3{ 0, 1, 0 } : float3{ 1, 0, 0 };
    return normalize(cross(a, n));
}
} // namespace

GpuScene::GpuScene(Device& device) : m_device(device) {}

GpuScene::~GpuScene()
{
    for (Buffer* b : { &m_instanceBuffer, &m_meshBuffer, &m_submeshBuffer, &m_vertexBuffer, &m_indexBuffer, &m_materialBuffer, &m_materialRemapBuffer, &m_lightBuffer,
                       &m_skinBuffer, &m_bonePalette, &m_prevBonePalette, &m_albedoTable, &m_clusterBuffer, &m_lodLevelBuffer, &m_lodLevelClusterBuffer,
                       &m_clusterVertexIndexBuffer, &m_clusterTriangleBuffer })
        release(*b);
    for (auto& [name, b] : m_named) release(b);
    DescriptorHeaps& h = m_device.descriptors();
    for (uint32_t u : { m_instanceUav, m_paletteUav, m_prevPaletteUav })
        if (u != gpu::kNone) h.freeResource(u);
    for (Upload& u : m_uploads)
    {
        if (u.buffer) m_device.deferRelease(u.buffer);
        if (u.srv != gpu::kNone) h.freeResource(u.srv);
    }
}

void GpuScene::release(Buffer& b)
{
    if (b.resource) m_device.deferRelease(b.resource);
    if (b.srv != gpu::kNone)
    {
        DescriptorHeaps* h = &m_device.descriptors();
        uint32_t srv = b.srv;
        m_device.deferCall([h, srv] { h->freeResource(srv); });
    }
    b = {};
}

GpuScene::Buffer GpuScene::createStructured(const void* data, size_t stride, size_t count, const wchar_t* name, bool uav)
{
    // Empty streams still get one zeroed element so every published index is a valid descriptor.
    std::vector<uint8_t> zero;
    if (count == 0)
    {
        zero.assign(stride, 0);
        data = zero.data();
        count = 1;
    }
    const uint64_t bytes = (uint64_t)stride * count;
    Buffer b;
    b.count = (uint32_t)count;
    D3D12_HEAP_PROPERTIES defaultHeap{ D3D12_HEAP_TYPE_DEFAULT }, uploadHeap{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_RESOURCE_DESC1 dd = d;
    if (uav) dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(m_device.d3d()->CreateCommittedResource3(&defaultHeap, D3D12_HEAP_FLAG_NONE, &dd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&b.resource)),
          "GpuScene buffer");
    b.resource->SetName(name);
    ComPtr<ID3D12Resource> staging;
    check(m_device.d3d()->CreateCommittedResource3(&uploadHeap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&staging)),
          "GpuScene staging");
    void* mapped = nullptr;
    D3D12_RANGE none{ 0, 0 };
    check(staging->Map(0, &none, &mapped), "Map staging");
    std::memcpy(mapped, data, (size_t)bytes);
    staging->Unmap(0, nullptr);
    CommandList cl = m_device.acquireCommandList(QueueType::Graphics);
    cl.list->CopyBufferRegion(b.resource.Get(), 0, staging.Get(), 0, bytes);
    uint64_t fence = m_device.submit(cl);
    m_device.queue(QueueType::Graphics).waitCpu(fence);

    DescriptorHeaps& h = m_device.descriptors();
    b.srv = h.allocateResource();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Format = DXGI_FORMAT_UNKNOWN;
    sd.Buffer.NumElements = (UINT)count;
    sd.Buffer.StructureByteStride = (UINT)stride;
    m_device.d3d()->CreateShaderResourceView(b.resource.Get(), &sd, h.resourceCpu(b.srv));
    return b;
}

void GpuScene::upload(const scene::Scene& s)
{
    scene::validate(s);
    m_source = &s;
    ++m_revision;

    // Meshes, submeshes, vertices, indices, skin.
    std::vector<gpu::Submesh> submeshes;
    std::vector<gpu::Vertex> vertices;
    std::vector<uint32_t> indices;
    std::vector<gpu::SkinVertex> skin;
    m_meshes.clear();
    for (const scene::Mesh& m : s.meshes)
    {
        gpu::Mesh g{};
        float3 lo{ 1e30f, 1e30f, 1e30f }, hi{ -1e30f, -1e30f, -1e30f };
        for (const float3& p : m.positions)
        {
            lo = { std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z) };
            hi = { std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z) };
        }
        const float3 c = (lo + hi) * 0.5f;
        float radius = 0;
        for (const float3& p : m.positions) radius = std::max(radius, length(p - c));
        g.boundsSphere = { c.x, c.y, c.z, radius };
        g.boundsMin = lo;
        g.boundsMax = hi;
        g.vertexOffset = (uint32_t)vertices.size();
        g.vertexCount = (uint32_t)m.positions.size();
        g.indexOffset = (uint32_t)indices.size();
        g.triangleCount = (uint32_t)(m.indices.size() / 3);
        g.submeshOffset = (uint32_t)submeshes.size();
        g.submeshCount = (uint32_t)m.submeshes.size();
        g.clusterOffset = 0;
        g.clusterCount = 0;
        g.lodLevelOffset = 0;
        g.skinOffset = m.skin.joints.empty() ? gpu::kNone : (uint32_t)skin.size();
        for (size_t v = 0; v < m.positions.size(); ++v)
        {
            gpu::Vertex gv{};
            gv.position = m.positions[v];
            gv.normalOct = octEncode(m.normals[v]);
            const float3 t = m.tangents.empty() ? anyTangent(m.normals[v]) : float3{ m.tangents[v].x, m.tangents[v].y, m.tangents[v].z };
            gv.tangentOct = octEncode(t);
            gv.tangentSign = (!m.tangents.empty() && m.tangents[v].w < 0) ? 1u : 0u;
            gv.uv = m.uv0.empty() ? float2{} : m.uv0[v];
            vertices.push_back(gv);
        }
        indices.insert(indices.end(), m.indices.begin(), m.indices.end());
        for (const scene::Submesh& sm : m.submeshes) submeshes.push_back({ sm.indexOffset, sm.indexCount, sm.material, 0 });
        for (size_t v = 0; v < m.skin.joints.size() / 4; ++v)
        {
            gpu::SkinVertex sv{};
            const uint16_t* j = &m.skin.joints[4 * v];
            const float* w = &m.skin.weights[4 * v];
            auto u16 = [](float x) { return (uint32_t)std::lround(std::clamp(x, 0.f, 1.f) * 65535.f); };
            sv.joints01 = j[0] | ((uint32_t)j[1] << 16);
            sv.joints23 = j[2] | ((uint32_t)j[3] << 16);
            sv.weights01 = u16(w[0]) | (u16(w[1]) << 16);
            sv.weights23 = u16(w[2]) | (u16(w[3]) << 16);
            skin.push_back(sv);
        }
        m_meshes.push_back(g);
    }

    // Materials (textures are bound by M's texture system; until then no texture indices are published).
    std::vector<gpu::Material>& materials = m_materials;
    materials.clear();
    for (const scene::Material& m : s.materials)
    {
        gpu::Material g{};
        g.baseColor = m.baseColor;
        g.roughness = m.roughness;
        g.emissive = m.emissive;
        g.metallic = m.metallic;
        g.specular = m.specular;
        g.alphaCutoff = m.alphaCutoff;
        g.transmission = m.transmission;
        g.ior = m.ior;
        g.classFlags = (uint32_t)m.cls | ((m.twoSided ? gpu::MaterialTwoSided : 0u) | (m.alphaCutoff > 0 ? gpu::MaterialAlphaTested : 0u)) << 8;
        g.baseColorTexture = g.normalTexture = g.roughMetalTexture = g.emissiveTexture = g.occlusionTexture = gpu::kNone;  // setMaterialTextures
        g.textureClamp = 0;
        g.revision = m_revision;
        materials.push_back(g);
    }

    // Instances, material remaps, bone palettes (jointToModel * inverseBind per skinned instance).
    std::vector<uint32_t> remap;
    std::vector<float4> palette;
    m_instances.clear();
    for (const scene::Instance& in : s.instances)
    {
        gpu::Instance g{};
        rows(in.transform, g.objectToWorld);
        rows(in.transform, g.prevObjectToWorld);
        g.mesh = in.mesh;
        g.flags = in.flags;
        g.materialRemap = gpu::kNone;
        if (!in.materialOverrides.empty())
        {
            g.materialRemap = (uint32_t)remap.size();
            remap.insert(remap.end(), in.materialOverrides.begin(), in.materialOverrides.end());
        }
        g.bonePalette = gpu::kNone;
        if ((in.flags & scene::InstanceSkinned) && in.skeleton != scene::kNone)
        {
            const scene::Mesh& mesh = s.meshes[in.mesh];
            const scene::Skeleton& sk = s.skeletons[in.skeleton];
            g.bonePalette = (uint32_t)(palette.size() / 3);
            for (size_t j = 0; j < mesh.skin.inverseBind.size(); ++j)
            {
                float4 r[3];
                rows(compose(sk.jointToModel[j], mesh.skin.inverseBind[j]), r);
                palette.insert(palette.end(), r, r + 3);
            }
        }
        g.transformRevision = m_revision;
        g.deformRevision = m_revision;
        g.windStiffness = in.wind.stiffness;
        g.windPhase = in.wind.phase;
        g.windAnchor = in.wind.anchorHeight;
        m_instances.push_back(g);
    }

    // Lights.
    std::vector<gpu::Light> lights;
    for (const scene::Light& l : s.lights)
    {
        gpu::Light g{};
        g.position = l.position;
        g.forward = l.forward;
        g.right = l.right;
        g.range = l.range;
        g.intensity = l.intensity;
        g.color = l.color;
        const float ci = std::cos(l.spotInner), co = std::cos(l.spotOuter);
        g.spotScale = 1.0f / std::max(ci - co, 1e-4f);
        g.spotOffset = -co * g.spotScale;
        g.size = l.size;
        g.typeFlags = (uint32_t)l.type | ((l.castShadow ? 1u : 0u) << 8) | (0xFFFFu << 16);
        g.revision = m_revision;
        lights.push_back(g);
    }

    for (Buffer* b : { &m_instanceBuffer, &m_meshBuffer, &m_submeshBuffer, &m_vertexBuffer, &m_indexBuffer, &m_materialBuffer, &m_materialRemapBuffer, &m_lightBuffer,
                       &m_skinBuffer, &m_bonePalette, &m_prevBonePalette, &m_albedoTable })
        release(*b);
    m_instanceBuffer = createStructured(m_instances.data(), sizeof(gpu::Instance), m_instances.size(), L"scene instances", true);
    m_meshBuffer = createStructured(m_meshes.data(), sizeof(gpu::Mesh), m_meshes.size(), L"scene meshes");
    m_submeshBuffer = createStructured(submeshes.data(), sizeof(gpu::Submesh), submeshes.size(), L"scene submeshes");
    m_vertexBuffer = createStructured(vertices.data(), sizeof(gpu::Vertex), vertices.size(), L"scene vertices");
    m_indexBuffer = createStructured(indices.data(), sizeof(uint32_t), indices.size(), L"scene indices");
    m_materialBuffer = createStructured(materials.data(), sizeof(gpu::Material), materials.size(), L"scene materials");
    m_materialRemapBuffer = createStructured(remap.data(), sizeof(uint32_t), remap.size(), L"scene material remap");
    m_lightBuffer = createStructured(lights.data(), sizeof(gpu::Light), lights.size(), L"scene lights");
    m_skinBuffer = createStructured(skin.data(), sizeof(gpu::SkinVertex), skin.size(), L"scene skin");
    m_bonePalette = createStructured(palette.data(), sizeof(float4), palette.size(), L"bone palette", true);
    m_prevBonePalette = createStructured(palette.data(), sizeof(float4), palette.size(), L"bone palette (previous)", true);
    // Update state: mirrors, raw UAVs of the updatable buffers (SceneUpdate.hlsl).
    m_palette = palette;
    m_prevPalette = palette;
    m_poses = s.skeletons;
    m_transformFrame.assign(m_instances.size(), UINT64_MAX);
    m_paletteFrame.assign(m_instances.size(), UINT64_MAX);
    m_movedNow.clear();
    m_movedBefore.clear();
    m_posedNow.clear();
    m_posedBefore.clear();
    m_records.clear();
    m_recordMarked.assign(m_instances.size(), 0);
    DescriptorHeaps& h = m_device.descriptors();
    auto rawUav = [&](uint32_t& index, const Buffer& b, uint64_t bytes) {
        if (index == gpu::kNone) index = h.allocateResource();
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = (UINT)(bytes / 4);
        ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        m_device.d3d()->CreateUnorderedAccessView(b.resource.Get(), nullptr, &ud, h.resourceCpu(index));
    };
    rawUav(m_instanceUav, m_instanceBuffer, (uint64_t)m_instanceBuffer.count * sizeof(gpu::Instance));
    rawUav(m_paletteUav, m_bonePalette, (uint64_t)m_bonePalette.count * sizeof(float4));
    rawUav(m_prevPaletteUav, m_prevBonePalette, (uint64_t)m_prevBonePalette.count * sizeof(float4));
    const std::vector<float>& table = scene::model::directionalAlbedoTable();
    m_albedoTable = createStructured(table.data(), sizeof(float), table.size(), L"material model E table");
    if (!m_clusterBuffer.resource) setClusters(ClusterData{});
}

void GpuScene::setClusters(ClusterData data)
{
    m_clusterData = std::move(data);
    const ClusterData& c = m_clusterData;
    for (Buffer* b : { &m_clusterBuffer, &m_lodLevelBuffer, &m_lodLevelClusterBuffer, &m_clusterVertexIndexBuffer, &m_clusterTriangleBuffer }) release(*b);
    for (auto& [name, b] : m_named) release(b);
    m_named.clear();
    m_clusterBuffer = createStructured(c.clusters.data(), sizeof(gpu::Cluster), c.clusters.size(), L"scene clusters");
    m_lodLevelBuffer = createStructured(c.lodLevels.data(), sizeof(gpu::LodLevel), c.lodLevels.size(), L"scene lod levels");
    m_lodLevelClusterBuffer = createStructured(c.lodLevelClusters.data(), sizeof(uint32_t), c.lodLevelClusters.size(), L"lod level clusters");
    m_clusterVertexIndexBuffer = createStructured(c.clusterVertexIndices.data(), sizeof(uint32_t), c.clusterVertexIndices.size(), L"cluster vertex indices");
    m_clusterTriangleBuffer = createStructured(c.clusterTriangles.data(), sizeof(uint32_t), c.clusterTriangles.size(), L"cluster triangles");
    m_clusterBuffer.count = (uint32_t)c.clusters.size();
    for (const ClusterData::Named& n : c.named)
    {
        if (n.stride == 0 || n.bytes.size() % n.stride != 0)
            fail("GpuScene::setClusters: buffer '%s' size %zu is not a multiple of stride %u", n.name.c_str(), n.bytes.size(), n.stride);
        const std::wstring wide(n.name.begin(), n.name.end());
        m_named.emplace_back(n.name, createStructured(n.bytes.data(), n.stride, n.bytes.size() / n.stride, wide.c_str()));
    }
    if (!c.meshes.empty())
    {
        if (c.meshes.size() != m_meshes.size()) fail("GpuScene::setClusters: %zu mesh ranges for %zu meshes", c.meshes.size(), m_meshes.size());
        for (size_t i = 0; i < m_meshes.size(); ++i)
        {
            m_meshes[i].clusterOffset = c.meshes[i].clusterOffset;
            m_meshes[i].clusterCount = c.meshes[i].clusterCount;
            m_meshes[i].lodLevelOffset = c.meshes[i].lodLevelOffset;
        }
        release(m_meshBuffer);
        m_meshBuffer = createStructured(m_meshes.data(), sizeof(gpu::Mesh), m_meshes.size(), L"scene meshes");
    }
    ++m_revision;
}

void GpuScene::setMaterialTextures(const std::vector<gpu::MaterialTextures>& perMaterial)
{
    if (perMaterial.size() != m_materials.size()) fail("GpuScene::setMaterialTextures: %zu entries for %zu materials", perMaterial.size(), m_materials.size());
    const uint32_t revision = m_revision + 1;
    bool changed = false;
    for (size_t i = 0; i < m_materials.size(); ++i)
    {
        gpu::Material& g = m_materials[i];
        const gpu::MaterialTextures& t = perMaterial[i];
        if (g.baseColorTexture == t.baseColor && g.normalTexture == t.normal && g.roughMetalTexture == t.roughMetal && g.emissiveTexture == t.emissive &&
            g.occlusionTexture == t.occlusion && g.textureClamp == t.clamp)
            continue;
        g.baseColorTexture = t.baseColor;
        g.normalTexture = t.normal;
        g.roughMetalTexture = t.roughMetal;
        g.emissiveTexture = t.emissive;
        g.occlusionTexture = t.occlusion;
        g.textureClamp = t.clamp;
        g.revision = revision;
        changed = true;
    }
    if (!changed) return;
    m_revision = revision;
    release(m_materialBuffer);
    m_materialBuffer = createStructured(m_materials.data(), sizeof(gpu::Material), m_materials.size(), L"scene materials");
}

void GpuScene::markRecord(uint32_t instance)
{
    if (m_recordMarked[instance]) return;
    m_recordMarked[instance] = 1;
    m_records.push_back(instance);
}

uint32_t GpuScene::paletteJoints(uint32_t instance) const
{
    const gpu::Instance& g = m_instances[instance];
    return g.bonePalette == gpu::kNone ? 0u : (uint32_t)m_source->meshes[g.mesh].skin.inverseBind.size();
}

void GpuScene::writePalette(uint32_t instance, std::vector<float4>& palette)
{
    const scene::Instance& in = m_source->instances[instance];
    const scene::Mesh& mesh = m_source->meshes[in.mesh];
    const scene::Skeleton& pose = m_poses[in.skeleton];
    const uint32_t first = m_instances[instance].bonePalette;
    for (size_t j = 0; j < mesh.skin.inverseBind.size(); ++j) rows(compose(pose.jointToModel[j], mesh.skin.inverseBind[j]), &palette[(first + j) * 3]);
}

void GpuScene::updateTransforms(uint64_t frameIndex, std::span<const InstanceTransformUpdate> updates)
{
    for (const InstanceTransformUpdate& u : updates)
    {
        if (u.instance >= m_instances.size()) fail("GpuScene::updateTransforms: instance %u of %zu", u.instance, m_instances.size());
        gpu::Instance& g = m_instances[u.instance];
        if (m_transformFrame[u.instance] != frameIndex)
        {
            std::memcpy(g.prevObjectToWorld, g.objectToWorld, sizeof g.objectToWorld);  // the previous rendered frame's
            ++g.transformRevision;
            m_transformFrame[u.instance] = frameIndex;
            m_movedNow.push_back(u.instance);
        }
        rows(u.objectToWorld, g.objectToWorld);
        markRecord(u.instance);
    }
}

void GpuScene::updateSkeleton(uint64_t frameIndex, uint32_t skeleton, std::span<const float3x4> jointToModel)
{
    if (skeleton >= m_poses.size()) fail("GpuScene::updateSkeleton: skeleton %u of %zu", skeleton, m_poses.size());
    if (jointToModel.size() != m_poses[skeleton].jointToModel.size())
        fail("GpuScene::updateSkeleton: %zu joints for skeleton '%s' with %zu", jointToModel.size(), m_poses[skeleton].name.c_str(), m_poses[skeleton].jointToModel.size());
    m_poses[skeleton].jointToModel.assign(jointToModel.begin(), jointToModel.end());
    for (uint32_t i = 0; i < m_instances.size(); ++i)
    {
        gpu::Instance& g = m_instances[i];
        if (g.bonePalette == gpu::kNone || m_source->instances[i].skeleton != skeleton) continue;
        if (m_paletteFrame[i] != frameIndex)
        {
            const uint32_t first = g.bonePalette * 3, n = paletteJoints(i) * 3;
            std::copy(m_palette.begin() + first, m_palette.begin() + first + n, m_prevPalette.begin() + first);
            ++g.deformRevision;
            m_paletteFrame[i] = frameIndex;
            m_posedNow.push_back(i);
            markRecord(i);
        }
        writePalette(i, m_palette);
    }
}

void GpuScene::setInstanceVisible(uint32_t instance, bool visible)
{
    if (instance >= m_instances.size()) fail("GpuScene::setInstanceVisible: instance %u of %zu", instance, m_instances.size());
    gpu::Instance& g = m_instances[instance];
    const uint32_t flags = visible ? g.flags & ~gpu::kInstanceHidden : g.flags | gpu::kInstanceHidden;
    if (flags == g.flags) return;
    g.flags = flags;
    markRecord(instance);
}

void GpuScene::flushUpdates(uint64_t frameIndex, uint32_t framesInFlight, ShaderLibrary& shaders)
{
    // Instances changed in the previous frame and not in this one settle: previous = current.
    for (uint32_t i : m_movedBefore)
        if (m_transformFrame[i] != frameIndex)
        {
            std::memcpy(m_instances[i].prevObjectToWorld, m_instances[i].objectToWorld, sizeof m_instances[i].objectToWorld);
            markRecord(i);
        }
    std::vector<uint32_t> settledPalettes;
    for (uint32_t i : m_posedBefore)
        if (m_paletteFrame[i] != frameIndex)
        {
            const uint32_t first = m_instances[i].bonePalette * 3, n = paletteJoints(i) * 3;
            std::copy(m_palette.begin() + first, m_palette.begin() + first + n, m_prevPalette.begin() + first);
            settledPalettes.push_back(i);
        }
    m_movedBefore.swap(m_movedNow);
    m_movedNow.clear();
    m_posedBefore.swap(m_posedNow);
    m_posedNow.clear();

    // 16-byte elements: (target << 28 | element) headers, then payloads (SceneUpdate.hlsl).
    std::vector<uint32_t> headers;
    std::vector<float4> payload;
    constexpr uint32_t kRecordElements = sizeof(gpu::Instance) / 16;
    static_assert(sizeof(gpu::Instance) % 16 == 0);
    for (uint32_t i : m_records)
    {
        const float4* src = reinterpret_cast<const float4*>(&m_instances[i]);
        for (uint32_t k = 0; k < kRecordElements; ++k)
        {
            headers.push_back(i * kRecordElements + k);
            payload.push_back(src[k]);
        }
        m_recordMarked[i] = 0;
    }
    auto addRows = [&](uint32_t target, const std::vector<float4>& rowsOf, uint32_t instance) {
        const uint32_t first = m_instances[instance].bonePalette * 3, n = paletteJoints(instance) * 3;
        for (uint32_t k = 0; k < n; ++k)
        {
            headers.push_back(target << 28 | (first + k));
            payload.push_back(rowsOf[first + k]);
        }
    };
    for (uint32_t i : m_posedBefore)  // posed in this frame (swapped above)
    {
        addRows(1, m_palette, i);
        addRows(2, m_prevPalette, i);
    }
    for (uint32_t i : settledPalettes) addRows(2, m_prevPalette, i);
    m_records.clear();
    if (headers.empty()) return;
    if (headers.size() >= (1u << 28) || m_instances.size() * kRecordElements >= (1u << 28)) fail("GpuScene::flushUpdates: element index beyond 28 bits");

    // Upload slot of this frame (grown when needed; the caller waited for the frame that used it last).
    if (m_uploads.size() < framesInFlight) m_uploads.resize(framesInFlight);
    Upload& u = m_uploads[frameIndex % framesInFlight];
    const uint64_t headerBytes = (headers.size() * 4 + 15) & ~uint64_t(15);
    const uint64_t bytes = headerBytes + payload.size() * 16;
    DescriptorHeaps& h = m_device.descriptors();
    if (u.bytes < bytes)
    {
        if (u.buffer) m_device.deferRelease(u.buffer);
        u.bytes = std::max<uint64_t>(bytes + bytes / 2, 64 * 1024);
        D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC1 d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = u.bytes;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(m_device.d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&u.buffer)),
              "GpuScene update upload");
        u.buffer->SetName(L"scene update upload");
        D3D12_RANGE none{ 0, 0 };
        check(u.buffer->Map(0, &none, reinterpret_cast<void**>(&u.mapped)), "map scene update upload");
        if (u.srv == gpu::kNone) u.srv = h.allocateResource();
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements = (UINT)(u.bytes / 4);
        sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        m_device.d3d()->CreateShaderResourceView(u.buffer.Get(), &sd, h.resourceCpu(u.srv));
    }
    std::memcpy(u.mapped, headers.data(), headers.size() * 4);
    std::memcpy(u.mapped + headerBytes, payload.data(), payload.size() * 16);

    // Graphics queue, after every queue's earlier work (the previous frame may still read the scene on another queue);
    // every other queue then waits for the update before this frame's work.
    Queue& graphics = m_device.queue(QueueType::Graphics);
    for (uint32_t q = 0; q < kQueueTypeCount; ++q)
        if (q != (uint32_t)QueueType::Graphics && m_device.queue((QueueType)q).lastSignaled())
            graphics.waitGpu(m_device.queue((QueueType)q), m_device.queue((QueueType)q).lastSignaled());
    ID3D12PipelineState* pso = shaders.compute("Passes/Common/SceneUpdate");
    CommandList cl = m_device.acquireCommandList(QueueType::Graphics);
    ID3D12GraphicsCommandList7* cmd = cl.list.Get();
    ID3D12Resource* targets[3] = { m_instanceBuffer.resource.Get(), m_bonePalette.resource.Get(), m_prevBonePalette.resource.Get() };
    D3D12_BUFFER_BARRIER before[3], after[3];
    for (uint32_t k = 0; k < 3; ++k)
    {
        // Command-list start: earlier submissions on this queue are complete; the other queues were waited for above.
        before[k] = { D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, targets[k], 0, UINT64_MAX };
        after[k] = { D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_SYNC_ALL_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_ACCESS_SHADER_RESOURCE, targets[k], 0, UINT64_MAX };
    }
    D3D12_BARRIER_GROUP group{ D3D12_BARRIER_TYPE_BUFFER, 3 };
    group.pBufferBarriers = before;
    cmd->Barrier(1, &group);
    cmd->SetPipelineState(pso);  // heaps and root signature: bound by acquireCommandList
    const uint32_t k[8] = { u.srv, (uint32_t)headers.size(), m_instanceUav, m_paletteUav, m_prevPaletteUav, 0, 0, 0 };
    cmd->SetComputeRoot32BitConstants(0, 8, k, 0);
    cmd->Dispatch((uint32_t)((headers.size() + 63) / 64), 1, 1);
    group.pBufferBarriers = after;
    cmd->Barrier(1, &group);
    const uint64_t fence = m_device.submit(cl);
    for (uint32_t q = 0; q < kQueueTypeCount; ++q)
        if (q != (uint32_t)QueueType::Graphics) m_device.queue((QueueType)q).waitGpu(graphics, fence);
}

void GpuScene::fill(gpu::FrameConstants& f) const
{
    f.instances = m_instanceBuffer.srv;
    f.meshes = m_meshBuffer.srv;
    f.submeshes = m_submeshBuffer.srv;
    f.vertices = m_vertexBuffer.srv;
    f.indices = m_indexBuffer.srv;
    f.clusters = m_clusterBuffer.srv;
    f.clusterVertexIndices = m_clusterVertexIndexBuffer.srv;
    f.clusterTriangles = m_clusterTriangleBuffer.srv;
    f.lodLevels = m_lodLevelBuffer.srv;
    f.lodLevelClusters = m_lodLevelClusterBuffer.srv;
    f.materials = m_materialBuffer.srv;
    f.materialRemap = m_materialRemapBuffer.srv;
    f.lights = m_lightBuffer.srv;
    f.skinVertices = m_skinBuffer.srv;
    f.bonePalette = m_bonePalette.srv;
    f.prevBonePalette = m_prevBonePalette.srv;
    f.materialModelLut = m_albedoTable.srv;
    f.instanceCount = (uint32_t)m_instances.size();
    f.meshCount = (uint32_t)m_meshes.size();
    f.clusterCount = m_clusterBuffer.count;
    f.lightCount = m_source ? (uint32_t)m_source->lights.size() : 0;
    f.materialCount = m_source ? (uint32_t)m_source->materials.size() : 0;
    f.sceneRevision = m_revision;
}

ID3D12Resource* GpuScene::buffer(const char* name) const
{
    const std::string n = name;
    if (n == "vertices") return m_vertexBuffer.resource.Get();
    if (n == "indices") return m_indexBuffer.resource.Get();
    if (n == "instances") return m_instanceBuffer.resource.Get();
    if (n == "meshes") return m_meshBuffer.resource.Get();
    if (n == "materials") return m_materialBuffer.resource.Get();
    if (n == "clusters") return m_clusterBuffer.resource.Get();
    if (n == "clusterVertexIndices") return m_clusterVertexIndexBuffer.resource.Get();
    if (n == "clusterTriangles") return m_clusterTriangleBuffer.resource.Get();
    if (n == "skin") return m_skinBuffer.resource.Get();
    if (n == "bonePalette") return m_bonePalette.resource.Get();
    if (n == "prevBonePalette") return m_prevBonePalette.resource.Get();
    if (n == "lodLevels") return m_lodLevelBuffer.resource.Get();
    if (n == "lodLevelClusters") return m_lodLevelClusterBuffer.resource.Get();
    for (const auto& [key, b] : m_named)
        if (key == n) return b.resource.Get();
    fail("GpuScene::buffer: unknown buffer '%s'", name);
}

uint32_t GpuScene::srv(const char* name) const
{
    const std::string n = name;
    for (const auto& [key, b] : m_named)
        if (key == n) return b.srv;
    if (n == "clusters") return m_clusterBuffer.srv;
    if (n == "lodLevels") return m_lodLevelBuffer.srv;
    if (n == "lodLevelClusters") return m_lodLevelClusterBuffer.srv;
    if (n == "clusterVertexIndices") return m_clusterVertexIndexBuffer.srv;
    if (n == "clusterTriangles") return m_clusterTriangleBuffer.srv;
    fail("GpuScene::srv: unknown buffer '%s'", name);
}
} // namespace unx::render
