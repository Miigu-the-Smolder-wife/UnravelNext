// Water surface height clipmap (track W, B7). See include/unx/water/OceanHeight.h and OceanHeight.hlsli.
#include "unx/water/OceanHeight.h"

#include "unx/core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace unx::water
{
using namespace unx::render;

namespace
{
constexpr uint32_t kSlots = 8;       // parameter upload slots (records in flight)
constexpr uint64_t kSlotBytes = 256; // 48 B of refine parameters, then 4 zero bytes for the fold counter
ComPtr<ID3D12Resource> texture(Device& device, uint32_t levels, uint32_t mips, DXGI_FORMAT format, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = d.Height = OceanHeight::kN;
    d.DepthOrArraySize = UINT16(levels);
    d.MipLevels = UINT16(mips);
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_COMMON, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)), "ocean height texture");
    r->SetName(name);
    return r;
}
ComPtr<ID3D12Resource> buffer(Device& device, uint64_t bytes, D3D12_HEAP_TYPE type, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES heap{ type };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = std::max<uint64_t>(bytes, 256);
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (type == D3D12_HEAP_TYPE_DEFAULT) d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)), "ocean height buffer");
    r->SetName(name);
    return r;
}
} // namespace

OceanHeight::OceanHeight(Device& device, ShaderLibrary& shaders, const OceanHeightDesc& desc) : m_device(device), m_shaders(shaders), m_desc(desc)
{
    if (!desc.levels || desc.levels > 16 || !(desc.s0 > 0) || !(desc.foldRadius >= 0)) fail("ocean height: invalid description");
    // The scatter's upper envelope is a 64-bit atomic max on a bindless raw buffer (required hardware: FEATURES_GAME 1.8).
    D3D12_FEATURE_DATA_D3D12_OPTIONS11 options11{};
    if (FAILED(device.d3d()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS11, &options11, sizeof(options11))) || !options11.AtomicInt64OnDescriptorHeapResourceSupported)
        fail("ocean height: the device lacks 64-bit atomics on descriptor-heap resources (D3D12_OPTIONS11.AtomicInt64OnDescriptorHeapResourceSupported), "
             "which the water surface build requires");
    m_height = texture(device, desc.levels, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, L"ocean height clipmap");
    m_bounds = texture(device, desc.levels, kMips, DXGI_FORMAT_R32G32_FLOAT, L"ocean height bounds");
    m_params = buffer(device, 64, D3D12_HEAP_TYPE_DEFAULT, L"ocean refine parameters");
    m_folds = buffer(device, 8, D3D12_HEAP_TYPE_DEFAULT, L"ocean height counters");
    // Scatter keys, zero at creation (committed memory is zeroed) and cleared again by every resolve.
    m_keys = buffer(device, uint64_t(desc.levels) * kN * kN * 8, D3D12_HEAP_TYPE_DEFAULT, L"ocean height scatter keys");
    m_paramUpload = buffer(device, kSlots * kSlotBytes, D3D12_HEAP_TYPE_UPLOAD, L"ocean refine parameter upload");
    auto& heaps = device.descriptors();
    for (uint32_t m = 0; m < kMips; ++m)
    {
        m_boundsUav[m] = heaps.allocateResource();
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32G32_FLOAT;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
        ud.Texture2DArray.MipSlice = m;
        ud.Texture2DArray.ArraySize = desc.levels;
        device.d3d()->CreateUnorderedAccessView(m_bounds.Get(), nullptr, &ud, heaps.resourceCpu(m_boundsUav[m]));
    }
    // Stable SRVs for the refine parameters (the water layer's pixel shader reads them through `params`).
    m_heightSrv = heaps.allocateResource();
    m_boundsSrv = heaps.allocateResource();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    sd.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    sd.Texture2DArray.MipLevels = 1;
    sd.Texture2DArray.ArraySize = desc.levels;
    device.d3d()->CreateShaderResourceView(m_height.Get(), &sd, heaps.resourceCpu(m_heightSrv));
    sd.Format = DXGI_FORMAT_R32G32_FLOAT;
    sd.Texture2DArray.MipLevels = kMips;
    device.d3d()->CreateShaderResourceView(m_bounds.Get(), &sd, heaps.resourceCpu(m_boundsSrv));
    void* mapped = nullptr;
    D3D12_RANGE none{ 0, 0 };
    check(m_paramUpload->Map(0, &none, &mapped), "map ocean refine parameters");
    std::memset(mapped, 0, kSlots * kSlotBytes);
    m_paramUpload->Unmap(0, nullptr);
}
OceanHeight::~OceanHeight()
{
    for (const ComPtr<ID3D12Resource>& r : { m_height, m_bounds, m_params, m_folds, m_paramUpload, m_keys }) m_device.deferRelease(r);
    auto& heaps = m_device.descriptors();
    for (uint32_t index : m_boundsUav) heaps.freeResource(index);
    heaps.freeResource(m_heightSrv);
    heaps.freeResource(m_boundsSrv);
}

