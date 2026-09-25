// Host scene gate (I track): renders a .unxscene saved by a host (UnxSceneSave, e.g. the old engine's data World from
// Unity) through FrameRenderer with the core harness, from its camera 0, at 4K and 1440p: the same scene outside Unity,
// so the Unity-hosted frame time can be compared with the renderer alone. GPU lock required:
//   GpuLock.ps1 -Track I -- build/I/bin/unx_gate_host_hostscene.exe --scene <file.unxscene> [--resolution 4K|1440p|both]
//       [--frames 600] [--out DIR]
//   unx_gate_host_hostscene.exe --scene <file.unxscene> --describe      (CPU only, no lock: content checks)
#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/render/FrameRenderer.h"
#include "unx/render/GpuLock.h"
#include "unx/render/GpuScene.h"
#include "unx/render/Harness.h"

#if __has_include("unx/clusterbuilder/ClusterBuilder.h")
#include "unx/clusterbuilder/ClusterBuilder.h"
#define HOST_SCENE_GATE 1
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::render;

namespace
{
// --describe: CPU-only check of a host-saved scene (no GPU, no lock): sun, camera, materials, and per mesh the values
// that turn shading into NaN or black (non-finite or zero-length normals/tangents, degenerate triangles) and whether
// the winding agrees with the vertex normals (counter-clockwise front faces).
void describe(const scene::Scene& s)
{
    auto finite3 = [](float3 v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); };
    logf("sun direction (%.4f, %.4f, %.4f) %.0f lux, colour (%.3f, %.3f, %.3f), angular radius %.5f\n", s.sun.direction.x, s.sun.direction.y, s.sun.direction.z,
         s.sun.illuminance, s.sun.color.x, s.sun.color.y, s.sun.color.z, s.sun.angularRadius);
    for (const scene::Camera& c : s.cameras)
        logf("camera '%s' at (%.2f, %.2f, %.2f) forward (%.3f, %.3f, %.3f) up (%.3f, %.3f, %.3f) fov %.3f ev100 %.2f\n", c.name.c_str(), c.position.x, c.position.y,
             c.position.z, c.forward.x, c.forward.y, c.forward.z, c.up.x, c.up.y, c.up.z, c.verticalFov, c.ev100);
    for (size_t i = 0; i < s.materials.size(); ++i)
    {
        const scene::Material& m = s.materials[i];
        logf("material %zu '%s' class %u base (%.3f, %.3f, %.3f) rough %.3f metal %.3f spec %.3f emissive (%.1f, %.1f, %.1f) cutoff %.2f ior %.2f two-sided %d\n", i,
             m.name.c_str(), (unsigned)m.cls, m.baseColor.x, m.baseColor.y, m.baseColor.z, m.roughness, m.metallic, m.specular, m.emissive.x, m.emissive.y,
             m.emissive.z, m.alphaCutoff, m.ior, m.twoSided ? 1 : 0);
    }
    for (size_t mi = 0; mi < s.meshes.size(); ++mi)
    {
        const scene::Mesh& m = s.meshes[mi];
        size_t badPos = 0, badNormal = 0, badTangent = 0, degenerate = 0, agree = 0, disagree = 0;
        float3 lo{ 1e30f, 1e30f, 1e30f }, hi{ -1e30f, -1e30f, -1e30f };
        for (float3 p : m.positions)
        {
            if (!finite3(p)) ++badPos;
            lo = { std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z) };
            hi = { std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z) };
        }
        for (float3 n : m.normals)
            if (!finite3(n) || std::fabs(length(n) - 1) > 1e-3f) ++badNormal;
        for (float4 t : m.tangents)
        {
            const float3 v{ t.x, t.y, t.z };
            if (!finite3(v) || std::fabs(length(v) - 1) > 1e-3f || std::fabs(std::fabs(t.w) - 1) > 1e-3f) ++badTangent;
        }
        for (size_t t = 0; t + 2 < m.indices.size(); t += 3)
        {
            const float3 a = m.positions[m.indices[t]], b = m.positions[m.indices[t + 1]], c = m.positions[m.indices[t + 2]];
            const float3 g = cross(b - a, c - a);
            if (length(g) < 1e-12f)
            {
                ++degenerate;
                continue;
            }
            const float3 n = m.normals[m.indices[t]] + m.normals[m.indices[t + 1]] + m.normals[m.indices[t + 2]];
            (dot(g, n) >= 0 ? agree : disagree)++;
        }
        logf("mesh %zu '%s': %zu vertices, %zu triangles, bounds (%.2f, %.2f, %.2f)-(%.2f, %.2f, %.2f), bad positions %zu, bad normals %zu, tangents %zu (bad %zu), "
             "degenerate %zu, winding agrees with normals %zu / disagrees %zu%s\n",
             mi, m.name.c_str(), m.positions.size(), m.indices.size() / 3, lo.x, lo.y, lo.z, hi.x, hi.y, hi.z, badPos, badNormal, m.tangents.size(), badTangent, degenerate,
             agree, disagree, m.skin.joints.empty() ? "" : " (skinned)");
    }
    for (size_t i = 0; i < s.instances.size(); ++i)
    {
        const scene::Instance& in = s.instances[i];
        const float3 x{ in.transform.m[0][0], in.transform.m[1][0], in.transform.m[2][0] }, y{ in.transform.m[0][1], in.transform.m[1][1], in.transform.m[2][1] },
            z{ in.transform.m[0][2], in.transform.m[1][2], in.transform.m[2][2] };
        logf("instance %zu mesh %u flags %u at (%.2f, %.2f, %.2f), axis lengths %.4f %.4f %.4f, handedness %+.3f\n", i, in.mesh, in.flags, in.transform.m[0][3],
             in.transform.m[1][3], in.transform.m[2][3], length(x), length(y), length(z), dot(cross(x, y), z));
    }
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
#if !HOST_SCENE_GATE
        (void)argc;
        (void)argv;
        fail("this build lacks V's cluster builder: Tools/CI/Build.ps1 -Track I");
