// M performance gate (ARCHITECTURE 4.2 table, design revision 1: per scene kind at 4K, the average column at 1440p):
// renders one of C's procedural scenes through the whole frame (FrameRenderer: every track of this build) and reports
// M's passes against the design terms:
//   m.resolve.begin + m.resolve   vis buffer -> G-buffer 8 B, material word, tile classes, reflection lobe tiles
//   m.shade*                      sky + surface class kernels -> final 4 B (design before band scheduling, 4.8)
//   m.edge.detect                 edge pixel detection (thin kernel)
//   m.edge.args + m.edge          edge (E) composite
// Performance runs only under the GPU lock (INTERFACES 3.3), in the integrated build (V's clusters, C's scenes):
//   powershell -File Tools/CI/GpuLock.ps1 -Track M -- build/all/bin/unx_gate_shading_mgate.exe
//       --scene city_block|forest_thin|... [--camera NAME] [--resolution 4K|1440p|both] [--frames 600] [--scale 1]
//       [--moving] [--out DIR]    (--camera: one of the scene's cameras, e.g. forest_combat eye|up|edge; default the first)
//       [--set key=value ...]   (e.g. shading.experiment_disable=1 for cost attribution; never in a gate verdict)
//       [--hash]                (FNV-1a of the last frame's display output; copies the output every frame, so hashed
//                                runs are not timing runs)
//       [--dump PATH]           (as --hash, and writes the last frame's output to PATH_<resolution>.mgd for image A/Bs)
#include <fstream>

#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/render/FrameRenderer.h"
#include "unx/render/GpuLock.h"
#include "unx/render/GpuScene.h"
#include "unx/render/Harness.h"
#include "unx/shading/ShadingSystem.h"
#if UNX_M_HAS_SCENEGEN && UNX_M_HAS_CLUSTERBUILDER
#include "unx/clusterbuilder/ClusterBuilder.h"
#include "unx/scenegen/SceneGen.h"
#endif

#include <chrono>
#include <cmath>
#include <functional>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::render;

namespace
{
// The scene's camera by name (default: the first); the path of the moving run is path 0 whichever camera is named.
scene::Camera cameraAt(const scene::Scene& s, bool moving, double time, const std::string& name)
{
    scene::Camera c = s.cameras.at(0);
    bool found = name.empty();
    for (const scene::Camera& k : s.cameras)
        if (k.name == name)
        {
            c = k;
            found = true;
        }
    if (!found) fail("scene %s has no camera %s", s.name.c_str(), name.c_str());
    if (!moving || s.paths.empty() || s.paths[0].keys.size() < 2) return c;
    const auto& keys = s.paths[0].keys;
    const double span = keys.back().time - keys.front().time;
    const float t = (float)(keys.front().time + std::fmod(time, span));
    size_t k = 0;
    while (k + 2 < keys.size() && keys[k + 1].time < t) ++k;
    const float u = std::clamp((t - keys[k].time) / std::max(keys[k + 1].time - keys[k].time, 1e-6f), 0.0f, 1.0f);
    c.position = keys[k].position + (keys[k + 1].position - keys[k].position) * u;
    c.forward = normalize(keys[k].forward + (keys[k + 1].forward - keys[k].forward) * u);
    c.up = normalize(keys[k].up + (keys[k + 1].up - keys[k].up) * u);
    return c;
}

double sumPasses(const HarnessResult& r, const std::function<bool(const std::string&)>& match)
{
    double ms = 0;
    for (const auto& [name, d] : r.passMs)
        if (match(name)) ms += d.median;
    return ms;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        std::string sceneName = "city_block", cameraName, resolutionArg = "both", out, dumpPath;
        uint32_t frames = 600;
        float scale = 1.0f;
        bool moving = false, hashOutput = false;
        std::vector<std::string> overrides;  // quality overrides (recorded in the quality hash), e.g. cost attribution
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) fail("missing value after %s", a.c_str());
                return argv[++i];
            };
            if (a == "--scene") sceneName = next();
            else if (a == "--camera") cameraName = next();
            else if (a == "--resolution") resolutionArg = next();
            else if (a == "--frames") frames = (uint32_t)std::stoul(next());
            else if (a == "--scale") scale = std::stof(next());
            else if (a == "--moving") moving = true;
            else if (a == "--out") out = next();
            else if (a == "--set") overrides.push_back(next());
            else if (a == "--hash") hashOutput = true;
            else if (a == "--dump") hashOutput = !(dumpPath = next()).empty();
            else fail("unknown argument %s", a.c_str());
        }
        requireGpuLock("unx_gate_shading_mgate");
