// GPU admission test: all keys collide in the old hash table, requests exceed
// the pool, duplicate anchors arrive in different orders, and the scan crosses
// its 65,536-record hierarchy boundary. No renderer or scene fixture required.
#include "unx/core/File.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"

#include <algorithm>
#include <cstring>
#include <numeric>
#include <random>
#include <vector>

using namespace unx;
using namespace unx::render;

namespace
{
constexpr uint32_t pool = 65, requests = 66017, slots = 128, retained = 3;
constexpr uint32_t table = 1024, freeList = table + slots * 16, meta = freeList + pool * 4;
constexpr uint32_t anchor = meta + pool * 16, sh = anchor + pool * 16, texels = sh + pool * 80;
constexpr uint32_t update = texels + pool * 512, selected = update + pool * 4, hitStamp = selected + pool * 4;
constexpr uint32_t hitList = hitStamp + pool * 4, anchorMin = hitList + pool * 8, irr = anchorMin + pool * 8;
constexpr uint32_t slotAnchor = irr + pool * 336, emit = slotAnchor + slots * 8, admission = emit + pool * 256;
constexpr uint32_t total = pool + requests, bytes = admission + 256 + total * 64 + 4096;

uint32_t hashKey(uint64_t x)
{
    x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull; x ^= x >> 33;
    return (uint32_t)x;
}
ComPtr<ID3D12Resource> buffer(Device& device, uint64_t size, D3D12_HEAP_TYPE type, bool uav = false)
{
    D3D12_HEAP_PROPERTIES heap{ type };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; d.Width = size; d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1; d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> out;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED,
                                               nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&out)), "admission test buffer");
    return out;
}
void upload(ID3D12Resource* resource, const void* data, size_t size)
{
    void* mapped = nullptr; D3D12_RANGE none{ 0, 0 };
    check(resource->Map(0, &none, &mapped), "admission upload");
    std::memcpy(mapped, data, size); resource->Unmap(0, nullptr);
}

std::vector<uint32_t> run(Device& device, ShaderLibrary& shaders, const std::vector<uint64_t>& keys, uint32_t seed, uint32_t requestCount)
{
    std::vector<uint32_t> initial(bytes / 4, 0);
    initial[0] = pool; initial[1] = slots; initial[4] = table; initial[5] = freeList; initial[6] = meta; initial[7] = anchor;
    initial[8] = sh; initial[9] = texels; initial[10] = update; initial[12] = pool - retained; initial[15] = 2;
    initial[24] = selected; initial[25] = hitStamp; initial[26] = hitList;
    initial[60] = anchorMin; initial[61] = 1; initial[62] = irr; initial[63] = slotAnchor; initial[192] = admission;
    initial[admission / 4] = requestCount; initial[admission / 4 + 1] = requests;
    for (uint32_t i = 0; i < retained; ++i)
    {
        const uint32_t entry = 2 * i + 1;
        initial[meta / 4 + entry * 4] = (uint32_t)keys[i];
        initial[meta / 4 + entry * 4 + 1] = (uint32_t)(keys[i] >> 32);
        initial[meta / 4 + entry * 4 + 2] = 1;
    }
    std::vector<uint32_t> order(requestCount);
    std::iota(order.begin(), order.end(), 0u);
    std::mt19937 rng(seed); std::shuffle(order.begin(), order.end(), rng);
    for (uint32_t i = 0; i < requestCount; ++i)
    {
        const uint32_t j = order[i], k = retained + j % ((uint32_t)keys.size() - retained);
        const uint32_t a = (admission + 256 + (pool + i) * 32) / 4;
        initial[a] = (uint32_t)keys[k]; initial[a + 1] = (uint32_t)(keys[k] >> 32);
        initial[a + 2] = j; initial[a + 3] = 0; initial[a + 5] = 1;
    }
    auto cache = buffer(device, bytes, D3D12_HEAP_TYPE_DEFAULT, true);
    auto staging = buffer(device, bytes, D3D12_HEAP_TYPE_UPLOAD);
    auto keyBuffer = buffer(device, keys.size() * 8, D3D12_HEAP_TYPE_UPLOAD);
    auto readback = buffer(device, bytes + keys.size() * 4, D3D12_HEAP_TYPE_READBACK);
    upload(staging.Get(), initial.data(), bytes); upload(keyBuffer.Get(), keys.data(), keys.size() * 8);
    RenderGraph graph(device);
    const auto cbuf = graph.importBuffer(cache.Get(), { "admission test", bytes, 0 });
    const auto src = graph.importBuffer(staging.Get(), { "admission input", bytes, 0 });
    const auto keyRef = graph.importBuffer(keyBuffer.Get(), { "admission keys", keys.size() * 8, 8 });
    const auto rb = graph.importBuffer(readback.Get(), { "admission readback", bytes + keys.size() * 4, 0 });
    const auto args = graph.createBuffer({ "admission args", 16, 0 });
    const auto found = graph.createBuffer({ "admission found", keys.size() * 4, 4 });
    graph.addPass("test.upload", QueueType::Graphics, [&](PassBuilder& b) { b.use(src, Use::CopySrc); b.use(cbuf, Use::CopyDst); },
                  [src, cbuf](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(cbuf), 0, c.resource(src), 0, bytes); });
    graph.addPass("test.prepare", QueueType::Compute, [&](PassBuilder& b) { b.use(cbuf, Use::UavCompute); b.use(args, Use::UavCompute); },
                  [&, cbuf, args](PassContext& c) {
                      const uint32_t k[4] = { c.uav(cbuf), c.uav(args), requests, 0 };
                      c.cmd->SetPipelineState(shaders.compute("Passes/GI/GiAdmissionPrepare")); c.computeConstants(k, 4);
                      c.cmd->Dispatch((pool + 255) / 256, 1, 1);
                  });
    auto dispatch = [&](const char* kernel, uint32_t x, uint32_t y, uint32_t groups) {
        graph.addPass(kernel, QueueType::Compute, [&](PassBuilder& b) { b.use(cbuf, Use::UavCompute); },
                      [&, cbuf, kernel, x, y, groups](PassContext& c) {
                          const uint32_t k[4] = { c.uav(cbuf), requests, x, y };
                          c.cmd->SetPipelineState(shaders.compute(kernel)); c.computeConstants(k, 4); c.cmd->Dispatch(groups, 1, 1);
                      });
    };
    uint32_t parity = 0;
    for (uint32_t span = 1; span < total; span *= 2)
    {
        dispatch("Passes/GI/GiAdmissionMerge", parity, span, (pool + requestCount + 255) / 256); parity = 1 - parity;
    }
    dispatch("Passes/GI/GiAdmissionMark", parity, 0, (pool + requestCount + 255) / 256);
    uint32_t level = 0;
    for (uint32_t count = (total + 255) / 256;; count = (count + 255) / 256, ++level)
    {
        dispatch("Passes/GI/GiAdmissionScan", level, 0, (count + 255) / 256);
        if (count <= 256) break;
    }
    dispatch("Passes/GI/GiAdmissionPublish", parity, 0, (pool + requestCount + 255) / 256);
    graph.addPass("test.find", QueueType::Compute,
                  [&](PassBuilder& b) { b.use(cbuf, Use::UavCompute); b.use(keyRef, Use::SrvCompute); b.use(found, Use::UavCompute); },
                  [&, cbuf, keyRef, found](PassContext& c) {
                      const uint32_t k[4] = { c.uav(cbuf), c.srv(keyRef), c.uav(found), (uint32_t)keys.size() };
                      c.cmd->SetPipelineState(shaders.compute("Passes/GI/Tests/AdmissionFind")); c.computeConstants(k, 4);
                      c.cmd->Dispatch(((uint32_t)keys.size() + 63) / 64, 1, 1);
                  });
    graph.addPass("test.read", QueueType::Graphics,
                  [&](PassBuilder& b) { b.use(cbuf, Use::CopySrc); b.use(found, Use::CopySrc); b.use(rb, Use::CopyDst); b.keep(); },
                  [&, cbuf, found, rb](PassContext& c) {
                      c.cmd->CopyBufferRegion(c.resource(rb), 0, c.resource(cbuf), 0, bytes);
                      c.cmd->CopyBufferRegion(c.resource(rb), bytes, c.resource(found), 0, keys.size() * 4);
                  });
    graph.execute(nullptr); device.waitIdle();
    std::vector<uint32_t> output(bytes / 4 + keys.size());
    void* mapped = nullptr; D3D12_RANGE all{ 0, output.size() * 4 };
    check(readback->Map(0, &all, &mapped), "admission readback");
    std::memcpy(output.data(), mapped, output.size() * 4); D3D12_RANGE none{ 0, 0 }; readback->Unmap(0, &none);
    return output;
}
}