OceanHeightOutput OceanHeight::record(RenderGraph& g, const OceanOutput& fields, const float cascadeLengths[3], const OceanHeightView& view)
{
    m_slot = (m_slot + 1) % kSlots;
    const uint64_t at = uint64_t(m_slot) * kSlotBytes;
    uint8_t* mapped = nullptr;
    D3D12_RANGE none{ 0, 0 };
    check(m_paramUpload->Map(0, &none, (void**)&mapped), "map ocean refine parameters");
    uint32_t p[12] = { m_heightSrv, m_boundsSrv, m_desc.levels, 0 };
    std::memcpy(&p[4], &view.camera[0], 4);
    std::memcpy(&p[5], &view.camera[2], 4);
    std::memcpy(&p[6], &m_desc.s0, 4);
    std::memcpy(&p[7], &m_desc.waterLevel, 4);
    std::memcpy(&p[8], &view.pixelAngle, 4);
    std::memcpy(&p[9], &view.farDistance, 4);
    std::memcpy(mapped + at, p, sizeof(p));
    std::memset(mapped + at + 64, 0, 8);
    m_paramUpload->Unmap(0, nullptr);

    auto import = [&](ID3D12Resource* r, const char* name) { return g.importBuffer(r, { name, r->GetDesc().Width, 0 }); };
    const BufferRef params = import(m_params.Get(), "ocean refine parameters"), folds = import(m_folds.Get(), "ocean height counters");
    const BufferRef keys = import(m_keys.Get(), "ocean height scatter keys");
    const TextureRef height = g.importTexture(m_height.Get(), TextureDesc{ "ocean height clipmap", kN, kN, uint16_t(m_desc.levels), 1, DXGI_FORMAT_R32G32B32A32_FLOAT },
                                              D3D12_BARRIER_LAYOUT_COMMON);
    const TextureRef bounds = g.importTexture(m_bounds.Get(), TextureDesc{ "ocean height bounds", kN, kN, uint16_t(m_desc.levels), uint16_t(kMips), DXGI_FORMAT_R32G32_FLOAT },
                                              D3D12_BARRIER_LAYOUT_COMMON);
    ID3D12Resource* upload = m_paramUpload.Get();
    g.addPass("ocean height parameters", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(params, Use::CopyDst); pb.use(folds, Use::CopyDst); pb.keep(); },
              [=](PassContext& c) {
                  c.cmd->CopyBufferRegion(c.resource(params), 0, upload, at, 48);
                  c.cmd->CopyBufferRegion(c.resource(folds), 0, upload, at + 64, 8);
              });
    const TextureRef displacement = fields.displacement, slopes = fields.slopes;
    const OceanHeightDesc d = m_desc;
    const float camera[2] = { view.camera[0], view.camera[2] };
    const float lengths[3] = { cascadeLengths[0], cascadeLengths[1], cascadeLengths[2] };
    auto constants = [=](PassContext& c, uint32_t (&k)[16]) {
        k[0] = c.srv(displacement);
        k[3] = d.levels;
        std::memcpy(&k[4], camera, 8);
        std::memcpy(&k[6], &d.s0, 4);
        std::memcpy(&k[7], &d.waterLevel, 4);
        std::memcpy(&k[8], lengths, 12);
        std::memcpy(&k[11], &d.foldRadius, 4);
        k[12] = c.uav(folds);
        k[15] = c.srv(slopes);
    };
    ID3D12PipelineState* scatter = m_shaders.compute("Passes/Water/OceanHeightScatter");
    g.addPass("ocean height scatter", QueueType::Graphics,
              [&](PassBuilder& pb) {
                  pb.use(displacement, Use::SrvCompute); pb.use(slopes, Use::SrvCompute); pb.use(keys, Use::UavCompute); pb.use(folds, Use::UavCompute);
              },
              [=](PassContext& c) {
                  c.cmd->SetPipelineState(scatter);
                  for (uint32_t level = 0; level < d.levels; ++level)  // levels write disjoint key ranges: no barrier between them
                  {
                      const float s = d.s0 * float(1u << level);
                      const uint32_t margin = uint32_t(std::ceil(d.foldRadius / s)) + 1, quads = kN + 2 * margin - 1;
                      uint32_t k[16] = {};
                      constants(c, k);
                      k[1] = c.uav(keys);
                      k[2] = margin;
                      k[14] = level;
                      c.computeConstants(k, 16);
                      c.cmd->Dispatch((quads + 7) / 8, (quads + 7) / 8, 1);
                  }
              });
    ID3D12PipelineState* resolve = m_shaders.compute("Passes/Water/OceanHeightResolve");
    g.addPass("ocean height resolve", QueueType::Graphics,
              [&](PassBuilder& pb) {
                  pb.use(displacement, Use::SrvCompute); pb.use(slopes, Use::SrvCompute); pb.use(keys, Use::UavCompute); pb.use(height, Use::UavCompute);
                  pb.use(folds, Use::UavCompute);
              },
              [=](PassContext& c) {
                  uint32_t k[16] = {};
                  constants(c, k);
                  k[1] = c.uav(height);
                  k[2] = c.uav(keys);
                  c.cmd->SetPipelineState(resolve);
                  c.computeConstants(k, 16);
                  c.cmd->Dispatch(kN / 8, kN / 8, d.levels);
              });
    ID3D12PipelineState* bound = m_shaders.compute("Passes/Water/OceanHeightBounds");
    for (uint32_t m = 0; m < kMips; ++m)
    {
        const uint32_t size = kN >> m, dst = m_boundsUav[m], src = m ? m_boundsUav[m - 1] : 0;
        g.addPass("ocean height bounds", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(height, Use::UavCompute); pb.use(bounds, Use::UavCompute); },
                  [=](PassContext& c) {
                      const uint32_t k[16] = { 0, c.uav(height), dst, d.levels, 0, 0, 0, 0, 0, 0, 0, 0, 0, src, m, 0 };
                      c.cmd->SetPipelineState(bound);
                      c.computeConstants(k, 16);
                      c.cmd->Dispatch((size + 7) / 8, (size + 7) / 8, d.levels);
                  });
    }
    return { height, bounds, params, folds };
}
} // namespace unx::water
