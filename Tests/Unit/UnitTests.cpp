// Fast unit tests (seconds): Core (SHA-256, quality config, jobs, distributions) and render-graph behaviour on the
// real device with the debug layer on (culling, aliasing, queue synchronisation, zero validation errors).
//   unx_unit_tests [filter]
#include "EmptyFrameScene.h"

#include <dxgi1_6.h>

#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/core/Jobs.h"
#include "unx/core/Sha256.h"
#include "unx/render/FrameRenderer.h"
#include "unx/render/GpuLock.h"
#include "unx/render/Harness.h"
#include "unx/scene/HairModel.h"
#include "unx/scene/MaterialModel.h"
#include "unx/shading/Post.h"
#include "unx/water/RoundPool.h"
#if UNX_HAS_CLUSTERBUILDER
#include "unx/clusterbuilder/ClusterBuilder.h"
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <functional>
#include <string>
#include <utility>
#include <vector>

using namespace unx;
using namespace unx::render;

namespace
{
struct TestCase
{
    const char* name;
    std::function<void()> fn;
};
std::vector<TestCase>& registry()
{
    static std::vector<TestCase> r;
    return r;
}
struct Register
{
    Register(const char* name, std::function<void()> fn) { registry().push_back({ name, std::move(fn) }); }
};
#define UNX_TEST(name) \
    static void name(); \
    static Register reg_##name(#name, name); \
    static void name()
#define CHECK(cond) \
    do { if (!(cond)) fail("%s:%d: CHECK failed: %s", __FILE__, __LINE__, #cond); } while (0)
template <typename F>
bool throws(F&& f)
{
    try { f(); } catch (const std::exception&) { return true; }
    return false;
}

Device* g_device = nullptr;
// UNX_WARP=1: the software adapter (no debug layer), for compute checks while the GPU is reserved.
ID3D12Device* warpDeviceIfAsked()
{
    char* env = nullptr;
    size_t length = 0;
    const bool warp = _dupenv_s(&env, &length, "UNX_WARP") == 0 && env != nullptr;
    std::free(env);
    if (!warp) return nullptr;
    static ComPtr<ID3D12Device> device;
    ComPtr<IDXGIFactory6> factory;
    check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "DXGI factory");
    ComPtr<IDXGIAdapter> adapter;
    check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP adapter");
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_2, IID_PPV_ARGS(&device))))
        check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&device)), "WARP device");
    return device.Get();
}
Device& testDevice()
{
    static Device device([] {
        DeviceOptions o;
        o.externalDevice = warpDeviceIfAsked();
        o.debugLayer = o.externalDevice == nullptr;
        return o;
    }());
    g_device = &device;
    return device;
}
ShaderLibrary& shaders()
{
    static ShaderLibrary lib(testDevice(), executableDirectory() / "shaders");
    return lib;
}
void runFrames(RenderGraph& graph, const std::function<void(RenderGraph&)>& build, int frames = 3)
{
    for (int f = 0; f < frames; ++f)
    {
        build(graph);
        graph.execute(nullptr);
        for (uint32_t q = 0; q < kQueueTypeCount; ++q) testDevice().queue((QueueType)q).waitCpu(graph.lastFence((QueueType)q));
    }
}
void touchPass(RenderGraph& g, const char* name, QueueType q, std::vector<TextureRef> in, std::vector<TextureRef> out)
{
    ID3D12PipelineState* pso = shaders().compute("Passes/Test/Touch");
    g.addPass(name, q,
              [=](PassBuilder& b) {
                  for (auto t : in) b.use(t, Use::SrvCompute);
                  for (auto t : out) b.use(t, Use::UavCompute);
              },
              [=](PassContext& c) {
                  uint32_t k[16] = {};
                  k[0] = (uint32_t)in.size();
                  k[1] = (uint32_t)out.size();
                  for (size_t i = 0; i < in.size(); ++i) k[4 + i] = c.srv(in[i]);
                  for (size_t j = 0; j < out.size(); ++j) k[12 + j] = c.uav(out[j]);
                  c.cmd->SetPipelineState(pso);
                  c.computeConstants(k, 16);
                  c.cmd->Dispatch(1, 1, 1);
              });
}
} // namespace

UNX_TEST(sha256_vectors)
{
    CHECK(Sha256::hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(Sha256::hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(Sha256::hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq") == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    std::string million(1000000, 'a');
    CHECK(Sha256::hex(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

UNX_TEST(quality_config_parse_and_hash)
{
    QualityConfig a = QualityConfig::parse("# c\n[gi]\nrays = 500_000\nspacing = 8.0 # px\n[output]\nresolutions = [\"3840x2160\", \"2560x1440\"]\nenabled = true\nname = \"x\"\n", "a");
    QualityConfig b = QualityConfig::parse("[output]\nname=\"x\"\nenabled=true\nresolutions=[\"3840x2160\",\"2560x1440\"]\n\n[gi]\nspacing=8.0\nrays=500000\n", "b");
    CHECK(a.integer("gi.rays") == 500000);
    CHECK(a.number("gi.spacing") == 8.0);
    CHECK(a.number("gi.rays") == 500000.0);
    CHECK(a.boolean("output.enabled"));
    CHECK(a.strings("output.resolutions").size() == 2);
    CHECK(a.hash() == b.hash());  // formatting, order and comments do not change the identity
    CHECK(a.unreadKeys().size() == 1 && a.unreadKeys()[0] == "output.name");
    QualityConfig c = QualityConfig::parse("[gi]\nrays = 500000\nspacing = 8.0\n[output]\nresolutions = [\"3840x2160\", \"2560x1440\"]\nenabled = true\nname = \"x\"\n", "c");
    c.applyOverride("gi.rays=250000");
    CHECK(c.hash() != a.hash());  // an override is part of the reported identity
    CHECK(throws([&] { a.integer("gi.missing"); }));       // no code defaults
    CHECK(throws([&] { a.integer("gi.spacing"); }));       // float is not an integer
    CHECK(throws([&] { c.applyOverride("gi.unknown=1"); }));
    CHECK(throws([] { QualityConfig::parse("[a]\nx = 1\nx = 2\n", "dup"); }));
    CHECK(throws([] { QualityConfig::parse("x = [1, 2\n", "bad"); }));
}

UNX_TEST(quality_file_loads)
{
    QualityConfig q = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
    CHECK(resolutionFromString("4K", q).width == 3840);
    CHECK(resolutionFromString("2560x1440", q).height == 1440);
    CHECK(resolutionFromString("1920x1080", q).width == 1920);  // 1080p is a configured target since 2026-09-28
    CHECK(throws([&] { resolutionFromString("1280x720", q); }));  // the harness still refuses non-target output resolutions
}

UNX_TEST(quality_directory_namespaces)
{
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "unx_quality_ns_test";
    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);
    writeTextFile(dir / "gi.toml", "[gi]\nrays = 1\n");
    writeTextFile(dir / "shadow.toml", "[shadow.vsm]\npage = 128\n");
    QualityConfig q = QualityConfig::loadDirectory(dir);
    CHECK(q.integer("gi.rays") == 1 && q.integer("shadow.vsm.page") == 128);
    writeTextFile(dir / "reflection.toml", "[gi]\nrays = 2\n");  // another track's namespace
    CHECK(throws([&] { QualityConfig::loadDirectory(dir); }));
    std::filesystem::remove_all(dir);
    QualityConfig real = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
    CHECK(real.integer("gi.rays_per_frame") == 500000);
}

UNX_TEST(jobs_parallel_for)
{
    std::atomic<uint64_t> sum{ 0 };
    Jobs::instance().parallelFor(10000, [&](uint32_t i) { sum += i; });
    CHECK(sum == 10000ull * 9999 / 2);
    CHECK(throws([] { Jobs::instance().parallelFor(100, [](uint32_t i) { if (i == 57) fail("boom"); }); }));
    for (int k = 0; k < 200; ++k) Jobs::instance().parallelFor(3, [](uint32_t) {});  // loop lifetime race check
}

UNX_TEST(distribution_percentiles)
{
    std::vector<double> v;
    for (int i = 1; i <= 100; ++i) v.push_back(i);
    Distribution d = Distribution::of(v);
    CHECK(d.median == 50.5 && d.p95 == 95 && d.p99 == 99 && d.min == 1 && d.max == 100);
}

UNX_TEST(graph_culls_unused_passes)
{
    RenderGraph g(testDevice());
    runFrames(g, [](RenderGraph& graph) {
        TextureRef a = graph.createTexture({ "a", 256, 256, 1, 1, DXGI_FORMAT_R32_FLOAT });
        TextureRef unused = graph.createTexture({ "unused", 256, 256, 1, 1, DXGI_FORMAT_R32_FLOAT });
        TextureRef out = graph.createTexture({ "out", 256, 256, 1, 1, DXGI_FORMAT_R32_FLOAT });
        touchPass(graph, "write a", QueueType::Graphics, {}, { a });
        touchPass(graph, "write unused", QueueType::Graphics, {}, { unused });  // nobody reads it
        touchPass(graph, "a -> out", QueueType::Graphics, { a }, { out });
        graph.addPass("keep", QueueType::Graphics, [&](PassBuilder& b) { b.use(out, Use::SrvCompute); b.keep(); }, [](PassContext&) {});
    });
    CHECK(g.stats().declaredPasses == 4);
    CHECK(g.stats().livePasses == 3);
    CHECK(g.stats().planReused);
}

UNX_TEST(graph_aliases_disjoint_lifetimes)
{
    RenderGraph g(testDevice());
    runFrames(g, [](RenderGraph& graph) {
        const uint32_t n = 2048;
        TextureRef a = graph.createTexture({ "a", n, n, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
        TextureRef b = graph.createTexture({ "b", n, n, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
        TextureRef c = graph.createTexture({ "c", n, n, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
        TextureRef d = graph.createTexture({ "d", n, n, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
        touchPass(graph, "a", QueueType::Graphics, {}, { a });
        touchPass(graph, "a->b", QueueType::Graphics, { a }, { b });
        touchPass(graph, "b->c", QueueType::Graphics, { b }, { c });  // a is dead here: c can take a's memory
        touchPass(graph, "c->d", QueueType::Graphics, { c }, { d });  // b is dead here
        graph.addPass("keep", QueueType::Graphics, [&](PassBuilder& p) { p.use(d, Use::SrvCompute); p.keep(); }, [](PassContext&) {});
    });
    const RenderGraphStats& s = g.stats();
    CHECK(s.transientResources == 4);
    CHECK(s.transientBytesAliased * 2 <= s.transientBytesUnaliased);  // at most two live at once
}

UNX_TEST(graph_cross_queue_sync)
{
    RenderGraph g(testDevice());
    g.setAsyncCompute(true);
    runFrames(g, [](RenderGraph& graph) {
        TextureRef a = graph.createTexture({ "a", 512, 512, 1, 1, DXGI_FORMAT_R32_FLOAT });
        TextureRef b = graph.createTexture({ "b", 512, 512, 1, 1, DXGI_FORMAT_R32_FLOAT });
        TextureRef c = graph.createTexture({ "c", 512, 512, 1, 1, DXGI_FORMAT_R32_FLOAT });
        touchPass(graph, "gfx a", QueueType::Graphics, {}, { a });
        touchPass(graph, "async a->b", QueueType::Compute, { a }, { b });
        touchPass(graph, "gfx b->c", QueueType::Graphics, { b }, { c });
        graph.addPass("keep", QueueType::Graphics, [&](PassBuilder& p) { p.use(c, Use::SrvCompute); p.keep(); }, [](PassContext&) {});
    });
    CHECK(g.stats().crossQueueSyncs == 2);
    CHECK(g.stats().commandLists == 3);
}

UNX_TEST(graph_views_of_reused_transients)
{
    // A transient whose consumer was culled in earlier frames: when the consumer is live again, the recompiled plan
    // reuses the placed resource and must still create the views the new uses need (S froxel light lists).
    RenderGraph g(testDevice());
    uint32_t bufferSrv = gpu::kNone, textureSrv = gpu::kNone;
    for (int f = 0; f < 4; ++f)
    {
        const bool consumer = f >= 2;
        const BufferRef x = g.createBuffer({ "x", 4096, 16 });
        const TextureRef t = g.createTexture({ "t", 64, 64, 1, 1, DXGI_FORMAT_R32_FLOAT });
        g.addPass("write", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(x, Use::UavCompute);
                      b.use(t, Use::UavCompute);
                      b.keep();
                  },
                  [](PassContext&) {});
        if (consumer)
            g.addPass("read", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          b.use(x, Use::SrvCompute);
                          b.use(t, Use::SrvCompute);
                          b.keep();
                      },
                      [&](PassContext& c) {
                          bufferSrv = c.srv(x);
                          textureSrv = c.srv(t);
                      });
        g.execute(nullptr);
        for (uint32_t q = 0; q < kQueueTypeCount; ++q) testDevice().queue((QueueType)q).waitCpu(g.lastFence((QueueType)q));
    }
    CHECK(bufferSrv != gpu::kNone && textureSrv != gpu::kNone);
}

UNX_TEST(graph_depth_memory_not_shared)
{
    // A depth buffer dies, then a 3D UAV texture is written and read back in the same frame. With depth and other
    // textures aliasing the same memory the 3D texture lost its writes [measured, S froxel volume]; every texel must
    // hold what the kernel wrote, and the two never share memory.
    RenderGraph g(testDevice());
    ID3D12PipelineState* fill = shaders().compute("Passes/Test/Fill3D");
    MeshPipelineDesc md;
    md.meshShader = "Passes/Test/DepthField.ms";
    md.depthFormat = DXGI_FORMAT_D32_FLOAT;
    md.cull = D3D12_CULL_MODE_NONE;
    ID3D12PipelineState* field = shaders().mesh("test.depthfield", md);
    const uint32_t w = 80, h = 45, d = 195, dw = 1920, dh = 1152;
    ComPtr<ID3D12Resource> rb;
    uint64_t rowPitch = 0, slicePitch = 0, total = 0;
    TextureRef lastDepth, lastVolume;
    for (int f = 0; f < 2; ++f)
    {
        const TextureRef depth = g.createTexture({ "depth", dw, dh, 1, 1, DXGI_FORMAT_D32_FLOAT });
        const TextureRef volume = g.createTexture({ "volume", w, h, (uint16_t)d, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_DIMENSION_TEXTURE3D });
        const TextureRef after = g.createTexture({ "after depth", 64, 64, 1, 1, DXGI_FORMAT_R32_FLOAT });
        lastDepth = depth;
        lastVolume = volume;
        // Rasterised depth (compressed tiles with varied planes), as a real depth buffer holds.
        g.addPass("depth", QueueType::Graphics, [&](PassBuilder& b) { b.use(depth, Use::DepthWrite); },
                  [=](PassContext& c) {
                      const D3D12_CPU_DESCRIPTOR_HANDLE dsv = c.dsv(depth);
                      c.cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
                      c.cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
                      const D3D12_VIEWPORT vp{ 0, 0, (float)dw, (float)dh, 0, 1 };
                      const D3D12_RECT sc{ 0, 0, (LONG)dw, (LONG)dh };
                      c.cmd->RSSetViewports(1, &vp);
                      c.cmd->RSSetScissorRects(1, &sc);
                      c.cmd->SetPipelineState(field);
                      const uint32_t k[2] = { dw / 16, dh / 16 };
                      c.graphicsConstants(k, 2);
                      c.cmd->DispatchMesh(dw / 16, dh / 16, 1);
                  });
        touchPass(g, "depth read", QueueType::Graphics, { depth }, { after });
        g.addPass("fill", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(after, Use::SrvCompute);  // ordered after the depth's last use
                      b.use(volume, Use::UavCompute);
                  },
                  [=](PassContext& c) {
                      const uint32_t k[4] = { c.uav(volume), w, h, d };
                      c.cmd->SetPipelineState(fill);
                      c.computeConstants(k, 4);
                      c.cmd->Dispatch((w + 3) / 4, (h + 3) / 4, (d + 3) / 4);
                  });
        g.addPass("readback", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(volume, Use::CopySrc);
                      b.keep();
                  },
                  [&](PassContext& c) {
                      ID3D12Resource* src = c.resource(volume);
                      const D3D12_RESOURCE_DESC desc = src->GetDesc();
                      D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
                      UINT rows;
                      UINT64 rowBytes;
                      testDevice().d3d()->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &rowBytes, &total);
                      rowPitch = fp.Footprint.RowPitch;
                      slicePitch = (uint64_t)fp.Footprint.RowPitch * rows;
                      if (!rb)
                      {
                          D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
                          D3D12_RESOURCE_DESC1 rd{};
                          rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                          rd.Width = total;
                          rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
                          rd.SampleDesc.Count = 1;
                          rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                          check(testDevice().d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr,
                                                                             IID_PPV_ARGS(&rb)),
                                "readback");
                      }
                      D3D12_TEXTURE_COPY_LOCATION dst{ rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                      dst.PlacedFootprint = fp;
                      D3D12_TEXTURE_COPY_LOCATION s{ src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                      c.cmd->CopyTextureRegion(&dst, 0, 0, 0, &s, nullptr);
                  });
        g.execute(nullptr);
        for (uint32_t q = 0; q < kQueueTypeCount; ++q) testDevice().queue((QueueType)q).waitCpu(g.lastFence((QueueType)q));
    }
    CHECK(!g.sharesMemory(lastDepth, lastVolume));
    const uint8_t* p = nullptr;
    check(rb->Map(0, nullptr, (void**)&p), "map");
    auto half = [](uint16_t v) {  // IEEE half -> float (normal numbers and zero: what the kernel writes)
        const uint32_t e = (v >> 10) & 31, m = v & 1023;
        return e == 0 ? 0.0f : std::ldexp(1.0f + m / 1024.0f, (int)e - 15) * ((v & 0x8000) ? -1.0f : 1.0f);
    };
    size_t wrong = 0;
    for (uint32_t z = 0; z < d; ++z)
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint16_t* t = reinterpret_cast<const uint16_t*>(p + z * slicePitch + y * rowPitch + x * 8);
                if (half(t[0]) != (float)x || half(t[1]) != (float)y || half(t[2]) != (float)z || half(t[3]) != 1.0f) ++wrong;
            }
    rb->Unmap(0, nullptr);
    logf("    3D texture after a depth buffer: %zu of %u texels wrong\n", wrong, w * h * d);
    CHECK(wrong == 0);
}

UNX_TEST(graph_aliased_buffers_keep_their_writes)
{
    // Buffer B reuses buffer A's memory after A's last use; B is written by a kernel (UAV) or by a copy and read back.
    // Every word must be what B's writer wrote (A's barrier, deactivation and B's first-use barrier order the reuse).
    // In the copy mode A is also written by a copy: in the second frame its first use follows its deactivation as B's
    // predecessor in the first frame (same plan, same placed resource), which must not leave it inaccessible.
    RenderGraph g(testDevice());
    ID3D12PipelineState* fillPso = shaders().compute("Passes/Test/FillBuffer");
    const uint32_t words = 1u << 20;  // 4 MB
    ComPtr<ID3D12Resource> source, rb;
    D3D12_HEAP_PROPERTIES up{ D3D12_HEAP_TYPE_UPLOAD }, rp{ D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC1 rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = (uint64_t)words * 4;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    check(testDevice().d3d()->CreateCommittedResource3(&up, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&source)), "upload");
    check(testDevice().d3d()->CreateCommittedResource3(&rp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&rb)), "readback");
    uint32_t* mapped = nullptr;
    check(source->Map(0, nullptr, (void**)&mapped), "map upload");
    for (uint32_t i = 0; i < words; ++i) mapped[i] = i * 2654435761u;
    source->Unmap(0, nullptr);
    size_t wrong[2] = {};
    for (int mode = 0; mode < 2; ++mode)  // 0: B written by a kernel, 1: B written by a copy
        for (int f = 0; f < 2; ++f)
        {
            const BufferRef a = g.createBuffer({ "a", (uint64_t)words * 4, 0 });
            const BufferRef b = g.createBuffer({ "b", (uint64_t)words * 4, 0 });
            const TextureRef sink = g.createTexture({ "sink", 64, 64, 1, 1, DXGI_FORMAT_R32_FLOAT });
            auto fill = [&](const char* name, BufferRef target, uint32_t seed, BufferRef after) {
                g.addPass(name, QueueType::Graphics,
                          [&](PassBuilder& pb) {
                              if (after.valid()) pb.use(after, Use::SrvCompute);
                              pb.use(target, Use::UavCompute);
                          },
                          [=](PassContext& c) {
                              const uint32_t k[4] = { c.uav(target), words, seed, 0 };
                              c.cmd->SetPipelineState(fillPso);
                              c.computeConstants(k, 4);
                              c.cmd->Dispatch(words / 64, 1, 1);
                          });
            };
            if (mode == 0) fill("fill a", a, 0xA5A5A5A5u, {});
            else
            {
                ID3D12Resource* src = source.Get();
                g.addPass("copy a", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(a, Use::CopyDst); },
                          [=](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(a), 0, src, 0, (uint64_t)words * 4); });
            }
            g.addPass("read a", QueueType::Graphics,
                      [&](PassBuilder& pb) {
                          pb.use(a, Use::SrvCompute);
                          pb.use(sink, Use::UavCompute);
                          pb.keep();
                      },
                      [](PassContext&) {});
            if (mode == 0) fill("fill b", b, 0x5A5A5A5Au, {});
            else
            {
                ID3D12Resource* src = source.Get();
                g.addPass("copy b", QueueType::Graphics,
                          [&](PassBuilder& pb) {
                              pb.use(sink, Use::SrvCompute);
                              pb.use(b, Use::CopyDst);
                          },
                          [=](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(b), 0, src, 0, (uint64_t)words * 4); });
            }
            ID3D12Resource* dst = rb.Get();
            g.addPass("readback b", QueueType::Graphics,
                      [&](PassBuilder& pb) {
                          pb.use(b, Use::CopySrc);
                          pb.keep();
                      },
                      [=](PassContext& c) { c.cmd->CopyBufferRegion(dst, 0, c.resource(b), 0, (uint64_t)words * 4); });
            g.execute(nullptr);
            for (uint32_t q = 0; q < kQueueTypeCount; ++q) testDevice().queue((QueueType)q).waitCpu(g.lastFence((QueueType)q));
            if (f == 1)
            {
                CHECK(g.sharesMemory(a, b));  // the case under test: B reuses A's memory
                const uint32_t* r = nullptr;
                check(rb->Map(0, nullptr, (void**)&r), "map readback");
                for (uint32_t i = 0; i < words; ++i)
                    if (r[i] != (mode == 0 ? (i ^ 0x5A5A5A5Au) : i * 2654435761u)) ++wrong[mode];
                rb->Unmap(0, nullptr);
            }
        }
    logf("    aliased buffer B: %zu wrong words after a kernel write, %zu after a copy (of %u)\n", wrong[0], wrong[1], words);
    CHECK(wrong[0] == 0 && wrong[1] == 0);
}

UNX_TEST(graph_alias_reuse_waits_for_readers)
{
    // Write-after-read across aliases: buffer A is filled by a kernel and read by a long kernel (into C: each thread
    // loads A at the end of a long dependent hash chain); buffer B reuses A's memory. C must hold what A held: B's first
    // write may not start while A's reader still runs. Mode 0: B's first write is a kernel (UAV) right after the reader
    // [S's froxel frame: the VSM search fill written by searchgrid, read by searchdilate, then the froxel light lists
    // written by froxel.begin in the same memory; per-resource barriers on the fill and on the lists did not hold the
    // writes back on the dev GPU, the blocker search bound came out wrong]. Mode 1: A is also read by a copy (into D)
    // and B's first write is a copy from an upload buffer.
    RenderGraph g(testDevice());
    ID3D12PipelineState* fillPso = shaders().compute("Passes/Test/FillBuffer");
    ID3D12PipelineState* hashPso = shaders().compute("Passes/Test/HashWords");
    const uint32_t words = 1u << 20, rounds = 4096, seed = 0xC3C3C3C3u, seedB = 0x3C3C3C3Cu;
    ComPtr<ID3D12Resource> source, rb;
    D3D12_HEAP_PROPERTIES up{ D3D12_HEAP_TYPE_UPLOAD }, rp{ D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC1 rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = (uint64_t)words * 4;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    check(testDevice().d3d()->CreateCommittedResource3(&up, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&source)), "upload");
    rd.Width = (uint64_t)words * 4 * 4;
    check(testDevice().d3d()->CreateCommittedResource3(&rp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&rb)), "readback");
    uint32_t* mapped = nullptr;
    check(source->Map(0, nullptr, (void**)&mapped), "map upload");
    for (uint32_t i = 0; i < words; ++i) mapped[i] = i * 2654435761u;
    source->Unmap(0, nullptr);
    size_t wrong[2][3] = {};
    bool shared = true;
    for (int mode = 0; mode < 2; ++mode)
    for (int f = 0; f < 3; ++f)
    {
        const uint64_t bytes = (uint64_t)words * 4;
        const BufferRef a = g.createBuffer({ "a", bytes, 0 });
        const BufferRef c = g.createBuffer({ "c", 2 * bytes, 0 });  // A ^ hash, then the hashes
        const BufferRef d = mode == 1 ? g.createBuffer({ "d", bytes, 0 }) : BufferRef{};
        const BufferRef b = g.createBuffer({ "b", bytes, 0 });
        g.addPass("fill a", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(a, Use::UavCompute); },
                  [=](PassContext& ctx) {
                      const uint32_t k[4] = { ctx.uav(a), words, seed, 0 };
                      ctx.cmd->SetPipelineState(fillPso);
                      ctx.computeConstants(k, 4);
                      ctx.cmd->Dispatch(words / 64, 1, 1);
                  });
        g.addPass("hash a", QueueType::Graphics,
                  [&](PassBuilder& pb) {
                      pb.use(a, Use::SrvCompute);
                      pb.use(c, Use::UavCompute);
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[4] = { ctx.srv(a), ctx.uav(c), words, rounds };
                      ctx.cmd->SetPipelineState(hashPso);
                      ctx.computeConstants(k, 4);
                      ctx.cmd->Dispatch(words / 64, 1, 1);
                  });
        if (mode == 1)
            g.addPass("copy a", QueueType::Graphics,
                      [&](PassBuilder& pb) {
                          pb.use(a, Use::CopySrc);
                          pb.use(d, Use::CopyDst);
                      },
                      [=](PassContext& ctx) { ctx.cmd->CopyBufferRegion(ctx.resource(d), 0, ctx.resource(a), 0, bytes); });
        ID3D12Resource* src = source.Get();
        if (mode == 1)
            g.addPass("upload b", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(b, Use::CopyDst); },
                      [=](PassContext& ctx) { ctx.cmd->CopyBufferRegion(ctx.resource(b), 0, src, 0, bytes); });
        else
            g.addPass("fill b", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(b, Use::UavCompute); },
                      [=](PassContext& ctx) {
                          const uint32_t k[4] = { ctx.uav(b), words, seedB, 0 };
                          ctx.cmd->SetPipelineState(fillPso);
                          ctx.computeConstants(k, 4);
                          ctx.cmd->Dispatch(words / 64, 1, 1);
                      });
        ID3D12Resource* dst = rb.Get();
        g.addPass("readback", QueueType::Graphics,
                  [&](PassBuilder& pb) {
                      pb.use(b, Use::CopySrc);
                      pb.use(c, Use::CopySrc);
                      if (d.valid()) pb.use(d, Use::CopySrc);
                      pb.keep();
                  },
                  [=](PassContext& ctx) {
                      ctx.cmd->CopyBufferRegion(dst, 0, ctx.resource(b), 0, bytes);
                      ctx.cmd->CopyBufferRegion(dst, bytes, ctx.resource(c), 0, 2 * bytes);
                      if (d.valid()) ctx.cmd->CopyBufferRegion(dst, 3 * bytes, ctx.resource(d), 0, bytes);
                  });
        g.execute(nullptr);
        for (uint32_t q = 0; q < kQueueTypeCount; ++q) testDevice().queue((QueueType)q).waitCpu(g.lastFence((QueueType)q));
        shared = shared && g.sharesMemory(a, b);
        const uint32_t* r = nullptr;
        check(rb->Map(0, nullptr, (void**)&r), "map readback");
        for (uint32_t i = 0; i < words; ++i)
        {
            if (r[i] != (mode == 1 ? i * 2654435761u : (i ^ seedB))) ++wrong[mode][0];
            if ((r[words + i] ^ r[2 * words + i]) != (i ^ seed)) ++wrong[mode][1];
            if (mode == 1 && r[3 * words + i] != (i ^ seed)) ++wrong[mode][2];
        }
        rb->Unmap(0, nullptr);
    }
    for (int mode = 0; mode < 2; ++mode)
        logf("    alias reuse after readers, B written by a %s (3 frames): %zu wrong words in B, %zu in the kernel reader's output, %zu in D (of %u each)\n",
             mode == 0 ? "kernel" : "copy", wrong[mode][0], wrong[mode][1], wrong[mode][2], 3 * words);
    CHECK(shared);  // the case under test: B reuses A's memory
    for (int mode = 0; mode < 2; ++mode) CHECK(wrong[mode][0] == 0 && wrong[mode][1] == 0 && wrong[mode][2] == 0);
}

