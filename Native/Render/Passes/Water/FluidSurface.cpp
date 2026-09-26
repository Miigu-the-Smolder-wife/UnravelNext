// Fluid surface reconstruction (track W, B8). See include/unx/water/FluidSurface.h and FluidSurface.hlsli.
#include "unx/water/FluidSurface.h"

#include "unx/core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace unx::water
{
using namespace unx::render;

namespace
{
constexpr uint32_t kBlock = 8, kBlockNodes = 512, kCounters = 16;

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
          "fluid surface buffer");
    r->SetName(name);
    return r;
}
uint32_t groups(uint64_t threads, uint32_t size = 256) { return (uint32_t)((threads + size - 1) / size); }

// Case table construction. Corner c has coordinates (c & 1, (c >> 1) & 1, c >> 2). A face of the cube is a 4-cycle of
// corners; on a face whose corners alternate inside/outside (four crossing edges) each inside corner is cut off by its
// own segment, a rule both cubes sharing the face apply identically, so neighbouring cells agree on every face and the
// surface has no cracks. The segments of all faces join into closed loops (each crossing edge lies on two faces and in
// one segment of each), and each loop is triangulated (triangulate(): no diagonal in a cube face) with triangles oriented
// CCW when seen from the outside.
// Two cube edges lie on a common face.
bool shareFace(uint32_t e0, uint32_t e1)
{
    const auto& E = FluidSurface::kEdges;
    for (uint32_t axis = 0; axis < 3; ++axis)
        for (uint32_t side = 0; side < 2; ++side)
        {
            auto onFace = [&](uint32_t e) { return ((E[e][0] >> axis) & 1) == side && ((E[e][1] >> axis) & 1) == side; };
            if (onFace(e0) && onFace(e1)) return true;
        }
    return false;
}
// Triangulates a loop (oriented) without any diagonal between two vertices on a common cube face: such a diagonal lies in
// the face, and the neighbouring cube, which shares both vertices, may draw the same one, so four triangles would meet
// along it (a non-manifold pinch). Among the admissible triangulations the one of least total diagonal length (vertices
// at the edge midpoints) is taken; a loop without one is a table construction failure.
void triangulate(const std::vector<uint32_t>& loop, std::vector<uint32_t>& triangles, uint32_t mask)
{
    const size_t n = loop.size();
    auto mid = [&](uint32_t e) {
        const auto& E = FluidSurface::kEdges;
        return std::array<double, 3>{ ((E[e][0] & 1) + (E[e][1] & 1)) / 2.0, (((E[e][0] >> 1) & 1) + ((E[e][1] >> 1) & 1)) / 2.0, ((E[e][0] >> 2) + (E[e][1] >> 2)) / 2.0 };
    };
    auto chord = [&](size_t i, size_t j) {  // admissible side (i < j): a loop edge, or a diagonal off every face
        if (j == i + 1 || (i == 0 && j == n - 1)) return 0.0;
        if (shareFace(loop[i], loop[j])) return -1.0;
        const auto a = mid(loop[i]), b = mid(loop[j]);
        return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
    };
    constexpr double kNone = 1e30;
    std::vector<double> cost(n * n, kNone);
    std::vector<size_t> split(n * n, 0);
    for (size_t i = 0; i + 1 < n; ++i) cost[i * n + i + 1] = 0;
    for (size_t span = 2; span < n; ++span)
        for (size_t i = 0; i + span < n; ++i)
        {
            const size_t j = i + span;
            const double side = chord(i, j);
            if (side < 0) continue;
            for (size_t k = i + 1; k < j; ++k)
            {
                const double c = cost[i * n + k] + cost[k * n + j] + side;
                if (cost[i * n + k] < kNone && cost[k * n + j] < kNone && c < cost[i * n + j]) { cost[i * n + j] = c; split[i * n + j] = k; }
            }
        }
    if (!(cost[n - 1] < kNone)) fail("fluid surface: case %u has a loop of %zu vertices with no triangulation off the cube faces", mask, n);
    std::vector<std::pair<size_t, size_t>> stack{ { 0, n - 1 } };
    while (!stack.empty())
    {
        const auto [i, j] = stack.back();
        stack.pop_back();
        if (j < i + 2) continue;
        const size_t k = split[i * n + j];
        triangles.insert(triangles.end(), { loop[i], loop[k], loop[j] });
        stack.push_back({ i, k });
        stack.push_back({ k, j });
    }
}

