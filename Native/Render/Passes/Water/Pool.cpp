// Closed basins W2 (track W; FEATURES_GAME 1.10). See include/unx/water/Pool.h and Pool.hlsli.
#include "unx/water/Pool.h"

#include "unx/core/Log.h"
#include "unx/render/Frame.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace unx::water
{
using namespace unx::render;

namespace
{
constexpr uint64_t kSamples = uint64_t(Pool::kQ) * Pool::kQ;
constexpr uint64_t kVertices = 6ull * Pool::kCells * Pool::kCells;
constexpr double kPi = 3.14159265358979323846;

ComPtr<ID3D12Resource> makeBuffer(Device& device, uint64_t bytes, D3D12_HEAP_TYPE type, const wchar_t* name)
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
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)),
          "pool buffer");
    r->SetName(name);
    return r;
}
// The basin's per-mode constants in double, rounded once (PoolEvolve's table): w, K / w, w / K and the damping delta
// (Pool.hlsli), for mode (m, n): k = (m pi / Lx, n pi / Lz).
std::vector<float> modeTable(const PoolDesc& d)
{
    std::vector<float> t(size_t(Pool::kQ) * Pool::kQ * 4, 0.0f);
    for (uint32_t n = 0; n < Pool::kQ; ++n)
        for (uint32_t m = 0; m < Pool::kQ; ++m)
        {
            const double kx = kPi * m / d.sizeX, kz = kPi * n / d.sizeZ, k = std::sqrt(kx * kx + kz * kz);
            if (k == 0) continue;
            const double D = d.depth, K = D > 0 ? k * std::tanh(k * D) : k, G = double(d.gravity) + double(d.tensionOverDensity) * k * k, w = std::sqrt(K * G);
            const double nu = d.viscosity, half2k = 0.5 / k, s = D > 0 ? std::sinh(2 * k * D) : 0, dOverS = D > 0 ? D / s : 0;
            const double wallsX = (m != 0 ? 4.0 : 2.0) / d.sizeX, wallsZ = (n != 0 ? 4.0 : 2.0) / d.sizeZ;
            const double layers = (D > 0 ? 2 * k * k / s : 0) + wallsX * (kz * kz * (dOverS + half2k) + k * k * (half2k - dOverS)) +
                                  wallsZ * (kx * kx * (dOverS + half2k) + k * k * (half2k - dOverS)) + double(d.surfaceFilm) * k * k * (D > 0 ? 1 / std::tanh(k * D) : 1.0);
            const double delta = 2 * nu * k * k + std::sqrt(0.5 * nu * w) * half2k * layers;
            float* e = &t[4 * (size_t(n) * Pool::kQ + m)];
            e[0] = float(w); e[1] = float(K / w); e[2] = float(w / K); e[3] = float(delta);
        }
    return t;
}
void axes(float yaw, double ax[2], double az[2])
{
    const double c = std::cos(double(yaw)), s = std::sin(double(yaw));
    ax[0] = c; ax[1] = -s;
    az[0] = s; az[1] = c;
}
} // namespace

