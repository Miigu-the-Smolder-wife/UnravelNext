#include "Renderer/HostRenderer.h"

#include "GpuBridge/GpuBridge.h"

#include "unx/render/Device.h"
#include "unx/render/FrameContext.h"
#include "unx/render/FrameRenderer.h"
#include "unx/render/GpuProfiler.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"
#include "unx/metrics/Metrics.h"
#include "unx/reference/GpuPathTracer.h"

#include <wincodec.h>

#if __has_include("unx/clusterbuilder/ClusterBuilder.h")
#include "unx/clusterbuilder/ClusterBuilder.h"
#define UNX_HOST_HAS_CLUSTERBUILDER 1
#endif

#if __has_include("unx/cook/TextureCook.h")
#include "unx/cook/TextureCook.h"
#include "unx/material/TextureSystem.h"
#define UNX_HOST_HAS_COOK 1
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include "unx/scene/TerrainPatch.h"
#include <cstring>
#include <fstream>

namespace unx::host
{
using namespace unx::render;

// B11 photo mode, submission thread.
struct HostRenderer::Photo
{
    uint64_t generation = 0;                        // the request the tracer belongs to
    std::unique_ptr<reference::GpuPathTracer> tracer;
    uint32_t width = 0, height = 0, target = 0;
    bool failed = false;                            // this generation's start failed (not retried)
    bool shown = false;                             // the last frame showed the photo
    // A save's SDR encoding (RGB10A2) and its read-back; the PNGs written once the frame's lists were submitted.
    ComPtr<ID3D12Resource> sdr, readback;
    uint32_t sdrWidth = 0, sdrHeight = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    uint64_t readbackBytes = 0;
    std::vector<std::filesystem::path> pngs;
};

namespace
{
// 16-bit RGB PNG through WIC (Windows' own codecs; photo mode's display-encoded image).
void writePng16(const std::filesystem::path& path, uint32_t width, uint32_t height, const std::vector<uint16_t>& rgb)
{
    const HRESULT init = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct Uninit
    {
        bool on;
        ~Uninit()
        {
            if (on) CoUninitialize();
        }
    } uninit{ SUCCEEDED(init) };
    ComPtr<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "WIC imaging factory");
    ComPtr<IWICStream> stream;
    check(factory->CreateStream(&stream), "WIC stream");
    check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "photo PNG file");
    ComPtr<IWICBitmapEncoder> encoder;
    check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "WIC PNG encoder");
    check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "WIC encoder");
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> properties;
    check(encoder->CreateNewFrame(&frame, &properties), "WIC frame");
    check(frame->Initialize(properties.Get()), "WIC frame");
    check(frame->SetSize(width, height), "WIC frame size");
    WICPixelFormatGUID format = GUID_WICPixelFormat48bppRGB;
    check(frame->SetPixelFormat(&format), "WIC pixel format");
    if (format != GUID_WICPixelFormat48bppRGB) fail("the PNG encoder does not take 16-bit RGB");
    check(frame->WritePixels(height, width * 6, (UINT)(rgb.size() * 2), (BYTE*)rgb.data()), "WIC pixels");
    check(frame->Commit(), "WIC frame commit");
    check(encoder->Commit(), "WIC encoder commit");
}
} // namespace

struct HostRenderer::Standalone
{
    ComPtr<ID3D12Resource> output, readback;
    uint32_t width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    uint64_t readbackBytes = 0;
};

HostRenderer::HostRenderer(const HostRendererOptions& options) : m_options(options)
{
#if UNX_HOST_HAS_COOK
    // C1: textures are uploaded in their cooked form (block-compressed within the error bound, memory and disk caches under
    // UNX_COOK_CACHE, which the Unity editor sets to Library/UnravelNextCook).
    material::setChainProvider(&cook::textureChain);
#endif
    if (options.framesInFlight == 0) fail("framesInFlight must be at least 1");
    if (!options.standalone && (!options.hostDevice || !options.hostQueue)) fail("a host renderer needs the host's device and graphics queue");
    m_quality = QualityConfig::loadDirectory(options.qualityDirectory);
    for (const std::string& o : options.qualityOverrides) m_quality.applyOverride(o);
    DeviceOptions d;
    if (options.standalone)
    {
        d.debugLayer = options.debugLayer;
        d.gpuValidation = options.gpuValidation;
        if (options.standaloneDevice)
        {
            if (options.debugLayer || options.gpuValidation) fail("a standalone renderer on a given device has no debug layer");
            d.externalDevice = options.standaloneDevice;
        }
    }
    else
    {
        if (options.debugLayer || options.gpuValidation) fail("the debug layer is for standalone renderers (enabling it removes the host's device)");
        d.externalDevice = options.hostDevice;
        d.externalGraphicsQueue = options.hostQueue;
    }
    m_device = std::make_unique<Device>(d);
    {
        // engine 1's GPU bridge on this device: a device of its own is a generation of its own (fences never compare across)
        static std::atomic<uint64_t> generations{ 0 };
        Queue& graphics = m_device->queue(QueueType::Graphics);
        m_gpuBridge = std::make_shared<GpuBridgeHost>(m_device->d3d(), graphics.get(), graphics.fence(), ++generations);
    }
    m_shaders = std::make_unique<ShaderLibrary>(*m_device, options.shaderDirectory);
    m_slotFence.assign(options.framesInFlight, std::array<uint64_t, 3>{});
    m_slotHostFrame.assign(options.framesInFlight, UINT64_MAX);
    m_slotGraph.assign(options.framesInFlight, {});
    m_standalone = std::make_unique<Standalone>();
}

HostRenderer::~HostRenderer()
{
    if (m_device) m_device->waitIdle();
    if (m_gpuBridge) m_gpuBridge->quiesce();  // the modules' work on the bridge's queues ends before the device
    m_gpuBridge.reset();
    m_photo.reset();  // (B11) the tracer returns its descriptors while the device lives
    m_profiler.reset();
    m_graph.reset();
    m_frameRenderer.reset();
    m_gpuScene.reset();
    m_standalone.reset();
}

void HostRenderer::requireOpen() const
{
    if (m_committed) fail("the scene is committed; content can no longer be added");
}

void HostRenderer::requireCommitted() const
{
    if (!m_committed) fail("commit the scene first");
}

SceneCommitInfo HostRenderer::commit()
{
    requireOpen();
    const auto t0 = std::chrono::steady_clock::now();
    scene::validate(m_scene);
    SceneCommitInfo info;
    for (const scene::Mesh& m : m_scene.meshes) info.triangles += m.indices.size() / 3;
#if UNX_HOST_HAS_CLUSTERBUILDER
    // (the builder's own vertices - enlarged pieces of thin geometry, visibility.lod_thin_preserve_area - go into the
    // scene's meshes before the upload; a scene is committed once)
    clusterbuilder::LodVertices lodVertices;
    ClusterData clusters = clusterbuilder::build(m_scene, clusterbuilder::Settings::fromQuality(m_quality), nullptr, &lodVertices);
    lodVertices.appendTo(m_scene);
    info.clusters = clusters.clusters.size();
#else
    fail("this build has no cluster builder (track V): build with Tools/CI/Build.ps1 -Track I");
#endif
    m_gpuScene = std::make_unique<GpuScene>(*m_device);
    // A3 mesh particles (render C): GPU-written instances after the CPU-known ones (fx.particles.mesh_instances_max)
    m_runtimeCapacity.gpuInstances = (uint32_t)std::max<int64_t>(0, m_quality.integer("fx.particles.mesh_instances_max"));
    if (m_runtimeCapacity.meshes || m_runtimeCapacity.instances || m_runtimeCapacity.gpuInstances) m_gpuScene->reserveRuntime(m_runtimeCapacity);  // C2b
    m_gpuScene->upload(m_scene);
    m_gpuScene->setClusters(std::move(clusters));
    m_frameRenderer = std::make_unique<FrameRenderer>(*m_device, *m_shaders, m_quality, *m_gpuScene, m_options.framesInFlight);
    m_graph = std::make_unique<RenderGraph>(*m_device);
    m_profiler = std::make_unique<GpuProfiler>(*m_device, m_options.framesInFlight, 1024);
    m_committed = true;
    for (const scene::Instance& i : m_scene.instances) m_applied.transforms.push_back(i.transform);
    for (const scene::Skeleton& k : m_scene.skeletons) m_applied.poses.push_back(std::make_shared<const std::vector<float3x4>>(k.jointToModel));
    m_applied.visible.assign(m_scene.instances.size(), 1);
    m_applied.sun = m_scene.sun;
    m_applied.atmosphere = m_scene.atmosphere;
    m_applied.wind = { m_scene.windDirection, m_scene.windSpeed };
    m_gpuTransforms = m_applied.transforms;
    m_gpuPoses = m_applied.poses;
    m_hostInstances = (uint32_t)m_scene.instances.size();
    m_hostMaterials = (uint32_t)m_scene.materials.size();
    for (const scene::Material& m : m_scene.materials) m_hostMaterialClasses.push_back(m.cls);
    for (const scene::Instance& i : m_scene.instances) m_hostSkinned.push_back((i.flags & scene::InstanceSkinned) ? 1 : 0);
    info.contentHash = scene::contentHash(m_scene);
    info.buildMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    logf("UnravelNext host: scene committed, %zu meshes, %zu instances, %llu triangles, %llu clusters, %.1f ms, hash %s\n", m_scene.meshes.size(),
         m_scene.instances.size(), (unsigned long long)info.triangles, (unsigned long long)info.clusters, info.buildMs, info.contentHash.substr(0, 16).c_str());
    return info;
}

void HostRenderer::setTransforms(std::span<const InstanceTransformUpdate> updates)
{
    requireCommitted();
    std::lock_guard lock(m_mutex);
    for (const InstanceTransformUpdate& u : updates)
        if (u.instance >= m_hostInstances) fail("transform update for instance %u of %u", u.instance, m_hostInstances);
    m_pending.transforms.insert(m_pending.transforms.end(), updates.begin(), updates.end());
}

uint32_t HostRenderer::instanceCount() const
{
    std::lock_guard lock(m_mutex);
    return m_hostInstances;
}

uint32_t HostRenderer::materialCount() const
{
    std::lock_guard lock(m_mutex);
    return m_hostMaterials;
}

void HostRenderer::editInstances(std::span<const std::pair<uint32_t, scene::Instance>> edits)
{
    requireCommitted();
    std::lock_guard lock(m_mutex);
    // Meshes and textures are fixed after commit, so they can be read here; instances and materials change on the
    // submission thread, so the host's own counts decide.
    uint32_t count = m_hostInstances;
    for (const auto& [i, inst] : edits)
    {
        if (i > count) fail("instance edit %u of %u (append in order)", i, count);
        if (inst.mesh >= m_scene.meshes.size()) fail("instance edit %u: mesh %u of %zu (a new mesh needs a new renderer)", i, inst.mesh, m_scene.meshes.size());
        if (inst.flags & scene::InstanceSkinned) fail("instance edit %u: a skinned instance needs a new renderer (its palette slot is fixed at commit)", i);
        if (i < count && i < m_hostSkinned.size() && m_hostSkinned[i]) fail("instance edit %u replaces a skinned instance", i);
        const size_t submeshes = m_scene.meshes[inst.mesh].submeshes.size();
        if (!inst.materialOverrides.empty() && inst.materialOverrides.size() != submeshes)
            fail("instance edit %u: %zu material overrides for %zu submeshes", i, inst.materialOverrides.size(), submeshes);
        for (uint32_t m : inst.materialOverrides)
            if (m >= m_hostMaterials) fail("instance edit %u: material %u of %u", i, m, m_hostMaterials);
        for (int r = 0; r < 3; ++r)
            for (int c = 0; c < 4; ++c)
                if (!std::isfinite(inst.transform.m[r][c])) fail("instance edit %u: transform is not finite", i);
        if (i == count) ++count;
    }
    for (const auto& e : edits)
    {
        if (e.first == m_hostInstances)
        {
            ++m_hostInstances;
            m_hostSkinned.push_back(0);
        }
        m_pending.instanceEdits.push_back(e);
    }
}

void HostRenderer::editMaterials(std::span<const std::pair<uint32_t, scene::Material>> edits)
{
    requireCommitted();
    std::lock_guard lock(m_mutex);
    uint32_t count = m_hostMaterials;
    const uint32_t textures = (uint32_t)m_scene.textures.size();
    for (const auto& [i, m] : edits)
    {
        if (i > count) fail("material edit %u of %u (append in order)", i, count);
        for (uint32_t t : { m.baseColorTexture, m.normalTexture, m.roughMetalTexture, m.emissiveTexture, m.occlusionTexture })
            if (t != scene::kNone && t >= textures) fail("material edit %u: texture %u of %u (textures are fixed at commit)", i, t, textures);
        if (i == count) ++count;
    }
    for (const auto& e : edits)
    {
        if (e.first == m_hostMaterials)
        {
            ++m_hostMaterials;
            m_hostMaterialClasses.push_back(e.second.cls);
        }
        else m_hostMaterialClasses[e.first] = e.second.cls;
        m_pending.materialEdits.push_back(e);
    }
}

void HostRenderer::setCharacterShading(uint32_t material, const CharacterShading& c)
{
    auto in = [](float x, float lo, float hi) { return x >= lo && x <= hi; };
    if (!(c.subsurfaceMeanFreePath.x >= 0 && c.subsurfaceMeanFreePath.y >= 0 && c.subsurfaceMeanFreePath.z >= 0 && std::isfinite(c.subsurfaceMeanFreePath.x) &&
          std::isfinite(c.subsurfaceMeanFreePath.y) && std::isfinite(c.subsurfaceMeanFreePath.z)))
        fail("character shading: the mean free path must be >= 0 and finite (m)");
    if (!(in(c.subsurfaceLobeMix, 0, 1) && c.subsurfaceLobeRoughness.x >= 0 && c.subsurfaceLobeRoughness.y >= 0 && std::isfinite(c.subsurfaceLobeRoughness.x) &&
          std::isfinite(c.subsurfaceLobeRoughness.y)))
        fail("character shading: the lobe mix must be in [0, 1], the lobes' roughness scales >= 0 and finite");
    if (!in(c.cloth, 0, 1)) fail("character shading: cloth %g outside [0, 1]", c.cloth);
    if (c.eyeIrisRadius != 0)
    {
        const float axis2 = c.eyeAxis.x * c.eyeAxis.x + c.eyeAxis.y * c.eyeAxis.y + c.eyeAxis.z * c.eyeAxis.z;
        if (!(c.eyeIrisRadius > 0 && c.eyeIrisRadius <= 0.5f && c.eyeIrisDepth > 0 && c.eyeIrisDepth <= 2 && in(c.eyeLimbusWidth, 0.01f, 1) && in(c.eyeLimbusDarkening, 0, 1) &&
              c.eyePupilScale > 0 && c.eyePupilScale <= 8 && in(c.eyeIrisConcavity, 0, 1) && in(c.eyeIor, 1, 2) && std::fabs(axis2 - 1) <= 1e-3f))
            fail("character shading: an eye needs an iris radius in (0, 0.5], a depth in (0, 2], a limbus width in [0.01, 1], darkening and concavity in [0, 1], a "
                 "pupil scale in (0, 8], an index in [1, 2] and a unit axis");
    }
    if (!m_committed)
    {
        if (material >= m_scene.materials.size()) fail("character shading: material %u of %zu", material, m_scene.materials.size());
        applyCharacter(c, m_scene.materials[material]);
        return;
    }
    std::lock_guard lock(m_mutex);
    if (material >= m_hostMaterials) fail("character shading: material %u of %u", material, m_hostMaterials);
    m_pending.characterEdits.push_back({ material, c });
}

// The groups of 'c' the material's class defines: the skin's and the eye's on a Subsurface material (an eye has no light
// through thin parts), the cloth factor on a material with a sheen. The others are left at "none".
void HostRenderer::applyCharacter(const CharacterShading& c, scene::Material& m)
{
    const bool subsurface = m.cls == scene::MaterialClass::Subsurface;
    if (subsurface)
    {
        m.subsurfaceMeanFreePath = c.subsurfaceMeanFreePath;
        m.subsurfaceLobeMix = c.subsurfaceLobeMix;
        m.subsurfaceLobeRoughness = c.subsurfaceLobeRoughness;
    }
    m.eyeIrisRadius = subsurface ? c.eyeIrisRadius : 0.0f;
    if (m.eyeIrisRadius > 0)
    {
        m.eyeIrisDepth = c.eyeIrisDepth;
        m.eyeLimbusWidth = c.eyeLimbusWidth;
        m.eyeLimbusDarkening = c.eyeLimbusDarkening;
        m.eyePupilScale = c.eyePupilScale;
        m.eyeIrisConcavity = c.eyeIrisConcavity;
        m.eyeIor = c.eyeIor;
        m.eyeAxis = normalize(c.eyeAxis);
        m.transmission = 0;
    }
    const bool sheen = m.cls == scene::MaterialClass::Standard && (m.sheenColor.x > 0 || m.sheenColor.y > 0 || m.sheenColor.z > 0);
    m.cloth = sheen ? c.cloth : 0.0f;
}

// A material described anew (the ABI's description has no character fields) keeps the old one's while they still apply.
void HostRenderer::keepCharacter(const scene::Material& old, scene::Material& next)
{
    CharacterShading c;
    c.subsurfaceMeanFreePath = old.subsurfaceMeanFreePath;
    c.subsurfaceLobeMix = old.subsurfaceLobeMix;
    c.subsurfaceLobeRoughness = old.subsurfaceLobeRoughness;
    c.cloth = old.cloth;
    c.eyeIrisRadius = old.eyeIrisRadius;
    c.eyeIrisDepth = old.eyeIrisDepth;
    c.eyeLimbusWidth = old.eyeLimbusWidth;
    c.eyeLimbusDarkening = old.eyeLimbusDarkening;
    c.eyePupilScale = old.eyePupilScale;
    c.eyeIrisConcavity = old.eyeIrisConcavity;
    c.eyeIor = old.eyeIor;
    c.eyeAxis = old.eyeAxis;
    if (old.cls == next.cls) applyCharacter(c, next);
}