std::array<uint32_t, 256 * FluidSurface::kCaseStride> buildCases()
{
    const auto& E = FluidSurface::kEdges;
    auto edgeOf = [&](uint32_t a, uint32_t b) {
        for (uint32_t e = 0; e < 12; ++e)
            if ((E[e][0] == a && E[e][1] == b) || (E[e][0] == b && E[e][1] == a)) return e;
        fail("fluid surface: corners %u and %u share no edge", a, b);
    };
    // Faces: fixed axis a and side s; the other axes u, v in cyclic order.
    std::vector<std::array<uint32_t, 4>> faces;
    for (uint32_t a = 0; a < 3; ++a)
        for (uint32_t s = 0; s < 2; ++s)
        {
            const uint32_t u = (a + 1) % 3, v = (a + 2) % 3;
            auto corner = [&](uint32_t cu, uint32_t cv) { return (s << a) | (cu << u) | (cv << v); };
            faces.push_back({ corner(0, 0), corner(1, 0), corner(1, 1), corner(0, 1) });
        }
    auto point = [](uint32_t c) { return std::array<double, 3>{ double(c & 1), double((c >> 1) & 1), double(c >> 2) }; };
    std::array<uint32_t, 256 * FluidSurface::kCaseStride> table{};
    for (uint32_t mask = 0; mask < 256; ++mask)
    {
        auto inside = [&](uint32_t c) { return ((mask >> c) & 1) != 0; };
        std::multimap<uint32_t, uint32_t> link;  // crossing edge -> the other end of a segment
        for (const auto& f : faces)
        {
            uint32_t crossing[4], n = 0;
            for (uint32_t k = 0; k < 4; ++k)
                if (inside(f[k]) != inside(f[(k + 1) % 4])) crossing[n++] = k;
            auto segment = [&](uint32_t e0, uint32_t e1) { link.emplace(e0, e1); link.emplace(e1, e0); };
            if (n == 2) segment(edgeOf(f[crossing[0]], f[(crossing[0] + 1) % 4]), edgeOf(f[crossing[1]], f[(crossing[1] + 1) % 4]));
            else if (n == 4)
                for (uint32_t k = 0; k < 4; ++k)
                    if (inside(f[k])) segment(edgeOf(f[(k + 3) % 4], f[k]), edgeOf(f[k], f[(k + 1) % 4]));
        }
        std::vector<uint32_t> triangles;
        std::map<uint32_t, bool> used;
        for (const auto& [start, unusedOther] : link)
        {
            (void)unusedOther;
            if (used[start]) continue;
            std::vector<uint32_t> loop{ start };
            used[start] = true;
            uint32_t previous = UINT32_MAX, current = start;
            for (;;)
            {
                uint32_t next = UINT32_MAX;
                auto range = link.equal_range(current);
                for (auto it = range.first; it != range.second; ++it)
                    if (it->second != previous) { next = it->second; break; }
                if (next == UINT32_MAX) fail("fluid surface: open loop in case %u", mask);
                if (next == start) break;
                if (used[next]) fail("fluid surface: loop crosses itself in case %u", mask);
                used[next] = true;
                loop.push_back(next);
                previous = current;
                current = next;
            }
            if (loop.size() < 3) fail("fluid surface: degenerate loop in case %u", mask);
            // Orientation: the Newell normal of the loop (vertices at edge midpoints) against the edges' inside -> outside
            // directions.
            std::array<double, 3> nrm{}, out{};
            for (size_t k = 0; k < loop.size(); ++k)
            {
                auto mid = [&](uint32_t e) { auto p = point(E[e][0]), q = point(E[e][1]); return std::array<double, 3>{ (p[0] + q[0]) / 2, (p[1] + q[1]) / 2, (p[2] + q[2]) / 2 }; };
                const auto a = mid(loop[k]), b = mid(loop[(k + 1) % loop.size()]);
                nrm[0] += (a[1] - b[1]) * (a[2] + b[2]);
                nrm[1] += (a[2] - b[2]) * (a[0] + b[0]);
                nrm[2] += (a[0] - b[0]) * (a[1] + b[1]);
                const uint32_t in = inside(E[loop[k]][0]) ? E[loop[k]][0] : E[loop[k]][1], outside = in == E[loop[k]][0] ? E[loop[k]][1] : E[loop[k]][0];
                const auto pi = point(in), po = point(outside);
                for (int i = 0; i < 3; ++i) out[i] += po[i] - pi[i];
            }
            if (nrm[0] * out[0] + nrm[1] * out[1] + nrm[2] * out[2] < 0) std::reverse(loop.begin() + 1, loop.end());
            triangulate(loop, triangles, mask);
        }
        const uint32_t count = (uint32_t)triangles.size() / 3;
        if (count > FluidSurface::kMaxCaseTriangles) fail("fluid surface: case %u needs %u triangles (table holds %u)", mask, count, FluidSurface::kMaxCaseTriangles);
        table[mask * FluidSurface::kCaseStride] = count;
        std::copy(triangles.begin(), triangles.end(), table.begin() + mask * FluidSurface::kCaseStride + 1);
    }
    return table;
}
} // namespace