Pool::Pool(Device& device, ShaderLibrary& shaders, const PoolDesc& desc) : m_device(device), m_shaders(shaders), m_desc(desc)
{
    m_topologyId = allocateTriangleStreamTopologyId();
    m_indices = makeBuffer(device, kVertices * 4, D3D12_HEAP_TYPE_DEFAULT, L"pool grid indices");
    if (!(desc.sizeX > 0) || !(desc.sizeZ > 0) || !(desc.depth >= 0) || !(desc.gravity > 0) || !(desc.tensionOverDensity >= 0) || !(desc.viscosity >= 0) || !(desc.surfaceFilm == 0 || desc.surfaceFilm == 1) ||
        !desc.maxSources || !desc.framesInFlight)
        fail("pool: invalid description");
    m_modes = makeBuffer(device, kSamples * 8, D3D12_HEAP_TYPE_DEFAULT, L"pool modes");
    m_input = makeBuffer(device, kSamples * 8, D3D12_HEAP_TYPE_DEFAULT, L"pool input field");
    m_accum = makeBuffer(device, kSamples * 8, D3D12_HEAP_TYPE_DEFAULT, L"pool sources");
    m_previous = makeBuffer(device, kSamples * 4, D3D12_HEAP_TYPE_DEFAULT, L"pool previous eta");
    m_twiddles = makeBuffer(device, kN * 4, D3D12_HEAP_TYPE_DEFAULT, L"pool twiddles");
    m_table = makeBuffer(device, kSamples * 16, D3D12_HEAP_TYPE_DEFAULT, L"pool mode table");
    {
        const std::vector<float> table = modeTable(desc);
        m_tableUpload = makeBuffer(device, kSamples * 16, D3D12_HEAP_TYPE_UPLOAD, L"pool mode table upload");
        void* mapped = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(m_tableUpload->Map(0, &none, &mapped), "map pool mode table");
        std::memcpy(mapped, table.data(), kSamples * 16);
        m_tableUpload->Unmap(0, nullptr);
    }
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC1 t{};
    t.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    t.Width = t.Height = kQ;
    t.DepthOrArraySize = t.MipLevels = 1;
    t.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    t.SampleDesc.Count = 1;
    t.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &t, D3D12_BARRIER_LAYOUT_COMMON, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_output)), "pool output");
    m_output->SetName(L"pool field");
    m_stats = makeBuffer(device, uint64_t(kQ + 1) * 16, D3D12_HEAP_TYPE_DEFAULT, L"pool statistics");
    for (uint32_t s = 0; s <= desc.framesInFlight; ++s)
    {
        m_statsReadback.push_back(makeBuffer(device, 256, D3D12_HEAP_TYPE_READBACK, L"pool statistics readback"));
        m_statsFrame.push_back(UINT64_MAX);
        m_statsTime.push_back(0);
    }
    for (uint32_t s = 0; s < desc.framesInFlight; ++s)
    {
        m_sourceUpload.push_back(makeBuffer(device, uint64_t(desc.maxSources) * 32 + kN * 4, D3D12_HEAP_TYPE_UPLOAD, L"pool upload"));
        uint8_t* mapped = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(m_sourceUpload.back()->Map(0, &none, (void**)&mapped), "map pool upload");
        m_sourceMapped.push_back(mapped);
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements = UINT(desc.maxSources) * 8;
        sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        m_sourceSrv.push_back(device.descriptors().allocateResource());
        device.d3d()->CreateShaderResourceView(m_sourceUpload.back().Get(), &sd, device.descriptors().resourceCpu(m_sourceSrv.back()));
    }
}
Pool::~Pool()
{
    for (auto& u : m_sourceUpload) u->Unmap(0, nullptr);
    for (const ComPtr<ID3D12Resource>& r : { m_modes, m_input, m_accum, m_previous, m_twiddles, m_table, m_tableUpload, m_output, m_stateUpload, m_stats, m_indices })
        if (r) m_device.deferRelease(r);
    for (auto& u : m_sourceUpload) m_device.deferRelease(u);
    for (auto& u : m_statsReadback) m_device.deferRelease(u);
    for (uint32_t srv : m_sourceSrv) m_device.descriptors().freeResource(srv);
}

void Pool::toSamples(const PoolDesc& desc, const PoolPlacement& placement, double x, double z, float& u, float& v)
{
    double ax[2], az[2];
    axes(placement.yaw, ax, az);
    const double dx = x - placement.centre[0], dz = z - placement.centre[2];
    const double lx = dx * ax[0] + dz * ax[1] + 0.5 * desc.sizeX, lz = dx * az[0] + dz * az[1] + 0.5 * desc.sizeZ;
    u = float(lx * kCells / desc.sizeX);
    v = float(lz * kCells / desc.sizeZ);
}
bool Pool::visible(const PoolDesc& desc, const PoolPlacement& placement, const float4x4& viewProj)
{
    double ax[2], az[2];
    axes(placement.yaw, ax, az);
    const double vertical = desc.depth > 0 ? desc.depth : 1.0;
    uint32_t outside[5] = {};  // corners beyond x < -w, x > w, y < -w, y > w, w <= 0
    for (int c = 0; c < 8; ++c)
    {
        const double sx = (c & 1) ? 0.5 : -0.5, sz = (c & 2) ? 0.5 : -0.5, sy = (c & 4) ? 1.0 : -1.0;
        const double p[3] = { placement.centre[0] + sx * desc.sizeX * ax[0] + sz * desc.sizeZ * az[0], placement.centre[1] + sy * vertical,
                              placement.centre[2] + sx * desc.sizeX * ax[1] + sz * desc.sizeZ * az[1] };
        double clip[4];
        for (int r = 0; r < 4; ++r) clip[r] = viewProj.m[r][0] * p[0] + viewProj.m[r][1] * p[1] + viewProj.m[r][2] * p[2] + viewProj.m[r][3];
        outside[0] += clip[0] < -clip[3];
        outside[1] += clip[0] > clip[3];
        outside[2] += clip[1] < -clip[3];
        outside[3] += clip[1] > clip[3];
        outside[4] += clip[3] <= 0;
    }
    for (uint32_t o : outside)
        if (o == 8) return false;
    return true;
}
bool Pool::contains(const PoolDesc& desc, const PoolPlacement& placement, double x, double z)
{
    float u, v;
    toSamples(desc, placement, x, z, u, v);
    return u >= 0 && v >= 0 && u <= float(kCells) && v <= float(kCells);
}

