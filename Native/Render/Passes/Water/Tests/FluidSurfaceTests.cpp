// Track W fluid surface correctness (B8; the first run of new kernels under GpuLock -Kind correctness):
//   1. case table: every case's triangles close into a surface per cell face rule (checked through the meshes below)
//   2. a lattice box (16 x 8 x 16 cells, 8 particles per cell): the GPU mesh equals a CPU reference of the same fixed-
//      point splat and marching cubes (same triangles in the same order, vertices within 1e-5 m), it is closed and
//      consistently oriented (every directed edge's reverse appears exactly once), and its volume is the particles'
//      volume N h^3 within 1 % (the 0.5 level of the B-spline sum lies half a spacing past the last particle row)
//   3. a lattice sphere (radius 10 cells) and a jittered cloud: equal to the reference, closed; sphere volume within 1 %
//   4. blended ticks: the previous tick's buffer shuffled (indexed by each particle's tick-start slot), positions and
//      velocities blended at alpha 0.3: equal to the reference of the blended particles
//   5. isolated particles (spray) produce no surface (peak density 0.42 < 0.5)
//   6. many blocks: 1,280 separate 2 x 2 x 2-cell clusters, one per block of a 256 x 8 x 320-node domain (more than
//      WATER_LINEAR_ROW = 1,024 active blocks, so the per-block and per-node passes run on two-dimensional dispatches of
//      rows): equal to the reference and closed
//   Every mesh also carries vertex velocities (the density-weighted particle velocity), equal to the reference.
//   unx_test_water_fluidsurfacetests [--no-debug-layer] [--warp]
#include "unx/water/FluidSurface.h"

#include "unx/core/File.h"
#include "unx/core/Log.h"

#include <dxgi1_6.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <random>
#include <string>
#include <tuple>
#include <vector>

using namespace unx;
using namespace unx::render;
using unx::water::FluidSurface;

#define W_CHECK(cond, ...)                                                                                            \
    do                                                                                                                \
    {                                                                                                                 \
        if (!(cond)) fail("%s:%d: %s", __FILE__, __LINE__, unx::format(__VA_ARGS__).c_str());                         \
    } while (0)