const std::array<uint32_t, 256 * FluidSurface::kCaseStride>& FluidSurface::caseTable()
{
    static const auto table = buildCases();
    return table;
}

FluidSurface::FluidSurface(Device& device, ShaderLibrary& shaders, const FluidSurfaceDesc& desc) : m_device(device), m_shaders(shaders), m_desc(desc)
{
    for (int a = 0; a < 3; ++a)
    {
        if (desc.nodes[a] == 0 || desc.nodes[a] % kBlock) fail("fluid surface: node grid sides must be positive multiples of 8");
        m_blocks[a] = desc.nodes[a] / kBlock;
    }
    const uint64_t table = uint64_t(m_blocks[0]) * m_blocks[1] * m_blocks[2];
    if (table > 1024u * 1024u) fail("fluid surface: %llu blocks exceed the two-level scan (1M)", (unsigned long long)table);
    if (!(desc.h > 0) || !(desc.scale > 0) || desc.maxParticles == 0 || desc.maxTriangles == 0) fail("fluid surface: invalid description");
    m_tableSize = (uint32_t)table;
    m_maxBlocks = desc.maxBlocks ? std::min<uint32_t>(desc.maxBlocks, m_tableSize) : m_tableSize;
    m_table = makeBuffer(device, uint64_t(m_tableSize) * 4, D3D12_HEAP_TYPE_DEFAULT, L"fluid surface table");
    m_scan = makeBuffer(device, (uint64_t(m_tableSize) + 2048 + m_maxBlocks) * 4, D3D12_HEAP_TYPE_DEFAULT, L"fluid surface scan");
    m_density = makeBuffer(device, uint64_t(m_maxBlocks) * kBlockNodes * 16, D3D12_HEAP_TYPE_DEFAULT, L"fluid surface density");
    m_counters = makeBuffer(device, kCounters * 4, D3D12_HEAP_TYPE_DEFAULT, L"fluid surface counters");
    m_info = makeBuffer(device, uint64_t(m_maxBlocks) * kBlockNodes * 4, D3D12_HEAP_TYPE_DEFAULT, L"fluid surface cells");
    m_blockTris = makeBuffer(device, uint64_t(m_maxBlocks) * 2 * 4, D3D12_HEAP_TYPE_DEFAULT, L"fluid surface block triangles");
    m_vertices = makeBuffer(device, uint64_t(desc.maxTriangles) * 3 * kVertexBytes, D3D12_HEAP_TYPE_DEFAULT, L"fluid surface vertices");
    m_velocities = makeBuffer(device, uint64_t(desc.maxTriangles) * 3 * 16, D3D12_HEAP_TYPE_DEFAULT, L"fluid surface velocities");
    m_cases = makeBuffer(device, sizeof(uint32_t) * 256 * kCaseStride, D3D12_HEAP_TYPE_DEFAULT, L"fluid surface cases");
    m_dispatch = makeBuffer(device, 6 * 4, D3D12_HEAP_TYPE_DEFAULT, L"fluid surface dispatch");
    m_draw = makeBuffer(device, 4 * 4, D3D12_HEAP_TYPE_DEFAULT, L"fluid surface draw");
    m_caseUpload = makeBuffer(device, sizeof(uint32_t) * 256 * kCaseStride, D3D12_HEAP_TYPE_UPLOAD, L"fluid surface cases upload");
    void* mapped = nullptr;
    D3D12_RANGE none{ 0, 0 };
    check(m_caseUpload->Map(0, &none, &mapped), "map fluid surface cases");
    std::memcpy(mapped, caseTable().data(), sizeof(uint32_t) * 256 * kCaseStride);
    m_caseUpload->Unmap(0, nullptr);
    D3D12_INDIRECT_ARGUMENT_DESC arg{};
    arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC sd{};
    sd.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    sd.NumArgumentDescs = 1;
    sd.pArgumentDescs = &arg;
    check(device.d3d()->CreateCommandSignature(&sd, nullptr, IID_PPV_ARGS(&m_signature)), "fluid surface dispatch signature");
}

