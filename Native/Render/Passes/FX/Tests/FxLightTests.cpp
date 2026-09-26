// A3 FX particle lights (FxLights.cpp / FxLights.hlsl, contract NativeVfxStream.h NV_STREAM_PROGRAM_LIGHT):
//   1. the RPP stream (reduced) with the light flag on root sprite programs 1, 3, 5 and 9, through the particle module;
//   2. the frame time at the latest tick's end (w = 1: the particles alive then are exactly the checkpoint's), the scene
//      light tail sized by tracks::particleLightCapacity, then tracks::particleLights;
//   3. the tail and the count read back and compared with a CPU double reference over the checkpoint: per active light row
//      (row order) I = sum alpha L pi (s/2)^2, the Y-weighted centre, colour I / Y, spread = RMS radius + mean radius,
//      range = sqrt(Y exposure / (pi 2^-10)), point light without shadow;
//   4. both World conventions (stream axes (1, 1, 1) and the Unity host's (1, 1, -1), camera away from the origin);
//      two runs bit identical (determinism).
// Options: --ticks N (40) --particles P (8192) --warp --no-debug-layer
#include "RppStream.h"

#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/fx/Particles.h"
#include "unx/render/Frame.h"
#include "unx/render/FrameResources.h"
#include "unx/render/GpuScene.h"
#include "unx/render/GpuSceneLayout.h"
#include "unx/render/Tracks.h"

#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::render;

namespace
{
#define FX_CHECK(cond, ...)                                                                                           \
    do                                                                                                                \
    {                                                                                                                 \
        if (!(cond))                                                                                                  \
        {                                                                                                             \
            logf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                 \
            logf(__VA_ARGS__);                                                                                        \
            logf("\n");                                                                                               \
            throw std::runtime_error("check failed");                                                                 \
        }                                                                                                             \
    } while (0)

ComPtr<ID3D12Resource> makeBuffer(Device& device, uint64_t bytes, D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES heap{ type };
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    check(device.d3d()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, type == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST : D3D12_RESOURCE_STATE_GENERIC_READ,
                                                nullptr, IID_PPV_ARGS(&r)),
          "test buffer");
    return r;
}

std::vector<uint8_t> readBack(Device& device, ID3D12Resource* source, uint64_t bytes)
{
    ComPtr<ID3D12Resource> rb = makeBuffer(device, bytes, D3D12_HEAP_TYPE_READBACK);
    CommandList cl = device.acquireCommandList(QueueType::Graphics);
    cl.list->CopyBufferRegion(rb.Get(), 0, source, 0, bytes);
    device.queue(QueueType::Graphics).waitCpu(device.submit(cl));
    std::vector<uint8_t> out(bytes);
    uint8_t* p = nullptr;
    check(rb->Map(0, nullptr, reinterpret_cast<void**>(&p)), "map readback");
    std::memcpy(out.data(), p, bytes);
    rb->Unmap(0, nullptr);
    return out;
}

struct Result
{
    std::vector<gpu::Light> lights;
    uint32_t count = 0;
    uint32_t sceneLights = 0;
    std::vector<uint8_t> bytes;
};

constexpr uint32_t kLightPrograms = (1u << 1) | (1u << 3) | (1u << 5) | (1u << 9);