UNX_TEST(graph_imported_views_follow_their_resource)
{
    // Imported resources' views are cached across frames. The cache holds a reference to each resource while cached (no
    // other resource can take its address), rebuilds the views when the declared description changes, and drops the
    // entry (views and reference, after the GPU) once a frame does not import it. Then resources created and destroyed
    // at one size, so their addresses may repeat, each get views of their own [M: a raw-pointer key handed a new
    // resource a destroyed one's descriptors: GBV "invalid resource pointed to by descriptor", DEVICE_HUNG].
    RenderGraph g(testDevice());
    ID3D12PipelineState* fillPso = shaders().compute("Passes/Test/FillBuffer");
    const uint32_t words = 1u << 18;  // 1 MB
    auto makeBuffer = [&](bool readback) {
        D3D12_HEAP_PROPERTIES hp{ readback ? D3D12_HEAP_TYPE_READBACK : D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC1 rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (uint64_t)words * 4;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = readback ? D3D12_RESOURCE_FLAG_NONE : D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> r;
        check(testDevice().d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)),
              "test buffer");
        return r;
    };
    ComPtr<ID3D12Resource> rb = makeBuffer(true);
    auto refs = [](ID3D12Resource* r) {
        r->AddRef();
        return r->Release();
    };
    auto finish = [&]() {
        for (uint32_t q = 0; q < kQueueTypeCount; ++q) testDevice().queue((QueueType)q).waitCpu(g.lastFence((QueueType)q));
        testDevice().collectGarbage();
    };
    // One frame: fill 'count' words of the imported buffer (declared 'declared' words) with i ^ seed and read it back.
    auto frame = [&](ID3D12Resource* res, uint32_t declared, uint32_t count, uint32_t seed) {
        const BufferRef b = g.importBuffer(res, { "imported", (uint64_t)declared * 4, 0 });
        g.addPass("fill", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(b, Use::UavCompute); },
                  [=](PassContext& c) {
                      const uint32_t k[4] = { c.uav(b), count, seed, 0 };
                      c.cmd->SetPipelineState(fillPso);
                      c.computeConstants(k, 4);
                      c.cmd->Dispatch((count + 63) / 64, 1, 1);
                  });
        ID3D12Resource* dst = rb.Get();
        g.addPass("readback", QueueType::Graphics,
                  [&](PassBuilder& pb) {
                      pb.use(b, Use::CopySrc);
                      pb.keep();
                  },
                  [=](PassContext& c) { c.cmd->CopyBufferRegion(dst, 0, c.resource(b), 0, (uint64_t)words * 4); });
        g.execute(nullptr);
        finish();
        const uint32_t* r = nullptr;
        size_t wrong = 0;
        check(rb->Map(0, nullptr, (void**)&r), "map readback");
        for (uint32_t i = 0; i < count; ++i)
            if (r[i] != (i ^ seed)) ++wrong;
        rb->Unmap(0, nullptr);
        return wrong;
    };
    auto idle = [&]() {  // a frame that imports nothing
        g.addPass("idle", QueueType::Graphics, [&](PassBuilder& pb) { pb.keep(); }, [](PassContext&) {});
        g.execute(nullptr);
        finish();
    };

    ComPtr<ID3D12Resource> a = makeBuffer(false);
    const ULONG base = refs(a.Get());
    CHECK(frame(a.Get(), words / 2, words / 2, 0x1111u) == 0);
    CHECK(refs(a.Get()) == base + 1);                          // held while cached
    CHECK(frame(a.Get(), words, words, 0x2222u) == 0);         // declared twice as large: new views reach every word
    idle();
    idle();
    CHECK(refs(a.Get()) == base);                              // not imported: dropped after the GPU
    a.Reset();

    size_t wrong = 0;
    for (uint32_t k = 0; k < 24; ++k)  // same size each time: addresses may repeat once a buffer is gone
    {
        ComPtr<ID3D12Resource> b = makeBuffer(false);
        wrong += frame(b.Get(), words, words, 0x9E3779B9u * (k + 1));
        b.Reset();
        idle();  // (the cache lets go of it here)
    }
    logf("    imported buffers created and destroyed 24 times at one size: %zu wrong words\n", wrong);
    CHECK(wrong == 0);
}

UNX_TEST(shader_library_refuses_kernels_of_another_abi)
{
    // bin/shaders/abi.stamp (Native/Render/CMakeLists.txt): kernels built for another binding contract (root constants,
    // frame constants, scene records) are refused when the library opens, and so is a folder without a stamp; the real
    // kernel folder (and a sub-folder of it) opens.
    const std::filesystem::path root = executableDirectory() / "abi_check";
    std::filesystem::remove_all(root);
    std::filesystem::create_directories(root / "none");
    std::filesystem::create_directories(root / "other");
    {
        std::ofstream(root / "other" / "abi.stamp") << "0123456789abcdef";
    }
    auto message = [&](const std::filesystem::path& dir) {
        try { ShaderLibrary lib(testDevice(), dir); } catch (const std::exception& e) { return std::string(e.what()); }
        return std::string();
    };
    const std::string other = message(root / "other"), none = message(root / "none");
    logf("    %s\n    %s\n", other.c_str(), none.c_str());
    CHECK(other.find("ABI mismatch") != std::string::npos && other.find("0123456789abcdef") != std::string::npos);
    CHECK(none.find("no abi.stamp") != std::string::npos);
    CHECK(message(executableDirectory() / "shaders").empty());
    CHECK(message(executableDirectory() / "shaders" / "Passes").empty());
    std::filesystem::remove_all(root);
}

UNX_TEST(graph_import_without_view_flag_fails_at_record)
{
    // An imported resource used as a UAV without D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS: the graph refuses it with an
    // error naming the resource before anything reaches the GPU (without the check, a run without the debug layer removed
    // the device - engine 2's volume gate, twice).
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC1 rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = 4096;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    check(testDevice().d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)),
          "buffer without UAV flag");
    std::string message;
    try
    {
        RenderGraph g(testDevice());
        const BufferRef b = g.importBuffer(r.Get(), { "no uav flag", 4096, 0 });
        g.addPass("write", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(b, Use::UavCompute); }, [=](PassContext& c) { (void)c.uav(b); });
        g.execute(nullptr);
    }
    catch (const std::exception& e)
    {
        message = e.what();
    }
    logf("    %s\n", message.c_str());
    CHECK(message.find("no uav flag") != std::string::npos && message.find("UAV") != std::string::npos);
    CHECK(testDevice().drainDebugMessages() == 0);
}

UNX_TEST(graph_banded_group_covers_every_row)
{
    // RenderGraph::addBandedGroup: a producer and a consumer pass over a W x H grid recorded band by band (A0 B0 A1 B1
    // ...): every cell is produced and consumed exactly as in one full-screen pass pair, the bands tile the rows on
    // 8-row boundaries, and the group declares passes x bands passes.
    RenderGraph g(testDevice());
    ID3D12PipelineState* produce = shaders().compute("Passes/Test/BandRows.MODE0");
    ID3D12PipelineState* consume = shaders().compute("Passes/Test/BandRows.MODE1");
    const uint32_t width = 257, height = 999, cells = width * height;
    ComPtr<ID3D12Resource> rb;
    {
        D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC1 rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (uint64_t)cells * 4;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(testDevice().d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&rb)), "readback");
    }
    for (uint32_t bands : { 1u, 3u, 8u })
    {
        const BufferRef inter = g.createBuffer({ "inter", (uint64_t)cells * 4, 0 });
        const BufferRef out = g.createBuffer({ "out", (uint64_t)cells * 4, 0 });
        std::vector<std::pair<uint32_t, uint32_t>> rows;
        RenderGraph::BandedPass a, b;
        a.name = "produce";
        a.setup = [&](PassBuilder& pb) { pb.use(inter, Use::UavCompute); };
        a.execute = [=](PassContext& c) {
            const uint32_t k[8] = { c.uav(inter), 0, width, height, c.band.y0, c.band.y1, 0, 0 };
            c.cmd->SetPipelineState(produce);
            c.computeConstants(k, 8);
            c.cmd->Dispatch((width + 63) / 64, c.band.y1 - c.band.y0, 1);
        };
        b.name = "consume";
        b.setup = [&](PassBuilder& pb) {
            pb.use(inter, Use::SrvCompute);
            pb.use(out, Use::UavCompute);
        };
        b.execute = [=, &rows](PassContext& c) {
            rows.push_back({ c.band.y0, c.band.y1 });
            const uint32_t k[8] = { c.srv(inter), c.uav(out), width, height, c.band.y0, c.band.y1, 0, 0 };
            c.cmd->SetPipelineState(consume);
            c.computeConstants(k, 8);
            c.cmd->Dispatch((width + 63) / 64, c.band.y1 - c.band.y0, 1);
        };
        g.addBandedGroup("test", height, bands, { a, b });
        ID3D12Resource* dst = rb.Get();
        g.addPass("readback", QueueType::Graphics,
                  [&](PassBuilder& pb) {
                      pb.use(out, Use::CopySrc);
                      pb.keep();
                  },
                  [=](PassContext& c) { c.cmd->CopyBufferRegion(dst, 0, c.resource(out), 0, (uint64_t)cells * 4); });
        g.execute(nullptr);
        for (uint32_t q = 0; q < kQueueTypeCount; ++q) testDevice().queue((QueueType)q).waitCpu(g.lastFence((QueueType)q));
        CHECK(g.stats().livePasses == 2 * bands + 1);
        CHECK(rows.size() == bands && rows.front().first == 0 && rows.back().second == height);
        for (uint32_t k = 0; k < bands; ++k) CHECK(rows[k].first == passBand(height, bands, k).y0 && rows[k].second == passBand(height, bands, k).y1);
        for (size_t k = 0; k < rows.size(); ++k)
        {
            CHECK(rows[k].first % 8 == 0 && rows[k].first < rows[k].second);
            if (k > 0) CHECK(rows[k].first == rows[k - 1].second);
        }
        const uint32_t* r = nullptr;
        check(rb->Map(0, nullptr, (void**)&r), "map");
        size_t wrong = 0;
        for (uint32_t i = 0; i < cells; ++i)
            if (r[i] != ((i * 2654435761u) ^ 0x5A5A5A5Au)) ++wrong;
        rb->Unmap(0, nullptr);
        logf("    %u bands over %u rows: %zu wrong cells of %u\n", bands, height, wrong, cells);
        CHECK(wrong == 0);
    }
}

UNX_TEST(graph_band_rows_and_lag)
{
    // passBand cuts exactly the bands addBandedGroup records, and PassBand::lagged ranges tile the view for any lag with
    // every non-empty lagged range reading only rows its own or an earlier band produced (y1' + rows <= y1).
    size_t cases = 0;
    for (uint32_t height : { 1u, 7u, 8u, 20u, 999u, 1440u, 2160u })
        for (uint32_t count : { 1u, 2u, 3u, 8u, 13u, 300u })
            for (uint32_t rows : { 0u, 1u, 8u, 16u })
            {
                uint32_t plainEnd = 0, laggedEnd = 0;
                for (uint32_t b = 0; b < count; ++b)
                {
                    const PassBand band = passBand(height, count, b);
                    CHECK(band.index == b && band.count == count && band.y0 == plainEnd && band.y0 <= band.y1);
                    CHECK(b + 1 == count || band.y1 % 8 == 0);
                    const PassBand late = band.lagged(rows);
                    CHECK(late.y0 == laggedEnd && late.y0 <= late.y1);
                    if (late.y1 > late.y0 && b + 1 < count) CHECK(late.y1 + rows <= band.y1);
                    plainEnd = band.y1;
                    laggedEnd = late.y1;
                    ++cases;
                }
                CHECK(plainEnd == height && laggedEnd == height);
            }
    // A pass outside a group: band 0 of 1 over every row, unchanged by a lag.
    const PassBand whole;
    CHECK(whole.lagged(8).y0 == 0 && whole.lagged(8).y1 == UINT32_MAX);
    logf("    %zu bands checked\n", cases);
}