namespace
{
struct Particle { float x[3]; float J; float v[3]; uint32_t id; float c0[3]; uint32_t origin; float c1[3]; uint32_t flags; float c2[3]; float pad; };  // FluidGpu.hlsl Particle (80 B)
static_assert(sizeof(Particle) == 80);

struct Mesh
{
    std::vector<std::array<float, 6>> vertices;
    std::vector<std::array<float, 3>> velocities;
    std::vector<uint64_t> edges;  // reference only: the grid edge (its two node indices) each vertex lies on
    uint32_t triangles = 0, active = 0, overflow = 0;
};
constexpr float kVelocityScale = 0.05f;  // cells/s -> m/s at dx = 5 cm

// CPU reference of FluidSurface.hlsli: same float arithmetic for the weights, same fixed point, same tables and order.
Mesh reference(const unx::water::FluidSurfaceDesc& d, const std::vector<Particle>& now, const std::vector<Particle>* previous, float alpha)
{
    const int nb[3] = { int(d.nodes[0] / 8), int(d.nodes[1] / 8), int(d.nodes[2] / 8) };
    auto blockIndex = [&](int x, int y, int z) { return (z * nb[1] + y) * nb[0] + x; };
    auto position = [&](size_t i, int a) {
        float x = now[i].x[a];
        if (previous) { const float p = (*previous)[now[i].origin].x[a]; x = p + (x - p) * alpha; }
        return x * d.scale;
    };
    std::vector<char> active(size_t(nb[0]) * nb[1] * nb[2], 0);
    for (size_t i = 0; i < now.size(); ++i)
    {
        int lo[3], hi[3];
        for (int a = 0; a < 3; ++a)
        {
            const int base = (int)std::floor(position(i, a) - 0.5f);
            lo[a] = std::clamp((base - 2) >> 3, 0, nb[a] - 1);
            hi[a] = std::clamp((base + 4) >> 3, 0, nb[a] - 1);
        }
        for (int z = lo[2]; z <= hi[2]; ++z) for (int y = lo[1]; y <= hi[1]; ++y) for (int x = lo[0]; x <= hi[0]; ++x) active[blockIndex(x, y, z)] = 1;
    }
    std::vector<int> slotOf(active.size(), -1), blockOf;
    for (size_t b = 0; b < active.size(); ++b) if (active[b]) { slotOf[b] = (int)blockOf.size(); blockOf.push_back((int)b); }
    std::vector<uint32_t> density(blockOf.size() * 512, 0);
    std::vector<std::array<int32_t, 3>> momentum(blockOf.size() * 512, std::array<int32_t, 3>{});
    auto velocity = [&](size_t i, int a) {
        float v = now[i].v[a];
        if (previous) { const float p = (*previous)[now[i].origin].v[a]; v = p + (v - p) * alpha; }
        return v * kVelocityScale;
    };
    auto node = [&](int x, int y, int z) -> uint32_t* {
        const int bx = x >> 3, by = y >> 3, bz = z >> 3;
        if (bx < 0 || by < 0 || bz < 0 || bx >= nb[0] || by >= nb[1] || bz >= nb[2]) return nullptr;
        const int s = slotOf[blockIndex(bx, by, bz)];
        if (s < 0) return nullptr;
        return &density[size_t(s) * 512 + ((z - bz * 8) * 8 + (y - by * 8)) * 8 + (x - bx * 8)];
    };
    for (size_t i = 0; i < now.size(); ++i)
    {
        float q[3], f[3], w[3][3];
        int base[3];
        for (int a = 0; a < 3; ++a)
        {
            q[a] = position(i, a);
            base[a] = (int)std::floor(q[a] - 0.5f);
            f[a] = q[a] - (float)base[a];
            w[a][0] = 0.5f * (1.5f - f[a]) * (1.5f - f[a]);
            w[a][1] = 0.75f - (f[a] - 1.0f) * (f[a] - 1.0f);
            w[a][2] = 0.5f * (f[a] - 0.5f) * (f[a] - 0.5f);
        }
        const float vel[3] = { velocity(i, 0), velocity(i, 1), velocity(i, 2) };
        for (int z = 0; z < 3; ++z) for (int y = 0; y < 3; ++y) for (int x = 0; x < 3; ++x)
            if (uint32_t* n = node(base[0] + x, base[1] + y, base[2] + z))
            {
                const float weight = w[0][x] * w[1][y] * w[2][z];
                *n += (uint32_t)std::lround(weight * 1048576.0f);
                auto& m = momentum[size_t(n - density.data())];
                for (int a = 0; a < 3; ++a) m[a] += (int32_t)std::lround(weight * vel[a] * 65536.0f);
            }
    }
    auto at = [&](int x, int y, int z) { uint32_t* n = node(x, y, z); return n ? *n : 0u; };
    auto momentumAt = [&](int x, int y, int z, int a) { uint32_t* n = node(x, y, z); return n ? (float)momentum[size_t(n - density.data())][a] / 65536.0f : 0.0f; };
    const auto& cases = FluidSurface::caseTable();
    Mesh m;
    m.active = (uint32_t)blockOf.size();
    for (int b : blockOf)
    {
        const int bx = b % nb[0], by = (b / nb[0]) % nb[1], bz = b / (nb[0] * nb[1]);
        for (int t = 0; t < 512; ++t)
        {
            const int c[3] = { bx * 8 + t % 8, by * 8 + (t / 8) % 8, bz * 8 + t / 64 };
            uint32_t mask = 0;
            for (uint32_t k = 0; k < 8; ++k) if (at(c[0] + int(k & 1), c[1] + int((k >> 1) & 1), c[2] + int(k >> 2)) >= 524288u) mask |= 1u << k;
            const uint32_t count = cases[mask * FluidSurface::kCaseStride];
            auto value = [&](int x, int y, int z) { return (float)at(x, y, z) / 1048576.0f; };
            for (uint32_t k = 0; k < count; ++k)
            {
                ++m.triangles;
                for (uint32_t v = 0; v < 3; ++v)
                {
                    const uint32_t e = cases[mask * FluidSurface::kCaseStride + 1 + 3 * k + v];
                    const uint32_t ca = FluidSurface::kEdges[e][0], cb = FluidSurface::kEdges[e][1];
                    const int pa[3] = { c[0] + int(ca & 1), c[1] + int((ca >> 1) & 1), c[2] + int(ca >> 2) }, pb[3] = { c[0] + int(cb & 1), c[1] + int((cb >> 1) & 1), c[2] + int(cb >> 2) };
                    const float da = value(pa[0], pa[1], pa[2]), db = value(pb[0], pb[1], pb[2]);
                    const float s = std::clamp((0.5f - da) / (db - da), 0.0f, 1.0f);
                    std::array<float, 6> vertex{};
                    for (int a = 0; a < 3; ++a) vertex[a] = d.origin[a] + ((float)c[a] + ((float)(pa[a] - c[a]) + ((float)(pb[a] - c[a]) - (float)(pa[a] - c[a])) * s)) * d.h;
                    m.vertices.push_back(vertex);
                    auto nodeId = [&](const int* q) { return uint64_t((uint32_t(q[2]) * d.nodes[1] + uint32_t(q[1])) * d.nodes[0] + uint32_t(q[0])); };
                    m.edges.push_back(std::min(nodeId(pa), nodeId(pb)) << 32 | std::max(nodeId(pa), nodeId(pb)));
                    const float mass = da + (db - da) * s;
                    std::array<float, 3> vv{};
                    for (int a = 0; a < 3; ++a) { const float ma = momentumAt(pa[0], pa[1], pa[2], a), mb = momentumAt(pb[0], pb[1], pb[2], a); vv[a] = mass > 0 ? (ma + (mb - ma) * s) / mass : 0.0f; }
                    m.velocities.push_back(vv);
                }
            }
        }
    }
    return m;
}

ComPtr<ID3D12Device> warpDevice()
{
    ComPtr<IDXGIFactory4> factory;
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "DXGI factory");
    ComPtr<IDXGIAdapter> adapter;
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP adapter");
    ComPtr<ID3D12Device> device;
    check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device)), "WARP device");
    return device;
}
struct Gpu
{
    ComPtr<ID3D12Device> external;
    Device device;
    ShaderLibrary shaders;
    Gpu(bool debugLayer, bool warp)
        : external(warp ? warpDevice() : nullptr), device([&] {
              DeviceOptions o;
              o.debugLayer = debugLayer && !warp;
              o.externalDevice = external.Get();
              return o;
          }()),
          shaders(device, executableDirectory() / "shaders") {}
};