Result run(Device& device, uint32_t ticks, uint32_t particles, float zSign, bool verify)
{
    ShaderLibrary shaders(device, executableDirectory() / "shaders");
    const QualityConfig quality = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
    GpuScene scene(device);
    scene::Scene empty;
    scene::Light sceneLight;  // one ordinary light before the tail
    sceneLight.type = scene::LightType::Point;
    sceneLight.position = { 3, 2, 1 };
    sceneLight.intensity = 100;
    sceneLight.range = 10;
    sceneLight.color = { 1, 1, 1 };
    empty.lights.push_back(sceneLight);
    scene.upload(empty);
    RenderGraph graph(device);
    TrackState state;
    fx::ParticleSystem& ps = fx::particles(state, device, quality);
    fx::test::RppConfig rpp;
    rpp.particles = particles;
    rpp.lightPrograms = kLightPrograms;
    fx::test::RppStream stream(rpp);
    FrameContext frame;
    frame.streamAxes[2] = zSign;
    frame.worldOrigin[0] = 2048;  // the camera far from the World origin: offsets are double differences
    FrameServices services;
    std::vector<NV_StreamEvent> previous;
    std::vector<NV_StreamEmitter> table;
    std::vector<NV_StreamProgram> programs;
    NV_StreamHeader last{};
    for (uint32_t t = 1; t <= ticks; ++t)
    {
        const std::vector<uint8_t> packet = stream.next(t == 1 ? nullptr : &previous);
        const NV_StreamHeader& h = *reinterpret_cast<const NV_StreamHeader*>(packet.data());
        nv_stream::apply_emitter_table(table, h, packet.data(), packet.size());
        if (h.flags & NV_STREAM_PROGRAMS)
        {
            const auto* p = reinterpret_cast<const NV_StreamProgram*>(packet.data() + h.programs);
            programs.assign(p, p + h.program_count);
        }
        ps.submit(packet.data(), packet.size());
        FrameResources resources;
        FramePassContext fc{ device, graph, shaders, quality, scene, frame, resources, services, [](const ViewDesc&) -> D3D12_GPU_VIRTUAL_ADDRESS { return 0; }, &state };
        tracks::simulation(fc);
        graph.execute(nullptr);
        ++frame.frameIndex;
        const fx::TickReadback rb = ps.readback(h.stream, h.generation, h.tick);
        FX_CHECK((rb.counters.status & ~1u) == 0, "tick %u: particle status 0x%x", t, rb.counters.status);
        previous = rb.events;
        last = h;
    }

    // the frame at the latest tick's end; the camera 5 m off the particles' anchor (renderer world = stream x axes - origin)
    frame.time = last.time;
    scene::Camera cam;
    const double camStream[3] = { last.anchor[0] + 5, last.anchor[1] + 2, last.anchor[2] - 4 };
    for (int a = 0; a < 3; ++a) (&cam.position.x)[a] = (float)(camStream[a] * (&frame.streamAxes[0])[a] - frame.worldOrigin[a]);
    cam.forward = { 0, 0, 1 };
    ViewResources main;
    main.view = ViewDesc::fromCamera(cam, 256, 256, float4x4{});
    const float exposure = 1.0f / (1.2f * 16.0f);  // EV100 4
    ComPtr<ID3D12Resource> cbuffer = makeBuffer(device, 1024, D3D12_HEAP_TYPE_UPLOAD);
    {
        gpu::FrameConstants c{};
        c.cameraPosition = main.view.position;
        c.exposure = exposure;
        void* p = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(cbuffer->Map(0, &none, &p), "map frame constants");
        std::memcpy(p, &c, sizeof c);
        cbuffer->Unmap(0, nullptr);
    }
    main.frameConstants = cbuffer->GetGPUVirtualAddress();

    tracks::particleLightCapacity(state, scene);
    const GpuScene::FxLightRange range = scene.fxLightRange();
    FX_CHECK(range.capacity >= ps.lightTables().slotChunks.size() && range.capacity > 0, "capacity %u for %zu light rows", range.capacity, ps.lightTables().slotChunks.size());
    FrameResources resources;
    resources.fxLights = graph.importBuffer(range.lightBuffer, BufferDesc{ "scene lights (FX tail)", (uint64_t)(range.first + range.capacity) * sizeof(gpu::Light), (uint32_t)sizeof(gpu::Light) });
    resources.fxLightCount = graph.importBuffer(range.countBuffer, BufferDesc{ "FX light count", 16, 4 });
    FramePassContext fc{ device, graph, shaders, quality, scene, frame, resources, services, [](const ViewDesc&) -> D3D12_GPU_VIRTUAL_ADDRESS { return 0; }, &state };
    tracks::particleLights(fc, main);
    graph.execute(nullptr);
    device.waitIdle();

    Result r;
    r.sceneLights = range.first;
    const std::vector<uint8_t> count = readBack(device, range.countBuffer, 4);
    std::memcpy(&r.count, count.data(), 4);
    r.bytes = readBack(device, range.lightBuffer, (uint64_t)(range.first + range.capacity) * sizeof(gpu::Light));
    r.lights.resize(range.first + range.capacity);
    std::memcpy(r.lights.data(), r.bytes.data(), r.bytes.size());
    if (!verify) return r;

    // CPU reference (double) over the checkpoint, per active light row in row order
    const std::vector<NV_StreamParticle> live = ps.checkpoint(shaders);
    std::vector<int> slotOf(table.size(), -1);
    uint32_t slots = 0;
    for (size_t row = 0; row < table.size(); ++row)
    {
        const NV_StreamEmitter& e = table[row];
        const NV_StreamProgram& p = programs[e.program];
        if ((e.flags & NV_STREAM_EMITTER_ACTIVE) && (p.flags & NV_STREAM_PROGRAM_LIGHT) && p.output == 0 && p.material == 0) slotOf[row] = (int)slots++;
    }
    FX_CHECK(slots > 0, "no light rows in the stream");
    FX_CHECK(r.count == slots, "count word %u, light rows %u", r.count, slots);
    struct Sum { double i[3] = {}, y = 0, yp[3] = {}, yp2 = 0, yr = 0; uint32_t n = 0; };
    std::vector<Sum> sums(slots);
    const double axes[3] = { frame.streamAxes[0], frame.streamAxes[1], frame.streamAxes[2] };
    for (const NV_StreamParticle& q : live)
    {
        const int s = slotOf[q.emitter];
        if (s < 0) continue;
        const NV_StreamEmitter& e = table[q.emitter];
        const NV_StreamProgram& p = programs[e.program];
        if (e.flags & NV_STREAM_EMITTER_KILLED) continue;
        const double size = std::max(0.0, (double)p.size * e.size_scale);  // curves constant 1 in the RPP stream
        const double alpha = std::clamp((double)p.color[3] * e.color_scale[3], 0.0, 1.0);
        double I[3], Y = 0;
        for (int c = 0; c < 3; ++c) I[c] = std::max(0.0, (double)p.color[c] * e.color_scale[c]) * alpha * 0.25 * 3.14159265358979 * size * size;
        Y = 0.2126 * I[0] + 0.7152 * I[1] + 0.0722 * I[2];
        if (!(Y > 0)) continue;
        double pos[3], p2 = 0;
        for (int a = 0; a < 3; ++a)
        {
            const double world = (last.anchor[a] + e.origin_anchor[a] + q.position[a]) * axes[a];  // renderer axes, + origin
            pos[a] = world - (frame.worldOrigin[a] + (&main.view.position.x)[a]);                 // camera-relative
            p2 += pos[a] * pos[a];
        }
        Sum& m = sums[s];
        for (int c = 0; c < 3; ++c) m.i[c] += I[c], m.yp[c] += Y * pos[c];
        m.y += Y, m.yp2 += Y * p2, m.yr += Y * 0.5 * size, ++m.n;
    }
    double worstI = 0, worstP = 0, worstC = 0, worstS = 0, worstR = 0;
    uint32_t particlesChecked = 0;
    for (uint32_t s = 0; s < slots; ++s)
    {
        const gpu::Light& g = r.lights[range.first + s];
        const Sum& m = sums[s];
        particlesChecked += m.n;
        FX_CHECK((g.typeFlags & 0xFF) == 0 && ((g.typeFlags >> 8) & 1) == 0 && (g.typeFlags >> 16) == 0xFFFF, "slot %u: type flags 0x%x (point, no shadow)", s, g.typeFlags);
        if (m.y <= 0)
        {
            FX_CHECK(g.intensity == 0, "slot %u: intensity %g without emitting particles", s, g.intensity);
            continue;
        }
        const double c[3] = { m.yp[0] / m.y, m.yp[1] / m.y, m.yp[2] / m.y };
        const double spread = std::sqrt(std::max(0.0, m.yp2 / m.y - (c[0] * c[0] + c[1] * c[1] + c[2] * c[2]))) + m.yr / m.y;
        const double rangeRef = std::sqrt(m.y * exposure / (3.14159265358979 / 1024.0));
        worstI = std::max(worstI, std::fabs(g.intensity - m.y) / m.y);
        for (int a = 0; a < 3; ++a)
        {
            worstP = std::max(worstP, std::fabs((double)(&g.position.x)[a] - ((&main.view.position.x)[a] + c[a])));
            worstC = std::max(worstC, std::fabs((double)(&g.color.x)[a] - m.i[a] / m.y));
        }
        worstS = std::max(worstS, std::fabs(g.size.x - spread) / std::max(spread, 1e-3));
        worstR = std::max(worstR, std::fabs(g.range - rangeRef) / rangeRef);
    }
    logf("FX lights: %u light rows, %u particles, stream axes z %+g: intensity rel %.2e, centre %.2e m, colour %.2e, spread rel %.2e, range rel %.2e\n", slots,
         particlesChecked, (double)zSign, worstI, worstP, worstC, worstS, worstR);
    FX_CHECK(particlesChecked > 0, "no emitting particle in the light rows");
    FX_CHECK(worstI < 1e-4 && worstC < 1e-4 && worstS < 1e-3 && worstR < 1e-4, "light values off the reference");
    FX_CHECK(worstP < 2e-4, "light centres off the reference by %g m", worstP);
    // the scene light before the tail is untouched
    FX_CHECK(r.lights[0].intensity == 100 && r.lights[0].range == 10, "scene light 0 changed: intensity %g range %g", r.lights[0].intensity, r.lights[0].range);
    return r;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        uint32_t ticks = 40, particles = 8192;
        bool warp = false, debugLayer = true;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            auto next = [&]() -> std::string { if (i + 1 >= argc) fail("missing value after %s", a.c_str()); return argv[++i]; };
            if (a == "--ticks") ticks = (uint32_t)std::stoul(next());
            else if (a == "--particles") particles = (uint32_t)std::stoul(next());
            else if (a == "--warp") warp = true;
            else if (a == "--no-debug-layer") debugLayer = false;
            else fail("unknown option %s", a.c_str());
        }
        DeviceOptions opts;
        ComPtr<ID3D12Device> warpDevice;
        if (warp)
        {
            ComPtr<IDXGIFactory6> factory;
            check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "DXGI factory");
            ComPtr<IDXGIAdapter> adapter;
            check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP adapter");
            if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_2, IID_PPV_ARGS(&warpDevice))))
                check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&warpDevice)), "WARP device");
            opts.externalDevice = warpDevice.Get();
            opts.debugLayer = false;
        }
        else opts.debugLayer = debugLayer;
        Device device(opts);
        logf("device: %s (%s), %u ticks, %u root particles\n", device.caps().adapter.c_str(), warp ? "WARP" : "hardware", ticks, particles);
        const Result a = run(device, ticks, particles, 1.0f, true);
        const Result b = run(device, ticks, particles, 1.0f, false);
        FX_CHECK(a.bytes == b.bytes && a.count == b.count, "determinism: the light tail differs between two runs");
        logf("determinism: light tail and count bit identical between two runs\n");
        run(device, ticks, particles, -1.0f, true);  // the Unity host's World axes
        const uint32_t errors = device.drainDebugMessages();
        FX_CHECK(errors == 0, "%u D3D12 debug-layer errors", errors);
        logf("passed\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        logf("error: %s\n", e.what());
        return 1;
    }
}
