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
//   7. retired tail: after every record, every vertex past the drawn triangles up to the capacity has a NaN position
//      (FluidTail.hlsl: a ray tracing build over the capacity sees them inactive); the cases run in an order where the
//      triangle count both grows and shrinks (box -> sphere -> cloud -> blend -> spray 0)
//   8. axes: the box with the output mirrored in z (the Unity host's World -> the renderer, FrameContext::streamAxes)
//      equals the reference with z negated in positions, normals and velocities and each triangle's last two vertices
//      swapped (the outside stays counter-clockwise), and it is closed with the mirrored orientation
//   9. W3 seam (engine 2): basins cut the surface exactly (see the case)
//   10. the node field's low-pass (FluidSmooth.hlsl; every case above runs with it, against the reference with it):
//      a. a jittered slab (24 x 8 x 24 cells, 8 particles per cell, each moved up to half a spacing at random, as a
//         settled MPM fluid's are): the top face's height RMS (its interior) at most 0.6 of the unfiltered one's, the
//         volume within 1 % of the particles' (the unfiltered surface's noise costs volume: -1.3 %), equal to the
//         reference; on the noise-free box and sphere the filter changes the volume by at most 0.2 %;
//      b. a crest (the top on the particle rows at 2 h sin(2 pi x / 8 h)): the fitted amplitude within 3 % of the
//         unfiltered surface's;
//      c. jets along x 2 and 4 spacings thick (2 x 2 and 4 x 4 particles across): the middle's mean radius within 5 % of
//         the unfiltered one's, the thin jet drawn over its whole length
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
    // The node field's low-pass (FluidSmooth.hlsl): [1, -6, 15, 44, 15, -6, 1] / 64 along x, y, z in integers (floor of
    // (sum + 32) / 64), inactive nodes read 0 and are not written, the density clamped at 0 after the last axis.
    if (d.smooth)
        for (int axis = 0; axis < 3; ++axis)
        {
            std::vector<std::array<int32_t, 4>> in(density.size()), out(density.size());
            for (size_t k = 0; k < density.size(); ++k) in[k] = { int32_t(density[k]), momentum[k][0], momentum[k][1], momentum[k][2] };
            auto read = [&](int x, int y, int z) -> std::array<int32_t, 4> { uint32_t* n = node(x, y, z); return n ? in[size_t(n - density.data())] : std::array<int32_t, 4>{}; };
            static const int32_t taps[7] = { 1, -6, 15, 44, 15, -6, 1 };
            for (size_t s = 0; s < blockOf.size(); ++s)
            {
                const int b = blockOf[s], bx = b % nb[0], by = (b / nb[0]) % nb[1], bz = b / (nb[0] * nb[1]);
                for (int t = 0; t < 512; ++t)
                {
                    const int j[3] = { bx * 8 + t % 8, by * 8 + (t / 8) % 8, bz * 8 + t / 64 };
                    std::array<int32_t, 4> sum{};
                    for (int k = -3; k <= 3; ++k)
                    {
                        const auto v = read(j[0] + (axis == 0 ? k : 0), j[1] + (axis == 1 ? k : 0), j[2] + (axis == 2 ? k : 0));
                        for (int c = 0; c < 4; ++c) sum[c] += taps[k + 3] * v[c];
                    }
                    for (int c = 0; c < 4; ++c) out[s * 512 + t][c] = (sum[c] + 32) >> 6;
                    if (axis == 2) out[s * 512 + t][0] = std::max(out[s * 512 + t][0], 0);
                }
            }
            for (size_t k = 0; k < density.size(); ++k) { density[k] = uint32_t(out[k][0]); momentum[k] = { out[k][1], out[k][2], out[k][3] }; }
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

// W3 seam: a basin whose water (level + eta, eta = tilt (lx - Lx / 2): a tilted surface, exact under bilinear sampling)
// cuts the fluid surface; renderer axes.
struct SeamBasin { float centre[3]; float yaw, sizeX, sizeZ, tilt; };
double seamAbove(const SeamBasin& b, double x, double y, double z)
{
    const double c = std::cos(double(b.yaw)), s = std::sin(double(b.yaw)), dx = x - b.centre[0], dz = z - b.centre[2];
    const double lx = dx * c - dz * s + 0.5 * b.sizeX, lz = dx * s + dz * c + 0.5 * b.sizeZ;
    if (lx < 0 || lz < 0 || lx > b.sizeX || lz > b.sizeZ) return 1e30;
    return y - (b.centre[1] + b.tilt * (lx - 0.5 * b.sizeX));
}
Mesh run(Gpu& gpu, FluidSurface& surface, const std::vector<Particle>& now, const std::vector<Particle>* previous, float alpha, const float* axes = nullptr,
         const SeamBasin* seam = nullptr)
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
    if (axes) for (int a = 0; a < 3; ++a) in.axes[a] = axes[a];
    ComPtr<ID3D12Resource> fieldUpload;
    if (seam)
    {
        // the pool field (RGBA32F 257^2, eta in .x at local (i Lx / 256, j Lz / 256)), uploaded by a copy
        constexpr uint32_t kSide = 257, kPitch = (kSide * 16 + 255) / 256 * 256;
        fieldUpload = buffer(gpu.device, uint64_t(kPitch) * kSide, D3D12_HEAP_TYPE_UPLOAD);
        uint8_t* f = nullptr;
        check(fieldUpload->Map(0, nullptr, (void**)&f), "map seam field");
        for (uint32_t j = 0; j < kSide; ++j)
            for (uint32_t i = 0; i < kSide; ++i)
            {
                const float e[4] = { seam->tilt * (float(i) * seam->sizeX / 256.0f - 0.5f * seam->sizeX), 0, 0, 0 };
                std::memcpy(f + uint64_t(j) * kPitch + uint64_t(i) * 16, e, 16);
            }
        fieldUpload->Unmap(0, nullptr);
        const TextureRef field = g.createTexture(TextureDesc{ "seam field", kSide, kSide, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT });
        ID3D12Resource* fu = fieldUpload.Get();
        g.addPass("upload seam field", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(field, Use::CopyDst); },
                  [=](PassContext& c) {
                      D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
                      dst.pResource = c.resource(field); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0;
                      src.pResource = fu; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                      src.PlacedFootprint.Footprint = { DXGI_FORMAT_R32G32B32A32_FLOAT, kSide, kSide, 1, kPitch };
                      c.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                  });
        unx::water::FluidSurfaceInput::Basin b;
        for (int a = 0; a < 3; ++a) b.centre[a] = seam->centre[a];
        b.cosYaw = std::cos(seam->yaw); b.sinYaw = std::sin(seam->yaw); b.sizeX = seam->sizeX; b.sizeZ = seam->sizeZ; b.field = field;
        in.basins.push_back(b);
    }
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
    uint32_t live = 0;
    for (uint64_t i = uint64_t(std::min(m.triangles, d.maxTriangles)) * 3; i < uint64_t(d.maxTriangles) * 3; ++i)
        if (!(std::isnan(v[8 * i]) && std::isnan(v[8 * i + 1]) && std::isnan(v[8 * i + 2]))) ++live;
    readVertices->Unmap(0, nullptr);
    W_CHECK(live == 0, "%u vertices past the %u drawn triangles (capacity %u) are not retired", live, m.triangles, d.maxTriangles);
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
        unx::water::FluidSurfaceDesc rawDesc = desc;  // the particles' own density, unfiltered (the reconstruction's baseline)
        rawDesc.smooth = false;
        FluidSurface rawSurface(gpu.device, gpu.shaders, rawDesc);
        uint32_t maxCase = 0;
        for (uint32_t c = 0; c < 256; ++c) maxCase = std::max(maxCase, FluidSurface::caseTable()[c * FluidSurface::kCaseStride]);
        auto all = [](float, float, float) { return true; };
        // 2. box
        const auto box = lattice(4, 4, 4, 20, 12, 20, all);
        const auto boxMesh = run(gpu, surface, box, nullptr, 1), boxRef = reference(desc, box, nullptr, 1);
        compare(boxMesh, boxRef, "box");
        checkClosed(boxMesh, boxRef, "box");
        const double h3 = double(desc.h) * desc.h * desc.h, boxVolume = volume(boxMesh), boxExpected = box.size() * h3;
        // The reconstruction's own volume (the 0.5 level of the unfiltered sum rounds the box's edges at the spacing:
        // -0.94 %), and the low-pass's change of it (FluidSmooth.hlsl: flat faces exact, shrinkage 4th order in h).
        const double boxRawVolume = volume(run(gpu, rawSurface, box, nullptr, 1));
        W_CHECK(std::abs(boxRawVolume / boxExpected - 1) <= 0.01, "box volume (unfiltered) %.6g m3, particles %.6g m3", boxRawVolume, boxExpected);
        W_CHECK(std::abs(boxVolume / boxRawVolume - 1) <= 0.002, "box volume %.6g m3, unfiltered %.6g m3", boxVolume, boxRawVolume);
        // 9. W3 seam (engine 2): a basin far from the fluid changes nothing (bit identical); a tilted bath surface through
        // the box cuts it exactly - no vertex of a drawn triangle under the water, and the kept area equals the unclipped
        // mesh clipped on the CPU in double against the same surface
        {
            double lo[3] = { 1e30, 1e30, 1e30 }, hi[3] = { -1e30, -1e30, -1e30 };
            for (const auto& v : boxMesh.vertices) for (int a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], double(v[a])); hi[a] = std::max(hi[a], double(v[a])); }
            const SeamBasin away{ { float(lo[0] + 100), float(0.5 * (lo[1] + hi[1])), float(lo[2]) }, 0.3f, 2, 2, 0.05f };
            const auto farMesh = run(gpu, surface, box, nullptr, 1, nullptr, &away);
            bool same = farMesh.triangles == boxMesh.triangles && farMesh.vertices.size() == boxMesh.vertices.size();
            for (size_t i = 0; same && i < farMesh.vertices.size(); ++i) same = std::memcmp(&farMesh.vertices[i], &boxMesh.vertices[i], sizeof(farMesh.vertices[i])) == 0;
            W_CHECK(same, "seam: a basin away from the fluid changed its surface");
            // a gentle surface and a steep one (|eta| to ~0.4 m: the band is the field's measured max |eta|, FluidBasin.hlsl)
            for (const float tilt : { 0.05f, 0.5f })
            {
                const SeamBasin cut{ { float(0.5 * (lo[0] + hi[0])), float(0.5 * (lo[1] + hi[1])), float(0.5 * (lo[2] + hi[2])) }, 0.3f,
                                     float(2 * (hi[0] - lo[0]) + 0.4), float(2 * (hi[2] - lo[2]) + 0.4), tilt };
                const auto cutMesh = run(gpu, surface, box, nullptr, 1, nullptr, &cut);
                auto area = [](const std::array<double, 3>& a, const std::array<double, 3>& b, const std::array<double, 3>& c) {
                    const double u[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, w[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
                    const double x = u[1] * w[2] - u[2] * w[1], y = u[2] * w[0] - u[0] * w[2], z = u[0] * w[1] - u[1] * w[0];
                    return 0.5 * std::sqrt(x * x + y * y + z * z);
                };
                double worstBelow = 0, kept = 0;
                uint32_t drawn = 0;
                for (size_t t = 0; t + 2 < cutMesh.vertices.size(); t += 3)
                {
                    std::array<double, 3> p[3];
                    for (int v = 0; v < 3; ++v) for (int a = 0; a < 3; ++a) p[v][a] = cutMesh.vertices[t + v][a];
                    const double A = area(p[0], p[1], p[2]);
                    if (A <= 1e-12) continue;
                    ++drawn;
                    kept += A;
                    for (int v = 0; v < 3; ++v) worstBelow = std::max(worstBelow, -seamAbove(cut, p[v][0], p[v][1], p[v][2]));
                }
                // reference: the unclipped box surface, each triangle clipped against the surface (f linear: exact interpolation)
                double reference = 0;
                for (size_t t = 0; t + 2 < boxMesh.vertices.size(); t += 3)
                {
                    std::vector<std::array<double, 3>> poly;
                    for (int v = 0; v < 3; ++v)
                    {
                        std::array<double, 3> a, b;
                        for (int k = 0; k < 3; ++k) { a[k] = boxMesh.vertices[t + v][k]; b[k] = boxMesh.vertices[t + (v + 1) % 3][k]; }
                        const double fa = seamAbove(cut, a[0], a[1], a[2]), fb = seamAbove(cut, b[0], b[1], b[2]);
                        if (fa >= 0) poly.push_back(a);
                        if ((fa >= 0) != (fb >= 0)) { const double s = fa / (fa - fb); poly.push_back({ a[0] + s * (b[0] - a[0]), a[1] + s * (b[1] - a[1]), a[2] + s * (b[2] - a[2]) }); }
                    }
                    for (size_t k = 1; k + 1 < poly.size(); ++k) reference += area(poly[0], poly[k], poly[k + 1]);
                }
                std::printf("seam: far basin bit identical (%u triangles); cut (tilt %.2f, max |eta| %.3f m): %u drawn of %u slots, deepest drawn vertex %.3g m under the water, kept area %.6f m2 vs clipped reference %.6f (rel %.2e)\n",
                            boxMesh.triangles, tilt, 0.5 * tilt * cut.sizeX, drawn, cutMesh.triangles, worstBelow, kept, reference, std::abs(kept / reference - 1));
                W_CHECK(worstBelow <= 2e-5, "seam: a drawn vertex %.3g m under the water", worstBelow);
                W_CHECK(std::abs(kept / reference - 1) <= 1e-4, "seam: kept area %.6f vs reference %.6f", kept, reference);
            }
        }
        // 8. axes: mirrored in z
        {
            const float mirror[3] = { 1, 1, -1 };
            const auto m = run(gpu, surface, box, nullptr, 1, mirror);
            W_CHECK(m.triangles == boxRef.triangles && m.vertices.size() == boxRef.vertices.size(), "mirrored box: %u triangles, reference %u", m.triangles, boxRef.triangles);
            double worst = 0, velocity = 0;
            for (size_t t = 0; t + 2 < boxRef.vertices.size(); t += 3)
                for (int v = 0; v < 3; ++v)
                {
                    const size_t gi = t + (v == 0 ? 0 : 3 - v), ri = t + v;  // (0, 1, 2) -> (0, 2, 1)
                    for (int a = 0; a < 6; ++a)
                    {
                        const double sign = (a % 3) == 2 ? -1 : 1;
                        const auto& unmirrored = a < 3 ? boxRef.vertices[ri] : boxMesh.vertices[ri];  // (the reference has no normals)
                        worst = std::max(worst, (double)std::abs(m.vertices[gi][a] - sign * unmirrored[a]));
                    }
                    for (int a = 0; a < 3; ++a)
                        velocity = std::max(velocity, (double)std::abs(m.velocities[gi][a] - (a == 2 ? -1 : 1) * boxMesh.velocities[ri][a]));
                }
            W_CHECK(worst <= 1e-5, "mirrored box: vertex differs from the mirrored reference by %.3g", worst);
            W_CHECK(velocity == 0, "mirrored box: velocity differs from the mirrored GPU box by %.3g", velocity);
            double mirroredVolume = 0;  // signed volume stays positive: outside still counter-clockwise
            for (size_t t = 0; t + 2 < m.vertices.size(); t += 3)
            {
                const auto& a = m.vertices[t]; const auto& b = m.vertices[t + 1]; const auto& c = m.vertices[t + 2];
                mirroredVolume += (double(a[0]) * (double(b[1]) * c[2] - double(b[2]) * c[1]) - double(a[1]) * (double(b[0]) * c[2] - double(b[2]) * c[0]) + double(a[2]) * (double(b[0]) * c[1] - double(b[1]) * c[0])) / 6;
            }
            W_CHECK(std::abs(mirroredVolume / volume(boxMesh) - 1) <= 1e-5, "mirrored box volume %.6g, box %.6g (orientation)", mirroredVolume, volume(boxMesh));
            std::printf("mirrored box: vertices within %.3g of the mirrored reference, volume %.6g m3 (orientation kept)\n", worst, mirroredVolume);
        }
        // 3. sphere and jittered cloud
        auto inSphere = [](float x, float y, float z) { const float dx = x - 16, dy = y - 16, dz = z - 16; return dx * dx + dy * dy + dz * dz <= 100; };
        const auto sphere = lattice(4, 4, 4, 28, 28, 28, inSphere);
        const auto sphereMesh = run(gpu, surface, sphere, nullptr, 1);
        const auto sphereRef = reference(desc, sphere, nullptr, 1);
        compare(sphereMesh, sphereRef, "sphere");
        checkClosed(sphereMesh, sphereRef, "sphere");
        const double sphereVolume = volume(sphereMesh), sphereExpected = sphere.size() * h3, sphereRawVolume = volume(run(gpu, rawSurface, sphere, nullptr, 1));
        W_CHECK(std::abs(sphereVolume / sphereExpected - 1) <= 0.01, "sphere volume %.6g m3, particles %.6g m3", sphereVolume, sphereExpected);
        W_CHECK(std::abs(sphereVolume / sphereRawVolume - 1) <= 0.002, "sphere volume %.6g m3, unfiltered %.6g m3", sphereVolume, sphereRawVolume);
        std::printf("volumes vs the particles: box %.3f %% (unfiltered %.3f %%), sphere %.3f %% (unfiltered %.3f %%)\n", 100 * (boxVolume / boxExpected - 1),
                    100 * (boxRawVolume / boxExpected - 1), 100 * (sphereVolume / sphereExpected - 1), 100 * (sphereRawVolume / sphereExpected - 1));
        // 10. the low-pass removes the particles' sampling noise and keeps the water's shape (FluidSmooth.hlsl): see the head.
        {
            const double cell = 2.0 * desc.h;  // a fluid cell (two spacings) in m
            auto world = [&](int a, double c) { return desc.origin[a] + c * cell; };
            std::mt19937 smoothRng(11);
            std::uniform_real_distribution<float> half(-0.25f, 0.25f);  // half a spacing (cells)
            auto slab = lattice(4, 4, 4, 28, 12, 28, all);
            for (auto& p : slab) for (float& x : p.x) x += half(smoothRng);
            const auto slabMesh = run(gpu, surface, slab, nullptr, 1), slabRaw = run(gpu, rawSurface, slab, nullptr, 1);
            compare(slabMesh, reference(desc, slab, nullptr, 1), "jittered slab");
            auto topRms = [&](const Mesh& m) {
                double sum = 0, sum2 = 0;
                size_t n = 0;
                for (const auto& v : m.vertices)
                    if (v[1] > world(1, 11) && v[0] > world(0, 5.5) && v[0] < world(0, 26.5) && v[2] > world(2, 5.5) && v[2] < world(2, 26.5)) sum += v[1], sum2 += double(v[1]) * v[1], ++n;
                const double mean = sum / n;
                return std::sqrt(std::max(0.0, sum2 / n - mean * mean));
            };
            const double rmsRaw = topRms(slabRaw), rms = topRms(slabMesh), slabExpected = slab.size() * h3, slabV = volume(slabMesh), slabRawV = volume(slabRaw);
            std::printf("jittered slab: top height RMS %.3f mm -> %.3f mm (%.2f x); volume %.3f %% vs particles (unfiltered %.3f %%)\n", 1e3 * rmsRaw, 1e3 * rms, rmsRaw / rms,
                        100 * (slabV / slabExpected - 1), 100 * (slabRawV / slabExpected - 1));
            W_CHECK(rms <= 0.6 * rmsRaw, "jittered slab: top RMS %.4g m, unfiltered %.4g m", rms, rmsRaw);
            W_CHECK(std::abs(slabV / slabExpected - 1) <= 0.01, "jittered slab volume %.6g, unfiltered %.6g, particles %.6g", slabV, slabRawV, slabExpected);
            // b. crest
            const double k = 2 * 3.14159265358979 / 4.0;  // per cell: wavelength 4 cells = 8 spacings
            auto crest = lattice(4, 4, 4, 28, 14, 28, [&](float x, float y, float) { return y < 10 + std::sin(k * x); });
            const auto crestMesh = run(gpu, surface, crest, nullptr, 1), crestRaw = run(gpu, rawSurface, crest, nullptr, 1);
            auto amplitude = [&](const Mesh& m) {
                // least squares y = a + b sin(kx) + c cos(kx) over the top vertices (upward normals) of the interior
                double A[3][3] = {}, r[3] = {};
                for (const auto& v : m.vertices)
                {
                    if (!(v[4] > 0.3f) || v[1] < world(1, 7) || v[0] < world(0, 6) || v[0] > world(0, 26) || v[2] < world(2, 6) || v[2] > world(2, 26)) continue;
                    const double x = (v[0] - desc.origin[0]) / cell, f[3] = { 1, std::sin(k * x), std::cos(k * x) };
                    for (int i = 0; i < 3; ++i) { r[i] += f[i] * v[1]; for (int j = 0; j < 3; ++j) A[i][j] += f[i] * f[j]; }
                }
                for (int i = 0; i < 3; ++i)  // Gauss-Jordan on the 3 x 3 normal equations
                {
                    const double p = A[i][i];
                    for (int j = 0; j < 3; ++j) A[i][j] /= p;
                    r[i] /= p;
                    for (int e = 0; e < 3; ++e)
                        if (e != i) { const double q = A[e][i]; for (int j = 0; j < 3; ++j) A[e][j] -= q * A[i][j]; r[e] -= q * r[i]; }
                }
                return std::sqrt(r[1] * r[1] + r[2] * r[2]);
            };
            const double crestA = amplitude(crestMesh), crestRawA = amplitude(crestRaw);
            std::printf("crest (wavelength 8 h, particle amplitude 2 h): fitted amplitude %.3f mm, unfiltered %.3f mm (%.4f)\n", 1e3 * crestA, 1e3 * crestRawA, crestA / crestRawA);
            W_CHECK(std::abs(crestA / crestRawA - 1) <= 0.03, "crest amplitude %.4g m, unfiltered %.4g m", crestA, crestRawA);
            // c. jets
            for (const float R : { 0.75f, 1.25f })
            {
                auto jet = lattice(4, 12, 12, 28, 20, 20, [&](float, float y, float z) { return (y - 16) * (y - 16) + (z - 16) * (z - 16) <= R * R; });
                const auto jetMesh = run(gpu, surface, jet, nullptr, 1), jetRaw = run(gpu, rawSurface, jet, nullptr, 1);
                std::vector<char> covered(48, 0);
                auto radius = [&](const Mesh& m, bool mark) {
                    double sum = 0;
                    size_t n = 0;
                    for (const auto& v : m.vertices)
                    {
                        const double x = (v[0] - desc.origin[0]) / cell;
                        if (mark && x >= 4 && x < 28) covered[size_t((x - 4) * 2)] = 1;
                        if (x < 12 || x > 20) continue;
                        sum += std::hypot(v[1] - world(1, 16), v[2] - world(2, 16)), ++n;
                    }
                    return n ? sum / n : 0.0;
                };
                const double r = radius(jetMesh, true), rRaw = radius(jetRaw, false);
                const size_t spans = size_t(std::count(covered.begin(), covered.end(), 1));
                std::printf("jet %.0f spacings thick: mean radius %.3f mm, unfiltered %.3f mm (%.4f), drawn over %zu of 48 half cells\n", 4 * R - 1, 1e3 * r, 1e3 * rRaw, r / rRaw, spans);
                W_CHECK(rRaw > 0 && std::abs(r / rRaw - 1) <= 0.05, "jet R %.2f: mean radius %.4g m, unfiltered %.4g m", R, r, rRaw);
                W_CHECK(spans == 48, "jet R %.2f: drawn over %zu of 48 half cells", R, spans);
            }
        }
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