void HostRenderer::setMaterialInputs(uint32_t material, const MaterialInputs& in)
{
    auto finite2 = [](float2 v) { return std::isfinite(v.x) && std::isfinite(v.y); };
    if (!(finite2(in.uvScale) && in.uvScale.x != 0 && in.uvScale.y != 0 && finite2(in.uvOffset) && std::isfinite(in.uvRotation)))
        fail("material inputs: the uv transform needs a finite scale other than 0, a finite offset and rotation");
    if (!(finite2(in.detailScale) && in.detailScale.x != 0 && in.detailScale.y != 0 && finite2(in.detailOffset)))
        fail("material inputs: the detail maps need a finite uv scale other than 0 and a finite offset");
    if (in.occlusionUvSet > 1 || in.detailUvSet > 1) fail("material inputs: a uv set is 0 or 1");
    if (!(in.detailColorStrength >= 0 && in.detailColorStrength <= 1 && in.detailNormalScale >= 0 && in.detailNormalScale <= 4))
        fail("material inputs: detailColorStrength in [0, 1], detailNormalScale in [0, 4]");
    if (!(in.heightScale >= 0 && in.heightScale <= 1)) fail("material inputs: heightScale %g outside [0, 1] (metres)", in.heightScale);
    if (!(in.emissiveScale >= 0 && std::isfinite(in.emissiveScale))) fail("material inputs: emissiveScale must be >= 0 and finite");
    // (the textures are fixed at commit and their list does not change after it: read without the lock)
    auto texture = [&](uint32_t t, scene::TextureFormat f, const char* what) {
        if (t == scene::kNone) return;
        if (t >= m_scene.textures.size()) fail("material inputs: %s texture %u of %zu", what, t, m_scene.textures.size());
        if (m_scene.textures[t].format != f) fail("material inputs: %s texture %u has format %u", what, t, (unsigned)m_scene.textures[t].format);
    };
    texture(in.detailColorTexture, scene::TextureFormat::Rgba8Srgb, "detail colour");
    texture(in.detailNormalTexture, scene::TextureFormat::Rg8Normal, "detail normal");
    texture(in.heightTexture, scene::TextureFormat::R8Linear, "height");
    texture(in.emissiveMaskTexture, scene::TextureFormat::R8Linear, "emissive mask");
    if (!m_committed)
    {
        if (material >= m_scene.materials.size()) fail("material inputs: material %u of %zu", material, m_scene.materials.size());
        applyInputs(in, m_scene.materials[material]);
        return;
    }
    std::lock_guard lock(m_mutex);
    if (material >= m_hostMaterials) fail("material inputs: material %u of %u", material, m_hostMaterials);
    m_pending.inputEdits.push_back({ material, in });
}

// The inputs on a material: none of them on the Cut and Terrain classes (their textures are not read at the mesh's uv)
// but the emission's scale, and no dither without an alpha test.
void HostRenderer::applyInputs(const MaterialInputs& in, scene::Material& m)
{
    const bool uvClass = m.cls != scene::MaterialClass::Cut && m.cls != scene::MaterialClass::Terrain;
    const MaterialInputs none;
    const MaterialInputs& v = uvClass ? in : none;
    m.uvScale = v.uvScale;
    m.uvOffset = v.uvOffset;
    m.uvRotation = v.uvRotation;
    m.occlusionUvSet = v.occlusionUvSet;
    m.detailColorTexture = v.detailColorTexture;
    m.detailNormalTexture = v.detailNormalTexture;
    m.detailScale = v.detailScale;
    m.detailOffset = v.detailOffset;
    m.detailUvSet = v.detailUvSet;
    m.detailColorStrength = v.detailColorStrength;
    m.detailNormalScale = v.detailNormalScale;
    m.heightTexture = v.heightTexture;
    m.heightScale = v.heightScale;
    m.emissiveMaskTexture = v.emissiveMaskTexture;
    m.vertexColorTint = v.vertexColorTint;
    m.vertexAlphaBlend = v.vertexAlphaBlend;
    m.alphaDither = v.alphaDither && m.alphaCutoff > 0;
    m.emissiveScale = in.emissiveScale;
}

// A material described anew (the ABI's description has no such fields) keeps the old one's inputs.
void HostRenderer::keepInputs(const scene::Material& old, scene::Material& next)
{
    MaterialInputs in;
    in.uvScale = old.uvScale;
    in.uvOffset = old.uvOffset;
    in.uvRotation = old.uvRotation;
    in.occlusionUvSet = old.occlusionUvSet;
    in.detailColorTexture = old.detailColorTexture;
    in.detailNormalTexture = old.detailNormalTexture;
    in.detailScale = old.detailScale;
    in.detailOffset = old.detailOffset;
    in.detailUvSet = old.detailUvSet;
    in.detailColorStrength = old.detailColorStrength;
    in.detailNormalScale = old.detailNormalScale;
    in.heightTexture = old.heightTexture;
    in.heightScale = old.heightScale;
    in.emissiveScale = old.emissiveScale;
    in.emissiveMaskTexture = old.emissiveMaskTexture;
    in.vertexColorTint = old.vertexColorTint;
    in.vertexAlphaBlend = old.vertexAlphaBlend;
    in.alphaDither = old.alphaDither;
    applyInputs(in, next);
}

void HostRenderer::setMeshAttributes(uint32_t mesh, std::vector<float2> uv1, std::vector<uint32_t> colors)
{
    requireOpen();
    if (mesh >= m_scene.meshes.size()) fail("mesh attributes: mesh %u of %zu", mesh, m_scene.meshes.size());
    scene::Mesh& m = m_scene.meshes[mesh];
    if ((!uv1.empty() && uv1.size() != m.positions.size()) || (!colors.empty() && colors.size() != m.positions.size()))
        fail("mesh attributes: mesh %u has %zu vertices (uv1 %zu, colours %zu)", mesh, m.positions.size(), uv1.size(), colors.size());
    m.uv1 = std::move(uv1);
    m.colors = std::move(colors);
}

void HostRenderer::setLightComponents(uint32_t light, const scene::Light& c)
{
    requireOpen();
    if (light >= m_scene.lights.size()) fail("light components: light %u of %zu", light, m_scene.lights.size());
    scene::Light& l = m_scene.lights[light];
    l.specularScale = c.specularScale;
    l.diffuseScale = c.diffuseScale;
    l.volumetricScattering = c.volumetricScattering;
    l.indirectIntensity = c.indirectIntensity;
    l.sourceTexture = c.sourceTexture;
    l.barnDoorAngle = c.barnDoorAngle;
    l.barnDoorLength = c.barnDoorLength;
    l.lightingChannels = c.lightingChannels;
    l.maxDrawDistance = c.maxDrawDistance;
    l.maxDistanceFadeRange = c.maxDistanceFadeRange;
    l.temperature = c.temperature;
    l.falloffExponent = c.falloffExponent;
}

void HostRenderer::setInstanceLightingChannels(uint32_t instance, uint32_t channels)
{
    requireOpen();
    if (instance >= m_scene.instances.size()) fail("lighting channels: instance %u of %zu", instance, m_scene.instances.size());
    if (channels > 7) fail("lighting channels: 0x%x (three bits)", channels);
    m_scene.instances[instance].flags = scene::withLightingChannels(m_scene.instances[instance].flags, channels);
}

void HostRenderer::applyEdits(const FramePacket& p, scene::Scene& s)
{
    for (const auto& [i, m] : p.materialEdits)
        if (i == s.materials.size()) s.materials.push_back(m);
        else
        {
            scene::Material next = m;
            keepCharacter(s.materials[i], next);
            keepInputs(s.materials[i], next);
            s.materials[i] = std::move(next);
        }
    for (const auto& [i, c] : p.characterEdits)
        if (i < s.materials.size()) applyCharacter(c, s.materials[i]);
    for (const auto& [i, in] : p.inputEdits)
        if (i < s.materials.size()) applyInputs(in, s.materials[i]);
    for (const auto& [i, inst] : p.instanceEdits)
        if (i == s.instances.size()) s.instances.push_back(inst);
        else s.instances[i] = inst;
}

void HostRenderer::addBlendShape(uint32_t mesh, scene::BlendShape shape)
{
    requireOpen();
    if (mesh >= m_scene.meshes.size()) fail("blend shape of mesh %u of %zu", mesh, m_scene.meshes.size());
    m_scene.meshes[mesh].blendShapes.push_back(std::move(shape));
}

void HostRenderer::setVertexAnimation(uint32_t mesh, scene::VertexAnimation animation)
{
    requireOpen();
    if (mesh >= m_scene.meshes.size()) fail("vertex animation of mesh %u of %zu", mesh, m_scene.meshes.size());
    m_scene.meshes[mesh].vertexAnimation = std::move(animation);
}

void HostRenderer::setMorph(uint32_t instance, std::vector<float> weights, float time)
{
    requireCommitted();
    if (instance >= m_scene.instances.size()) fail("morph of instance %u of %zu", instance, m_scene.instances.size());
    const scene::Mesh& m = m_scene.meshes[m_scene.instances[instance].mesh];
    if (m.blendShapes.empty() && m.vertexAnimation.framesPerSecond <= 0) fail("instance %u: its mesh '%s' has no blend shapes or vertex animation", instance, m.name.c_str());
    if (weights.size() != m.blendShapes.size()) fail("instance %u: %zu weights for %zu blend shapes", instance, weights.size(), m.blendShapes.size());
    for (float w : weights)
        if (!std::isfinite(w)) fail("instance %u: blend weight not finite", instance);
    if (!std::isfinite(time)) fail("instance %u: vertex animation time not finite", instance);
    std::lock_guard lock(m_mutex);
    m_pending.morphs.push_back({ instance, std::move(weights), time });
}

// C9: runtime adds and moves queued in older coordinates follow an origin shift like transform updates.
static void shiftRuntime(std::vector<FramePacket::RuntimeOp>& ops, float3 shift)
{
    for (FramePacket::RuntimeOp& op : ops)
        if (op.kind == FramePacket::RuntimeOp::AddInstance || op.kind == FramePacket::RuntimeOp::Transform)
            op.transform.m[0][3] -= shift.x, op.transform.m[1][3] -= shift.y, op.transform.m[2][3] -= shift.z;
}

void HostRenderer::setOriginShift(float3 shift)
{
    requireCommitted();
    for (float c : { shift.x, shift.y, shift.z })
        if (!std::isfinite(c) || std::fabs(c / kOriginGrid - std::round(c / kOriginGrid)) != 0.0f) fail("origin shift (%g, %g, %g) is not on the 1024 m grid", shift.x, shift.y, shift.z);
    std::lock_guard lock(m_mutex);
    m_mainOriginOffset = m_mainOriginOffset + shift;
    m_pending.originShift = m_pending.originShift + shift;
    for (InstanceTransformUpdate& u : m_pending.transforms)  // queued for this frame in the old coordinates
        u.objectToWorld.m[0][3] -= shift.x, u.objectToWorld.m[1][3] -= shift.y, u.objectToWorld.m[2][3] -= shift.z;
    shiftRuntime(m_pending.runtime, shift);
    for (auto& [id, p] : m_runtimeInstancePlacement) p.transform.m[0][3] -= shift.x, p.transform.m[1][3] -= shift.y, p.transform.m[2][3] -= shift.z;
    m_lastCamera.position = m_lastCamera.position - shift;
}

void HostRenderer::reserveRuntime(const render::RuntimeCapacity& capacity)
{
    requireOpen();
    m_runtimeCapacity = capacity;
}

uint32_t HostRenderer::addRuntimeMesh(scene::Mesh mesh)
{
    requireCommitted();
    if (m_runtimeCapacity.meshes == 0) fail("runtime mesh: no runtime room (UnxSceneReserveRuntime before commit)");
    for (const scene::Submesh& sm : mesh.submeshes)
        if (sm.material >= m_scene.materials.size()) fail("runtime mesh '%s': material %u of %zu", mesh.name.c_str(), sm.material, m_scene.materials.size());
    // Clusters on the calling thread (no simplification: a shallow hierarchy, exact at every distance).
    scene::Scene one;
    one.materials = m_scene.materials;
    one.textures.resize(m_scene.textures.size());  // empty placeholders: validation checks indices only; clusters need no texels
    one.meshes.push_back(mesh);
    scene::validate(one);
#if UNX_HOST_HAS_CLUSTERBUILDER
    clusterbuilder::Settings settings = clusterbuilder::Settings::fromQuality(m_quality);
    settings.noSimplification = true;
    auto clusters = std::make_shared<const ClusterData>(clusterbuilder::build(one, settings));
#else
    fail("this build has no cluster builder");
#endif
    std::lock_guard lock(m_mutex);
    const uint32_t id = 0x80000000u | m_nextRuntimeMesh++;
    FramePacket::RuntimeOp op;
    op.kind = FramePacket::RuntimeOp::AddMesh;
    op.id = id;
    op.meshData = std::make_shared<const scene::Mesh>(std::move(mesh));
    op.clusters = std::move(clusters);
    m_runtimeMeshData[id] = op.meshData;
    m_pending.runtime.push_back(std::move(op));
    m_runtimeMeshLive.insert(id);
    return id;
}

void HostRenderer::removeRuntimeMesh(uint32_t id)
{
    requireCommitted();
    std::lock_guard lock(m_mutex);
    if (!m_runtimeMeshLive.count(id)) fail("runtime mesh %u is not live", id);
    for (const auto& [instance, mesh] : m_runtimeInstanceMesh)
        if (mesh == id) fail("runtime mesh %u: runtime instance %u still uses it (remove the instances first)", id, instance);
    m_runtimeMeshLive.erase(id);
    m_runtimeMeshData.erase(id);
    FramePacket::RuntimeOp op;
    op.kind = FramePacket::RuntimeOp::RemoveMesh;
    op.id = id;
    m_pending.runtime.push_back(std::move(op));
}

uint32_t HostRenderer::addRuntimeInstance(uint32_t mesh, const float3x4& transform, uint32_t flags)
{
    requireCommitted();
    std::lock_guard lock(m_mutex);
    if ((mesh & 0x80000000u) ? !m_runtimeMeshLive.count(mesh) : mesh >= m_scene.meshes.size()) fail("runtime instance: mesh %u is neither committed nor a live runtime mesh", mesh);
    const uint32_t id = m_nextRuntimeInstance++;
    FramePacket::RuntimeOp op;
    op.kind = FramePacket::RuntimeOp::AddInstance;
    op.id = id, op.mesh = mesh, op.flags = flags & ~(uint32_t)scene::InstanceSkinned, op.transform = transform;
    m_runtimeInstancePlacement[id] = { transform, op.flags };
    m_pending.runtime.push_back(std::move(op));
    m_runtimeInstanceMesh[id] = mesh;
    return id;
}

void HostRenderer::removeRuntimeInstance(uint32_t id)
{
    requireCommitted();
    std::lock_guard lock(m_mutex);
    if (!m_runtimeInstanceMesh.erase(id)) fail("runtime instance %u is not live", id);
    m_runtimeInstancePlacement.erase(id);
    FramePacket::RuntimeOp op;
    op.kind = FramePacket::RuntimeOp::RemoveInstance;
    op.id = id;
    m_pending.runtime.push_back(std::move(op));
}

void HostRenderer::setRuntimeTransform(uint32_t id, const float3x4& transform)
{
    requireCommitted();
    std::lock_guard lock(m_mutex);
    if (!m_runtimeInstanceMesh.count(id)) fail("runtime instance %u is not live", id);
    m_runtimeInstancePlacement[id].transform = transform;
    FramePacket::RuntimeOp op;
    op.kind = FramePacket::RuntimeOp::Transform;
    op.id = id, op.transform = transform;
    m_pending.runtime.push_back(std::move(op));
}

void HostRenderer::setTerrainDeformation(double originX, double originZ, float spacing, uint32_t texels, const float* heights, std::span<const uint32_t> tiles)
{
    requireCommitted();
    if (texels && !heights) fail("terrain deformation: %u^2 texels without heights", texels);
    std::unordered_set<uint32_t> listed(tiles.begin(), tiles.end());
    auto dropTile = [&](uint32_t tile) {
        auto it = m_terrainPatches.find(tile);
        if (it == m_terrainPatches.end()) return;
        for (const auto& [block, p] : it->second)
        {
            removeRuntimeInstance(p.instance);
            removeRuntimeMesh(p.mesh);
        }
        m_terrainPatches.erase(it);
        FramePacket::RuntimeOp op;
        op.kind = FramePacket::RuntimeOp::Patch;
        op.id = tile;
        op.region = std::make_shared<const render::GpuScene::PatchRegion>();
        std::lock_guard lock(m_mutex);
        m_patchRegions.erase(tile);
        m_pending.runtime.push_back(std::move(op));
    };
    std::vector<uint32_t> gone;
    for (const auto& [tile, blocks] : m_terrainPatches)
        if (!listed.count(tile)) gone.push_back(tile);
    for (uint32_t tile : gone) dropTile(tile);
    for (uint32_t tile : listed)
    {
        if (tile >= m_scene.instances.size()) fail("terrain deformation: tile instance %u of %zu", tile, m_scene.instances.size());
        const scene::Instance& in = m_scene.instances[tile];
        const float3x4& t = in.transform;
        if (t.m[0][0] != 1 || t.m[1][1] != 1 || t.m[2][2] != 1 || t.m[0][1] != 0 || t.m[0][2] != 0 || t.m[1][0] != 0 || t.m[1][2] != 0 || t.m[2][0] != 0 || t.m[2][1] != 0)
            fail("terrain deformation: tile instance %u is not placed by a translation", tile);
        const scene::TerrainGrid grid = scene::terrainGrid(m_scene.meshes[in.mesh]);
        // The tile's translation in this frame's coordinates; D in the tile's object space.
        float3x4 now = t;
        now.m[0][3] -= m_mainOriginOffset.x, now.m[1][3] -= m_mainOriginOffset.y, now.m[2][3] -= m_mainOriginOffset.z;
        scene::TerrainDeformation d;
        d.originX = (float)(originX - now.m[0][3]);
        d.originZ = (float)(originZ - now.m[2][3]);
        d.spacing = spacing;
        d.size = texels;
        d.height = heights;
        const std::vector<uint32_t> active = texels ? scene::activePatchBlocks(grid, d) : std::vector<uint32_t>{};
        const uint32_t side = scene::patchBlocksPerSide(grid);
        std::unordered_map<uint32_t, PatchBlock>& state = m_terrainPatches[tile];
        std::unordered_map<uint32_t, PatchBlock> next;
        for (uint32_t b : active)
        {
            const uint64_t hash = scene::patchBlockHash(grid, d, b % side, b / side);
            auto old = state.find(b);
            if (old != state.end() && old->second.hash == hash)
            {
                next[b] = old->second;
                state.erase(old);
                continue;
            }
            PatchBlock p;
            p.hash = hash;
            p.mesh = addRuntimeMesh(scene::buildTerrainPatch(grid, d, b % side, b / side));
            ++m_patchBuilds;
            p.instance = addRuntimeInstance(p.mesh, now, in.flags);
            next[b] = p;
        }
        for (const auto& [block, p] : state)  // changed or no longer replaced
        {
            removeRuntimeInstance(p.instance);
            removeRuntimeMesh(p.mesh);
        }
        state = std::move(next);
        auto region = std::make_shared<render::GpuScene::PatchRegion>();
        region->originX = grid.originX, region->originZ = grid.originZ;
        region->blockX = grid.stepX * scene::kPatchBlockCells, region->blockZ = grid.stepZ * scene::kPatchBlockCells;
        region->blocksPerSide = side;
        region->blocks = active;
        FramePacket::RuntimeOp op;
        op.kind = FramePacket::RuntimeOp::Patch;
        op.id = tile;
        op.region = std::move(region);
        {
            std::lock_guard lock(m_mutex);
            if (op.region->blocks.empty()) m_patchRegions.erase(tile);
            else m_patchRegions[tile] = op.region;
            m_pending.runtime.push_back(std::move(op));
        }
        if (active.empty()) m_terrainPatches.erase(tile);
    }
}