void Pool::setState(const std::vector<float>& etaPhi)
{
    if (etaPhi.size() != kSamples * 2) fail("pool: a state has %llu floats, expected %llu", (unsigned long long)etaPhi.size(), (unsigned long long)(kSamples * 2));
    if (m_stateUpload) m_device.deferRelease(m_stateUpload);
    m_stateUpload = makeBuffer(m_device, kSamples * 8, D3D12_HEAP_TYPE_UPLOAD, L"pool state upload");
    void* mapped = nullptr;
    D3D12_RANGE none{ 0, 0 };
    check(m_stateUpload->Map(0, &none, &mapped), "map pool state upload");
    std::memcpy(mapped, etaPhi.data(), kSamples * 8);
    m_stateUpload->Unmap(0, nullptr);
    m_stateDirty = true;
}

void Pool::evolve(RenderGraph& g, const Refs& r, float dt, uint32_t sourceSrv, uint32_t sourceCount, float meanShift, bool replace)
{
    const BufferRef modes = r.modes, input = r.input, accum = r.accum, previous = r.previous, twiddles = r.twiddles, table = r.table;
    const TextureRef field = r.field;
    const BufferRef spectrum = g.createBuffer({ "pool spectrum", uint64_t(kN) * (kN + 1) * 16, 0 });
    const PoolDesc d = m_desc;
    const float hx = d.sizeX / kCells, hz = d.sizeZ / kCells;
    // An increment (sources, or the uploaded field that replaces the state) is transformed only when there is one: the
    // spectral state itself is never round-tripped (PoolEvolve).
    const bool increment = sourceCount > 0 || replace || meanShift != 0;
    const uint32_t flags = (increment ? 1u : 0u) | (replace ? 2u : 0u);
    auto constants = [=](PassContext& c) {
        uint32_t k[24] = { c.uav(modes), c.uav(spectrum), c.uav(field), c.uav(accum), 0, c.uav(previous), c.srv(twiddles), 0, sourceSrv, sourceCount };
        std::memcpy(&k[4], &dt, 4);
        std::memcpy(&k[7], &d.depth, 4);
        std::memcpy(&k[10], &hx, 4);
        std::memcpy(&k[11], &hz, 4);
        std::memcpy(&k[12], &d.gravity, 4);
        std::memcpy(&k[13], &d.tensionOverDensity, 4);
        std::memcpy(&k[14], &d.viscosity, 4);
        std::memcpy(&k[15], &meanShift, 4);
        std::memcpy(&k[16], &d.surfaceFilm, 4);
        std::memcpy(&k[17], &d.sizeX, 4);
        std::memcpy(&k[18], &d.sizeZ, 4);
        k[19] = flags;
        k[20] = c.uav(input);
        k[21] = c.srv(table);
        c.computeConstants(k, 24);
    };
    auto uses = [&](PassBuilder& pb) {
        pb.use(modes, Use::UavCompute); pb.use(input, Use::UavCompute); pb.use(spectrum, Use::UavCompute); pb.use(field, Use::UavCompute);
        pb.use(accum, Use::UavCompute); pb.use(previous, Use::UavCompute); pb.use(twiddles, Use::SrvCompute); pb.use(table, Use::SrvCompute);
    };
    auto pass = [&](const char* name, const char* kernel, uint32_t groups) {
        ID3D12PipelineState* pso = m_shaders.compute(kernel);
        g.addPass(name, QueueType::Graphics, uses, [=](PassContext& c) { c.cmd->SetPipelineState(pso); constants(c); c.cmd->Dispatch(groups, 1, 1); });
    };
    if (sourceCount) pass("pool sources", "Passes/Water/PoolSplat", sourceCount);
    if (increment)
    {
        pass("pool forward rows", "Passes/Water/PoolRows", kQ);
        pass("pool forward columns", "Passes/Water/RippleForwardColumns", kQ);  // only canonical columns enter the modes
    }
    pass("pool evolve rows", "Passes/Water/PoolEvolveRows", kQ);
    pass("pool inverse columns", "Passes/Water/PoolColumns", kQ);
}

