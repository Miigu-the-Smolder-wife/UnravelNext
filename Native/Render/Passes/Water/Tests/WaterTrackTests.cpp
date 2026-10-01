// Track W entry point correctness (B8: tracks::waterGeometry consumes FrameContext::fluids, INTERFACES v1.70):
//   1. a fluid of the frame becomes one layer-1 (water layer) triangle stream with the fluid's material, the node grid's world bounds
//      and a capacity for its particles; its vertices and draw arguments are bit-identical to a FluidSurface recorded
//      directly with the same description and input
//   2. origin rebase: the same particles with the fluid's origin moved by -1024 m in x give the same triangles moved by
//      -1024 m (within float spacing at 1 km)
//   3. a fluid without particles adds no stream
//   4. an anchored domain (NP_FluidGpuView2, physics a342d694): the start buffer's cells from startOrigin, the current one's
//      from origin; at alpha 0.5 the surface lies at the blended origin (the same particles in both buffers: the triangles
//      of test 1 moved by half the domain's motion) and every vertex velocity carries the frame velocity
//   unx_test_water_watertracktests [--no-debug-layer] [--warp]
#include "unx/render/Frame.h"
#include "unx/render/GpuScene.h"
#include "unx/render/Tracks.h"
#include "unx/water/FluidSurface.h"

#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"

#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::render;

#define W_CHECK(cond, ...)                                                                                            \
    do                                                                                                                \
    {                                                                                                                 \
        if (!(cond)) fail("%s:%d: %s", __FILE__, __LINE__, unx::format(__VA_ARGS__).c_str());                         \
    } while (0)