UNX_TEST(graph_castable_view_formats)
{
    // TextureDesc::srvFormat/uavFormat: written as R32_UINT through the UAV, read as R9G9B9E5_SHAREDEXP through the SRV
    // (the hardware decodes). Every texel must decode to what the packed bits say (R's filterable K-path maps).
    if (!testDevice().caps().relaxedFormatCasting)
    {
        logf("    relaxed format casting not supported: skipped\n");
        return;
    }
    RenderGraph g(testDevice());
    ID3D12PipelineState* write = shaders().compute("Passes/Test/CastFormat.MODE0");
    ID3D12PipelineState* read = shaders().compute("Passes/Test/CastFormat.MODE1");
    const uint32_t n = 64;
    ComPtr<ID3D12Resource> rb = [&] {
        D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC1 rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (uint64_t)n * n * 16;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;
        check(testDevice().d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)),
              "readback");
        return r;
    }();
    TextureDesc td{ "cast", n, n, 1, 1, DXGI_FORMAT_R32_UINT };
    td.srvFormat = DXGI_FORMAT_R9G9B9E5_SHAREDEXP;
    const TextureRef t = g.createTexture(td);
    const BufferRef out = g.createBuffer({ "decoded", (uint64_t)n * n * 16, 0 });
    g.addPass("write", QueueType::Graphics, [&](PassBuilder& b) { b.use(t, Use::UavCompute); },
              [=](PassContext& c) {
                  const uint32_t k[4] = { c.uav(t), n, 0, 0 };
                  c.cmd->SetPipelineState(write);
                  c.computeConstants(k, 4);
                  c.cmd->Dispatch(n / 8, n / 8, 1);
              });
    g.addPass("read", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(t, Use::SrvCompute);
                  b.use(out, Use::UavCompute);
              },
              [=](PassContext& c) {
                  const uint32_t k[4] = { c.srv(t), n, c.uav(out), 0 };
                  c.cmd->SetPipelineState(read);
                  c.computeConstants(k, 4);
                  c.cmd->Dispatch(n / 8, n / 8, 1);
              });
    ID3D12Resource* dst = rb.Get();
    g.addPass("readback", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(out, Use::CopySrc);
                  b.keep();
              },
              [=](PassContext& c) { c.cmd->CopyBufferRegion(dst, 0, c.resource(out), 0, (uint64_t)n * n * 16); });
    g.execute(nullptr);
    for (uint32_t q = 0; q < kQueueTypeCount; ++q) testDevice().queue((QueueType)q).waitCpu(g.lastFence((QueueType)q));
    const float* v = nullptr;
    check(rb->Map(0, nullptr, (void**)&v), "map");
    size_t wrong = 0;
    for (uint32_t y = 0; y < n; ++y)
        for (uint32_t x = 0; x < n; ++x)
        {
            const uint32_t bits = ((x * 37) & 511) | (((y * 59) & 511) << 9) | ((((x + y) * 13) & 511) << 18) | ((10 + ((x + y) & 7)) << 27);
            const int e = (int)(bits >> 27) - 15 - 9;
            const float expect[3] = { std::ldexp((float)(bits & 511), e), std::ldexp((float)((bits >> 9) & 511), e), std::ldexp((float)((bits >> 18) & 511), e) };
            const float* got = v + 4 * (y * n + x);
            if (got[0] != expect[0] || got[1] != expect[1] || got[2] != expect[2] || got[3] != 1.0f) ++wrong;
        }
    rb->Unmap(0, nullptr);
    logf("    R32_UINT written, R9G9B9E5 read: %zu of %u texels wrong\n", wrong, n * n);
    CHECK(wrong == 0);
}

UNX_TEST(skin_normals_use_the_cofactor)
{
    // Deformation.hlsli cofactorNormal (I request, skin normals): the GPU result equals the inverse transpose of the joint
    // 3x3 applied to the normal (normalised), for rotation + uniform scale, non-uniform scale, shear and a mirror.
    ID3D12PipelineState* pso = shaders().compute("Passes/Test/CofactorNormal");
    RenderGraph g(testDevice());
    struct Case
    {
        float a[3][3];
        float3 n;
    };
    const float c = std::cos(0.7f), s = std::sin(0.7f);
    const Case cases[] = {
        { { { 2 * c, -2 * s, 0 }, { 2 * s, 2 * c, 0 }, { 0, 0, 2 } }, normalize(float3{ 0.3f, 0.5f, 0.8f }) },  // rotation x uniform scale
        { { { 0.6f, 0, 0 }, { 0, 0.8f, 0 }, { 0, 0, 0.6f } }, normalize(float3{ 1, 1, 0 }) },                  // host part scale
        { { { 1, 0.7f, 0 }, { 0, 1, 0 }, { 0, 0, 1 } }, normalize(float3{ 0, 1, 0.2f }) },                    // shear
        { { { -1, 0, 0 }, { 0, 1.5f, 0 }, { 0, 0, 1 } }, normalize(float3{ 0.6f, 0.8f, 0 }) },                // mirror
    };
    std::vector<float3> gpu(std::size(cases));
    for (size_t k = 0; k < std::size(cases); ++k)
    {
        const Case& cs = cases[k];
        const BufferRef out = g.createBuffer({ "cofactor out", 256, 0 });
        auto rb = std::make_shared<ComPtr<ID3D12Resource>>();
        {
            D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
            D3D12_RESOURCE_DESC1 rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = 256;
            rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            check(testDevice().d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&*rb)),
                  "readback");
        }
        g.addPass("cofactor", QueueType::Graphics, [&](PassBuilder& b) { b.use(out, Use::UavCompute); },
                  [=](PassContext& ctx) {
                      uint32_t k[16] = {};
                      for (int r = 0; r < 3; ++r) std::memcpy(&k[4 * r], cs.a[r], 12);
                      std::memcpy(&k[12], &cs.n, 12);
                      k[15] = ctx.uav(out);
                      ctx.cmd->SetPipelineState(pso);
                      ctx.computeConstants(k, 16);
                      ctx.cmd->Dispatch(1, 1, 1);
                  });
        ID3D12Resource* dst = rb->Get();
        g.addPass("cofactor readback", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(out, Use::CopySrc);
                      b.keep();
                  },
                  [=](PassContext& ctx) { ctx.cmd->CopyBufferRegion(dst, 0, ctx.resource(out), 0, 12); });
        g.execute(nullptr);
        for (uint32_t q = 0; q < kQueueTypeCount; ++q) testDevice().queue((QueueType)q).waitCpu(g.lastFence((QueueType)q));
        float* v = nullptr;
        check((*rb)->Map(0, nullptr, (void**)&v), "map");
        gpu[k] = { v[0], v[1], v[2] };
        (*rb)->Unmap(0, nullptr);
    }
    float worst = 0;
    for (size_t k = 0; k < std::size(cases); ++k)
    {
        // CPU: inverse transpose of the 3x3 (cofactor / det) applied to n.
        const auto& a = cases[k].a;
        const double det = a[0][0] * (a[1][1] * a[2][2] - a[1][2] * a[2][1]) - a[0][1] * (a[1][0] * a[2][2] - a[1][2] * a[2][0]) + a[0][2] * (a[1][0] * a[2][1] - a[1][1] * a[2][0]);
        double cof[3][3];
        for (int r = 0; r < 3; ++r)
            for (int col = 0; col < 3; ++col)
            {
                const int r1 = (r + 1) % 3, r2 = (r + 2) % 3, c1 = (col + 1) % 3, c2 = (col + 2) % 3;
                cof[r][col] = a[r1][c1] * a[r2][c2] - a[r1][c2] * a[r2][c1];
            }
        const float3 n = cases[k].n;
        double e[3];
        for (int r = 0; r < 3; ++r) e[r] = (cof[r][0] * n.x + cof[r][1] * n.y + cof[r][2] * n.z) / det;  // (A^-T n)_r = cof[r] . n / det
        const double len = std::sqrt(e[0] * e[0] + e[1] * e[1] + e[2] * e[2]);
        const float3 expect{ (float)(e[0] / len), (float)(e[1] / len), (float)(e[2] / len) };
        worst = std::max({ worst, std::fabs(gpu[k].x - expect.x), std::fabs(gpu[k].y - expect.y), std::fabs(gpu[k].z - expect.z) });
    }
    logf("    skin normal (cofactor) vs inverse transpose: worst component difference %.2e over %zu joints\n", worst, std::size(cases));
    CHECK(worst < 1e-5f);
}

UNX_TEST(material_specular_albedo_split)
{
    // scene::model::specularAlbedoTable (8.1, v1.25): the split (A, B) of the same samples as E, so A + B = E per grid
    // point, both non-negative, and the bilinear reads agree with E's.
    using namespace scene::model;
    const auto& e = directionalAlbedoTable();
    const auto& ab = specularAlbedoTable();
    CHECK(ab.size() == 2 * e.size());
    float worst = 0, worstRead = 0;
    bool negative = false;
    for (size_t i = 0; i < e.size(); ++i)
    {
        worst = std::max(worst, std::fabs(ab[2 * i] + ab[2 * i + 1] - e[i]));
        negative = negative || ab[2 * i] < 0 || ab[2 * i + 1] < 0;
    }
    for (float mu = 0.013f; mu < 1; mu += 0.071f)
        for (float r = 0.007f; r < 1; r += 0.093f)
        {
            const float2 v = specularAlbedo(mu, r);
            worstRead = std::max(worstRead, std::fabs(v.x + v.y - directionalAlbedo(mu, r)));
        }
    logf("    (A, B) vs E: worst |A + B - E| %.2e per grid point, %.2e in bilinear reads\n", worst, worstRead);
    CHECK(!negative && worst < 1e-5f && worstRead < 1e-5f);
}

UNX_TEST(graph_single_queue_is_one_list)
{
    // Default policy: compute-queue passes run on the graphics queue; the frame is one command list.
    RenderGraph g(testDevice());
    runFrames(g, [](RenderGraph& graph) {
        TextureRef a = graph.createTexture({ "a", 512, 512, 1, 1, DXGI_FORMAT_R32_FLOAT });
        TextureRef b = graph.createTexture({ "b", 512, 512, 1, 1, DXGI_FORMAT_R32_FLOAT });
        touchPass(graph, "gfx a", QueueType::Graphics, {}, { a });
        touchPass(graph, "async a->b", QueueType::Compute, { a }, { b });
        graph.addPass("keep", QueueType::Graphics, [&](PassBuilder& p) { p.use(b, Use::SrvCompute); p.keep(); }, [](PassContext&) {});
    });
    CHECK(g.stats().crossQueueSyncs == 0);
    CHECK(g.stats().commandLists == 1);
}

UNX_TEST(graph_acceleration_structure_uses)
{
    // GPU-written build inputs -> BLAS build -> TLAS build -> inline ray, all in one frame with graph barriers only
    // (Use::AccelerationStructure*, INTERFACES_KO.md 4). The ray hits the triangle at t = 5 only if every step waited.
    Device& dev = testDevice();
    ID3D12Device5* d5 = nullptr;
    check(dev.d3d()->QueryInterface(IID_PPV_ARGS(&d5)), "ID3D12Device5");
    D3D12_RAYTRACING_GEOMETRY_DESC geom{};
    geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
    geom.Triangles.VertexCount = 3;
    geom.Triangles.VertexBuffer.StrideInBytes = 12;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blasIn{};
    blasIn.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    blasIn.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    blasIn.NumDescs = 1;
    blasIn.pGeometryDescs = &geom;
    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasIn{};
    tlasIn.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tlasIn.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tlasIn.NumDescs = 1;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blasInfo{}, tlasInfo{};
    d5->GetRaytracingAccelerationStructurePrebuildInfo(&blasIn, &blasInfo);
    d5->GetRaytracingAccelerationStructurePrebuildInfo(&tlasIn, &tlasInfo);
    d5->Release();

    auto buffer = [&](uint64_t bytes, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_FLAGS flags, const wchar_t* name) {
        D3D12_HEAP_PROPERTIES hp{ heapType };
        D3D12_RESOURCE_DESC1 rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = flags;
        ComPtr<ID3D12Resource> r;
        check(dev.d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)), "test buffer");
        r->SetName(name);
        return r;
    };
    const D3D12_RESOURCE_FLAGS asFlags = D3D12_RESOURCE_FLAG_RAYTRACING_ACCELERATION_STRUCTURE | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> blas = buffer(blasInfo.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, asFlags, L"test blas");
    ComPtr<ID3D12Resource> tlas = buffer(tlasInfo.ResultDataMaxSizeInBytes, D3D12_HEAP_TYPE_DEFAULT, asFlags, L"test tlas");
    ComPtr<ID3D12Resource> readback = buffer(256, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE, L"test readback");
    const uint32_t tlasSrv = dev.descriptors().allocateResource();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_RAYTRACING_ACCELERATION_STRUCTURE;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.RaytracingAccelerationStructure.Location = tlas->GetGPUVirtualAddress();
    dev.d3d()->CreateShaderResourceView(nullptr, &sd, dev.descriptors().resourceCpu(tlasSrv));

    ID3D12PipelineState* writePso = shaders().compute("Passes/Test/AsTestWrite");
    ID3D12PipelineState* tracePso = shaders().compute("Passes/Test/AsTestTrace");
    RenderGraph g(dev);
    runFrames(g, [&](RenderGraph& graph) {
        BufferRef blasRef = graph.importBuffer(blas.Get(), { "blas", blasInfo.ResultDataMaxSizeInBytes, 0 });
        BufferRef tlasRef = graph.importBuffer(tlas.Get(), { "tlas", tlasInfo.ResultDataMaxSizeInBytes, 0 });
        BufferRef vertices = graph.createBuffer({ "vertices", 48, 0 });
        BufferRef instances = graph.createBuffer({ "instances", 64, 0 });
        BufferRef blasScratch = graph.createBuffer({ "blas scratch", blasInfo.ScratchDataSizeInBytes, 0 });
        BufferRef tlasScratch = graph.createBuffer({ "tlas scratch", tlasInfo.ScratchDataSizeInBytes, 0 });
        BufferRef result = graph.createBuffer({ "result", 16, 0 });
        graph.addPass("write inputs", QueueType::Graphics, [&](PassBuilder& p) { p.use(vertices, Use::UavCompute); p.use(instances, Use::UavCompute); },
                      [&, vertices, instances](PassContext& c) {
                          const D3D12_GPU_VIRTUAL_ADDRESS a = blas->GetGPUVirtualAddress();
                          const uint32_t k[4] = { c.uav(vertices), c.uav(instances), (uint32_t)a, (uint32_t)(a >> 32) };
                          c.cmd->SetPipelineState(writePso);
                          c.computeConstants(k, 4);
                          c.cmd->Dispatch(1, 1, 1);
                      });
        graph.addPass("build blas", QueueType::Graphics,
                      [&](PassBuilder& p) { p.use(vertices, Use::AccelerationStructureInput); p.use(blasRef, Use::AccelerationStructureWrite); p.use(blasScratch, Use::AccelerationStructureScratch); },
                      [=](PassContext& c) {
                          D3D12_RAYTRACING_GEOMETRY_DESC gd = geom;
                          gd.Triangles.VertexBuffer.StartAddress = c.address(vertices);
                          D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC b{};
                          b.Inputs = blasIn;
                          b.Inputs.pGeometryDescs = &gd;
                          b.DestAccelerationStructureData = c.address(blasRef);
                          b.ScratchAccelerationStructureData = c.address(blasScratch);
                          c.cmd->BuildRaytracingAccelerationStructure(&b, 0, nullptr);
                      });
        graph.addPass("build tlas", QueueType::Graphics,
                      [&](PassBuilder& p) {
                          p.use(instances, Use::AccelerationStructureInput);
                          p.use(blasRef, Use::AccelerationStructureRead);
                          p.use(tlasRef, Use::AccelerationStructureWrite);
                          p.use(tlasScratch, Use::AccelerationStructureScratch);
                      },
                      [=](PassContext& c) {
                          D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC b{};
                          b.Inputs = tlasIn;
                          b.Inputs.InstanceDescs = c.address(instances);
                          b.DestAccelerationStructureData = c.address(tlasRef);
                          b.ScratchAccelerationStructureData = c.address(tlasScratch);
                          c.cmd->BuildRaytracingAccelerationStructure(&b, 0, nullptr);
                      });
        graph.addPass("trace", QueueType::Graphics, [&](PassBuilder& p) { p.use(tlasRef, Use::AccelerationStructureRead); p.use(result, Use::UavCompute); },
                      [=](PassContext& c) {
                          const uint32_t k[2] = { tlasSrv, c.uav(result) };
                          c.cmd->SetPipelineState(tracePso);
                          c.computeConstants(k, 2);
                          c.cmd->Dispatch(1, 1, 1);
                      });
        graph.addPass("readback", QueueType::Graphics, [&](PassBuilder& p) { p.use(result, Use::CopySrc); p.keep(); },
                      [&, result](PassContext& c) { c.cmd->CopyBufferRegion(readback.Get(), 0, c.resource(result), 0, 16); });
    });
    float t = 0;
    void* mapped = nullptr;
    D3D12_RANGE range{ 0, 4 };
    check(readback->Map(0, &range, &mapped), "map readback");
    std::memcpy(&t, mapped, 4);
    readback->Unmap(0, nullptr);
    logf("    inline ray hit distance %.6f (expected 5)\n", t);
    CHECK(std::fabs(t - 5.0f) < 1e-4f);
    dev.deferRelease(blas);
    dev.deferRelease(tlas);
    dev.deferRelease(readback);
}

UNX_TEST(graph_empty_frame_scene_is_valid)
{
    // The full 120-pass gate graph at 4K with the debug layer: no errors, plan reuse, aliasing effective.
    QualityConfig q = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
    Resolution r = resolutionFromString("4K", q);
    test::EmptyFrameScene scene(testDevice(), shaders(), r.width, r.height);
    for (bool async : { true, false })
    {
        RenderGraph g(testDevice());
        g.setAsyncCompute(async);
        uint64_t frame = 0;
        runFrames(g, [&](RenderGraph& graph) { scene.build(graph, frame++); }, 4);
        const RenderGraphStats& s = g.stats();
        logf("    120-pass graph (async %d): %u live, %u barriers / %u batches, %u syncs, %u lists, %.1f of %.1f MB\n", async, s.livePasses, s.barriers, s.barrierBatches,
             s.crossQueueSyncs, s.commandLists, s.transientBytesAliased / 1048576.0, s.transientBytesUnaliased / 1048576.0);
        CHECK(s.livePasses == test::EmptyFrameScene::kPassCount);
        CHECK(s.planReused);
        CHECK(s.transientBytesAliased < s.transientBytesUnaliased);
        CHECK(async || s.commandLists == 1);
    }
}

namespace
{
scene::Scene tinyScene()
{
    scene::Scene s;
    s.name = "tiny";
    scene::Material m;
    m.name = "grey";
    s.materials.push_back(m);
    scene::Mesh mesh;
    mesh.name = "quad";
    mesh.positions = { { -1, 0, -1 }, { 1, 0, -1 }, { 1, 0, 1 }, { -1, 0, 1 } };
    mesh.normals = { { 0, 1, 0 }, { 0, 1, 0 }, { 0, 1, 0 }, { 0, 1, 0 } };
    mesh.uv0 = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    mesh.indices = { 0, 2, 1, 0, 3, 2 };
    mesh.submeshes = { { 0, 6, 0 } };
    s.meshes.push_back(mesh);
    scene::Instance inst;
    inst.mesh = 0;
    s.instances.push_back(inst);
    scene::Light l;
    l.position = { 0, 2, 0 };
    s.lights.push_back(l);
    scene::Camera cam;
    cam.name = "main";
    cam.position = { 0, 1.7f, 5 };
    s.cameras.push_back(cam);
    return s;
}
} // namespace

