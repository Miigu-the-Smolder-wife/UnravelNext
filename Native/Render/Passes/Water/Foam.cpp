// Water foam F (track W, B7). See include/unx/water/Foam.h and Foam.hlsli.
#include "unx/water/Foam.h"

#include "unx/core/Log.h"

#include <algorithm>
#include <cstring>

namespace unx::water
{
using namespace unx::render;

Foam::Foam(Device& device, ShaderLibrary& shaders, const FoamDesc& desc, uint32_t framesInFlight) : m_device(device), m_shaders(shaders)
{
    setDesc(desc);
    if (!framesInFlight) fail("foam: no frames in flight");
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC1 t{};
    t.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    t.Width = t.Height = kN;
    t.DepthOrArraySize = kMaxLevels;
    t.MipLevels = 1;
    t.Format = DXGI_FORMAT_R16_FLOAT;
    t.SampleDesc.Count = 1;
    t.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &t, D3D12_BARRIER_LAYOUT_COMMON, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_foam)), "foam");
    m_foam->SetName(L"water foam");
    D3D12_RESOURCE_DESC1 b{};
    b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    b.Width = 256;
    b.Height = b.DepthOrArraySize = b.MipLevels = 1;
    b.SampleDesc.Count = 1;
    b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    b.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &b, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_variance)),
          "foam variance");
    m_variance->SetName(L"water foam variances");
    for (uint32_t s = 0; s < framesInFlight; ++s)
    {
        D3D12_HEAP_PROPERTIES up{ D3D12_HEAP_TYPE_UPLOAD };
        b.Flags = D3D12_RESOURCE_FLAG_NONE;
        ComPtr<ID3D12Resource> r;
        check(device.d3d()->CreateCommittedResource3(&up, D3D12_HEAP_FLAG_NONE, &b, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)),
              "foam parameters");
        uint8_t* mapped = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(r->Map(0, &none, (void**)&mapped), "map foam parameters");
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements = 64;
        sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        const uint32_t srv = device.descriptors().allocateResource();
        device.d3d()->CreateShaderResourceView(r.Get(), &sd, device.descriptors().resourceCpu(srv));
        m_upload.push_back(r);
        m_mapped.push_back(mapped);
        m_paramSrv.push_back(srv);
    }
}
Foam::~Foam()
{
    for (auto& u : m_upload)
    {
        u->Unmap(0, nullptr);
        m_device.deferRelease(u);
    }
    for (uint32_t srv : m_paramSrv) m_device.descriptors().freeResource(srv);
    m_device.deferRelease(m_foam);
    m_device.deferRelease(m_variance);
}
void Foam::setDesc(const FoamDesc& desc)
{
    if (!desc.levels || desc.levels > kMaxLevels || !(desc.s0 > 0) || !(desc.tau > 0)) fail("foam: invalid description");
    const bool resolution = desc.s0 != m_desc.s0 || desc.levels != m_desc.levels;
    m_desc = desc;
    if (resolution) std::fill(std::begin(m_placed), std::end(m_placed), false);  // every texel starts again
    m_varianceValid = m_varianceValid && !resolution;
}

