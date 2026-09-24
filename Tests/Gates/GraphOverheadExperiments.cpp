// Cost decomposition for the empty-frame gate: what one pass, one barrier kind, one timestamp pair, one command-list
// boundary and one cross-queue round trip cost on this GPU, measured with raw command lists (no render graph).
// Every number is a GPU timestamp span, median of 101 submissions after a 1.5 s warm-up.
#include "EmptyFrameScene.h"

#include "unx/render/GpuLock.h"
#include "unx/render/Harness.h"

#include <winternl.h>  // NTSTATUS for d3dkmthk.h
#include <d3dkmthk.h>

#include <algorithm>
#include <chrono>
#include <functional>
#include <vector>

#pragma comment(lib, "gdi32.lib")

namespace unx::test
{
namespace
{
struct Timer
{
    Device& device;
    ComPtr<ID3D12QueryHeap> heap;
    ComPtr<ID3D12Resource> readback;
    uint64_t* mapped = nullptr;

    explicit Timer(Device& d) : device(d)
    {
        D3D12_QUERY_HEAP_DESC qd{ D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 64, 0 };
        check(d.d3d()->CreateQueryHeap(&qd, IID_PPV_ARGS(&heap)), "query heap");
        D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC1 rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = 64 * 8;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(d.d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&readback)), "readback");
        D3D12_RANGE none{ 0, 0 };
        check(readback->Map(0, &none, reinterpret_cast<void**>(&mapped)), "map");
    }

    // L recorded lists between a start-timestamp list and an end-timestamp list. mode 0: one ExecuteCommandLists call,
    // 1: one call per list, 2: one call per list followed by a Signal.
    double measureLists(int L, int mode, const std::function<void(ID3D12GraphicsCommandList7*, int)>& record)
    {
        const double freq = (double)device.queue(QueueType::Graphics).timestampFrequency();
        Queue& g = device.queue(QueueType::Graphics);
        auto once = [&]() -> double {
            std::vector<CommandList> cls(L + 2);
            for (auto& cl : cls) cl = device.acquireCommandList(QueueType::Graphics);
            cls[0].list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            for (int i = 0; i < L; ++i) record(cls[i + 1].list.Get(), i);
            cls[L + 1].list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            cls[L + 1].list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, readback.Get(), 0);
            std::vector<ID3D12CommandList*> raw;
            for (auto& cl : cls)
            {
                check(cl.list->Close(), "close");
                raw.push_back(cl.list.Get());
            }
            if (mode == 0) g.get()->ExecuteCommandLists((UINT)raw.size(), raw.data());
            else
                for (ID3D12CommandList* l : raw)
                {
                    g.get()->ExecuteCommandLists(1, &l);
                    if (mode == 2) g.signal();
                }
            uint64_t v = g.signal();
            g.waitCpu(v);
            for (auto& cl : cls)
            {
                cl.retireValue = v;
                device.recycle(std::move(cl));
            }
            return (double)(mapped[1] - mapped[0]) / freq * 1e6;
        };
        auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 1.5) once();
        std::vector<double> v;
        for (int i = 0; i < 101; ++i) v.push_back(once());
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    }

    // body records between two graphics-queue timestamps; it may submit extra lists through 'submitSplit'.
    // Returns the median GPU span in microseconds.
    double measure(const std::function<void(ID3D12GraphicsCommandList7*, const std::function<ID3D12GraphicsCommandList7*()>&)>& body)
    {
        const double freq = (double)device.queue(QueueType::Graphics).timestampFrequency();
        auto once = [&]() -> double {
            std::vector<CommandList> pending;
            CommandList cl = device.acquireCommandList(QueueType::Graphics);
            cl.list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0);
            ID3D12GraphicsCommandList7* current = cl.list.Get();
            // Splits the graphics recording: submits what was recorded so far and continues in a new list.
            auto split = [&]() -> ID3D12GraphicsCommandList7* {
                device.submit(cl);
                cl = device.acquireCommandList(QueueType::Graphics);
                current = cl.list.Get();
                return current;
            };
            body(current, split);
            cl.list->EndQuery(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 1);
            cl.list->ResolveQueryData(heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2, readback.Get(), 0);
            uint64_t v = device.submit(cl);
            device.queue(QueueType::Graphics).waitCpu(v);
            device.waitIdle();
            return (double)(mapped[1] - mapped[0]) / freq * 1e6;
        };
        auto t0 = std::chrono::steady_clock::now();
        while (std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() < 1.5) once();
        std::vector<double> v;
        for (int i = 0; i < 101; ++i) v.push_back(once());
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    }
};