ComPtr<ID3D12Resource> buffer(Device& device, uint64_t bytes, D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES heap{ type };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = std::max<uint64_t>(bytes, 256);
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)), "test buffer");
    return r;
}

Mesh run(Gpu& gpu, FluidSurface& surface, const std::vector<Particle>& now, const std::vector<Particle>* previous, float alpha)
{
    const uint64_t bytes = std::max<size_t>(now.size(), 1) * sizeof(Particle);
    auto upload = buffer(gpu.device, bytes * (previous ? 2 : 1), D3D12_HEAP_TYPE_UPLOAD);
    uint8_t* mapped = nullptr;
    check(upload->Map(0, nullptr, (void**)&mapped), "map particles");
    std::memcpy(mapped, now.data(), now.size() * sizeof(Particle));
    if (previous) std::memcpy(mapped + bytes, previous->data(), previous->size() * sizeof(Particle));
    upload->Unmap(0, nullptr);
    const auto& d = surface.desc();
    const uint64_t vertexBytes = uint64_t(d.maxTriangles) * 3 * FluidSurface::kVertexBytes;
    auto readVertices = buffer(gpu.device, vertexBytes, D3D12_HEAP_TYPE_READBACK), readSmall = buffer(gpu.device, 256, D3D12_HEAP_TYPE_READBACK);
    const uint64_t velocityBytes = uint64_t(d.maxTriangles) * 3 * 16;
    auto readVelocities = buffer(gpu.device, velocityBytes, D3D12_HEAP_TYPE_READBACK);
    RenderGraph g(gpu.device);
    const BufferRef particles = g.createBuffer({ "particles", bytes, 0 });
    const BufferRef old = previous ? g.createBuffer({ "previous particles", bytes, 0 }) : BufferRef{};
    ID3D12Resource* source = upload.Get();
    g.addPass("upload particles", QueueType::Graphics,
              [&](PassBuilder& pb) { pb.use(particles, Use::CopyDst); if (old.valid()) pb.use(old, Use::CopyDst); },
              [=](PassContext& c) {
                  c.cmd->CopyBufferRegion(c.resource(particles), 0, source, 0, bytes);
                  if (old.valid()) c.cmd->CopyBufferRegion(c.resource(old), 0, source, bytes, bytes);
              });
    unx::water::FluidSurfaceInput in;
    in.particles = particles; in.previous = old; in.count = (uint32_t)now.size(); in.stride = sizeof(Particle); in.alpha = alpha;
    in.velocityOffset = 16; in.velocityScale = kVelocityScale; in.previousSlotOffset = previous ? 44 : UINT32_MAX;
    const auto out = surface.record(g, in);
    ID3D12Resource* rv = readVertices.Get();
    ID3D12Resource* rs = readSmall.Get();
    ID3D12Resource* rvel = readVelocities.Get();
    g.addPass("read fluid surface", QueueType::Graphics,
              [&](PassBuilder& pb) { pb.use(out.vertices, Use::CopySrc); pb.use(out.velocities, Use::CopySrc); pb.use(out.draw, Use::CopySrc); pb.use(out.counters, Use::CopySrc); pb.keep(); },
              [=](PassContext& c) {
                  c.cmd->CopyBufferRegion(rv, 0, c.resource(out.vertices), 0, vertexBytes);
                  c.cmd->CopyBufferRegion(rvel, 0, c.resource(out.velocities), 0, velocityBytes);
                  c.cmd->CopyBufferRegion(rs, 0, c.resource(out.draw), 0, 16);
                  c.cmd->CopyBufferRegion(rs, 64, c.resource(out.counters), 0, 64);
              });
    g.execute(nullptr);
    for (uint32_t q = 0; q < kQueueTypeCount; ++q) gpu.device.queue((QueueType)q).waitCpu(g.lastFence((QueueType)q));
    Mesh m;
    const uint32_t* words = nullptr;
    check(readSmall->Map(0, nullptr, (void**)&words), "map counters");
    m.active = words[16]; m.triangles = words[17]; m.overflow = words[18];
    const uint32_t drawn = words[0];
    readSmall->Unmap(0, nullptr);
    W_CHECK(drawn == 3 * std::min(m.triangles, d.maxTriangles), "draw arguments %u for %u triangles", drawn, m.triangles);
    const float* v = nullptr;
    check(readVertices->Map(0, nullptr, (void**)&v), "map vertices");
    for (uint32_t i = 0; i < std::min(m.triangles, d.maxTriangles) * 3; ++i) m.vertices.push_back({ v[8 * i], v[8 * i + 1], v[8 * i + 2], v[8 * i + 4], v[8 * i + 5], v[8 * i + 6] });
    readVertices->Unmap(0, nullptr);
    check(readVelocities->Map(0, nullptr, (void**)&v), "map velocities");
    for (uint32_t i = 0; i < std::min(m.triangles, d.maxTriangles) * 3; ++i) m.velocities.push_back({ v[4 * i], v[4 * i + 1], v[4 * i + 2] });
    readVelocities->Unmap(0, nullptr);
    return m;
}

