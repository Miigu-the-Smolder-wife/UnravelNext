#include "unx/render/GpuScene.h"
#include "unx/render/FrameContext.h"

#include "unx/render/Shaders.h"
#include "unx/scene/MaterialModel.h"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <cstddef>
#include <cstring>

namespace unx::render
{
const std::vector<uint32_t>& coverageMaskTable()
{
    static const std::vector<uint32_t> table = [] {
        // Subsamples exactly as Coverage.hlsli coverageSample, relative to the pixel centre.
        double sx[32], sy[32];
        for (uint32_t i = 0; i < 32; ++i)
        {
            uint32_t r = 0;
            for (uint32_t b = 0; b < 5; ++b) r |= ((i >> b) & 1u) << (4 - b);
            sx[i] = (i + 0.5) / 32.0 - 0.5;
            sy[i] = r / 32.0 + 1.0 / 64.0 - 0.5;
        }
        const double pi = 3.14159265358979323846, reach = std::sqrt(0.5), step = 2 * reach / kCoverageLutDistances, margin = kCoverageLutMargin;
        auto angle = [](double pa) { return std::atan2(1 - std::fabs(pa), pa); };  // pseudo-angle -> angle in [0, pi]
        std::vector<uint32_t> t(2 * kCoverageLutAngles * kCoverageLutDistances);
        for (uint32_t k = 0; k < kCoverageLutAngles; ++k)
        {
            const double phiA = angle(1 - (double)k / 32), phiB = angle(1 - (double)(k + 1) / 32);  // phiA < phiB
            for (uint32_t j = 0; j < kCoverageLutDistances; ++j)
            {
                const double h0 = -reach + j * step, h1 = h0 + step;
                uint32_t inside = 0, outside = 0;
                for (uint32_t i = 0; i < 32; ++i)
                {
                    // d(phi) = dot(n(phi), x_i - centre) = r cos(phi - theta): extremes at the ends or where phi = theta
                    // (max r) or theta +- pi (min -r) inside [phiA, phiB].
                    const double r = std::hypot(sx[i], sy[i]), theta = std::atan2(sy[i], sx[i]);
                    double lo = std::min(r * std::cos(phiA - theta), r * std::cos(phiB - theta)), hi = std::max(r * std::cos(phiA - theta), r * std::cos(phiB - theta));
                    for (double c : { theta, theta + 2 * pi, theta - 2 * pi })
                        if (c >= phiA && c <= phiB) hi = r;
                    for (double c : { theta + pi, theta - pi, theta + 3 * pi, theta - 3 * pi })
                        if (c >= phiA && c <= phiB) lo = -r;
                    if (lo + h0 >= margin) inside |= 1u << i;
                    if (hi + h1 <= -margin) outside |= 1u << i;
                }
                t[2 * (k * kCoverageLutDistances + j)] = inside;
                t[2 * (k * kCoverageLutDistances + j) + 1] = outside;
            }
        }
        return t;
    }();
    return table;
}

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

GpuScene::GpuScene(Device& device) : m_device(device)
{
    for (uint32_t& u : m_rtUav) u = gpu::kNone;
}

GpuScene::~GpuScene()
{
    for (Buffer* b : { &m_instanceBuffer, &m_meshBuffer, &m_submeshBuffer, &m_vertexBuffer, &m_indexBuffer, &m_materialBuffer, &m_materialRemapBuffer, &m_lightBuffer,
                       &m_skinBuffer, &m_bonePalette, &m_prevBonePalette, &m_albedoTable, &m_specularTable, &m_coverageTable, &m_clusterBuffer, &m_lodLevelBuffer,
                       &m_lodLevelClusterBuffer,
                       &m_clusterVertexIndexBuffer, &m_clusterTriangleBuffer, &m_morphRecords, &m_morphData, &m_patchData })
        release(*b);
    for (auto& [name, b] : m_named) release(b);
    DescriptorHeaps& h = m_device.descriptors();
    for (uint32_t u : { m_instanceUav, m_paletteUav, m_prevPaletteUav, m_morphUav })
        if (u != gpu::kNone) h.freeResource(u);
    for (uint32_t u : m_rtUav)
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

GpuScene::Buffer GpuScene::createStructured(const void* data, size_t stride, size_t count, const wchar_t* name, bool uav, size_t capacity)
{
    // Empty streams still get one zeroed element so every published index is a valid descriptor. A capacity past the
    // content (C2b runtime tails) is zero-filled; 16-byte multiples keep the scatter's elements inside the buffer.
    std::vector<uint8_t> zero;
    const size_t content = count;
    count = std::max(count, capacity);
    if (capacity > content)
        while ((stride * count) % 16) ++count;
    if (count == 0) count = 1;
    if (content < count)
    {
        zero.assign(stride * count, 0);
        if (content) std::memcpy(zero.data(), data, stride * content);
        data = zero.data();
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

    // C4 morph data: one block per mesh with blend shapes or a vertex animation (Deformation.hlsli morphVertex).
    std::vector<uint32_t> morphData;
    m_morphMeshBlock.assign(s.meshes.size(), gpu::kNone);
    for (size_t mi = 0; mi < s.meshes.size(); ++mi)
    {
        const scene::Mesh& m = s.meshes[mi];
        const scene::VertexAnimation& va = m.vertexAnimation;
        const bool blend = !m.blendShapes.empty(), vat = va.framesPerSecond > 0;
        if (!blend && !vat) continue;
        const uint32_t block = (uint32_t)morphData.size(), n = (uint32_t)m.positions.size();
        morphData.resize(block + 8, 0);
        auto bits = [](float f) {
            uint32_t u;
            std::memcpy(&u, &f, 4);
            return u;
        };
        auto push3 = [&](float3 v) { morphData.insert(morphData.end(), { bits(v.x), bits(v.y), bits(v.z) }); };
        if (blend)
        {
            // Vertex-major records in ascending shape order (scene::evaluateMorph's summation order).
            std::vector<std::vector<uint32_t>> perVertex(n);
            for (uint32_t sh = 0; sh < (uint32_t)m.blendShapes.size(); ++sh)
                for (size_t k = 0; k < m.blendShapes[sh].vertices.size(); ++k) perVertex[m.blendShapes[sh].vertices[k]].push_back(sh << 16 | (uint32_t)0);
            const uint32_t rows = (uint32_t)morphData.size();
            morphData.resize(rows + n + 1, 0);
            uint32_t records = 0;
            for (uint32_t v = 0; v < n; ++v)
            {
                morphData[rows + v] = records;
                records += (uint32_t)perVertex[v].size();
            }
            morphData[rows + n] = records;
            const uint32_t recordOffset = (uint32_t)morphData.size();
            for (uint32_t v = 0; v < n; ++v)
                for (uint32_t sh = 0; sh < (uint32_t)m.blendShapes.size(); ++sh)
                {
                    const scene::BlendShape& b = m.blendShapes[sh];
                    const auto it = std::lower_bound(b.vertices.begin(), b.vertices.end(), v);
                    if (it == b.vertices.end() || *it != v) continue;
                    const size_t k = (size_t)(it - b.vertices.begin());
                    morphData.push_back(sh);
                    push3(b.deltaPositions[k]);
                    push3(b.deltaNormals.empty() ? float3{} : b.deltaNormals[k]);
                    morphData.push_back(0);
                }
            const uint32_t h[8] = { 1, n, (uint32_t)m.blendShapes.size(), 0, 0, rows, recordOffset, 0 };
            std::memcpy(&morphData[block], h, sizeof h);
        }
        else
        {
            const uint32_t positions = (uint32_t)morphData.size();
            for (const float3& p : va.positions) push3(p);
            const uint32_t normals = (uint32_t)morphData.size();
            for (const float3& nn : va.normals) push3(nn);
            const uint32_t h[8] = { 2, n, va.frameCount, (va.loop ? 1u : 0u) | (va.normals.empty() ? 0u : 2u), bits(va.framesPerSecond), positions, normals, 0 };
            std::memcpy(&morphData[block], h, sizeof h);
        }
        m_morphMeshBlock[mi] = block;
    }

    // Materials (textures are bound by M's texture system; until then no texture indices are published).
    std::vector<gpu::Material>& materials = m_materials;
    materials.clear();
    for (const scene::Material& m : s.materials) materials.push_back(packMaterial(m));

    // Instances, material remaps, bone palettes (jointToModel * inverseBind per skinned instance).
    std::vector<float4> palette;
    m_instances.clear();
    m_remap.clear();
    for (const scene::Instance& in : s.instances) m_instances.push_back(packInstance(in, &palette));
    m_windInstances = 0;
    for (const gpu::Instance& g : m_instances) m_windInstances += (g.flags & scene::InstanceWind) != 0;
    const std::vector<uint32_t>& remap = m_remap;
    // C4 morph records: per morph instance one record row, then its weights (current, previous).
    m_morphRows.clear();
    for (uint32_t i = 0; i < (uint32_t)s.instances.size(); ++i)
    {
        const scene::Instance& in = s.instances[i];
        const uint32_t block = m_morphMeshBlock[in.mesh];
        if (block == gpu::kNone) continue;
        const uint32_t shapes = (uint32_t)s.meshes[in.mesh].blendShapes.size(), weightRows = (shapes + 3) / 4;
        const uint32_t record = (uint32_t)m_morphRows.size(), weights = record + 1;
        float4 r;
        std::memcpy(&r.x, &block, 4);
        std::memcpy(&r.y, &weights, 4);
        r.z = r.w = in.vertexAnimationTime;
        m_morphRows.push_back(r);
        for (uint32_t copy = 0; copy < 2; ++copy)
            for (uint32_t k = 0; k < weightRows; ++k)
            {
                float4 w{};
                float* c = &w.x;
                for (uint32_t j = 0; j < 4; ++j)
                    if (4 * k + j < in.blendWeights.size()) c[j] = in.blendWeights[4 * k + j];
                m_morphRows.push_back(w);
            }
        m_instances[i].morph = record;
        m_instances[i].morphRadius = morphRadiusOf(i);
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
                       &m_skinBuffer, &m_bonePalette, &m_prevBonePalette, &m_albedoTable, &m_specularTable, &m_coverageTable, &m_morphRecords, &m_morphData })
        release(*b);
    // C2b runtime regions start 16-byte aligned after the content (the scatter writes 16-byte elements).
    auto regionStart = [](size_t count, uint32_t stride) { return (uint32_t)((count * stride + 15) / 16 * 16 / stride); };
    const RuntimeCapacity& rc = m_runtimeCap;
    m_staticInstances = (uint32_t)m_instances.size();
    m_staticMeshes = (uint32_t)m_meshes.size();
    m_rtStride[RtMeshes] = sizeof(gpu::Mesh), m_rtStride[RtSubmeshes] = sizeof(gpu::Submesh), m_rtStride[RtVertices] = sizeof(gpu::Vertex), m_rtStride[RtIndices] = 4;
    m_rtFirst[RtMeshes] = regionStart(m_meshes.size(), 8);  // even: the 8-byte mesh roots of V share its numbering
    m_rtFirst[RtSubmeshes] = (uint32_t)submeshes.size();
    m_rtFirst[RtVertices] = (uint32_t)vertices.size();
    m_rtFirst[RtIndices] = regionStart(indices.size(), 4);
    const uint32_t caps[4] = { rc.meshes, rc.submeshes, rc.vertices, rc.indices };
    for (uint32_t t = RtMeshes; t <= RtIndices; ++t)
    {
        m_rtAlloc[t].free.clear();
        if (caps[t - RtMeshes]) m_rtAlloc[t].free.push_back({ 0, caps[t - RtMeshes] });
        m_rtBytes[t].assign((size_t)caps[t - RtMeshes] * m_rtStride[t], 0);
    }
    m_runtimeMeshes.assign(rc.meshes, {});
    m_runtimeInstanceLive.clear();
    m_rtDirty.clear();
    m_rtReleases.clear();
    const bool pool = rc.meshes > 0;
    m_instanceBuffer = createStructured(m_instances.data(), sizeof(gpu::Instance), m_instances.size(), L"scene instances", true, m_instances.size() + rc.instances + rc.gpuInstances);
    m_meshBuffer = createStructured(m_meshes.data(), sizeof(gpu::Mesh), m_meshes.size(), L"scene meshes", pool, pool ? m_rtFirst[RtMeshes] + rc.meshes : 0);
    m_submeshBuffer = createStructured(submeshes.data(), sizeof(gpu::Submesh), submeshes.size(), L"scene submeshes", pool, pool ? m_rtFirst[RtSubmeshes] + rc.submeshes : 0);
    m_vertexBuffer = createStructured(vertices.data(), sizeof(gpu::Vertex), vertices.size(), L"scene vertices", pool, pool ? m_rtFirst[RtVertices] + rc.vertices : 0);
    m_indexBuffer = createStructured(indices.data(), sizeof(uint32_t), indices.size(), L"scene indices", pool, pool ? m_rtFirst[RtIndices] + rc.indices : 0);
    if (pool)
    {
        rawUav(m_rtUav[RtMeshes], m_meshBuffer, (uint64_t)m_meshBuffer.count * sizeof(gpu::Mesh), true);
        rawUav(m_rtUav[RtSubmeshes], m_submeshBuffer, (uint64_t)m_submeshBuffer.count * sizeof(gpu::Submesh), true);
        rawUav(m_rtUav[RtVertices], m_vertexBuffer, (uint64_t)m_vertexBuffer.count * sizeof(gpu::Vertex), true);
        rawUav(m_rtUav[RtIndices], m_indexBuffer, (uint64_t)m_indexBuffer.count * 4, true);
    }
    if (pool || rc.gpuInstances > 0)
    {
        // C5 terrain patch slots and the GPU-written instance count: zeroed, written through the scatter (target RtPatch).
        m_rtStride[RtPatch] = 16, m_rtFirst[RtPatch] = 0;
        m_rtBytes[RtPatch].assign(((size_t)gpu::kGpuInstanceCountElement + 1) * 16, 0);
        m_patchData = createStructured(m_rtBytes[RtPatch].data(), 16, (size_t)gpu::kGpuInstanceCountElement + 1, L"terrain patch slots", true);
        rawUav(m_rtUav[RtPatch], m_patchData, (uint64_t)m_patchData.count * 16, true);
        m_patchFreeSlots.clear();
        for (uint32_t slot = gpu::kPatchSlots; slot-- > 0;) m_patchFreeSlots.push_back(slot);
    }
    m_patchSlotOf.assign(m_instances.size() + rc.instances, gpu::kNone);
    m_materialBuffer = createStructured(materials.data(), sizeof(gpu::Material), materials.size(), L"scene materials");
    m_materialRemapBuffer = createStructured(remap.data(), sizeof(uint32_t), remap.size(), L"scene material remap");
    m_lightBuffer = createStructured(lights.data(), sizeof(gpu::Light), lights.size(), L"scene lights");
    m_lights = lights;
    m_skinBuffer = createStructured(skin.data(), sizeof(gpu::SkinVertex), skin.size(), L"scene skin");
    m_bonePalette = createStructured(palette.data(), sizeof(float4), palette.size(), L"bone palette", true);
    m_prevBonePalette = createStructured(palette.data(), sizeof(float4), palette.size(), L"bone palette (previous)", true);
    m_morphRecords = createStructured(m_morphRows.data(), sizeof(float4), m_morphRows.size(), L"morph records", true);
    m_morphData = createStructured(morphData.data(), 4, morphData.size(), L"morph data");  // read as raw (4-byte words)
    m_morphFrame.assign(m_instances.size(), UINT64_MAX);
    m_morphedNow.clear();
    m_morphedBefore.clear();
    m_morphRowsDirty.clear();
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
    m_brokenNow.clear();
    m_brokenBefore.clear();
    m_brokenMarked.assign(m_instances.size(), 0);
    m_records.clear();
    m_recordMarked.assign(m_instances.size(), 0);
    rawUav(m_instanceUav, m_instanceBuffer, (uint64_t)m_instanceBuffer.count * sizeof(gpu::Instance), false);
    rawUav(m_paletteUav, m_bonePalette, (uint64_t)m_bonePalette.count * sizeof(float4), false);
    rawUav(m_prevPaletteUav, m_prevBonePalette, (uint64_t)m_prevBonePalette.count * sizeof(float4), false);
    rawUav(m_morphUav, m_morphRecords, (uint64_t)m_morphRecords.count * sizeof(float4), false);
    const std::vector<float>& table = scene::model::directionalAlbedoTable();
    m_albedoTable = createStructured(table.data(), sizeof(float), table.size(), L"material model E table");
    const std::vector<float>& specular = scene::model::specularAlbedoTable();
    m_specularTable = createStructured(specular.data(), 2 * sizeof(float), specular.size() / 2, L"material model (A, B) table");
    const std::vector<uint32_t>& coverage = coverageMaskTable();
    m_coverageTable = createStructured(coverage.data(), 2 * sizeof(uint32_t), coverage.size() / 2, L"coverage mask LUT");
    if (!m_clusterBuffer.resource) setClusters(ClusterData{});
}

gpu::Material GpuScene::packMaterial(const scene::Material& m) const
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
    return g;
}

// An instance record at the current revision, with no motion (previous = current). 'palette' (upload only) receives the
// bone palette rows of a skinned instance.
gpu::Instance GpuScene::packInstance(const scene::Instance& in, std::vector<float4>* palette)
{
    const scene::Scene& s = *m_source;
    gpu::Instance g{};
    rows(in.transform, g.objectToWorld);
    rows(in.transform, g.prevObjectToWorld);
    g.mesh = in.mesh;
    g.flags = in.flags;
    g.materialRemap = gpu::kNone;
    if (!in.materialOverrides.empty())
    {
        g.materialRemap = (uint32_t)m_remap.size();
        m_remap.insert(m_remap.end(), in.materialOverrides.begin(), in.materialOverrides.end());
    }
    g.bonePalette = gpu::kNone;
    if ((in.flags & scene::InstanceSkinned) && in.skeleton != scene::kNone)
    {
        if (!palette) fail("GpuScene: skinned instance of mesh %u after upload (skinned instances are added with GpuScene::upload)", in.mesh);
        const scene::Mesh& mesh = s.meshes[in.mesh];
        const scene::Skeleton& sk = s.skeletons[in.skeleton];
        g.bonePalette = (uint32_t)(palette->size() / 3);
        for (size_t j = 0; j < mesh.skin.inverseBind.size(); ++j)
        {
            float4 r[3];
            rows(compose(sk.jointToModel[j], mesh.skin.inverseBind[j]), r);
            palette->insert(palette->end(), r, r + 3);
        }
    }
    g.transformRevision = m_revision;
    g.deformRevision = m_revision;
    g.windStiffness = in.wind.stiffness;
    g.windPhase = in.wind.phase;
    g.windAnchor = in.wind.anchorHeight;
    g.morph = gpu::kNone;  // set by upload for morph instances (C4)
    g.morphRadius = 0;
    g.patch = gpu::kNone;  // C5
    return g;
}

// Raw UAV of an updatable buffer (SceneUpdate.hlsl). fresh: the buffer was replaced while frames may still read the old
// descriptor, so the view goes to a new descriptor and the old one is freed when the GPU is done.
void GpuScene::rawUav(uint32_t& index, const Buffer& b, uint64_t bytes, bool fresh)
{
    if (bytes < 4 || !b.resource) fail("GpuScene: raw UAV of %llu bytes (buffer %p)", (unsigned long long)bytes, (void*)b.resource.Get());
    DescriptorHeaps& h = m_device.descriptors();
    if (fresh && index != gpu::kNone)
    {
        DescriptorHeaps* heaps = &h;
        const uint32_t old = index;
        m_device.deferCall([heaps, old] { heaps->freeResource(old); });
        index = gpu::kNone;
    }
    if (index == gpu::kNone) index = h.allocateResource();
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R32_TYPELESS;
    ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = (UINT)(bytes / 4);
    ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    m_device.d3d()->CreateUnorderedAccessView(b.resource.Get(), nullptr, &ud, h.resourceCpu(index));
}

void GpuScene::setInstances(std::span<const uint32_t> indices)
{
    if (!m_source) fail("GpuScene::setInstances: no scene uploaded");
    if (indices.empty()) return;
    const scene::Scene& s = *m_source;
    const size_t remapBefore = m_remap.size();
    ++m_revision;  // the records below carry the new revision
    for (uint32_t i : indices)
    {
        if (i > m_instances.size() || i >= s.instances.size())
            fail("GpuScene::setInstances: instance %u (the GPU scene has %zu, the source %zu; append in order)", i, m_instances.size(), s.instances.size());
        const scene::Instance& in = s.instances[i];
        if (in.mesh >= m_meshes.size()) fail("GpuScene::setInstances: instance %u uses mesh %u of %zu (a new mesh needs GpuScene::upload)", i, in.mesh, m_meshes.size());
        if (i < m_instances.size() && m_instances[i].bonePalette != gpu::kNone)
            fail("GpuScene::setInstances: instance %u is skinned (its palette slot is fixed at upload)", i);
        if (in.mesh < m_morphMeshBlock.size() && m_morphMeshBlock[in.mesh] != gpu::kNone)
            fail("GpuScene::setInstances: instance %u uses a mesh with blend shapes or a vertex animation (morph records are fixed at upload)", i);
        for (uint32_t m : in.materialOverrides)
            if (m >= m_materials.size()) fail("GpuScene::setInstances: instance %u overrides with material %u of %zu", i, m, m_materials.size());
        const gpu::Instance g = packInstance(in, nullptr);
        if (i < m_instances.size()) m_windInstances -= (m_instances[i].flags & scene::InstanceWind) != 0;
        m_windInstances += (g.flags & scene::InstanceWind) != 0;
        if (i == m_instances.size())
        {
            m_instances.push_back(g);
            m_transformFrame.push_back(UINT64_MAX);
            m_paletteFrame.push_back(UINT64_MAX);
            m_recordMarked.push_back(0);
            m_brokenMarked.push_back(0);
        }
        else
        {
            m_instances[i] = g;
            m_transformFrame[i] = UINT64_MAX;
        }
    }
    // The whole table from the CPU mirror (pending records of this frame hold the same values). Readers take the new SRV
    // from the frame constants; the scatter gets a new raw UAV descriptor.
    release(m_instanceBuffer);
    if (m_instances.size() > m_staticInstances && m_runtimeCap.instances) fail("GpuScene::setInstances: runtime instances exist (scene edits come before them)");
    m_staticInstances = (uint32_t)m_instances.size();
    m_instanceBuffer = createStructured(m_instances.data(), sizeof(gpu::Instance), m_instances.size(), L"scene instances", true, m_instances.size() + m_runtimeCap.instances + m_runtimeCap.gpuInstances);
    rawUav(m_instanceUav, m_instanceBuffer, (uint64_t)m_instanceBuffer.count * sizeof(gpu::Instance), true);
    if (m_remap.size() != remapBefore)
    {
        release(m_materialRemapBuffer);
        m_materialRemapBuffer = createStructured(m_remap.data(), sizeof(uint32_t), m_remap.size(), L"scene material remap");
    }
}

void GpuScene::setMaterials(std::span<const uint32_t> indices)
{
    if (!m_source) fail("GpuScene::setMaterials: no scene uploaded");
    if (indices.empty()) return;
    const scene::Scene& s = *m_source;
    ++m_revision;
    for (uint32_t i : indices)
    {
        if (i > m_materials.size() || i >= s.materials.size())
            fail("GpuScene::setMaterials: material %u (the GPU scene has %zu, the source %zu; append in order)", i, m_materials.size(), s.materials.size());
        const scene::Material& m = s.materials[i];
        for (uint32_t t : { m.baseColorTexture, m.normalTexture, m.roughMetalTexture, m.emissiveTexture, m.occlusionTexture })
            if (t != scene::kNone && t >= s.textures.size()) fail("GpuScene::setMaterials: material %u uses texture %u of %zu", i, t, s.textures.size());
        gpu::Material g = packMaterial(m);
        if (i < m_materials.size())
        {
            const gpu::Material& old = m_materials[i];  // published textures stay until M republishes
            g.baseColorTexture = old.baseColorTexture;
            g.normalTexture = old.normalTexture;
            g.roughMetalTexture = old.roughMetalTexture;
            g.emissiveTexture = old.emissiveTexture;
            g.occlusionTexture = old.occlusionTexture;
            g.textureClamp = old.textureClamp;
            m_materials[i] = g;
        }
        else m_materials.push_back(g);
    }
    release(m_materialBuffer);
    m_materialBuffer = createStructured(m_materials.data(), sizeof(gpu::Material), m_materials.size(), L"scene materials");
}

void GpuScene::setClusters(ClusterData data)
{
    m_clusterData = std::move(data);
    const ClusterData& c = m_clusterData;
    for (Buffer* b : { &m_clusterBuffer, &m_lodLevelBuffer, &m_lodLevelClusterBuffer, &m_clusterVertexIndexBuffer, &m_clusterTriangleBuffer }) release(*b);
    for (auto& [name, b] : m_named) release(b);
    m_named.clear();
    const RuntimeCapacity& rc = m_runtimeCap;
    const bool pool = rc.meshes > 0;
    auto regionStart = [](size_t count, uint32_t stride) { return (uint32_t)((count * stride + 15) / 16 * 16 / stride); };
    m_rtStride[RtClusters] = sizeof(gpu::Cluster), m_rtStride[RtClusterVertexIndices] = 4, m_rtStride[RtClusterTriangles] = 4;
    m_rtStride[RtNodes] = 32, m_rtStride[RtRoots] = 8, m_rtStride[RtSpheres] = 16, m_rtStride[RtSheets] = 16;
    m_rtFirst[RtClusters] = (uint32_t)c.clusters.size();
    m_rtFirst[RtClusterVertexIndices] = regionStart(c.clusterVertexIndices.size(), 4);
    m_rtFirst[RtClusterTriangles] = regionStart(c.clusterTriangles.size(), 4);
    m_rtFirst[RtSpheres] = m_rtFirst[RtSheets] = m_rtFirst[RtClusters];  // per cluster
    m_rtFirst[RtRoots] = m_rtFirst[RtMeshes];  // roots are indexed by mesh
    const uint32_t caps[7] = { rc.clusters, rc.clusterVertexIndices, rc.clusterTriangles, rc.nodes, rc.meshes, rc.clusters, rc.clusters };
    for (uint32_t t = RtClusters; t <= RtSheets; ++t)
    {
        m_rtAlloc[t].free.clear();
        if (caps[t - RtClusters] && t != RtRoots && t != RtSpheres && t != RtSheets) m_rtAlloc[t].free.push_back({ 0, caps[t - RtClusters] });
        m_rtBytes[t].assign((size_t)caps[t - RtClusters] * m_rtStride[t], 0);
    }
    m_clusterBuffer = createStructured(c.clusters.data(), sizeof(gpu::Cluster), c.clusters.size(), L"scene clusters", pool, pool ? m_rtFirst[RtClusters] + rc.clusters : 0);
    m_lodLevelBuffer = createStructured(c.lodLevels.data(), sizeof(gpu::LodLevel), c.lodLevels.size(), L"scene lod levels");
    m_lodLevelClusterBuffer = createStructured(c.lodLevelClusters.data(), sizeof(uint32_t), c.lodLevelClusters.size(), L"lod level clusters");
    m_clusterVertexIndexBuffer = createStructured(c.clusterVertexIndices.data(), sizeof(uint32_t), c.clusterVertexIndices.size(), L"cluster vertex indices", pool,
                                                  pool ? m_rtFirst[RtClusterVertexIndices] + rc.clusterVertexIndices : 0);
    m_clusterTriangleBuffer = createStructured(c.clusterTriangles.data(), sizeof(uint32_t), c.clusterTriangles.size(), L"cluster triangles", pool,
                                               pool ? m_rtFirst[RtClusterTriangles] + rc.clusterTriangles : 0);
    m_clusterBuffer.count = (uint32_t)c.clusters.size();  // published cluster count: uploaded clusters (runtime ones follow)
    for (const ClusterData::Named& n : c.named)
    {
        if (n.stride == 0 || n.bytes.size() % n.stride != 0)
            fail("GpuScene::setClusters: buffer '%s' size %zu is not a multiple of stride %u", n.name.c_str(), n.bytes.size(), n.stride);
        const std::wstring wide(n.name.begin(), n.name.end());
        const size_t count = n.bytes.size() / n.stride;
        size_t capacity = 0;
        if (pool)
        {
            if (n.name == "clusterNodes") m_rtFirst[RtNodes] = regionStart(count, 32), capacity = m_rtFirst[RtNodes] + rc.nodes;
            else if (n.name == "meshClusterRoots") capacity = m_rtFirst[RtRoots] + rc.meshes;
            else if (n.name == "clusterLodSpheres" || n.name == "clusterSheets") capacity = m_rtFirst[RtSpheres] + rc.clusters;
        }
        m_named.emplace_back(n.name, createStructured(n.bytes.data(), n.stride, count, wide.c_str(), capacity > 0, capacity));
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
        m_meshBuffer = createStructured(m_meshes.data(), sizeof(gpu::Mesh), m_meshes.size(), L"scene meshes", pool, pool ? m_rtFirst[RtMeshes] + rc.meshes : 0);
    }
    if (pool)
    {
        rawUav(m_rtUav[RtMeshes], m_meshBuffer, (uint64_t)m_meshBuffer.count * sizeof(gpu::Mesh), true);
        rawUav(m_rtUav[RtClusters], m_clusterBuffer, ((uint64_t)m_rtFirst[RtClusters] + rc.clusters) * sizeof(gpu::Cluster), true);  // with the runtime tail
        rawUav(m_rtUav[RtClusterVertexIndices], m_clusterVertexIndexBuffer, (uint64_t)m_clusterVertexIndexBuffer.count * 4, true);
        rawUav(m_rtUav[RtClusterTriangles], m_clusterTriangleBuffer, (uint64_t)m_clusterTriangleBuffer.count * 4, true);
        for (auto& [name, b] : m_named)
        {
            const uint32_t t = name == "clusterNodes" ? RtNodes : name == "meshClusterRoots" ? RtRoots : name == "clusterLodSpheres" ? RtSpheres : name == "clusterSheets" ? RtSheets : 0;
            if (t) rawUav(m_rtUav[t], b, (uint64_t)b.count * m_rtStride[t], true);
        }
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
        if (u.flags & kTransformTeleport)
        {
            // prev is still the previous rendered frame's transform here, unless a teleport of this frame already broke it.
            if (!m_brokenMarked[u.instance]) markBreak(u.instance, g.prevObjectToWorld);
            std::memcpy(g.prevObjectToWorld, g.objectToWorld, sizeof g.objectToWorld);
        }
        markRecord(u.instance);
    }
}

void GpuScene::markBreak(uint32_t instance, const float4 (&before)[3])
{
    gpu::Instance& g = m_instances[instance];
    const float4 c = m_meshes[g.mesh].boundsSphere;
    g.breakCentre = { before[0].x * c.x + before[0].y * c.y + before[0].z * c.z + before[0].w,
                      before[1].x * c.x + before[1].y * c.y + before[1].z * c.z + before[1].w,
                      before[2].x * c.x + before[2].y * c.y + before[2].z * c.z + before[2].w };
    g.flags |= gpu::kInstanceMotionBreak;
    if (!m_brokenMarked[instance])
    {
        m_brokenMarked[instance] = 1;
        m_brokenNow.push_back(instance);
    }
    markRecord(instance);
}

void GpuScene::resetMotion()
{
    // Only what changed in this frame differs from its previous state (last frame's changes settle in flushUpdates).
    // Each is flagged kInstanceMotionBreak: its motion is zero now, and a cache keyed by where it was drawn before (VSM
    // pages) re-renders from breakCentre instead (the restore check found shadows of the pre-restore places otherwise).
    for (uint32_t i : m_movedNow)
    {
        if (!m_brokenMarked[i]) markBreak(i, m_instances[i].prevObjectToWorld);  // (a teleport of this frame already did)
        std::memcpy(m_instances[i].prevObjectToWorld, m_instances[i].objectToWorld, sizeof m_instances[i].objectToWorld);
        markRecord(i);
    }
    for (uint32_t i : m_posedNow)  // uploaded with this frame's palettes (flushUpdates: the posed instances' previous rows)
    {
        if (!m_brokenMarked[i]) markBreak(i, m_instances[i].objectToWorld);
        const uint32_t first = m_instances[i].bonePalette * 3, n = paletteJoints(i) * 3;
        std::copy(m_palette.begin() + first, m_palette.begin() + first + n, m_prevPalette.begin() + first);
    }
}

std::span<const float4> GpuScene::palette(uint32_t instance) const
{
    if (instance >= m_instances.size()) fail("GpuScene::palette: instance %u of %zu", instance, m_instances.size());
    const uint32_t n = paletteJoints(instance) * 3;
    if (n == 0) return {};
    return { m_palette.data() + (size_t)m_instances[instance].bonePalette * 3, n };
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

void GpuScene::setInstanceViewModel(uint32_t instance, bool viewModel)
{
    if (instance >= m_instances.size()) fail("GpuScene::setInstanceViewModel: instance %u of %zu", instance, m_instances.size());
    gpu::Instance& g = m_instances[instance];
    const uint32_t flags = viewModel ? g.flags | gpu::kInstanceViewModel : g.flags & ~gpu::kInstanceViewModel;
    if (flags == g.flags) return;
    g.flags = flags;
    ++m_viewModelRevision;  // C3: V keeps view models out of its static chunks
    if (viewModel) ++m_viewModelInstances;
    else --m_viewModelInstances;
    markRecord(instance);
}

float GpuScene::morphRadiusOf(uint32_t instance) const
{
    // Bound of both the current and the previous weights (the frame draws the current pose; phase-1 HiZ tests the
    // previous transform with the same sphere).
    const gpu::Instance& g = m_instances[instance];
    const scene::Mesh& mesh = m_source->meshes[g.mesh];
    if (g.morph == gpu::kNone) return 0;
    const uint32_t shapes = (uint32_t)mesh.blendShapes.size(), weightRows = (shapes + 3) / 4;
    std::vector<float> bound(shapes, 0.0f);
    for (uint32_t k = 0; k < shapes; ++k)
    {
        const float cur = (&m_morphRows[g.morph + 1 + k / 4].x)[k % 4], prev = (&m_morphRows[g.morph + 1 + weightRows + k / 4].x)[k % 4];
        bound[k] = std::max(std::fabs(cur), std::fabs(prev));
    }
    return scene::morphBound(mesh, bound);
}

void GpuScene::writeMorphRows(uint32_t instance, bool settle)
{
    const gpu::Instance& g = m_instances[instance];
    const uint32_t shapes = (uint32_t)m_source->meshes[g.mesh].blendShapes.size(), weightRows = (shapes + 3) / 4;
    if (settle)
    {
        float4& r = m_morphRows[g.morph];
        r.w = r.z;
        for (uint32_t k = 0; k < weightRows; ++k) m_morphRows[g.morph + 1 + weightRows + k] = m_morphRows[g.morph + 1 + k];
    }
    for (uint32_t k = 0; k <= 2 * weightRows; ++k) m_morphRowsDirty.push_back(g.morph + k);
}

void GpuScene::setMorph(uint64_t frameIndex, uint32_t instance, std::span<const float> weights, float vertexAnimationTime)
{
    if (instance >= m_instances.size()) fail("GpuScene::setMorph: instance %u of %zu", instance, m_instances.size());
    gpu::Instance& g = m_instances[instance];
    if (g.morph == gpu::kNone) fail("GpuScene::setMorph: instance %u's mesh has no blend shapes or vertex animation", instance);
    const uint32_t shapes = (uint32_t)m_source->meshes[g.mesh].blendShapes.size(), weightRows = (shapes + 3) / 4;
    if (weights.size() != shapes) fail("GpuScene::setMorph: %zu weights for %u blend shapes", weights.size(), shapes);
    if (m_morphFrame[instance] != frameIndex)
    {
        // The previous frame's values become the previous ones (once per frame).
        float4& r = m_morphRows[g.morph];
        r.w = r.z;
        for (uint32_t k = 0; k < weightRows; ++k) m_morphRows[g.morph + 1 + weightRows + k] = m_morphRows[g.morph + 1 + k];
        m_morphFrame[instance] = frameIndex;
        m_morphedNow.push_back(instance);
        ++g.deformRevision;
    }
    m_morphRows[g.morph].z = vertexAnimationTime;
    for (uint32_t k = 0; k < shapes; ++k) (&m_morphRows[g.morph + 1 + k / 4].x)[k % 4] = weights[k];
    g.morphRadius = morphRadiusOf(instance);
    writeMorphRows(instance, false);
    markRecord(instance);
}

void GpuScene::rebase(float3 shift)
{
    for (float c : { shift.x, shift.y, shift.z })
        if (!(std::fabs(c / kOriginGrid - std::round(c / kOriginGrid)) == 0.0f)) fail("GpuScene::rebase: shift (%g, %g, %g) is not on the 1024 m grid", shift.x, shift.y, shift.z);
    if (shift.x == 0 && shift.y == 0 && shift.z == 0) return;
    for (gpu::Instance& g : m_instances)
    {
        g.objectToWorld[0].w -= shift.x, g.objectToWorld[1].w -= shift.y, g.objectToWorld[2].w -= shift.z;
        g.prevObjectToWorld[0].w -= shift.x, g.prevObjectToWorld[1].w -= shift.y, g.prevObjectToWorld[2].w -= shift.z;
        g.breakCentre = g.breakCentre - shift;
    }
    m_pendingShift = m_pendingShift + shift;
    m_originOffset = m_originOffset + shift;
    // Lights: small; the table is rebuilt from the source with the offset.
    if (m_source)
    {
        std::vector<gpu::Light> lights;
        const DescriptorHeaps& h = m_device.descriptors();
        (void)h;
        for (const scene::Light& l : m_source->lights)
        {
            gpu::Light g{};
            g.position = l.position - m_originOffset;
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
        release(m_lightBuffer);
        m_lightBuffer = createStructured(lights.data(), sizeof(gpu::Light), lights.size(), L"scene lights");
    }
}

// ---- C2b runtime pool ----------------------------------------------------------------------------------------------

uint32_t GpuScene::RangeAllocator::allocate(uint32_t count)
{
    if (count == 0) return 0;
    for (size_t i = 0; i < free.size(); ++i)
        if (free[i].second >= count)
        {
            const uint32_t first = free[i].first;
            free[i].first += count;
            free[i].second -= count;
            if (free[i].second == 0) free.erase(free.begin() + (ptrdiff_t)i);
            return first;
        }
    return gpu::kNone;
}

void GpuScene::RangeAllocator::release(uint32_t first, uint32_t count)
{
    if (count == 0) return;
    auto it = std::lower_bound(free.begin(), free.end(), std::make_pair(first, 0u));
    it = free.insert(it, { first, count });
    const size_t i = (size_t)(it - free.begin());
    if (i + 1 < free.size() && free[i].first + free[i].second == free[i + 1].first)
    {
        free[i].second += free[i + 1].second;
        free.erase(free.begin() + (ptrdiff_t)i + 1);
    }
    if (i > 0 && free[i - 1].first + free[i - 1].second == free[i].first)
    {
        free[i - 1].second += free[i].second;
        free.erase(free.begin() + (ptrdiff_t)i);
    }
}

void GpuScene::reserveRuntime(const RuntimeCapacity& capacity)
{
    if (m_source) fail("GpuScene::reserveRuntime: call before upload");
    m_runtimeCap = capacity;
}

GpuScene::GpuInstanceRange GpuScene::gpuInstanceRange() const
{
    GpuInstanceRange r;
    if (m_runtimeCap.gpuInstances == 0 || !m_patchData.resource) return r;
    r.first = m_staticInstances + m_runtimeCap.instances;
    r.capacity = m_runtimeCap.gpuInstances;
    r.instanceUav = m_instanceUav;
    r.countUav = m_rtUav[RtPatch];
    r.countByteOffset = gpu::kGpuInstanceCountElement * 16;
    r.instanceBuffer = m_instanceBuffer.resource.Get();
    r.countBuffer = m_patchData.resource.Get();
    return r;
}

GpuScene::Buffer* GpuScene::runtimeBuffer(uint32_t target)
{
    auto named = [&](const char* n) -> Buffer* {
        for (auto& [name, b] : m_named)
            if (name == n) return &b;
        return nullptr;
    };
    switch (target)
    {
    case RtMeshes: return &m_meshBuffer;
    case RtSubmeshes: return &m_submeshBuffer;
    case RtVertices: return &m_vertexBuffer;
    case RtIndices: return &m_indexBuffer;
    case RtClusters: return &m_clusterBuffer;
    case RtClusterVertexIndices: return &m_clusterVertexIndexBuffer;
    case RtClusterTriangles: return &m_clusterTriangleBuffer;
    case RtNodes: return named("clusterNodes");
    case RtRoots: return named("meshClusterRoots");
    case RtSpheres: return named("clusterLodSpheres");
    case RtSheets: return named("clusterSheets");
    case RtPatch: return &m_patchData;
    default: return nullptr;
    }
}

// Writes 'count' elements at runtime element 'element' (relative to the target's runtime region) into the mirror and
// marks the 16-byte elements they touch for the scatter.
void GpuScene::writeRuntime(uint32_t target, uint32_t element, const void* data, uint32_t count)
{
    const uint32_t stride = m_rtStride[target];
    const size_t at = (size_t)element * stride, bytes = (size_t)count * stride;
    if (at + bytes > m_rtBytes[target].size()) fail("GpuScene: runtime write past the pool of target %u", target);
    std::memcpy(m_rtBytes[target].data() + at, data, bytes);
    const uint64_t base = (uint64_t)m_rtFirst[target] * stride;  // 16-byte aligned
    for (uint64_t b = (base + at) / 16; b < (base + at + bytes + 15) / 16; ++b) m_rtDirty.push_back({ target, (uint32_t)b });
}

void GpuScene::setPatchRegion(uint32_t instance, const PatchRegion& region)
{
    if (instance >= m_instances.size()) fail("GpuScene::setPatchRegion: instance %u of %zu", instance, m_instances.size());
    if (!m_patchData.resource) fail("GpuScene::setPatchRegion: no runtime pool (reserveRuntime before upload)");
    if (m_patchSlotOf.size() < m_instances.size()) m_patchSlotOf.resize(m_instances.size(), gpu::kNone);
    uint32_t& slot = m_patchSlotOf[instance];
    gpu::Instance& g = m_instances[instance];
    if (region.blocks.empty())
    {
        if (slot == gpu::kNone) return;
        g.patch = gpu::kNone;
        markRecord(instance);
        const uint32_t freed = slot;
        slot = gpu::kNone;
        m_rtReleases.push_back({ m_flushes + m_framesInFlightSeen + 1, [this, freed] { m_patchFreeSlots.push_back(freed); } });
        return;
    }
    if (region.blocksPerSide == 0 || region.blocksPerSide > gpu::kPatchMaxBlocksPerSide)
        fail("GpuScene::setPatchRegion: %u blocks per side (1..%u)", region.blocksPerSide, gpu::kPatchMaxBlocksPerSide);
    if (slot == gpu::kNone)
    {
        if (m_patchFreeSlots.empty()) fail("GpuScene::setPatchRegion: all %u patch slots in use", gpu::kPatchSlots);
        slot = m_patchFreeSlots.back();
        m_patchFreeSlots.pop_back();
    }
    uint32_t words[gpu::kPatchSlotElements * 4] = {};
    float head[8] = { region.originX, region.originZ, region.blockX, region.blockZ, FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX };
    for (uint32_t b : region.blocks)
    {
        if (b >= region.blocksPerSide * region.blocksPerSide) fail("GpuScene::setPatchRegion: block %u of %u^2", b, region.blocksPerSide);
        words[12 + b / 32] |= 1u << (b % 32);
        const float x0 = region.originX + (b % region.blocksPerSide) * region.blockX, x1 = x0 + region.blockX;
        const float z0 = region.originZ + (b / region.blocksPerSide) * region.blockZ, z1 = z0 + region.blockZ;
        head[4] = std::min({ head[4], x0, x1 }), head[5] = std::min({ head[5], z0, z1 });
        head[6] = std::max({ head[6], x0, x1 }), head[7] = std::max({ head[7], z0, z1 });
    }
    std::memcpy(words, head, sizeof head);
    words[8] = region.blocksPerSide;
    words[9] = (uint32_t)region.blocks.size();
    writeRuntime(RtPatch, slot * gpu::kPatchSlotElements, words, gpu::kPatchSlotElements);
    if (g.patch != slot)
    {
        g.patch = slot;
        markRecord(instance);
    }
}

uint32_t GpuScene::addRuntimeMesh(const scene::Mesh& m, const ClusterData& cd)
{
    if (!m_source || m_runtimeCap.meshes == 0) fail("GpuScene::addRuntimeMesh: no runtime pool (reserveRuntime before upload)");
    if (cd.meshes.size() != 1) fail("GpuScene::addRuntimeMesh: cluster data of %zu meshes (one expected)", cd.meshes.size());
    if (!m.skin.joints.empty() || !m.blendShapes.empty() || m.vertexAnimation.framesPerSecond > 0) fail("GpuScene::addRuntimeMesh: runtime meshes are rigid");
    const ClusterData::Named *nodesN = nullptr, *rootsN = nullptr, *spheresN = nullptr, *sheetsN = nullptr;
    for (const auto& n : cd.named)
    {
        if (n.name == "clusterNodes") nodesN = &n;
        if (n.name == "meshClusterRoots") rootsN = &n;
        if (n.name == "clusterLodSpheres") spheresN = &n;
        if (n.name == "clusterSheets") sheetsN = &n;
    }
    if (!nodesN || !rootsN || !spheresN || !sheetsN) fail("GpuScene::addRuntimeMesh: cluster data without the hierarchy buffers");
    struct Node { float4 sphere; float error; uint32_t first, count, leaf; };
    static_assert(sizeof(Node) == 32);
    std::vector<Node> nodes(nodesN->bytes.size() / 32);
    std::memcpy(nodes.data(), nodesN->bytes.data(), nodesN->bytes.size());
    uint32_t roots[2];
    std::memcpy(roots, rootsN->bytes.data(), 8);
    // Depth of the forest (V runs max(scene depth, kRuntimeMaxDepth) node passes).
    uint32_t depth = 0;
    std::vector<std::pair<uint32_t, uint32_t>> stack;
    for (uint32_t k = 0; k < roots[1]; ++k) stack.push_back({ roots[0] + k, 1 });
    while (!stack.empty())
    {
        const auto [n, d] = stack.back();
        stack.pop_back();
        depth = std::max(depth, d);
        if (!nodes[n].leaf)
            for (uint32_t c = 0; c < nodes[n].count; ++c) stack.push_back({ nodes[n].first + c, d + 1 });
    }
    if (depth > kRuntimeMaxDepth) fail("GpuScene::addRuntimeMesh: hierarchy depth %u > %u (build runtime meshes without simplification)", depth, kRuntimeMaxDepth);
    const uint32_t n = (uint32_t)m.positions.size();
    RuntimeMesh r;
    struct Want { uint32_t target, count; uint32_t* first; };
    Want wants[] = { { RtVertices, n, &r.vertices }, { RtIndices, (uint32_t)m.indices.size(), &r.indices }, { RtSubmeshes, (uint32_t)m.submeshes.size(), &r.submeshes },
                     { RtClusters, (uint32_t)cd.clusters.size(), &r.clusters }, { RtClusterVertexIndices, (uint32_t)cd.clusterVertexIndices.size(), &r.cvi },
                     { RtClusterTriangles, (uint32_t)cd.clusterTriangles.size(), &r.ct }, { RtNodes, (uint32_t)nodes.size(), &r.nodes } };
    uint32_t slot = m_rtAlloc[RtMeshes].allocate(1);
    bool ok = slot != gpu::kNone;
    size_t got = 0;
    for (; ok && got < std::size(wants); ++got)
    {
        *wants[got].first = m_rtAlloc[wants[got].target].allocate(wants[got].count);
        ok = *wants[got].first != gpu::kNone;
    }
    if (!ok)  // pool full: give back what was taken
    {
        for (size_t k = 0; k + 1 < got; ++k) m_rtAlloc[wants[k].target].release(*wants[k].first, wants[k].count);
        if (slot != gpu::kNone) m_rtAlloc[RtMeshes].release(slot, 1);
        return gpu::kNone;
    }
    r.live = true;
    r.vertexCount = n, r.indexCount = (uint32_t)m.indices.size(), r.submeshCount = (uint32_t)m.submeshes.size();
    r.clusterCount = (uint32_t)cd.clusters.size(), r.cviCount = (uint32_t)cd.clusterVertexIndices.size(), r.ctCount = (uint32_t)cd.clusterTriangles.size();
    r.nodeCount = (uint32_t)nodes.size();
    const uint32_t gv = m_rtFirst[RtVertices] + r.vertices, gi = m_rtFirst[RtIndices] + r.indices, gs = m_rtFirst[RtSubmeshes] + r.submeshes;
    const uint32_t gc = m_rtFirst[RtClusters] + r.clusters, gcvi = m_rtFirst[RtClusterVertexIndices] + r.cvi, gct = m_rtFirst[RtClusterTriangles] + r.ct;
    const uint32_t gn = m_rtFirst[RtNodes] + r.nodes, meshIndex = m_rtFirst[RtMeshes] + slot;
    // Vertices, indices, submeshes (packed as upload does).
    std::vector<gpu::Vertex> vertices(n);
    for (uint32_t v = 0; v < n; ++v)
    {
        gpu::Vertex& x = vertices[v];
        x.position = m.positions[v];
        x.normalOct = octEncode(m.normals[v]);
        const float3 t = m.tangents.empty() ? anyTangent(m.normals[v]) : float3{ m.tangents[v].x, m.tangents[v].y, m.tangents[v].z };
        x.tangentOct = octEncode(t);
        x.tangentSign = (!m.tangents.empty() && m.tangents[v].w < 0) ? 1u : 0u;
        x.uv = m.uv0.empty() ? float2{} : m.uv0[v];
    }
    writeRuntime(RtVertices, r.vertices, vertices.data(), n);
    writeRuntime(RtIndices, r.indices, m.indices.data(), r.indexCount);
    std::vector<gpu::Submesh> subs;
    for (const scene::Submesh& sm : m.submeshes) subs.push_back({ sm.indexOffset, sm.indexCount, sm.material, 0 });
    writeRuntime(RtSubmeshes, r.submeshes, subs.data(), r.submeshCount);
    // Clusters (pool offsets rebased), their vertex and triangle pools, LOD spheres, sheets; nodes and roots.
    std::vector<gpu::Cluster> clusters = cd.clusters;
    for (gpu::Cluster& c : clusters)
    {
        c.vertexOffset += gcvi;
        c.triangleOffset += gct;
    }
    writeRuntime(RtClusters, r.clusters, clusters.data(), r.clusterCount);
    writeRuntime(RtClusterVertexIndices, r.cvi, cd.clusterVertexIndices.data(), r.cviCount);
    writeRuntime(RtClusterTriangles, r.ct, cd.clusterTriangles.data(), r.ctCount);
    writeRuntime(RtSpheres, r.clusters, spheresN->bytes.data(), r.clusterCount);
    writeRuntime(RtSheets, r.clusters, sheetsN->bytes.data(), r.clusterCount);
    for (Node& x : nodes) x.first += x.leaf ? gc : gn;
    writeRuntime(RtNodes, r.nodes, nodes.data(), r.nodeCount);
    const uint32_t root[2] = { roots[0] + gn, roots[1] };
    writeRuntime(RtRoots, slot, root, 1);
    // The mesh record.
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
    g.boundsMin = lo, g.boundsMax = hi;
    g.vertexOffset = gv, g.vertexCount = n, g.indexOffset = gi, g.triangleCount = r.indexCount / 3;
    g.submeshOffset = gs, g.submeshCount = r.submeshCount, g.clusterOffset = gc, g.clusterCount = r.clusterCount;
    g.lodLevelOffset = gpu::kNone, g.skinOffset = gpu::kNone;
    writeRuntime(RtMeshes, slot, &g, 1);
    if (m_meshes.size() <= meshIndex) m_meshes.resize(meshIndex + 1, gpu::Mesh{});
    m_meshes[meshIndex] = g;
    if (m_morphMeshBlock.size() <= meshIndex) m_morphMeshBlock.resize(meshIndex + 1, gpu::kNone);
    m_runtimeMeshes[slot] = r;
    return meshIndex;
}

void GpuScene::removeRuntimeMesh(uint32_t mesh)
{
    if (mesh < m_rtFirst[RtMeshes] || mesh - m_rtFirst[RtMeshes] >= m_runtimeMeshes.size() || !m_runtimeMeshes[mesh - m_rtFirst[RtMeshes]].live)
        fail("GpuScene::removeRuntimeMesh: %u is not a live runtime mesh", mesh);
    const uint32_t slot = mesh - m_rtFirst[RtMeshes];
    RuntimeMesh r = m_runtimeMeshes[slot];
    m_runtimeMeshes[slot].live = false;
    // Frames in flight may still draw it: its ranges return to the pool framesInFlight + 1 flushes later.
    m_rtReleases.push_back({ m_flushes + m_framesInFlightSeen + 1, [this, r, slot] {
                                m_rtAlloc[RtVertices].release(r.vertices, r.vertexCount);
                                m_rtAlloc[RtIndices].release(r.indices, r.indexCount);
                                m_rtAlloc[RtSubmeshes].release(r.submeshes, r.submeshCount);
                                m_rtAlloc[RtClusters].release(r.clusters, r.clusterCount);
                                m_rtAlloc[RtClusterVertexIndices].release(r.cvi, r.cviCount);
                                m_rtAlloc[RtClusterTriangles].release(r.ct, r.ctCount);
                                m_rtAlloc[RtNodes].release(r.nodes, r.nodeCount);
                                m_rtAlloc[RtMeshes].release(slot, 1);
                            } });
}

uint32_t GpuScene::addRuntimeInstance(const scene::Instance& in)
{
    if (!m_source || m_runtimeCap.instances == 0) fail("GpuScene::addRuntimeInstance: no runtime pool (reserveRuntime before upload)");
    if (in.mesh >= m_meshes.size() || (in.mesh >= m_staticMeshes && !m_runtimeMeshes[in.mesh - m_rtFirst[RtMeshes]].live))
        fail("GpuScene::addRuntimeInstance: mesh %u is neither uploaded nor a live runtime mesh", in.mesh);
    if (in.flags & scene::InstanceSkinned) fail("GpuScene::addRuntimeInstance: runtime instances are rigid");
    if (in.mesh < m_morphMeshBlock.size() && m_morphMeshBlock[in.mesh] != gpu::kNone) fail("GpuScene::addRuntimeInstance: mesh %u morphs (records are fixed at upload)", in.mesh);
    uint32_t index = gpu::kNone;
    for (uint32_t k = 0; k < (uint32_t)m_runtimeInstanceLive.size(); ++k)
        if (m_runtimeInstanceLive[k] == 2)  // freed and safe to reuse
        {
            index = m_staticInstances + k;
            break;
        }
    if (index == gpu::kNone)
    {
        if (m_runtimeInstanceLive.size() >= m_runtimeCap.instances) return gpu::kNone;
        index = m_staticInstances + (uint32_t)m_runtimeInstanceLive.size();
        m_runtimeInstanceLive.push_back(1);
        m_instances.push_back({});
        m_transformFrame.push_back(UINT64_MAX);
        m_paletteFrame.push_back(UINT64_MAX);
        m_recordMarked.push_back(0);
        m_brokenMarked.push_back(0);
        m_morphFrame.push_back(UINT64_MAX);
    }
    m_runtimeInstanceLive[index - m_staticInstances] = 1;
    gpu::Instance g{};
    rows(in.transform, g.objectToWorld);
    rows(in.transform, g.prevObjectToWorld);
    g.mesh = in.mesh;
    g.flags = in.flags;
    g.materialRemap = gpu::kNone;
    g.bonePalette = gpu::kNone;
    g.transformRevision = g.deformRevision = m_revision;
    g.windStiffness = in.wind.stiffness, g.windPhase = in.wind.phase, g.windAnchor = in.wind.anchorHeight;
    g.morph = gpu::kNone;
    g.patch = gpu::kNone;
    m_instances[index] = g;
    m_transformFrame[index] = UINT64_MAX;
    markRecord(index);
    return index;
}

void GpuScene::removeRuntimeInstance(uint32_t instance)
{
    if (instance < m_staticInstances || instance - m_staticInstances >= m_runtimeInstanceLive.size() || m_runtimeInstanceLive[instance - m_staticInstances] != 1)
        fail("GpuScene::removeRuntimeInstance: %u is not a live runtime instance", instance);
    setInstanceVisible(instance, false);
    m_runtimeInstanceLive[instance - m_staticInstances] = 0;  // freed, not yet safe
    m_rtReleases.push_back({ m_flushes + m_framesInFlightSeen + 1, [this, instance] { m_runtimeInstanceLive[instance - m_staticInstances] = 2; } });
}

void GpuScene::flushUpdates(uint64_t frameIndex, uint32_t framesInFlight, ShaderLibrary& shaders)
{
    // C2b: releases whose frames can no longer be in flight.
    ++m_flushes;
    m_framesInFlightSeen = framesInFlight;
    for (size_t k = 0; k < m_rtReleases.size();)
        if (m_rtReleases[k].first <= m_flushes)
        {
            m_rtReleases[k].second();
            m_rtReleases.erase(m_rtReleases.begin() + (ptrdiff_t)k);
        }
        else
            ++k;
    // C4: morphs changed in the previous frame and not in this one settle (previous = current).
    for (uint32_t i : m_morphedBefore)
        if (m_morphFrame[i] != frameIndex)
        {
            writeMorphRows(i, true);
            m_instances[i].morphRadius = morphRadiusOf(i);
            markRecord(i);
        }
    m_morphedBefore.swap(m_morphedNow);
    m_morphedNow.clear();
    // C9: a pending origin shift moves every instance on the GPU first (the records below carry already-shifted values).
    if (m_pendingShift.x != 0 || m_pendingShift.y != 0 || m_pendingShift.z != 0)
    {
        Queue& graphics = m_device.queue(QueueType::Graphics);
        for (uint32_t q = 0; q < kQueueTypeCount; ++q)
            if (q != (uint32_t)QueueType::Graphics && m_device.queue((QueueType)q).lastSignaled())
                graphics.waitGpu(m_device.queue((QueueType)q), m_device.queue((QueueType)q).lastSignaled());
        CommandList cl = m_device.acquireCommandList(QueueType::Graphics);
        ID3D12GraphicsCommandList7* cmd = cl.list.Get();
        D3D12_BUFFER_BARRIER b{ D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                                m_instanceBuffer.resource.Get(), 0, UINT64_MAX };
        D3D12_BARRIER_GROUP group{ D3D12_BARRIER_TYPE_BUFFER, 1 };
        group.pBufferBarriers = &b;
        cmd->Barrier(1, &group);
        cmd->SetPipelineState(shaders.compute("Passes/Common/SceneShift"));
        uint32_t k[8] = { m_instanceUav, (uint32_t)m_instances.size(), 0, 0, 0, (uint32_t)sizeof(gpu::Instance), (uint32_t)offsetof(gpu::Instance, breakCentre), 0 };
        std::memcpy(&k[2], &m_pendingShift, 12);
        cmd->SetComputeRoot32BitConstants(0, 8, k, 0);
        cmd->Dispatch((uint32_t)((m_instances.size() + 63) / 64), 1, 1);
        b = { D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_SYNC_ALL_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_ACCESS_SHADER_RESOURCE,
              m_instanceBuffer.resource.Get(), 0, UINT64_MAX };
        cmd->Barrier(1, &group);
        const uint64_t fence = m_device.submit(cl);
        for (uint32_t q = 0; q < kQueueTypeCount; ++q)
            if (q != (uint32_t)QueueType::Graphics) m_device.queue((QueueType)q).waitGpu(graphics, fence);
        m_pendingShift = {};
    }
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
    // Motion breaks last one frame: last frame's flags clear unless this frame broke the instance again.
    for (uint32_t i : m_brokenBefore)
        if (!m_brokenMarked[i])
        {
            m_instances[i].flags &= ~gpu::kInstanceMotionBreak;
            markRecord(i);
        }
    for (uint32_t i : m_brokenNow) m_brokenMarked[i] = 0;
    m_brokenBefore.swap(m_brokenNow);
    m_brokenNow.clear();

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
    std::sort(m_morphRowsDirty.begin(), m_morphRowsDirty.end());
    m_morphRowsDirty.erase(std::unique(m_morphRowsDirty.begin(), m_morphRowsDirty.end()), m_morphRowsDirty.end());
    for (uint32_t row : m_morphRowsDirty)
    {
        headers.push_back(3u << 28 | row);
        payload.push_back(m_morphRows[row]);
    }
    m_morphRowsDirty.clear();
    // GPU-written instances: this frame's count starts at 0 (the writer runs after this update).
    if (m_runtimeCap.gpuInstances > 0 && m_patchData.resource) m_rtDirty.push_back({ RtPatch, gpu::kGpuInstanceCountElement });
    // C2b runtime pool writes: 16-byte elements from the mirrors.
    std::sort(m_rtDirty.begin(), m_rtDirty.end());
    m_rtDirty.erase(std::unique(m_rtDirty.begin(), m_rtDirty.end()), m_rtDirty.end());
    for (const auto& [target, element] : m_rtDirty)
    {
        const uint64_t base = (uint64_t)m_rtFirst[target] * m_rtStride[target];
        const uint64_t at = (uint64_t)element * 16 - base;
        float4 v{};
        std::memcpy(&v, m_rtBytes[target].data() + at, std::min<uint64_t>(16, m_rtBytes[target].size() - at));
        headers.push_back(target << 28 | element);
        payload.push_back(v);
    }
    m_rtDirty.clear();
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
    // Targets 0-3 always; the C2b runtime pool buffers when the pool exists (SceneUpdate.hlsl target table).
    std::vector<ID3D12Resource*> targets = { m_instanceBuffer.resource.Get(), m_bonePalette.resource.Get(), m_prevBonePalette.resource.Get(), m_morphRecords.resource.Get() };
    uint32_t k[20] = { u.srv, (uint32_t)headers.size(), m_instanceUav, m_paletteUav, m_prevPaletteUav, m_morphUav };
    for (uint32_t t = RtMeshes; t < RtTargetEnd; ++t)
        if (m_runtimeCap.meshes > 0 || (t == RtPatch && m_patchData.resource))
        {
            k[2 + t] = m_rtUav[t];
            if (Buffer* b = runtimeBuffer(t)) targets.push_back(b->resource.Get());
        }
    std::vector<D3D12_BUFFER_BARRIER> before(targets.size()), after(targets.size());
    for (size_t t = 0; t < targets.size(); ++t)
    {
        // Command-list start: earlier submissions on this queue are complete; the other queues were waited for above.
        before[t] = { D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_NO_ACCESS, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, targets[t], 0, UINT64_MAX };
        after[t] = { D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_SYNC_ALL_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_ACCESS_SHADER_RESOURCE, targets[t], 0, UINT64_MAX };
    }
    D3D12_BARRIER_GROUP group{ D3D12_BARRIER_TYPE_BUFFER, (UINT32)targets.size() };
    group.pBufferBarriers = before.data();
    cmd->Barrier(1, &group);
    cmd->SetPipelineState(pso);  // heaps and root signature: bound by acquireCommandList
    cmd->SetComputeRoot32BitConstants(0, 20, k, 0);
    cmd->Dispatch((uint32_t)((headers.size() + 63) / 64), 1, 1);
    group.pBufferBarriers = after.data();
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
    f.specularAlbedoLut = m_specularTable.srv;
    f.coverageMaskLut = m_coverageTable.srv;
    f.giRaysThisFrame = 0;
    f.debugDraw = 0xFFFFFFFFu;  // off; FrameRenderer sets E's debug buffer (FrameRenderer::allocateFrameConstants)
    f.viewModelScale = 1.0f;  // no remap; FrameRenderer sets E's value for the main view
    f.morphRecords = m_morphRecords.srv;
    f.morphData = m_morphData.srv;
    f.patchData = m_patchData.resource ? m_patchData.srv : gpu::kNone;
    f.instanceCount = (uint32_t)m_instances.size();
    f.meshCount = (uint32_t)m_meshes.size();
    f.clusterCount = m_clusterBuffer.count;
    f.lightCount = m_source ? (uint32_t)m_source->lights.size() : 0;
    f.materialCount = (uint32_t)m_materials.size();
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
