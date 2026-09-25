// R measurement: the dynamic TLAS build floor on this GPU. One small BLAS (a box, 12 triangles) or 256 distinct copies,
// N instances with random transforms in a 400 m cube, TLAS built 32 times per configuration with GPU timestamps
// around each build (global barrier between builds), median reported. Flags: PREFER_FAST_TRACE and PREFER_FAST_BUILD.
// Compares with the frame's r.as.tlas.dynamic (host_dynamic: 1,280 instances 0.33-0.54 ms).
//
//   unx_gate_raytracing_tlasbench   (under Tools/CI/GpuLock.ps1 -Kind timing)
#include "unx/core/Log.h"
#include "unx/render/Device.h"

#include <algorithm>
#include <cstring>
#include <random>
#include <vector>

using namespace unx;
using namespace unx::render;

namespace
{
ComPtr<ID3D12Resource> buffer(Device& device, uint64_t bytes, D3D12_HEAP_TYPE type, D3D12_RESOURCE_FLAGS flags)
{
    D3D12_HEAP_PROPERTIES heap{ type };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = std::max<uint64_t>(bytes, 256);
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    ComPtr<ID3D12Resource> r;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)), "bench buffer");
    return r;
}

void asBarrier(ID3D12GraphicsCommandList7* cmd)
{
    D3D12_GLOBAL_BARRIER g{};
    g.SyncBefore = D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE | D3D12_BARRIER_SYNC_COPY;
    g.SyncAfter = D3D12_BARRIER_SYNC_BUILD_RAYTRACING_ACCELERATION_STRUCTURE;
    g.AccessBefore = D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE | D3D12_BARRIER_ACCESS_COPY_DEST;
    g.AccessAfter = D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_READ | D3D12_BARRIER_ACCESS_RAYTRACING_ACCELERATION_STRUCTURE_WRITE;
    D3D12_BARRIER_GROUP group{ D3D12_BARRIER_TYPE_GLOBAL, 1 };
    group.pGlobalBarriers = &g;
    cmd->Barrier(1, &group);
}
} // namespace