uint32_t HostRenderer::meshVertexCount(uint32_t mesh) const
{
    if (mesh >= m_scene.meshes.size()) fail("mesh %u of %zu", mesh, m_scene.meshes.size());
    return (uint32_t)m_scene.meshes[mesh].positions.size();
}

uint32_t HostRenderer::blendShapeCount(uint32_t instance) const
{
    if (instance >= m_scene.instances.size()) fail("instance %u of %zu", instance, m_scene.instances.size());
    return (uint32_t)m_scene.meshes[m_scene.instances[instance].mesh].blendShapes.size();
}

void HostRenderer::setSkeleton(uint32_t skeleton, std::vector<float3x4> jointToModel)
{
    requireCommitted();
    if (skeleton >= m_scene.skeletons.size()) fail("skeleton %u of %zu", skeleton, m_scene.skeletons.size());
    if (jointToModel.size() != m_scene.skeletons[skeleton].jointToModel.size())
        fail("skeleton %u has %zu joints, pose has %zu", skeleton, m_scene.skeletons[skeleton].jointToModel.size(), jointToModel.size());
    auto pose = std::make_shared<const std::vector<float3x4>>(std::move(jointToModel));
    std::lock_guard lock(m_mutex);
    m_pending.skeletons.push_back({ skeleton, std::move(pose) });
}

uint32_t HostRenderer::jointCount(uint32_t skeleton) const
{
    if (skeleton >= m_scene.skeletons.size()) fail("skeleton %u of %zu", skeleton, m_scene.skeletons.size());
    return (uint32_t)m_scene.skeletons[skeleton].jointToModel.size();
}

void HostRenderer::overlay(const FramePacket& p, HostState& state)
{
    if (p.sun) state.sun = *p.sun;
    if (p.atmosphere) state.atmosphere = *p.atmosphere;
    if (p.wind) state.wind = *p.wind;
    for (const auto& [i, inst] : p.instanceEdits)  // before the packet's transforms and visibility
    {
        if (i == state.transforms.size())
        {
            state.transforms.push_back(inst.transform);
            state.visible.push_back(1);
        }
        state.transforms[i] = inst.transform;
        state.visible[i] = 1;
    }
    if (p.originShift.x != 0 || p.originShift.y != 0 || p.originShift.z != 0)
        for (float3x4& t : state.transforms) t.m[0][3] -= p.originShift.x, t.m[1][3] -= p.originShift.y, t.m[2][3] -= p.originShift.z;
    for (const InstanceTransformUpdate& u : p.transforms) state.transforms[u.instance] = u.objectToWorld;
    for (const FramePacket::Morph& m : p.morphs)
    {
        if (state.morphs.size() <= m.instance) state.morphs.resize(m.instance + 1);
        state.morphs[m.instance] = { m.weights, m.time };
    }
    for (const SkeletonPose& s : p.skeletons) state.poses[s.skeleton] = s.jointToModel;
    for (const auto& [instance, visible] : p.visibility) state.visible[instance] = visible ? 1 : 0;
}

scene::Scene HostRenderer::currentScene() const { return snapshot(false); }

scene::Scene HostRenderer::photoScene() const { return snapshot(true); }

namespace
{
float3x4 compose(const float3x4& a, const float3x4& b)  // a * b (affine; viewmodel::compose's operations)
{
    float3x4 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
        {
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
            if (j == 3) r.m[i][j] += a.m[i][3];
        }
    return r;
}

// A patched terrain tile's mesh without the triangles V leaves out (VisibilityCommon.hlsli patchReplaced: the object-space
// centroid's block, floor((c.xz - origin) / block), is a replaced one), in the same float operations.
// keptSubmeshes: the source submesh of each output submesh (a submesh left without triangles is dropped).
scene::Mesh withoutReplaced(const scene::Mesh& mesh, const render::GpuScene::PatchRegion& r, std::vector<uint32_t>& keptSubmeshes)
{
    std::unordered_set<uint32_t> replaced(r.blocks.begin(), r.blocks.end());
    scene::Mesh out = mesh;
    out.indices.clear();
    out.submeshes.clear();
    for (uint32_t k = 0; k < mesh.submeshes.size(); ++k)
    {
        const scene::Submesh& sm = mesh.submeshes[k];
        scene::Submesh kept{ (uint32_t)out.indices.size(), 0, sm.material };
        for (uint32_t t = 0; t < sm.indexCount; t += 3)
        {
            const uint32_t* i = &mesh.indices[sm.indexOffset + t];
            const float3 a = mesh.positions[i[0]], b = mesh.positions[i[1]], c = mesh.positions[i[2]];
            const float cx = (a.x + b.x + c.x) / 3.0f, cz = (a.z + b.z + c.z) / 3.0f;
            const float bx = std::floor((cx - r.originX) / r.blockX), bz = std::floor((cz - r.originZ) / r.blockZ);
            const bool inside = bx >= 0 && bz >= 0 && bx < (float)r.blocksPerSide && bz < (float)r.blocksPerSide;
            if (inside && replaced.count((uint32_t)bz * r.blocksPerSide + (uint32_t)bx)) continue;
            out.indices.insert(out.indices.end(), i, i + 3);
            kept.indexCount += 3;
        }
        if (!kept.indexCount) continue;
        out.submeshes.push_back(kept);
        keptSubmeshes.push_back(k);
    }
    return out;
}
} // namespace

scene::Scene HostRenderer::snapshot(bool photo) const
{
    requireCommitted();
    scene::Scene s;
    HostState state;
    float3 originOffset;
    std::unordered_map<uint32_t, std::shared_ptr<const scene::Mesh>> runtimeMeshes;
    std::unordered_map<uint32_t, RuntimePlacement> runtimeInstances;
    std::unordered_map<uint32_t, uint32_t> runtimeInstanceMesh;
    std::unordered_map<uint32_t, std::shared_ptr<const render::GpuScene::PatchRegion>> patches;
    std::vector<viewmodel::ViewModels::Entry> viewModels;
    scene::Camera camera;
    bool haveCamera = false;
    render::CloudLayerDesc cloudsNow;
    render::FogDesc fogNow;
    std::vector<render::FogVolumeDesc> fogVolumesNow;
    {
        std::lock_guard lock(m_mutex);
        {
            std::lock_guard applied(m_appliedMutex);
            s = m_scene;
            state = m_applied;
        }
        for (const FramePacket& p : m_packets)
        {
            applyEdits(p, s);
            overlay(p, state);
        }
        applyEdits(m_pending, s);
        overlay(m_pending, state);
        originOffset = m_mainOriginOffset;
        cloudsNow = m_clouds;
        fogNow = m_fog;
        fogVolumesNow = m_fogVolumes;
        if (photo)
        {
            runtimeMeshes = m_runtimeMeshData;
            runtimeInstances = m_runtimeInstancePlacement;
            runtimeInstanceMesh = m_runtimeInstanceMesh;
            patches = m_patchRegions;
            viewModels = m_viewModels.entries();
            camera = m_lastCamera;
            haveCamera = m_haveLastCamera;
        }
    }
    // Lights stay where they were added; the GPU scene moves them by every origin shift (GpuScene::rebase).
    for (scene::Light& l : s.lights) l.position = l.position - originOffset;
    // The weather the host set (UnxFrameSetClouds / SetFog / SetFogVolumes), in the saved scene's coordinates: a gate that
    // loads the file shows the frame's clouds and fog (scene::Scene's CLDS and FOGS blocks).
    {
        s.clouds.coverage = cloudsNow.coverage;
        s.clouds.baseAltitude = cloudsNow.baseAltitude - originOffset.y;
        s.clouds.topAltitude = cloudsNow.topAltitude - originOffset.y;
        s.clouds.sigmaMax = cloudsNow.sigmaMax;
        s.clouds.albedo = cloudsNow.albedo;
        s.clouds.windX = cloudsNow.windX;
        s.clouds.windZ = cloudsNow.windZ;
        s.clouds.cirrusCoverage = cloudsNow.cirrusCoverage;
        s.clouds.cirrusAltitude = cloudsNow.cirrusAltitude - originOffset.y;
        s.clouds.cirrusOpticalDepth = cloudsNow.cirrusOpticalDepth;
        s.clouds.cirrusWindX = cloudsNow.cirrusWindX;
        s.clouds.cirrusWindZ = cloudsNow.cirrusWindZ;
        s.fog.enabled = fogNow.enabled;
        s.fog.density = fogNow.density;
        s.fog.heightFalloff = fogNow.heightFalloff;
        s.fog.height = fogNow.height - originOffset.y;
        s.fog.albedo = { fogNow.albedo[0], fogNow.albedo[1], fogNow.albedo[2] };
        s.fog.phaseG = fogNow.phaseG;
        s.fog.startDistance = fogNow.startDistance;
        s.fog.skyAmount = fogNow.skyAmount;
        s.fog.noiseAmount = fogNow.noiseAmount;
        s.fog.noiseScale = fogNow.noiseScale;
        s.fog.density2 = fogNow.density2;
        s.fog.heightFalloff2 = fogNow.heightFalloff2;
        s.fog.height2 = fogNow.height2 - originOffset.y;
        s.fogVolumes.clear();
        for (const render::FogVolumeDesc& v : fogVolumesNow)
        {
            scene::FogVolume o;
            o.centre = { (float)(v.centre[0] - originOffset.x), (float)(v.centre[1] - originOffset.y), (float)(v.centre[2] - originOffset.z) };
            o.halfSize = { v.halfSize[0], v.halfSize[1], v.halfSize[2] };
            o.yaw = v.yaw;
            o.shape = v.shape;
            o.density = v.density;
            o.heightFalloff = v.heightFalloff;
            o.edge = v.edge;
            o.albedo = { v.albedo[0], v.albedo[1], v.albedo[2] };
            // (the steam's values; a density grid is the frame's alone: the scene file holds none)
            o.sourcePlane = v.sourcePlane;
            o.riseSpeed = v.riseSpeed;
            o.turbulence = v.turbulence;
            o.turbulenceScale = v.turbulenceScale;
            s.fogVolumes.push_back(o);
        }
    }
    if (photo)
    {
        // A12: view models where the frames draw them, the host's latest frame camera x their pose
        if (haveCamera)
        {
            const float3x4 cameraToWorld = viewmodel::cameraToWorld(ViewDesc::fromCamera(camera, 16, 9, {}));
            for (const viewmodel::ViewModels::Entry& e : viewModels)
                if (e.live && e.instance < state.transforms.size()) state.transforms[e.instance] = compose(cameraToWorld, e.cameraLocal);
        }
        // C5: a patched tile draws its mesh without the replaced triangles (the patch meshes are runtime meshes)
        for (const auto& [tile, region] : patches)
        {
            if (tile >= s.instances.size() || region->blocks.empty()) continue;
            std::vector<uint32_t> kept;
            scene::Mesh cut = withoutReplaced(s.meshes[s.instances[tile].mesh], *region, kept);
            if (cut.submeshes.empty())
            {
                state.visible[tile] = 0;  // every triangle replaced
                continue;
            }
            scene::Instance& in = s.instances[tile];
            if (!in.materialOverrides.empty())
            {
                std::vector<uint32_t> overrides;
                for (uint32_t k : kept) overrides.push_back(in.materialOverrides[k]);
                in.materialOverrides = std::move(overrides);
            }
            s.meshes.push_back(std::move(cut));
            in.mesh = (uint32_t)s.meshes.size() - 1;
        }
    }
    s.sun = state.sun;
    s.atmosphere = state.atmosphere;
    s.windDirection = state.wind.direction;
    s.windSpeed = state.wind.speed;
    std::vector<scene::Instance> shown;
    for (size_t i = 0; i < s.instances.size(); ++i)
    {
        if (!state.visible[i]) continue;
        shown.push_back(std::move(s.instances[i]));
        shown.back().transform = state.transforms[i];
        if (i < state.morphs.size() && !state.morphs[i].first.empty())  // C4: the current weights and time
        {
            shown.back().blendWeights = state.morphs[i].first;
            shown.back().vertexAnimationTime = state.morphs[i].second;
        }
        else if (i < state.morphs.size() && state.morphs[i].second != 0)
            shown.back().vertexAnimationTime = state.morphs[i].second;
    }
    s.instances = std::move(shown);
    for (size_t k = 0; k < s.skeletons.size(); ++k) s.skeletons[k].jointToModel = *state.poses[k];
    if (photo)
    {
        // C2b: the live runtime meshes and instances (ids in creation order, so the snapshot is deterministic)
        std::vector<uint32_t> meshIds, instanceIds;
        for (const auto& [id, m] : runtimeMeshes) meshIds.push_back(id);
        for (const auto& [id, p] : runtimeInstances) instanceIds.push_back(id);
        std::sort(meshIds.begin(), meshIds.end());
        std::sort(instanceIds.begin(), instanceIds.end());
        std::unordered_map<uint32_t, uint32_t> meshIndex;
        for (uint32_t id : meshIds)
        {
            meshIndex[id] = (uint32_t)s.meshes.size();
            s.meshes.push_back(*runtimeMeshes[id]);
        }
        for (uint32_t id : instanceIds)
        {
            const uint32_t mesh = runtimeInstanceMesh.at(id);
            scene::Instance in;
            in.mesh = (mesh & 0x80000000u) ? meshIndex.at(mesh) : mesh;
            in.transform = runtimeInstances[id].transform;
            in.flags = runtimeInstances[id].flags;
            s.instances.push_back(std::move(in));
        }
    }
    return s;
}

void HostRenderer::setInstanceVisible(uint32_t instance, bool visible)
{
    requireCommitted();
    std::lock_guard lock(m_mutex);
    if (instance >= m_hostInstances) fail("visibility of instance %u of %u", instance, m_hostInstances);
    m_pending.visibility.push_back({ instance, visible });
}

void HostRenderer::setSun(const scene::Sun& sun)
{
    std::lock_guard lock(m_mutex);
    m_pending.sun = sun;
}

void HostRenderer::setLens(float aperture, float focus)
{
    if (!(aperture >= 0) || !std::isfinite(aperture) || (aperture > 0 && !(focus > 0 && std::isfinite(focus))))
        fail("lens: aperture %g m (>= 0) and focus %g m (> 0 with an aperture)", aperture, focus);
    std::lock_guard lock(m_mutex);
    m_lensAperture = aperture;
    m_lensFocus = focus;
}

void HostRenderer::setWhiteBalance(float kelvin, float tint)
{
    if (!std::isfinite(kelvin) || !std::isfinite(tint) || !(kelvin == 0 || (kelvin >= 1000 && kelvin <= 40000)) || !(std::abs(tint) <= 0.1f))
        fail("white balance: temperature %g K (0, or 1000..40000) and tint %g (|tint| <= 0.1)", kelvin, tint);
    std::lock_guard lock(m_mutex);
    m_whiteBalanceKelvin = kelvin;
    m_whiteBalanceTint = tint;
}

void HostRenderer::mapMeshAsset(uint64_t asset, uint32_t mesh)
{
    requireCommitted();
    if (mesh != 0xFFFFFFFFu && !(mesh & 0x80000000u) && mesh >= m_scene.meshes.size())
        fail("mesh asset %llu: mesh %u of %zu committed meshes", (unsigned long long)asset, mesh, m_scene.meshes.size());
    if (mesh != 0xFFFFFFFFu && (mesh & 0x80000000u) && !m_runtimeMeshLive.count(mesh)) fail("mesh asset %llu: runtime mesh %u is not live", (unsigned long long)asset, mesh);
    std::lock_guard lock(m_mutex);
    if (mesh == 0xFFFFFFFFu) m_meshAssetMap.erase(asset);
    else m_meshAssetMap[asset] = mesh;
    m_meshAssetsChanged = true;
}

void HostRenderer::surfaceDelta(const surface::BrickInput* changed, size_t changedCount, const int32_t* removedKeys, size_t removedCount)
{
    if ((changedCount && !changed) || (removedCount && !removedKeys)) fail("surface delta: null records");
    FramePacket::SurfaceDelta d;
    d.changed.assign(changed, changed + changedCount);
    d.removed.assign(removedKeys, removedKeys + 3 * removedCount);
    std::lock_guard lock(m_mutex);
    m_pending.surfaceDeltas.push_back(std::move(d));
}