PoolOutput Pool::record(RenderGraph& g, uint64_t frame, const PoolPlacement& placement, double time, float frameDt, const std::vector<PoolSource>& sources)
{
    if (!(frameDt >= 0)) fail("pool: negative frame time");
    if (sources.size() > m_desc.maxSources) fail("pool: %zu sources exceed the capacity %u", sources.size(), m_desc.maxSources);
    if (m_started && !(time >= m_time)) fail("pool: time went back (%.9g after %.9g)", time, m_time);
    const uint32_t slot = uint32_t(frame % m_desc.framesInFlight);
    uint8_t* mapped = m_sourceMapped[slot];
    for (size_t i = 0; i < sources.size(); ++i)
    {
        float u, v;
        toSamples(m_desc, placement, sources[i].x, sources[i].z, u, v);
        if (!(u >= 0 && v >= 0 && u <= float(kCells) && v <= float(kCells)))
            fail("pool: source %zu at (%.6g, %.6g) lies outside the basin (samples %.6g, %.6g)", i, sources[i].x, sources[i].z, u, v);
        const float s[8] = { u, v, sources[i].radius, sources[i].impulse, sources[i].volume, 0, 0, 0 };
        std::memcpy(mapped + 32 * i, s, 32);
    }
    // Every persistent resource is imported once (the graph orders passes by resource node).
    auto import = [&](ID3D12Resource* r, const char* name) { return g.importBuffer(r, { name, r->GetDesc().Width, 0 }); };
    Refs refs;
    refs.modes = import(m_modes.Get(), "pool modes");
    refs.input = import(m_input.Get(), "pool input field");
    refs.accum = import(m_accum.Get(), "pool sources");
    refs.previous = import(m_previous.Get(), "pool previous eta");
    refs.twiddles = import(m_twiddles.Get(), "pool twiddles");
    refs.table = import(m_table.Get(), "pool mode table");
    refs.field = g.importTexture(m_output.Get(), TextureDesc{ "pool field", kQ, kQ, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT }, D3D12_BARRIER_LAYOUT_COMMON);
    const BufferRef modes = refs.modes, input = refs.input, accum = refs.accum, previous = refs.previous, twiddles = refs.twiddles;
    const TextureRef field = refs.field;
    ID3D12Resource* upload = m_sourceUpload[slot].Get();
    if (!m_initialised)
    {
        float* tw = (float*)(mapped + uint64_t(m_desc.maxSources) * 32);
        for (uint32_t j = 0; j < kN / 2; ++j) { tw[2 * j] = float(std::cos(2 * kPi * j / kN)); tw[2 * j + 1] = float(std::sin(2 * kPi * j / kN)); }
        const uint64_t at = uint64_t(m_desc.maxSources) * 32;
        const BufferRef table = refs.table;
        ID3D12Resource* tableUpload = m_tableUpload.Get();
        g.addPass("pool twiddles", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(twiddles, Use::CopyDst); pb.use(table, Use::CopyDst); pb.keep(); },
                  [=](PassContext& c) {
                      c.cmd->CopyBufferRegion(c.resource(twiddles), 0, upload, at, kN * 4);
                      c.cmd->CopyBufferRegion(c.resource(table), 0, tableUpload, 0, kSamples * 16);
                  });
        ID3D12PipelineState* clear = m_shaders.compute("Passes/Water/PoolClear");  // a calm basin
        g.addPass("pool clear", QueueType::Graphics,
                  [&](PassBuilder& pb) {
                      pb.use(accum, Use::UavCompute); pb.use(modes, Use::UavCompute); pb.use(previous, Use::UavCompute); pb.use(field, Use::UavCompute);
                      pb.keep();
                  },
                  [=](PassContext& c) {
                      const uint32_t k[8] = { c.uav(modes), 0, c.uav(field), c.uav(accum), 0, c.uav(previous), 0, 0 };
                      c.cmd->SetPipelineState(clear);
                      c.computeConstants(k, 8);
                      c.cmd->Dispatch(uint32_t((kSamples + 255) / 256), 1, 1);
                  });
        m_initialised = true;
    }
    const bool replace = m_stateDirty;  // setState: the first evolution replaces the state by the uploaded field
    if (m_stateDirty)
    {
        ID3D12Resource* source = m_stateUpload.Get();
        g.addPass("pool state upload", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(input, Use::CopyDst); pb.keep(); },
                  [=](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(input), 0, source, 0, kSamples * 8); });
        m_stateDirty = false;
    }
    // The interval since the last evolution: beyond one frame (the basin was not recorded), first to time - frameDt.
    double gap = m_started ? time - m_time : 0.0;
    if (gap > double(frameDt) * (1 + 1e-6) + 1e-9)
    {
        evolve(g, refs, float(gap - double(frameDt)), 0, 0, 0.0f, replace);
        gap = frameDt;
        double volume = 0;  // the sources' mean-level term, in double (PoolSplat)
        for (const PoolSource& s : sources) volume += s.volume;
        evolve(g, refs, float(gap), m_sourceSrv[slot], uint32_t(sources.size()), float(volume / (double(m_desc.sizeX) * m_desc.sizeZ)), false);
    }
    else
    {
        double volume = 0;
        for (const PoolSource& s : sources) volume += s.volume;
        evolve(g, refs, float(gap), m_sourceSrv[slot], uint32_t(sources.size()), float(volume / (double(m_desc.sizeX) * m_desc.sizeZ)), replace);
    }
    m_time = time;
    m_started = true;

    // Surface statistics (PoolStats.hlsl; the host's UnxPoolStatsLatest): per row the sum, sum of squares, max and min of
    // eta, then one group reduces the rows in order (mean, RMS about the mean, max |eta - mean|), copied to this record's
    // readback slot. Read back: the slot written framesInFlight records ago (the host waited for that frame; a record
    // is at most one per frame).
    {
        const BufferRef stats = import(m_stats.Get(), "pool statistics");
        ID3D12PipelineState* rows = m_shaders.compute("Passes/Water/PoolStats.MODE0");
        ID3D12PipelineState* reduce = m_shaders.compute("Passes/Water/PoolStats.MODE1");
        g.addPass("pool stats rows", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(field, Use::SrvCompute); pb.use(stats, Use::UavCompute); },
                  [=](PassContext& c) {
                      const uint32_t k[4] = { c.srv(field), c.uav(stats), 0, 0 };
                      c.cmd->SetPipelineState(rows);
                      c.computeConstants(k, 4);
                      c.cmd->Dispatch(kQ, 1, 1);
                  });
        g.addPass("pool stats reduce", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(stats, Use::UavCompute); },
                  [=](PassContext& c) {
                      const uint32_t k[4] = { 0, c.uav(stats), 0, 0 };
                      c.cmd->SetPipelineState(reduce);
                      c.computeConstants(k, 4);
                      c.cmd->Dispatch(1, 1, 1);
                  });
        const uint32_t ring = uint32_t(m_statsReadback.size()), writeSlot = uint32_t(m_records % ring), readSlot = uint32_t((m_records + 1) % ring);
        ID3D12Resource* rb = m_statsReadback[writeSlot].Get();
        g.addPass("pool stats readback", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(stats, Use::CopySrc); pb.keep(); },
                  [=](PassContext& c) { c.cmd->CopyBufferRegion(rb, 0, c.resource(stats), uint64_t(kQ) * 16, 16); });
        m_statsFrame[writeSlot] = frame;
        m_statsTime[writeSlot] = time;
        if (m_records + 1 >= ring && m_statsFrame[readSlot] != UINT64_MAX)  // written ring - 1 = framesInFlight records ago
        {
            const uint8_t* m = nullptr;
            D3D12_RANGE all{ 0, 16 };
            check(m_statsReadback[readSlot]->Map(0, &all, (void**)&m), "map pool statistics");
            float v[4];
            std::memcpy(v, m, 16);
            D3D12_RANGE none{ 0, 0 };
            m_statsReadback[readSlot]->Unmap(0, &none);
            m_latestStats.valid = true;
            m_latestStats.frame = m_statsFrame[readSlot];
            m_latestStats.time = m_statsTime[readSlot];
            m_latestStats.mean = v[0];
            m_latestStats.rms = v[1];
            m_latestStats.maxDeviation = v[2];
        }
        ++m_records;
    }

    // The surface's triangle stream: frame buffers (the graph's transient memory; only the drawn basins hold any).
    const BufferRef indices = import(m_indices.Get(), "pool grid indices");
    if (!m_indicesReady)
    {
        ID3D12PipelineState* grid = m_shaders.compute("Passes/Water/PoolIndices");
        g.addPass("pool grid indices", QueueType::Graphics,
                  [&](PassBuilder& pb) { pb.use(indices, Use::UavCompute); pb.onSubmitted([this](Queue&, uint64_t) { m_indicesReady = true; }); },
                  [=](PassContext& c) { const uint32_t k[4] = {c.uav(indices), 0, 0, 0}; c.computeConstants(k, 4); c.cmd->SetPipelineState(grid); c.cmd->Dispatch(uint32_t((kVertices + 255) / 256), 1, 1); });
    }
    const BufferRef vertices = g.createBuffer({ "pool surface vertices", kSamples * 32, 0 }), velocities = g.createBuffer({ "pool surface velocities", kSamples * 16, 0 }),
                    draw = g.createBuffer({ "pool surface draw", 16, 0 });
    double ax[2], az[2];
    axes(placement.yaw, ax, az);
    const float hx = m_desc.sizeX / kCells, hz = m_desc.sizeZ / kCells;
    const float origin[2] = { float(placement.centre[0] - 0.5 * m_desc.sizeX * ax[0] - 0.5 * m_desc.sizeZ * az[0]),
                              float(placement.centre[2] - 0.5 * m_desc.sizeX * ax[1] - 0.5 * m_desc.sizeZ * az[1]) };
    const float level = float(placement.centre[1]), invDt = frameDt > 0 ? 1.0f / frameDt : 0.0f;
    const float axis[4] = { float(ax[0]), float(ax[1]), float(az[0]), float(az[1]) };
    ID3D12PipelineState* mesh = m_shaders.compute("Passes/Water/PoolMesh");
    g.addPass("pool surface", QueueType::Graphics,
              [&](PassBuilder& pb) {
                  pb.use(field, Use::SrvCompute); pb.use(previous, Use::SrvCompute);
                  pb.use(vertices, Use::UavCompute); pb.use(velocities, Use::UavCompute); pb.use(draw, Use::UavCompute);
              },
              [=](PassContext& c) {
                  uint32_t k[20] = { c.srv(field), c.srv(previous), c.uav(vertices), c.uav(velocities), c.uav(draw) };
                  std::memcpy(&k[5], &invDt, 4);
                  std::memcpy(&k[6], &level, 4);
                  std::memcpy(&k[8], &origin[0], 4);
                  std::memcpy(&k[10], &origin[1], 4);
                  std::memcpy(&k[11], &hx, 4);
                  std::memcpy(&k[12], axis, 16);
                  std::memcpy(&k[16], &hz, 4);
                  c.cmd->SetPipelineState(mesh);
                  c.computeConstants(k, 20);
                  c.cmd->Dispatch(uint32_t((kSamples + 63) / 64), 1, 1);
              });

    PoolOutput out;
    out.field = field;
    TriangleStream& s = out.stream;
    s.vertices = vertices;
    s.velocities = velocities;
    s.drawArgs = draw;
    s.indices = indices;
    s.vertexCount = uint32_t(kSamples);
    s.maxTriangles = uint32_t(kVertices / 3);
    s.layer = 1;
    s.fixedTopologyId = m_topologyId; // PoolMesh writes every cell, without inactive triangles
    s.knownTriangleCount = s.maxTriangles;
    // Bounds: the rectangle's corners, the surface within +-d of the still level (the linear model's validity: the waves'
    // height stays well below the depth; deep water: 1 m).
    const double vertical = m_desc.depth > 0 ? m_desc.depth : 1.0;
    double lo[3] = { 1e300, placement.centre[1] - vertical, 1e300 }, hi[3] = { -1e300, placement.centre[1] + vertical, -1e300 };
    for (int cx = -1; cx <= 1; cx += 2)
        for (int cz = -1; cz <= 1; cz += 2)
            for (int a = 0; a < 2; ++a)
            {
                const double p = placement.centre[2 * a] + 0.5 * (cx * m_desc.sizeX * ax[a] + cz * m_desc.sizeZ * az[a]);
                lo[2 * a] = std::min(lo[2 * a], p);
                hi[2 * a] = std::max(hi[2 * a], p);
            }
    s.boundsMin = { float(lo[0]), float(lo[1]), float(lo[2]) };
    s.boundsMax = { float(hi[0]), float(hi[1]), float(hi[2]) };
    return out;
}
} // namespace unx::water
