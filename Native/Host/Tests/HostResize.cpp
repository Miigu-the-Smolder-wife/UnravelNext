// Output size changes (U2 green rectangle: a D0 play capture showed a 571 x 587 top-left region tinted (0, 0.97 G, 0)
// after the first play-mode frames rendered at that size and the capture target switched to 1280 x 720):
//   1. a renderer that drew frames at 571 x 587 (and at 1920 x 1080) and then 64 at 1280 x 720 differs from one that only
//      drew 1280 x 720 no more inside the old 571 x 587 region than elsewhere (history-dependent state differs everywhere
//      alike; a resource kept at the earlier size would concentrate its difference in that region);
//   2. no D3D12 debug-layer errors.
// Correctness run (standalone HostRenderer, hardware GPU; GpuLock -Kind correctness).
#include "Renderer/HostRenderer.h"

#include "TestScenes.h"
#include "unx/core/File.h"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::host;
using namespace unx::render;

namespace
{
std::vector<std::string> diagnosticOverrides;
std::filesystem::path dumpDirectory;
uint32_t settleFrames = 64;
scene::Scene litScene()
{
    scene::Scene s = host::test::oneBox();
    s.materials[0].baseColor = { 0.7f, 0.6f, 0.5f };
    scene::Mesh floor;
    floor.name = "floor";
    const float h = 30;
    floor.positions = { { -h, -0.5f, h }, { h, -0.5f, h }, { h, -0.5f, -h }, { -h, -0.5f, -h } };
    floor.normals.assign(4, float3{ 0, 1, 0 });
    floor.indices = { 0, 1, 2, 0, 2, 3 };
    floor.submeshes.push_back({ 0, 6, 0 });
    s.meshes.push_back(std::move(floor));
    scene::Instance fi;
    fi.mesh = 1;
    s.instances.push_back(fi);
    s.cameras[0].ev100 = 13;
    return s;
}

// frames[k] frames at sizes[k]; the last frame of the last size is read back.
std::vector<uint32_t> renderSequence(const std::vector<std::pair<uint32_t, uint32_t>>& sizes, const std::vector<uint32_t>& frames, uint32_t& errors, uint32_t& w, uint32_t& h)
{
    HostRendererOptions o;
    o.standalone = true;
    o.framesInFlight = 2;
    o.debugLayer = true;
    o.shaderDirectory = executableDirectory() / "shaders";
    o.qualityDirectory = std::filesystem::path(UNX_SOURCE_DIR) / "Config/quality";
    o.qualityOverrides = { "gi.deterministic=true" };
    o.qualityOverrides.insert(o.qualityOverrides.end(), diagnosticOverrides.begin(), diagnosticOverrides.end());
    HostRenderer r(o);
    r.scene() = litScene();
    r.commit();
    std::vector<uint32_t> pixels;
    uint64_t index = 0;
    for (size_t k = 0; k < sizes.size(); ++k)
        for (uint32_t f = 0; f < frames[k]; ++f)
        {
            FramePacket p;
            p.frameIndex = index;
            p.time = 0;  // a still scene: frame n of either renderer sees the same inputs
            ++index;
            p.deltaTime = 1.0f / 60;
            p.width = sizes[k].first;
            p.height = sizes[k].second;
            p.camera = r.scene().cameras[0];
            const bool last = k + 1 == sizes.size() && f + 1 == frames[k];
            if (last) pixels.assign((size_t)p.width * p.height, 0);
            w = p.width, h = p.height;
            r.renderStandalone(r.queueFrame(std::move(p)), last ? pixels.data() : nullptr, last ? pixels.size() * 4 : 0);
        }
    errors += r.debugErrors();
    return pixels;
}

int channelDiff(uint32_t a, uint32_t b)
{
    int d = 0;
    for (int c = 0; c < 3; ++c) d = std::max(d, std::abs((int)((a >> (10 * c)) & 1023u) - (int)((b >> (10 * c)) & 1023u)));
    return d;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            if (arg == "--set" && i + 1 < argc) diagnosticOverrides.push_back(argv[++i]);
            else if (arg == "--settle-frames" && i + 1 < argc) settleFrames = (uint32_t)std::stoul(argv[++i]);
            else if (arg == "--dump-dir" && i + 1 < argc) dumpDirectory = argv[++i];
            else fail("unknown argument %s", arg.c_str());
        }
        uint32_t failures = 0, errors = 0, w = 0, h = 0;
        auto expect = [&](const char* what, bool ok) {
            logf("  %-96s %s\n", what, ok ? "ok" : "FAILED");
            if (!ok) ++failures;
        };
        // 12 frames at the first size, then 64 at 1280 x 720 (R's GI cache converges within 64 frames after a change)
        const std::vector<uint32_t> frames = { 12, settleFrames };
        const std::vector<uint32_t> fresh = renderSequence({ { 1280, 720 }, { 1280, 720 } }, frames, errors, w, h);
        const std::vector<uint32_t> resized = renderSequence({ { 571, 587 }, { 1280, 720 } }, frames, errors, w, h);
        const std::vector<uint32_t> larger = renderSequence({ { 1920, 1080 }, { 1280, 720 } }, frames, errors, w, h);
        if (!dumpDirectory.empty())
        {
            std::filesystem::create_directories(dumpDirectory);
            const std::vector<uint32_t>* images[] = {&fresh, &resized, &larger};
            const char* names[] = {"fresh", "small-first", "large-first"};
            for (int i = 0; i < 3; ++i)
            {
                std::ofstream stream(dumpDirectory / (std::string(names[i]) + ".rgb10"), std::ios::binary);
                stream.write((const char*)images[i]->data(), (std::streamsize)images[i]->size() * 4);
            }
        }
        auto compare = [&](const std::vector<uint32_t>& img, const char* name) {
            uint32_t inside = 0, outside = 0, worstIn = 0, worstOut = 0;
            double sumIn[3]{}, sumOut[3]{}, square = 0;
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x)
                {
                    const int d = channelDiff(img[(size_t)y * w + x], fresh[(size_t)y * w + x]);
                    const bool in = x < 571 && y < 587;
                    for (int c = 0; c < 3; ++c)
                    {
                        const int delta = int((img[(size_t)y * w + x] >> (10 * c)) & 1023u) - int((fresh[(size_t)y * w + x] >> (10 * c)) & 1023u);
                        (in ? sumIn : sumOut)[c] += delta;
                        square += double(delta) * delta;
                    }
                    if (d > 1) ++(in ? inside : outside);
                    (in ? worstIn : worstOut) = std::max<uint32_t>(in ? worstIn : worstOut, (uint32_t)d);
                }
            // View-dependent GI samples concentrate near actual bounce geometry, so the rate of differing pixels
            // need not be spatially uniform. The regression was a broad regional tint, not sparse local GI noise.
            // Require sub-code signed regional errors and sub-code RMS over the complete image.
            const double inRate = inside / (571.0 * 587.0), outRate = outside / ((double)w * h - 571.0 * 587.0);
            logf("  %s then 1280x720 vs only 1280x720: %.3f %% of the old 571x587 region differs by > 1 code (worst %u), %.3f %% of the rest (worst %u)\n",
                 name, 100 * inRate, worstIn, 100 * outRate, worstOut);
            bool unbiased = true;
            for (int c = 0; c < 3; ++c)
            {
                const double a = sumIn[c] / (571.0 * 587.0), b = sumOut[c] / ((double)w * h - 571.0 * 587.0);
                logf("    channel %d: signed old-region %.5f codes, rest %.5f codes\n", c, a, b);
                unbiased &= std::fabs(a) < 1 && std::fabs(b) < 1 && std::fabs(a - b) < 1;
            }
            const double rms = std::sqrt(square / ((double)w * h * 3));
            logf("    full-image RMS %.5f RGB10 codes\n", rms);
            return unbiased && rms < 1;
        };
        expect("571x587 -> 1280x720: sub-code regional bias and full-image RMS", compare(resized, "571x587"));
        expect("1920x1080 -> 1280x720: sub-code regional bias and full-image RMS", compare(larger, "1920x1080"));
        expect("D3D12 debug layer errors 0", errors == 0);
        logf(failures ? "HOST RESIZE TEST FAILED (%u)\n" : "HOST RESIZE TEST PASS\n", failures);
        return failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        logf("HOST RESIZE TEST ERROR: %s\n", e.what());
        return 2;
    }
}