FluidSurface::~FluidSurface() { m_device.waitIdle(); }

void FluidSurface::bounds(float minimum[3], float maximum[3]) const
{
    for (int a = 0; a < 3; ++a) { minimum[a] = m_desc.origin[a]; maximum[a] = m_desc.origin[a] + float(m_desc.nodes[a]) * m_desc.h; }
}

FluidSurfaceOutput FluidSurface::record(RenderGraph& g, const FluidSurfaceInput& in)
{
    if (in.count > m_desc.maxParticles) fail("fluid surface: %u particles exceed the capacity %u", in.count, m_desc.maxParticles);
    if (in.count && (in.stride < 12 || in.stride % 4)) fail("fluid surface: particle stride must be a multiple of 4, at least 12");
    auto import = [&](ID3D12Resource* r, const char* name) { return g.importBuffer(r, { name, r->GetDesc().Width, 0 }); };
    const BufferRef table = import(m_table.Get(), "fluid table"), scan = import(m_scan.Get(), "fluid scan"), density = import(m_density.Get(), "fluid density"),
                    counters = import(m_counters.Get(), "fluid counters"), info = import(m_info.Get(), "fluid cells"), blockTris = import(m_blockTris.Get(), "fluid block triangles"),
                    vertices = import(m_vertices.Get(), "fluid vertices"), velocities = import(m_velocities.Get(), "fluid velocities"), cases = import(m_cases.Get(), "fluid cases"),
                    dispatch = import(m_dispatch.Get(), "fluid dispatch"),
                    draw = import(m_draw.Get(), "fluid draw");
    if (!m_casesUploaded)
    {
        ID3D12Resource* source = m_caseUpload.Get();
        g.addPass("fluid cases", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(cases, Use::CopyDst); pb.keep(); },
                  [=](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(cases), 0, source, 0, sizeof(uint32_t) * 256 * kCaseStride); });
        m_casesUploaded = true;
    }
    if (in.velocityOffset != UINT32_MAX && (in.velocityOffset % 4 || in.velocityOffset + 12 > in.stride)) fail("fluid surface: the particle velocity must be a float3 inside the element");
    if (in.previousSlotOffset != UINT32_MAX && (in.previousSlotOffset % 4 || in.previousSlotOffset + 4 > in.stride)) fail("fluid surface: the previous slot must be a uint inside the element");
    struct Constants { uint32_t p[8][4]; };
    auto constants = [this, in, table, scan, density, counters, info, blockTris, vertices, velocities, cases, dispatch, draw](const PassContext& c) {
        Constants k{};
        k.p[0][0] = in.count ? c.srv(in.particles) : 0; k.p[0][1] = in.previous.valid() ? c.srv(in.previous) : 0xFFFFFFFFu; k.p[0][2] = in.count; k.p[0][3] = in.stride;
        std::memcpy(&k.p[1][0], &in.alpha, 4); std::memcpy(&k.p[1][1], &m_desc.scale, 4); std::memcpy(&k.p[1][2], &m_desc.h, 4); k.p[1][3] = m_tableSize;
        k.p[2][0] = m_blocks[0]; k.p[2][1] = m_blocks[1]; k.p[2][2] = m_blocks[2]; k.p[2][3] = m_maxBlocks;
        std::memcpy(&k.p[3][0], m_desc.origin, 12); k.p[3][3] = m_desc.maxTriangles;
        k.p[4][0] = c.uav(table); k.p[4][1] = c.uav(scan); k.p[4][2] = c.uav(density); k.p[4][3] = c.uav(counters);
        k.p[5][0] = c.uav(info); k.p[5][1] = c.uav(blockTris); k.p[5][2] = c.uav(vertices); k.p[5][3] = c.srv(cases);
        k.p[6][0] = c.uav(dispatch); k.p[6][1] = c.uav(draw); k.p[6][2] = c.uav(velocities);
        k.p[7][0] = in.velocityOffset; std::memcpy(&k.p[7][1], &in.velocityScale, 4); k.p[7][2] = in.previousSlotOffset;
        c.computeConstants(&k, 32);
    };
    // Every pass declares all the module's buffers it can touch (the constants carry every view).
    auto uses = [&, in](PassBuilder& pb, bool indirect) {
        if (in.count) pb.use(in.particles, Use::SrvCompute);
        if (in.previous.valid()) pb.use(in.previous, Use::SrvCompute);
        for (BufferRef b : { table, scan, density, counters, info, blockTris, vertices, velocities, draw }) pb.use(b, Use::UavCompute);
        pb.use(cases, Use::SrvCompute);
        pb.use(dispatch, indirect ? Use::IndirectArgs : Use::UavCompute);
    };
    auto direct = [&](const char* name, const char* kernel, uint32_t x) {
        ID3D12PipelineState* pso = m_shaders.compute(kernel);
        g.addPass(name, QueueType::Graphics, [&](PassBuilder& pb) { uses(pb, false); },
                  [=](PassContext& c) { c.cmd->SetPipelineState(pso); constants(c); if (x) c.cmd->Dispatch(x, 1, 1); });
    };
    auto indirectPass = [&](const char* name, const char* kernel, uint32_t argument) {
        ID3D12PipelineState* pso = m_shaders.compute(kernel);
        ID3D12CommandSignature* signature = m_signature.Get();
        g.addPass(name, QueueType::Graphics, [&](PassBuilder& pb) { uses(pb, true); },
                  [=](PassContext& c) { c.cmd->SetPipelineState(pso); constants(c); c.cmd->ExecuteIndirect(signature, 1, c.resource(dispatch), argument * 12, nullptr, 0); });
    };
    direct("fluid clear", "Passes/Water/FluidClear", groups(std::max(m_tableSize, kCounters)));
    direct("fluid mark", "Passes/Water/FluidMark", groups(in.count));
    direct("fluid scan 0", "Passes/Water/FluidScan0", groups(m_tableSize, 1024));
    direct("fluid scan 1", "Passes/Water/FluidScan1", 1);
    direct("fluid finish", "Passes/Water/FluidFinish", groups(m_tableSize));
    indirectPass("fluid clear density", "Passes/Water/FluidClearDensity", 0);
    direct("fluid splat", "Passes/Water/FluidSplat", groups(in.count));
    indirectPass("fluid count", "Passes/Water/FluidCount", 1);
    direct("fluid block scan", "Passes/Water/FluidBlockScan", 1);
    indirectPass("fluid emit", "Passes/Water/FluidEmit", 1);
    return { vertices, velocities, draw, counters };
}
} // namespace unx::water