namespace
{
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
    if (type == D3D12_HEAP_TYPE_DEFAULT) d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)), "test buffer");
    return r;
}
void run(Gpu& gpu, RenderGraph& g)
{
    g.execute(nullptr);
    for (uint32_t q = 0; q < kQueueTypeCount; ++q) gpu.device.queue((QueueType)q).waitCpu(g.lastFence((QueueType)q));
}
// Reads a stream's draw arguments and its first `vertices` vertices (32 B each).
struct Read
{
    uint32_t drawn = 0;
    std::vector<float> vertices;
};
Read readStream(Gpu& gpu, RenderGraph& g, BufferRef vertices, BufferRef draw, uint32_t maxVertices)
{
    ComPtr<ID3D12Resource> rb = buffer(gpu.device, 256 + uint64_t(maxVertices) * 32, D3D12_HEAP_TYPE_READBACK);
    ID3D12Resource* r = rb.Get();
    g.addPass("W test read", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(vertices, Use::CopySrc); pb.use(draw, Use::CopySrc); pb.keep(); },
              [=](PassContext& c) {
                  c.cmd->CopyBufferRegion(r, 0, c.resource(draw), 0, 16);
                  c.cmd->CopyBufferRegion(r, 256, c.resource(vertices), 0, uint64_t(maxVertices) * 32);
              });
    run(gpu, g);
    Read out;
    const uint8_t* m = nullptr;
    check(rb->Map(0, nullptr, (void**)&m), "map stream");
    std::memcpy(&out.drawn, m, 4);
    out.vertices.assign((const float*)(m + 256), (const float*)(m + 256) + size_t(std::min(out.drawn, maxVertices)) * 8);
    rb->Unmap(0, nullptr);
    return out;
}
// The stream's first `vertices` velocities (16 B each: m/s, w = 0).
std::vector<float> readVelocities(Gpu& gpu, RenderGraph& g, BufferRef velocities, uint32_t vertices)
{
    ComPtr<ID3D12Resource> rb = buffer(gpu.device, uint64_t(vertices) * 16, D3D12_HEAP_TYPE_READBACK);
    ID3D12Resource* r = rb.Get();
    g.addPass("W test read velocities", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(velocities, Use::CopySrc); pb.keep(); },
              [=](PassContext& c) { c.cmd->CopyBufferRegion(r, 0, c.resource(velocities), 0, uint64_t(vertices) * 16); });
    run(gpu, g);
    std::vector<float> out(size_t(vertices) * 4);
    const uint8_t* m = nullptr;
    check(rb->Map(0, nullptr, (void**)&m), "map velocities");
    std::memcpy(out.data(), m, out.size() * 4);
    rb->Unmap(0, nullptr);
    return out;
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
        QualityConfig quality = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
        GpuScene scene(gpu.device);
        FrameServices services;
        TrackState trackState;

        // Particles (NP_FluidParticle, 48 B: position in cells at 0, velocity at 16, start slot at 44): a ball of radius 6
        // cells in a 32^3-cell domain, 8 particles per cell on a jittered lattice.
        const uint32_t cells = 32;
        std::vector<uint8_t> bytes;
        uint32_t count = 0;
        for (uint32_t z = 0; z < 2 * cells; ++z)
            for (uint32_t y = 0; y < 2 * cells; ++y)
                for (uint32_t x = 0; x < 2 * cells; ++x)
                {
                    const float p[3] = { (x + 0.5f) * 0.5f, (y + 0.5f) * 0.5f, (z + 0.5f) * 0.5f };
                    const float dx = p[0] - 16, dy = p[1] - 16, dz = p[2] - 16;
                    if (dx * dx + dy * dy + dz * dz > 36) continue;
                    uint8_t rec[48] = {};
                    const float v[3] = { 1.0f, 0.0f, -0.5f };
                    std::memcpy(rec, p, 12);
                    std::memcpy(rec + 16, v, 12);
                    std::memcpy(rec + 44, &count, 4);
                    bytes.insert(bytes.end(), rec, rec + 48);
                    ++count;
                }
        ComPtr<ID3D12Resource> particles = buffer(gpu.device, bytes.size(), D3D12_HEAP_TYPE_DEFAULT);
        {
            ComPtr<ID3D12Resource> upload = buffer(gpu.device, bytes.size(), D3D12_HEAP_TYPE_UPLOAD);
            void* m = nullptr;
            check(upload->Map(0, nullptr, &m), "map particles");
            std::memcpy(m, bytes.data(), bytes.size());
            upload->Unmap(0, nullptr);
            RenderGraph g(gpu.device);
            const BufferRef dst = g.importBuffer(particles.Get(), { "particles", particles->GetDesc().Width, 0 });
            ID3D12Resource* src = upload.Get();
            const uint64_t size = bytes.size();
            g.addPass("particles upload", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(dst, Use::CopyDst); pb.keep(); },
                      [=](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(dst), 0, src, 0, size); });
            run(gpu, g);
        }

        FluidFrame fluid;
        fluid.current = particles.Get();
        fluid.count = count;
        fluid.stride = 48;
        fluid.origin[0] = 10.5; fluid.origin[1] = 2.0; fluid.origin[2] = -3.0;
        fluid.dx = 0.05f;
        fluid.alpha = 1;
        fluid.domainCells[0] = fluid.domainCells[1] = fluid.domainCells[2] = cells;
        fluid.material = 1;
        auto frameWith = [&](const FluidFrame* fluids, uint32_t n, FrameResources& resources, RenderGraph& g) {
            FrameContext fr;
            fr.fluids = fluids;
            fr.fluidCount = n;
            FramePassContext fc{ gpu.device, g, gpu.shaders, quality, scene, fr, resources, services, [](const ViewDesc&) { return D3D12_GPU_VIRTUAL_ADDRESS(0); }, &trackState, 2 };
            tracks::waterGeometry(fc);
        };

        // 1. the stream against a direct record
        Read viaTrack, direct;
        uint32_t maxTriangles = 0;
        {
            RenderGraph g(gpu.device);
            FrameResources resources;
            frameWith(&fluid, 1, resources, g);
            W_CHECK(resources.triangleStreams.size() == 1, "%zu streams for one fluid", resources.triangleStreams.size());
            const TriangleStream& st = resources.triangleStreams[0];
            maxTriangles = st.maxTriangles;
            W_CHECK(st.material == 1 && st.layer == 1 && st.instance == 0xFFFFFFFFu, "stream material %u layer %u instance %u", st.material, st.layer, st.instance);
            W_CHECK(maxTriangles == 2 * std::max<uint32_t>(4096, count * 5 / 4), "capacity %u for %u particles", maxTriangles, count);
            const float h = 0.025f;
            W_CHECK(st.boundsMin.x == 10.5f && st.boundsMin.y == 2.0f && st.boundsMin.z == -3.0f && std::abs(st.boundsMax.x - (10.5f + 64 * h)) < 1e-5f,
                    "bounds (%g %g %g) - (%g %g %g)", st.boundsMin.x, st.boundsMin.y, st.boundsMin.z, st.boundsMax.x, st.boundsMax.y, st.boundsMax.z);
            viaTrack = readStream(gpu, g, st.vertices, st.drawArgs, 3 * maxTriangles);
        }
        {
            water::FluidSurfaceDesc d;
            d.nodes[0] = d.nodes[1] = d.nodes[2] = 64;
            d.scale = 2.0f;
            d.h = 0.025f;
            d.origin[0] = 10.5f; d.origin[1] = 2.0f; d.origin[2] = -3.0f;
            d.maxParticles = std::max<uint32_t>(4096, count * 5 / 4);
            d.maxTriangles = 2 * d.maxParticles;
            water::FluidSurface surface(gpu.device, gpu.shaders, d);
            RenderGraph g(gpu.device);
            water::FluidSurfaceInput input;
            input.particles = g.importBuffer(particles.Get(), { "particles", particles->GetDesc().Width, 0 });
            input.count = count;
            input.stride = 48;
            input.velocityOffset = 16;
            input.velocityScale = 0.05f;
            const auto out = surface.record(g, input);
            direct = readStream(gpu, g, out.vertices, out.draw, 3 * maxTriangles);
        }
        W_CHECK(viaTrack.drawn > 0 && viaTrack.drawn == direct.drawn, "vertices %u via the track, %u direct", viaTrack.drawn, direct.drawn);
        W_CHECK(std::memcmp(viaTrack.vertices.data(), direct.vertices.data(), viaTrack.vertices.size() * 4) == 0, "the track's vertices differ from the direct record");
        std::printf("waterGeometry: %u particles -> 1 layer-1 stream, material 1, %u triangles (capacity %u), bit-identical to a direct FluidSurface record\n", count,
                    viaTrack.drawn / 3, maxTriangles);

        // 2. origin rebase
        {
            FluidFrame shifted = fluid;
            shifted.origin[0] -= 1024;
            RenderGraph g(gpu.device);
            FrameResources resources;
            frameWith(&shifted, 1, resources, g);
            const TriangleStream& st = resources.triangleStreams.at(0);
            const Read moved = readStream(gpu, g, st.vertices, st.drawArgs, 3 * maxTriangles);
            W_CHECK(moved.drawn == viaTrack.drawn, "rebased: %u vertices, before %u", moved.drawn, viaTrack.drawn);
            double worst = 0;
            for (size_t v = 0; v < moved.vertices.size(); v += 8)
                worst = std::max({ worst, std::abs(double(moved.vertices[v]) - (double(viaTrack.vertices[v]) - 1024)), std::abs(double(moved.vertices[v + 1]) - viaTrack.vertices[v + 1]),
                                   std::abs(double(moved.vertices[v + 2]) - viaTrack.vertices[v + 2]) });
            W_CHECK(worst <= 2e-4, "rebased vertices differ from the moved ones by %.3g m", worst);
            std::printf("origin rebase: -1024 m in x moves every vertex by -1024 m within %.2e m\n", worst);
        }

        // 3. a fluid without particles
        {
            FluidFrame empty = fluid;
            empty.count = 0;
            RenderGraph g(gpu.device);
            FrameResources resources;
            frameWith(&empty, 1, resources, g);
            W_CHECK(resources.triangleStreams.empty(), "an empty fluid added %zu streams", resources.triangleStreams.size());
            std::printf("a fluid without particles adds no stream\n");
        }
        // 4. an anchored domain: moved +0.4 m in x over the tick (24 m/s at 60 Hz), the particles still in the domain's frame
        {
            FluidFrame anchored = fluid;
            anchored.start = particles.Get();
            anchored.startValid = 1;
            anchored.startCount = count;
            anchored.startOrigin[0] = fluid.origin[0] - 0.4;
            anchored.startOrigin[1] = fluid.origin[1];
            anchored.startOrigin[2] = fluid.origin[2];
            anchored.alpha = 0.5f;
            anchored.frameVelocity[0] = 24.0f;
            RenderGraph g(gpu.device);
            FrameResources resources;
            frameWith(&anchored, 1, resources, g);
            const TriangleStream& st = resources.triangleStreams.at(0);
            const Read moved = readStream(gpu, g, st.vertices, st.drawArgs, 3 * maxTriangles);
            W_CHECK(moved.drawn == viaTrack.drawn, "anchored: %u vertices, unanchored %u", moved.drawn, viaTrack.drawn);
            double worst = 0;
            for (size_t v = 0; v < moved.vertices.size(); v += 8)
                worst = std::max({ worst, std::abs(double(moved.vertices[v]) - (double(viaTrack.vertices[v]) - 0.2)), std::abs(double(moved.vertices[v + 1]) - viaTrack.vertices[v + 1]),
                                   std::abs(double(moved.vertices[v + 2]) - viaTrack.vertices[v + 2]) });
            W_CHECK(worst <= 1e-5, "anchored vertices differ from the surface at the blended origin by %.3g m", worst);
            RenderGraph gv(gpu.device);
            FrameResources rv;
            frameWith(&anchored, 1, rv, gv);
            const std::vector<float> velocity = readVelocities(gpu, gv, rv.triangleStreams.at(0).velocities, moved.drawn);
            // the particles' own velocity (1, 0, -0.5) cells/s x 0.05 m = (0.05, 0, -0.025) m/s plus the frame's (24, 0, 0)
            double worstV = 0;
            for (size_t v = 0; v < velocity.size(); v += 4)
                worstV = std::max({ worstV, std::abs(double(velocity[v]) - 24.05), std::abs(double(velocity[v + 1])), std::abs(double(velocity[v + 2]) + 0.025) });
            W_CHECK(worstV <= 1e-3, "anchored vertex velocities differ from the particles' plus the frame's by %.3g m/s", worstV);
            std::printf("anchored domain: at alpha 0.5 every vertex lies at the blended origin within %.2e m, velocities carry the frame's within %.2e m/s\n", worst, worstV);
        }
        if (!warp)
        {
            const uint32_t errors = gpu.device.drainDebugMessages();
            W_CHECK(errors == 0, "%u debug-layer errors", errors);
        }
        std::printf("water track tests passed\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
}