UNX_TEST(scene_roundtrip_and_validation)
{
    scene::Scene s = tinyScene();
    scene::validate(s);
    const auto bytes = scene::serialize(s);
    scene::Scene back = scene::deserialize(bytes);
    CHECK(scene::serialize(back) == bytes);
    CHECK(scene::contentHash(back) == scene::contentHash(s));
    scene::Scene bad = s;
    bad.meshes[0].indices.push_back(99);
    CHECK(throws([&] { scene::validate(bad); }));
    bad = s;
    bad.instances[0].transform.m[0][0] = 2;  // non-uniform scale
    CHECK(throws([&] { scene::validate(bad); }));
    bad = s;
    bad.materials[0].normalTexture = 0;  // normal map without tangents (and a missing texture)
    CHECK(throws([&] { scene::validate(bad); }));

    // v1.66 extension blocks: hair and cut parameters survive the round trip; a scene without such materials keeps its
    // bytes (the blocks are written only when used), so earlier content hashes stay valid.
    scene::Scene ext = s;
    scene::Material hair = s.materials[0];
    hair.cls = scene::MaterialClass::Hair;
    hair.ior = 1.55f;
    hair.hairEumelanin = 1.3f, hair.hairPheomelanin = 0.2f, hair.hairBetaN = 0.4f, hair.hairTilt = 0.05f;
    scene::Material cut = s.materials[0];
    cut.cls = scene::MaterialClass::Cut;
    cut.cutScale = 2.5f, cut.cutDamageWidth = 0.03f;
    ext.materials.push_back(hair);
    ext.materials.push_back(cut);
    scene::validate(ext);
    const scene::Scene extBack = scene::deserialize(scene::serialize(ext));
    const scene::Material& h2 = extBack.materials[extBack.materials.size() - 2];
    const scene::Material& c2 = extBack.materials.back();
    CHECK(h2.hairEumelanin == 1.3f && h2.hairPheomelanin == 0.2f && h2.hairBetaN == 0.4f && h2.hairTilt == 0.05f);
    CHECK(c2.cutScale == 2.5f && c2.cutDamageWidth == 0.03f);
    CHECK(scene::serialize(extBack) == scene::serialize(ext));
    CHECK(scene::serialize(s) == bytes);
    scene::Scene badCut = ext;
    badCut.materials.back().cutScale = 0;
    CHECK(throws([&] { scene::validate(badCut); }));

    // v1.74 terrain block: splats and layers survive the round trip; layer materials must be Standard, splats Rgba8Linear
    scene::Scene ter = s;
    scene::Texture splat;
    splat.name = "splat";
    splat.width = splat.height = 2;
    splat.format = scene::TextureFormat::Rgba8Linear;
    splat.texels.assign(16, 64);
    ter.textures.push_back(splat);
    scene::Material t = s.materials[0];
    t.cls = scene::MaterialClass::Terrain;
    t.terrainSplat[0] = (uint32_t)ter.textures.size() - 1;
    t.terrainLayers = { { 0, { 4, 5 }, { 0.25f, -0.5f } }, { 0, { 2, 2 }, { 0, 0 } } };
    ter.materials.push_back(t);
    scene::validate(ter);
    const scene::Scene terBack = scene::deserialize(scene::serialize(ter));
    const scene::Material& t2 = terBack.materials.back();
    CHECK(t2.cls == scene::MaterialClass::Terrain && t2.terrainSplat[0] == t.terrainSplat[0] && t2.terrainSplat[1] == scene::kNone && t2.terrainLayers.size() == 2 &&
          t2.terrainLayers[0].scale.y == 5 && t2.terrainLayers[0].offset.y == -0.5f);
    CHECK(scene::serialize(terBack) == scene::serialize(ter));
    scene::Scene badTer = ter;
    badTer.materials.back().terrainLayers.resize(5);  // above 4 layers without splat 1
    CHECK(throws([&] { scene::validate(badTer); }));
    badTer = ter;
    badTer.materials.back().terrainLayers[1].material = (uint32_t)badTer.materials.size() - 1;  // a terrain layer that is not Standard
    CHECK(throws([&] { scene::validate(badTer); }));

    // hair absorption: melanin (d'Eon 2011) and target colour (Chiang 2016, the inverse of its albedo fit)
    const float3 melanin = scene::model::hairAbsorption(hair);
    CHECK(std::abs(melanin.x - (0.419f * 1.3f + 0.187f * 0.2f)) < 1e-6f && std::abs(melanin.z - (1.37f * 1.3f + 1.05f * 0.2f)) < 1e-6f);
    scene::Material colour = hair;
    colour.hairEumelanin = colour.hairPheomelanin = 0;
    colour.baseColor = { 1.0f, 0.5f, 0.1f };
    const float3 sa = scene::model::hairAbsorption(colour);
    const double b = 0.4, d = 5.969 - 0.215 * b + 2.532 * b * b - 10.73 * b * b * b + 5.574 * b * b * b * b + 0.245 * b * b * b * b * b;
    CHECK(sa.x == 0.0f && std::abs(sa.y - std::pow(std::log(0.5) / d, 2)) < 1e-6 && std::abs(sa.z - std::pow(std::log(0.1) / d, 2)) < 1e-5);
}

UNX_TEST(material_model_table)
{
    using namespace scene::model;
    const auto& t = directionalAlbedoTable();
    CHECK(t.size() == kAlbedoTableSize * kAlbedoTableSize);
    for (uint32_t i = 0; i < t.size(); ++i)
        if (!(t[i] > 0.0f && t[i] <= 1.001f)) fail("E table [r %u, mu %u] = %.6f outside (0, 1.001]", i / kAlbedoTableSize, i % kAlbedoTableSize, t[i]);
    CHECK(directionalAlbedo(1.0f, 0.0f) > 0.99f);   // smooth lobe at normal incidence keeps all energy
    CHECK(directionalAlbedo(1.0f, 1.0f) < 0.95f);   // rough lobe loses energy to multiple scattering
    Surface white;
    white.baseColor = { 1, 1, 1 };
    white.roughness = 1.0f;
    white.metallic = 1.0f;
    // Compensated rough white metal: directional albedo at normal incidence ~ 1 (Turquin 2019 restores the loss).
    double sum = 0;
    const int n = 256;
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j)
        {
            const float u = (i + 0.5f) / n, v = (j + 0.5f) / n;
            const float z = std::sqrt(u), r = std::sqrt(1 - u), phi = 2 * kPi * v;  // cosine-weighted hemisphere
            const float3 l{ r * std::cos(phi), r * std::sin(phi), z };
            sum += evaluate(white, { 0, 0, 1 }, { 0, 0, 1 }, l).x * kPi;  // f * cos / pdf, pdf = cos / pi
        }
    const double albedo = sum / (n * n);
    CHECK(albedo > 0.97 && albedo < 1.03);

    // Mirror (roughness 0, alpha 1e-4): the specular peak in the reflection direction is finite (GGX D without the
    // NoH^2 (a^2 - 1) + 1 cancellation, INTERFACES 8.1) and equals 1 / (pi alpha^2) there.
    scene::model::Surface mirror;
    mirror.baseColor = { 1, 1, 1 };
    mirror.roughness = 0;
    mirror.metallic = 1;
    const float3 view = normalize(float3{ 0.3f, 0, 1 });
    const float3 reflected{ -view.x, -view.y, view.z };
    const float peak = evaluate(mirror, { 0, 0, 1 }, view, reflected).x;
    CHECK(std::isfinite(peak) && peak > 1e6f);
    const float d = distributionGgx(1.0f, 0.0f, 1e-4f);
    CHECK(std::fabs(d * kPi * 1e-8f - 1.0f) < 1e-4f);
}

UNX_TEST(clearcoat_model)
{
    // A9 clearcoat (MaterialModel.h evaluateCoated): no coat = the base; the tables are finite and in [0, 1]; a white base
    // under a rough coat keeps an albedo near 1 (a gross-error bound only: the v1 base is Lambert + a Schlick lobe, so its
    // own albedo is not 1; the MATERIAL_LAYERS 3 criteria are judged by C's study tool against the layer model).
    namespace m = scene::model;
    const auto& t = m::coatTable();
    CHECK(t.size() == 2 * m::kCoatTableStride);
    for (float x : t) CHECK(std::isfinite(x) && x >= 0 && x <= 1);
    const float3 n{ 0, 0, 1 };
    m::Surface white;
    white.baseColor = { 1, 1, 1 };
    white.roughness = 0.6f;
    white.specular = 0;
    m::Coat none;
    const float3 v0 = normalize(float3{ 0.6f, 0.1f, 0.7f }), l0 = normalize(float3{ -0.3f, 0.4f, 0.8f });
    const float3 a = m::evaluate(white, n, v0, l0), b = m::evaluateCoated(white, none, n, v0, l0);
    CHECK(a.x == b.x && a.y == b.y && a.z == b.z);
    for (float eta : m::kCoatEtas)
        for (float mu : { 1.0f, 0.7f, 0.4f })
        {
            m::Coat coat;
            coat.cover = 1;
            coat.roughness = 0.3f;
            coat.eta = eta;
            const float3 v{ std::sqrt(1 - mu * mu), 0, mu };
            // albedo = integral of f cos over the hemisphere (midpoint rule in (cos theta, phi); the lobes are broad)
            double albedo = 0, baseAlbedo = 0;
            const int NT = 512, NP = 512;
            for (int i = 0; i < NT; ++i)
                for (int j = 0; j < NP; ++j)
                {
                    const float c = (i + 0.5f) / NT, sn = std::sqrt(1 - c * c), ph = 2 * m::kPi * (j + 0.5f) / NP;
                    const float3 l{ sn * std::cos(ph), sn * std::sin(ph), c };
                    albedo += m::evaluateCoated(white, coat, n, v, l).y * c * (2 * m::kPi / NT / NP);
                    baseAlbedo += m::evaluate(white, n, v, l).y * c * (2 * m::kPi / NT / NP);
                }
            logf("clearcoat eta %.2f r_c 0.3 over a white base, mu %.1f: albedo %.4f (uncoated %.4f)\n", eta, mu, albedo, baseAlbedo);
            CHECK(albedo > 0.9 && albedo < 1.1);
        }
    // the scene block
    scene::Scene s;
    scene::Material coated;
    coated.clearcoat = 0.75f;
    coated.clearcoatRoughness = 0.1f;
    coated.clearcoatIor = 1.33f;
    s.materials.push_back(coated);
    scene::validate(s);
    const scene::Scene back = scene::deserialize(scene::serialize(s));
    CHECK(back.materials[0].clearcoat == 0.75f && back.materials[0].clearcoatRoughness == 0.1f && back.materials[0].clearcoatIor == 1.33f);
    scene::Scene bad = s;
    bad.materials[0].clearcoatIor = 1.4f;  // not tabulated
    CHECK(throws([&] { scene::validate(bad); }));
    bad = s;
    bad.materials[0].cls = scene::MaterialClass::Glass;
    CHECK(throws([&] { scene::validate(bad); }));
}

UNX_TEST(subsurface_model_and_scene_block)
{
    // Subsurface class, stage A (MaterialModel.h evaluateSubsurface), CPU:
    //   1  one lobe (mix 1, scales (1, 1)) without transmission is the Standard model bit for bit; a mix of 1 at any scales
    //      is the Standard model at lobe 0's roughness;
    //   2  finite and non-negative over roughness, lobes, transmission and directions on both sides of the normal;
    //   3  the two lobes' directional albedo against the single lobe's at their average roughness;
    //   4  the thin parts' term: none without transmission, t f_d W across the surface, W between c and sqrt(c), 0 at the
    //      terminator;
    //   5  the parameters in the GPU record's class slots, and in the scene file's block.
    namespace m = scene::model;
    const float3 n{ 0, 0, 1 };
    auto dir = [](float mu, float phi) {
        const float st = std::sqrt(std::max(0.0f, 1 - mu * mu));
        return float3{ st * std::cos(phi), st * std::sin(phi), mu };
    };
    auto same = [](float3 a, float3 b) { return std::memcmp(&a, &b, sizeof a) == 0; };
    const float3 skinTone{ 0.80f, 0.56f, 0.45f };
    const float lightMu[] = { -1.0f, -0.5f, -1e-4f, 1e-4f, 0.1f, 0.4f, 0.7f, 0.95f, 1.0f };
    const float phis[] = { 0.0f, 1.1f, 2.5f, 3.14159265f, 4.9f };

    // 1. the invariant
    m::Subsurface one;
    one.lobeMix = 1;
    one.lobeRoughness = { 1, 1 };
    m::Subsurface firstOnly;  // the default scales, all the weight on lobe 0
    firstOnly.lobeMix = 1;
    uint32_t compared = 0;
    for (float r : { 0.0f, 0.05f, 0.2f, 0.45f, 0.7f, 1.0f })
        for (float metallic : { 0.0f, 1.0f })
            for (float muV : { 0.05f, 0.3f, 0.7f, 1.0f })
                for (float muL : lightMu)
                    for (float phi : phis)
                    {
                        m::Surface skin;
                        skin.cls = scene::MaterialClass::Subsurface;
                        skin.baseColor = skinTone;
                        skin.roughness = r;
                        skin.metallic = metallic;
                        skin.specular = 0.35f;
                        m::Surface standard = skin;
                        standard.cls = scene::MaterialClass::Standard;
                        const float3 v = dir(muV, 0.3f), l = dir(muL, phi);
                        CHECK(same(m::evaluateSubsurface(skin, one, n, v, l), m::evaluate(standard, n, v, l)));
                        CHECK(same(m::evaluateSubsurface(skin, one, n, v, l), m::evaluate(skin, n, v, l)));  // (evaluate() leaves the class to the caller)
                        standard.roughness = m::subsurfaceRoughness(firstOnly, r).x;
                        CHECK(same(m::evaluateSubsurface(skin, firstOnly, n, v, l), m::evaluate(standard, n, v, l)));
                        ++compared;
                    }

    // 2. the sweep
    uint32_t swept = 0;
    const float2 scales[] = { { 0.75f, 1.30f }, { 0.0f, 2.5f }, { 1.0f, 1.0f }, { 0.2f, 4.0f } };
    for (float mix : { 0.0f, 0.3f, 0.85f, 1.0f })
        for (float2 scale : scales)
            for (float r : { 0.0f, 0.02f, 0.3f, 0.6f, 1.0f })
                for (float t : { 0.0f, 0.5f, 1.0f })
                    for (float muV : { 1e-3f, 0.1f, 0.5f, 1.0f })
                        for (float muL : lightMu)
                            for (float phi : phis)
                            {
                                m::Surface skin;
                                skin.cls = scene::MaterialClass::Subsurface;
                                skin.baseColor = skinTone;
                                skin.roughness = r;
                                skin.specular = 0.35f;
                                skin.transmission = t;
                                m::Subsurface k;
                                k.lobeMix = mix;
                                k.lobeRoughness = scale;
                                const float3 f = m::evaluateSubsurface(skin, k, n, dir(muV, 0.3f), dir(muL, phi));
                                if (!(std::isfinite(f.x) && std::isfinite(f.y) && std::isfinite(f.z) && f.x >= 0 && f.y >= 0 && f.z >= 0))
                                    fail("subsurface: f = (%g, %g, %g) at mix %g scales (%g, %g) r %g t %g n.v %g n.l %g phi %g", f.x, f.y, f.z, mix, scale.x, scale.y, r, t, muV, muL,
                                         phi);
                                ++swept;
                            }

    // 3. energy. The directional albedo, integrated over the half vector with the lobes' own GGX distributions as the
    // measure (a (u, phi) grid per lobe, weighed as the mixture): sharp lobes are resolved as well as broad ones.
    auto albedo = [&](const std::function<float(float3, float3)>& f, float mu, double a0, double a1, double w0) {
        const double pi = 3.14159265358979323846;
        const float3 v = dir(mu, 0);
        auto D = [&](double c2, double a) {
            const double tt = (1 - c2) + a * a * c2;
            return a * a / (pi * tt * tt);
        };
        const int NU = 512, NP = 256;
        double sum = 0;
        for (int lobe = 0; lobe < 2; ++lobe)
        {
            const double a = lobe == 0 ? a0 : a1, w = lobe == 0 ? w0 : 1 - w0;
            if (!(w > 0)) continue;
            for (int i = 0; i < NU; ++i)
                for (int j = 0; j < NP; ++j)
                {
                    const double u = (i + 0.5) / NU, ph = 2 * pi * (j + 0.5) / NP;
                    const double c2 = (1 - u) / (1 + (a * a - 1) * u), ch = std::sqrt(c2), sh = std::sqrt(1 - c2);
                    const float3 h{ (float)(sh * std::cos(ph)), (float)(sh * std::sin(ph)), (float)ch };
                    const float VoH = dot(v, h);
                    if (VoH <= 0) continue;
                    const float3 l = h * (2 * VoH) - v;
                    if (l.z <= 0) continue;
                    const double pdf = (w0 * D(c2, a0) + (1 - w0) * D(c2, a1)) * ch;  // of h under the mixture
                    sum += w * f(v, l) * l.z * 4 * VoH / pdf;
                }
        }
        return sum / (NU * NP);
    };
    double worstEnergy = 0, worstEnergyGrazing = 0;
    const m::Subsurface defaults;
    for (float f0 : { 1.0f, 0.028f })  // a white metal (F = 1) and skin's f0 = 0.08 x 0.35
        for (float r : { 0.2f, 0.45f, 0.7f, 1.0f })
            for (float mu : { 1.0f, 0.7f, 0.4f, 0.2f })
            {
                m::Surface skin;  // (metallic 1: no diffuse, f0 = baseColor)
                skin.cls = scene::MaterialClass::Subsurface;
                skin.baseColor = { f0, f0, f0 };
                skin.metallic = 1;
                skin.roughness = r;
                const float3 lobes = m::subsurfaceRoughness(defaults, r);
                m::Surface single = skin;
                single.roughness = lobes.z;
                const double dual = albedo([&](float3 v, float3 l) { return m::evaluateSubsurface(skin, defaults, n, v, l).x; }, mu,
                                           m::alphaFromRoughness(lobes.x), m::alphaFromRoughness(lobes.y), defaults.lobeMix);
                const double average = albedo([&](float3 v, float3 l) { return m::evaluate(single, n, v, l).x; }, mu, m::alphaFromRoughness(lobes.z),
                                              m::alphaFromRoughness(lobes.z), 1.0);
                const double rel = std::fabs(dual - average) / average;
                (mu >= 0.4f ? worstEnergy : worstEnergyGrazing) = std::max(mu >= 0.4f ? worstEnergy : worstEnergyGrazing, rel);
                CHECK(std::isfinite(dual) && dual > 0 && dual < 1.08);
            }
    logf("    subsurface: %u points equal the Standard model bit for bit; %u points finite and non-negative; the two lobes' albedo against the average lobe's: worst %.2f %% "
         "(n.v >= 0.4), %.2f %% (n.v 0.2)\n",
         compared, swept, 100 * worstEnergy, 100 * worstEnergyGrazing);
    CHECK(worstEnergy < 0.04 && worstEnergyGrazing < 0.04);  // [measured: 3.1 % and 3.3 %, both on the dielectric at r 0.7]

    // 4. thin parts
    {
        m::Surface skin;
        skin.cls = scene::MaterialClass::Subsurface;
        skin.baseColor = skinTone;
        skin.transmission = 0.8f;
        const float3 fd = skinTone * (1 / m::kPi);
        for (float muV : { 0.1f, 0.6f, 1.0f })
            for (float c : { 1e-6f, 0.01f, 0.3f, 0.8f, 1.0f })
                for (float phi : phis)
                {
                    const float3 v = dir(muV, 0.3f), l = dir(-c, phi);
                    const float W = m::subsurfaceThin(c, v, l);
                    CHECK(W >= c * (1 - 1e-6f) && W <= std::sqrt(c) * (1 + 1e-6f));
                    const float3 f = m::evaluateSubsurface(skin, defaults, n, v, l);
                    CHECK(std::fabs(f.y * c - 0.8f * fd.y * W) <= 1e-5f * fd.y);
                }
        // looking through the part at the light: sqrt(c); the light off the view axis: Lambert's c; nothing at the terminator
        const float3 v = dir(0.6f, 0.0f);
        CHECK(std::fabs(m::subsurfaceThin(0.6f, v, float3{ -v.x, -v.y, -v.z }) - std::sqrt(0.6f)) < 1e-6f);
        CHECK(std::fabs(m::subsurfaceThin(0.25f, float3{ 0, 0, 1 }, dir(-0.25f, 1.0f)) - (0.25f + 0.25f * std::pow(0.25f, 12.0f))) < 1e-6f);
        CHECK(m::subsurfaceThin(0.0f, v, dir(0.0f, 3.14159265f)) == 0.0f);
        CHECK(m::subsurfaceThin(1e-6f, v, dir(-1e-6f, 3.14159265f)) < 2e-3f);
    }

    // 5. the GPU record's class slots (MaterialModel.hlsli modelSubsurfaceOf reads them back) ...
    scene::Material skin;
    skin.name = "skin";
    skin.cls = scene::MaterialClass::Subsurface;
    skin.subsurfaceMeanFreePath = { 0.02f, 0.01f, 0.005f };
    skin.subsurfaceLobeMix = 0.6f;
    skin.subsurfaceLobeRoughness = { 0.5f, 1.75f };
    {
        gpu::Material g{};
        packMaterialClass(skin, g);
        CHECK(g.hairAbsorption.x == 0.02f && g.hairAbsorption.y == 0.01f && g.hairAbsorption.z == 0.005f);
        CHECK(g.hairBetaN == 0.6f && g.cutScale == 0.5f && g.cutDamageWidth == 1.75f && g.hairTilt == 0.0f);
        m::Subsurface unpacked;
        unpacked.lobeMix = g.hairBetaN;
        unpacked.lobeRoughness = { g.cutScale, g.cutDamageWidth };
        const float3 a = m::subsurfaceRoughness(unpacked, 0.45f), b = m::subsurfaceRoughness(m::subsurfaceOf(skin), 0.45f);
        CHECK(same(a, b) && a.x == 0.45f * 0.5f && a.y == 0.45f * 1.75f);
        CHECK(m::subsurfaceRoughness(unpacked, 0.8f).y == 1.0f);  // saturated
        // the defaults, and the other classes' slots as before
        gpu::Material d{};
        scene::Material plain;
        plain.cls = scene::MaterialClass::Subsurface;
        packMaterialClass(plain, d);
        CHECK(d.hairAbsorption.x == 0.00130f && d.hairAbsorption.y == 0.00095f && d.hairAbsorption.z == 0.00067f && d.hairBetaN == 0.85f && d.cutScale == 0.75f &&
              d.cutDamageWidth == 1.30f);
        gpu::Material st{};
        packMaterialClass(scene::Material{}, st);
        CHECK(st.hairAbsorption.x == 0 && st.hairBetaN == 0 && st.hairTilt == 0 && st.cutScale == 0 && st.cutDamageWidth == 0);
        scene::Material cut;
        cut.cls = scene::MaterialClass::Cut;
        cut.cutScale = 2.5f, cut.cutDamageWidth = 0.03f;
        gpu::Material gc{};
        packMaterialClass(cut, gc);
        CHECK(gc.cutScale == 2.5f && gc.cutDamageWidth == 0.03f && gc.hairBetaN == 0 && gc.hairAbsorption.x == 0);
    }
    // ... and the scene file: no block for the defaults (a file written before the parameters existed has none either:
    // it loads to the defaults), the block and an exact round trip otherwise
    {
        auto contains = [](const std::vector<uint8_t>& bytes, const char* tag) {
            for (size_t i = 0; i + 4 <= bytes.size(); ++i)
                if (std::memcmp(bytes.data() + i, tag, 4) == 0) return true;
            return false;
        };
        scene::Scene before = tinyScene();
        scene::Material old;  // a Subsurface material as a scene saved it before this stage: the class and the common fields only
        old.name = "skin";
        old.cls = scene::MaterialClass::Subsurface;
        old.baseColor = skinTone;
        old.transmission = 0.25f;
        before.materials.push_back(old);
        scene::validate(before);
        const std::vector<uint8_t> beforeBytes = scene::serialize(before);
        CHECK(!contains(beforeBytes, "SUBS"));
        scene::Scene sameSize = before;  // the same scene with that material in the Standard class: byte for byte the same length
        sameSize.materials.back().cls = scene::MaterialClass::Standard;
        CHECK(scene::serialize(sameSize).size() == beforeBytes.size());
        const scene::Scene loaded = scene::deserialize(beforeBytes);
        const scene::Material defaults0;
        const scene::Material& l0 = loaded.materials.back();
        CHECK(l0.cls == scene::MaterialClass::Subsurface && l0.transmission == 0.25f && l0.subsurfaceLobeMix == defaults0.subsurfaceLobeMix &&
              l0.subsurfaceLobeRoughness.x == defaults0.subsurfaceLobeRoughness.x && l0.subsurfaceLobeRoughness.y == defaults0.subsurfaceLobeRoughness.y &&
              l0.subsurfaceMeanFreePath.x == defaults0.subsurfaceMeanFreePath.x && l0.subsurfaceMeanFreePath.y == defaults0.subsurfaceMeanFreePath.y &&
              l0.subsurfaceMeanFreePath.z == defaults0.subsurfaceMeanFreePath.z);
        CHECK(l0.subsurfaceLobeMix == 0.85f && l0.subsurfaceLobeRoughness.x == 0.75f && l0.subsurfaceLobeRoughness.y == 1.30f);
        CHECK(scene::serialize(loaded) == beforeBytes);

        scene::Scene with = before;
        with.materials.push_back(skin);
        scene::validate(with);
        const std::vector<uint8_t> withBytes = scene::serialize(with);
        CHECK(contains(withBytes, "SUBS"));
        scene::Scene withDefault = before;  // the same material with the defaults: no block, so the difference is the block
        scene::Material skinDefaults = skin;
        skinDefaults.subsurfaceMeanFreePath = defaults0.subsurfaceMeanFreePath;
        skinDefaults.subsurfaceLobeMix = defaults0.subsurfaceLobeMix;
        skinDefaults.subsurfaceLobeRoughness = defaults0.subsurfaceLobeRoughness;
        withDefault.materials.push_back(skinDefaults);
        const std::vector<uint8_t> withDefaultBytes = scene::serialize(withDefault);
        CHECK(!contains(withDefaultBytes, "SUBS"));
        CHECK(withBytes.size() == withDefaultBytes.size() + 4 + 8 + (4 + 12 + 4 + 8));  // tag, count, { index, mean free path, mix, scales }
        const scene::Scene back = scene::deserialize(withBytes);
        const scene::Material& b0 = back.materials[back.materials.size() - 2];
        const scene::Material& b1 = back.materials.back();
        CHECK(b0.subsurfaceLobeMix == 0.85f && b0.subsurfaceLobeRoughness.x == 0.75f);  // (not in the block: the defaults)
        CHECK(b1.subsurfaceMeanFreePath.x == 0.02f && b1.subsurfaceMeanFreePath.y == 0.01f && b1.subsurfaceMeanFreePath.z == 0.005f && b1.subsurfaceLobeMix == 0.6f &&
              b1.subsurfaceLobeRoughness.x == 0.5f && b1.subsurfaceLobeRoughness.y == 1.75f);
        CHECK(scene::serialize(back) == withBytes);
        CHECK(scene::contentHash(with) != scene::contentHash(withDefault));
        // a block naming a material the scene does not have is refused; so are parameters outside their ranges
        std::vector<uint8_t> broken = withBytes;
        const uint32_t bad = 99;
        std::memcpy(broken.data() + withDefaultBytes.size() + 4 + 8, &bad, 4);
        CHECK(throws([&] { scene::deserialize(broken); }));
        scene::Scene invalid = with;
        invalid.materials.back().subsurfaceLobeMix = 1.5f;
        CHECK(throws([&] { scene::validate(invalid); }));
        invalid = with;
        invalid.materials.back().subsurfaceLobeRoughness.y = -1.0f;
        CHECK(throws([&] { scene::validate(invalid); }));
    }
}

