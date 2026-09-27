// Host dynamic-load gate (I track): the per-frame path a Unity host drives, at RPP-1 dynamic scale, through the exported
// ABI (UnravelNext.dll, standalone renderer), in an RPP-1 scene: the scene (UnxSceneLoad; C's generator, unx_scenegen
// --scene <s> --out <dir> --bodies <dir>/<s>_bodies.json) with its rigid bodies moving as the placement file says
// (BodiesFile.h: resting, falling, rolling, replayed kinematically; UnxFrameSetTransforms every frame) and skinned
// characters (a 64-bone tube stand-in, no character assets yet) on the file's character slots, posed every frame
// (UnxFrameSetSkeletons, one call); camera 0 of the scene. Reports the host-side CPU cost of the updates, the renderer's
// CPU record/submit and the GPU frame (per pass) at 4K and 1440p. GPU lock required:
//   GpuLock.ps1 -Track I -- build/I/bin/unx_gate_host_hostdynamic.exe --scene <dir>/<s>.unxscene [--bodies-file <json>]
//       [--characters 256] [--bones 64] [--triangles 60000] [--frames 600] [--resolution 4K|1440p|both]
//   unx_gate_host_hostdynamic.exe --scene ... --save-scene <file.unxscene>   (content only, no lock; inspect with
//       unx_gate_host_hostscene --scene <file> --describe)
//   ... --bench <dir>/gamebench_<variant>.json: the game bench's scene, bodies and camera track (BenchTrack.h): one frame
//       per tick of the track, cuts as camera cuts, results per case (and the first frame after every cut).
// The bodies file must come from the same generator build as the scene (every body's instance is dynamic and starts at the
// file's position); a missing or mismatched file stops the gate (no fallback placement).
#include "BenchTrack.h"
#include "BodiesFile.h"
#include "Contention.h"

#include "unx/host/UnravelNextHost.h"
#include "unx/scene/SceneData.h"

#include "unx/core/File.h"
#include "unx/render/GpuLock.h"
#include "unx/render/Harness.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

using namespace unx;
using render::Distribution;
namespace gate = unx::host::gate;

