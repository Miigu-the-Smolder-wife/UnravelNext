#include "unx/refl/ReflectionSystem.h"

#include "unx/rt/RayPipeline.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <memory>

namespace unx::render::refl
{
namespace
{
uint32_t asU(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

constexpr uint32_t kDescStride = (uint32_t)((sizeof(D3D12_DISPATCH_RAYS_DESC) + 7) / 8 * 8);
constexpr uint32_t kArgumentsBytes = 16 + 2 * kDescStride;
const char* const kTraceLibrary[2] = { "Passes/Reflection/ReflectionTrace.SKY0", "Passes/Reflection/ReflectionTrace.SKY1" };
} // namespace

ReflectionSettings ReflectionSettings::fromQuality(const QualityConfig& q)
{
    ReflectionSettings s;
    s.kHalfAngle = (float)(q.number("reflection.cache_lobe_half_angle_min_deg") * 3.14159265358979 / 180.0);
    s.mirrorRoughness = (float)q.number("reflection.mirror_roughness_max");
    s.raysPerSample = (uint32_t)q.integer("reflection.g_rays_per_sample");
    const std::vector<double> spacing = q.numbers("reflection.g_sample_spacing_px");
    if (spacing.size() != 2 || spacing[0] != 1) fail("reflection.g_sample_spacing_px must be [1, max]");
    // Samples live on per-tile grids: spacings above 8 px are sampled at 8 (denser than the bound, never sparser).
    s.maxSpacing = (uint32_t)std::min(spacing[1], 8.0);
    if (s.raysPerSample == 0) fail("reflection.g_rays_per_sample must be > 0");
    return s;
}

ReflectionSystem& ReflectionSystem::get(FramePassContext& fc)
{
    struct Slot
    {
        std::unique_ptr<ReflectionSystem> system;
    };
    Slot& slot = fc.state<Slot>("R.reflection");
    if (!slot.system) slot.system = std::make_unique<ReflectionSystem>(fc.device, fc.shaders, fc.quality);
    return *slot.system;
}

ReflectionSystem::ReflectionSystem(Device& device, ShaderLibrary& shaders, const QualityConfig& quality)
    : m_device(device), m_settings(ReflectionSettings::fromQuality(quality))
{
    // Indirect dispatch arguments: counter + one description per sky variant (shader tables fixed; Width per frame).
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT }, upload{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = kArgumentsBytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_arguments)),
          "reflection arguments");
    m_arguments->SetName(L"R reflection dispatch arguments");
    uint8_t image[kArgumentsBytes] = {};
    for (int v = 0; v < 2; ++v)
    {
        const D3D12_DISPATCH_RAYS_DESC desc = rt::RayPipeline::get(device, shaders, rt::standardRayPipeline(kTraceLibrary[v], { "ReflectionTraceGen" })).dispatchDesc(0, 0, 1, 1);
        std::memcpy(image + 16 + v * kDescStride, &desc, sizeof desc);
    }
    d.Flags = D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> staging;
    check(device.d3d()->CreateCommittedResource3(&upload, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&staging)),
          "reflection arguments staging");
    void* mapped = nullptr;
    D3D12_RANGE none{ 0, 0 };
    check(staging->Map(0, &none, &mapped), "map arguments staging");
    std::memcpy(mapped, image, sizeof image);
    staging->Unmap(0, nullptr);
    CommandList cl = device.acquireCommandList(QueueType::Graphics);
    cl.list->CopyBufferRegion(m_arguments.Get(), 0, staging.Get(), 0, sizeof image);
    device.queue(QueueType::Graphics).waitCpu(device.submit(cl));
}

ReflectionSystem::~ReflectionSystem()
{
    m_device.deferRelease(m_arguments);
    m_device.deferRelease(m_history);
}

void ReflectionSystem::ensureHistory(uint32_t width, uint32_t height)
{
    if (m_history && m_historyWidth == width && m_historyHeight == height) return;
    if (m_history) m_device.deferRelease(m_history);
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R16_FLOAT;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(m_device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_history)),
          "reflection distance history");
    m_history->SetName(L"R reflection distance history");
    m_historyWidth = width;
    m_historyHeight = height;
}