ComPtr<ID3D12Resource> texture4k(Device& device, uint32_t& srv, uint32_t& uav)
{
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = 3840;
    d.Height = 2160;
    d.DepthOrArraySize = d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R32_FLOAT;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;
    check(device.d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)), "texture");
    DescriptorHeaps& h = device.descriptors();
    srv = h.allocateResource();
    uav = h.allocateResource();
    device.d3d()->CreateShaderResourceView(r.Get(), nullptr, h.resourceCpu(srv));
    device.d3d()->CreateUnorderedAccessView(r.Get(), nullptr, nullptr, h.resourceCpu(uav));
    return r;
}

void logSchedulingMode(Device& device)
{
    LUID luid = device.d3d()->GetAdapterLuid();
    D3DKMT_OPENADAPTERFROMLUID open{};
    open.AdapterLuid = luid;
    if (D3DKMTOpenAdapterFromLuid(&open) != 0) return;
    D3DKMT_WDDM_2_7_CAPS caps{};
    D3DKMT_QUERYADAPTERINFO q{};
    q.hAdapter = open.hAdapter;
    q.Type = KMTQAITYPE_WDDM_2_7_CAPS;
    q.pPrivateDriverData = &caps;
    q.PrivateDriverDataSize = sizeof caps;
    if (D3DKMTQueryAdapterInfo(&q) == 0)
        logf("hardware-accelerated GPU scheduling: supported %u, enabled %u\n", caps.HwSchSupported, caps.HwSchEnabled);
    D3DKMT_CLOSEADAPTER close{ open.hAdapter };
    D3DKMTCloseAdapter(&close);
}
} // namespace

