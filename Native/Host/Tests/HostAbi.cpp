// Correctness of UnravelNext.dll's renderer C ABI (I track), no GPU lock:
//  1. Round trip: every SceneGen scene plus a skinned character is pushed through the exported ABI (LoadLibrary +
//     GetProcAddress, the same entry points Unity's P/Invoke calls) and the renderer's scene::contentHash must equal the
//     hash of the source scene: every field of textures, materials, meshes, skins, skeletons, instances, lights, sun,
//     atmosphere and wind crosses the boundary unchanged.
//  1b. Live save: after commit, UnxSceneSave writes the latest transforms, bulk poses, sun and visibility the host set.
//  2. Frame: one scene is committed and rendered through the ABI (standalone renderer, readback) and through
//     FrameRenderer directly in this process; the two RGB10A2 images must match.
// --content runs part 1 only (no frames rendered); --live runs parts 1 and 1b (one frame rendered).
// Needs the C track for scenes: Tools/CI/Build.ps1 -Track I -Tracks "V;M;S;R;C;I".
#include "unx/host/UnravelNextHost.h"

#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/render/FrameRenderer.h"
#include "unx/render/GpuScene.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"

#if __has_include("unx/scenegen/SceneGen.h") && __has_include("unx/clusterbuilder/ClusterBuilder.h")
#define HOST_ABI_TEST 1
#include "unx/clusterbuilder/ClusterBuilder.h"
#include "unx/scenegen/SceneGen.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::render;