FoamOutput Foam::record(RenderGraph& g, uint64_t frame, double seconds, const OceanOutput& fields, const float lengths[3], double cameraX, double cameraZ)
{
    const uint32_t slot = uint32_t(frame % m_upload.size());
    uint8_t* p = m_mapped[slot];
    const float header[4] = { 0, m_desc.s0, m_desc.tau, m_desc.threshold };
    std::memcpy(p, header, 16);
    std::memcpy(p, &m_desc.levels, 4);
    bool update[kMaxLevels] = {};
    for (uint32_t l = 0; l < m_desc.levels; ++l)
    {
        const float spacing = m_desc.s0 * float(1u << (2 * l));
        const int32_t o[2] = { origin(cameraX, spacing), origin(cameraZ, spacing) };
        const bool moved = !m_placed[l] || o[0] != m_origin[l][0] || o[1] != m_origin[l][1];
        update[l] = moved || frame % (uint64_t(1) << l) == 0;
        // A level's first record: every texel entered (a previous window far away).
        const int32_t previous[2] = { m_placed[l] ? m_origin[l][0] : o[0] - 4 * int32_t(kN), m_placed[l] ? m_origin[l][1] : o[1] - 4 * int32_t(kN) };
        const float elapsed = m_placed[l] ? float(seconds - m_updated[l]) : 0.0f;
        if (update[l])
        {
            m_origin[l][0] = o[0];
            m_origin[l][1] = o[1];
            m_updated[l] = seconds;
            m_placed[l] = true;
        }
        const uint32_t updated = update[l] ? 1 : 0;
        uint8_t* row = p + 16 + 32 * l;
        std::memcpy(row, m_origin[l], 8);
        std::memcpy(row + 8, update[l] ? previous : m_origin[l], 8);
        std::memcpy(row + 16, &elapsed, 4);
        std::memcpy(row + 20, &updated, 4);
    }
    FoamOutput out;
    out.paramSrv = m_paramSrv[slot];
    out.foam = g.importTexture(m_foam.Get(), TextureDesc{ "water foam", kN, kN, uint16_t(kMaxLevels), 1, DXGI_FORMAT_R16_FLOAT }, D3D12_BARRIER_LAYOUT_COMMON);
    const BufferRef variance = g.importBuffer(m_variance.Get(), { "water foam variances", 256, 0 });
    out.variance = variance;
    const TextureRef foam = out.foam, displacement = fields.displacement, slopes = fields.slopes;
    const BufferRef h0 = fields.h0;
    const uint32_t paramSrv = out.paramSrv, levels = m_desc.levels;
    const float cascades[3] = { lengths[0], lengths[1], lengths[2] }, s0 = m_desc.s0;
    if (!m_varianceValid || !fields.previousValid)
    {
        ID3D12PipelineState* clear = m_shaders.compute("Passes/Water/ViewGridClear");
        ID3D12PipelineState* reduce = m_shaders.compute("Passes/Water/FoamVariance");
        g.addPass("foam variance clear", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(variance, Use::UavCompute); },
                  [=](PassContext& c) {
                      const uint32_t z[4] = { 0, 0, c.uav(variance), 16 };  // ViewGridClear: the accumulators only
                      c.cmd->SetPipelineState(clear);
                      c.computeConstants(z, 4);
                      c.cmd->Dispatch(1, 1, 1);
                  });
        g.addPass("foam variance", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(h0, Use::SrvCompute); pb.use(variance, Use::UavCompute); },
                  [=](PassContext& c) {
                      uint32_t k[12] = { c.srv(h0), c.uav(variance), levels, 0 };
                      std::memcpy(&k[4], &s0, 4);
                      std::memcpy(&k[8], cascades, 12);
                      c.cmd->SetPipelineState(reduce);
                      c.computeConstants(k, 12);
                      c.cmd->Dispatch((Ocean::kN * Ocean::kN * 3 + 255) / 256, 1, 1);
                  });
        m_varianceValid = true;
    }
    ID3D12PipelineState* kernel = m_shaders.compute("Passes/Water/FoamUpdate");
    for (int32_t l = int32_t(levels) - 1; l >= 0; --l)  // coarse first: entering fine texels read the coarser level
    {
        if (!update[l]) continue;
        const uint32_t level = uint32_t(l);
        g.addPass("foam update", QueueType::Graphics,
                  [&](PassBuilder& pb) { pb.use(foam, Use::UavCompute); pb.use(variance, Use::SrvCompute); pb.use(displacement, Use::SrvCompute); pb.use(slopes, Use::SrvCompute); },
                  [=](PassContext& c) {
                      uint32_t k[12] = { c.uav(foam), paramSrv, c.srv(variance), level, c.srv(displacement), c.srv(slopes), 0, 0 };
                      std::memcpy(&k[8], cascades, 12);
                      c.cmd->SetPipelineState(kernel);
                      c.computeConstants(k, 12);
                      c.cmd->Dispatch(kN / 8, kN / 8, 1);
                  });
    }
    return out;
}
} // namespace unx::water