void HostRenderer::setSurfaceHalfLives(const std::array<double, surface::kChannels>& halfLife)
{
    for (double h : halfLife)
        if (!(h >= 0) || !std::isfinite(h)) fail("surface half-lives: %g s (>= 0, finite)", h);
    std::lock_guard lock(m_mutex);
    m_pending.surfaceHalfLives = halfLife;
}

void HostRenderer::setSurfaceTime(double seconds)
{
    if (!std::isfinite(seconds)) fail("surface time: %g s", seconds);
    std::lock_guard lock(m_mutex);
    m_surfaceTime = seconds;
}

void HostRenderer::debugPrimitives(std::span<const debug::Line> lines, std::span<const debug::Triangle> triangles, std::span<const debug::Glyph> glyphs)
{
    std::lock_guard lock(m_mutex);
    m_pending.debugLines.insert(m_pending.debugLines.end(), lines.begin(), lines.end());
    m_pending.debugTriangles.insert(m_pending.debugTriangles.end(), triangles.begin(), triangles.end());
    m_pending.debugGlyphs.insert(m_pending.debugGlyphs.end(), glyphs.begin(), glyphs.end());
}

void HostRenderer::debugText(float3 anchor, std::string_view text, uint32_t color, float sizePx, uint32_t flags, float2 offsetPx)
{
    debug::DrawList list;  // E's glyph layout, into this frame's records
    list.text(anchor, text, color, sizePx, flags, offsetPx);
    std::lock_guard lock(m_mutex);
    m_pending.debugGlyphs.insert(m_pending.debugGlyphs.end(), list.glyphs.begin(), list.glyphs.end());
}

namespace
{
void checkDecal(const decal::Decal& d, uint32_t materials, uint32_t instances)
{
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            if (!std::isfinite(d.box.m[r][c])) fail("decal: box is not finite");
    if (d.material >= materials) fail("decal: material %u of %u", d.material, materials);
    if (d.instance != decal::kNone && d.instance >= instances) fail("decal: instance %u of %u", d.instance, instances);
    if (!(d.opacity >= 0 && d.opacity <= 1) || !(d.fadeStartDegrees >= 0 && d.fadeStartDegrees <= d.fadeEndDegrees && d.fadeEndDegrees <= 180) ||
        !(d.edge >= 0 && d.edge <= 1))
        fail("decal: opacity %g (0..1), fade %g..%g degrees, edge %g (0..1)", d.opacity, d.fadeStartDegrees, d.fadeEndDegrees, d.edge);
}
} // namespace

uint32_t HostRenderer::decalAdd(const decal::Decal& d)
{
    requireCommitted();
    std::lock_guard lock(m_mutex);
    checkDecal(d, m_hostMaterials, m_hostInstances);
    const uint32_t id = m_decals.add(d);
    if (id >= m_decalLive.size()) m_decalLive.resize(id + 1, 0);
    m_decalLive[id] = 1;
    m_decalsChanged = true;
    return id;
}

void HostRenderer::decalUpdate(uint32_t id, const decal::Decal& d)
{
    requireCommitted();
    std::lock_guard lock(m_mutex);
    if (id >= m_decalLive.size() || !m_decalLive[id]) fail("decal %u is not live", id);
    checkDecal(d, m_hostMaterials, m_hostInstances);
    m_decals.update(id, d);
    m_decalsChanged = true;
}

void HostRenderer::decalRemove(uint32_t id)
{
    requireCommitted();
    std::lock_guard lock(m_mutex);
    if (id >= m_decalLive.size() || !m_decalLive[id]) fail("decal %u is not live", id);
    m_decals.remove(id);
    m_decalLive[id] = 0;
    m_decalsChanged = true;
}

namespace
{
void checkPose(const float3x4& p, const char* what)
{
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 4; ++c)
            if (!std::isfinite(p.m[r][c])) fail("%s: the camera-space pose is not finite", what);
}
} // namespace

uint32_t HostRenderer::hairAddBody(const hair::BodyDesc& desc)
{
    requireCommitted();
    {
        hair::HairSystem check;  // E's own validation, on the calling thread (a body needs no GPU until its first frame)
        check.addBody(desc);
    }
    if (desc.material >= m_scene.materials.size() || m_scene.materials[desc.material].cls != scene::MaterialClass::Hair)
        fail("hair body: material %u is not a Hair-class material", desc.material);
    std::lock_guard lock(m_mutex);
    uint32_t id;
    if (!m_hairFree.empty())
    {
        id = m_hairFree.back();
        m_hairFree.pop_back();
    }
    else
    {
        id = (uint32_t)m_hairJoints.size();
        m_hairJoints.push_back(0);
    }
    m_hairJoints[id] = desc.joints;
    FramePacket::HairOp op;
    op.kind = FramePacket::HairOp::AddBody;
    op.body = id;
    op.desc = std::make_shared<const hair::BodyDesc>(desc);
    m_pending.hairOps.push_back(std::move(op));
    return id;
}

void HostRenderer::hairTick(uint32_t body, std::span<const float3x4> joints, std::span<const hair::Capsule> capsules, float3 wind, float dt)
{
    requireCommitted();
    if (!(dt > 0)) fail("hair tick: dt %g", dt);
    std::lock_guard lock(m_mutex);
    if (body >= m_hairJoints.size() || m_hairJoints[body] == 0) fail("hair body %u is not live", body);
    if (joints.size() != m_hairJoints[body]) fail("hair tick: %zu joints for a body of %u", joints.size(), m_hairJoints[body]);
    FramePacket::HairOp op;
    op.kind = FramePacket::HairOp::Tick;
    op.body = body;
    op.joints.assign(joints.begin(), joints.end());
    op.capsules.assign(capsules.begin(), capsules.end());
    op.wind = wind;
    op.dt = dt;
    m_pending.hairOps.push_back(std::move(op));
}

void HostRenderer::hairSetFrameFraction(float fraction)
{
    if (!(fraction >= 0 && fraction <= 1)) fail("hair frame fraction %g outside [0, 1]", fraction);
    std::lock_guard lock(m_mutex);
    m_pending.hairFraction = fraction;
}

void HostRenderer::hairRemoveBody(uint32_t body)
{
    requireCommitted();
    std::lock_guard lock(m_mutex);
    if (body >= m_hairJoints.size() || m_hairJoints[body] == 0) fail("hair body %u is not live", body);
    m_hairJoints[body] = 0;
    m_hairFree.push_back(body);
    FramePacket::HairOp op;
    op.kind = FramePacket::HairOp::RemoveBody;
    op.body = body;
    m_pending.hairOps.push_back(std::move(op));
}

namespace
{
// NP_FluidGpuView (Unravel Native/NativePhysics/include/NativePhysicsFluid.h, 96 B): the layout the host reads.
struct FluidGpuView
{
    uint32_t size, version;
    void* current;
    void* start;
    uint64_t currentResource, startResource;
    uint32_t count, startCount, stride, startValid;
    double origin[3];
    float dx;
    uint32_t reserved;
    uint64_t tick;
};
static_assert(sizeof(FluidGpuView) == 96, "NP_FluidGpuView");
// NP_FluidGpuView2 (version 2, 136 B; physics a342d694, an anchored domain): the start buffer's positions are cells from
// start_origin (where the domain was when the tick started), the current buffer's from origin; velocities in both are
// relative to frame_velocity (m/s). Unanchored: start_origin = origin, frame_velocity 0.
struct FluidGpuView2
{
    FluidGpuView view;
    double startOrigin[3];
    float frameVelocity[3];
    uint32_t reserved;
};
static_assert(sizeof(FluidGpuView2) == 136, "NP_FluidGpuView2");
} // namespace

void HostRenderer::setFluids(std::span<const FluidInput> fluids, const uint64_t (&stamp)[6])
{
    requireCommitted();
    auto list = std::make_shared<std::vector<FramePacket::Fluid>>();
    std::lock_guard lock(m_mutex);
    for (const FluidInput& in : fluids)
    {
        if (!in.view) fail("fluids: no view");
        uint32_t head[2];
        std::memcpy(head, in.view, sizeof head);
        FluidGpuView2 v2{};
        if (head[0] == sizeof(FluidGpuView) && head[1] == 1)
            std::memcpy(&v2.view, in.view, sizeof(FluidGpuView));
        else if (head[0] == sizeof(FluidGpuView2) && head[1] == 2)
            std::memcpy(&v2, in.view, sizeof v2);
        else
            fail("fluids: NP_FluidGpuView size %u version %u (96 B version 1 or 136 B version 2)", head[0], head[1]);
        const FluidGpuView& v = v2.view;
        if (head[1] == 1) std::copy(std::begin(v.origin), std::end(v.origin), v2.startOrigin);  // not anchored
        if (!std::isfinite(v2.startOrigin[0]) || !std::isfinite(v2.startOrigin[1]) || !std::isfinite(v2.startOrigin[2]) || !std::isfinite(v2.frameVelocity[0]) ||
            !std::isfinite(v2.frameVelocity[1]) || !std::isfinite(v2.frameVelocity[2]))
            fail("fluids: NP_FluidGpuView2 start origin or frame velocity not finite");
        if (!v.current || !v.count || v.stride < 48 || v.stride % 4 || !(v.dx > 0)) fail("fluids: a view without particles (count %u, stride %u: >= 48 and a multiple of 4, dx %g)", v.count, v.stride, v.dx);
        if (!(in.alpha >= 0 && in.alpha <= 1)) fail("fluids: alpha %g outside [0, 1]", in.alpha);
        for (void* resource : { v.current, v.startValid ? v.start : nullptr })  // the particles must live on this device
        {
            if (!resource) continue;
            ComPtr<ID3D12Device> owner;
            if (FAILED(static_cast<ID3D12Resource*>(resource)->GetDevice(IID_PPV_ARGS(&owner))) || owner.Get() != m_device->d3d())
                fail("fluids: the particle buffers belong to another device (the physics GPU work runs on another renderer's bridge)");
        }
        FramePacket::Fluid f;
        f.frame.current = static_cast<ID3D12Resource*>(v.current);
        f.frame.start = v.startValid ? static_cast<ID3D12Resource*>(v.start) : nullptr;
        f.frame.count = v.count;
        f.frame.startCount = v.startCount;
        f.frame.stride = v.stride;
        f.frame.startValid = v.startValid && v.start ? 1u : 0u;
        // world -> this frame's coordinates (the origin shifts the host applied so far, whole 1024 m steps)
        f.frame.origin[0] = v.origin[0] - m_mainOriginOffset.x;
        f.frame.origin[1] = v.origin[1] - m_mainOriginOffset.y;
        f.frame.origin[2] = v.origin[2] - m_mainOriginOffset.z;
        f.frame.startOrigin[0] = v2.startOrigin[0] - m_mainOriginOffset.x;
        f.frame.startOrigin[1] = v2.startOrigin[1] - m_mainOriginOffset.y;
        f.frame.startOrigin[2] = v2.startOrigin[2] - m_mainOriginOffset.z;
        std::copy(std::begin(v2.frameVelocity), std::end(v2.frameVelocity), f.frame.frameVelocity);
        f.frame.dx = v.dx;
        f.frame.alpha = in.alpha;
        f.frame.tick = v.tick;
        std::memcpy(f.frame.domainCells, in.domainCells, sizeof f.frame.domainCells);
        if (in.material >= m_hostMaterials || m_hostMaterialClasses[in.material] != scene::MaterialClass::Water)
            fail("fluids: material %u is not a Water-class material", in.material);
        f.frame.material = in.material;
        f.currentResource = v.currentResource;
        f.startResource = v.startResource;
        list->push_back(f);
    }
    m_fluids = list->empty() ? nullptr : std::shared_ptr<const std::vector<FramePacket::Fluid>>(std::move(list));
    std::copy(std::begin(stamp), std::end(stamp), m_fluidStamp.begin());
}

void HostRenderer::setOcean(const OceanInput* ocean)
{
    requireCommitted();
    if (ocean)
    {
        const OceanInput& o = *ocean;
        const bool finite = std::isfinite(o.windSpeed) && std::isfinite(o.windDirection) && std::isfinite(o.fetch) && std::isfinite(o.spread) && std::isfinite(o.level) &&
                            std::isfinite(o.lakeCentre[0]) && std::isfinite(o.lakeCentre[1]);
        if (!finite || !(o.windSpeed >= 0) || !(o.fetch > 0) || !(o.spread >= 0) || !(o.horizontalBound > 0) || !(o.verticalBound > 0) || (o.lake && !(o.lakeRadius > 0)))
            fail("ocean: wind speed %g (>= 0), fetch %g (> 0), spread %g (>= 0), bounds %g %g (> 0), lake radius %g (> 0 for a lake)", o.windSpeed, o.fetch, o.spread,
                 o.horizontalBound, o.verticalBound, o.lakeRadius);
    }
    std::lock_guard lock(m_mutex);
    m_ocean = ocean ? std::optional<OceanInput>(*ocean) : std::nullopt;
}

void HostRenderer::setClouds(const render::CloudLayerDesc& c)
{
    requireCommitted();
    const bool finite = std::isfinite(c.coverage) && std::isfinite(c.baseAltitude) && std::isfinite(c.topAltitude) && std::isfinite(c.sigmaMax) &&
                        std::isfinite(c.albedo) && std::isfinite(c.windX) && std::isfinite(c.windZ);
    if (!finite || c.coverage < 0 || c.coverage > 1 || (c.coverage > 0 && !(c.topAltitude > c.baseAltitude && c.sigmaMax > 0 && c.albedo >= 0 && c.albedo <= 1)))
        fail("clouds: coverage %g in [0, 1]; with coverage: base %g < top %g, sigma_max %g > 0, albedo %g in [0, 1]", c.coverage, c.baseAltitude, c.topAltitude, c.sigmaMax,
             c.albedo);
    // the cirrus sheet (with or without the layer)
    const bool cirrusFinite = std::isfinite(c.cirrusCoverage) && std::isfinite(c.cirrusAltitude) && std::isfinite(c.cirrusOpticalDepth) &&
                              std::isfinite(c.cirrusWindX) && std::isfinite(c.cirrusWindZ);
    if (!cirrusFinite || c.cirrusCoverage < 0 || c.cirrusCoverage > 1 || (c.cirrusCoverage > 0 && !(c.cirrusAltitude > 0 && c.cirrusOpticalDepth > 0)))
        fail("clouds: cirrus coverage %g in [0, 1]; with coverage: altitude %g > 0, optical depth %g > 0", c.cirrusCoverage, c.cirrusAltitude, c.cirrusOpticalDepth);
    std::lock_guard lock(m_mutex);
    m_clouds = c;
}

void HostRenderer::setFogVolumes(const std::vector<render::FogVolumeDesc>& volumes)
{
    requireCommitted();
    for (size_t i = 0; i < volumes.size(); ++i)
    {
        const render::FogVolumeDesc& v = volumes[i];
        bool ok = std::isfinite(v.yaw) && std::isfinite(v.density) && std::isfinite(v.heightFalloff) && std::isfinite(v.edge) && v.shape <= 1 && v.density >= 0 &&
                  v.heightFalloff >= 0 && v.edge > 0 && v.edge <= 1;
        for (int k = 0; k < 3; ++k) ok = ok && std::isfinite(v.centre[k]) && std::isfinite(v.halfSize[k]) && v.halfSize[k] > 0 && v.albedo[k] >= 0 && v.albedo[k] <= 1;
        if (!ok) fail("fog volume %zu: finite values, half sizes > 0, shape 0 or 1, density and height falloff >= 0, edge in (0, 1], albedo in [0, 1]", i);
        // rising steam (all 0: none)
        const bool steam = std::isfinite(v.sourcePlane) && std::isfinite(v.riseSpeed) && std::isfinite(v.turbulence) && std::isfinite(v.turbulenceScale) &&
                           v.sourcePlane >= 0 && v.sourcePlane <= 0.95f && v.turbulence >= 0 && v.turbulence <= 1 && (v.turbulence == 0 || v.turbulenceScale > 0);
        if (!steam)
            fail("fog volume %zu: source plane %g in [0, 0.95], rise speed %g finite, turbulence %g in [0, 1], turbulence scale %g > 0 with turbulence", i, v.sourcePlane,
                 v.riseSpeed, v.turbulence, v.turbulenceScale);
        // the density grid: all of it or none
        const bool sized = v.gridSize[0] != 0 || v.gridSize[1] != 0 || v.gridSize[2] != 0;
        bool grid = (v.grid != nullptr) == sized;
        for (uint32_t n : v.gridSize) grid = grid && (!sized || (n >= 1 && n <= render::kFogGridMax));
        if (!grid)
            fail("fog volume %zu: density grid %u x %u x %u (each side 1 .. %u with texels; 0, 0, 0 and no texels without)", i, v.gridSize[0], v.gridSize[1], v.gridSize[2],
                 render::kFogGridMax);
    }
    // The grids are the caller's memory: the frames read copies (a packet keeps its own references until it is recorded).
    std::vector<render::FogVolumeDesc> held = volumes;
    std::vector<std::shared_ptr<const std::vector<uint8_t>>> grids(held.size());
    for (size_t i = 0; i < held.size(); ++i)
    {
        render::FogVolumeDesc& v = held[i];
        if (!v.grid) continue;
        grids[i] = std::make_shared<const std::vector<uint8_t>>(v.grid, v.grid + (size_t)v.gridSize[0] * v.gridSize[1] * v.gridSize[2]);
        v.grid = grids[i]->data();
    }
    std::lock_guard lock(m_mutex);
    m_fogVolumes = std::move(held);
    m_fogGrids = std::move(grids);
}