#else
        std::string scenePath, resolutionArg = "both", out;
        uint32_t frames = 600;
        bool describeOnly = false;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) fail("missing value after %s", a.c_str());
                return argv[++i];
            };
            if (a == "--scene") scenePath = next();
            else if (a == "--resolution") resolutionArg = next();
            else if (a == "--frames") frames = (uint32_t)std::stoul(next());
            else if (a == "--out") out = next();
            else if (a == "--describe") describeOnly = true;
            else fail("unknown argument %s", a.c_str());
        }
        if (scenePath.empty()) fail("--scene <file.unxscene> is required");
        if (describeOnly)
        {
            describe(scene::load(scenePath));
            return 0;
        }
        requireGpuLock("unx_gate_host_hostscene");
        const QualityConfig quality = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
        const scene::Scene s = scene::load(scenePath);
        scene::validate(s);
        if (s.cameras.empty()) fail("scene %s has no camera", scenePath.c_str());
        ClusterData clusters = clusterbuilder::build(s, clusterbuilder::Settings::fromQuality(quality));
        logf("scene %s '%s' (%s): %zu meshes, %zu instances, %zu clusters, camera '%s'\n", scenePath.c_str(), s.name.c_str(), scene::contentHash(s).substr(0, 16).c_str(),
             s.meshes.size(), s.instances.size(), clusters.clusters.size(), s.cameras[0].name.c_str());
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
            HarnessOptions options;
            options.frames = frames;
            options.label = "I host scene " + (s.name.empty() ? std::string("unnamed") : s.name) + " " + rs;
            options.outputDirectory = out.empty() ? std::filesystem::path(UNX_SOURCE_DIR) / "Results/I/HostScene" : std::filesystem::path(out);
            options.writePassCsv = false;
            float4x4 prev = ViewDesc::fromCamera(s.cameras[0], res.width, res.height, {}).viewProj;
            const HarnessResult r = harness.run(res, options, [&](RenderGraph& g, const Resolution& rr, uint64_t frame) {
                FrameContext fc;
                fc.frameIndex = frame;
                fc.time = frame / 60.0;
                fc.deltaTime = 1.0f / 60;
                fc.mainView = ViewDesc::fromCamera(s.cameras[0], rr.width, rr.height, prev);
                prev = fc.mainView.viewProj;
                const TextureRef output = g.createTexture({ "host scene output", rr.width, rr.height, 1, 1, DXGI_FORMAT_R10G10B10A2_UNORM });
                renderer.record(g, fc, output);
            });
            harness.printSummary(r);
        }
        return 0;
#endif
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