// Closed and consistently oriented: every directed edge's reverse appears exactly once. Vertices are identified by the
// grid edge they lie on (the reference's, equal to the GPU mesh triangle by triangle: compare()), not by position: a
// crossing a few 1e-5 of a spacing from a node rounds to the node's float position, so vertices on different grid edges
// can coincide geometrically (a zero-area triangle, no crack) while the topology stays that of the edge graph.
void checkClosed(const Mesh& gpu, const Mesh& ref, const char* what)
{
    W_CHECK(gpu.vertices.size() == ref.edges.size(), "%s: %zu GPU vertices, %zu reference", what, gpu.vertices.size(), ref.edges.size());
    std::map<std::pair<uint64_t, uint64_t>, int> edges;
    uint32_t collapsed = 0;
    for (size_t t = 0; t + 2 < ref.edges.size(); t += 3)
    {
        const uint64_t a = ref.edges[t], b = ref.edges[t + 1], c = ref.edges[t + 2];
        W_CHECK(a != b && b != c && a != c, "%s: triangle %zu uses a grid edge twice", what, t / 3);
        ++edges[{ a, b }]; ++edges[{ b, c }]; ++edges[{ c, a }];
        auto same = [&](size_t i, size_t j) { return gpu.vertices[i][0] == gpu.vertices[j][0] && gpu.vertices[i][1] == gpu.vertices[j][1] && gpu.vertices[i][2] == gpu.vertices[j][2]; };
        if (same(t, t + 1) || same(t + 1, t + 2) || same(t, t + 2)) ++collapsed;
    }
    uint32_t open = 0, repeated = 0;
    for (const auto& [e, n] : edges)
    {
        if (n > 1) ++repeated;
        auto r = edges.find({ e.second, e.first });
        if (r == edges.end() || r->second != n) ++open;
    }
    W_CHECK(open == 0 && repeated == 0, "%s: %u open and %u repeated directed edges (of %zu)", what, open, repeated, edges.size());
    std::printf("%s: closed (%zu directed edges; %u zero-area triangles from crossings at a node's float position)\n", what, edges.size(), collapsed);
}
double volume(const Mesh& m)
{
    double v = 0;
    for (size_t t = 0; t + 2 < m.vertices.size(); t += 3)
    {
        const auto& a = m.vertices[t]; const auto& b = m.vertices[t + 1]; const auto& c = m.vertices[t + 2];
        v += (double(a[0]) * (double(b[1]) * c[2] - double(b[2]) * c[1]) - double(a[1]) * (double(b[0]) * c[2] - double(b[2]) * c[0]) + double(a[2]) * (double(b[0]) * c[1] - double(b[1]) * c[0])) / 6;
    }
    return v;
}
void compare(const Mesh& gpu, const Mesh& ref, const char* what)
{
    W_CHECK(gpu.overflow == 0, "%s: overflow %u", what, gpu.overflow);
    W_CHECK(gpu.active == ref.active && gpu.triangles == ref.triangles, "%s: GPU %u blocks / %u triangles, reference %u / %u", what, gpu.active, gpu.triangles, ref.active, ref.triangles);
    double worst = 0, normals = 0;
    for (size_t i = 0; i < ref.vertices.size(); ++i)
    {
        for (int a = 0; a < 3; ++a) worst = std::max(worst, (double)std::abs(gpu.vertices[i][a] - ref.vertices[i][a]));
        normals = std::max(normals, std::abs(1.0 - std::sqrt(double(gpu.vertices[i][3]) * gpu.vertices[i][3] + double(gpu.vertices[i][4]) * gpu.vertices[i][4] + double(gpu.vertices[i][5]) * gpu.vertices[i][5])));
    }
    W_CHECK(worst <= 1e-5, "%s: vertex differs from the reference by %.3g m", what, worst);
    // Vertex velocities: the node momenta are fixed-point sums (2^-16 m/s per unit) of up to 27 particle contributions,
    // each rounded; the GPU's float product (fused or not) may land on the other side of a rounding boundary, so a node
    // may differ by up to 27 units, and the vertex (density 0.5, a blend of two nodes) by 27 / 2^16 / 0.5 = 8.2e-4 m/s
    // (1.4 um of motion over a 165 Hz frame).
    double velocityError = 0;
    for (size_t i = 0; i < ref.velocities.size(); ++i)
        for (int a = 0; a < 3; ++a) velocityError = std::max(velocityError, (double)std::abs(gpu.velocities[i][a] - ref.velocities[i][a]));
    W_CHECK(velocityError <= 27.0 / 65536.0 / 0.5, "%s: vertex velocity differs from the reference by %.3g m/s", what, velocityError);
    std::printf("%s: vertex error %.3g m, velocity error %.3g m/s\n", what, worst, velocityError);
    W_CHECK(normals <= 1e-4, "%s: normal length off by %.3g", what, normals);
}
std::vector<Particle> lattice(int x0, int y0, int z0, int x1, int y1, int z1, const std::function<bool(float, float, float)>& keep)
{
    std::vector<Particle> p;
    for (int z = z0; z < z1; ++z) for (int y = y0; y < y1; ++y) for (int x = x0; x < x1; ++x)
        for (int k = 0; k < 8; ++k)
        {
            Particle q{};
            q.x[0] = x + ((k & 1) ? .75f : .25f); q.x[1] = y + ((k & 2) ? .75f : .25f); q.x[2] = z + ((k & 4) ? .75f : .25f);
            q.v[0] = 0.3f * (q.x[2] - 16) + 2; q.v[1] = -1; q.v[2] = -0.3f * (q.x[0] - 16);  // cells/s
            if (keep(q.x[0], q.x[1], q.x[2])) p.push_back(q);
        }
    return p;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        bool debugLayer = true, warp = false;
        for (int i = 1; i < argc; ++i)
        {
            if (std::string(argv[i]) == "--no-debug-layer") debugLayer = false;
            if (std::string(argv[i]) == "--warp") warp = true;
        }
        Gpu gpu(debugLayer, warp);
        unx::water::FluidSurfaceDesc desc;
        desc.nodes[0] = desc.nodes[1] = desc.nodes[2] = 64;  // a 32^3-cell fluid domain at dx = 5 cm (h = 2.5 cm)
        desc.scale = 2; desc.h = 0.025f; desc.origin[0] = 10; desc.origin[1] = -2; desc.origin[2] = 3;
        desc.maxParticles = 300000; desc.maxTriangles = 400000;
        FluidSurface surface(gpu.device, gpu.shaders, desc);
        uint32_t maxCase = 0;
        for (uint32_t c = 0; c < 256; ++c) maxCase = std::max(maxCase, FluidSurface::caseTable()[c * FluidSurface::kCaseStride]);
        auto all = [](float, float, float) { return true; };
        // 2. box
        const auto box = lattice(4, 4, 4, 20, 12, 20, all);
        const auto boxMesh = run(gpu, surface, box, nullptr, 1), boxRef = reference(desc, box, nullptr, 1);
        compare(boxMesh, boxRef, "box");
        checkClosed(boxMesh, boxRef, "box");
        const double h3 = double(desc.h) * desc.h * desc.h, boxVolume = volume(boxMesh), boxExpected = box.size() * h3;
        W_CHECK(std::abs(boxVolume / boxExpected - 1) <= 0.01, "box volume %.6g m3, particles %.6g m3", boxVolume, boxExpected);
        // 3. sphere and jittered cloud
        auto inSphere = [](float x, float y, float z) { const float dx = x - 16, dy = y - 16, dz = z - 16; return dx * dx + dy * dy + dz * dz <= 100; };
        const auto sphere = lattice(4, 4, 4, 28, 28, 28, inSphere);
        const auto sphereMesh = run(gpu, surface, sphere, nullptr, 1);
        const auto sphereRef = reference(desc, sphere, nullptr, 1);
        compare(sphereMesh, sphereRef, "sphere");
        checkClosed(sphereMesh, sphereRef, "sphere");
        const double sphereVolume = volume(sphereMesh), sphereExpected = sphere.size() * h3;
        W_CHECK(std::abs(sphereVolume / sphereExpected - 1) <= 0.01, "sphere volume %.6g m3, particles %.6g m3", sphereVolume, sphereExpected);
        std::mt19937 rng(7);
        std::uniform_real_distribution<float> jitter(-0.2f, 0.2f);
        auto cloud = lattice(6, 6, 6, 26, 18, 26, all);
        for (auto& p : cloud) for (float& x : p.x) x += jitter(rng);
        const auto cloudMesh = run(gpu, surface, cloud, nullptr, 1);
        const auto cloudRef = reference(desc, cloud, nullptr, 1);
        compare(cloudMesh, cloudRef, "cloud");
        checkClosed(cloudMesh, cloudRef, "cloud");
        // 4. blended ticks
        // The previous tick's buffer in another order (the physics fluid re-sorts at the tick start): each current particle
        // names its previous slot.
        std::vector<uint32_t> order(cloud.size());
        for (uint32_t i = 0; i < order.size(); ++i) order[i] = i;
        std::shuffle(order.begin(), order.end(), rng);
        std::vector<Particle> earlier(cloud.size()), moved = cloud;
        for (uint32_t i = 0; i < order.size(); ++i) { earlier[order[i]] = cloud[i]; moved[i].origin = order[i]; moved[i].x[0] += 0.8f; moved[i].x[1] -= 0.3f; moved[i].v[1] += 2; }
        const auto blendMesh = run(gpu, surface, moved, &earlier, 0.3f);
        const auto blendRef = reference(desc, moved, &earlier, 0.3f);
        compare(blendMesh, blendRef, "blend");
        checkClosed(blendMesh, blendRef, "blend");
        // 5. spray
        std::vector<Particle> spray;
        for (int i = 0; i < 20; ++i) { Particle p{}; p.x[0] = 4.0f + 1.2f * i; p.x[1] = 20; p.x[2] = 8; spray.push_back(p); }
        const auto sprayMesh = run(gpu, surface, spray, nullptr, 1);
        W_CHECK(sprayMesh.triangles == 0, "isolated particles made %u triangles", sprayMesh.triangles);
        // 6. many blocks
        unx::water::FluidSurfaceDesc wide = desc;
        wide.nodes[0] = 256; wide.nodes[1] = 8; wide.nodes[2] = 320;
        wide.maxParticles = 100000; wide.maxTriangles = 400000;
        FluidSurface wideSurface(gpu.device, gpu.shaders, wide);
        std::vector<Particle> clusters;
        for (int bz = 0; bz < 40; ++bz)
            for (int bx = 0; bx < 32; ++bx)
            {
                const auto c = lattice(4 * bx + 1, 1, 4 * bz + 1, 4 * bx + 3, 3, 4 * bz + 3, all);
                clusters.insert(clusters.end(), c.begin(), c.end());
            }
        const auto wideMesh = run(gpu, wideSurface, clusters, nullptr, 1), wideRef = reference(wide, clusters, nullptr, 1);
        compare(wideMesh, wideRef, "many blocks");
        checkClosed(wideMesh, wideRef, "many blocks");
        std::printf("many blocks: 1280 clusters, %zu particles -> %u triangles, equal to the reference and closed\n", clusters.size(), wideMesh.triangles);
        if (!warp)
        {
            const uint32_t errors = gpu.device.drainDebugMessages();
            W_CHECK(errors == 0, "%u debug-layer errors", errors);
        }
        std::printf("fluid surface: case table max %u triangles; box %zu particles -> %u triangles, volume %.4f %% off; sphere %zu -> %u, %.4f %%; cloud %zu -> %u; blend -> %u; "
                    "spray 0; all equal to the reference and closed\n",
                    maxCase, box.size(), boxMesh.triangles, 100 * (boxVolume / boxExpected - 1), sphere.size(), sphereMesh.triangles,
                    100 * (sphereVolume / sphereExpected - 1), cloud.size(), cloudMesh.triangles, blendMesh.triangles);
        std::printf("fluid surface tests passed\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
}