int main()
{
    try
    {
        Device device(DeviceOptions{});
        const float box[8][3] = { { -1, -1, -1 }, { 1, -1, -1 }, { 1, 1, -1 }, { -1, 1, -1 }, { -1, -1, 1 }, { 1, -1, 1 }, { 1, 1, 1 }, { -1, 1, 1 } };
        const uint32_t idx[36] = { 0, 2, 1, 0, 3, 2, 4, 5, 6, 4, 6, 7, 0, 1, 5, 0, 5, 4, 2, 3, 7, 2, 7, 6, 1, 2, 6, 1, 6, 5, 3, 0, 4, 3, 4, 7 };
        ComPtr<ID3D12Resource> geometry = buffer(device, sizeof box + sizeof idx, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE);
        {
            void* m = nullptr;
            check(geometry->Map(0, nullptr, &m), "map geometry");
            std::memcpy(m, box, sizeof box);
            std::memcpy((uint8_t*)m + sizeof box, idx, sizeof idx);
            geometry->Unmap(0, nullptr);
        }
        D3D12_RAYTRACING_GEOMETRY_DESC g{};
        g.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        g.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
        g.Triangles.VertexBuffer = { geometry->GetGPUVirtualAddress(), 12 };
        g.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
        g.Triangles.VertexCount = 8;
        g.Triangles.IndexBuffer = geometry->GetGPUVirtualAddress() + sizeof box;
        g.Triangles.IndexFormat = DXGI_FORMAT_R32_UINT;
        g.Triangles.IndexCount = 36;
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS bi{};
        bi.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        bi.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
        bi.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
        bi.NumDescs = 1;
        bi.pGeometryDescs = &g;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO bs{};
        device.d3d()->GetRaytracingAccelerationStructurePrebuildInfo(&bi, &bs);
        const uint32_t blasCount = 256;
        const uint64_t blasStride = (bs.ResultDataMaxSizeInBytes + 255) & ~255ull;
        ComPtr<ID3D12Resource> blas = buffer(device, blasStride * blasCount, D3D12_HEAP_TYPE_DEFAULT,
                                             D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE);
        ComPtr<ID3D12Resource> blasScratch = buffer(device, bs.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        {
            CommandList cl = device.acquireCommandList(QueueType::Graphics);
            for (uint32_t k = 0; k < blasCount; ++k)
            {
                D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
                d.Inputs = bi;
                d.DestAccelerationStructureData = blas->GetGPUVirtualAddress() + k * blasStride;
                d.ScratchAccelerationStructureData = blasScratch->GetGPUVirtualAddress();
                cl.list->BuildRaytracingAccelerationStructure(&d, 0, nullptr);
                asBarrier(cl.list.Get());
            }
            device.queue(QueueType::Graphics).waitCpu(device.submit(cl));
        }

        // 256 update-able BLASes (ALLOW_UPDATE | PREFER_FAST_BUILD, like the deformed ones), each with its own scratch.
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS ui = bi;
        ui.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE | D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD;
        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO us{};
        device.d3d()->GetRaytracingAccelerationStructurePrebuildInfo(&ui, &us);
        const uint64_t uStride = (us.ResultDataMaxSizeInBytes + 255) & ~255ull;
        const uint64_t uScratch = (std::max(us.ScratchDataSizeInBytes, us.UpdateScratchDataSizeInBytes) + 255) & ~255ull;
        ComPtr<ID3D12Resource> ublas = buffer(device, uStride * blasCount, D3D12_HEAP_TYPE_DEFAULT,
                                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE);
        ComPtr<ID3D12Resource> uscratch = buffer(device, uScratch * blasCount, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        auto buildUpdatable = [&](ID3D12GraphicsCommandList7* cmd, bool update) {
            for (uint32_t k = 0; k < blasCount; ++k)
            {
                D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
                d.Inputs = ui;
                d.DestAccelerationStructureData = ublas->GetGPUVirtualAddress() + k * uStride;
                if (update)
                {
                    d.Inputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
                    d.SourceAccelerationStructureData = d.DestAccelerationStructureData;
                }
                d.ScratchAccelerationStructureData = uscratch->GetGPUVirtualAddress() + k * uScratch;
                cmd->BuildRaytracingAccelerationStructure(&d, 0, nullptr);
            }
        };
        {
            CommandList cl = device.acquireCommandList(QueueType::Graphics);
            buildUpdatable(cl.list.Get(), false);
            asBarrier(cl.list.Get());
            device.queue(QueueType::Graphics).waitCpu(device.submit(cl));
        }

        const uint32_t maxInstances = 16384, repeats = 32;
        ComPtr<ID3D12Resource> descUpload = buffer(device, maxInstances * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_FLAG_NONE);
        ComPtr<ID3D12Resource> descs = buffer(device, maxInstances * sizeof(D3D12_RAYTRACING_INSTANCE_DESC), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE);
        D3D12_QUERY_HEAP_DESC qd{ D3D12_QUERY_HEAP_TYPE_TIMESTAMP, repeats * 2, 0 };
        ComPtr<ID3D12QueryHeap> queries;
        check(device.d3d()->CreateQueryHeap(&qd, IID_PPV_ARGS(&queries)), "query heap");
        ComPtr<ID3D12Resource> readback = buffer(device, repeats * 16, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE);
        const double ticksPerMs = device.queue(QueueType::Graphics).timestampFrequency() / 1000.0;
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> ang(0, 6.2831853f);

        struct Case
        {
            uint32_t distinct, n;
            float spread, scale;  // positions uniform in +-spread m, uniform scale
            const char* name;
            bool refit = false;   // the first 'distinct' instances use the update-able BLASes, refit before every TLAS build
            bool graphStyle = false;  // the begin timestamp right after issuing the refits (no barrier first), as the frame's
                                      // profiler times a pass from the previous pass's end
        };
        std::vector<Case> cases;
        for (const uint32_t distinct : { 1u, blasCount })
            for (const uint32_t n : { 64u, 256u, 1024u, 1280u, 4096u, 16384u }) cases.push_back({ distinct, n, 200, 1, "spread 400 m" });
        cases.push_back({ blasCount, 1280, 5, 1, "clustered in 10 m" });
        cases.push_back({ blasCount, 1280, 1, 1, "clustered in 2 m" });
        cases.push_back({ blasCount, 1280, 200, 50, "spread 400 m, scale 50 (overlapping)" });
        cases.push_back({ blasCount, 1280, 200, 1, "256 update-able BLAS refit before each build", true });
        cases.push_back({ blasCount, 1280, 200, 1, "refit, then timed as the frame's profiler does", true, true });
        cases.push_back({ 1, 1024, 0, 1, "1,024 coincident instances (same position)" });
        cases.push_back({ 1, 1280, 0, 1, "1,280 coincident instances (same position)" });
        for (const Case& cs : cases)
            {
                const uint32_t distinct = cs.distinct, n = cs.n;
                std::uniform_real_distribution<float> place(-std::max(cs.spread, 1e-6f), std::max(cs.spread, 1e-6f));  // spread 0: coincident
                std::vector<D3D12_RAYTRACING_INSTANCE_DESC> inst(n);
                for (uint32_t k = 0; k < n; ++k)
                {
                    const float a = ang(rng), c = std::cos(a), s = std::sin(a);
                    const float sc = cs.scale;
                    const float t[3][4] = { { sc * c, 0, sc * s, place(rng) }, { 0, sc, 0, place(rng) }, { -sc * s, 0, sc * c, place(rng) } };
                    std::memcpy(inst[k].Transform, t, sizeof t);
                    inst[k].InstanceID = k;
                    inst[k].InstanceMask = 0xFF;
                    inst[k].AccelerationStructure = cs.refit && k < blasCount ? ublas->GetGPUVirtualAddress() + k * uStride
                                                                              : blas->GetGPUVirtualAddress() + (k % distinct) * blasStride;
                }
                void* m = nullptr;
                check(descUpload->Map(0, nullptr, &m), "map descs");
                std::memcpy(m, inst.data(), n * sizeof inst[0]);
                descUpload->Unmap(0, nullptr);
                for (const auto flags : { D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE, D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_BUILD })
                {
                    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS ti{};
                    ti.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
                    ti.Flags = flags;
                    ti.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
                    ti.NumDescs = n;
                    ti.InstanceDescs = descs->GetGPUVirtualAddress();
                    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO ts{};
                    device.d3d()->GetRaytracingAccelerationStructurePrebuildInfo(&ti, &ts);
                    ComPtr<ID3D12Resource> tlas = buffer(device, ts.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
                                                         D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE);
                    ComPtr<ID3D12Resource> scratch = buffer(device, ts.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
                    CommandList cl = device.acquireCommandList(QueueType::Graphics);
                    cl.list->CopyBufferRegion(descs.Get(), 0, descUpload.Get(), 0, n * sizeof inst[0]);
                    for (uint32_t r = 0; r < repeats; ++r)
                    {
                        if (cs.refit) buildUpdatable(cl.list.Get(), true);
                        if (cs.graphStyle) cl.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * r);
                        asBarrier(cl.list.Get());
                        if (!cs.graphStyle) cl.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * r);
                        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC d{};
                        d.Inputs = ti;
                        d.DestAccelerationStructureData = tlas->GetGPUVirtualAddress();
                        d.ScratchAccelerationStructureData = scratch->GetGPUVirtualAddress();
                        cl.list->BuildRaytracingAccelerationStructure(&d, 0, nullptr);
                        asBarrier(cl.list.Get());
                        cl.list->EndQuery(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * r + 1);
                    }
                    cl.list->ResolveQueryData(queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, repeats * 2, readback.Get(), 0);
                    device.queue(QueueType::Graphics).waitCpu(device.submit(cl));
                    uint64_t* t = nullptr;
                    check(readback->Map(0, nullptr, (void**)&t), "map readback");
                    std::vector<double> ms;
                    for (uint32_t r = 1; r < repeats; ++r) ms.push_back((t[2 * r + 1] - t[2 * r]) / ticksPerMs);  // first build: cold
                    readback->Unmap(0, nullptr);
                    std::sort(ms.begin(), ms.end());
                    logf("TLAS %5u instances over %3u distinct BLAS, %s, %s: median %.4f ms (min %.4f, max %.4f), result %.2f MB\n", n, distinct,
                         flags == D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE ? "FAST_TRACE" : "FAST_BUILD", cs.name, ms[ms.size() / 2], ms.front(), ms.back(),
                         ts.ResultDataMaxSizeInBytes / 1048576.0);
                }
            }
        device.waitIdle();
        return 0;
    }
    catch (const std::exception& e)
    {
        logf("error: %s\n", e.what());
        return 2;
    }
}
