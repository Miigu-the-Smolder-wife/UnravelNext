// The sun's particle transmittance map (FxShadow.hlsl, ParticleShadow.hlsli; fx.particles.shadows): after simulation,
// before S's screen visibility and the particle render pass, which read it (FrameResources::particleShadowParams /
// particleShadowMap; invalid: no map this frame - no shadow-casting look, no tick, or the switch off).
#include "unx/fx/Particles.h"
#include "unx/fx/SpriteLooks.h"
#include "unx/core/Log.h"
#include "unx/render/Device.h"
#include "unx/render/FrameResources.h"
#include "unx/render/GpuScene.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"
#include "unx/render/Tracks.h"

#include <algorithm>
#include <cstring>

namespace unx::render::tracks
{
namespace
{
constexpr uint32_t kUploadSlots = 4;  // > frames in flight
constexpr uint32_t kSlotBytes = fx::kMaxSpriteLooks * fx::kSpriteLookBytes;

// The looks' records of the frame go up through a ring (the particle render pass has its own, per view).
struct FxShadowState
{
    ComPtr<ID3D12Resource> upload;
    uint8_t* mapped = nullptr;
    uint32_t next = 0;
    ~FxShadowState()
    {
        if (upload && mapped) upload->Unmap(0, nullptr);
    }
};
void ensureUpload(Device& device, FxShadowState& s)
{
    if (s.upload) return;
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = (uint64_t)kSlotBytes * kUploadSlots;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    check(device.d3d()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&s.upload)), "FX shadow looks");
    s.upload->SetName(L"FX shadow looks ring");
    D3D12_RANGE none{ 0, 0 };
    check(s.upload->Map(0, &none, reinterpret_cast<void**>(&s.mapped)), "map FX shadow looks");
}
} // namespace