void ReflectionSystem::record(FramePassContext& fc, ViewResources& main, rt::RayScene& rays)
{
    RenderGraph& g = fc.graph;
    const ReflectionSettings& s = m_settings;
    const uint32_t width = main.view.width, height = main.view.height;
    const uint32_t tilesX = (width + 7) / 8, tilesY = (height + 7) / 8;
    const bool fresh = !m_history || m_historyWidth != width || m_historyHeight != height;
    ensureHistory(width, height);

    main.reflection = g.createTexture({ "R reflection", width, height + tilesY, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
    const TextureRef reflection = main.reflection, depth = main.depth, gbuffer = main.gbuffer, probes = main.screenProbes, lobes = main.reflectionLobeTiles;
    const TextureRef modes = g.createTexture({ "R reflection modes", width, height, 1, 1, DXGI_FORMAT_R32_UINT });
    m_modes = modes;
    const TextureRef history = g.importTexture(m_history.Get(), { "R reflection distance history", width, height, 1, 1, DXGI_FORMAT_R16_FLOAT },
                                               D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    const BufferRef jobs = g.createBuffer({ "R reflection jobs", (uint64_t)width * height * 4, 4 });
    const BufferRef results = g.createBuffer({ "R reflection results", (uint64_t)width * height * 8, 8 });
    const BufferRef args = g.importBuffer(m_arguments.Get(), { "R reflection dispatch arguments", kArgumentsBytes, 0 });
    const BufferRef cache = fc.resources.giCache;
    const D3D12_GPU_VIRTUAL_ADDRESS frameConstants = main.frameConstants;
    ShaderLibrary& shaders = fc.shaders;
    const float focal = height / (2.0f * std::tan(main.view.verticalFov * 0.5f));

    if (fresh)
        g.addPass("r.refl.history.clear", QueueType::Compute, [&](PassBuilder& b) { b.use(history, Use::UavCompute); },
                  [&shaders, history, width, height](PassContext& c) {
                      const uint32_t k[4] = { c.uav(history), width, height, 0 };
                      c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/ReflectionHistoryClear"));
                      c.computeConstants(k, 4);
                      c.cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
                  });
    g.addPass("r.refl.begin", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(reflection, Use::UavCompute);
                  b.use(args, Use::UavCompute);
              },
              [&shaders, reflection, args, tilesX, tilesY, height](PassContext& c) {
                  const uint32_t k[8] = { c.uav(reflection), c.uav(args), tilesX, tilesY, height, 0, 0, 0 };
                  c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/ReflectionBegin"));
                  c.computeConstants(k, 8);
                  c.cmd->Dispatch((tilesX + 7) / 8, (tilesY + 7) / 8, 1);
              });
    g.addPass("r.refl.classify", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(depth, Use::SrvCompute);
                  b.use(gbuffer, Use::SrvCompute);
                  if (lobes.valid()) b.use(lobes, Use::SrvCompute);
                  b.use(history, Use::SrvCompute);
                  b.use(modes, Use::UavCompute);
                  b.use(jobs, Use::UavCompute);
                  b.use(args, Use::UavCompute);
                  b.use(reflection, Use::UavCompute);
              },
              [&shaders, depth, gbuffer, lobes, history, modes, jobs, args, reflection, s, focal, width, height, tilesX, tilesY, frameConstants](PassContext& c) {
                  const uint32_t k[16] = { c.srv(depth), c.srv(gbuffer), lobes.valid() ? c.srv(lobes) : 0xFFFFFFFFu, c.srv(history),
                                           c.uav(modes), c.uav(jobs), c.uav(args), c.uav(reflection),
                                           asU(s.kHalfAngle), asU(s.mirrorRoughness), asU(focal), height,
                                           width, height, 0, 0 };
                  c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/ReflectionClassify"));
                  c.computeConstants(k, 16);
                  c.bindFrameConstants(frameConstants);
                  c.cmd->Dispatch(tilesX, tilesY, 1);
              });
    g.addPass("r.refl.args", QueueType::Compute, [&](PassBuilder& b) { b.use(args, Use::UavCompute); },
              [&shaders, args](PassContext& c) {
                  const uint32_t k[4] = { c.uav(args), 2, kDescStride, (uint32_t)offsetof(D3D12_DISPATCH_RAYS_DESC, Width) };
                  c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/ReflectionArgs"));
                  c.computeConstants(k, 4);
                  c.cmd->Dispatch(1, 1, 1);
              });

    uint32_t scene[8];
    rays.rootConstants(scene);
    const FrameResources& fr = fc.resources;
    const bool atmosphere = fr.transmittanceLut.valid() && fr.multiScatterLut.valid() && fr.skyViewLut.valid() && fr.aerialPerspective.valid();
    const TextureRef luts[4] = { fr.transmittanceLut, fr.multiScatterLut, fr.skyViewLut, fr.aerialPerspective };
    const int variant = atmosphere ? 0 : 1;
    rt::RayPipeline& pipeline = rt::RayPipeline::get(fc.device, shaders, rt::standardRayPipeline(kTraceLibrary[variant], { "ReflectionTraceGen" }));
    const float3 sky = m_skyRadiance, sun = m_sunIlluminance;
    const float rayLength = (float)fc.quality.number("gi.ray_length_m");
    const uint32_t frame = (uint32_t)fc.frame.frameIndex;
    ID3D12Resource* argumentResource = m_arguments.Get();
    g.addPass("r.refl.trace", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(args, Use::IndirectArgs);
                  b.use(jobs, Use::SrvGraphics);
                  b.use(modes, Use::SrvGraphics);
                  b.use(probes, Use::SrvGraphics);
                  b.use(depth, Use::SrvGraphics);
                  b.use(gbuffer, Use::SrvGraphics);
                  b.use(cache, Use::UavGraphics);
                  b.use(results, Use::UavGraphics);
                  rays.declareTraversal(b);
                  if (atmosphere)
                      for (const TextureRef& t : luts) b.use(t, Use::SrvGraphics);
              },
              [&pipeline, jobs, results, modes, probes, depth, gbuffer, cache, luts, atmosphere, sky, sun, rayLength, s, frame, scene, frameConstants, argumentResource,
               variant](PassContext& c) {
                  uint32_t k[32] = {};
                  k[0] = c.srv(jobs);
                  k[1] = c.uav(results);
                  k[2] = c.srv(modes);
                  k[3] = c.srv(probes);
                  k[4] = asU(sky.x);
                  k[5] = asU(sky.y);
                  k[6] = asU(sky.z);
                  k[7] = asU(rayLength);
                  for (int i = 0; i < 4; ++i) k[8 + i] = atmosphere ? c.srv(luts[i]) : 0xFFFFFFFFu;
                  k[12] = asU(sun.x);
                  k[13] = asU(sun.y);
                  k[14] = asU(sun.z);
                  k[16] = c.srv(depth);
                  k[17] = c.srv(gbuffer);
                  k[18] = c.uav(cache);
                  k[19] = s.raysPerSample;
                  k[20] = frame;
                  std::memcpy(&k[24], scene, sizeof scene);
                  c.computeConstants(k, 32);
                  c.bindFrameConstants(frameConstants);
                  pipeline.dispatchIndirect(c.cmd, argumentResource, 16 + variant * kDescStride);
              });
    g.addPass("r.refl.resolve", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(modes, Use::SrvCompute);
                  b.use(results, Use::SrvCompute);
                  b.use(depth, Use::SrvCompute);
                  b.use(gbuffer, Use::SrvCompute);
                  b.use(reflection, Use::UavCompute);
                  b.use(history, Use::UavCompute);
              },
              [&shaders, modes, results, depth, gbuffer, reflection, history, width, height, tilesX, tilesY, frameConstants](PassContext& c) {
                  const uint32_t k[12] = { c.srv(modes), c.srv(results), c.srv(depth), c.srv(gbuffer), c.uav(reflection), c.uav(history), height, 0, width, height, 0, 0 };
                  c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/ReflectionResolve"));
                  c.computeConstants(k, 12);
                  c.bindFrameConstants(frameConstants);
                  c.cmd->Dispatch(tilesX, tilesY, 1);
              });
}
} // namespace unx::render::refl