#if !(UNX_M_HAS_SCENEGEN && UNX_M_HAS_CLUSTERBUILDER)
        fail("this build has no scene generator or cluster builder: use the integrated build (Build.ps1 -Track all)");
#else
        QualityConfig quality = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
        for (const std::string& o : overrides) quality.applyOverride(o);
        scenegen::Request request;
        bool found = false;
        for (scenegen::SceneId id : scenegen::allScenes())
            if (sceneName == scenegen::sceneName(id))
            {
                request.id = id;
                found = true;
            }
        if (!found) fail("unknown scene %s", sceneName.c_str());
        request.scale = scale;
        const scene::Scene s = scenegen::generate(request);
        const std::string label = cameraName.empty() ? sceneName : sceneName + "/" + cameraName;  // result lines
        ClusterData clusters = clusterbuilder::build(s, clusterbuilder::Settings::fromQuality(quality));
        logf("scene %s (scale %.2f): %zu meshes, %zu instances, %zu materials, %zu textures, %zu clusters\n", sceneName.c_str(), scale, s.meshes.size(), s.instances.size(),
             s.materials.size(), s.textures.size(), clusters.clusters.size());

        Device device({});
        ShaderLibrary shaders(device, executableDirectory() / "shaders");
        GpuScene gpuScene(device);
        gpuScene.upload(s);
        gpuScene.setClusters(std::move(clusters));
        Harness harness(device, quality);
        const std::vector<std::string> resolutions = resolutionArg == "both" ? std::vector<std::string>{ "4K", "1440p" } : std::vector<std::string>{ resolutionArg };
        for (const std::string& rs : resolutions)
        {
            const Resolution res = resolutionFromString(rs, quality);
            FrameRenderer renderer(device, shaders, quality, gpuScene, 2);
            // The display output is a persistent texture outside the graph (like a swap chain image), so the passes that
            // write it are live; a transient nobody reads would be culled with the shading kernels.
            ComPtr<ID3D12Resource> outputTexture;
            {
                D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
                D3D12_RESOURCE_DESC1 d{};
                d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
                d.Width = res.width;
                d.Height = res.height;
                d.DepthOrArraySize = d.MipLevels = 1;
                d.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
                d.SampleDesc.Count = 1;
                d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_COMMON, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&outputTexture)),
                      "M gate output");
            }
            // --hash: the display output of every frame is copied to this readback buffer (the last frame's copy remains).
            ComPtr<ID3D12Resource> readback;
            const uint32_t pitch = (res.width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
            if (hashOutput)
            {
                D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_READBACK };
                D3D12_RESOURCE_DESC1 d{};
                d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                d.Width = (uint64_t)pitch * res.height;
                d.Height = d.DepthOrArraySize = d.MipLevels = 1;
                d.SampleDesc.Count = 1;
                d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
                check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&readback)),
                      "M gate hash readback");
            }
            HarnessOptions options;
            options.frames = frames;
            options.label = "M " + sceneName + (moving ? " moving" : " static");
            if (!out.empty()) options.outputDirectory = out;
            float4x4 prev = ViewDesc::fromCamera(cameraAt(s, moving, 0, cameraName), res.width, res.height, {}).viewProj;
            const HarnessResult r = harness.run(res, options, [&](RenderGraph& g, const Resolution& rr, uint64_t frame) {
                FrameContext fc;
                fc.frameIndex = frame;
                fc.time = frame / 60.0;
                fc.deltaTime = 1.0f / 60;
                fc.mainView = ViewDesc::fromCamera(cameraAt(s, moving, fc.time, cameraName), rr.width, rr.height, prev);
                prev = fc.mainView.viewProj;
                const TextureRef output = g.importTexture(outputTexture.Get(), { "display output", rr.width, rr.height, 1, 1, DXGI_FORMAT_R10G10B10A2_UNORM }, D3D12_BARRIER_LAYOUT_COMMON);
                renderer.record(g, fc, output);
                if (hashOutput)
                {
                    const BufferRef rb = g.importBuffer(readback.Get(), { "M gate hash readback", (uint64_t)pitch * rr.height, 0 });
                    const uint32_t w = rr.width, h = rr.height;
                    ID3D12Resource* dst = readback.Get();
                    ID3D12Resource* src = outputTexture.Get();
                    g.addPass("m.gate.hash", QueueType::Graphics,
                              [&](PassBuilder& b) {
                                  b.use(output, Use::CopySrc);
                                  b.use(rb, Use::CopyDst);
                                  b.keep();
                              },
                              [=](PassContext& c) {
                                  D3D12_TEXTURE_COPY_LOCATION to{ dst, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                                  to.PlacedFootprint.Footprint = { DXGI_FORMAT_R10G10B10A2_UNORM, w, h, 1, pitch };
                                  D3D12_TEXTURE_COPY_LOCATION from{ src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                                  from.SubresourceIndex = 0;
                                  c.cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                              });
                }
            });
            harness.printSummary(r);
            if (hashOutput)
            {
                void* p = nullptr;
                check(readback->Map(0, nullptr, &p), "map M gate hash readback");
                uint64_t hash = 1469598103934665603ull;
                for (uint32_t y = 0; y < res.height; ++y)
                {
                    const uint8_t* row = static_cast<const uint8_t*>(p) + (size_t)y * pitch;
                    for (uint32_t x = 0; x < res.width * 4; ++x) hash = (hash ^ row[x]) * 1099511628211ull;
                }
                if (!dumpPath.empty())
                {
                    // Raw rows of the last frame's display output (RGB10A2, width x 4 bytes each), after a 16-byte header
                    // { 'MGD1', width, height, 0 }: for image comparisons between builds (the frame is not bit-deterministic
                    // run to run: other tracks' atomics and accumulations).
                    std::vector<uint8_t> file(16 + (size_t)res.width * 4 * res.height);
                    const uint32_t header[4] = { 0x3144474Du, res.width, res.height, 0 };
                    std::memcpy(file.data(), header, 16);
                    for (uint32_t y = 0; y < res.height; ++y)
                        std::memcpy(file.data() + 16 + (size_t)y * res.width * 4, static_cast<const uint8_t*>(p) + (size_t)y * pitch, (size_t)res.width * 4);
                    std::ofstream f(dumpPath + "_" + rs + ".mgd", std::ios::binary);
                    f.write(reinterpret_cast<const char*>(file.data()), (std::streamsize)file.size());
                    if (!f) fail("cannot write %s_%s.mgd", dumpPath.c_str(), rs.c_str());
                }
                D3D12_RANGE none{ 0, 0 };
                readback->Unmap(0, &none);
                logf("M %s %s: display output hash %016llx (last frame)\n", label.c_str(), rs.c_str(), (unsigned long long)hash);
            }
            // Planar reflection views (R's renderView) run M's passes under ".planar" names: their cost is R's reflection
            // budget (ARCHITECTURE 2.6 C_planar), reported apart.
            // Banded passes are "<group>.<pass>.b<band>": the frame's lighting group "lit" (v1.31, S's and M's passes) or M's
            // own "m.lit" ("m.lit.planar" in planar views).
            auto planar = [](const std::string& n) { return n.find(".planar") != std::string::npos; };
            auto banded = [](const std::string& n, const char* pass) {
                return n.rfind(std::string("lit.") + pass + ".b", 0) == 0 || n.rfind(std::string("m.lit.") + pass + ".b", 0) == 0;
            };
            const double resolve = sumPasses(r, [&](const std::string& n) { return n.rfind("m.resolve", 0) == 0 && !planar(n); });
            const double shade = sumPasses(r, [&](const std::string& n) { return (n.rfind("m.shade", 0) == 0 || banded(n, "shade")) && !planar(n); });
            const double planarM = sumPasses(r, [&](const std::string& n) { return n.rfind("m.", 0) == 0 && planar(n); });
            const bool is4k = res.width == 3840;
            const double detect = sumPasses(r, [&](const std::string& n) { return banded(n, "edge.detect") && !planar(n); });
            const double edge = sumPasses(r, [&](const std::string& n) { return n.rfind("m.edge", 0) == 0 && !planar(n); });
            // Coverage composite over V's band B fragments (design COVERAGE_REDESIGN 4.5; with visibility.coverage_layer).
            const double coverage = sumPasses(r, [&](const std::string& n) { return n.rfind("m.coverage", 0) == 0 && !planar(n); });
            const shading::Stats st = shading::latestStats(renderer.trackState());
            const double pixels = (double)res.width * res.height;
            // Design terms (ARCHITECTURE 4.2 table, revision 1): resolve and shading kernel before band scheduling, edge
            // detection, E composite with UI. 4K by scene kind; 1440p is the table's average column (shading scaled to
            // before bands like 4K city: 0.24 x 0.70 / 0.55).
            struct Design { double resolve, shade, detect, composite; };
            Design d{ 0.19, 0.24 * 0.70 / 0.55, 0.05, 0.07 };
            const char* kind = "1440p average";
            if (is4k)
            {
                kind = "not in the table";
                d = { 0, 0, 0.10, 0 };
                if (sceneName.rfind("city", 0) == 0) { d = { 0.55, 0.70, 0.10, 0.15 }; kind = "city"; }
                else if (sceneName.rfind("forest", 0) == 0) { d = { 0.33, 0.50, 0.10, 0.08 }; kind = "forest"; }
                else if (sceneName.find("water") != std::string::npos || sceneName.find("lake") != std::string::npos) { d = { 0.45, 0.60, 0.10, 0.12 }; kind = "waterside"; }
                else if (sceneName.rfind("interior", 0) == 0) { d = { 0.55, 0.70, 0.10, 0.12 }; kind = "interior"; }
            }
            logf("M %s %s: edge detection %.3f ms (design %.2f) | edge composite %.3f ms (design %.2f) on %u edge pixels (%.2f %% of the view) | class tiles sky %u "
                 "opaque %u subsurface %u water %u of %u\n",
                 label.c_str(), rs.c_str(), detect, d.detect, edge, d.composite, st.edgePixels, 100.0 * st.edgePixels / pixels, st.classTiles[0], st.classTiles[1],
                 st.classTiles[2], st.classTiles[3], st.tiles);
            logf("M %s %s: material resolve %.3f ms (design %.2f) | shading %.3f ms (design %.2f before bands) | design kind: %s | planar views, M passes %.3f ms | "
                 "frame %.3f ms\n",
                 label.c_str(), rs.c_str(), resolve, d.resolve, shade, d.shade, kind, planarM, r.gpuFrameMs.median);
            if (coverage > 0) logf("M %s %s: coverage composite %.3f ms\n", label.c_str(), rs.c_str(), coverage);
        }
        return 0;
#endif
    }
    catch (const std::exception& e)
    {
        logf("unx_gate_shading_mgate: %s\n", e.what());
        return 1;
    }
}