void particleShadows(FramePassContext& fc, const ViewResources& main)
{
    if (!fc.trackState || !fc.quality.has("fx.particles.shadows") || !fc.quality.boolean("fx.particles.shadows")) return;
    fx::ParticleSystem* system = fx::findParticles(*fc.trackState);
    if (!system) return;
    const fx::SpriteLooks& looks = fx::spriteLooks(*fc.trackState);
    if (!looks.anyShadow()) return;
    const fx::ParticleRenderInputs in = system->renderInputs(fc.graph, fc.frame.frameIndex);
    if (!in.valid || in.threads == 0) return;
    const uint32_t resolution = (uint32_t)fc.quality.integer("fx.particles.shadow_resolution");
    const float extent = (float)fc.quality.number("fx.particles.shadow_extent_m"), depthRange = (float)fc.quality.number("fx.particles.shadow_depth_m");
    if (resolution < 16 || resolution > 2048 || !(extent > 0) || !(depthRange > 0))
        fail("fx.particles: shadow_resolution in [16, 2048], shadow_extent_m and shadow_depth_m > 0");
    const float texel = 2.0f * extent / (float)resolution;
    const uint64_t texels = (uint64_t)resolution * resolution;

    const std::vector<uint8_t> lookRecords = looks.records(fc.scene.source(), fc.scene.textureSrvs());
    const uint32_t lookCount = (uint32_t)(lookRecords.size() / fx::kSpriteLookBytes);
    if (lookCount == 0) return;
    FxShadowState& s = fc.trackState->get<FxShadowState>("fx.shadow");
    ensureUpload(fc.device, s);
    const uint64_t uploadOffset = (uint64_t)s.next * kSlotBytes;
    s.next = (s.next + 1) % kUploadSlots;
    std::memcpy(s.mapped + uploadOffset, lookRecords.data(), lookRecords.size());

    RenderGraph& g = fc.graph;
    const BufferRef lookBuf = g.createBuffer(BufferDesc{ "fx.shadow.looks", (uint64_t)lookCount * fx::kSpriteLookBytes, fx::kSpriteLookBytes });
    const BufferRef params = g.createBuffer(BufferDesc{ "fx.shadow.params", 64, 0 });
    const BufferRef map = g.createBuffer(BufferDesc{ "fx.shadow.map", texels * 12, 0 });
    ID3D12Resource* upload = s.upload.Get();
    g.addPass("fx.shadow.looks", QueueType::Graphics, [=](PassBuilder& b) { b.use(lookBuf, Use::CopyDst); },
              [=](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(lookBuf), 0, upload, uploadOffset, (uint64_t)lookCount * fx::kSpriteLookBytes); });

    // the particle render pass's frame-time interpolation (ParticleLayer.cpp): offsets of the stream anchors from the
    // main camera in stream space (double differences), w between the two ticks
    float offsetCur[3] = {}, offsetPrev[3] = {}, axes[3] = {};
    for (int a = 0; a < 3; ++a)
    {
        const double camera = ((&main.view.position.x)[a] + fc.frame.worldOrigin[a]) * fc.frame.streamAxes[a];
        offsetCur[a] = (float)(in.anchor[1][a] - camera);
        offsetPrev[a] = (float)(in.anchor[0][a] - camera);
        axes[a] = fc.frame.streamAxes[a];
    }
    const double w = in.dt > 0 ? std::clamp((fc.frame.time - (in.tickTime - in.dt)) / in.dt, 0.0, 1.0) : 1.0;
    const float wf = (float)w, dt = in.dt;
    auto asUint = [](float f) {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        return u;
    };
    const D3D12_GPU_VIRTUAL_ADDRESS frameConstants = main.frameConstants;
    ShaderLibrary& shaders = fc.shaders;
    const uint32_t threads = in.threads, rangeCount = in.rangeCount;
    auto fill = [=](PassContext& c, uint32_t k[32]) {
        k[0] = c.srv(in.posAge[1]), k[1] = c.srv(in.velocity[1]), k[2] = c.srv(in.posAge[0]), k[3] = c.srv(in.velocity[0]);
        k[4] = c.srv(in.dynamic[1]), k[5] = c.srv(in.dynamic[0]), k[6] = c.srv(in.emitters), k[7] = c.srv(in.programs);
        k[8] = c.srv(in.curveKeys), k[9] = c.srv(in.renderRanges), k[10] = c.srv(in.renderBlocks), k[11] = rangeCount;
        k[12] = c.uav(params), k[13] = c.uav(map), k[14] = c.srv(lookBuf), k[15] = lookCount;
        k[16] = asUint(offsetCur[0]), k[17] = asUint(offsetCur[1]), k[18] = asUint(offsetCur[2]), k[19] = asUint(wf);
        k[20] = asUint(offsetPrev[0]), k[21] = asUint(offsetPrev[1]), k[22] = asUint(offsetPrev[2]), k[23] = asUint(dt);
        k[24] = asUint(axes[0]), k[25] = asUint(axes[1]), k[26] = asUint(axes[2]), k[27] = threads;
        k[28] = resolution, k[29] = asUint(texel), k[30] = asUint(depthRange), k[31] = c.srv(map);
    };
    g.addPass("fx.shadow.clear", QueueType::Graphics,
              [=](PassBuilder& b) {
                  b.use(params, Use::UavCompute);
                  b.use(map, Use::UavCompute);
              },
              [=, &shaders](PassContext& c) {
                  uint32_t k[32] = {};
                  fill(c, k);
                  c.cmd->SetPipelineState(shaders.compute("Passes/FX/FxShadow.STEP0"));
                  c.computeConstants(k, 32);
                  c.bindFrameConstants(frameConstants);
                  c.cmd->Dispatch((uint32_t)((texels + 255) / 256), 1, 1);
              });
    g.addPass("fx.shadow.splat", QueueType::Graphics,
              [=](PassBuilder& b) {
                  for (const BufferRef& x : { in.posAge[0], in.posAge[1], in.velocity[0], in.velocity[1], in.dynamic[0], in.dynamic[1], in.emitters, in.programs,
                                              in.curveKeys, in.renderRanges, in.renderBlocks })
                      b.use(x, Use::SrvCompute);
                  b.use(lookBuf, Use::SrvCompute);
                  b.use(map, Use::UavCompute);
              },
              [=, &shaders](PassContext& c) {
                  uint32_t k[32] = {};
                  fill(c, k);
                  c.cmd->SetPipelineState(shaders.compute("Passes/FX/FxShadow.STEP1"));
                  c.computeConstants(k, 32);
                  c.bindFrameConstants(frameConstants);
                  c.cmd->Dispatch((threads + 255) / 256, 1, 1);
              });
    fc.resources.particleShadowParams = params;
    fc.resources.particleShadowMap = map;
}
} // namespace unx::render::tracks