UNX_TEST(subsurface_profile_and_sampling)
{
    // Subsurface class, stage B (MaterialModel.h: the diffusion profile and the scatter pass's estimate), CPU:
    //   1  the profile integrates to 1 over the plane, P is its running integral, P^-1 inverts it, the mean radius is 2.5 d;
    //   2  d = l / s(A) at the fit's values, a channel without a mean free path;
    //   3  radii drawn as the pass draws them (strata beyond the pixel's own footprint) reproduce the profile: the share of
    //      samples in rings against P's differences;
    //   4  energy: a uniformly lit plane keeps its radiance after scattering - with any part of the samples rejected, on a
    //      surface off the plane -, and the weights' mean is each channel's mass beyond the footprint;
    //   5  a half-lit plane after scattering against the profile's own integral over the lit half (dense quadrature): the
    //      estimate's mean and its noise at the pass's sample counts.
    namespace m = scene::model;
    const double pi = 3.14159265358979323846;

    // 1. normalisation, P, P^-1, mean radius (midpoint rule in r / d over [0, 80]: the tail beyond is under 1e-11)
    double worstNorm = 0, worstCdf = 0, worstInverse = 0, worstMean = 0;
    for (float d : { 1e-4f, 2.3e-3f, 0.011f, 0.4f })
    {
        const int N = 400000;
        const double step = 80.0 * d / N;
        double mass = 0, mean = 0;
        int next = 1;
        for (int i = 0; i < N; ++i)
        {
            const double r = (i + 0.5) * step;
            const double ring = 2 * pi * r * m::subsurfaceProfile(d, (float)r) * step;
            mass += ring;
            mean += ring * r;
            if (i + 1 == next * (N / 64))  // P at 64 radii against the running integral
            {
                worstCdf = std::max(worstCdf, std::fabs(mass - m::subsurfaceRadialCdf(d, (float)((i + 1) * step))));
                ++next;
            }
        }
        worstNorm = std::max(worstNorm, std::fabs(mass - 1));
        worstMean = std::max(worstMean, std::fabs(mean / d - m::kSubsurfaceMeanRadius));
        for (int i = 0; i < 4096; ++i)
        {
            const float xi = (i + 0.5f) / 4096;
            const float r = m::subsurfaceRadius(d, xi);
            CHECK(std::isfinite(r) && r >= 0);
            worstInverse = std::max(worstInverse, std::fabs((double)m::subsurfaceRadialCdf(d, r) - xi));
        }
        CHECK(m::subsurfaceRadius(d, 0.0f) == 0.0f);
        CHECK(std::isfinite(m::subsurfaceRadius(d, 1.0f)));
        CHECK(std::fabs(m::subsurfaceRadialPdf(d, 0.0f) * d - 0.5f) < 1e-6f);
    }
    CHECK(worstNorm < 2e-5 && worstCdf < 2e-5 && worstInverse < 2e-6 && worstMean < 1e-4);

    // 2. the mapping
    {
        const float3 s = m::subsurfaceScaling({ 0.8f, 0.2f, 1.5f });  // (albedo above 1: as 1)
        CHECK(std::fabs(s.x - 1.1f) < 1e-6f && std::fabs(s.y - 2.96f) < 1e-6f && std::fabs(s.z - 1.04f) < 1e-6f);
        const float3 d = m::subsurfaceDistance({ 0.011f, 0.0074f, 0.0f }, { 0.8f, 0.2f, 0.5f });
        CHECK(std::fabs(d.x - 0.01f) < 1e-8f && std::fabs(d.y - 0.0025f) < 1e-8f && d.z == 1e-6f);
        const float3 w = m::subsurfaceSampleWeight(d, 0.004f, 0.003f, 2.0f);  // p_c(5 mm) / 2
        CHECK(std::fabs(w.x - 0.5f * m::subsurfaceRadialPdf(0.01f, 0.005f)) < 1e-4f * w.x && w.z == 0.0f);
    }

    // 3. the pass's radii against the profile
    double worstRing = 0;
    for (float centre : { 0.0f, 0.3f, 0.85f })
        for (uint32_t pairs : { 1u, 4u, 8u, 32u })
        {
            const float d = 0.01f;
            const float rc = m::subsurfaceRadius(d, centre);
            const int rings = 24, pixels = 4096;
            std::vector<double> count(rings + 1, 0.0);
            for (int p = 0; p < pixels; ++p)
                for (uint32_t k = 0; k < pairs; ++k)
                {
                    const float r = m::subsurfaceSampleRadius(d, centre, k, pairs, (p + 0.5f) / pixels);
                    CHECK(r >= rc * (1 - 1e-4f) - 1e-9f);
                    count[std::min(rings, (int)(r / (0.5f * d)))] += 1.0 / ((double)pixels * pairs);  // rings of d / 2, the last one open
                }
            for (int ring = 0; ring <= rings; ++ring)
            {
                const double a = std::max<double>(m::subsurfaceRadialCdf(d, ring * 0.5f * d), centre);
                const double b = ring == rings ? 1.0 : std::max<double>(m::subsurfaceRadialCdf(d, (ring + 1) * 0.5f * d), centre);
                worstRing = std::max(worstRing, std::fabs(count[ring] - (b - a) / (1 - centre)));
            }
        }
    CHECK(worstRing < 1e-3);  // [measured: 2.3e-4, the 4096 pixels' strata against the rings' edges]

    // The pass's estimate (SubsurfaceScatter.hlsli sssScatter) at a point of a surface whose diffuse light and height off
    // the plane are functions of the plane position; 'keep' says which sample points count. footprint: the radius r_c.
    struct Estimate
    {
        float3 value;
        float3 weightMean;  // mean of the weights x (1 - T_s): each channel's mass beyond the footprint
    };
    auto estimate = [&](float3 d, float rc, uint32_t samples, float u, float u2, float3 own, const std::function<float3(float, float)>& light,
                        const std::function<float(float, float)>& height, const std::function<bool(float, float)>& keep) {
        const float dS = std::max({ d.x, d.y, d.z });
        const float3 centre{ m::subsurfaceRadialCdf(d.x, rc), m::subsurfaceRadialCdf(d.y, rc), m::subsurfaceRadialCdf(d.z, rc) };
        const float centreS = m::subsurfaceRadialCdf(dS, rc);
        const uint32_t pairs = samples / 2;
        double sum[3] = {}, weight[3] = {}, all[3] = {};
        for (uint32_t k = 0; k < pairs; ++k)
        {
            const float r = m::subsurfaceSampleRadius(dS, centreS, k, pairs, u);
            const float pdf = m::subsurfaceRadialPdf(dS, r), angle = m::subsurfaceSampleAngle(k, u2);
            for (int side = 0; side < 2; ++side)
            {
                const float x = (side ? -r : r) * std::cos(angle), y = (side ? -r : r) * std::sin(angle);
                const float3 w = m::subsurfaceSampleWeight(d, r, height(x, y), pdf);
                const float wc[3] = { w.x, w.y, w.z };
                for (int c = 0; c < 3; ++c) all[c] += wc[c] * (1 - centreS) / samples;
                if (!keep(x, y)) continue;
                const float3 e = light(x, y);
                const float ec[3] = { e.x, e.y, e.z };
                for (int c = 0; c < 3; ++c) sum[c] += ec[c] * wc[c], weight[c] += wc[c];
            }
        }
        const float o[3] = { own.x, own.y, own.z }, t[3] = { centre.x, centre.y, centre.z };
        float v[3];
        for (int c = 0; c < 3; ++c)
        {
            const float tail = weight[c] > 0 ? (float)(sum[c] / weight[c]) : o[c];
            v[c] = tail + (o[c] - tail) * t[c];
        }
        return Estimate{ { v[0], v[1], v[2] }, { (float)all[0], (float)all[1], (float)all[2] } };
    };
    const float3 skinD = m::subsurfaceDistance(scene::Material{}.subsurfaceMeanFreePath, { 0.80f, 0.56f, 0.45f });  // the class's default on shading_ball's tone
    const auto flat = [](float, float) { return 0.0f; };
    const auto everywhere = [](float, float) { return true; };

    // 4. energy
    double worstEnergy = 0, worstMass = 0;
    {
        const float3 lit{ 3.0f, 0.5f, 0.125f };
        const auto uniform = [&](float, float) { return lit; };
        uint32_t cases = 0;
        for (float rc : { 0.0005f, 0.004f, 0.02f })
            for (uint32_t samples : { 2u, 8u, 16u, 64u })
                for (int p = 0; p < 64; ++p)
                {
                    const float u = (p + 0.5f) / 64, u2 = std::fmod(p * 0.754877666f, 1.0f);
                    // every sample; half of them rejected (a silhouette through the pixel); none; a curved surface
                    const std::function<bool(float, float)> keeps[] = { everywhere, [](float x, float) { return x > 0; }, [](float, float) { return false; } };
                    for (const auto& keep : keeps)
                        for (int curved = 0; curved < 2; ++curved)
                        {
                            const auto bowl = [](float x, float y) { return (x * x + y * y) / (2 * 0.02f); };
                            const Estimate e = estimate(skinD, rc, samples, u, u2, lit, uniform, curved ? std::function<float(float, float)>(bowl) : flat, keep);
                            worstEnergy = std::max({ worstEnergy, std::fabs((double)e.value.x / lit.x - 1), std::fabs((double)e.value.y / lit.y - 1), std::fabs((double)e.value.z / lit.z - 1) });
                            ++cases;
                        }
                }
        CHECK(cases > 0);
        // the weights' mean over the pixels' patterns: 1 - T_c per channel (the widest channel's weights are 1 each)
        for (float rc : { 0.0005f, 0.004f })
        {
            double mass[3] = {};
            const int pixels = 1024;
            for (int p = 0; p < pixels; ++p)
            {
                const Estimate e = estimate(skinD, rc, 16, (p + 0.5f) / pixels, std::fmod(p * 0.754877666f, 1.0f), lit, uniform, flat, everywhere);
                mass[0] += e.weightMean.x / pixels, mass[1] += e.weightMean.y / pixels, mass[2] += e.weightMean.z / pixels;
            }
            const float dc[3] = { skinD.x, skinD.y, skinD.z };
            for (int c = 0; c < 3; ++c) worstMass = std::max(worstMass, std::fabs(mass[c] - (1 - (double)m::subsurfaceRadialCdf(dc[c], rc))));
        }
    }
    CHECK(worstEnergy < 1e-5);
    CHECK(worstMass < 1e-4);  // [measured: 1e-7]

    // 5. a half-lit plane (light 1 where x > 0, the pixel at x0 from the edge): the exact value is the profile's mass on the
    // lit side, the integral over xi of the lit share of the circle of radius P^-1(xi) around the pixel.
    auto exactHalfPlane = [&](float d, float x0) {
        const int N = 200000;
        double sum = 0;
        for (int i = 0; i < N; ++i)
        {
            const double r = m::subsurfaceRadius(d, (float)((i + 0.5) / N));
            const double share = r <= std::fabs(x0) ? (x0 > 0 ? 1.0 : 0.0) : (x0 > 0 ? 1 - std::acos(x0 / r) / pi : std::acos(-x0 / r) / pi);
            sum += share / N;
        }
        return sum;
    };
    double worstBias[2] = {}, worstNoise[2] = {};
    const uint32_t counts[2] = { 16, 64 };
    for (int n = 0; n < 2; ++n)
        for (float x0 : { -0.03f, -0.012f, -0.004f, 0.002f, 0.008f, 0.02f })
        {
            const float rc = 0.0015f;  // (a pixel of 2.7 mm: shading_ball's distance at 1080p)
            const auto halfLit = [&](float x, float) { return x + x0 > 0 ? float3{ 1, 1, 1 } : float3{ 0, 0, 0 }; };
            const float own = x0 > 0 ? 1.0f : 0.0f;
            double mean[3] = {}, square[3] = {};
            const int grid = 48;
            for (int a = 0; a < grid; ++a)
                for (int b = 0; b < grid; ++b)
                {
                    const Estimate e = estimate(skinD, rc, counts[n], (a + 0.5f) / grid, (b + 0.5f) / grid, { own, own, own }, halfLit, flat, everywhere);
                    const float v[3] = { e.value.x, e.value.y, e.value.z };
                    for (int c = 0; c < 3; ++c) mean[c] += v[c], square[c] += (double)v[c] * v[c];
                }
            const float dc[3] = { skinD.x, skinD.y, skinD.z };
            for (int c = 0; c < 3; ++c)
            {
                mean[c] /= grid * grid;
                // (|x0| > r_c: the footprint's mass, which the estimate gives to the pixel's own light, lies on one side)
                worstBias[n] = std::max(worstBias[n], std::fabs(mean[c] - exactHalfPlane(dc[c], x0)));
                worstNoise[n] = std::max(worstNoise[n], std::sqrt(std::max(square[c] / (grid * grid) - mean[c] * mean[c], 0.0)));
            }
        }
    logf("    subsurface profile: integral off 1 by %.1e, P off the running integral by %.1e, P(P^-1) off by %.1e; the pass's radii in rings off P by %.1e; a uniformly lit "
         "plane off by %.1e, the weights' mean off the tail mass by %.1e; a half-lit plane against the exact integral: mean off by %.4f (16 samples), %.4f (64), noise %.4f, "
         "%.4f of the lit level\n",
         worstNorm, worstCdf, worstInverse, worstRing, worstEnergy, worstMass, worstBias[0], worstBias[1], worstNoise[0], worstNoise[1]);
    CHECK(worstBias[0] < 0.01 && worstBias[1] < 0.003);    // [measured: 0.0026 and 0.0015 - the narrow channels' ratio of sums]
    CHECK(worstNoise[0] < 0.10 && worstNoise[1] < 0.035);  // [measured: 0.081 and 0.023, the blue channel 2 mm inside the lit half]
}

UNX_TEST(reflection_view_geometry)
{
    scene::Camera cam;
    cam.position = { 0, 2, 6 };
    cam.forward = normalize(float3{ 0, -0.3f, -1 });
    const ViewDesc main = ViewDesc::fromCamera(cam, 1920, 1080, {});
    const ViewDesc refl = ViewDesc::planarReflection(main, { 0, 1, 0, 0 }, 0, 0, 1920, 1080);  // plane y = 0, full screen
    CHECK(refl.mirrored && refl.kind == gpu::ViewKind::PlanarReflection);
    CHECK(std::fabs(refl.position.y + 2) < 1e-5f);
    auto project = [](const float4x4& m, float3 p) {
        const float x = m.m[0][0] * p.x + m.m[0][1] * p.y + m.m[0][2] * p.z + m.m[0][3];
        const float y = m.m[1][0] * p.x + m.m[1][1] * p.y + m.m[1][2] * p.z + m.m[1][3];
        const float w = m.m[3][0] * p.x + m.m[3][1] * p.y + m.m[3][2] * p.z + m.m[3][3];
        return float2{ x / w, y / w };
    };
    const float3 q{ 0.7f, 1.3f, -3.0f }, mirror{ 0.7f, -1.3f, -3.0f };
    const float2 a = project(refl.viewProj, q), b = project(main.viewProj, mirror);
    CHECK(std::fabs(a.x - b.x) < 1e-4f && std::fabs(a.y - b.y) < 1e-4f);
    // A sub-rectangle maps its pixels onto the whole reflection target.
    const ViewDesc crop = ViewDesc::planarReflection(main, { 0, 1, 0, 0 }, 960, 540, 480, 270);
    const float2 c = project(crop.viewProj, q);
    const float xl = 960.f / 1920 * 2 - 1, yt = 1 - 540.f / 1080 * 2;
    CHECK(std::fabs(c.x - ((b.x - xl) * 2 / (480.f / 1920 * 2) - 1)) < 1e-3f);
    CHECK(std::fabs(c.y - ((b.y - yt) * 2 / (270.f / 1080 * 2) + 1)) < 1e-3f);
    // The crop keeps the main view's pixel angle 2 tan(verticalFov / 2) / height (froxel tiles, texture LOD, VSM
    // footprints, ray cones read it), and verticalFov is the projection's own: proj[1][1] = 1 / tan(verticalFov / 2).
    CHECK(std::fabs(2 * std::tan(crop.verticalFov * 0.5f) / crop.height - 2 * std::tan(main.verticalFov * 0.5f) / main.height) < 1e-8f);
    CHECK(std::fabs(crop.proj.m[1][1] * std::tan(crop.verticalFov * 0.5f) - 1) < 1e-5f);
}