int runOverheadExperiments(Device& device, ShaderLibrary& shaders)
{
    requireGpuLock("graph overhead experiments");
    logSchedulingMode(device);
    Timer timer(device);
    ID3D12PipelineState* pso = shaders.compute("Passes/Test/Touch");
    uint32_t srv = 0, uav = 0;
    ComPtr<ID3D12Resource> tex = texture4k(device, srv, uav);
    const int N = 120;
    uint32_t k[16] = {};
    k[1] = 1;  // one output
    k[12] = uav;
    k[14] = 0;  // float texture

    auto pass = [&](ID3D12GraphicsCommandList7* l) {
        l->SetPipelineState(pso);
        l->SetComputeRoot32BitConstants(0, 16, k, 0);
        l->Dispatch(1, 1, 1);
    };
    auto global = [](ID3D12GraphicsCommandList7* l) {
        D3D12_GLOBAL_BARRIER g{ D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS };
        D3D12_BARRIER_GROUP grp{ D3D12_BARRIER_TYPE_GLOBAL, 1 };
        grp.pGlobalBarriers = &g;
        l->Barrier(1, &grp);
    };
    auto texBarrier = [&](ID3D12GraphicsCommandList7* l, D3D12_BARRIER_LAYOUT from, D3D12_BARRIER_LAYOUT to, D3D12_BARRIER_ACCESS aFrom, D3D12_BARRIER_ACCESS aTo) {
        D3D12_TEXTURE_BARRIER t{ D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_SYNC_COMPUTE_SHADING, aFrom, aTo, from, to, tex.Get(), { 0xffffffffu, 0, 0, 0, 0, 0 }, D3D12_TEXTURE_BARRIER_FLAG_NONE };
        D3D12_BARRIER_GROUP grp{ D3D12_BARRIER_TYPE_TEXTURE, 1 };
        grp.pTextureBarriers = &t;
        l->Barrier(1, &grp);
    };
    std::vector<std::pair<const char*, double>> results;
    auto run = [&](const char* name, const std::function<void(ID3D12GraphicsCommandList7*, const std::function<ID3D12GraphicsCommandList7*()>&)>& body) {
        double us = timer.measure(body);
        results.push_back({ name, us });
        logf("  %-78s %8.2f us total, %6.3f us per pass\n", name, us, us / N);
    };

    logf("\n== render-graph cost decomposition (%d passes, one 64-thread group each)\n", N);
    run("Dispatch(1), no barrier", [&](auto* l, auto&) { for (int i = 0; i < N; ++i) pass(l); });
    run("Dispatch(1) + legacy UAV barrier", [&](auto* l, auto&) {
        for (int i = 0; i < N; ++i)
        {
            pass(l);
            D3D12_RESOURCE_BARRIER b{};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            l->ResourceBarrier(1, &b);
        }
    });
    run("Dispatch(1) + enhanced global barrier (compute UAV -> UAV)", [&](auto* l, auto&) { for (int i = 0; i < N; ++i) { pass(l); global(l); } });
    run("Dispatch(1) + enhanced texture barrier, 4K R32F, UAV -> UAV", [&](auto* l, auto&) {
        for (int i = 0; i < N; ++i) { pass(l); texBarrier(l, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS); }
    });
    run("Dispatch(1) + 4K layout change UAV <-> SHADER_RESOURCE every pass", [&](auto* l, auto&) {
        for (int i = 0; i < N; ++i)
        {
            pass(l);
            texBarrier(l, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_ACCESS_SHADER_RESOURCE);
            texBarrier(l, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, D3D12_BARRIER_ACCESS_SHADER_RESOURCE, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS);
        }
    });
    run("Dispatch(1) + global barrier + timestamp pair", [&](auto* l, auto&) {
        for (int i = 0; i < N; ++i)
        {
            l->EndQuery(timer.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 + (i % 30));
            pass(l);
            l->EndQuery(timer.heap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 32 + (i % 30));
            global(l);
        }
    });
    run("Dispatch(1) + global barrier, split into 11 command lists (Signal between)", [&](auto* l, auto& split) {
        auto* cur = l;
        for (int i = 0; i < N; ++i)
        {
            pass(cur);
            global(cur);
            if (i % 11 == 10) cur = split();
        }
    });
    // The same 120 passes recorded into L command lists, bracketed by a first list (start timestamp) and a last list
    // (end timestamp): submitted in one ExecuteCommandLists call, in L+2 calls, or in L+2 calls with a Signal after each.
    for (int L : { 1, 11 })
        for (int mode = 0; mode < 3; ++mode)
        {
            if (L == 1 && mode != 0) continue;
            const char* how = mode == 0 ? "one ExecuteCommandLists call" : mode == 1 ? "separate calls" : "separate calls + Signal each";
            double us = timer.measureLists(L, mode, [&](ID3D12GraphicsCommandList7* l, int list) {
                const int per = (N + L - 1) / L;
                for (int i = list * per; i < std::min(N, (list + 1) * per); ++i)
                {
                    pass(l);
                    global(l);
                }
            });
            logf("  Dispatch(1) + global barrier in %2d lists (+2 timestamp lists), %-30s %8.2f us\n", L, how, us);
        }
    // Cross-queue round trips: graphics -> compute (one dispatch) -> graphics, R times.
    for (int R : { 1, 3 })
    {
        const std::string name = std::to_string(R) + " cross-queue round trip(s): graphics signal -> compute wait + Dispatch(1) + signal -> graphics wait";
        double us = timer.measure([&](ID3D12GraphicsCommandList7*, auto& split) {
            for (int r = 0; r < R; ++r)
            {
                split();  // submit graphics so far
                Queue& g = device.queue(QueueType::Graphics);
                Queue& c = device.queue(QueueType::Compute);
                uint64_t gv = g.signal();
                c.waitGpu(g, gv);
                CommandList cl = device.acquireCommandList(QueueType::Compute);
                cl.list->SetPipelineState(pso);
                cl.list->SetComputeRoot32BitConstants(0, 16, k, 0);
                cl.list->Dispatch(1, 1, 1);
                uint64_t cv = device.submit(cl);
                g.waitGpu(c, cv);
            }
        });
        logf("  %-78s %8.2f us total, %6.2f us per round trip\n", name.c_str(), us, us / R);
    }
    return 0;
}
} // namespace unx::test