void HostRenderer::setFog(const render::FogDesc& f)
{
    requireCommitted();
    const bool finite = std::isfinite(f.density) && std::isfinite(f.heightFalloff) && std::isfinite(f.height) && std::isfinite(f.albedo[0]) &&
                        std::isfinite(f.albedo[1]) && std::isfinite(f.albedo[2]) && std::isfinite(f.phaseG) && std::isfinite(f.startDistance) &&
                        std::isfinite(f.skyAmount) && std::isfinite(f.noiseAmount) && std::isfinite(f.noiseScale);
    if (!finite || f.density < 0 || f.heightFalloff < 0 || !(f.phaseG > -1 && f.phaseG < 1) || f.startDistance < 0 || f.skyAmount < 0 || f.skyAmount > 1 ||
        f.noiseAmount < 0 || f.noiseAmount > 1 || (f.enabled && f.noiseScale < 1))
        fail("fog: density %g >= 0, falloff %g >= 0, phase g %g in (-1, 1), start %g >= 0, sky amount %g in [0, 1], noise amount %g in [0, 1], noise scale %g >= 1",
             f.density, f.heightFalloff, f.phaseG, f.startDistance, f.skyAmount, f.noiseAmount, f.noiseScale);
    if (!std::isfinite(f.density2) || !std::isfinite(f.heightFalloff2) || !std::isfinite(f.height2) || f.density2 < 0 || f.heightFalloff2 < 0)
        fail("fog: the second layer's density %g >= 0, falloff %g >= 0, height %g finite", f.density2, f.heightFalloff2, f.height2);
    std::lock_guard lock(m_mutex);
    m_fog = f;
}

void HostRenderer::setWeather(const render::WeatherFrame& in)
{
    requireCommitted();
    render::WeatherFrame w = in;
    const float3 d = w.rainDirection;
    const float length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
    const bool finite = std::isfinite(w.rainRate) && std::isfinite(w.wetness) && std::isfinite(w.snowRate) && std::isfinite(w.snowDepth) &&
                        std::isfinite(w.fogDensity) && std::isfinite(w.cloudCover) && std::isfinite(length);
    if (!finite || w.rainRate < 0 || w.wetness < 0 || w.wetness > 1 || w.snowRate < 0 || w.snowDepth < 0 || w.fogDensity < 0 || w.cloudCover < 0 || w.cloudCover > 1 ||
        !(length > 1e-6f))
        fail("weather: rain rate %g >= 0, wetness %g in [0, 1], snow rate %g >= 0, snow depth %g >= 0, cloud cover %g in [0, 1], a rain direction of length > 0",
             w.rainRate, w.wetness, w.snowRate, w.snowDepth, w.cloudCover);
    w.rainDirection = { d.x / length, d.y / length, d.z / length };
    std::lock_guard lock(m_mutex);
    m_weather = w;
}

void HostRenderer::setLightning(const render::LightningDesc& l)
{
    requireCommitted();
    bool ok = std::isfinite(l.intensity) && l.intensity >= 0 && std::isfinite(l.radius) && l.radius > 0;
    for (int k = 0; k < 3; ++k) ok = ok && std::isfinite(l.position[k]) && std::isfinite(l.color[k]) && l.color[k] >= 0;
    if (!ok) fail("lightning: a finite position, intensity %g >= 0, colour >= 0, radius %g > 0", l.intensity, l.radius);
    std::lock_guard lock(m_mutex);
    m_pending.lightning = l;  // (the next queued frame's alone: queueFrame starts the pending packet anew)
}

namespace
{
// Whether world (x, z) lies in the basin (its samples 0..256 per axis, half a sample of slack for rounding).
bool poolContains(const HostRenderer::PoolInput& p, double x, double z)
{
    if (p.shape == 1)
    {
        const double dx = x - p.centre[0], dz = z - p.centre[2];
        return std::sqrt(dx * dx + dz * dz) <= 0.5 * p.sizeX * (1 + 0.5 / 128);  // RoundPool::contains
    }
    const double c = std::cos(double(p.yaw)), s = std::sin(double(p.yaw)), dx = x - p.centre[0], dz = z - p.centre[2];
    const double u = (dx * c - dz * s) / p.sizeX + 0.5, v = (dx * s + dz * c) / p.sizeZ + 0.5, slackU = 0.5 / 256, slackV = 0.5 / 256;
    return u >= -slackU && u <= 1 + slackU && v >= -slackV && v <= 1 + slackV;
}
} // namespace

void HostRenderer::setPools(std::span<const PoolInput> pools)
{
    requireCommitted();
    for (size_t i = 0; i < pools.size(); ++i)
    {
        const PoolInput& p = pools[i];
        const bool finite = std::isfinite(p.sizeX) && std::isfinite(p.sizeZ) && std::isfinite(p.depth) && std::isfinite(p.yaw) && std::isfinite(p.centre[0]) &&
                            std::isfinite(p.centre[1]) && std::isfinite(p.centre[2]);
        if (!p.id || !finite || !(p.sizeX > 0) || !(p.sizeZ > 0) || !(p.depth >= 0) || !(p.surfaceFilm == 0 || p.surfaceFilm == 1) || p.shape > 1)
            fail("pools: basin %zu (id %u): id nonzero, sizes %g x %g (> 0), depth %g (>= 0), film %g (0 or 1), shape %u (0 or 1), finite", i, p.id, p.sizeX, p.sizeZ, p.depth,
                 p.surfaceFilm, p.shape);
        for (size_t j = 0; j < i; ++j)
            if (pools[j].id == p.id) fail("pools: id %u appears twice", p.id);
    }
    std::lock_guard lock(m_mutex);
    m_pools.assign(pools.begin(), pools.end());
    // Sources of basins that are gone are dropped with them.
    std::erase_if(m_pendingPoolSources, [&](const FramePacket::PoolSource& s) { return std::none_of(m_pools.begin(), m_pools.end(), [&](const PoolInput& p) { return p.id == s.pool; }); });
}

void HostRenderer::addPoolSources(std::span<const FramePacket::PoolSource> sources)
{
    requireCommitted();
    std::lock_guard lock(m_mutex);
    for (size_t i = 0; i < sources.size(); ++i)
    {
        const FramePacket::PoolSource& s = sources[i];
        const auto basin = std::find_if(m_pools.begin(), m_pools.end(), [&](const PoolInput& p) { return p.id == s.pool; });
        const render::PoolSourceFrame& f = s.source;
        if (basin == m_pools.end()) fail("pool sources: source %zu names basin %u, which is not in the current set", i, s.pool);
        if (!std::isfinite(f.x) || !std::isfinite(f.z) || !(f.radius > 0) || !std::isfinite(f.radius) || !std::isfinite(f.impulse) || !std::isfinite(f.volume))
            fail("pool sources: source %zu is not finite or has radius %g (> 0)", i, f.radius);
        if (!poolContains(*basin, f.x, f.z)) fail("pool sources: source %zu at (%g, %g) lies outside basin %u", i, f.x, f.z, s.pool);
    }
    m_pendingPoolSources.insert(m_pendingPoolSources.end(), sources.begin(), sources.end());
}

void HostRenderer::poolsLocked(FramePacket& packet)
{
    packet.pools.clear();
    for (const PoolInput& p : m_pools)
    {
        render::PoolFrame f;
        f.id = p.id, f.material = p.material, f.shape = p.shape;
        f.sizeX = p.sizeX, f.sizeZ = p.sizeZ, f.depth = p.depth, f.surfaceFilm = p.surfaceFilm;
        f.centre[0] = p.centre[0] - m_mainOriginOffset.x, f.centre[1] = p.centre[1] - m_mainOriginOffset.y, f.centre[2] = p.centre[2] - m_mainOriginOffset.z;
        f.yaw = p.yaw;
        for (const PoolWeather& w : m_poolWeather)
            if (w.id == p.id)
            {
                f.steamDensity = w.steamDensity, f.steamHeight = w.steamHeight, f.steamRiseSpeed = w.steamRiseSpeed;
                f.steamTurbulence = w.steamTurbulence;
                f.rainExposure = w.rainExposure;
            }
        packet.pools.push_back(f);
    }
    for (FramePacket::PoolSource& s : m_pendingPoolSources)
    {
        s.source.x -= m_mainOriginOffset.x;
        s.source.z -= m_mainOriginOffset.z;
    }
    packet.poolSources = std::move(m_pendingPoolSources);
    m_pendingPoolSources.clear();
}

void HostRenderer::setPoolWeather(std::span<const PoolWeather> pools)
{
    requireCommitted();
    for (size_t i = 0; i < pools.size(); ++i)
    {
        const PoolWeather& w = pools[i];
        const bool finite = std::isfinite(w.steamDensity) && std::isfinite(w.steamHeight) && std::isfinite(w.steamRiseSpeed) && std::isfinite(w.steamTurbulence) &&
                            std::isfinite(w.rainExposure);
        if (!w.id || !finite || w.steamDensity < 0 || (w.steamDensity > 0 && !(w.steamHeight > 0)) || w.steamTurbulence < 0 || w.steamTurbulence > 1 ||
            w.rainExposure < 0 || w.rainExposure > 1)
            fail("pool weather %zu (id %u): id nonzero, steam density %g >= 0 with a height %g > 0, turbulence %g in [0, 1], rain exposure %g in [0, 1]", i, w.id,
                 w.steamDensity, w.steamHeight, w.steamTurbulence, w.rainExposure);
        for (size_t j = 0; j < i; ++j)
            if (pools[j].id == w.id) fail("pool weather: id %u appears twice", w.id);
    }
    std::lock_guard lock(m_mutex);
    m_poolWeather.assign(pools.begin(), pools.end());
}

std::pair<std::vector<render::PoolFrame>, std::vector<FramePacket::PoolSource>> HostRenderer::queuedPools()
{
    std::lock_guard lock(m_mutex);
    FramePacket p;
    const std::vector<FramePacket::PoolSource> keep = m_pendingPoolSources;
    poolsLocked(p);
    m_pendingPoolSources = keep;
    return { p.pools, p.poolSources };
}

std::optional<render::OceanFrame> HostRenderer::oceanFrameLocked() const
{
    if (!m_ocean) return std::nullopt;
    const OceanInput& o = *m_ocean;
    render::OceanFrame f;
    f.windSpeed = o.windSpeed, f.windDirection = o.windDirection, f.fetch = o.fetch, f.spread = o.spread, f.seed = o.seed;
    f.level = (float)(o.level - m_mainOriginOffset.y);
    f.horizontalBound = o.horizontalBound, f.verticalBound = o.verticalBound;
    f.lake = o.lake ? 1u : 0u;
    f.lakeCentre[0] = (float)(o.lakeCentre[0] - m_mainOriginOffset.x), f.lakeCentre[1] = (float)(o.lakeCentre[1] - m_mainOriginOffset.z);
    f.lakeRadius = o.lakeRadius;
    return f;
}

std::optional<render::OceanFrame> HostRenderer::queuedOcean()
{
    std::lock_guard lock(m_mutex);
    return oceanFrameLocked();
}

void HostRenderer::fluidsBeforeExecute(const FramePacket& p)
{
    m_fluidTicket = 0;
    if (!p.fluids || p.fluids->empty()) return;
    std::vector<GpuBridgeHost::GraphicsUse> uses;
    for (const FramePacket::Fluid& f : *p.fluids)
    {
        uses.push_back({ f.frame.current, 0, 0, NRC_GPU_READ });  // buffers decay to COMMON at the bridge's boundaries
        if (f.frame.startValid) uses.push_back({ f.frame.start, 0, 0, NRC_GPU_READ });
    }
    NRC_GpuWorldStamp stamp{};
    stamp.world = p.fluidStamp[0], stamp.world_generation = p.fluidStamp[1], stamp.epoch = p.fluidStamp[2], stamp.tick = p.fluidStamp[3];
    stamp.branch = p.fluidStamp[4], stamp.phase = (uint32_t)p.fluidStamp[5];
    m_fluidTicket = gpuBridge().prepareGraphics(uses, stamp);
}

void HostRenderer::fluidsAfterExecute()
{
    if (!m_fluidTicket) return;
    gpuBridge().commitGraphics(m_fluidTicket, m_device->queue(QueueType::Graphics).lastSignaled());
    m_fluidTicket = 0;
}

uint32_t HostRenderer::viewModelAdd(uint32_t instance, const float3x4& cameraLocal)
{
    requireCommitted();
    checkPose(cameraLocal, "view model add");
    std::lock_guard lock(m_mutex);
    if (instance >= m_hostInstances) fail("view model: instance %u of %u", instance, m_hostInstances);
    for (const auto& e : m_viewModels.entries())
        if (e.live && e.instance == instance) fail("view model: instance %u is already one", instance);
    const uint32_t id = m_viewModels.add(instance, cameraLocal);
    m_pending.viewModelOps.push_back({ FramePacket::ViewModelOp::Add, id, instance, cameraLocal });
    return id;
}

void HostRenderer::viewModelSetPose(uint32_t id, const float3x4& cameraLocal)
{
    requireCommitted();
    checkPose(cameraLocal, "view model pose");
    std::lock_guard lock(m_mutex);
    const auto& e = m_viewModels.entries();
    if (id >= e.size() || !e[id].live) fail("view model %u is not live", id);
    m_viewModels.setPose(id, cameraLocal);
    m_pending.viewModelOps.push_back({ FramePacket::ViewModelOp::SetPose, id, e[id].instance, cameraLocal });
}

void HostRenderer::viewModelRemove(uint32_t id)
{
    requireCommitted();
    std::lock_guard lock(m_mutex);
    const auto& e = m_viewModels.entries();
    if (id >= e.size() || !e[id].live) fail("view model %u is not live", id);
    const uint32_t instance = e[id].instance;
    m_viewModels.remove(id);
    m_viewModels.takeRemovedInstances();  // (the mirror keeps no transforms)
    m_pending.viewModelOps.push_back({ FramePacket::ViewModelOp::Remove, id, instance, {} });
}

void HostRenderer::setDiscontinuity(uint32_t flags)
{
    requireCommitted();
    if (flags & ~(kDiscontinuityRestore | kDiscontinuityCut)) fail("unknown discontinuity bits 0x%x", flags);
    std::lock_guard lock(m_mutex);
    m_pending.discontinuity |= flags;
}

void HostRenderer::setSimulation(uint32_t gpuSimulation)
{
    requireCommitted();
    if (gpuSimulation & ~(kGpuSimulationSoft | kGpuSimulationVfx | kGpuSimulationRigid)) fail("unknown GPU simulation bits 0x%x", gpuSimulation);
    std::lock_guard lock(m_mutex);
    m_pending.gpuSimulation |= gpuSimulation;
}

void HostRenderer::setEnvironment(const scene::Sun& sun, const scene::Atmosphere& atmosphere, std::optional<FramePacket::Wind> wind)
{
    requireCommitted();
    std::lock_guard lock(m_mutex);
    m_pending.sun = sun;
    m_pending.atmosphere = atmosphere;
    if (wind) m_pending.wind = wind;
}

uint64_t HostRenderer::queueFrame(FramePacket packet)
{
    requireCommitted();
    if (packet.width == 0 || packet.height == 0) fail("frame output size is zero");
    std::lock_guard lock(m_mutex);
    const uint64_t ticket = m_nextTicket++;
    packet.ticket = ticket;
    m_lastCamera = packet.camera;  // (B11: the view models of a photo snapshot)
    m_lastTime = packet.time;
    m_haveLastCamera = true;
    packet.sun = std::move(m_pending.sun);
    packet.atmosphere = std::move(m_pending.atmosphere);
    packet.wind = std::move(m_pending.wind);
    packet.discontinuity = m_pending.discontinuity;
    packet.lensAperture = m_lensAperture;
    packet.lensFocus = m_lensFocus;
    packet.whiteBalanceKelvin = m_whiteBalanceKelvin;
    packet.whiteBalanceTint = m_whiteBalanceTint;
    packet.grading = m_grading;
    packet.post = m_post;
    packet.exposureCompensation = m_exposureCompensation;
    packet.displayEncoding = m_displayEncoding;
    packet.displayPaperWhite = m_displayPaperWhite;
    if (m_meshAssetsChanged)
    {
        packet.meshAssets.emplace(m_meshAssetMap.begin(), m_meshAssetMap.end());
        m_meshAssetsChanged = false;
    }
    packet.gpuSimulation = m_pending.gpuSimulation;
    packet.transforms = std::move(m_pending.transforms);
    packet.skeletons = std::move(m_pending.skeletons);
    packet.visibility = std::move(m_pending.visibility);
    packet.morphs = std::move(m_pending.morphs);
    packet.runtime = std::move(m_pending.runtime);
    packet.originShift = m_pending.originShift;
    packet.worldOrigin = m_mainOriginOffset;
    packet.instanceEdits = std::move(m_pending.instanceEdits);
    packet.materialEdits = std::move(m_pending.materialEdits);
    packet.characterEdits = std::move(m_pending.characterEdits);
    packet.inputEdits = std::move(m_pending.inputEdits);
    packet.surfaceDeltas = std::move(m_pending.surfaceDeltas);
    packet.surfaceHalfLives = m_pending.surfaceHalfLives;
    packet.surfaceTime = m_surfaceTime;
    packet.debugLines = std::move(m_pending.debugLines);
    packet.debugTriangles = std::move(m_pending.debugTriangles);
    packet.debugGlyphs = std::move(m_pending.debugGlyphs);
    if (m_decalsChanged) packet.decals = std::make_shared<const decal::DecalSet>(m_decals);
    packet.viewModelOps = std::move(m_pending.viewModelOps);
    packet.hairOps = std::move(m_pending.hairOps);
    packet.hairFraction = m_pending.hairFraction;
    packet.fluids = m_fluids;  // (a state: every frame reads the latest until the host sets another)
    packet.fluidStamp = m_fluidStamp;
    packet.ocean = oceanFrameLocked();  // in this frame's coordinates (the origin shifts applied so far)
    packet.clouds = m_clouds;
    packet.fog = m_fog;
    packet.fogVolumes = m_fogVolumes;
    packet.fogGrids = m_fogGrids;
    packet.weather = m_weather;
    packet.lightning = m_pending.lightning;
    poolsLocked(packet);  // W2: the basins (a state) and this frame's sources (handed over once)
    m_decalsChanged = false;
    m_pending = FramePacket{};
    m_packets.push_back(std::move(packet));
    // A frame the host never renders (camera disabled, event dropped) must not hold its updates back from later
    // frames: its scene changes move into the next queued packet when it is dropped.
    while (m_packets.size() > m_options.framesInFlight + 2)
    {
        FramePacket dropped = std::move(m_packets.front());
        m_packets.pop_front();
        FramePacket& next = m_packets.front();
        if (!next.sun) next.sun = dropped.sun;
        if (!next.atmosphere) next.atmosphere = dropped.atmosphere;
        if (!next.wind) next.wind = dropped.wind;
        next.discontinuity |= dropped.discontinuity;
        next.gpuSimulation |= dropped.gpuSimulation;
        // C9: the dropped packet's transforms are in its coordinates; the next packet's shift moves them.
        for (InstanceTransformUpdate& u : dropped.transforms)
            u.objectToWorld.m[0][3] -= next.originShift.x, u.objectToWorld.m[1][3] -= next.originShift.y, u.objectToWorld.m[2][3] -= next.originShift.z;
        shiftRuntime(dropped.runtime, next.originShift);
        next.originShift = next.originShift + dropped.originShift;
        next.morphs.insert(next.morphs.begin(), std::make_move_iterator(dropped.morphs.begin()), std::make_move_iterator(dropped.morphs.end()));
        next.runtime.insert(next.runtime.begin(), std::make_move_iterator(dropped.runtime.begin()), std::make_move_iterator(dropped.runtime.end()));
        next.transforms.insert(next.transforms.begin(), dropped.transforms.begin(), dropped.transforms.end());
        next.skeletons.insert(next.skeletons.begin(), dropped.skeletons.begin(), dropped.skeletons.end());
        next.visibility.insert(next.visibility.begin(), dropped.visibility.begin(), dropped.visibility.end());
        next.instanceEdits.insert(next.instanceEdits.begin(), std::make_move_iterator(dropped.instanceEdits.begin()), std::make_move_iterator(dropped.instanceEdits.end()));
        next.materialEdits.insert(next.materialEdits.begin(), std::make_move_iterator(dropped.materialEdits.begin()), std::make_move_iterator(dropped.materialEdits.end()));
        next.characterEdits.insert(next.characterEdits.begin(), dropped.characterEdits.begin(), dropped.characterEdits.end());
        next.inputEdits.insert(next.inputEdits.begin(), dropped.inputEdits.begin(), dropped.inputEdits.end());
        next.surfaceDeltas.insert(next.surfaceDeltas.begin(), std::make_move_iterator(dropped.surfaceDeltas.begin()), std::make_move_iterator(dropped.surfaceDeltas.end()));
        if (!next.surfaceHalfLives) next.surfaceHalfLives = dropped.surfaceHalfLives;
        if (!next.decals) next.decals = dropped.decals;
        next.viewModelOps.insert(next.viewModelOps.begin(), dropped.viewModelOps.begin(), dropped.viewModelOps.end());
        next.hairOps.insert(next.hairOps.begin(), std::make_move_iterator(dropped.hairOps.begin()), std::make_move_iterator(dropped.hairOps.end()));
        if (!next.hairFraction) next.hairFraction = dropped.hairFraction;
        for (FramePacket::PoolSource& s : dropped.poolSources) s.source.x -= next.originShift.x, s.source.z -= next.originShift.z;
        next.poolSources.insert(next.poolSources.begin(), dropped.poolSources.begin(), dropped.poolSources.end());
    }
    return ticket;
}

