// Fast unit tests (seconds): Core (SHA-256, quality config, jobs, distributions) and render-graph behaviour on the
// real device with the debug layer on (culling, aliasing, queue synchronisation, zero validation errors).
//   unx_unit_tests [filter]
#include "EmptyFrameScene.h"

#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/core/Jobs.h"
#include "unx/core/Sha256.h"
#include "unx/render/FrameRenderer.h"
#include "unx/render/Harness.h"
#include "unx/scene/MaterialModel.h"
#if UNX_HAS_CLUSTERBUILDER
#include "unx/clusterbuilder/ClusterBuilder.h"
#endif

#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <string>
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
Device& testDevice()
{
    static Device device([] { DeviceOptions o; o.debugLayer = true; return o; }());
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
    CHECK(throws([&] { resolutionFromString("1920x1080", q); }));  // the harness refuses non-target resolutions
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

int main(int argc, char** argv)
{
    const char* filter = argc > 1 ? argv[1] : nullptr;
    int failed = 0, run = 0;
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