int main(int argc, char** argv)
{
    try
    {
        DeviceOptions options;
        for (int i = 1; i < argc; ++i)
            if (std::string(argv[i]) == "--validate") options.debugLayer = options.gpuValidation = true;
            else fail("unknown argument %s", argv[i]);
        Device device(options); ShaderLibrary shaders(device, executableDirectory() / "shaders");
        std::vector<uint64_t> keys;
        for (uint64_t k = 0; keys.size() < 101; ++k)
        {
            const uint64_t key = (1ull << 63) | (k << 8);
            if ((hashKey(key) & (slots - 1)) == 0) keys.push_back(key);
        }
        for (uint32_t count : { 0u, 1u, 255u, requests })
        {
            const auto reference = run(device, shaders, keys, 1, count);
            for (uint32_t seed : { 7u, 139u })
            {
                const auto result = run(device, shaders, keys, seed, count);
                if (!std::equal(reference.begin() + table / 4, reference.begin() + (table + pool * 16) / 4, result.begin() + table / 4) ||
                    !std::equal(reference.begin() + anchorMin / 4, reference.begin() + (anchorMin + pool * 8) / 4, result.begin() + anchorMin / 4))
                    fail("admission depends on arrival order (%u requests, seed %u)", count, seed);
            }
            const uint32_t admitted = std::min(pool, retained + std::min(count, (uint32_t)keys.size() - retained));
            if (reference[12] != pool - admitted) fail("wrong free count");
            uint32_t nextFree = 0;
            for (uint32_t k = 0; k < keys.size(); ++k)
            {
                while (nextFree == 1 || nextFree == 3 || nextFree == 5) ++nextFree;
                const uint32_t expected = k < retained ? 2 * k + 1 : k < admitted ? nextFree++ : 0xFFFFFFFFu;
                if (reference[bytes / 4 + k] != expected) fail("wrong lookup/admission for key %u", k);
                if (k >= retained && k < admitted && reference[anchorMin / 4 + expected * 2] != k - retained)
                    fail("wrong minimum anchor for key %u", k);
            }
        }
        if (device.drainDebugMessages() != 0) fail("GPU validation errors");
        logf("RESULT PASS: exhausted pool, colliding keys, anchor minima, permutation invariance, scan hierarchy\n");
        return 0;
    }
    catch (const std::exception& e) { logf("error: %s\n", e.what()); return 1; }
}