#if HOST_ABI_TEST
namespace
{
struct Api
{
    HMODULE dll = nullptr;
#define UNX_FN(name) decltype(&::name) name = nullptr;
    UNX_FN(UnxAbiVersion)
    UNX_FN(UnxLastError)
    UNX_FN(UnxRendererCreate)
    UNX_FN(UnxRendererDestroy)
    UNX_FN(UnxSceneAddTexture)
    UNX_FN(UnxSceneAddMaterial)
    UNX_FN(UnxSceneAddMesh)
    UNX_FN(UnxSceneAddSkeleton)
    UNX_FN(UnxSceneAddInstance)
    UNX_FN(UnxSceneAddLight)
    UNX_FN(UnxSceneSetEnvironment)
    UNX_FN(UnxEnvironmentDefaults)
    UNX_FN(UnxSceneCommit)
    UNX_FN(UnxSceneContentHash)
    UNX_FN(UnxFrameQueue)
    UNX_FN(UnxSceneSave)
    UNX_FN(UnxFrameSetSun)
    UNX_FN(UnxFrameSetEnvironment)
    UNX_FN(UnxFrameSetInstanceVisible)
    UNX_FN(UnxFrameSetSkeletons)
    UNX_FN(UnxFrameSetTransforms)
    UNX_FN(UnxFrameRenderStandalone)
    UNX_FN(UnxFrameStatsLatest)
    UNX_FN(UnxFrameGraphStatsLatest)
#undef UNX_FN
    void load(const std::filesystem::path& path)
    {
        dll = LoadLibraryW(path.c_str());
        if (!dll) fail("LoadLibrary %s failed (%lu)", path.string().c_str(), GetLastError());
#define UNX_FN(name)                                                                                                     \
    name = reinterpret_cast<decltype(name)>(GetProcAddress(dll, #name));                                                 \
    if (!name) fail("UnravelNext.dll does not export " #name);
        UNX_FN(UnxAbiVersion)
        UNX_FN(UnxLastError)
        UNX_FN(UnxRendererCreate)
        UNX_FN(UnxRendererDestroy)
        UNX_FN(UnxSceneAddTexture)
        UNX_FN(UnxSceneAddMaterial)
        UNX_FN(UnxSceneAddMesh)
        UNX_FN(UnxSceneAddSkeleton)
        UNX_FN(UnxSceneAddInstance)
        UNX_FN(UnxSceneAddLight)
        UNX_FN(UnxSceneSetEnvironment)
        UNX_FN(UnxEnvironmentDefaults)
        UNX_FN(UnxSceneCommit)
        UNX_FN(UnxSceneContentHash)
        UNX_FN(UnxFrameQueue)
        UNX_FN(UnxSceneSave)
        UNX_FN(UnxFrameSetSun)
        UNX_FN(UnxFrameSetEnvironment)
        UNX_FN(UnxFrameSetInstanceVisible)
        UNX_FN(UnxFrameSetSkeletons)
        UNX_FN(UnxFrameSetTransforms)
        UNX_FN(UnxFrameRenderStandalone)
        UNX_FN(UnxFrameStatsLatest)
        UNX_FN(UnxFrameGraphStatsLatest)
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
    if (s.size() >= N) fail("name '%s' longer than %zu bytes", s.c_str(), N - 1);
    std::memset(dst, 0, N);
    std::memcpy(dst, s.data(), s.size());
}

void put3(float* d, float3 v)
{
    d[0] = v.x;
    d[1] = v.y;
    d[2] = v.z;
}

void putAffine(float* d, const float3x4& m)
{
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c) d[r * 4 + c] = m.m[r][c];
}

// A two-bone bar (skinned) appended to a scene, so the skin stream, skeleton and skinned instance cross the ABI.
void addSkinnedCharacter(scene::Scene& s)
{
    scene::Material mat;
    mat.name = "abi skin";
    mat.baseColor = { 0.6f, 0.45f, 0.4f };
    mat.roughness = 0.55f;
    const uint32_t material = (uint32_t)s.materials.size();
    s.materials.push_back(mat);
    scene::Mesh m;
    m.name = "abi bar";
    const float h = 2.0f, w = 0.2f;
    for (int ring = 0; ring <= 4; ++ring)
    {
        const float y = h * ring / 4.0f;
        const float3 corners[4] = { { -w, y, -w }, { w, y, -w }, { w, y, w }, { -w, y, w } };
        const float3 normals[4] = { { -0.7071068f, 0, -0.7071068f }, { 0.7071068f, 0, -0.7071068f }, { 0.7071068f, 0, 0.7071068f }, { -0.7071068f, 0, 0.7071068f } };
        for (int c = 0; c < 4; ++c)
        {
            m.positions.push_back(corners[c]);
            m.normals.push_back(normals[c]);
            m.uv0.push_back({ c / 4.0f, ring / 4.0f });
            const float t = ring / 4.0f;
            m.skin.joints.insert(m.skin.joints.end(), { 0, 1, 0, 0 });
            m.skin.weights.insert(m.skin.weights.end(), { 1 - t, t, 0, 0 });
        }
    }
    for (uint32_t ring = 0; ring < 4; ++ring)
        for (uint32_t c = 0; c < 4; ++c)
        {
            const uint32_t a = ring * 4 + c, b = ring * 4 + (c + 1) % 4, a2 = a + 4, b2 = b + 4;
            m.indices.insert(m.indices.end(), { a, b2, b, a, a2, b2 });  // counter-clockwise from outside
        }
    m.submeshes.push_back({ 0, (uint32_t)m.indices.size(), material });
    float3x4 bind0, bind1;
    bind1.m[1][3] = -1.0f;  // joint 1 sits at y = 1 in bind pose
    m.skin.inverseBind = { bind0, bind1 };
    const uint32_t mesh = (uint32_t)s.meshes.size();
    s.meshes.push_back(std::move(m));
    scene::Skeleton sk;
    sk.name = "skeleton" + std::to_string(s.skeletons.size());
    float3x4 j0, j1;
    j1.m[1][3] = 1.0f;
    j1.m[0][3] = 0.3f;  // bent pose
    sk.jointToModel = { j0, j1 };
    const uint32_t skeleton = (uint32_t)s.skeletons.size();
    s.skeletons.push_back(sk);
    scene::Instance inst;
    inst.mesh = mesh;
    inst.flags = scene::InstanceCastShadow | scene::InstanceDynamic | scene::InstanceSkinned;
    inst.skeleton = skeleton;
    inst.transform.m[0][3] = 2.0f;
    s.instances.push_back(inst);
}

// Pushes a scene through the ABI in the order a host exporter does.
void pushScene(const Api& api, UnxRenderer r, const scene::Scene& s)
{
    for (const scene::Texture& t : s.textures)
    {
        UnxTextureDesc d{};
        d.size = sizeof d;
        d.version = 1;
        d.width = t.width;
        d.height = t.height;
        d.format = (uint32_t)t.format;
        d.wrap = t.wrap ? 1 : 0;
        d.texels = t.texels.data();
        d.byteCount = t.texels.size();
        copyName(d.name, t.name);
        api.ok(api.UnxSceneAddTexture(r, &d, nullptr), "UnxSceneAddTexture");
    }
    for (const scene::Material& m : s.materials)
    {
        UnxMaterialDesc d{};
        d.size = sizeof d;
        d.version = 1;
        d.materialClass = (uint32_t)m.cls;
        d.twoSided = m.twoSided ? 1 : 0;
        put3(d.baseColor, m.baseColor);
        d.roughness = m.roughness;
        d.metallic = m.metallic;
        d.specular = m.specular;
        d.alphaCutoff = m.alphaCutoff;
        d.transmission = m.transmission;
        put3(d.emissive, m.emissive);
        d.ior = m.ior;
        d.baseColorTexture = m.baseColorTexture;
        d.normalTexture = m.normalTexture;
        d.roughMetalTexture = m.roughMetalTexture;
        d.emissiveTexture = m.emissiveTexture;
        d.occlusionTexture = m.occlusionTexture;
        copyName(d.name, m.name);
        api.ok(api.UnxSceneAddMaterial(r, &d, nullptr), "UnxSceneAddMaterial");
    }
    for (const scene::Mesh& m : s.meshes)
    {
        std::vector<UnxSubmesh> subs;
        for (const scene::Submesh& sm : m.submeshes) subs.push_back({ sm.indexOffset, sm.indexCount, sm.material, 0 });
        UnxMeshDesc d{};
        d.size = sizeof d;
        d.version = 1;
        d.vertexCount = (uint32_t)m.positions.size();
        d.indexCount = (uint32_t)m.indices.size();
        d.submeshCount = (uint32_t)subs.size();
        d.jointCount = (uint32_t)m.skin.inverseBind.size();
        d.positions = &m.positions[0].x;
        d.normals = &m.normals[0].x;
        d.tangents = m.tangents.empty() ? nullptr : &m.tangents[0].x;
        d.uv0 = m.uv0.empty() ? nullptr : &m.uv0[0].x;
        d.indices = m.indices.data();
        d.submeshes = subs.data();
        d.joints = m.skin.joints.empty() ? nullptr : m.skin.joints.data();
        d.weights = m.skin.weights.empty() ? nullptr : m.skin.weights.data();
        std::vector<float> ib;
        for (const float3x4& b : m.skin.inverseBind)
            for (int rr = 0; rr < 3; ++rr)
                for (int c = 0; c < 4; ++c) ib.push_back(b.m[rr][c]);
        d.inverseBind = ib.empty() ? nullptr : ib.data();
        copyName(d.name, m.name);
        api.ok(api.UnxSceneAddMesh(r, &d, nullptr), "UnxSceneAddMesh");
    }
    for (const scene::Skeleton& sk : s.skeletons)
    {
        std::vector<float> j(sk.jointToModel.size() * 12);
        for (size_t i = 0; i < sk.jointToModel.size(); ++i) putAffine(j.data() + 12 * i, sk.jointToModel[i]);
        api.ok(api.UnxSceneAddSkeleton(r, j.data(), (uint32_t)sk.jointToModel.size(), nullptr), "UnxSceneAddSkeleton");
    }
    for (const scene::Instance& inst : s.instances)
    {
        UnxInstanceDesc d{};
        d.size = sizeof d;
        d.version = 1;
        d.mesh = inst.mesh;
        d.flags = inst.flags;
        d.skeleton = inst.skeleton;
        d.materialOverrideCount = (uint32_t)inst.materialOverrides.size();
        d.materialOverrides = inst.materialOverrides.empty() ? nullptr : inst.materialOverrides.data();
        putAffine(d.transform, inst.transform);
        d.windStiffness = inst.wind.stiffness;
        d.windPhase = inst.wind.phase;
        d.windAnchorHeight = inst.wind.anchorHeight;
        api.ok(api.UnxSceneAddInstance(r, &d, nullptr), "UnxSceneAddInstance");
    }
    for (const scene::Light& l : s.lights)
    {
        UnxLightDesc d{};
        d.size = sizeof d;
        d.version = 1;
        d.type = (uint32_t)l.type;
        d.castShadow = l.castShadow ? 1 : 0;
        put3(d.position, l.position);
        d.intensity = l.intensity;
        put3(d.forward, l.forward);
        d.range = l.range;
        put3(d.right, l.right);
        d.spotInner = l.spotInner;
        put3(d.color, l.color);
        d.spotOuter = l.spotOuter;
        d.areaSize[0] = l.size.x;
        d.areaSize[1] = l.size.y;
        api.ok(api.UnxSceneAddLight(r, &d, nullptr), "UnxSceneAddLight");
    }
    UnxEnvironmentDesc e{};
    api.ok(api.UnxEnvironmentDefaults(&e), "UnxEnvironmentDefaults");
    put3(e.sunDirection, s.sun.direction);
    e.sunIlluminance = s.sun.illuminance;
    put3(e.sunColor, s.sun.color);
    e.sunAngularRadius = s.sun.angularRadius;
    put3(e.windDirection, s.windDirection);
    e.windSpeed = s.windSpeed;
    const scene::Atmosphere& a = s.atmosphere;
    e.bottomRadius = a.bottomRadius;
    e.topRadius = a.topRadius;
    e.rayleighScaleHeight = a.rayleighScaleHeight;
    e.mieScaleHeight = a.mieScaleHeight;
    put3(e.rayleighScattering, a.rayleighScattering);
    e.mieG = a.mieG;
    put3(e.mieScattering, a.mieScattering);
    e.ozoneCenter = a.ozoneCenter;
    put3(e.mieAbsorption, a.mieAbsorption);
    e.ozoneWidth = a.ozoneWidth;
    put3(e.ozoneAbsorption, a.ozoneAbsorption);
    put3(e.groundAlbedo, a.groundAlbedo);
    api.ok(api.UnxSceneSetEnvironment(r, &e), "UnxSceneSetEnvironment");
}

UnxRendererDesc rendererDesc(const std::filesystem::path& shaders, const std::filesystem::path& quality)
{
    UnxRendererDesc d{};
    d.size = sizeof d;
    d.version = 1;
    d.flags = UNX_RENDERER_STANDALONE;
    d.framesInFlight = 1;  // parity check: both paths pace one frame at a time (GI feedback latency follows it)
    copyName(d.shaderDirectory, shaders.string());
    copyName(d.qualityDirectory, quality.string());
    return d;
}

// Direct path: the same scene and camera through FrameRenderer in this process.
std::vector<uint32_t> renderDirect(const scene::Scene& s, const QualityConfig& quality, const std::filesystem::path& shaderDir, uint32_t w, uint32_t h,
                                   uint32_t frames)
{
    Device device({});
    ShaderLibrary shaders(device, shaderDir);
    GpuScene gpuScene(device);
    gpuScene.upload(s);
    gpuScene.setClusters(clusterbuilder::build(s, clusterbuilder::Settings::fromQuality(quality)));
    FrameRenderer renderer(device, shaders, quality, gpuScene, 1);
    RenderGraph graph(device);
    D3D12_HEAP_PROPERTIES rb{ D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w;
    td.Height = h;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    td.SampleDesc.Count = 1;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 total = 0;
    device.d3d()->GetCopyableFootprints(&td, 0, 1, 0, &fp, nullptr, nullptr, &total);
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    check(device.d3d()->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback)), "readback");
    float4x4 prev = ViewDesc::fromCamera(s.cameras.at(0), w, h, {}).viewProj;
    for (uint32_t frame = 0; frame < frames; ++frame)
    {
        FrameContext fc;
        fc.frameIndex = frame;
        fc.time = frame / 60.0;
        fc.deltaTime = 1.0f / 60;
        fc.mainView = ViewDesc::fromCamera(s.cameras.at(0), w, h, prev);
        prev = fc.mainView.viewProj;
        const TextureRef output = graph.createTexture({ "direct output", w, h, 1, 1, DXGI_FORMAT_R10G10B10A2_UNORM });
        renderer.record(graph, fc, output);
        if (frame + 1 == frames)
        {
            const BufferRef rbRef = graph.importBuffer(readback.Get(), { "direct readback", total, 0 });
            graph.addPass(
                "test.readback", QueueType::Graphics,
                [&](PassBuilder& b) {
                    b.use(output, Use::CopySrc);
                    b.use(rbRef, Use::CopyDst);
                    b.keep();
                },
                [&](PassContext& c) {
                    D3D12_TEXTURE_COPY_LOCATION dst{ readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                    dst.PlacedFootprint = fp;
                    D3D12_TEXTURE_COPY_LOCATION src{ c.resource(output), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                    src.SubresourceIndex = 0;
                    c.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                });
        }
        graph.execute(nullptr);
        device.waitIdle();  // simple pacing: one frame in flight (FrameRenderer allows up to its framesInFlight)
    }
    std::vector<uint32_t> pixels((size_t)w * h);
    const uint8_t* mapped = nullptr;
    check(readback->Map(0, nullptr, (void**)&mapped), "Map");
    for (uint32_t y = 0; y < h; ++y) std::memcpy(pixels.data() + (size_t)y * w, mapped + fp.Offset + (size_t)y * fp.Footprint.RowPitch, (size_t)w * 4);
    readback->Unmap(0, nullptr);
    return pixels;
}
} // namespace
#endif

int main(int argc, char** argv)
{
    try
    {
#if !HOST_ABI_TEST
        fail("this build lacks C's scene generator or V's cluster builder: Tools/CI/Build.ps1 -Track I -Tracks \"V;M;S;R;C;I\"");
#else
        const std::filesystem::path bin = executableDirectory();
        const std::filesystem::path quality = std::filesystem::path(UNX_SOURCE_DIR) / "Config/quality";
        Api api;
        api.load(bin / "UnravelNext.dll");
        if (api.UnxAbiVersion() != UNX_ABI_VERSION) fail("DLL ABI %u, header %u", api.UnxAbiVersion(), UNX_ABI_VERSION);
        const UnxRendererDesc desc = rendererDesc(bin / "shaders", quality);
        uint32_t failures = 0;
        const bool contentOnly = argc > 1 && std::string(argv[1]) == "--content";
        const bool liveOnly = argc > 1 && std::string(argv[1]) == "--live";

        // 1. Round trip of every scene (+ a skinned character).
        for (scenegen::SceneId id : scenegen::allScenes())
        {
            scenegen::Request request;
            request.id = id;
            scene::Scene s = scenegen::generate(request);
            addSkinnedCharacter(s);
            scene::validate(s);
            UnxRenderer r = 0;
            api.ok(api.UnxRendererCreate(&desc, &r), "UnxRendererCreate");
            pushScene(api, r, s);
            char hash[65] = {};
            api.ok(api.UnxSceneContentHash(r, hash), "UnxSceneContentHash");
            // The ABI carries render content only: the scene's name, seed, cameras and camera paths stay on the host
            // (cameras arrive per frame).
            scene::Scene content = s;
            content.name.clear();
            content.seed = 0;
            content.cameras.clear();
            content.paths.clear();
            const std::string expected = scene::contentHash(content);
            const bool same = expected == hash;
            logf("round trip %-12s %zu meshes %zu instances %zu textures %zu lights: %s\n", scenegen::sceneName(id), s.meshes.size(), s.instances.size(),
                 s.textures.size(), s.lights.size(), same ? "hash equal" : "HASH DIFFERS");
            if (!same) ++failures;
            api.ok(api.UnxRendererDestroy(r), "UnxRendererDestroy");
        }
        if (contentOnly)
        {
            logf(failures ? "HOST ABI CONTENT TEST FAILED (%u)\n" : "HOST ABI CONTENT TEST PASSED\n", failures);
            return failures ? 1 : 0;
        }

        // 1b. After commit, UnxSceneSave writes the scene as the host shows it: rendered, queued and pending updates
        // (newest last), poses set in one call (UnxFrameSetSkeletons), hidden instances left out.
        {
            scenegen::Request request;
            request.id = scenegen::allScenes().front();
            scene::Scene s = scenegen::generate(request);
            addSkinnedCharacter(s);
            addSkinnedCharacter(s);
            scene::validate(s);
            UnxRenderer r = 0;
            api.ok(api.UnxRendererCreate(&desc, &r), "UnxRendererCreate");
            pushScene(api, r, s);
            UnxSceneInfo info{};
            info.size = sizeof info;
            info.version = 1;
            api.ok(api.UnxSceneCommit(r, &info), "UnxSceneCommit");
            const uint32_t k0 = (uint32_t)s.skeletons.size() - 2, k1 = k0 + 1;
            auto moved = [](float x) {
                UnxTransformUpdate u{};
                u.instance = 0;
                const float m[12] = { 0, 0, 1, x, 0, 1, 0, 0.5f, -1, 0, 0, -3 };  // yaw 90 degrees
                std::memcpy(u.transform, m, sizeof m);
                return u;
            };
            auto frame = [&](uint64_t index) {
                UnxFrameDesc f{};
                f.size = sizeof f;
                f.version = 1;
                f.frameIndex = index;
                f.deltaTime = 1.0f / 60;
                f.outputWidth = 2560;
                f.outputHeight = 1440;
                put3(f.camera.position, { 0, 2, 8 });
                put3(f.camera.forward, { 0, 0, -1 });
                put3(f.camera.up, { 0, 1, 0 });
                f.camera.verticalFov = 1.0471976f;
                f.camera.nearPlane = 0.05f;
                f.camera.ev100 = 14;
                uint64_t ticket = 0;
                api.ok(api.UnxFrameQueue(r, &f, &ticket), "UnxFrameQueue");
                return ticket;
            };
            // Frame 0, rendered: instance 0 moves, instance 1 hides, both characters get pose A.
            UnxTransformUpdate a = moved(1.5f);
            api.ok(api.UnxFrameSetTransforms(r, &a, 1), "UnxFrameSetTransforms");
            api.ok(api.UnxFrameSetInstanceVisible(r, 1, 0), "UnxFrameSetInstanceVisible");
            const uint32_t skeletons[2] = { k0, k1 };
            float poseA[48] = {}, poseB[48] = {};
            for (int j = 0; j < 4; ++j)
            {
                poseA[12 * j + 0] = poseA[12 * j + 5] = poseA[12 * j + 10] = 1;
                poseA[12 * j + 7] = (float)(j % 2);
                poseA[12 * j + 3] = 0.1f * j;
                std::memcpy(poseB + 12 * j, poseA + 12 * j, 48);
                poseB[12 * j + 3] = -0.25f * j - 0.5f;
            }
            api.ok(api.UnxFrameSetSkeletons(r, 2, skeletons, poseA, 4), "UnxFrameSetSkeletons");
            api.ok(api.UnxFrameRenderStandalone(r, frame(0), nullptr, 0), "UnxFrameRenderStandalone");
            // Frame 1, queued, not rendered: instance 0 moves again.
            UnxTransformUpdate b = moved(-2.25f);
            api.ok(api.UnxFrameSetTransforms(r, &b, 1), "UnxFrameSetTransforms");
            frame(1);
            // Pending: pose B and a new sun.
            api.ok(api.UnxFrameSetSkeletons(r, 2, skeletons, poseB, 4), "UnxFrameSetSkeletons");
            const float sunDir[3] = { 0, 0.8f, 0.6f }, sunColor[3] = { 1, 0.9f, 0.8f };
            api.ok(api.UnxFrameSetSun(r, sunDir, 90000, sunColor, 0.005f), "UnxFrameSetSun");
            // Then weather: a later sun and a hazier atmosphere through UnxFrameSetEnvironment (the committed wind).
            UnxEnvironmentDesc env{};
            api.ok(api.UnxEnvironmentDefaults(&env), "UnxEnvironmentDefaults");
            put3(env.sunDirection, { 0, 0.6f, 0.8f });
            env.sunIlluminance = 95000;
            put3(env.mieScattering, { 2.1e-5f, 2.1e-5f, 2.1e-5f });
            put3(env.windDirection, s.windDirection);
            env.windSpeed = s.windSpeed;
            api.ok(api.UnxFrameSetEnvironment(r, &env), "UnxFrameSetEnvironment");
            // Then the wind turns and strengthens (INTERFACES 6.4 v1.23); a malformed wind is refused.
            UnxEnvironmentDesc windy = env;
            put3(windy.windDirection, { 0.6f, 0, -0.8f });
            windy.windSpeed = s.windSpeed + 3;
            api.ok(api.UnxFrameSetEnvironment(r, &windy), "UnxFrameSetEnvironment (wind)");
            UnxEnvironmentDesc bad = windy;
            bad.windSpeed = -1;
            if (api.UnxFrameSetEnvironment(r, &bad) == UNX_OK) fail("UnxFrameSetEnvironment accepted a negative wind speed");
            // A pose buffer of the wrong length is refused before anything is recorded.
            if (api.UnxFrameSetSkeletons(r, 2, skeletons, poseA, 3) == UNX_OK) fail("UnxFrameSetSkeletons accepted a short pose buffer");
            const std::filesystem::path saved = bin / "host_abi_live.unxscene";
            api.ok(api.UnxSceneSave(r, saved.string().c_str(), "live", nullptr), "UnxSceneSave");
            api.ok(api.UnxRendererDestroy(r), "UnxRendererDestroy");
            const scene::Scene live = scene::load(saved);
            bool ok = live.instances.size() + 1 == s.instances.size();
            for (int i = 0; i < 12 && ok; ++i) ok = live.instances[0].transform.m[i / 4][i % 4] == b.transform[i];
            for (int k = 0; k < 2 && ok; ++k)
                for (int j = 0; j < 2 && ok; ++j)
                    for (int e = 0; e < 12 && ok; ++e) ok = live.skeletons[skeletons[k]].jointToModel[j].m[e / 4][e % 4] == poseB[12 * (2 * k + j) + e];
            ok = ok && live.sun.illuminance == 95000 && live.sun.direction.y == 0.6f && live.sun.direction.z == 0.8f;
            ok = ok && live.atmosphere.mieScattering.x == 2.1e-5f && live.atmosphere.rayleighScaleHeight == env.rayleighScaleHeight;
            ok = ok && live.windSpeed == windy.windSpeed && live.windDirection.x == 0.6f && live.windDirection.z == -0.8f;
            // Instance 1 was hidden: the saved list is the host's list without it (instance 2 comes next).
            ok = ok && live.instances[1].mesh == s.instances[2].mesh;
            logf("live scene save: %zu of %zu instances (1 hidden), newest transform, bulk poses, sun, atmosphere and wind: %s\n", live.instances.size(), s.instances.size(),
                 ok ? "as set" : "DIFFERS");
            if (!ok) ++failures;
        }
        if (liveOnly)
        {
            logf(failures ? "HOST ABI LIVE TEST FAILED (%u)\n" : "HOST ABI LIVE TEST PASSED\n", failures);
            return failures ? 1 : 0;
        }

        // 2. Frames through the ABI vs FrameRenderer directly (1440p). This checks the host's frame setup (camera, time,
        // output, pacing), not image quality (the renderer's tracks verify that against the reference). Both paths use
        // a test copy of the quality files that leaves out the two stochastic shading terms (GI probe irradiance and
        // reflection radiance, shading.experiment_disable = 2 | 4): the GI cache is not deterministic run to run, and
        // two direct runs alone differ by P99 ~300/1023 at frame 64. Everything else is rendered and compared.
        const std::filesystem::path testQuality = bin / "host_abi_quality";
        std::filesystem::remove_all(testQuality);
        std::filesystem::create_directories(testQuality);
        for (const auto& entry : std::filesystem::directory_iterator(quality))
        {
            if (entry.path().extension() != ".toml") continue;
            std::string text = readTextFile(entry.path());
            if (entry.path().filename() == "shading.toml")
            {
                const std::string from = "experiment_disable = 0", to = "experiment_disable = 6";
                const size_t at = text.find(from);
                if (at == std::string::npos) fail("shading.toml has no '%s' line to override for the parity check", from.c_str());
                text.replace(at, from.size(), to);
            }
            writeTextFile(testQuality / entry.path().filename(), text);
        }
        const UnxRendererDesc parityDesc = rendererDesc(bin / "shaders", testQuality);
        const uint32_t w = 2560, h = 1440;
        scenegen::Request request;
        request.id = scenegen::SceneId::Interior;
        scene::Scene s = scenegen::generate(request);
        addSkinnedCharacter(s);
        UnxRenderer r = 0;
        api.ok(api.UnxRendererCreate(&parityDesc, &r), "UnxRendererCreate");
        pushScene(api, r, s);
        UnxSceneInfo info{};
        info.size = sizeof info;
        info.version = 1;
        api.ok(api.UnxSceneCommit(r, &info), "UnxSceneCommit");
        logf("committed %s: %u meshes, %u instances, %llu triangles, %llu clusters, %.1f ms\n", scenegen::sceneName(request.id), info.meshes, info.instances,
             (unsigned long long)info.triangles, (unsigned long long)info.clusters, info.buildMs);
        const scene::Camera& cam = s.cameras.at(0);
        UnxFrameDesc f{};
        f.size = sizeof f;
        f.version = 1;
        f.frameIndex = 0;
        f.time = 0;
        f.deltaTime = 1.0f / 60;
        f.outputWidth = w;
        f.outputHeight = h;
        put3(f.camera.position, cam.position);
        put3(f.camera.forward, cam.forward);
        put3(f.camera.up, cam.up);
        f.camera.verticalFov = cam.verticalFov;
        f.camera.nearPlane = cam.nearPlane;
        f.camera.ev100 = cam.ev100;
        // Static camera, kFrames frames: the GI cache converges (its first frames update a random subset of entries,
        // which differs between two runs), so the last frames of both paths are comparable pixel by pixel.
        constexpr uint32_t kFrames = 64;
        std::vector<uint32_t> viaAbi((size_t)w * h);
        for (uint32_t frame = 0; frame < kFrames; ++frame)
        {
            f.frameIndex = frame;
            f.time = frame / 60.0;
            uint64_t ticket = 0;
            api.ok(api.UnxFrameQueue(r, &f, &ticket), "UnxFrameQueue");
            const bool last = frame + 1 == kFrames;
            api.ok(api.UnxFrameRenderStandalone(r, ticket, last ? viaAbi.data() : nullptr, last ? viaAbi.size() * 4 : 0), "UnxFrameRenderStandalone");
        }
        UnxFrameStats stats{};
        stats.size = sizeof stats;
        stats.version = 1;
        api.ok(api.UnxFrameStatsLatest(r, &stats), "UnxFrameStatsLatest");
        logf("ABI frames: %u, latest completed frame %llu: GPU %.3f ms, %u passes, CPU record %.3f ms, submit %.3f ms (1440p, correctness run, not a measurement)\n",
             kFrames, (unsigned long long)stats.frameIndex, stats.gpuMs, stats.passes, stats.cpuRecordMs, stats.cpuSubmitMs);
        UnxFrameGraphStats graph{};
        graph.size = sizeof graph;
        graph.version = 2;
        api.ok(api.UnxFrameGraphStatsLatest(r, &graph), "UnxFrameGraphStatsLatest");
        logf("ABI graph of frame %llu: %u live passes, %u command lists, %u barriers in %u batches, %u cross-queue syncs, %u transients (%.1f MB aliased), plan %s\n",
             (unsigned long long)graph.frameIndex, graph.livePasses, graph.commandLists, graph.barriers, graph.barrierBatches, graph.crossQueueSyncs,
             graph.transientResources, graph.transientBytesAliased / 1048576.0, graph.planReused ? "reused" : "compiled");
        // The graph numbers belong to the frame the timings report, and that frame recorded passes and lists.
        if (graph.frameIndex != stats.frameIndex) fail("UnxFrameGraphStatsLatest: frame %llu, stats frame %llu", (unsigned long long)graph.frameIndex, (unsigned long long)stats.frameIndex);
        if (graph.livePasses == 0 || graph.commandLists == 0) fail("UnxFrameGraphStatsLatest: %u live passes, %u command lists", graph.livePasses, graph.commandLists);
        logf("ABI queue timing of frame %llu: graphics %u lists, head %.4f tail %.4f gap %.4f ms; compute %u lists (correctness run, not a measurement)\n",
             (unsigned long long)graph.frameIndex, graph.queues[0].lists, graph.queues[0].headMs, graph.queues[0].tailMs, graph.queues[0].gapMs, graph.queues[1].lists);
        if (graph.queues[0].lists == 0) fail("UnxFrameGraphStatsLatest: no graphics list in the queue timing");
        // Version 1 (the first 64 bytes) is still served.
        UnxFrameGraphStats v1{};
        v1.size = (uint32_t)offsetof(UnxFrameGraphStats, queues);
        v1.version = 1;
        api.ok(api.UnxFrameGraphStatsLatest(r, &v1), "UnxFrameGraphStatsLatest v1");
        if (v1.commandLists != graph.commandLists || v1.queues[0].lists != 0) fail("UnxFrameGraphStatsLatest v1 wrote past its 64 bytes or lost fields");
        api.ok(api.UnxRendererDestroy(r), "UnxRendererDestroy");

        const QualityConfig q = QualityConfig::loadDirectory(testQuality);
        const std::vector<uint32_t> direct = renderDirect(s, q, bin / "shaders", w, h, kFrames);
        const std::vector<uint32_t> direct2 = renderDirect(s, q, bin / "shaders", w, h, kFrames);
        // Raw RGB10A2 dumps for inspection.
        auto dump = [&](const char* name, const std::vector<uint32_t>& px) {
            FILE* file = nullptr;
            if (fopen_s(&file, (bin / name).string().c_str(), "wb") == 0 && file)
            {
                std::fwrite(px.data(), 4, px.size(), file);
                std::fclose(file);
            }
        };
        dump("host_abi_frame_abi.rgb10a2", viaAbi);
        dump("host_abi_frame_direct.rgb10a2", direct);
        // Per-pixel largest channel difference (10-bit codes). The GI cache is not deterministic run to run (which
        // entries a frame updates depends on GPU atomics) and is far from converged after 64 frames (7.8k of 200k
        // entries updated per frame), so two runs of the same path already differ. The ABI path is correct when it differs
        // from the direct path no more than the direct path differs from itself; a wrong camera, output or frame setup
        // moves edges and shading everywhere and exceeds that floor.
        struct Stats
        {
            double mean = 0, identical = 0;
            uint32_t p99 = 0, p999 = 0, max = 0;
        };
        auto compare = [](const std::vector<uint32_t>& a, const std::vector<uint32_t>& b) {
            std::vector<uint16_t> diff(a.size());
            uint64_t sum = 0, same = 0;
            for (size_t i = 0; i < a.size(); ++i)
            {
                uint32_t d = 0;
                for (int c = 0; c < 3; ++c)
                {
                    const int x = (a[i] >> (10 * c)) & 1023, y = (b[i] >> (10 * c)) & 1023;
                    d = std::max<uint32_t>(d, (uint32_t)std::abs(x - y));
                }
                diff[i] = (uint16_t)d;
                sum += d;
                same += d == 0;
            }
            std::sort(diff.begin(), diff.end());
            Stats st;
            st.mean = (double)sum / diff.size();
            st.identical = (double)same / diff.size();
            st.p99 = diff[(size_t)(0.99 * (diff.size() - 1))];
            st.p999 = diff[(size_t)(0.999 * (diff.size() - 1))];
            st.max = diff.back();
            return st;
        };
        std::vector<uint32_t> distinct;
        for (size_t i = 0; i < viaAbi.size() && distinct.size() < 64; ++i)
            if (std::find(distinct.begin(), distinct.end(), viaAbi[i]) == distinct.end()) distinct.push_back(viaAbi[i]);
        const Stats abi = compare(viaAbi, direct), floor = compare(direct2, direct);
        logf("frame %u (%ux%u), channel difference in codes of 1023:\n", kFrames, w, h);
        logf("  ABI vs direct:    mean %.3f  P99 %u  P99.9 %u  max %u  identical %.2f %%\n", abi.mean, abi.p99, abi.p999, abi.max, 100 * abi.identical);
        logf("  direct vs direct: mean %.3f  P99 %u  P99.9 %u  max %u  identical %.2f %%  (run-to-run floor)\n", floor.mean, floor.p99, floor.p999, floor.max,
             100 * floor.identical);
        if (distinct.size() < 16)
        {
            logf("FAIL: the ABI frame is (nearly) uniform\n");
            ++failures;
        }
        // With the stochastic terms out, two direct runs are bit-identical [measured]; then the ABI frame must be too.
        if (floor.max == 0 ? abi.max != 0 : (abi.mean > 1.5 * floor.mean + 1.0 || abi.p99 > 1.5 * floor.p99 + 4))
        {
            logf("FAIL: the ABI frame differs from the direct frame (floor exact: any difference; otherwise x1.5 + 1 mean, x1.5 + 4 P99)\n");
            ++failures;
        }
        logf(failures ? "HOST ABI TEST FAILED (%u)\n" : "HOST ABI TEST PASSED\n", failures);
        return failures ? 1 : 0;
#endif
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