UNX_TEST(device_on_host_device_and_queue)
{
    // Host integration (DeviceOptions::externalDevice / externalGraphicsQueue, Queue::setExecuteHook): a Device built on
    // an existing device and a host-owned DIRECT queue submits through the host's hook and signals its own fence there.
    ComPtr<ID3D12CommandQueue> hostQueue;
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    check(testDevice().d3d()->CreateCommandQueue(&qd, IID_PPV_ARGS(&hostQueue)), "host queue");
    DeviceOptions o;
    o.externalDevice = testDevice().d3d();
    o.externalGraphicsQueue = hostQueue.Get();
    uint32_t hooked = 0;
    {
        Device host(o);
        CHECK(host.d3d() == testDevice().d3d() && host.queue(QueueType::Graphics).get() == hostQueue.Get() && !host.caps().adapter.empty());
        host.queue(QueueType::Graphics).setExecuteHook([&](ID3D12CommandList* list) {
            ++hooked;
            hostQueue->ExecuteCommandLists(1, &list);
        });
        CommandList cl = host.acquireCommandList(QueueType::Graphics);
        const uint64_t fence = host.submit(cl);
        host.queue(QueueType::Graphics).waitCpu(fence);
        CHECK(host.queue(QueueType::Graphics).completed() >= fence);
        host.queue(QueueType::Graphics).setExecuteHook({});
    }
    CHECK(hooked == 1);
    DeviceOptions bad = o;
    bad.debugLayer = true;
    CHECK(throws([&] { Device d(bad); }));
}

UNX_TEST(gpu_scene_material_textures)
{
    // M's texture system publishes texture SRVs into the material records (INTERFACES_KO.md 6.3 v1.10): new material
    // buffer and SRV, revisions bumped, GPU contents match; an unchanged set rewrites nothing.
    scene::Scene s = tinyScene();
    GpuScene gs(testDevice());
    gs.upload(s);
    gpu::FrameConstants before{};
    gs.fill(before);
    const uint32_t sceneRevision = gs.revision();
    std::vector<gpu::MaterialTextures> t(1);
    t[0].baseColor = 1234;
    t[0].clamp = gpu::MaterialTextureBaseColor;
    gs.setMaterialTextures(t);
    gpu::FrameConstants after{};
    gs.fill(after);
    CHECK(after.materials != before.materials && gs.revision() == sceneRevision + 1 && gs.materials()[0].revision == gs.revision());
    D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC1 rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = sizeof(gpu::Material);
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> rb;
    check(testDevice().d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&rb)), "readback");
    CommandList cl = testDevice().acquireCommandList(QueueType::Graphics);
    cl.list->CopyBufferRegion(rb.Get(), 0, gs.buffer("materials"), 0, sizeof(gpu::Material));
    testDevice().queue(QueueType::Graphics).waitCpu(testDevice().submit(cl));
    gpu::Material m{};
    void* p = nullptr;
    check(rb->Map(0, nullptr, &p), "map");
    std::memcpy(&m, p, sizeof m);
    rb->Unmap(0, nullptr);
    CHECK(m.baseColorTexture == 1234 && m.normalTexture == gpu::kNone && m.textureClamp == gpu::MaterialTextureBaseColor);
    gs.setMaterialTextures(t);  // same set: nothing changes
    gpu::FrameConstants again{};
    gs.fill(again);
    CHECK(again.materials == after.materials && gs.revision() == sceneRevision + 1);
}

UNX_TEST(gpu_scene_edits_after_upload)
{
    // Scene edits after upload (INTERFACES 6.3 v1.44, D0): setInstances appends and replaces instances of uploaded meshes
    // (no motion, visible again, overrides in the remap table), setMaterials appends and replaces materials (published
    // textures kept); each call bumps the scene revision; the GPU tables match the CPU mirrors after the edits and after a
    // frame's scatter into the replaced instance buffer; per-frame updates keep working on appended instances.
    scene::Scene s = tinyScene();
    GpuScene gs(testDevice());
    gs.upload(s);
    const uint32_t r0 = gs.revision();
    auto translation = [](float x, float y, float z) {
        float3x4 m;
        m.m[0][3] = x;
        m.m[1][3] = y;
        m.m[2][3] = z;
        return m;
    };
    auto readInstances = [&]() {
        const uint64_t bytes = gs.instances().size() * sizeof(gpu::Instance);
        D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC1 rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> rb;
        check(testDevice().d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&rb)),
              "readback");
        CommandList cl = testDevice().acquireCommandList(QueueType::Graphics);
        cl.list->CopyBufferRegion(rb.Get(), 0, gs.buffer("instances"), 0, bytes);
        testDevice().queue(QueueType::Graphics).waitCpu(testDevice().submit(cl));
        std::vector<gpu::Instance> out(gs.instances().size());
        uint8_t* q = nullptr;
        check(rb->Map(0, nullptr, reinterpret_cast<void**>(&q)), "map");
        std::memcpy(out.data(), q, bytes);
        rb->Unmap(0, nullptr);
        return out;
    };
    auto sameRecords = [](const std::vector<gpu::Instance>& a, const std::vector<gpu::Instance>& b) {
        return a.size() == b.size() && std::memcmp(a.data(), b.data(), a.size() * sizeof(gpu::Instance)) == 0;
    };

    // Append two instances of mesh 0 (one with a material override), replace instance 0 by a moved copy.
    const size_t n0 = s.instances.size();
    s.materials.push_back(s.materials[0]);
    s.materials.back().name = "appended";
    s.materials.back().baseColor = { 0.9f, 0.1f, 0.1f };
    const uint32_t newMaterial = (uint32_t)s.materials.size() - 1;
    const uint32_t mats[] = { newMaterial };
    gs.setMaterials(mats);
    CHECK(gs.revision() == r0 + 1 && gs.materials().size() == s.materials.size() && gs.materials()[newMaterial].baseColor.x == 0.9f);
    scene::Instance a = s.instances[0], b = s.instances[0];
    a.transform = translation(3, 0, 0);
    b.transform = translation(-3, 0, 0);
    b.materialOverrides.assign(s.meshes[b.mesh].submeshes.size(), newMaterial);
    s.instances.push_back(a);
    s.instances.push_back(b);
    s.instances[0].transform = translation(0, 5, 0);
    const uint32_t edited[] = { (uint32_t)n0, (uint32_t)n0 + 1, 0 };
    gs.setInstances(edited);
    CHECK(gs.revision() == r0 + 2 && gs.instances().size() == n0 + 2);
    const gpu::Instance& g0 = gs.instances()[0];
    CHECK(g0.objectToWorld[1].w == 5.0f && g0.prevObjectToWorld[1].w == 5.0f && g0.transformRevision == r0 + 2);
    CHECK(gs.instances()[n0 + 1].materialRemap != gpu::kNone && gs.instances()[n0].materialRemap == gpu::kNone);
    CHECK(sameRecords(readInstances(), gs.instances()));

    // Remove (hide) the appended instance, reuse its slot for another copy: visible again, no motion.
    gs.setInstanceVisible((uint32_t)n0, false);
    CHECK((gs.instances()[n0].flags & gpu::kInstanceHidden) != 0);
    s.instances[n0].transform = translation(7, 0, 0);
    const uint32_t reuse[] = { (uint32_t)n0 };
    gs.setInstances(reuse);
    CHECK((gs.instances()[n0].flags & gpu::kInstanceHidden) == 0 && gs.instances()[n0].objectToWorld[0].w == 7.0f && gs.instances()[n0].prevObjectToWorld[0].w == 7.0f);

    // Per-frame updates on an appended instance, scattered into the replaced table.
    const InstanceTransformUpdate move[] = { { (uint32_t)n0 + 1, translation(-4, 0, 0) } };
    gs.updateTransforms(0, move);
    gs.flushUpdates(0, 2, shaders());
    testDevice().waitIdle();
    const std::vector<gpu::Instance> gpu0 = readInstances();
    CHECK(sameRecords(gpu0, gs.instances()) && gpu0[n0 + 1].objectToWorld[0].w == -4.0f && gpu0[n0 + 1].prevObjectToWorld[0].w == -3.0f);

    // Out-of-range edits fail.
    bool threw = false;
    try
    {
        const uint32_t bad[] = { (uint32_t)s.instances.size() + 1 };
        gs.setInstances(bad);
    }
    catch (const std::exception&)
    {
        threw = true;
    }
    CHECK(threw);
}

UNX_TEST(gpu_scene_frame_updates)
{
    // GpuScene per-frame updates (INTERFACES_KO.md 6.3 v1.8): previous = the previous rendered frame, settling to
    // current once an instance stops, revisions once per frame, hidden flag, bone palettes, read back from the GPU.
    scene::Scene s = tinyScene();
    scene::Mesh skinned = s.meshes[0];
    skinned.name = "skinned quad";
    skinned.skin.joints.assign(4 * skinned.positions.size(), 0);
    skinned.skin.weights.clear();
    for (size_t v = 0; v < skinned.positions.size(); ++v) skinned.skin.weights.insert(skinned.skin.weights.end(), { 1.0f, 0.0f, 0.0f, 0.0f });
    skinned.skin.inverseBind = { float3x4{} };
    s.meshes.push_back(skinned);
    s.skeletons.push_back({ "one joint", { float3x4{} } });
    scene::Instance a = s.instances[0];
    a.mesh = 1;
    a.flags |= scene::InstanceSkinned;
    a.skeleton = 0;
    s.instances.push_back(a);
    GpuScene gs(testDevice());
    gs.upload(s);
    const uint32_t revision0 = gs.instances()[0].transformRevision, deform0 = gs.instances()[1].deformRevision;

    auto translation = [](float x, float y, float z) {
        float3x4 m;
        m.m[0][3] = x;
        m.m[1][3] = y;
        m.m[2][3] = z;
        return m;
    };
    struct Gpu
    {
        std::vector<gpu::Instance> instances;
        std::vector<float4> palette, prevPalette;
    };
    auto readBack = [&]() {
        Gpu out;
        const uint64_t ib = s.instances.size() * sizeof(gpu::Instance), pb = 3 * sizeof(float4);
        D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC1 rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = ib + 2 * pb;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> rb;
        check(testDevice().d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&rb)),
              "readback");
        CommandList cl = testDevice().acquireCommandList(QueueType::Graphics);
        cl.list->CopyBufferRegion(rb.Get(), 0, gs.buffer("instances"), 0, ib);
        cl.list->CopyBufferRegion(rb.Get(), ib, gs.buffer("bonePalette"), 0, pb);
        cl.list->CopyBufferRegion(rb.Get(), ib + pb, gs.buffer("prevBonePalette"), 0, pb);
        testDevice().queue(QueueType::Graphics).waitCpu(testDevice().submit(cl));
        uint8_t* p = nullptr;
        check(rb->Map(0, nullptr, reinterpret_cast<void**>(&p)), "map");
        out.instances.resize(s.instances.size());
        out.palette.resize(3);
        out.prevPalette.resize(3);
        std::memcpy(out.instances.data(), p, ib);
        std::memcpy(out.palette.data(), p + ib, pb);
        std::memcpy(out.prevPalette.data(), p + ib + pb, pb);
        rb->Unmap(0, nullptr);
        return out;
    };
    auto translationOf = [](const float4 rows[3]) { return float3{ rows[0].w, rows[1].w, rows[2].w }; };
    auto same = [](float3 a, float3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; };

    // Frame 0: instance 0 moves, the skeleton poses, instance 1 is hidden.
    const InstanceTransformUpdate move0[] = { { 0, translation(1, 0, 0) } };
    gs.updateTransforms(0, move0);
    const float3x4 pose0[] = { translation(0, 2, 0) };
    gs.updateSkeleton(0, 0, pose0);
    gs.setInstanceVisible(1, false);
    gs.flushUpdates(0, 2, shaders());
    Gpu g = readBack();
    CHECK(same(translationOf(g.instances[0].objectToWorld), { 1, 0, 0 }) && same(translationOf(g.instances[0].prevObjectToWorld), { 0, 0, 0 }));
    CHECK(g.instances[0].transformRevision == revision0 + 1 && (g.instances[1].flags & gpu::kInstanceHidden) != 0);
    CHECK(g.instances[1].deformRevision == deform0 + 1 && g.palette[1].w == 2 && g.prevPalette[1].w == 0);

    // Frame 1: nothing changes: previous settles to current (zero motion), revisions stay.
    gs.setInstanceVisible(1, true);
    gs.flushUpdates(1, 2, shaders());
    g = readBack();
    CHECK(same(translationOf(g.instances[0].prevObjectToWorld), { 1, 0, 0 }) && g.instances[0].transformRevision == revision0 + 1);
    CHECK((g.instances[1].flags & gpu::kInstanceHidden) == 0 && g.prevPalette[1].w == 2 && g.palette[1].w == 2);

    // Frame 2: two updates in one frame: previous = frame 1's transform, one revision step.
    const InstanceTransformUpdate move2[] = { { 0, translation(2, 0, 0) }, { 0, translation(3, 0, 0) } };
    gs.updateTransforms(2, move2);
    gs.flushUpdates(2, 2, shaders());
    g = readBack();
    CHECK(same(translationOf(g.instances[0].objectToWorld), { 3, 0, 0 }) && same(translationOf(g.instances[0].prevObjectToWorld), { 1, 0, 0 }));
    CHECK(g.instances[0].transformRevision == revision0 + 2);
    CHECK(same(translationOf(gs.instances()[0].objectToWorld), { 3, 0, 0 }));  // CPU mirror
    CHECK((g.instances[0].flags & gpu::kInstanceMotionBreak) == 0 && (g.instances[1].flags & gpu::kInstanceMotionBreak) == 0);

    // Frame 3 (v1.35): a teleport has no motion (previous = the new transform).
    const InstanceTransformUpdate jump3[] = { { 0, translation(10, 0, 0), kTransformTeleport } };
    gs.updateTransforms(3, jump3);
    gs.flushUpdates(3, 2, shaders());
    g = readBack();
    CHECK(same(translationOf(g.instances[0].objectToWorld), { 10, 0, 0 }) && same(translationOf(g.instances[0].prevObjectToWorld), { 10, 0, 0 }));
    // v1.45: the break is flagged for this frame with the bounding-sphere centre of the previous rendered frame (x = 3),
    // where caches keyed by the drawn place (VSM pages) still hold it.
    const float4 bounds0 = gs.meshes()[s.instances[0].mesh].boundsSphere, bounds1 = gs.meshes()[s.instances[1].mesh].boundsSphere;
    CHECK((g.instances[0].flags & gpu::kInstanceMotionBreak) != 0 && same(g.instances[0].breakCentre, { bounds0.x + 3, bounds0.y, bounds0.z }));
    CHECK((g.instances[1].flags & gpu::kInstanceMotionBreak) == 0);

    // Frame 4: a restore (resetMotion) after this frame's move and pose: no motion anywhere; the CPU palette accessor.
    const InstanceTransformUpdate move4[] = { { 0, translation(11, 0, 0) } };
    gs.updateTransforms(4, move4);
    const float3x4 pose4[] = { translation(0, 5, 0) };
    gs.updateSkeleton(4, 0, pose4);
    gs.resetMotion();
    gs.flushUpdates(4, 2, shaders());
    g = readBack();
    CHECK(same(translationOf(g.instances[0].prevObjectToWorld), { 11, 0, 0 }) && g.palette[1].w == 5 && g.prevPalette[1].w == 5);
    const std::span<const float4> cpuPalette = gs.palette(1);
    CHECK(cpuPalette.size() == 3 && cpuPalette[1].w == 5 && gs.palette(0).empty());
    // Both changed instances break here: instance 0 was drawn at x = 10 last frame, instance 1 (re-posed, not moved) where it
    // is. The teleport's flag of frame 3 lasted one frame and is renewed only by this frame's restore.
    CHECK((g.instances[0].flags & gpu::kInstanceMotionBreak) != 0 && same(g.instances[0].breakCentre, { bounds0.x + 10, bounds0.y, bounds0.z }));
    const float3 at1 = translationOf(g.instances[1].objectToWorld);
    CHECK((g.instances[1].flags & gpu::kInstanceMotionBreak) != 0 && same(g.instances[1].breakCentre, { bounds1.x + at1.x, bounds1.y + at1.y, bounds1.z + at1.z }));

    // Frame 5: nothing changes: the flags clear.
    gs.flushUpdates(5, 2, shaders());
    g = readBack();
    CHECK((g.instances[0].flags & gpu::kInstanceMotionBreak) == 0 && (g.instances[1].flags & gpu::kInstanceMotionBreak) == 0);
}

UNX_TEST(material_tables_on_the_gpu)
{
    // GpuScene uploads the E and (A, B) tables and FrameConstants carries their SRVs (materialModelLut,
    // specularAlbedoLut); MaterialModel.hlsli's bilinear reads equal scene::model's at a grid off the table nodes.
    const scene::Scene s = tinyScene();
    GpuScene gs(testDevice());
    gs.upload(s);
    gpu::FrameConstants fc{};
    gs.fill(fc);
    CHECK(fc.materialModelLut != gpu::kNone && fc.specularAlbedoLut != gpu::kNone);
    ComPtr<ID3D12Resource> constants, rb;
    const uint32_t n = 45, bytes = n * n * 20;
    {
        D3D12_HEAP_PROPERTIES up{ D3D12_HEAP_TYPE_UPLOAD }, rp{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC1 rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (sizeof(gpu::FrameConstants) + 255) / 256 * 256;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(testDevice().d3d()->CreateCommittedResource3(&up, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&constants)),
              "constants");
        void* mapped = nullptr;
        check(constants->Map(0, nullptr, &mapped), "map constants");
        std::memcpy(mapped, &fc, sizeof fc);
        constants->Unmap(0, nullptr);
        rd.Width = bytes;
        check(testDevice().d3d()->CreateCommittedResource3(&rp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&rb)), "readback");
    }
    ID3D12PipelineState* pso = shaders().compute("Passes/Test/SpecularAlbedo");
    RenderGraph g(testDevice());
    const BufferRef out = g.createBuffer({ "tables out", bytes, 0 });
    const D3D12_GPU_VIRTUAL_ADDRESS address = constants->GetGPUVirtualAddress();
    g.addPass("tables", QueueType::Graphics, [&](PassBuilder& b) { b.use(out, Use::UavCompute); },
              [=](PassContext& ctx) {
                  const uint32_t k[4] = { ctx.uav(out), n, 0, 0 };
                  ctx.cmd->SetPipelineState(pso);
                  ctx.bindFrameConstants(address);
                  ctx.computeConstants(k, 4);
                  ctx.cmd->Dispatch((n * n + 63) / 64, 1, 1);
              });
    ID3D12Resource* dst = rb.Get();
    g.addPass("tables readback", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(out, Use::CopySrc);
                  b.keep();
              },
              [=](PassContext& ctx) { ctx.cmd->CopyBufferRegion(dst, 0, ctx.resource(out), 0, bytes); });
    g.execute(nullptr);
    for (uint32_t q = 0; q < kQueueTypeCount; ++q) testDevice().queue((QueueType)q).waitCpu(g.lastFence((QueueType)q));
    const float* v = nullptr;
    check(rb->Map(0, nullptr, (void**)&v), "map readback");
    float worstAB = 0, worstE = 0;
    for (uint32_t i = 0; i < n * n; ++i)
    {
        const float* p = v + 5 * i;
        const float2 ab = scene::model::specularAlbedo(p[0], p[1]);
        worstAB = std::max({ worstAB, std::fabs(p[2] - ab.x), std::fabs(p[3] - ab.y) });
        worstE = std::max(worstE, std::fabs(p[4] - scene::model::directionalAlbedo(p[0], p[1])));
    }
    rb->Unmap(0, nullptr);
    logf("    GPU table reads vs scene::model over %u points: worst |dA|, |dB| %.2e, |dE| %.2e\n", n * n, worstAB, worstE);
    CHECK(worstAB < 1e-6f && worstE < 1e-6f);
    testDevice().deferRelease(constants);
    testDevice().deferRelease(rb);
}