namespace
{
struct Api
{
    HMODULE dll = nullptr;
#define UNX_FN(name) decltype(&::name) name = nullptr;
    UNX_FN(UnxLastError)
    UNX_FN(UnxRendererCreate)
    UNX_FN(UnxRendererDestroy)
    UNX_FN(UnxSceneAddMaterial)
    UNX_FN(UnxSceneAddMesh)
    UNX_FN(UnxSceneAddSkeleton)
    UNX_FN(UnxSceneAddInstance)
    UNX_FN(UnxSceneSetEnvironment)
    UNX_FN(UnxEnvironmentDefaults)
    UNX_FN(UnxSceneCommit)
    UNX_FN(UnxSceneSave)
    UNX_FN(UnxSceneLoad)
    UNX_FN(UnxFrameSetTransforms)
    UNX_FN(UnxFrameSetSkeletons)
    UNX_FN(UnxFrameQueue)
    UNX_FN(UnxFrameRenderStandalone)
    UNX_FN(UnxFrameStatsLatest)
    UNX_FN(UnxFramePassTimingsLatest)
    UNX_FN(UnxFrameGraphStatsLatest)
    UNX_FN(UnxFrameSetDiscontinuity)
#undef UNX_FN
    void load(const std::filesystem::path& path)
    {
        dll = LoadLibraryW(path.c_str());
        if (!dll) fail("LoadLibrary %s failed (%lu)", path.string().c_str(), GetLastError());
#define UNX_FN(name)                                                                                                     \
    name = reinterpret_cast<decltype(name)>(GetProcAddress(dll, #name));                                                 \
    if (!name) fail("UnravelNext.dll does not export " #name);
        UNX_FN(UnxLastError)
        UNX_FN(UnxRendererCreate)
        UNX_FN(UnxRendererDestroy)
        UNX_FN(UnxSceneAddMaterial)
        UNX_FN(UnxSceneAddMesh)
        UNX_FN(UnxSceneAddSkeleton)
        UNX_FN(UnxSceneAddInstance)
        UNX_FN(UnxSceneSetEnvironment)
        UNX_FN(UnxEnvironmentDefaults)
        UNX_FN(UnxSceneCommit)
        UNX_FN(UnxSceneSave)
        UNX_FN(UnxSceneLoad)
        UNX_FN(UnxFrameSetTransforms)
        UNX_FN(UnxFrameSetSkeletons)
        UNX_FN(UnxFrameQueue)
        UNX_FN(UnxFrameRenderStandalone)
        UNX_FN(UnxFrameStatsLatest)
        UNX_FN(UnxFramePassTimingsLatest)
        UNX_FN(UnxFrameGraphStatsLatest)
        UNX_FN(UnxFrameSetDiscontinuity)
#undef UNX_FN
    }
    void ok(int32_t r, const char* what) const
    {
        if (r != UNX_OK) fail("%s returned %d: %s", what, r, UnxLastError());
    }
};

template <size_t N>
void copyName(char (&dst)[N], const std::string& s)
{
    std::memset(dst, 0, N);
    std::memcpy(dst, s.data(), std::min(s.size(), N - 1));
}

void identity(float* m)
{
    std::memset(m, 0, 12 * sizeof(float));
    m[0] = m[5] = m[10] = 1;
}

// Row-major 3x4: rotation about Y by 'angle', uniform scale 1, translation t.
void yaw(float* m, float angle, float tx, float ty, float tz)
{
    const float c = std::cos(angle), s = std::sin(angle);
    const float v[12] = { c, 0, s, tx, 0, 1, 0, ty, -s, 0, c, tz };
    std::memcpy(m, v, sizeof v);
}

// A box with 24 vertices (flat normals).
uint32_t addBox(const Api& api, UnxRenderer r, uint32_t material, float hx, float hy, float hz)
{
    std::vector<float> p, n, uv;
    std::vector<uint32_t> idx;
    const float axes[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    for (int f = 0; f < 6; ++f)
    {
        const float* a = axes[f];
        // face axes u, w = a x u, so u x w = a
        const float u[3] = { a[0] != 0 ? 0.0f : 1.0f, 0, a[0] != 0 ? 1.0f : 0.0f };
        const float w[3] = { a[1] * u[2] - a[2] * u[1], a[2] * u[0] - a[0] * u[2], a[0] * u[1] - a[1] * u[0] };
        const uint32_t base = (uint32_t)(p.size() / 3);
        for (int c = 0; c < 4; ++c)
        {
            const float su = (c == 1 || c == 2) ? 1.0f : -1.0f, sw = (c >= 2) ? 1.0f : -1.0f;
            p.push_back((a[0] + u[0] * su + w[0] * sw) * hx);
            p.push_back((a[1] + u[1] * su + w[1] * sw) * hy);
            p.push_back((a[2] + u[2] * su + w[2] * sw) * hz);
            n.insert(n.end(), { a[0], a[1], a[2] });
            uv.insert(uv.end(), { (su + 1) * 0.5f, (sw + 1) * 0.5f });
        }
        // counter-clockwise seen from outside (normal a): u x w = a
        idx.insert(idx.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
    }
    UnxSubmesh sub{ 0, (uint32_t)idx.size(), material, 0 };
    UnxMeshDesc d{};
    d.size = sizeof d;
    d.version = 1;
    d.vertexCount = (uint32_t)(p.size() / 3);
    d.indexCount = (uint32_t)idx.size();
    d.submeshCount = 1;
    d.positions = p.data();
    d.normals = n.data();
    d.uv0 = uv.data();
    d.indices = idx.data();
    d.submeshes = &sub;
    copyName(d.name, "box");
    uint32_t index = 0;
    api.ok(api.UnxSceneAddMesh(r, &d, &index), "UnxSceneAddMesh box");
    return index;
}

// A tube of 'bones' segments along +Y (the character stand-in: one shared mesh, 'triangles' triangles, 96 sides), each
// ring skinned to the bone it lies in and blended into the next over the segment's upper half (two influences).
uint32_t addTube(const Api& api, UnxRenderer r, uint32_t material, uint32_t bones, uint32_t triangles, float radius, float segment, uint64_t& built)
{
    const uint32_t sides = 96, rings = std::max(bones, (triangles + 2 * sides - 1) / (2 * sides));
    const float length = segment * bones;
    std::vector<float> p, n, w, ib;
    std::vector<uint16_t> j;
    std::vector<uint32_t> idx;
    for (uint32_t ring = 0; ring <= rings; ++ring)
    {
        const float y = length * ring / rings, s = y / segment;
        const uint32_t b0 = std::min((uint32_t)s, bones - 1), b1 = std::min(b0 + 1, bones - 1);
        const float w1 = b1 == b0 ? 0.0f : std::clamp(s - (float)b0 - 0.5f, 0.0f, 0.5f);
        for (uint32_t k = 0; k < sides; ++k)
        {
            const float a = 6.2831853f * k / sides;
            p.insert(p.end(), { radius * std::cos(a), y, radius * std::sin(a) });
            n.insert(n.end(), { std::cos(a), 0, std::sin(a) });
            j.insert(j.end(), { (uint16_t)b0, (uint16_t)b1, 0, 0 });
            w.insert(w.end(), { 1 - w1, w1, 0, 0 });
        }
    }
    for (uint32_t ring = 0; ring < rings; ++ring)
        for (uint32_t k = 0; k < sides; ++k)
        {
            const uint32_t a = ring * sides + k, b = ring * sides + (k + 1) % sides, c = a + sides, d2 = b + sides;
            // outward, counter-clockwise from outside
            idx.insert(idx.end(), { a, c, b, b, c, d2 });
        }
    for (uint32_t b = 0; b < bones; ++b)
    {
        float m[12];
        identity(m);
        m[7] = -segment * b;  // inverse bind: bone b sits at y = segment * b
        ib.insert(ib.end(), m, m + 12);
    }
    UnxSubmesh sub{ 0, (uint32_t)idx.size(), material, 0 };
    UnxMeshDesc d{};
    d.size = sizeof d;
    d.version = 1;
    d.vertexCount = (uint32_t)(p.size() / 3);
    d.indexCount = (uint32_t)idx.size();
    d.submeshCount = 1;
    d.jointCount = bones;
    d.positions = p.data();
    d.normals = n.data();
    d.indices = idx.data();
    d.submeshes = &sub;
    d.joints = j.data();
    d.weights = w.data();
    d.inverseBind = ib.data();
    copyName(d.name, "tube");
    built = idx.size() / 3;
    uint32_t index = 0;
    api.ok(api.UnxSceneAddMesh(r, &d, &index), "UnxSceneAddMesh tube");
    return index;
}

// Pose of a bent tube: each bone rotates about Z by a time-varying angle relative to its parent.
void pose(float* joints, uint32_t bones, float segment, float t, float phase)
{
    float px = 0, py = 0, angle = 0;
    for (uint32_t b = 0; b < bones; ++b)
    {
        const float c = std::cos(angle), s = std::sin(angle);
        const float m[12] = { c, -s, 0, px, s, c, 0, py, 0, 0, 1, 0 };
        std::memcpy(joints + 12 * b, m, sizeof m);
        px += -s * segment;
        py += c * segment;
        angle += 0.05f * std::sin(1.3f * t + phase + 0.2f * b);
    }
}

std::string distJson(const std::vector<double>& v)
{
    const Distribution d = Distribution::of(v);
    return format("{\"count\": %u, \"median\": %.5f, \"p95\": %.5f, \"p99\": %.5f, \"mean\": %.5f, \"max\": %.5f}", d.count, d.median, d.p95, d.p99, d.mean, d.max);
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        uint32_t characters = 256, bones = 64, triangles = 60000, frames = 600;
        std::string resolutionArg = "both", saveScene, scenePath, bodiesPath, benchPath;
        bool charactersGiven = false;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            auto next = [&]() -> std::string {
                if (i + 1 >= argc) fail("missing value after %s", a.c_str());
                return argv[++i];
            };
            if (a == "--scene") scenePath = next();
            else if (a == "--bodies-file") bodiesPath = next();
            else if (a == "--characters") { characters = (uint32_t)std::stoul(next()); charactersGiven = true; }
            else if (a == "--bench") benchPath = next();
            else if (a == "--bones") bones = (uint32_t)std::stoul(next());
            else if (a == "--triangles") triangles = (uint32_t)std::stoul(next());
            else if (a == "--frames") frames = (uint32_t)std::stoul(next());
            else if (a == "--resolution") resolutionArg = next();
            else if (a == "--save-scene") saveScene = next();
            else fail("unknown argument %s", a.c_str());
        }
        gate::BenchTrack bench;
        if (!benchPath.empty())
        {
            bench = gate::loadBench(benchPath);
            if (scenePath.empty()) scenePath = bench.scene.string();
            if (bodiesPath.empty()) bodiesPath = bench.bodies.string();
        }
        if (scenePath.empty()) fail("--scene <dir>/<s>.unxscene is required (unx_scenegen --scene <s> --out <dir> --bodies <dir>/<s>_bodies.json)");
        if (bodiesPath.empty())
        {
            const std::filesystem::path sp(scenePath);
            bodiesPath = (sp.parent_path() / (sp.stem().string() + "_bodies.json")).string();
        }
        gate::BodiesFile placement = gate::loadBodies(bodiesPath);
        const uint32_t bodies = (uint32_t)placement.bodies.size();
        std::vector<uint32_t> bodyInstance(bodies);
        {
            // The file belongs to this scene: every body's instance is dynamic, not skinned, and starts where the file says.
            const scene::Scene check = scene::load(scenePath);
            for (uint32_t k = 0; k < bodies; ++k)
            {
                gate::Body& b = placement.bodies[k];
                if (b.instance >= check.instances.size()) fail("%s: body %u names instance %u of %zu", bodiesPath.c_str(), k, b.instance, check.instances.size());
                const scene::Instance& in = check.instances[b.instance];
                if (!(in.flags & scene::InstanceDynamic) || (in.flags & scene::InstanceSkinned))
                    fail("%s: body %u's instance %u is not a dynamic rigid instance of %s (stale scene?)", bodiesPath.c_str(), k, b.instance, scenePath.c_str());
                const float d = std::fabs(in.transform.m[0][3] - b.position[0]) + std::fabs(in.transform.m[1][3] - b.position[1]) + std::fabs(in.transform.m[2][3] - b.position[2]);
                if (d > 1e-3f) fail("%s: body %u starts %g m away from instance %u of %s (stale scene?)", bodiesPath.c_str(), k, d, b.instance, scenePath.c_str());
                bodyInstance[k] = b.instance;
                std::memcpy(placement.bodies[k].base, in.transform.m, sizeof placement.bodies[k].base);
            }
            if (!benchPath.empty() && !charactersGiven) characters = (uint32_t)placement.characters.size();  // a bench fills its slots
            if (characters > placement.characters.size()) fail("%u characters, the file has %zu slots", characters, placement.characters.size());
        }
        // --save-scene writes the content and stops before commit: a correctness/content tool, no measurement, no lock.
        const std::string lockHolder = saveScene.empty() ? render::requireGpuLock("unx_gate_host_hostdynamic") : std::string();
        const std::filesystem::path bin = executableDirectory();
        Api api;
        api.load(bin / "UnravelNext.dll");
        std::string json = "{\n  \"gate\": \"host_dynamic\",\n";
        json += format("  \"scene\": \"%s\", \"section\": \"%s\", \"bodiesFile\": \"%s\", \"mix\": {\"resting\": %u, \"falling\": %u, \"rolling\": %u},\n", placement.scene.c_str(),
                       placement.section.c_str(), std::filesystem::path(bodiesPath).filename().string().c_str(), placement.resting, placement.falling, placement.rolling);
        json += format("  \"bodies\": %u, \"characters\": %u, \"bonesPerCharacter\": %u, \"trianglesPerCharacter\": %u, \"frames\": %u, \"gpuLock\": \"%s\",\n", bodies, characters,
                       bones, triangles, frames, lockHolder.c_str());
        json += "  \"hostUpdateMs\": \"UnxFrameSetTransforms (every body) + UnxFrameSetSkeletons (every character, one call) + UnxFrameQueue; values precomputed\",\n  \"runs\": [\n";
        const std::vector<std::string> resolutions = resolutionArg == "both" ? std::vector<std::string>{ "4K", "1440p" } : std::vector<std::string>{ resolutionArg };
        bool firstRun = true;
        for (const std::string& rs : resolutions)
        {
            const uint32_t w = rs == "4K" ? 3840 : 2560, h = rs == "4K" ? 2160 : 1440;
            if (rs != "4K" && rs != "1440p") fail("measurements run at 4K or 1440p only");
            UnxRendererDesc desc{};
            desc.size = sizeof desc;
            desc.version = 1;
            desc.flags = UNX_RENDERER_STANDALONE;
            desc.framesInFlight = 2;
            copyName(desc.shaderDirectory, (bin / "shaders").string());
            copyName(desc.qualityDirectory, (std::filesystem::path(UNX_SOURCE_DIR) / "Config/quality").string());
            UnxRenderer r = 0;
            api.ok(api.UnxRendererCreate(&desc, &r), "UnxRendererCreate");
            // The RPP-1 scene (its bodies included), then the characters: one tube material and mesh.
            UnxCameraDesc camera{};
            uint32_t sceneInstances = 0, sceneSkeletons = 0;
            api.ok(api.UnxSceneLoad(r, scenePath.c_str(), &camera, &sceneInstances, &sceneSkeletons), "UnxSceneLoad");
            auto material = [&](float r0, float g0, float b0, float rough, float metal) {
                UnxMaterialDesc m{};
                m.size = sizeof m;
                m.version = 5;
                m.clearcoatIor = 1.5f;
                m.baseColor[0] = r0; m.baseColor[1] = g0; m.baseColor[2] = b0;
                m.roughness = rough;
                m.metallic = metal;
                m.specular = 0.5f;
                m.ior = 1.5f;
                m.baseColorTexture = m.normalTexture = m.roughMetalTexture = m.emissiveTexture = m.occlusionTexture = UNX_NONE;
                uint32_t index = 0;
                api.ok(api.UnxSceneAddMaterial(r, &m, &index), "UnxSceneAddMaterial");
                return index;
            };
            const uint32_t charMat = material(0.75f, 0.12f, 0.035f, 0.8f, 0);
            const float segment = 1.8f / bones;
            uint64_t characterTriangles = 0;
            const uint32_t tubeMesh = addTube(api, r, charMat, bones, triangles, 0.25f, segment, characterTriangles);
            UnxInstanceDesc inst{};
            inst.size = sizeof inst;
            inst.version = 1;
            std::vector<uint32_t> skeletons(characters);
            std::vector<float> joints(12ull * bones);
            for (uint32_t c = 0; c < characters; ++c)
            {
                pose(joints.data(), bones, segment, 0, (float)c);
                api.ok(api.UnxSceneAddSkeleton(r, joints.data(), bones, &skeletons[c]), "skeleton");
                inst.mesh = tubeMesh;
                inst.flags = UNX_INSTANCE_CAST_SHADOW | UNX_INSTANCE_DYNAMIC | UNX_INSTANCE_SKINNED;
                inst.skeleton = skeletons[c];
                const gate::CharacterSlot& slot = placement.characters[c];
                yaw(inst.transform, slot.yaw, slot.position[0], slot.position[1], slot.position[2]);
                api.ok(api.UnxSceneAddInstance(r, &inst, nullptr), "character");
            }
            if (!saveScene.empty())
            {
                api.ok(api.UnxSceneSave(r, saveScene.c_str(), "host_dynamic", &camera), "UnxSceneSave");
                api.ok(api.UnxRendererDestroy(r), "UnxRendererDestroy");
                logf("saved %s\n", saveScene.c_str());
                return 0;
            }
            UnxSceneInfo info{};
            info.size = sizeof info;
            info.version = 1;
            api.ok(api.UnxSceneCommit(r, &info), "UnxSceneCommit");

            std::vector<UnxTransformUpdate> updates(bodies);
            uint64_t teleports = 0;
            std::vector<float> poses(12ull * bones * characters);
            std::vector<double> updateMs, gpuMs, recordMs, submitMs;
            // Time outside the passes per queue (profiler list marks, v1.39) and each measured frame's submission time
            // (unix ms: GpuLock's contention samples are matched against it).
            std::vector<double> qLists[2], qHead[2], qTail[2], qGap[2], graphLists, graphBarriers;
            std::map<uint64_t, int64_t> submittedAt;
            // Scene revision changes in the measured frames: each one starts a new GI lighting epoch (history reset).
            uint32_t lastRevision = UINT32_MAX, revisionChanges = 0;
            std::vector<uint64_t> gpuFrames;
            int64_t windowStartUnix = 0;
            std::map<std::string, std::vector<double>> passMs;
            std::vector<std::string> order;
            std::vector<UnxPassTiming> passes(512);
            uint64_t lastStats = UINT64_MAX;
            const auto start = std::chrono::steady_clock::now();
            // Frames queued before the 1.5 s warm-up ends never count, even when their timings arrive later (the first frame
            // creates pipelines and builds the atmosphere LUTs).
            uint64_t firstMeasured = UINT64_MAX;
            // Bench: one frame per tick of the track, then a few more (the last ticks' timings arrive frames later).
            const uint64_t benchTicks = bench.samples.size();
            std::vector<std::vector<double>> caseGpu(bench.cases.size()), caseClean(bench.cases.size());
            std::vector<std::pair<uint64_t, double>> cutFrames;  // (tick, GPU ms) of the first frame after every cut
            std::map<uint64_t, double> gpuByTick;
            for (uint64_t frame = 0; benchTicks ? frame < benchTicks + 4 : gpuMs.size() < frames; ++frame)
            {
                const float t = (float)frame / 60.0f;
                if (benchTicks)
                {
                    const gate::BenchSample& b = bench.samples[std::min<uint64_t>(frame, benchTicks - 1)];
                    std::memcpy(camera.position, b.position, sizeof camera.position);
                    std::memcpy(camera.forward, b.forward, sizeof camera.forward);
                    gate::benchUp(b.forward, camera.up);
                    camera.verticalFov = b.verticalFovDeg * 3.14159265f / 180.0f;
                    if (b.cut && frame < benchTicks) api.ok(api.UnxFrameSetDiscontinuity(r, UNX_DISCONTINUITY_CUT), "UnxFrameSetDiscontinuity");
                }
                // The host's values (a World's interpolated bodies, an animation system's poses) exist before the calls;
                // only the calls are timed.
                for (uint32_t b = 0; b < bodies; ++b)
                {
                    updates[b].instance = bodyInstance[b];
                    const bool restart = gate::bodyAt(placement.bodies[b], placement.gravity, t, frame == 0 ? -1.0 : (double)(frame - 1) / 60.0, updates[b].transform);
                    updates[b].flags = restart ? UNX_TRANSFORM_TELEPORT : 0u;
                    teleports += restart ? 1 : 0;
                }
                for (uint32_t c = 0; c < characters; ++c) pose(poses.data() + 12ull * bones * c, bones, segment, t, (float)c);
                const auto u0 = std::chrono::steady_clock::now();
                api.ok(api.UnxFrameSetTransforms(r, updates.data(), bodies), "UnxFrameSetTransforms");
                api.ok(api.UnxFrameSetSkeletons(r, characters, skeletons.data(), poses.data(), (uint64_t)bones * characters), "UnxFrameSetSkeletons");
                UnxFrameDesc f{};
                f.size = sizeof f;
                f.version = 1;
                f.frameIndex = frame;
                f.time = t;
                f.deltaTime = 1.0f / 60;
                f.outputWidth = w;
                f.outputHeight = h;
                f.camera = camera;
                uint64_t ticket = 0;
                api.ok(api.UnxFrameQueue(r, &f, &ticket), "UnxFrameQueue");
                const double upd = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - u0).count();
                api.ok(api.UnxFrameRenderStandalone(r, ticket, nullptr, 0), "UnxFrameRenderStandalone");
                submittedAt[frame] = gate::unixMs();
                if (firstMeasured == UINT64_MAX && std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= 1.5)
                {
                    firstMeasured = frame + 1;
                    windowStartUnix = gate::unixMs();
                }
                if (firstMeasured == UINT64_MAX || frame < firstMeasured) continue;
                updateMs.push_back(upd);
                UnxFrameStats s{};
                s.size = sizeof s;
                s.version = 1;
                api.ok(api.UnxFrameStatsLatest(r, &s), "UnxFrameStatsLatest");
                if (s.frameIndex != UINT64_MAX && s.frameIndex >= firstMeasured && s.frameIndex != lastStats)
                {
                    lastStats = s.frameIndex;
                    if (benchTicks && s.frameIndex >= benchTicks) continue;  // the drain frames after the track
                    gpuMs.push_back(s.gpuMs);
                    gpuFrames.push_back(s.frameIndex);
                    if (benchTicks) gpuByTick[s.frameIndex] = s.gpuMs;
                    UnxFrameGraphStats gs{};
                    gs.size = sizeof gs;
                    gs.version = 2;
                    api.ok(api.UnxFrameGraphStatsLatest(r, &gs), "UnxFrameGraphStatsLatest");
                    if (gs.frameIndex == s.frameIndex)
                    {
                        if (lastRevision != UINT32_MAX && gs.sceneRevision != lastRevision) ++revisionChanges;
                        lastRevision = gs.sceneRevision;
                        graphLists.push_back(gs.commandLists);
                        graphBarriers.push_back(gs.barriers);
                        for (uint32_t q = 0; q < 2; ++q)
                            if (gs.queues[q].lists)
                            {
                                qLists[q].push_back(gs.queues[q].lists);
                                qHead[q].push_back(gs.queues[q].headMs);
                                qTail[q].push_back(gs.queues[q].tailMs);
                                qGap[q].push_back(gs.queues[q].gapMs);
                            }
                    }
                    recordMs.push_back(s.cpuRecordMs);
                    submitMs.push_back(s.cpuSubmitMs);
                    uint32_t count = 0;
                    api.ok(api.UnxFramePassTimingsLatest(r, passes.data(), (uint32_t)passes.size(), &count), "UnxFramePassTimingsLatest");
                    for (uint32_t p = 0; p < std::min<uint32_t>(count, (uint32_t)passes.size()); ++p)
                    {
                        auto& list = passMs[passes[p].name];
                        if (list.empty()) order.push_back(passes[p].name);
                        list.push_back(passes[p].ms);
                    }
                }
            }
            api.ok(api.UnxRendererDestroy(r), "UnxRendererDestroy");
            const gate::Contention contention = gate::readContention(windowStartUnix, gate::unixMs());
            std::vector<double> clean;
            uint32_t contendedFrames = 0;
            for (size_t i = 0; i < gpuMs.size(); ++i)
            {
                const auto at = submittedAt.find(gpuFrames[i]);
                if (at != submittedAt.end() && contention.contended(at->second)) ++contendedFrames;
                else clean.push_back(gpuMs[i]);
            }
            std::string casesJson;
            if (benchTicks)
            {
                // Per case over its measured ticks (ticks before the warm-up end have no timing), contended frames apart.
                for (size_t i = 0; i < gpuMs.size(); ++i)
                {
                    const int c = bench.caseOf(gpuFrames[i]);
                    if (c < 0) continue;
                    caseGpu[c].push_back(gpuMs[i]);
                    const auto at = submittedAt.find(gpuFrames[i]);
                    if (!(at != submittedAt.end() && contention.contended(at->second))) caseClean[c].push_back(gpuMs[i]);
                }
                for (size_t c = 0; c < bench.cases.size(); ++c)
                    casesJson += format("%s\"%s\": {\"ticks\": [%llu, %llu], \"gpuFrameMs\": %s, \"gpuFrameMsUncontended\": %s}", casesJson.empty() ? "" : ", ",
                                        bench.cases[c].name.c_str(), (unsigned long long)bench.cases[c].begin, (unsigned long long)bench.cases[c].end,
                                        distJson(caseGpu[c]).c_str(), distJson(caseClean[c]).c_str());
                std::string cuts;
                for (uint64_t k = 0; k < benchTicks; ++k)
                    if (bench.samples[k].cut)
                    {
                        const auto g = gpuByTick.find(k);
                        cuts += format("%s{\"tick\": %llu, \"gpuFrameMs\": %s}", cuts.empty() ? "" : ", ", (unsigned long long)k,
                                       g != gpuByTick.end() ? format("%.5f", g->second).c_str() : "null");
                    }
                casesJson = format("\"bench\": {\"name\": \"%s\", \"ticks\": %llu, \"lens\": \"focal length, aperture and focus in the track are not used (no lens in the renderer yet)\", \"cases\": {%s}, \"cutFrames\": [%s]}, ",
                                   bench.name.c_str(), (unsigned long long)benchTicks, casesJson.c_str(), cuts.c_str());
            }
            std::string samples;
            for (const std::string& line : contention.samples) samples += (samples.empty() ? "" : ", ") + line;
            const std::string contentionJson = contention.sampled
                ? format("{\"contendedSeconds\": %.1f, \"contendedFrames\": %u, \"gpuFrameMsUncontended\": %s, \"samples\": [%s]}", contention.seconds(), contendedFrames,
                         distJson(clean).c_str(), samples.c_str())
                : std::string("null");
            auto queueJson = [&](uint32_t q) {
                return qLists[q].empty() ? std::string("null")
                                         : format("{\"lists\": %s, \"headMs\": %s, \"tailMs\": %s, \"gapMs\": %s}", distJson(qLists[q]).c_str(), distJson(qHead[q]).c_str(),
                                                  distJson(qTail[q]).c_str(), distJson(qGap[q]).c_str());
            };
            const std::string graphJson = format("{\"commandLists\": %s, \"barriers\": %s, \"sceneRevisionChanges\": %u, \"queues\": {\"graphics\": %s, \"compute\": %s}}",
                                                 distJson(graphLists).c_str(), distJson(graphBarriers).c_str(), revisionChanges, queueJson(0).c_str(), queueJson(1).c_str());
            const Distribution g = Distribution::of(gpuMs), u = Distribution::of(updateMs);
            logf("%s: %s, %u bodies (%llu restarts) + %u characters x %u bones, %u instances: GPU frame median %.3f ms (P95 %.3f, P99 %.3f) | host updates %.3f ms | record %.3f submit %.3f ms\n",
                 rs.c_str(), placement.scene.c_str(), bodies, (unsigned long long)teleports, characters, bones, info.instances, g.median, g.p95, g.p99, u.median, Distribution::of(recordMs).median,
                 Distribution::of(submitMs).median);
            std::string passJson;
            // Per pass over the frames it ran in (passes that run only on some frames, e.g. LUT rebuilds, have count < frames).
            for (const std::string& name : order) passJson += format("%s\n      \"%s\": %s", passJson.empty() ? "" : ",", name.c_str(), distJson(passMs[name]).c_str());
            json += format("%s    {%s\"resolution\": \"%s\", \"meshTriangles\": %llu, \"instances\": %u, \"bodyRestarts\": %llu, \"clusters\": %llu, \"sceneBuildMs\": %.1f, \"gpuFrameMs\": %s, \"hostUpdateMs\": %s, \"cpuRecordMs\": %s, \"cpuSubmitMs\": %s, \"graph\": %s, \"gpuContention\": %s, \"passMs\": {%s}}",
                           firstRun ? "" : ",\n", casesJson.c_str(), rs.c_str(), (unsigned long long)info.triangles, info.instances, (unsigned long long)teleports, (unsigned long long)info.clusters,
                           info.buildMs, distJson(gpuMs).c_str(), distJson(updateMs).c_str(), distJson(recordMs).c_str(),
                           distJson(submitMs).c_str(), graphJson.c_str(), contentionJson.c_str(), passJson.c_str());
            firstRun = false;
        }
        json += "\n  ]\n}\n";
        const std::filesystem::path outDir = std::filesystem::path(UNX_SOURCE_DIR) / "Results/I/HostDynamic";
        std::filesystem::create_directories(outDir);
        char stamp[32];
        std::time_t now = std::time(nullptr);
        std::tm tm{};
        localtime_s(&tm, &now);
        std::strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &tm);
        const std::filesystem::path file = outDir / format("host_dynamic_%s.json", stamp);
        writeTextFile(file, json);
        logf("wrote %s\n", file.string().c_str());
        return 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
