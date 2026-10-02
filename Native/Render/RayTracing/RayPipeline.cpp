#include "unx/rt/RayPipeline.h"

#include "unx/core/File.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>

namespace unx::render::rt
{
namespace
{
std::wstring wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
uint64_t alignUp(uint64_t v, uint64_t a) { return (v + a - 1) / a * a; }
} // namespace

RayPipelineDesc standardRayPipeline(std::string library, std::vector<std::string> rayGen)
{
    RayPipelineDesc d;
    d.library = std::move(library);
    d.rayGen = std::move(rayGen);
    d.miss = { "RtMiss", "RtMissVisible" };
    // Hit group 0: triangles (alpha-tested any-hit). Hit group 1: the analytic area lights (RayScene's emitter instance,
    // InstanceContributionToHitGroupIndex 1; RayShaders.hlsli).
    d.hitGroups = { { "RtHitGroup", "RtClosestHit", "RtAnyHit", "" }, { "RtEmitterGroup", "RtEmitterClosestHit", "", "RtEmitterIntersect" } };
    return d;
}

RayPipeline::RayPipeline(Device& device, ShaderLibrary& shaders, const RayPipelineDesc& desc) : m_device(device)
{
    const std::vector<uint8_t> dxil = readBinaryFile(shaders.directory() / (desc.library + ".dxil"));

    // Every export once; hit groups import from them. Names must outlive CreateStateObject.
    std::vector<std::string> unique;
    auto add = [&](const std::string& n) {
        if (!n.empty() && std::find(unique.begin(), unique.end(), n) == unique.end()) unique.push_back(n);
    };
    for (const auto& n : desc.rayGen) add(n);
    for (const auto& n : desc.miss) add(n);
    for (const auto& g : desc.hitGroups)
    {
        add(g.closestHit);
        add(g.anyHit);
        add(g.intersection);
    }
    std::vector<std::wstring> exportNames, groupNames;
    for (const auto& n : unique) exportNames.push_back(wide(n));
    for (const auto& g : desc.hitGroups) groupNames.push_back(wide(g.name));
    std::vector<D3D12_EXPORT_DESC> exports;
    for (const auto& n : exportNames) exports.push_back({ n.c_str(), nullptr, D3D12_EXPORT_FLAG_NONE });
    auto import = [&](const std::string& n) -> const wchar_t* {
        if (n.empty()) return nullptr;
        return exportNames[std::find(unique.begin(), unique.end(), n) - unique.begin()].c_str();
    };
    std::vector<D3D12_HIT_GROUP_DESC> groups;
    for (size_t i = 0; i < desc.hitGroups.size(); ++i)
    {
        D3D12_HIT_GROUP_DESC h{};
        h.HitGroupExport = groupNames[i].c_str();
        h.Type = desc.hitGroups[i].intersection.empty() ? D3D12_HIT_GROUP_TYPE_TRIANGLES : D3D12_HIT_GROUP_TYPE_PROCEDURAL_PRIMITIVE;
        h.ClosestHitShaderImport = import(desc.hitGroups[i].closestHit);
        h.AnyHitShaderImport = import(desc.hitGroups[i].anyHit);
        h.IntersectionShaderImport = import(desc.hitGroups[i].intersection);
        groups.push_back(h);
    }

    D3D12_DXIL_LIBRARY_DESC lib{};
    lib.DXILLibrary = { dxil.data(), dxil.size() };
    lib.NumExports = (UINT)exports.size();
    lib.pExports = exports.data();
    D3D12_RAYTRACING_SHADER_CONFIG shaderConfig{ desc.payloadBytes, desc.attributeBytes };
    D3D12_RAYTRACING_PIPELINE_CONFIG pipelineConfig{ desc.maxRecursion };
    D3D12_GLOBAL_ROOT_SIGNATURE global{ device.rootSignature() };

    std::vector<D3D12_STATE_SUBOBJECT> sub;
    sub.reserve(4 + groups.size());
    sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_DXIL_LIBRARY, &lib });
    for (const auto& g : groups) sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_HIT_GROUP, &g });
    sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_SHADER_CONFIG, &shaderConfig });
    sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_RAYTRACING_PIPELINE_CONFIG, &pipelineConfig });
    sub.push_back({ D3D12_STATE_SUBOBJECT_TYPE_GLOBAL_ROOT_SIGNATURE, &global });
    D3D12_STATE_OBJECT_DESC so{ D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE, (UINT)sub.size(), sub.data() };
    const auto t0 = std::chrono::steady_clock::now();
    check(device.d3d()->CreateStateObject(&so, IID_PPV_ARGS(&m_state)), ("CreateStateObject " + desc.library).c_str());
    m_createMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    m_state->SetName(wide(desc.library).c_str());

    // Shader table: ray generation records (each 64 B aligned), then the miss and hit tables.
    ComPtr<ID3D12StateObjectProperties> props;
    check(m_state.As(&props), "ID3D12StateObjectProperties");
    auto identifier = [&](const std::string& n) {
        const void* id = props->GetShaderIdentifier(wide(n).c_str());
        if (!id) fail("RayPipeline %s: no shader identifier for %s", desc.library.c_str(), n.c_str());
        return id;
    };
    constexpr uint64_t idBytes = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;
    const uint64_t recordStride = alignUp(idBytes, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
    const uint64_t tableAlign = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
    std::vector<uint8_t> table;
    auto place = [&](uint64_t bytes) {
        const uint64_t at = alignUp(table.size(), tableAlign);
        table.resize(at + bytes, 0);
        return at;
    };
    std::vector<uint64_t> rayGenAt;
    for (const auto& n : desc.rayGen)
    {
        const uint64_t at = place(recordStride);
        std::memcpy(&table[at], identifier(n), idBytes);
        rayGenAt.push_back(at);
    }
    const uint64_t missAt = place(recordStride * std::max<size_t>(desc.miss.size(), 1));
    for (size_t i = 0; i < desc.miss.size(); ++i) std::memcpy(&table[missAt + i * recordStride], identifier(desc.miss[i]), idBytes);
    const uint64_t hitAt = place(recordStride * std::max<size_t>(desc.hitGroups.size(), 1));
    for (size_t i = 0; i < desc.hitGroups.size(); ++i) std::memcpy(&table[hitAt + i * recordStride], identifier(desc.hitGroups[i].name), idBytes);

    D3D12_HEAP_PROPERTIES defaultHeap{ D3D12_HEAP_TYPE_DEFAULT }, uploadHeap{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = table.size();
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    check(device.d3d()->CreateCommittedResource3(&defaultHeap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_table)),
          "shader table");
    m_table->SetName((wide(desc.library) + L" shader table").c_str());
    ComPtr<ID3D12Resource> staging;
    check(device.d3d()->CreateCommittedResource3(&uploadHeap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&staging)),
          "shader table staging");
    void* mapped = nullptr;
    D3D12_RANGE none{ 0, 0 };
    check(staging->Map(0, &none, &mapped), "map shader table");
    std::memcpy(mapped, table.data(), table.size());
    staging->Unmap(0, nullptr);
    CommandList cl = device.acquireCommandList(QueueType::Graphics);
    cl.list->CopyBufferRegion(m_table.Get(), 0, staging.Get(), 0, table.size());
    device.queue(QueueType::Graphics).waitCpu(device.submit(cl));

    D3D12_INDIRECT_ARGUMENT_DESC argument{ D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_RAYS };
    D3D12_COMMAND_SIGNATURE_DESC signature{ sizeof(D3D12_DISPATCH_RAYS_DESC), 1, &argument, 0 };
    check(device.d3d()->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&m_indirect)), "DispatchRays command signature");

    const D3D12_GPU_VIRTUAL_ADDRESS base = m_table->GetGPUVirtualAddress();
    for (uint64_t at : rayGenAt) m_rayGen.push_back(base + at);
    m_miss = base + missAt;
    m_missStride = recordStride;
    m_missBytes = recordStride * desc.miss.size();
    m_hit = base + hitAt;
    m_hitStride = recordStride;
    m_hitBytes = recordStride * desc.hitGroups.size();

    // dispatchTemplate(): the descriptions without a size
    d.Width = (uint64_t)std::max<size_t>(m_rayGen.size(), 1) * kDispatchDescStride;
    check(device.d3d()->CreateCommittedResource3(&uploadHeap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_template)),
          "dispatch description template");
    m_template->SetName((wide(desc.library) + L" dispatch descriptions").c_str());
    uint8_t* descriptions = nullptr;
    check(m_template->Map(0, &none, reinterpret_cast<void**>(&descriptions)), "map dispatch description template");
    std::memset(descriptions, 0, (size_t)d.Width);
    for (uint32_t i = 0; i < (uint32_t)m_rayGen.size(); ++i)
    {
        const D3D12_DISPATCH_RAYS_DESC one = dispatchDesc(i, 0, 0, 0);
        std::memcpy(descriptions + (size_t)i * kDispatchDescStride, &one, sizeof one);
    }
    m_template->Unmap(0, nullptr);
}