UNX_TEST(clearcoat_model_on_the_gpu)
{
    // A9 (v1.76): GpuScene uploads the coat tables (FrameConstants::coatTable) and MaterialModel.hlsli's
    // modelEvaluateCoated equals scene::model::evaluateCoated to float rounding at 4096 points over both coats, metal and
    // dielectric bases, every roughness and cover (Passes/Test/CoatModel.hlsl).
    const scene::Scene s = tinyScene();
    GpuScene gs(testDevice());
    gs.upload(s);
    gpu::FrameConstants fc{};
    gs.fill(fc);
    CHECK(fc.coatTable != gpu::kNone);
    ComPtr<ID3D12Resource> constants, rb;
    const uint32_t n = 4096, bytes = n * 80;
    {
        D3D12_HEAP_PROPERTIES up{ D3D12_HEAP_TYPE_UPLOAD }, rp{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC1 rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (sizeof(gpu::FrameConstants) + 255) / 256 * 256;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(testDevice().d3d()->CreateCommittedResource3(&up, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&constants)),
              "constants");
        void* mapped = nullptr;
        check(constants->Map(0, nullptr, &mapped), "map constants");
        std::memcpy(mapped, &fc, sizeof fc);
        constants->Unmap(0, nullptr);
        rd.Width = bytes;
        check(testDevice().d3d()->CreateCommittedResource3(&rp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&rb)), "readback");
    }
    ID3D12PipelineState* pso = shaders().compute("Passes/Test/CoatModel");
    RenderGraph g(testDevice());
    const BufferRef out = g.createBuffer({ "coat out", bytes, 0 });
    const D3D12_GPU_VIRTUAL_ADDRESS address = constants->GetGPUVirtualAddress();
    g.addPass("coat", QueueType::Graphics, [&](PassBuilder& b) { b.use(out, Use::UavCompute); },
              [=](PassContext& ctx) {
                  const uint32_t k[4] = { ctx.uav(out), n, 0, 0 };
                  ctx.cmd->SetPipelineState(pso);
                  ctx.bindFrameConstants(address);
                  ctx.computeConstants(k, 4);
                  ctx.cmd->Dispatch((n + 63) / 64, 1, 1);
              });
    ID3D12Resource* dst = rb.Get();
    g.addPass("coat readback", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(out, Use::CopySrc);
                  b.keep();
              },
              [=](PassContext& ctx) { ctx.cmd->CopyBufferRegion(dst, 0, ctx.resource(out), 0, bytes); });
    g.execute(nullptr);
    for (uint32_t q = 0; q < kQueueTypeCount; ++q) testDevice().queue((QueueType)q).waitCpu(g.lastFence((QueueType)q));
    const float* v = nullptr;
    check(rb->Map(0, nullptr, (void**)&v), "map readback");
    double worst = 0;
    for (uint32_t i = 0; i < n; ++i)
    {
        const float* p = v + 20 * i;
        scene::model::Surface su;
        su.baseColor = { p[6], p[7], p[8] };
        su.roughness = p[9];
        su.metallic = p[10];
        su.specular = 0.5f;
        scene::model::Coat c;
        c.cover = p[11];
        c.roughness = p[12];
        uint32_t coat;
        std::memcpy(&coat, p + 13, 4);
        c.eta = p[14];
        CHECK(scene::model::coatIndex(c.eta) == coat);
        const float3 want = scene::model::evaluateCoated(su, c, { 0, 0, 1 }, { p[0], p[1], p[2] }, { p[3], p[4], p[5] });
        const float got[3] = { p[15], p[16], p[17] }, w[3] = { want.x, want.y, want.z };
        for (int k = 0; k < 3; ++k) worst = std::max(worst, std::fabs((double)got[k] - w[k]) / std::max(std::fabs((double)w[k]), 1e-3));
    }
    rb->Unmap(0, nullptr);
    logf("    coated BRDF on the GPU vs scene::model over %u points: worst relative %.2e\n", n, worst);
    CHECK(worst < 1e-4);
    testDevice().deferRelease(constants);
    testDevice().deferRelease(rb);
}

UNX_TEST(subsurface_model_on_the_gpu)
{
    // Subsurface class, stage A: MaterialModel.hlsli's modelEvaluateSubsurface equals scene::model::evaluateSubsurface to
    // float rounding at 4096 points - both sides of the surface, metal and dielectric, every roughness and transmission -
    // with the lobes read from the scene's material records (modelSubsurfaceOf: GpuScene's class slots), which equal the
    // materials' (Passes/Test/SubsurfaceModel.hlsl).
    scene::Scene s = tinyScene();
    const uint32_t firstMaterial = (uint32_t)s.materials.size();
    {
        scene::Material skin;
        skin.name = "skin";
        skin.cls = scene::MaterialClass::Subsurface;
        s.materials.push_back(skin);  // the class's defaults
        skin.subsurfaceLobeMix = 1.0f;
        skin.subsurfaceLobeRoughness = { 1.0f, 1.0f };
        s.materials.push_back(skin);  // one lobe
        skin.subsurfaceLobeMix = 0.4f;
        skin.subsurfaceLobeRoughness = { 0.5f, 2.0f };
        s.materials.push_back(skin);
    }
    const uint32_t materialCount = (uint32_t)s.materials.size() - firstMaterial;
    scene::validate(s);
    GpuScene gs(testDevice());
    gs.upload(s);
    gpu::FrameConstants fc{};
    gs.fill(fc);
    CHECK(fc.materialModelLut != gpu::kNone);
    ComPtr<ID3D12Resource> constants, rb;
    const uint32_t n = 4096, bytes = n * 80;
    {
        D3D12_HEAP_PROPERTIES up{ D3D12_HEAP_TYPE_UPLOAD }, rp{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC1 rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (sizeof(gpu::FrameConstants) + 255) / 256 * 256;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(testDevice().d3d()->CreateCommittedResource3(&up, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&constants)),
              "constants");
        void* mapped = nullptr;
        check(constants->Map(0, nullptr, &mapped), "map constants");
        std::memcpy(mapped, &fc, sizeof fc);
        constants->Unmap(0, nullptr);
        rd.Width = bytes;
        check(testDevice().d3d()->CreateCommittedResource3(&rp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&rb)), "readback");
    }
    ID3D12PipelineState* pso = shaders().compute("Passes/Test/SubsurfaceModel");
    RenderGraph g(testDevice());
    const BufferRef out = g.createBuffer({ "subsurface out", bytes, 0 });
    const D3D12_GPU_VIRTUAL_ADDRESS address = constants->GetGPUVirtualAddress();
    g.addPass("subsurface", QueueType::Graphics, [&](PassBuilder& b) { b.use(out, Use::UavCompute); },
              [=](PassContext& ctx) {
                  const uint32_t k[4] = { ctx.uav(out), n, firstMaterial, materialCount };
                  ctx.cmd->SetPipelineState(pso);
                  ctx.bindFrameConstants(address);
                  ctx.computeConstants(k, 4);
                  ctx.cmd->Dispatch((n + 63) / 64, 1, 1);
              });
    ID3D12Resource* dst = rb.Get();
    g.addPass("subsurface readback", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(out, Use::CopySrc);
                  b.keep();
              },
              [=](PassContext& ctx) { ctx.cmd->CopyBufferRegion(dst, 0, ctx.resource(out), 0, bytes); });
    g.execute(nullptr);
    for (uint32_t q = 0; q < kQueueTypeCount; ++q) testDevice().queue((QueueType)q).waitCpu(g.lastFence((QueueType)q));
    const float* v = nullptr;
    check(rb->Map(0, nullptr, (void**)&v), "map readback");
    double worst = 0, worstLobes = 0;
    uint32_t across = 0;
    for (uint32_t i = 0; i < n; ++i)
    {
        const float* p = v + 20 * i;
        scene::model::Surface su;
        su.cls = scene::MaterialClass::Subsurface;
        su.baseColor = { p[6], p[7], p[8] };
        su.roughness = p[9];
        su.metallic = p[10];
        su.specular = 0.5f;
        su.transmission = p[11];
        const scene::model::Subsurface k = scene::model::subsurfaceOf(s.materials[firstMaterial + i % materialCount]);
        const float3 lobes = scene::model::subsurfaceRoughness(k, su.roughness);
        worstLobes = std::max({ worstLobes, std::fabs((double)p[12] - k.lobeMix), std::fabs((double)p[13] - lobes.x), std::fabs((double)p[14] - lobes.y),
                                std::fabs((double)p[15] - lobes.z) });
        across += p[5] < 0 && su.transmission > 0;
        const float3 want = scene::model::evaluateSubsurface(su, k, { 0, 0, 1 }, { p[0], p[1], p[2] }, { p[3], p[4], p[5] });
        const float got[3] = { p[16], p[17], p[18] }, w[3] = { want.x, want.y, want.z };
        for (int c = 0; c < 3; ++c) worst = std::max(worst, std::fabs((double)got[c] - w[c]) / std::max(std::fabs((double)w[c]), 1e-3));
    }
    rb->Unmap(0, nullptr);
    logf("    subsurface BRDF on the GPU vs scene::model over %u points (%u through a thin part): worst relative %.2e; the records' lobes: worst %.2e\n", n, across, worst,
         worstLobes);
    CHECK(across > 0);
    CHECK(worstLobes < 1e-6);
    CHECK(worst < 1e-4);
    testDevice().deferRelease(constants);
    testDevice().deferRelease(rb);
}

UNX_TEST(subsurface_profile_on_the_gpu)
{
    // Subsurface class, stage B: Passes/Common/SubsurfaceProfile.hlsli equals scene::model's profile, inverse distribution,
    // sample radius, angle and weight to float rounding at 4096 points - mean free paths from 0.1 mm to 10 cm, a channel
    // without one, points in and off the surface's plane (Passes/Test/SubsurfaceProfile.hlsl).
    namespace m = scene::model;
    const uint32_t n = 4096, bytes = n * 96;
    ComPtr<ID3D12Resource> rb;
    {
        D3D12_HEAP_PROPERTIES rp{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC1 rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(testDevice().d3d()->CreateCommittedResource3(&rp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&rb)), "readback");
    }
    ID3D12PipelineState* pso = shaders().compute("Passes/Test/SubsurfaceProfile");
    RenderGraph g(testDevice());
    const BufferRef out = g.createBuffer({ "subsurface profile out", bytes, 0 });
    g.addPass("subsurface profile", QueueType::Graphics, [&](PassBuilder& b) { b.use(out, Use::UavCompute); },
              [=](PassContext& ctx) {
                  const uint32_t k[4] = { ctx.uav(out), n, 0, 0 };
                  ctx.cmd->SetPipelineState(pso);
                  ctx.computeConstants(k, 4);
                  ctx.cmd->Dispatch((n + 63) / 64, 1, 1);
              });
    ID3D12Resource* dst = rb.Get();
    g.addPass("subsurface profile readback", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(out, Use::CopySrc);
                  b.keep();
              },
              [=](PassContext& ctx) { ctx.cmd->CopyBufferRegion(dst, 0, ctx.resource(out), 0, bytes); });
    g.execute(nullptr);
    for (uint32_t q = 0; q < kQueueTypeCount; ++q) testDevice().queue((QueueType)q).waitCpu(g.lastFence((QueueType)q));
    const float* v = nullptr;
    check(rb->Map(0, nullptr, (void**)&v), "map readback");
    // relative to the value, with a floor at 1e-3 of the quantity's scale (weights and densities fall to 0 in the tail)
    auto off = [](double got, double want, double scale) { return std::fabs(got - want) / std::max(std::fabs(want), 1e-3 * scale); };
    double worstDistance = 0, worstDensity = 0, worstIntegral = 0, worstRadius = 0, worstAngle = 0, worstWeight = 0;
    uint32_t offPlane = 0, noPath = 0;
    for (uint32_t i = 0; i < n; ++i)
    {
        const float* p = v + 24 * i;
        const float3 albedo{ p[0], p[1], p[2] }, path{ p[3], p[4], p[5] };
        const float r = p[6], h = p[7], xi = p[8], u = p[9], centre = p[23];
        const uint32_t k = (uint32_t)p[10], pairs = (uint32_t)p[11];
        const float3 d = m::subsurfaceDistance(path, albedo);
        const float dS = std::max({ d.x, d.y, d.z });
        offPlane += h > 0;
        noPath += path.z == 0;
        worstDistance = std::max({ worstDistance, off(p[12], d.x, d.x), off(p[13], d.y, d.y), off(p[14], d.z, d.z) });
        worstDensity = std::max(worstDensity, off(p[15], m::subsurfaceRadialPdf(d.x, r), 1 / d.x));
        worstIntegral = std::max(worstIntegral, std::fabs((double)p[16] - m::subsurfaceRadialCdf(d.x, r)));  // (absolute: 1 minus terms near 1)
        // radii as quantiles (in the tail a radius moves by more than 1e-4 of itself per float step of xi)
        worstRadius = std::max({ worstRadius, std::fabs((double)m::subsurfaceRadialCdf(dS, p[17]) - m::subsurfaceRadialCdf(dS, m::subsurfaceRadius(dS, xi))),
                                 std::fabs((double)m::subsurfaceRadialCdf(dS, p[18]) - m::subsurfaceRadialCdf(dS, m::subsurfaceSampleRadius(dS, centre, k, pairs, u))) });
        const double turn = std::fabs((double)p[19] - m::subsurfaceSampleAngle(k, u));
        worstAngle = std::max(worstAngle, std::min(turn, 6.28318530718 - turn));  // (on the circle: 0 and 2 pi are one angle)
        const float pdf = m::subsurfaceRadialPdf(dS, r);
        const float3 w = m::subsurfaceSampleWeight(d, r, h, pdf);
        worstWeight = std::max({ worstWeight, off(p[20], w.x, 1), off(p[21], w.y, 1), off(p[22], w.z, 1) });
    }
    rb->Unmap(0, nullptr);
    logf("    subsurface profile on the GPU vs scene::model over %u points (%u off the plane, %u without a blue mean free path): worst relative d %.2e, p %.2e, "
         "weights %.2e; P %.2e, radii as quantiles %.2e; angle %.2e rad\n",
         n, offPlane, noPath, worstDistance, worstDensity, worstWeight, worstIntegral, worstRadius, worstAngle);
    CHECK(offPlane > 0 && noPath > 0);
    CHECK(worstDistance < 1e-5 && worstDensity < 1e-4 && worstWeight < 1e-4);
    CHECK(worstIntegral < 1e-6 && worstRadius < 1e-5 && worstAngle < 1e-4);
    testDevice().deferRelease(rb);
}

UNX_TEST(hair_scattering_on_the_gpu)
{
    // Hair class: Passes/Hair/HairScattering.hlsli equals scene::model's twin (unx/scene/HairModel.h) at 4096 points -
    // the width-averaged kernel plain and spread, the forward and backward averages, what a fibre count lets through, the
    // backward lobe, a light's kernel and the moments - with the fibres read from the scene's material records (GpuScene's
    // Hair slots), which equal the materials' (Passes/Test/HairScatteringModel.hlsl).
    scene::Scene s = tinyScene();
    const uint32_t firstMaterial = (uint32_t)s.materials.size();
    {
        scene::Material hair;
        hair.name = "hair";
        hair.cls = scene::MaterialClass::Hair;
        hair.ior = 1.55f;
        hair.roughness = 0.3f;
        hair.hairEumelanin = 0.3f;
        s.materials.push_back(hair);  // blond
        hair.hairEumelanin = 2.5f;
        hair.roughness = 0.2f;
        hair.hairBetaN = 0.45f;
        s.materials.push_back(hair);  // dark, smoother along, rougher around
        hair.hairEumelanin = 0;
        hair.hairPheomelanin = 0;
        hair.baseColor = { 0.95f, 0.9f, 0.85f };  // by its colour (Chiang): nearly white
        hair.roughness = 0.5f;
        hair.hairBetaN = 0.25f;
        hair.hairTilt = 0.05f;
        s.materials.push_back(hair);
    }
    const uint32_t materialCount = (uint32_t)s.materials.size() - firstMaterial;
    scene::validate(s);
    GpuScene gs(testDevice());
    gs.upload(s);
    gpu::FrameConstants fc{};
    gs.fill(fc);
    ComPtr<ID3D12Resource> constants, rb;
    const uint32_t n = 4096, bytes = n * 192;
    {
        D3D12_HEAP_PROPERTIES up{ D3D12_HEAP_TYPE_UPLOAD }, rp{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC1 rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = (sizeof(gpu::FrameConstants) + 255) / 256 * 256;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(testDevice().d3d()->CreateCommittedResource3(&up, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&constants)),
              "constants");
        void* mapped = nullptr;
        check(constants->Map(0, nullptr, &mapped), "map constants");
        std::memcpy(mapped, &fc, sizeof fc);
        constants->Unmap(0, nullptr);
        rd.Width = bytes;
        check(testDevice().d3d()->CreateCommittedResource3(&rp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&rb)), "readback");
    }
    ID3D12PipelineState* pso = shaders().compute("Passes/Test/HairScatteringModel");
    RenderGraph g(testDevice());
    const BufferRef out = g.createBuffer({ "hair scattering out", bytes, 0 });
    const D3D12_GPU_VIRTUAL_ADDRESS address = constants->GetGPUVirtualAddress();
    g.addPass("hair scattering", QueueType::Graphics, [&](PassBuilder& b) { b.use(out, Use::UavCompute); },
              [=](PassContext& ctx) {
                  const uint32_t k[4] = { ctx.uav(out), n, firstMaterial, materialCount };
                  ctx.cmd->SetPipelineState(pso);
                  ctx.bindFrameConstants(address);
                  ctx.computeConstants(k, 4);
                  ctx.cmd->Dispatch((n + 63) / 64, 1, 1);
              });
    ID3D12Resource* dst = rb.Get();
    g.addPass("hair scattering readback", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(out, Use::CopySrc);
                  b.keep();
              },
              [=](PassContext& ctx) { ctx.cmd->CopyBufferRegion(dst, 0, ctx.resource(out), 0, bytes); });
    g.execute(nullptr);
    for (uint32_t q = 0; q < kQueueTypeCount; ++q) testDevice().queue((QueueType)q).waitCpu(g.lastFence((QueueType)q));
    const float* v = nullptr;
    check(rb->Map(0, nullptr, (void**)&v), "map readback");
    // worst difference per group, relative to the larger of the value and a floor (the kernels are sums of lobes whose
    // exponentials differ in the last bits between the two compilers)
    double worstKernel = 0, worstSpread = 0, worstAverage = 0, worstThrough = 0, worstBack = 0, worstLight = 0, worstMoments = 0;
    uint32_t lit = 0;
    auto differ = [](double& worst, float got, float want, double floor) { worst = std::max(worst, std::fabs((double)got - want) / std::max(std::fabs((double)want), floor)); };
    auto differ3 = [&](double& worst, const float* got, float3 want, double floor) {
        differ(worst, got[0], want.x, floor);
        differ(worst, got[1], want.y, floor);
        differ(worst, got[2], want.z, floor);
    };
    for (uint32_t i = 0; i < n; ++i)
    {
        const float* p = v + 48 * i;
        const scene::model::HairFibre f = scene::model::hairFibreOf(s.materials[firstMaterial + i % materialCount]);
        const float3 wo{ p[0], p[1], p[2] }, wi{ p[3], p[4], p[5] };
        const float front = p[6], behind = p[7], spread = p[8];
        const scene::model::HairStrand strand = scene::model::hairStrand(f, wo);
        const scene::model::HairAverage a = scene::model::hairAverage(f, wi.x);
        const scene::model::HairThrough t = scene::model::hairThrough(a, front);
        const scene::model::HairBack b = scene::model::hairBackscatter(a);
        const scene::model::HairMoments mo = scene::model::hairStrandMoments(strand, a, 1 - std::exp(-behind));
        differ3(worstKernel, p + 10, scene::model::hairStrandKernel(strand, wi, 0, false), 1e-3);
        differ3(worstSpread, p + 13, scene::model::hairStrandKernel(strand, wi, spread, true), 1e-3);
        differ3(worstAverage, p + 16, a.forward, 1e-3);
        differ3(worstAverage, p + 19, a.backward, 1e-3);
        differ(worstAverage, p[22], a.varianceForward, 1e-3);
        differ(worstAverage, p[23], a.varianceBackward, 1e-3);
        differ(worstAverage, p[24], a.shiftForward, 1e-3);
        differ(worstAverage, p[25], a.shiftBackward, 1e-3);
        differ(worstThrough, p[26], t.direct, 1e-3);
        differ3(worstThrough, p + 27, t.scattered, 1e-3);
        differ(worstThrough, p[30], t.spread, 1e-3);
        differ3(worstBack, p + 31, b.albedo, 1e-3);
        differ(worstBack, p[34], b.shift, 1e-3);
        differ(worstBack, p[35], b.variance, 1e-3);
        const float3 light = scene::model::hairStrandLight(strand, a, wi, front, behind);
        differ3(worstLight, p + 36, light, 1e-3);
        lit += light.y > 1e-3f;
        // (the first moments are sums of lobes of either sign: against a floor of 0.05 of the albedo's scale)
        differ3(worstMoments, p + 39, mo.albedo, 1e-3);
        differ3(worstMoments, p + 42, mo.along, 5e-2);
        differ3(worstMoments, p + 45, mo.across, 5e-2);
    }
    rb->Unmap(0, nullptr);
    logf("    hair scattering on the GPU vs scene::model over %u points (%u above 1e-3): worst relative - kernel %.2e, spread kernel %.2e, averages %.2e, through %.2e, "
         "backward lobe %.2e, light %.2e, moments %.2e\n",
         n, lit, worstKernel, worstSpread, worstAverage, worstThrough, worstBack, worstLight, worstMoments);
    CHECK(lit > n / 2);
    CHECK(worstKernel < 1e-3);
    CHECK(worstSpread < 1e-3);
    CHECK(worstAverage < 1e-3);
    CHECK(worstThrough < 1e-3);
    CHECK(worstBack < 1e-3);
    CHECK(worstLight < 1e-3);
    CHECK(worstMoments < 1e-3);
    testDevice().deferRelease(constants);
    testDevice().deferRelease(rb);
}

