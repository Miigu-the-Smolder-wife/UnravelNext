#include "unx/render/GpuScene.h"

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
                       &m_skinBuffer, &m_bonePalette, &m_prevBonePalette, &m_albedoTable, &m_clusterBuffer, &m_lodLevelBuffer, &m_clusterVertexIndexBuffer,
                       &m_clusterTriangleBuffer })
        release(*b);
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

GpuScene::Buffer GpuScene::createStructured(const void* data, size_t stride, size_t count, const wchar_t* name)
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
    check(m_device.d3d()->CreateCommittedResource3(&defaultHeap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&b.resource)),
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
    std::vector<gpu::Material> materials;
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
        g.baseColorTexture = g.normalTexture = g.roughMetalTexture = g.emissiveTexture = g.occlusionTexture = gpu::kNone;
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
    m_instanceBuffer = createStructured(m_instances.data(), sizeof(gpu::Instance), m_instances.size(), L"scene instances");
    m_meshBuffer = createStructured(m_meshes.data(), sizeof(gpu::Mesh), m_meshes.size(), L"scene meshes");
    m_submeshBuffer = createStructured(submeshes.data(), sizeof(gpu::Submesh), submeshes.size(), L"scene submeshes");
    m_vertexBuffer = createStructured(vertices.data(), sizeof(gpu::Vertex), vertices.size(), L"scene vertices");
    m_indexBuffer = createStructured(indices.data(), sizeof(uint32_t), indices.size(), L"scene indices");
    m_materialBuffer = createStructured(materials.data(), sizeof(gpu::Material), materials.size(), L"scene materials");
    m_materialRemapBuffer = createStructured(remap.data(), sizeof(uint32_t), remap.size(), L"scene material remap");
    m_lightBuffer = createStructured(lights.data(), sizeof(gpu::Light), lights.size(), L"scene lights");
    m_skinBuffer = createStructured(skin.data(), sizeof(gpu::SkinVertex), skin.size(), L"scene skin");
    m_bonePalette = createStructured(palette.data(), sizeof(float4), palette.size(), L"bone palette");
    m_prevBonePalette = createStructured(palette.data(), sizeof(float4), palette.size(), L"bone palette (previous)");
    const std::vector<float>& table = scene::model::directionalAlbedoTable();
    m_albedoTable = createStructured(table.data(), sizeof(float), table.size(), L"material model E table");
    if (!m_clusterBuffer.resource) setClusters(ClusterData{});
}

void GpuScene::setClusters(const ClusterData& c)
{
    for (Buffer* b : { &m_clusterBuffer, &m_lodLevelBuffer, &m_clusterVertexIndexBuffer, &m_clusterTriangleBuffer }) release(*b);
    m_clusterBuffer = createStructured(c.clusters.data(), sizeof(gpu::Cluster), c.clusters.size(), L"scene clusters");
    m_lodLevelBuffer = createStructured(c.lodLevels.data(), sizeof(gpu::LodLevel), c.lodLevels.size(), L"scene lod levels");
    m_clusterVertexIndexBuffer = createStructured(c.clusterVertexIndices.data(), sizeof(uint32_t), c.clusterVertexIndices.size(), L"cluster vertex indices");
    m_clusterTriangleBuffer = createStructured(c.clusterTriangles.data(), sizeof(uint32_t), c.clusterTriangles.size(), L"cluster triangles");
    m_clusterBuffer.count = (uint32_t)c.clusters.size();
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
    if (n == "clusters") return m_clusterBuffer.resource.Get();
    if (n == "clusterVertexIndices") return m_clusterVertexIndexBuffer.resource.Get();
    if (n == "clusterTriangles") return m_clusterTriangleBuffer.resource.Get();
    if (n == "skin") return m_skinBuffer.resource.Get();
    if (n == "bonePalette") return m_bonePalette.resource.Get();
    fail("GpuScene::buffer: unknown buffer '%s'", name);
}
} // namespace unx::render