// Pipelines live until releaseDevice() or process exit; D3D objects keep their device alive, so no deferred release.
RayPipeline::~RayPipeline() = default;

D3D12_DISPATCH_RAYS_DESC RayPipeline::dispatchDesc(uint32_t rayGen, uint32_t width, uint32_t height, uint32_t depth) const
{
    if (rayGen >= m_rayGen.size()) fail("RayPipeline: ray generation %u of %zu", rayGen, m_rayGen.size());
    D3D12_DISPATCH_RAYS_DESC d{};
    d.RayGenerationShaderRecord = { m_rayGen[rayGen], D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES };
    d.MissShaderTable = { m_miss, m_missBytes, m_missStride };
    d.HitGroupTable = { m_hit, m_hitBytes, m_hitStride };
    d.Width = width;
    d.Height = height;
    d.Depth = depth;
    return d;
}

void RayPipeline::dispatch(ID3D12GraphicsCommandList7* cmd, uint32_t rayGen, uint32_t width, uint32_t height, uint32_t depth) const
{
    const D3D12_DISPATCH_RAYS_DESC d = dispatchDesc(rayGen, width, height, depth);
    if (width == 0 || height == 0 || depth == 0) return;
    cmd->SetPipelineState1(m_state.Get());
    cmd->DispatchRays(&d);
}

void RayPipeline::dispatchIndirect(ID3D12GraphicsCommandList7* cmd, ID3D12Resource* arguments, uint64_t offset) const
{
    cmd->SetPipelineState1(m_state.Get());
    cmd->ExecuteIndirect(m_indirect.Get(), 1, arguments, offset, nullptr, 0);
}

namespace
{
std::mutex g_pipelineMutex;
std::map<std::pair<Device*, std::string>, std::unique_ptr<RayPipeline>> g_pipelines;
} // namespace

RayPipeline& RayPipeline::get(Device& device, ShaderLibrary& shaders, const RayPipelineDesc& desc)
{
    std::lock_guard lock(g_pipelineMutex);
    auto& slot = g_pipelines[{ &device, desc.library }];
    if (!slot) slot = std::make_unique<RayPipeline>(device, shaders, desc);
    return *slot;
}

void RayPipeline::releaseDevice(Device& device)
{
    device.waitIdle();
    std::lock_guard lock(g_pipelineMutex);
    std::erase_if(g_pipelines, [&](const auto& e) { return e.first.first == &device; });
}
} // namespace unx::render::rt