UNX_TEST(frame_renderer_records_with_track_stubs)
{
    QualityConfig q = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
    scene::Scene s = tinyScene();
    GpuScene gpuScene(testDevice());
    gpuScene.upload(s);
#if UNX_HAS_CLUSTERBUILDER
    gpuScene.setClusters(clusterbuilder::build(s, clusterbuilder::Settings::fromQuality(q)));
#endif
    gpu::FrameConstants fc{};
    gpuScene.fill(fc);
    CHECK(fc.instanceCount == 1 && fc.meshCount == 1 && fc.materialModelLut != gpu::kNone);
    FrameRenderer renderer(testDevice(), shaders(), q, gpuScene);
    RenderGraph graph(testDevice());
    FrameContext frame;
    frame.mainView = ViewDesc::fromCamera(s.cameras[0], 3840, 2160, {});
    TextureRef out = graph.createTexture({ "output", 3840, 2160, 1, 1, DXGI_FORMAT_R10G10B10A2_UNORM });
    ViewResources main = renderer.record(graph, frame, out);
    CHECK(main.frameConstants != 0);
    graph.execute(nullptr);
    testDevice().waitIdle();
}

namespace
{
// Runs this binary with 'argument' (a child mode of main) and returns its exit code and last line of output. 'fenceLimit'
// sets UNX_FENCE_TIMEOUT_S for the child only.
std::pair<DWORD, std::string> runChildMode(const wchar_t* argument, const wchar_t* fenceLimit = nullptr)
{
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring cmd = std::wstring(L"\"") + self + L"\" " + argument;
    wchar_t saved[64] = {};
    const bool hadLimit = GetEnvironmentVariableW(L"UNX_FENCE_TIMEOUT_S", saved, 64) > 0;
    if (fenceLimit) SetEnvironmentVariableW(L"UNX_FENCE_TIMEOUT_S", fenceLimit);
    SECURITY_ATTRIBUTES sa{ sizeof sa, nullptr, TRUE };
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    CHECK(CreatePipe(&readEnd, &writeEnd, &sa, 0));
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{ sizeof si };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeEnd;
    si.hStdError = writeEnd;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    CHECK(CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));
    if (fenceLimit) SetEnvironmentVariableW(L"UNX_FENCE_TIMEOUT_S", hadLimit ? saved : nullptr);
    CloseHandle(writeEnd);
    std::string output;
    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(readEnd, buffer, sizeof buffer, &got, nullptr) && got > 0) output.append(buffer, got);
    WaitForSingleObject(pi.hProcess, 60000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(readEnd);
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) output.pop_back();
    const std::string last = output.substr(output.find_last_of('\n') == std::string::npos ? 0 : output.find_last_of('\n') + 1);
    logf("    child exit %lu, last line '%s'\n", code, last.c_str());
    return { code, last };
}
} // namespace

UNX_TEST(gpu_lock_slice_protocol)
{
    // GpuLockSlice (INTERFACES 3.3, v1.42) on a private mutex and lock folder: acquire writes current.json and an acquire
    // line and sets UNX_GPU_LOCK, release removes both and writes the release line; HOLD blocks every acquire with its
    // reason; a slice yields to a live waiter that came before it, whatever the kinds (v1.85: first come), and takes the
    // lock once that process has ended (and removes its waiting file).
    namespace fs = std::filesystem;
    const std::string tag = std::to_string(GetCurrentProcessId());
    const fs::path dir = fs::temp_directory_path() / ("unx_gpulock_test_" + tag);
    fs::remove_all(dir);
    fs::create_directories(dir);
    {
        GpuLockSlice slice("T", "correctness", "unit test", dir.string(), "Local\\UnravelNext.GpuLockTest." + tag);
        CHECK(slice.acquire(std::chrono::seconds(5), "slice 1/2"));
        CHECK(fs::exists(dir / "current.json") && requireGpuLock("gpu_lock_slice_protocol") == "T");
        slice.release(0);
        CHECK(!fs::exists(dir / "current.json"));
        std::ofstream(dir / "HOLD") << "unit test hold\n";
        CHECK(!slice.acquire(std::chrono::milliseconds(600), "slice 2/2"));
        CHECK(slice.lastBlocker() == "HOLD: unit test hold");
        fs::remove(dir / "HOLD");
        STARTUPINFOW si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        wchar_t cmd[] = L"cmd.exe /c ping -n 3 127.0.0.1 >nul";
        CHECK(CreateProcessW(nullptr, cmd, nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi));
        const fs::path waiter = dir / "waiting" / (std::to_string(pi.dwProcessId) + ".json");
        // The waiter came first: its "since" is one second before now (the slice's own is the time of its acquire call; a
        // waiting file counts while its process is not more than 5 s younger than its "since").
        SYSTEMTIME earlier{};
        GetLocalTime(&earlier);
        FILETIME earlierTime{};
        CHECK(SystemTimeToFileTime(&earlier, &earlierTime));
        ULARGE_INTEGER ticks;
        ticks.LowPart = earlierTime.dwLowDateTime, ticks.HighPart = earlierTime.dwHighDateTime;
        ticks.QuadPart -= 10000000ull;
        earlierTime.dwLowDateTime = ticks.LowPart, earlierTime.dwHighDateTime = ticks.HighPart;
        CHECK(FileTimeToSystemTime(&earlierTime, &earlier));
        char since[32];
        std::snprintf(since, sizeof since, "%04u-%02u-%02uT%02u:%02u:%02u", earlier.wYear, earlier.wMonth, earlier.wDay, earlier.wHour, earlier.wMinute, earlier.wSecond);
        std::ofstream(waiter) << "{\"track\":\"W\",\"kind\":\"timing\",\"pid\":" << pi.dwProcessId << ",\"since\":\"" << since << "\",\"command\":\"waiter\"}";
        CHECK(!slice.acquire(std::chrono::milliseconds(600)));
        CHECK(slice.lastBlocker().rfind("in line behind W (timing", 0) == 0);
        WaitForSingleObject(pi.hProcess, 10000);
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        CHECK(slice.acquire(std::chrono::seconds(5), "slice 2/2") && !fs::exists(waiter));
    }  // the destructor releases
    std::ifstream in(dir / "history.log");
    std::stringstream history;
    history << in.rdbuf();
    const std::string h = history.str();
    size_t acquires = 0;
    for (size_t i = h.find("acquire T (correctness) :: unit test"); i != std::string::npos; i = h.find("acquire T (correctness) :: unit test", i + 1)) ++acquires;
    CHECK(acquires == 2 && h.find("release T (correctness) exit 0 slice 1/2 ") != std::string::npos && h.find("release T (correctness) exit 0 slice 2/2 ") != std::string::npos);
    in.close();
    fs::remove_all(dir);
}

UNX_TEST(device_removed_exit_policy)
{
    // Exit policy (tests, gates, tools): a child process of this binary reports a removal and must end with exit code
    // kDeviceRemovedExitCode and "UNX_DEVICE_REMOVED ..." as its last line of output.
    const auto [code, last] = runChildMode(L"--device-removed-exit-child");
    CHECK(code == (DWORD)kDeviceRemovedExitCode && last.rfind("UNX_DEVICE_REMOVED test.child hr 0x887A0006", 0) == 0);
}

UNX_TEST(fence_wait_limit_exit_policy)
{
    // A CPU wait for a fence value nobody signals (no GPU work: the value is simply never signalled) ends a tool at the
    // limit with exit code kFenceTimeoutExitCode and "UNX_FENCE_TIMEOUT ..." as its last line (UNX_FENCE_TIMEOUT_S = 2).
    const auto start = std::chrono::steady_clock::now();
    const auto [code, last] = runChildMode(L"--fence-timeout-child", L"2");
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    logf("    child ran %.1f s\n", seconds);
    CHECK(code == (DWORD)kFenceTimeoutExitCode && last.rfind("UNX_FENCE_TIMEOUT Queue::waitCpu(graphics) value ", 0) == 0);
    CHECK(last.find(" after 2 s") != std::string::npos && seconds >= 2.0 && seconds < 30.0);
}

UNX_TEST(fence_wait_limit_throw_policy)
{
    // Throw policy (a host process): the wait returns at the limit, the device counts as lost and the process lives on.
    const auto [code, last] = runChildMode(L"--fence-timeout-throw-child", L"2");
    CHECK(code == 0 && last == "fence wait returned, device lost 1");
}

// Last test (it leaves this process's device marked removed): the Throw policy of a host process.
UNX_TEST(zz_device_removed_throw_policy)
{
    testDevice();  // a device exists (the reason query has one)
    setDeviceRemovedPolicy(DeviceRemovedPolicy::Throw);
    bool thrown = false;
    try
    {
        check(DXGI_ERROR_DEVICE_HUNG, "test.throw");
    }
    catch (const DeviceRemovedError& e)
    {
        thrown = e.where == "test.throw" && e.hr == DXGI_ERROR_DEVICE_HUNG && std::string(e.what()).rfind("UNX_DEVICE_REMOVED test.throw", 0) == 0;
    }
    CHECK(thrown && deviceWasRemoved());
    // Paths that also run in destructors return instead of throwing once the device is removed.
    testDevice().waitIdle();
    setDeviceRemovedPolicy(DeviceRemovedPolicy::Exit);
}

UNX_TEST(white_balance_matrix)
{
    // v1.91 camera white balance (Post.cpp): D65 and 0 K give the identity (the chain skips the multiply); the locus
    // chromaticities match the CIE values (D65 (0.3127, 0.3290) from the daylight locus, illuminant A 2856 K
    // (0.4476, 0.4074) from the Planckian fit); the Bradford matrix maps the source white's linear sRGB to (1, 1, 1)
    // (a grey card under tungsten comes out grey); + tint moves the white towards green.
    float m[9];
    CHECK(!shading::whiteBalanceMatrix(0, 0, m) && m[0] == 1 && m[1] == 0 && m[2] == 0 && m[4] == 1 && m[8] == 1);
    CHECK(!shading::whiteBalanceMatrix(6504, 0, m));
    double x, y;
    shading::whiteBalanceChromaticity(6504, 0, x, y);
    CHECK(std::abs(x - 0.3127) < 1e-3 && std::abs(y - 0.3290) < 1e-3);
    shading::whiteBalanceChromaticity(2856, 0, x, y);
    CHECK(std::abs(x - 0.4476) < 3e-3 && std::abs(y - 0.4074) < 3e-3);
    CHECK(shading::whiteBalanceMatrix(2856, 0, m));
    const double X = x / y, Y = 1, Z = (1 - x - y) / y;
    const double rgb[3] = { 3.2404542 * X - 1.5371385 * Y - 0.4985314 * Z, -0.9692660 * X + 1.8760108 * Y + 0.0415560 * Z,
                            0.0556434 * X - 0.2040259 * Y + 1.0572252 * Z };
    for (int i = 0; i < 3; ++i)
    {
        const double o = m[i * 3] * rgb[0] + m[i * 3 + 1] * rgb[1] + m[i * 3 + 2] * rgb[2];
        CHECK(std::abs(o - 1) < 2e-3);
    }
    CHECK(m[0] < 1 && m[8] > 1);  // tungsten: red scaled down, blue up
    double xg, yg;
    shading::whiteBalanceChromaticity(6504, 0.02, xg, yg);
    CHECK(yg > 0.3290 + 0.01);
    CHECK(shading::whiteBalanceMatrix(6504, 0.02f, m));
}


UNX_TEST(round_pool_modes)
{
    // W2-R round basins (RoundPool.cpp, defect queue 13 (74)): Bessel values against tables, the Dini roots J_m' = 0, and
    // the per-order least-squares tables: synthesis then analysis recovers the amplitudes; frequencies rise with the
    // radial index, damping is positive, the order-0 k = 0 mode is the mean level.
    using namespace unx::water;
    CHECK(std::abs(roundBesselJ(0, 1.0) - 0.7651976866) < 1e-7);
    CHECK(std::abs(roundBesselJ(1, 1.0) - 0.4400505857) < 1e-7);
    CHECK(std::abs(roundBesselJ(5, 10.0) - (-0.2340615282)) < 1e-6);
    CHECK(std::abs(roundBesselJ(10, 5.0) - 0.0014678026) < 1e-7);
    CHECK(std::abs(roundBesselJ(2, 30.0) - roundBesselJ(2, 30.0)) == 0 && std::abs(roundBesselJ(40, 45.0)) < 0.2);
    const std::vector<double> r1 = roundDiniRoots(1, 20, 8);
    CHECK(r1.size() >= 3 && std::abs(r1[0] - 1.8411837813) < 1e-8 && std::abs(r1[1] - 5.3314427735) < 1e-8 && std::abs(r1[2] - 8.5363163663) < 1e-8);
    const std::vector<double> r0 = roundDiniRoots(0, 20, 8);
    CHECK(r0.size() >= 3 && r0[0] == 0 && std::abs(r0[1] - 3.8317059702) < 1e-8 && std::abs(r0[2] - 7.0155866698) < 1e-8);
    RoundPoolDesc d;
    d.radius = 1;
    d.depth = 0.5f;
    const RoundTables t = roundTables(d);
    CHECK(t.modeTotal > 8000 && t.modeCount[0] >= 100 && t.modeCount[255] >= 1 && t.modes.size() == t.modeTotal * 4);
    const uint32_t m = 3, count = t.modeCount[m], an = t.modeStart[256 + m], sy = t.modeStart[512 + m];
    std::vector<double> a(count, 0.0);
    a[0] = 1.0;
    a[2] = -0.5;
    a[std::min<uint32_t>(10, count - 1)] = 0.25;
    std::vector<double> ring(128, 0.0);
    for (uint32_t j = 0; j < 128; ++j)
        for (uint32_t n = 0; n < count; ++n) ring[j] += t.synthesis[sy + j * count + n] * a[n];
    double worst = 0;
    for (uint32_t n = 0; n < count; ++n)
    {
        double s = 0;
        for (uint32_t j = 0; j < 128; ++j) s += t.analysis[an + n * 128 + j] * ring[j];
        worst = std::max(worst, std::abs(s - a[n]));
    }
    CHECK(worst < 1e-3);
    CHECK(t.modes[0] == 0 && t.modes[4] > 0 && t.modes[8] > t.modes[4] && t.modes[7] > 0 && t.modes[11] > 0);
    // the sloshing mode (1, 0) of a 1 m, 0.5 m deep tub: k = 1.8412 / m, w^2 = k tanh(k d) g -> about 3.7 rad/s
    const uint32_t s1 = t.modeStart[1];
    CHECK(std::abs(t.modes[4 * s1] - std::sqrt(1.8411837813 * std::tanh(1.8411837813 * 0.5) * 9.81)) < 1e-3);
}


int main(int argc, char** argv)
{
    const char* filter = argc > 1 ? argv[1] : nullptr;
    if (filter && std::string(filter) == "--device-removed-exit-child") deviceRemoved("test.child", DXGI_ERROR_DEVICE_HUNG);
    if (filter && std::string(filter) == "--fence-timeout-child")
    {
        Queue& q = testDevice().queue(QueueType::Graphics);
        q.waitCpu(q.signal() + 1000);  // never signalled
        return 0;
    }
    if (filter && std::string(filter) == "--fence-timeout-throw-child")
    {
        setDeviceRemovedPolicy(DeviceRemovedPolicy::Throw);
        Queue& q = testDevice().queue(QueueType::Graphics);
        q.waitCpu(q.signal() + 1000);
        std::printf("fence wait returned, device lost %d\n", deviceWasRemoved() ? 1 : 0);
        std::fflush(stdout);
        TerminateProcess(GetCurrentProcess(), 0);  // (no teardown on a lost device: the line stays last)
    }
    int failed = 0, run = 0;
    // Tests named zz_* run after every other one, wherever the file defines them: zz_device_removed_throw_policy leaves
    // this process's device marked removed, and a GPU test after it would run on waits that return at once.
    std::stable_partition(registry().begin(), registry().end(), [](const TestCase& t) { return std::strncmp(t.name, "zz_", 3) != 0; });
    for (const TestCase& t : registry())
    {
        if (filter && !std::strstr(t.name, filter)) continue;
        ++run;
        try
        {
            t.fn();
            logf("PASS %s\n", t.name);
        }
        catch (const std::exception& e)
        {
            ++failed;
            logf("FAIL %s: %s\n", t.name, e.what());
        }
    }
    uint32_t debugErrors = 0;
    if (g_device)
    {
        g_device->waitIdle();
        debugErrors = g_device->drainDebugMessages();
    }
    logf("%d/%d passed, D3D12 debug-layer errors: %u\n", run - failed, run, debugErrors);
    return failed == 0 && debugErrors == 0 ? 0 : 1;
}

UNX_TEST(gpu_scene_fx_light_tail)
{
    // A3 FX particle lights (v1.79, S_STATUS 10): the light buffer's tail of F_max records after the scene lights, the count
    // word (0 after every capacity change), the frame constants, the 32,768 limit, and the tail kept across an origin rebase.
    scene::Scene s = tinyScene();
    GpuScene gs(testDevice());
    gs.upload(s);
    const uint32_t n = (uint32_t)s.lights.size();
    CHECK(n == 1);
    gpu::FrameConstants f{};
    gs.fill(f);
    CHECK(gs.fxLightRange().capacity == 0 && f.fxLightCount == gpu::kNone && f.fxLightCapacity == 0 && f.lightCount == n);

    auto readBack = [&](ID3D12Resource* buffer, uint64_t bytes) {
        std::vector<uint8_t> out(bytes);
        D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC1 rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> rb;
        check(testDevice().d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&rb)), "readback");
        CommandList cl = testDevice().acquireCommandList(QueueType::Graphics);
        cl.list->CopyBufferRegion(rb.Get(), 0, buffer, 0, bytes);
        testDevice().queue(QueueType::Graphics).waitCpu(testDevice().submit(cl));
        uint8_t* p = nullptr;
        check(rb->Map(0, nullptr, reinterpret_cast<void**>(&p)), "map");
        std::memcpy(out.data(), p, bytes);
        rb->Unmap(0, nullptr);
        return out;
    };
    auto checkTail = [&](uint32_t capacity) {
        const GpuScene::FxLightRange r = gs.fxLightRange();
        CHECK(r.first == n && r.capacity == capacity && r.lightUav != gpu::kNone && r.countUav != gpu::kNone && r.countSrv != gpu::kNone);
        CHECK(r.lightBuffer && r.countBuffer);
        CHECK(r.lightBuffer->GetDesc().Width >= (uint64_t)(n + capacity) * sizeof(gpu::Light));
        CHECK((r.lightBuffer->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) != 0);
        const std::vector<uint8_t> lights = readBack(r.lightBuffer, (uint64_t)(n + capacity) * sizeof(gpu::Light));
        CHECK(std::memcmp(lights.data(), gs.lights().data(), n * sizeof(gpu::Light)) == 0);  // the scene lights unchanged
        bool zero = true;
        for (size_t i = n * sizeof(gpu::Light); i < lights.size(); ++i) zero = zero && lights[i] == 0;
        CHECK(zero);  // the tail starts as type 0 / intensity 0
        const std::vector<uint8_t> count = readBack(r.countBuffer, 4);
        uint32_t c = ~0u;
        std::memcpy(&c, count.data(), 4);
        CHECK(c == 0);
        gpu::FrameConstants fc{};
        gs.fill(fc);
        CHECK(fc.fxLightCount == r.countSrv && fc.fxLightCapacity == capacity && fc.lightCount == n);
    };
    CHECK(gs.setFxLightCapacity(5));
    checkTail(5);
    CHECK(gs.setFxLightCapacity(5));  // unchanged: no rebuild needed
    checkTail(5);
    CHECK(gs.setFxLightCapacity(300));
    checkTail(300);
    CHECK(!gs.setFxLightCapacity(GpuScene::kMaxSceneLights - n + 1));  // over 32,768 lights: refused, tail unchanged
    checkTail(300);
    CHECK(gs.setFxLightCapacity(GpuScene::kMaxSceneLights - n));  // exactly at the limit
    checkTail(GpuScene::kMaxSceneLights - n);
    CHECK(gs.setFxLightCapacity(12));
    gs.rebase(float3{ 1024, 0, -2048 });  // the table is rebuilt from the source with the offset, tail included
    checkTail(12);
    CHECK(gs.lights()[0].position.x == s.lights[0].position.x - 1024);
    CHECK(gs.setFxLightCapacity(0));
    gs.fill(f);
    CHECK(gs.fxLightRange().capacity == 0 && f.fxLightCount == gpu::kNone && f.fxLightCapacity == 0);
}