std::optional<FramePacket> HostRenderer::takePacket(uint64_t ticket)
{
    std::lock_guard lock(m_mutex);
    // Older packets were never rendered: their updates carry into the one rendered now (applied first).
    FramePacket carried;
    bool haveCarried = false;
    while (!m_packets.empty() && m_packets.front().ticket < ticket)
    {
        FramePacket& old = m_packets.front();
        if (old.sun) carried.sun = old.sun;
        if (old.atmosphere) carried.atmosphere = old.atmosphere;
        if (old.wind) carried.wind = old.wind;
        carried.discontinuity |= old.discontinuity;
        carried.gpuSimulation |= old.gpuSimulation;
        // C9: transforms carried so far are in older coordinates; this packet's shift moves them.
        for (InstanceTransformUpdate& u : carried.transforms)
            u.objectToWorld.m[0][3] -= old.originShift.x, u.objectToWorld.m[1][3] -= old.originShift.y, u.objectToWorld.m[2][3] -= old.originShift.z;
        shiftRuntime(carried.runtime, old.originShift);
        carried.originShift = carried.originShift + old.originShift;
        carried.morphs.insert(carried.morphs.end(), std::make_move_iterator(old.morphs.begin()), std::make_move_iterator(old.morphs.end()));
        carried.runtime.insert(carried.runtime.end(), std::make_move_iterator(old.runtime.begin()), std::make_move_iterator(old.runtime.end()));
        carried.transforms.insert(carried.transforms.end(), old.transforms.begin(), old.transforms.end());
        carried.skeletons.insert(carried.skeletons.end(), std::make_move_iterator(old.skeletons.begin()), std::make_move_iterator(old.skeletons.end()));
        carried.visibility.insert(carried.visibility.end(), old.visibility.begin(), old.visibility.end());
        carried.instanceEdits.insert(carried.instanceEdits.end(), std::make_move_iterator(old.instanceEdits.begin()), std::make_move_iterator(old.instanceEdits.end()));
        carried.materialEdits.insert(carried.materialEdits.end(), std::make_move_iterator(old.materialEdits.begin()), std::make_move_iterator(old.materialEdits.end()));
        carried.characterEdits.insert(carried.characterEdits.end(), old.characterEdits.begin(), old.characterEdits.end());
        carried.inputEdits.insert(carried.inputEdits.end(), old.inputEdits.begin(), old.inputEdits.end());
        carried.surfaceDeltas.insert(carried.surfaceDeltas.end(), std::make_move_iterator(old.surfaceDeltas.begin()), std::make_move_iterator(old.surfaceDeltas.end()));
        if (old.surfaceHalfLives) carried.surfaceHalfLives = old.surfaceHalfLives;
        if (old.decals) carried.decals = old.decals;
        carried.viewModelOps.insert(carried.viewModelOps.end(), old.viewModelOps.begin(), old.viewModelOps.end());
        carried.hairOps.insert(carried.hairOps.end(), std::make_move_iterator(old.hairOps.begin()), std::make_move_iterator(old.hairOps.end()));
        if (old.hairFraction) carried.hairFraction = old.hairFraction;
        for (FramePacket::PoolSource& s : carried.poolSources) s.source.x -= old.originShift.x, s.source.z -= old.originShift.z;
        carried.poolSources.insert(carried.poolSources.end(), old.poolSources.begin(), old.poolSources.end());
        haveCarried = true;
        m_packets.pop_front();
    }
    if (m_packets.empty() || m_packets.front().ticket != ticket) return std::nullopt;
    FramePacket p = std::move(m_packets.front());
    m_packets.pop_front();
    if (haveCarried)
    {
        if (!p.sun) p.sun = carried.sun;
        if (!p.atmosphere) p.atmosphere = carried.atmosphere;
        if (!p.wind) p.wind = carried.wind;
        p.discontinuity |= carried.discontinuity;
        p.gpuSimulation |= carried.gpuSimulation;
        for (InstanceTransformUpdate& u : carried.transforms)
            u.objectToWorld.m[0][3] -= p.originShift.x, u.objectToWorld.m[1][3] -= p.originShift.y, u.objectToWorld.m[2][3] -= p.originShift.z;
        shiftRuntime(carried.runtime, p.originShift);
        p.originShift = p.originShift + carried.originShift;
        p.morphs.insert(p.morphs.begin(), std::make_move_iterator(carried.morphs.begin()), std::make_move_iterator(carried.morphs.end()));
        p.runtime.insert(p.runtime.begin(), std::make_move_iterator(carried.runtime.begin()), std::make_move_iterator(carried.runtime.end()));
        p.transforms.insert(p.transforms.begin(), carried.transforms.begin(), carried.transforms.end());
        p.skeletons.insert(p.skeletons.begin(), std::make_move_iterator(carried.skeletons.begin()), std::make_move_iterator(carried.skeletons.end()));
        p.visibility.insert(p.visibility.begin(), carried.visibility.begin(), carried.visibility.end());
        p.instanceEdits.insert(p.instanceEdits.begin(), std::make_move_iterator(carried.instanceEdits.begin()), std::make_move_iterator(carried.instanceEdits.end()));
        p.materialEdits.insert(p.materialEdits.begin(), std::make_move_iterator(carried.materialEdits.begin()), std::make_move_iterator(carried.materialEdits.end()));
        p.characterEdits.insert(p.characterEdits.begin(), carried.characterEdits.begin(), carried.characterEdits.end());
        p.inputEdits.insert(p.inputEdits.begin(), carried.inputEdits.begin(), carried.inputEdits.end());
        p.surfaceDeltas.insert(p.surfaceDeltas.begin(), std::make_move_iterator(carried.surfaceDeltas.begin()), std::make_move_iterator(carried.surfaceDeltas.end()));
        if (!p.surfaceHalfLives) p.surfaceHalfLives = carried.surfaceHalfLives;
        if (!p.decals) p.decals = carried.decals;
        p.viewModelOps.insert(p.viewModelOps.begin(), carried.viewModelOps.begin(), carried.viewModelOps.end());
        p.hairOps.insert(p.hairOps.begin(), std::make_move_iterator(carried.hairOps.begin()), std::make_move_iterator(carried.hairOps.end()));
        if (!p.hairFraction) p.hairFraction = carried.hairFraction;
        for (FramePacket::PoolSource& s : carried.poolSources) s.source.x -= p.originShift.x, s.source.z -= p.originShift.z;
        p.poolSources.insert(p.poolSources.begin(), carried.poolSources.begin(), carried.poolSources.end());
    }
    // Leaving the queue: its updates join the applied state here, under the queue's lock, so currentScene never misses
    // a packet between the queue and the GPU. Its scene edits join the scene (the GPU scene's source) at the same point;
    // beginFrame hands them to the GPU scene.
    std::lock_guard applied(m_appliedMutex);
    applyEdits(p, m_scene);
    overlay(p, m_applied);
    return p;
}

void HostRenderer::overrideQuality(const std::string& assignment)
{
    requireOpen();
    // the render threads read the quality config from commit on: a later change would race them
    if (m_committed) fail("overrideQuality('%s'): quality overrides are applied before commit", assignment.c_str());
    m_quality.applyOverride(assignment);
}

float HostRenderer::lastEv100ForTest() const { return m_frameRenderer ? m_frameRenderer->lastEv100() : 0.0f; }

render::TrackState& HostRenderer::trackStateForTest()
{
    requireCommitted();
    return m_frameRenderer->trackState();
}

uint32_t HostRenderer::debugErrors() { return m_device->drainDebugMessages(); }

void HostRenderer::removeDeviceForTest()
{
    ComPtr<ID3D12Device5> device;
    check(m_device->d3d()->QueryInterface(IID_PPV_ARGS(&device)), "ID3D12Device5 for RemoveDevice");
    device->RemoveDevice();
}

FrameStats HostRenderer::latestStats() const
{
    std::lock_guard lock(m_mutex);
    return m_stats;
}

uint32_t HostRenderer::beginFrame(const FramePacket& p)
{
    const uint64_t frame = m_recordedFrames;
    const uint32_t slot = (uint32_t)(frame % m_options.framesInFlight);
    for (uint32_t q = 0; q < kQueueTypeCount; ++q) m_device->queue((QueueType)q).waitCpu(m_slotFence[slot][q]);
    m_profiler->beginFrame(frame);
    if (const FrameTiming* t = m_profiler->lastCompleted())
    {
        std::lock_guard lock(m_mutex);
        m_stats.frameIndex = m_slotHostFrame[t->frame % m_options.framesInFlight];
        m_stats.graph = m_slotGraph[t->frame % m_options.framesInFlight];
        for (uint32_t q = 0; q < 2; ++q) m_stats.queues[q] = { t->queues[q].lists, t->queues[q].headMs, t->queues[q].tailMs, t->queues[q].gapMs };
        m_stats.gpuMs = t->gpuFrameMs;
        m_stats.passes = (uint32_t)t->passes.size();
        m_stats.passMs.clear();
        for (const PassTiming& pt : t->passes) m_stats.passMs.emplace_back(pt.name, pt.durationMs());
    }
    m_slotHostFrame[slot] = p.frameIndex;
    // Scene changes of this frame (GpuScene uploads them at the start of FrameRenderer::record).
    if (p.sun || p.atmosphere || p.wind)
    {
        std::lock_guard lock(m_appliedMutex);
        if (p.sun) m_scene.sun = *p.sun;
        if (p.atmosphere) m_scene.atmosphere = *p.atmosphere;
        if (p.wind)
        {
            m_scene.windDirection = p.wind->direction;
            m_scene.windSpeed = p.wind->speed;
        }
    }
    // Scene edits (the scene already has them, takePacket): materials first, since new instances may override with them.
    if (!p.materialEdits.empty() || !p.instanceEdits.empty() || !p.characterEdits.empty() || !p.inputEdits.empty())
    {
        std::vector<uint32_t> materials, instances;
        for (const auto& e : p.materialEdits) materials.push_back(e.first);
        for (const auto& e : p.characterEdits) materials.push_back(e.first);  // (character shading: the material's record again)
        for (const auto& e : p.inputEdits) materials.push_back(e.first);      // (material inputs: likewise)
        for (const auto& e : p.instanceEdits) instances.push_back(e.first);
        auto unique = [](std::vector<uint32_t>& v) {
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
        };
        unique(materials);
        unique(instances);
        if (!materials.empty()) m_gpuScene->setMaterials(materials);
        if (!instances.empty())
        {
            m_gpuScene->setInstances(instances);
            m_gpuTransforms.resize(m_gpuScene->instances().size());
            for (uint32_t i : instances) m_gpuTransforms[i] = m_scene.instances[i].transform;
        }
    }
    // Only changes reach the GPU scene. A host sends every visible instance and pose each frame (a body at rest included),
    // and the GPU scene counts every update as motion (transformRevision / deformRevision += 1): the caster would be
    // listed for the local lights' shadow pages and the static TLAS rebuilt every frame. A bit-identical update changes
    // nothing else: without it the settling rule (GpuScene.h) gives prev = current, as the update itself would, and a
    // teleport to where the instance already is has no motion either way.
    // C9: the origin shift moves the GPU scene (and this dedupe mirror) before the packet's transforms.
    if (p.originShift.x != 0 || p.originShift.y != 0 || p.originShift.z != 0)
    {
        m_gpuScene->rebase(p.originShift);
        for (float3x4& t : m_gpuTransforms) t.m[0][3] -= p.originShift.x, t.m[1][3] -= p.originShift.y, t.m[2][3] -= p.originShift.z;
    }
    m_changedTransforms.clear();
    for (const InstanceTransformUpdate& u : p.transforms)
    {
        if (std::memcmp(&m_gpuTransforms[u.instance], &u.objectToWorld, sizeof(float3x4)) == 0)
        {
            ++m_droppedTransforms;
            continue;
        }
        m_gpuTransforms[u.instance] = u.objectToWorld;
        m_changedTransforms.push_back(u);
    }
    if (!m_changedTransforms.empty()) m_gpuScene->updateTransforms(frame, m_changedTransforms);
    for (const SkeletonPose& s : p.skeletons)
    {
        std::shared_ptr<const std::vector<float3x4>>& last = m_gpuPoses[s.skeleton];
        if (last == s.jointToModel || std::memcmp(last->data(), s.jointToModel->data(), last->size() * sizeof(float3x4)) == 0)
        {
            ++m_droppedPoses;
            continue;
        }
        last = s.jointToModel;
        m_gpuScene->updateSkeleton(frame, s.skeleton, *s.jointToModel);
    }
    for (const auto& [instance, visible] : p.visibility) m_gpuScene->setInstanceVisible(instance, visible);
    for (const FramePacket::Morph& m : p.morphs) m_gpuScene->setMorph(frame, m.instance, m.weights, m.time);  // C4
    // C2b runtime geometry, in call order.
    std::vector<InstanceTransformUpdate> runtimeMoves;
    for (const FramePacket::RuntimeOp& op : p.runtime)
        switch (op.kind)
        {
        case FramePacket::RuntimeOp::AddMesh:
        {
            const uint32_t index = m_gpuScene->addRuntimeMesh(*op.meshData, *op.clusters);
            if (index == gpu::kNone) logf("UnravelNext host: runtime mesh pool full; runtime mesh %u not drawn\n", op.id);
            m_runtimeMeshIndex[op.id] = index;
            break;
        }
        case FramePacket::RuntimeOp::RemoveMesh:
        {
            auto it = m_runtimeMeshIndex.find(op.id);
            if (it != m_runtimeMeshIndex.end() && it->second != gpu::kNone) m_gpuScene->removeRuntimeMesh(it->second);
            if (it != m_runtimeMeshIndex.end()) m_runtimeMeshIndex.erase(it);
            break;
        }
        case FramePacket::RuntimeOp::AddInstance:
        {
            uint32_t mesh = op.mesh;
            if (mesh & 0x80000000u)
            {
                auto it = m_runtimeMeshIndex.find(mesh);
                mesh = it == m_runtimeMeshIndex.end() ? gpu::kNone : it->second;
            }
            uint32_t index = gpu::kNone;
            if (mesh != gpu::kNone)
            {
                scene::Instance in;
                in.mesh = mesh;
                in.transform = op.transform;
                in.flags = op.flags;
                index = m_gpuScene->addRuntimeInstance(in);
                if (index == gpu::kNone) logf("UnravelNext host: runtime instance pool full; runtime instance %u not drawn\n", op.id);
            }
            m_runtimeInstanceIndex[op.id] = index;
            break;
        }
        case FramePacket::RuntimeOp::RemoveInstance:
        {
            auto it = m_runtimeInstanceIndex.find(op.id);
            if (it != m_runtimeInstanceIndex.end() && it->second != gpu::kNone) m_gpuScene->removeRuntimeInstance(it->second);
            if (it != m_runtimeInstanceIndex.end()) m_runtimeInstanceIndex.erase(it);
            break;
        }
        case FramePacket::RuntimeOp::Transform:
        {
            auto it = m_runtimeInstanceIndex.find(op.id);
            if (it != m_runtimeInstanceIndex.end() && it->second != gpu::kNone) runtimeMoves.push_back({ it->second, op.transform, 0 });
            break;
        }
        case FramePacket::RuntimeOp::Patch:  // C5: after the patch meshes of this call (earlier in the list)
            m_gpuScene->setPatchRegion(op.id, *op.region);
            break;
        }
    if (!runtimeMoves.empty()) m_gpuScene->updateTransforms(frame, runtimeMoves);
    return slot;
}

