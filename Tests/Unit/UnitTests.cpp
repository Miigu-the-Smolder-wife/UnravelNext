// Fast unit tests (seconds): Core (SHA-256, quality config, jobs, distributions) and render-graph behaviour on the
// real device with the debug layer on (culling, aliasing, queue synchronisation, zero validation errors).
//   unx_unit_tests [filter]
#include "EmptyFrameScene.h"

#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/core/Jobs.h"
#include "unx/core/Sha256.h"
#include "unx/render/Harness.h"

#include <atomic>
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
Device& gpu()
{
    static Device device([] { DeviceOptions o; o.debugLayer = true; return o; }());
    g_device = &device;
    return device;
}
ShaderLibrary& shaders()
{
    static ShaderLibrary lib(gpu(), executableDirectory() / "shaders");
    return lib;
}
void runFrames(RenderGraph& graph, const std::function<void(RenderGraph&)>& build, int frames = 3)
{
    for (int f = 0; f < frames; ++f)
    {
        build(graph);
        graph.execute(nullptr);
        for (uint32_t q = 0; q < kQueueTypeCount; ++q) gpu().queue((QueueType)q).waitCpu(graph.lastFence((QueueType)q));
    }
}
void touchPass(RenderGraph& g, const char* name, QueueType q, std::vector<TextureRef> in, std::vector<TextureRef> out)
{
    ID3D12PipelineState* pso = shaders().compute("Test/Touch");
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
    QualityConfig q = QualityConfig::load(std::string(UNX_SOURCE_DIR) + "/Config/quality.toml");
    CHECK(resolutionFromString("4K", q).width == 3840);
    CHECK(resolutionFromString("2560x1440", q).height == 1440);
    CHECK(throws([&] { resolutionFromString("1920x1080", q); }));  // the harness refuses non-target resolutions
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
    RenderGraph g(gpu());
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
    RenderGraph g(gpu());
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
    RenderGraph g(gpu());
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

UNX_TEST(graph_single_queue_is_one_list)
{
    // Default policy: compute-queue passes run on the graphics queue; the frame is one command list.
    RenderGraph g(gpu());
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

UNX_TEST(graph_empty_frame_scene_is_valid)
{
    // The full 120-pass gate graph at 4K with the debug layer: no errors, plan reuse, aliasing effective.
    QualityConfig q = QualityConfig::load(std::string(UNX_SOURCE_DIR) + "/Config/quality.toml");
    Resolution r = resolutionFromString("4K", q);
    test::EmptyFrameScene scene(gpu(), shaders(), r.width, r.height);
    for (bool async : { true, false })
    {
        RenderGraph g(gpu());
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