void HostRenderer::recordFrame(const FramePacket& p, TextureRef output)
{
    FrameContext fc;
    fc.frameIndex = m_recordedFrames;
    fc.time = p.time;
    fc.deltaTime = p.deltaTime;
    const ViewDesc current = ViewDesc::fromCamera(p.camera, p.width, p.height, {});
    // Either discontinuity bit: the main view has no previous view (INTERFACES 5.5.2).
    const bool previous = m_havePrev && p.discontinuity == 0;
    fc.mainView = ViewDesc::fromCamera(p.camera, p.width, p.height, previous ? m_prevViewProj : current.viewProj);
    fc.discontinuity = p.discontinuity;
    // A4: a camera without an exposure (NaN EV100, UnxCameraDesc) asks for automatic exposure.
    fc.autoExposure = !std::isfinite(p.camera.ev100);
    fc.displayPeak = p.displayPeak;
    fc.lensAperture = p.lensAperture;
    fc.lensFocus = p.lensFocus;
    fc.whiteBalanceKelvin = p.whiteBalanceKelvin;
    fc.whiteBalanceTint = p.whiteBalanceTint;
    fc.grading = p.grading;
    fc.post = p.post;
    fc.exposureCompensation = p.exposureCompensation;
    fc.displayEncoding = p.displayEncoding;
    fc.displayPaperWhite = p.displayPaperWhite;
    fc.timing = m_profiler ? m_profiler->lastCompleted() : nullptr;  // (the debug HUD, E)
    fc.originShift = p.originShift;  // C9
    for (int a = 0; a < 3; ++a)
    {
        fc.worldOrigin[a] = (double)(&p.worldOrigin.x)[a];
        fc.streamAxes[a] = m_options.streamAxes[a];
    }
    fc.gpuSimulation = p.gpuSimulation;
    std::vector<FluidFrame>& fluidFrames = m_fluidFrames;  // B8: valid until record() returns
    fluidFrames.clear();
    if (p.fluids)
        for (const FramePacket::Fluid& f : *p.fluids) fluidFrames.push_back(f.frame);
    fc.fluids = fluidFrames.empty() ? nullptr : fluidFrames.data();
    fc.fluidCount = (uint32_t)fluidFrames.size();
    fc.ocean = p.ocean ? &*p.ocean : nullptr;
    fc.clouds = p.clouds;
    fc.fog = p.fog;
    fc.fogVolumes = p.fogVolumes;  // (their density grids: p.fogGrids, alive until this frame is recorded)
    fc.weather = p.weather;
    fc.lightning = p.lightning;
    // W2: the basins with their sources grouped (valid until record() returns); sources of basins no longer present drop.
    m_poolFrames = p.pools;
    m_poolSourceFrames.clear();
    std::vector<size_t> firstSource(m_poolFrames.size());
    for (size_t i = 0; i < m_poolFrames.size(); ++i)
    {
        firstSource[i] = m_poolSourceFrames.size();
        for (const FramePacket::PoolSource& s : p.poolSources)
            if (s.pool == m_poolFrames[i].id) m_poolSourceFrames.push_back(s.source);
        m_poolFrames[i].sourceCount = uint32_t(m_poolSourceFrames.size() - firstSource[i]);
    }
    for (size_t i = 0; i < m_poolFrames.size(); ++i)
        m_poolFrames[i].sources = m_poolFrames[i].sourceCount ? m_poolSourceFrames.data() + firstSource[i] : nullptr;
    fc.pools = m_poolFrames.empty() ? nullptr : m_poolFrames.data();
    fc.poolCount = uint32_t(m_poolFrames.size());
    m_lastDiscontinuity = p.discontinuity;
    m_lastGpuSimulation = p.gpuSimulation;
    m_prevViewProj = fc.mainView.viewProj;
    m_havePrev = true;
    // A7: the surface state field this frame samples (E uploads what changed in tracks::surfaceState)
    surface::SurfaceField& field = surface::surfaceField(m_frameRenderer->trackState());
    for (const FramePacket::SurfaceDelta& d : p.surfaceDeltas) field.apply(d.changed.data(), d.changed.size(), d.removed.data(), d.removed.size() / 3);
    if (p.surfaceHalfLives) field.setHalfLives(*p.surfaceHalfLives);
    field.setTime(p.surfaceTime);
    // A7 decals (the host's latest snapshot) and A15 debug primitives of this frame
    if (p.decals) decal::decals(m_frameRenderer->trackState()) = *p.decals;
    // A3 mesh particles (render C): the table of this frame, runtime mesh ids resolved now (a removed one draws nothing)
    if (p.meshAssets) m_meshAssetsRender = *p.meshAssets;
    if (!m_meshAssetsRender.empty() || !fx::meshAssets(m_frameRenderer->trackState()).empty())
    {
        std::vector<fx::MeshAsset>& table = fx::meshAssets(m_frameRenderer->trackState());
        table.clear();
        for (auto [asset, mesh] : m_meshAssetsRender)
        {
            if (mesh & 0x80000000u)
            {
                auto it = m_runtimeMeshIndex.find(mesh);
                mesh = it == m_runtimeMeshIndex.end() ? gpu::kNone : it->second;
            }
            table.push_back({ asset, mesh });
        }
    }
    // A12 view models: the host's operations in order (the same ids as the host's mirror)
    viewmodel::ViewModels& viewModels = viewmodel::viewModels(m_frameRenderer->trackState());
    for (const FramePacket::ViewModelOp& op : p.viewModelOps)
    {
        if (op.kind == FramePacket::ViewModelOp::Add)
        {
            const uint32_t id = viewModels.add(op.instance, op.pose);
            if (id != op.id) fail("view model replay: id %u, the host's %u", id, op.id);
        }
        else if (op.kind == FramePacket::ViewModelOp::SetPose) viewModels.setPose(op.id, op.pose);
        else viewModels.remove(op.id);
    }
    // B10 hair: the host's operations in order (the same body ids as the host's mirror), then the frame's tick fraction
    hair::HairSystem& hairs = hair::hairSystem(m_frameRenderer->trackState());
    for (const FramePacket::HairOp& op : p.hairOps)
    {
        if (op.kind == FramePacket::HairOp::AddBody)
        {
            const uint32_t id = hairs.addBody(*op.desc);
            if (id != op.body) fail("hair replay: body %u, the host's %u", id, op.body);
        }
        else if (op.kind == FramePacket::HairOp::Tick)
            hairs.tick(op.body, op.joints.data(), (uint32_t)op.joints.size(), op.capsules.data(), (uint32_t)op.capsules.size(), op.wind, op.dt);
        else hairs.removeBody(op.body);
    }
    if (p.hairFraction) hairs.setFrameFraction(*p.hairFraction);
    debug::DrawList& draw = debug::drawList(m_frameRenderer->trackState());
    draw.lines.insert(draw.lines.end(), p.debugLines.begin(), p.debugLines.end());
    draw.triangles.insert(draw.triangles.end(), p.debugTriangles.begin(), p.debugTriangles.end());
    draw.glyphs.insert(draw.glyphs.end(), p.debugGlyphs.begin(), p.debugGlyphs.end());
    if (photoFrame(fc, p, output)) return;  // B11: the photo's image instead of the scene
    m_frameRenderer->record(*m_graph, fc, output);
    {
        // W2: the basins' statistics (read back by the record, framesInFlight records behind) for UnxPoolStatsLatest
        std::vector<std::pair<uint32_t, water::PoolStats>> stats;
        water::poolStatsSnapshot(m_frameRenderer->trackState(), stats);
        std::lock_guard lock(m_mutex);
        m_poolStats = std::move(stats);
    }
}

bool HostRenderer::poolStats(uint32_t id, water::PoolStats& out) const
{
    requireCommitted();
    std::lock_guard lock(m_mutex);
    for (const auto& [pool, s] : m_poolStats)
        if (pool == id && s.valid)
        {
            out = s;
            return true;
        }
    return false;
}

void HostRenderer::photoBegin(const scene::Camera& camera, const PhotoSettings& settings)
{
    requireCommitted();
    if (settings.samplesPerPixel < 2 || settings.halfSamplesPerFrame == 0)
        fail("photo: %u samples per pixel (>= 2, both halves), %u per frame and half (>= 1)", settings.samplesPerPixel, settings.halfSamplesPerFrame);
    auto request = std::make_shared<PhotoRequest>();
    request->scene = photoScene();
    request->camera = camera;
    request->settings = settings;
    {
        std::lock_guard lock(m_mutex);
        request->lensAperture = m_lensAperture;
        request->lensFocus = m_lensFocus;
        request->whiteBalanceKelvin = m_whiteBalanceKelvin;
        request->whiteBalanceTint = m_whiteBalanceTint;
        request->time = m_lastTime;
    }
    std::lock_guard lock(m_photoMutex);
    m_photoRequest = std::move(request);
    m_photoSaves.clear();
    m_photoStatus = {};
    m_photoStatus.generation = ++m_photoGeneration;
    m_photoStatus.target = settings.samplesPerPixel;
}

void HostRenderer::photoSave(std::filesystem::path exr, std::filesystem::path png)
{
    if (exr.empty() && png.empty()) fail("photo save: no file");
    std::lock_guard lock(m_photoMutex);
    if (!m_photoRequest) fail("photo save: photo mode is off");
    m_photoSaves.push_back({ std::move(exr), std::move(png) });
}

void HostRenderer::photoEnd()
{
    std::lock_guard lock(m_photoMutex);
    m_photoRequest.reset();
    m_photoSaves.clear();
    m_photoStatus = {};
    m_photoStatus.generation = ++m_photoGeneration;
}

PhotoStatus HostRenderer::photoStatus() const
{
    std::lock_guard lock(m_photoMutex);
    return m_photoStatus;
}

bool HostRenderer::photoFrame(FrameContext& fc, const FramePacket& p, TextureRef output)
{
    std::shared_ptr<const PhotoRequest> request;
    uint64_t generation = 0;
    std::vector<PhotoSaveRequest> saves;
    {
        std::lock_guard lock(m_photoMutex);
        request = m_photoRequest;
        generation = m_photoGeneration;
        saves.swap(m_photoSaves);
    }
    if (!m_photo) m_photo = std::make_unique<Photo>();
    Photo& ph = *m_photo;
    if (generation != ph.generation || (ph.tracer && (ph.width != p.width || ph.height != p.height)))
    {
        ph.tracer.reset();  // (its destructor waits for the device's queues)
        ph.generation = generation;
        ph.failed = false;
    }
    auto leave = [&] {
        // Back to the scene: its histories (GI, exposure, motion) are as old as the photo; the first frame starts them anew.
        if (ph.shown) fc.discontinuity |= kDiscontinuityCut;
        ph.shown = false;
        return false;
    };
    if (!request || ph.failed) return leave();
    auto setStatus = [&](auto&& f) {
        std::lock_guard lock(m_photoMutex);
        if (m_photoStatus.generation == generation) f(m_photoStatus);
    };
    if (!ph.tracer)
    {
        const auto t0 = std::chrono::steady_clock::now();
        try
        {
            ph.tracer = std::make_unique<reference::GpuPathTracer>(request->scene, *m_device, m_options.shaderDirectory / "Reference", "photo");
            const scene::Camera& cam = request->camera;
            reference::ResolvedCamera c;
            c.position = cam.position;
            c.forward = cam.forward;
            c.up = cam.up;
            c.verticalFov = cam.verticalFov;
            c.nearPlane = cam.nearPlane;
            c.ev100 = std::isfinite(cam.ev100) ? cam.ev100 : m_frameRenderer->lastEv100();  // automatic exposure's last choice
            c.time = (float)request->time;
            c.lensAperture = request->lensAperture;
            c.lensFocus = request->lensFocus;
            reference::RenderSettings settings;
            settings.width = p.width;
            settings.height = p.height;
            settings.samplesPerPixel = request->settings.samplesPerPixel;
            settings.russianRouletteStart = (uint32_t)m_quality.number("reference.russian_roulette_start_bounce");
            ph.tracer->start(c, settings);
            ph.width = p.width;
            ph.height = p.height;
            ph.target = request->settings.samplesPerPixel;
        }
        catch (const std::exception& e)
        {
            ph.tracer.reset();
            ph.failed = true;
            logf("UnravelNext: photo mode could not start: %s\n", e.what());
            setStatus([&](PhotoStatus& s) { s.error = e.what(); });
            return leave();
        }
        const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        setStatus([&](PhotoStatus& s) {
            s.startSeconds = seconds;
            s.width = p.width;
            s.height = p.height;
        });
    }
    if (ph.tracer->samplesDone() < ph.target) ph.tracer->pass(request->settings.halfSamplesPerFrame);
    // The image is rewritten on the compute queue: after every earlier frame's read of it on the graphics queue.
    Queue& graphics = m_device->queue(QueueType::Graphics);
    m_device->queue(QueueType::Compute).waitGpu(graphics, graphics.lastSignaled());
    ID3D12Resource* image = ph.tracer->currentImageResource();
    TextureDesc id;
    id.name = "photo image";
    id.width = p.width;
    id.height = p.height;
    id.format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    const TextureRef imageRef = m_graph->importTexture(image, id, D3D12_BARRIER_LAYOUT_COMMON);

    // Saves: the linear image now (a read-back of the accumulation), the display encoding with this frame.
    TextureRef sdr;
    std::string error;
    double relMse = -1;
    uint32_t exrOnly = 0;
    ph.pngs.clear();
    if (!saves.empty())
    {
        try
        {
            const reference::RenderOutput out = ph.tracer->current();
            relMse = out.halvesRelMse / 4;
            for (const PhotoSaveRequest& s : saves)
            {
                if (!s.exr.empty()) metrics::writeExr(s.exr, out.image);
                if (!s.png.empty()) ph.pngs.push_back(s.png);
                else ++exrOnly;
            }
        }
        catch (const std::exception& e)
        {
            error = e.what();
            ph.pngs.clear();
        }
    }
    if (!ph.pngs.empty())
    {
        if (!ph.sdr || ph.sdrWidth != p.width || ph.sdrHeight != p.height)
        {
            m_device->waitIdle();  // (a size change: the old pair may still be in use)
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC1 td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = p.width;
            td.Height = p.height;
            td.DepthOrArraySize = td.MipLevels = 1;
            td.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
            td.SampleDesc.Count = 1;
            td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            ph.sdr.Reset();
            ph.readback.Reset();
            check(m_device->d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &td, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr, nullptr, 0, nullptr,
                                                           IID_PPV_ARGS(&ph.sdr)),
                  "photo SDR image");
            ph.sdr->SetName(L"UnravelNext photo SDR");
            const D3D12_RESOURCE_DESC legacy = ph.sdr->GetDesc();
            UINT64 total = 0;
            m_device->d3d()->GetCopyableFootprints(&legacy, 0, 1, 0, &ph.footprint, nullptr, nullptr, &total);
            D3D12_HEAP_PROPERTIES rheap{ D3D12_HEAP_TYPE_READBACK };
            D3D12_RESOURCE_DESC bd{};
            bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = total;
            bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
            bd.SampleDesc.Count = 1;
            bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            check(m_device->d3d()->CreateCommittedResource(&rheap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&ph.readback)),
                  "photo read-back");
            ph.readbackBytes = total;
            ph.sdrWidth = p.width;
            ph.sdrHeight = p.height;
        }
        TextureDesc sd;
        sd.name = "photo SDR";
        sd.width = p.width;
        sd.height = p.height;
        sd.format = DXGI_FORMAT_R10G10B10A2_UNORM;
        sdr = m_graph->importTexture(ph.sdr.Get(), sd, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    }
    m_frameRenderer->recordImage(*m_graph, fc, imageRef, output, sdr);
    if (sdr.valid())
    {
        BufferDesc bd;
        bd.name = "photo read-back";
        bd.size = ph.readbackBytes;
        const BufferRef rb = m_graph->importBuffer(ph.readback.Get(), bd);
        Photo* photo = &ph;
        m_graph->addPass(
            "host.photo.readback", QueueType::Graphics,
            [&](PassBuilder& b) {
                b.use(sdr, Use::CopySrc);
                b.use(rb, Use::CopyDst);
                b.keep();
            },
            [photo](PassContext& c) {
                D3D12_TEXTURE_COPY_LOCATION dst{};
                dst.pResource = photo->readback.Get();
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint = photo->footprint;
                D3D12_TEXTURE_COPY_LOCATION src{};
                src.pResource = photo->sdr.Get();
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                c.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            });
    }
    ph.shown = true;
    const uint32_t samples = ph.tracer->samplesDone();
    setStatus([&](PhotoStatus& s) {
        s.active = true;
        s.samples = samples;
        if (relMse >= 0) s.relMse = relMse;
        s.saves += exrOnly;
        if (!error.empty()) s.error = "photo save: " + error;
    });
    return true;
}

void HostRenderer::photoAfterExecute()
{
    if (!m_photo || m_photo->pngs.empty()) return;
    Photo& ph = *m_photo;
    Queue& graphics = m_device->queue(QueueType::Graphics);
    graphics.waitCpu(graphics.lastSignaled());
    std::string error;
    uint32_t written = 0;
    try
    {
        std::vector<uint16_t> rgb((size_t)ph.sdrWidth * ph.sdrHeight * 3);
        const uint8_t* mapped = nullptr;
        D3D12_RANGE range{ 0, (SIZE_T)ph.readbackBytes };
        check(ph.readback->Map(0, &range, (void**)&mapped), "photo read-back Map");
        for (uint32_t y = 0; y < ph.sdrHeight; ++y)
        {
            const uint32_t* row = (const uint32_t*)(mapped + ph.footprint.Offset + (size_t)y * ph.footprint.Footprint.RowPitch);
            for (uint32_t x = 0; x < ph.sdrWidth; ++x)
                for (uint32_t c = 0; c < 3; ++c)  // the 10-bit code v as v / 1023 of the 16-bit range, rounded
                    rgb[((size_t)y * ph.sdrWidth + x) * 3 + c] = (uint16_t)((((row[x] >> (10 * c)) & 1023u) * 65535u + 511u) / 1023u);
        }
        D3D12_RANGE none{ 0, 0 };
        ph.readback->Unmap(0, &none);
        for (const std::filesystem::path& png : ph.pngs)
        {
            writePng16(png, ph.sdrWidth, ph.sdrHeight, rgb);
            ++written;
        }
    }
    catch (const std::exception& e)
    {
        error = e.what();
    }
    ph.pngs.clear();
    std::lock_guard lock(m_photoMutex);
    if (m_photoStatus.generation != ph.generation) return;
    m_photoStatus.saves += written;
    if (!error.empty()) m_photoStatus.error = "photo save: " + error;
}

ID3D12Device* HostRenderer::d3dDevice() const { return m_device ? m_device->d3d() : nullptr; }

GpuBridgeHost& HostRenderer::gpuBridge()
{
    if (!m_gpuBridge) fail("the renderer has no GPU bridge");
    return *m_gpuBridge;
}

void HostRenderer::endFrame(uint32_t slot, uint64_t)
{
    for (uint32_t q = 0; q < kQueueTypeCount; ++q) m_slotFence[slot][q] = m_graph->lastFence((QueueType)q);
    const RenderGraphStats& g = m_graph->stats();
    m_slotGraph[slot] = { g.livePasses, g.commandLists, g.barrierBatches, g.barriers, g.crossQueueSyncs, g.transientResources, g.planReused,
                          g.transientBytesAliased, g.cpuCompileMs, m_gpuScene->revision() };
    ++m_recordedFrames;
    std::lock_guard lock(m_mutex);
    m_stats.cpuRecordMs = m_graph->stats().cpuRecordMs;
    m_stats.cpuSubmitMs = m_graph->stats().cpuSubmitMs;
}

fx::ParticleSystem& HostRenderer::fxModule()
{
    requireCommitted();
    fx::ParticleSystem& p = fx::particles(m_frameRenderer->trackState(), *m_device, m_quality);
    p.setExplicitQueue(QueueType::Compute);  // main-thread copies never touch the host's graphics queue
    return p;
}

void HostRenderer::fxRunPending()
{
    fx::ParticleSystem& p = fxModule();
    if (p.pendingTicks() == 0) return;
    Queue& graphics = m_device->queue(QueueType::Graphics);
    Queue& compute = m_device->queue(QueueType::Compute);
    // After every frame submitted so far (they read the state these ticks overwrite), before any later frame.
    compute.waitGpu(graphics, graphics.lastSignaled());
    if (!m_simGraph)
    {
        m_simGraph = std::make_unique<RenderGraph>(*m_device);
        m_simGraph->setAsyncCompute(true);  // (off, every pass would go to the graphics queue)
    }
    m_vfxImmediateTicks += p.pendingTicks();
    p.record(*m_simGraph, *m_shaders, m_simIndex++, QueueType::Compute);
    m_simGraph->execute(nullptr);
    m_simFence = compute.lastSignaled();
}

void HostRenderer::fxFrameWait()
{
    if (m_simFence <= m_simWaited) return;
    m_device->queue(QueueType::Graphics).waitGpu(m_device->queue(QueueType::Compute), m_simFence);
    m_simWaited = m_simFence;
}

void HostRenderer::vfxSubmit(const uint8_t* packet, uint64_t bytes)
{
    std::lock_guard lock(m_fxMutex);
    try
    {
        // A frame records every pending tick into one graph, and a tick reuses the readback slot of the tick one ring
        // before it: more pending ticks than the ring holds would wait on a slot of the same unexecuted graph. Ticks
        // submitted without frames or readbacks in between (a hitch, a long fixed-step catch-up) run at once on the
        // compute queue before the ring fills.
        fx::ParticleSystem& p = fxModule();
        if (p.pendingTicks() + 1 >= p.readbackSlots()) fxRunPending();
        // Diagnostic capture (UNX_FX_RECORD=<existing directory>): every packet as packet_<n>.bin in submission order, so
        // a game's stream replays natively (Host Tests/HostParticleLight --stream).
        char dir[1024];
        const DWORD n = GetEnvironmentVariableA("UNX_FX_RECORD", dir, sizeof dir);
        if (n > 0 && n < sizeof dir)
        {
            char name[64];
            std::snprintf(name, sizeof name, "packet_%06llu.bin", (unsigned long long)m_fxRecorded++);
            std::ofstream f(std::filesystem::path(std::string(dir, n)) / name, std::ios::binary);
            f.write(reinterpret_cast<const char*>(packet), (std::streamsize)bytes);
        }
        p.submit(packet, bytes);
    }
    catch (const std::exception& e)
    {
        // The commit must not fail (NV_StreamExecutor contract): the next readback reports it.
        if (m_vfxError.empty()) m_vfxError = e.what();
    }
}

const fx::TickReadback& HostRenderer::vfxReadback(uint64_t stream, uint64_t generation, uint64_t tick)
{
    std::lock_guard lock(m_fxMutex);
    if (!m_vfxError.empty()) fail("FX stream executor: %s", m_vfxError.c_str());
    fxRunPending();
    m_vfxReadback = fxModule().readback(stream, generation, tick);
    return m_vfxReadback;
}

const std::vector<NV_StreamParticle>& HostRenderer::vfxCheckpoint(uint64_t stream, uint64_t generation, uint64_t tick)
{
    std::lock_guard lock(m_fxMutex);
    if (!m_vfxError.empty()) fail("FX stream executor: %s", m_vfxError.c_str());
    fxRunPending();
    fx::ParticleSystem& p = fxModule();
    if (p.latestTick() != tick) fail("FX stream executor: checkpoint of tick %llu, the latest is %llu", (unsigned long long)tick, (unsigned long long)p.latestTick());
    (void)stream;
    (void)generation;
    m_vfxCheckpoint = p.checkpoint(*m_shaders);
    return m_vfxCheckpoint;
}

const std::vector<NV_StreamParticleOrientation>& HostRenderer::vfxCheckpointOrientations(uint64_t stream, uint64_t generation, uint64_t tick)
{
    std::lock_guard lock(m_fxMutex);
    if (!m_vfxError.empty()) fail("FX stream executor: %s", m_vfxError.c_str());
    fx::ParticleSystem& p = fxModule();
    if (p.latestTick() != tick) fail("FX stream executor: checkpoint orientations of tick %llu, the latest is %llu", (unsigned long long)tick, (unsigned long long)p.latestTick());
    (void)stream;
    (void)generation;
    m_vfxCheckpointOrientations = p.checkpointOrientations(*m_shaders);
    if (m_vfxCheckpointOrientations.size() != m_vfxCheckpoint.size()) fail("FX stream executor: %zu orientations for %zu checkpoint records", m_vfxCheckpointOrientations.size(), m_vfxCheckpoint.size());
    return m_vfxCheckpointOrientations;
}

void HostRenderer::renderOnHost(uint64_t ticket, const HostExecute& execute)
{
    if (m_options.standalone) fail("renderOnHost on a standalone renderer");
    requireCommitted();
    std::lock_guard fxLock(m_fxMutex);  // the frame's C0 ticks and particle pass use the module
    std::optional<FramePacket> packet = takePacket(ticket);
    if (!packet) return;  // an older ticket already rendered, or dropped: nothing to draw
    const FramePacket& p = *packet;
    if (!p.output) fail("frame %llu has no output texture", (unsigned long long)p.frameIndex);
    const D3D12_RESOURCE_DESC od = p.output->GetDesc();
    if (od.Width != p.width || od.Height != p.height || !(od.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
        fail("output texture is %llux%u flags 0x%x; the frame needs %ux%u with random write", (unsigned long long)od.Width, od.Height, (unsigned)od.Flags, p.width,
             p.height);
    // (an HDR frame may be written straight into the HDR10 swap chain's 10-bit format: the chain then requires the
    // ST 2084 encoding - the frame's, or the quality file's output.hdr_encoding - and says so otherwise)
    const bool tenBitOutput = od.Format == DXGI_FORMAT_R10G10B10A2_UNORM || od.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS;
    const DXGI_FORMAT format = p.displayPeak > 0 && !tenBitOutput ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R10G10B10A2_UNORM;
    if (od.Format != format && !(format == DXGI_FORMAT_R10G10B10A2_UNORM && od.Format == DXGI_FORMAT_R10G10B10A2_TYPELESS) &&
        !(format == DXGI_FORMAT_R16G16B16A16_FLOAT && od.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS))
        fail("output texture format %u; the frame needs %s", (unsigned)od.Format,
             p.displayPeak > 0 ? "R16G16B16A16 FLOAT (HDR display; R10G10B10A2 UNORM too under the ST 2084 encoding)" : "R10G10B10A2 UNORM");
    const uint32_t slot = beginFrame(p);
    TextureDesc desc;
    desc.name = "host output";
    desc.width = p.width;
    desc.height = p.height;
    desc.format = format;
    // The host executes every list with the output declared UNORDERED_ACCESS before and after, so the graph sees it in
    // that layout at frame start and leaves it there.
    const TextureRef output = m_graph->importTexture(p.output, desc, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    recordFrame(p, output);
    Queue& graphics = m_device->queue(QueueType::Graphics);
    ID3D12Resource* hostOutput = p.output;
    graphics.setExecuteHook([&](ID3D12CommandList* list) { execute(list, hostOutput); });
    try
    {
        fxFrameWait();  // on the host's queue, in its render event
        fluidsBeforeExecute(p);
        m_graph->execute(m_profiler.get());
        fluidsAfterExecute();
        photoAfterExecute();
    }
    catch (...)
    {
        graphics.setExecuteHook({});
        fluidsAfterExecute();  // the admission holds the bridge's lock until committed (what ran is behind the last signal)
        throw;
    }
    graphics.setExecuteHook({});
    endFrame(slot, p.frameIndex);
}

void HostRenderer::ensureStandaloneOutput(uint32_t width, uint32_t height, DXGI_FORMAT format)
{
    Standalone& s = *m_standalone;
    if (s.output && s.width == width && s.height == height && s.format == format) return;
    m_device->waitIdle();
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = 1;
    d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(m_device->d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr, nullptr, 0, nullptr,
                                                   IID_PPV_ARGS(&s.output)),
          "standalone output");
    s.output->SetName(L"UnravelNext host output");
    const D3D12_RESOURCE_DESC legacy = s.output->GetDesc();
    m_device->d3d()->GetCopyableFootprints(&legacy, 0, 1, 0, &s.footprint, nullptr, nullptr, &s.readbackBytes);
    D3D12_HEAP_PROPERTIES rb{ D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC b{};
    b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    b.Width = s.readbackBytes;
    b.Height = 1;
    b.DepthOrArraySize = 1;
    b.MipLevels = 1;
    b.SampleDesc.Count = 1;
    b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    check(m_device->d3d()->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &b, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s.readback)),
          "standalone readback");
    s.width = width;
    s.height = height;
    s.format = format;
}

void HostRenderer::renderStandalone(uint64_t ticket, void* readback, size_t readbackBytes)
{
    std::lock_guard fxLock(m_fxMutex);
    if (!m_options.standalone) fail("renderStandalone on a renderer bound to the host's device");
    requireCommitted();
    std::optional<FramePacket> packet = takePacket(ticket);
    if (!packet) fail("frame ticket %llu is not queued (dropped or already rendered)", (unsigned long long)ticket);
    const FramePacket& p = *packet;
    // RGB10A2 (4 B per pixel), or RGBA16F (8 B) for an HDR display
    const DXGI_FORMAT format = p.displayPeak > 0 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R10G10B10A2_UNORM;
    const size_t pixelBytes = p.displayPeak > 0 ? 8 : 4;
    if (readback && readbackBytes < (size_t)p.width * p.height * pixelBytes)
        fail("readback buffer holds %zu bytes, needs %zu", readbackBytes, (size_t)p.width * p.height * pixelBytes);
    const void* previousOutput = m_standalone->output.Get();
    if (m_recreateOutput && m_standalone->output)
    {
        m_device->waitIdle();  // the host releases a render target only after the GPU is done with it
        m_standalone->output.Reset();
        m_standalone->width = m_standalone->height = 0;
    }
    ensureStandaloneOutput(p.width, p.height, format);
    if (m_recreateOutput && previousOutput)
    {
        ++m_outputRecreations;
        if (m_standalone->output.Get() == previousOutput) ++m_outputReuses;
    }
    const uint32_t slot = beginFrame(p);

    Standalone& s = *m_standalone;
    TextureDesc od;
    od.name = "host output";
    od.width = p.width;
    od.height = p.height;
    od.format = format;
    const TextureRef output = m_graph->importTexture(s.output.Get(), od, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    recordFrame(p, output);
    if (readback)
    {
        BufferDesc bd;
        bd.name = "host readback";
        bd.size = s.readbackBytes;
        const BufferRef rb = m_graph->importBuffer(s.readback.Get(), bd);
        m_graph->addPass(
            "host.readback", QueueType::Graphics,
            [&](PassBuilder& b) {
                b.use(output, Use::CopySrc);
                b.use(rb, Use::CopyDst);
                b.keep();
            },
            [&s](PassContext& c) {
                D3D12_TEXTURE_COPY_LOCATION dst{};
                dst.pResource = s.readback.Get();
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint = s.footprint;
                D3D12_TEXTURE_COPY_LOCATION src{};
                src.pResource = s.output.Get();
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src.SubresourceIndex = 0;
                c.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            });
    }
    fxFrameWait();
    fluidsBeforeExecute(p);
    try
    {
        m_graph->execute(m_profiler.get());
    }
    catch (...)
    {
        fluidsAfterExecute();
        throw;
    }
    fluidsAfterExecute();
    photoAfterExecute();
    endFrame(slot, p.frameIndex);
    if (readback)
    {
        for (uint32_t q = 0; q < kQueueTypeCount; ++q) m_device->queue((QueueType)q).waitCpu(m_slotFence[slot][q]);
        const uint8_t* mapped = nullptr;
        D3D12_RANGE range{ 0, (SIZE_T)s.readbackBytes };
        check(s.readback->Map(0, &range, (void**)&mapped), "readback Map");
        for (uint32_t y = 0; y < p.height; ++y)
            std::memcpy((uint8_t*)readback + (size_t)y * p.width * pixelBytes, mapped + s.footprint.Offset + (size_t)y * s.footprint.Footprint.RowPitch,
                        (size_t)p.width * pixelBytes);
        D3D12_RANGE none{ 0, 0 };
        s.readback->Unmap(0, &none);
    }
}

// ---- The picture's settings a game changes while it runs (UnxFrameSetColorGrading, UnxFrameSetPost,
// UnxFrameSetDisplayEncoding): validated here, held under m_mutex, copied into every queued frame's packet.
void HostRenderer::setColorGrading(const render::ColorGradingDesc& g)
{
    if (g.enabled)
    {
        bool ok = std::isfinite(g.temperature) && g.temperature >= 1667 && g.temperature <= 25000 && std::isfinite(g.tint) && g.shadowsMax > 0 &&
                  std::isfinite(g.shadowsMax) && std::isfinite(g.highlightsMin) && std::isfinite(g.highlightsMax) && g.highlightsMax > g.highlightsMin;
        for (const render::ColorGradingRange* r : { &g.global, &g.shadows, &g.midtones, &g.highlights })
            for (int c = 0; c < 4; ++c)
                ok = ok && std::isfinite(r->saturation[c]) && r->saturation[c] >= 0 && std::isfinite(r->contrast[c]) && r->contrast[c] >= 0 &&
                     std::isfinite(r->gamma[c]) && r->gamma[c] > 0 && std::isfinite(r->gain[c]) && r->gain[c] >= 0 && std::isfinite(r->offset[c]);
        if (!ok)
            fail("colour grading: temperature %g K in [1667, 25000], finite values, saturation / contrast / gain >= 0, gamma > 0, shadows max %g > 0, "
                 "highlights max %g > min %g",
                 g.temperature, g.shadowsMax, g.highlightsMax, g.highlightsMin);
    }
    std::lock_guard lock(m_mutex);
    m_grading = g;
}

void HostRenderer::setPost(const render::PostSettingsDesc& p, float exposureCompensation)
{
    // (a NaN leaves the value to the quality file; a set value must be one the chain takes)
    auto unsetOr = [](float v, float lo, float hi) { return std::isnan(v) || (v >= lo && v <= hi); };
    const bool range = std::isnan(p.exposureMinEv) || std::isnan(p.exposureMaxEv) || p.exposureMinEv < p.exposureMaxEv;
    if (!std::isfinite(exposureCompensation) || std::abs(exposureCompensation) > 16 || !unsetOr(p.exposureMinEv, -30, 30) || !unsetOr(p.exposureMaxEv, -30, 30) ||
        !range || !unsetOr(p.bloomStrength, 0, 1) || !unsetOr(p.vignette, 0, 1) || !unsetOr(p.motionBlurShutter, 0, 1) ||
        !(p.diaphragmBlades == -1 || p.diaphragmBlades == 0 || (p.diaphragmBlades >= 4 && p.diaphragmBlades <= 16)) || !unsetOr(p.lensFullAperture, 0, 1))
        fail("post settings: exposure compensation %g stops (|c| <= 16), metering range %g .. %g EV100 (min < max, within +-30), bloom %g, vignette %g and "
             "motion blur %g in [0, 1], diaphragm blades %d (-1, 0 or 4 .. 16), full aperture %g m in [0, 1]; NaN leaves a value to the quality file",
             exposureCompensation, p.exposureMinEv, p.exposureMaxEv, p.bloomStrength, p.vignette, p.motionBlurShutter, p.diaphragmBlades, p.lensFullAperture);
    std::lock_guard lock(m_mutex);
    m_post = p;
    m_exposureCompensation = exposureCompensation;
}

void HostRenderer::setDisplayEncoding(int32_t encoding, float paperWhiteNits)
{
    if (encoding < -1 || encoding > 2 || !std::isfinite(paperWhiteNits) || !(paperWhiteNits == 0 || (paperWhiteNits >= 40 && paperWhiteNits <= 1000)))
        fail("display encoding %d (-1 the quality file's, 0 linear, 1 scRGB, 2 ST 2084) and paper white %g cd/m2 (0, or 40 .. 1000)", encoding, paperWhiteNits);
    std::lock_guard lock(m_mutex);
    m_displayEncoding = encoding;
    m_displayPaperWhite = paperWhiteNits;
}
} // namespace unx::host
