// A3 FX particle lights (FxLights.hlsl; contract NativeVfxStream.h NV_STREAM_PROGRAM_LIGHT, S_STATUS 10): the GPU scene's
// FX light tail (GpuScene::fxLightRange, INTERFACES v1.79 / v1.81) filled every frame from the latest tick's light rows.
//   particleLightCapacity (before the frame's imports and frame constants): the tail grows to the light rows of the
//     latest tick (max(F, 2 x capacity, 16): a burst of explosions does not rebuild the light buffer every tick); it never
//     shrinks while the particle module lives. A total over GpuScene::kMaxSceneLights is refused and reported once; the
//     rows past the tail then light nothing (the writer clamps F to the capacity and says so in the log).
//   particleLights (after simulation, before V and S): uploads the chunk and slot tables, reduces each chunk (STEP 0) and
//     writes the lights and F (STEP 1). With no module, no tick or no light rows it still writes F = 0.
//   Per-particle lights (fx.particles.particle_lights_max = N; FxLights.hlsl): the N most important particles of the
//     light rows are lights of their own after the rows' (the tail holds the rows + N), chosen by an importance
//     histogram (STEP 3, 2, 4), left out of their rows' sums (STEP 0), placed in chunk order (STEP 5) and written
//     (STEP 6). particleLightCapacity takes N from the state particleLights left (the previous frame's quality).
#include "unx/fx/Particles.h"
#include "unx/core/Log.h"
#include "unx/render/Device.h"
#include "unx/render/FrameResources.h"
#include "unx/render/GpuScene.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"
#include "unx/render/Tracks.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace unx::render::tracks
{
namespace
{
struct FxLightState
{
    ComPtr<ID3D12Resource> upload;
    uint8_t* mapped = nullptr;
    uint64_t slotBytes = 0;
    uint32_t next = 0;
    bool refused = false, clamped = false;
    uint32_t particleLights = 0;  // fx.particles.particle_lights_max as particleLights last read it
    ~FxLightState()
    {
        if (upload && mapped) upload->Unmap(0, nullptr);
    }
};
constexpr uint32_t kUploadSlots = 4;  // > frames in flight

void ensureUpload(Device& device, FxLightState& s, uint64_t bytes)
{
    if (s.upload && s.slotBytes >= bytes) return;
    if (s.upload)
    {
        s.upload->Unmap(0, nullptr);
        device.deferRelease(s.upload);
        s.upload.Reset();
        s.mapped = nullptr;
    }
    s.slotBytes = std::max<uint64_t>((bytes + 65535) & ~65535ull, s.slotBytes * 2);
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = s.slotBytes * kUploadSlots;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    check(device.d3d()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&s.upload)), "FX light tables");
    s.upload->SetName(L"FX light tables ring");
    D3D12_RANGE none{ 0, 0 };
    check(s.upload->Map(0, &none, reinterpret_cast<void**>(&s.mapped)), "map FX light tables");
}
} // namespace

void particleLightCapacity(TrackState& state, GpuScene& scene)
{
    fx::ParticleSystem* system = fx::findParticles(state);
    if (!system) return;
    FxLightState& s = state.get<FxLightState>("fx.lights");
    // (the rows' lights, then the per-particle lights when the rows have any particle to choose from)
    const uint32_t rowCount = (uint32_t)system->lightTables().slotChunks.size();
    const uint32_t rows = rowCount ? rowCount + s.particleLights : 0u;
    const uint32_t capacity = scene.fxLightRange().capacity;
    if (rows <= capacity) return;
    const uint32_t want = std::max({ rows, 2 * capacity, 16u });
    if (scene.setFxLightCapacity(want)) return;
    if (scene.setFxLightCapacity(rows)) return;  // the doubled size was over the limit; the rows themselves may fit
    if (!s.refused) logf("FX lights: %u light rows do not fit the scene light limit %u; rows past the tail light nothing\n", rows, GpuScene::kMaxSceneLights);
    s.refused = true;
}

void particleLights(FramePassContext& fc, const ViewResources& main)
{
    const GpuScene::FxLightRange range = fc.scene.fxLightRange();
    if (range.capacity == 0 || !fc.resources.fxLights.valid() || !fc.resources.fxLightCount.valid() || !fc.trackState) return;
    FxLightState& s = fc.trackState->get<FxLightState>("fx.lights");
    fx::ParticleSystem* system = fx::findParticles(*fc.trackState);
    fx::ParticleRenderInputs in;
    if (system) in = system->renderInputs(fc.graph, fc.frame.frameIndex);
    const fx::ParticleSystem::LightTables* t = in.valid ? in.lights.get() : nullptr;
    uint32_t slots = t ? (uint32_t)t->slotChunks.size() : 0;
    s.particleLights = fc.quality.has("fx.particles.particle_lights_max") ? (uint32_t)std::clamp<int64_t>(fc.quality.integer("fx.particles.particle_lights_max"), 0, 4096) : 0u;
    if (slots > range.capacity)
    {
        if (!s.clamped) logf("FX lights: %u light rows, tail capacity %u (limit %u): the rows past it light nothing\n", slots, range.capacity, GpuScene::kMaxSceneLights);
        s.clamped = true;
        slots = range.capacity;
    }
    uint32_t chunks = 0;
    if (slots)
    {
        const auto& last = t->slotChunks[slots - 1];
        chunks = last[0] + last[1];
    }
    if (chunks > 65535u) fail("FX lights: %u chunks exceed one dispatch (65,535 groups)", chunks);
    // the per-particle lights the tail has room for after the rows'
    const uint32_t perParticle = chunks ? std::min(s.particleLights, range.capacity - slots) : 0u;

    // tables of this frame: chunks (uint4), then slots (uint2)
    const uint64_t chunkBytes = (uint64_t)std::max(chunks, 1u) * 16, slotBytes = (uint64_t)std::max(slots, 1u) * 8;
    ensureUpload(fc.device, s, chunkBytes + slotBytes);
    const uint32_t uploadSlot = s.next;
    s.next = (s.next + 1) % kUploadSlots;
    const uint64_t uploadOffset = (uint64_t)uploadSlot * s.slotBytes;
    if (chunks) std::memcpy(s.mapped + uploadOffset, t->chunks.data(), (size_t)chunks * 16);
    if (slots) std::memcpy(s.mapped + uploadOffset + chunkBytes, t->slotChunks.data(), (size_t)slots * 8);

    RenderGraph& g = fc.graph;
    const BufferRef chunkBuf = g.createBuffer(BufferDesc{ "fx.lights.chunks", chunkBytes, 16 });
    const BufferRef slotBuf = g.createBuffer(BufferDesc{ "fx.lights.slots", slotBytes, 8 });
    const BufferRef partials = g.createBuffer(BufferDesc{ "fx.lights.partials", (uint64_t)std::max(chunks, 1u) * 48, 16 });
    const BufferRef lights = fc.resources.fxLights, count = fc.resources.fxLightCount;
    const BufferRef histogram = perParticle ? g.createBuffer(BufferDesc{ "fx.lights.histogram", 256 * 4, 0 }) : BufferRef{};
    const BufferRef cut = perParticle ? g.createBuffer(BufferDesc{ "fx.lights.cut", 8 + (uint64_t)chunks * 4, 0 }) : BufferRef{};
    ID3D12Resource* upload = s.upload.Get();
    g.addPass("fx.lights.tables", QueueType::Graphics,
              [=](PassBuilder& b) {
                  b.use(chunkBuf, Use::CopyDst);
                  b.use(slotBuf, Use::CopyDst);
              },
              [=](PassContext& c) {
                  c.cmd->CopyBufferRegion(c.resource(chunkBuf), 0, upload, uploadOffset, chunkBytes);
                  c.cmd->CopyBufferRegion(c.resource(slotBuf), 0, upload, uploadOffset + chunkBytes, slotBytes);
              });

    // the particle render pass's frame-time interpolation (ParticleLayer.cpp): offsets of the stream anchors from the
    // camera in stream space (double differences), w between the two ticks
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
    const uint32_t first = range.first;
    const D3D12_GPU_VIRTUAL_ADDRESS frameConstants = main.frameConstants;
    ShaderLibrary& shaders = fc.shaders;
    const fx::ParticleRenderInputs inputs = in;
    auto fill = [=](PassContext& c, uint32_t k[32]) {
        const uint32_t none = 0xFFFFFFFFu;
        if (inputs.valid)
        {
            k[0] = c.srv(inputs.posAge[1]), k[1] = c.srv(inputs.velocity[1]), k[2] = c.srv(inputs.posAge[0]), k[3] = c.srv(inputs.velocity[0]);
            k[4] = c.srv(inputs.dynamic[1]), k[5] = c.srv(inputs.dynamic[0]), k[6] = c.srv(inputs.emitters), k[7] = c.srv(inputs.programs);
            k[8] = c.srv(inputs.curveKeys), k[9] = c.srv(inputs.renderRanges);
        }
        else
            for (int i = 0; i < 10; ++i) k[i] = none;
        k[10] = c.srv(chunkBuf), k[11] = c.srv(slotBuf);
        k[13] = c.uav(lights), k[14] = c.uav(count), k[15] = first;
        k[16] = asUint(offsetCur[0]), k[17] = asUint(offsetCur[1]), k[18] = asUint(offsetCur[2]), k[19] = asUint(wf);
        k[20] = asUint(offsetPrev[0]), k[21] = asUint(offsetPrev[1]), k[22] = asUint(offsetPrev[2]), k[23] = asUint(dt);
        k[24] = asUint(axes[0]), k[25] = asUint(axes[1]), k[26] = asUint(axes[2]), k[27] = slots;
        k[28] = chunks, k[29] = k[30] = 0, k[31] = perParticle;
    };
    auto useInputs = [=](PassBuilder& b) {
        for (const BufferRef& x : { inputs.posAge[0], inputs.posAge[1], inputs.velocity[0], inputs.velocity[1], inputs.dynamic[0], inputs.dynamic[1],
                                    inputs.emitters, inputs.programs, inputs.curveKeys, inputs.renderRanges })
            b.use(x, Use::SrvCompute);
        b.use(chunkBuf, Use::SrvCompute);
    };
    if (perParticle)
    {
        // the importance histogram and its cut (FxLights.hlsl STEP 3, 2, 4)
        g.addPass("fx.lights.histogram.clear", QueueType::Graphics, [=](PassBuilder& b) { b.use(histogram, Use::UavCompute); },
                  [=, &shaders](PassContext& c) {
                      uint32_t k[32] = {};
                      fill(c, k);
                      k[29] = c.uav(histogram);
                      c.cmd->SetPipelineState(shaders.compute("Passes/FX/FxLights.STEP3"));
                      c.computeConstants(k, 32);
                      c.bindFrameConstants(frameConstants);
                      c.cmd->Dispatch(1, 1, 1);
                  });
        g.addPass("fx.lights.histogram", QueueType::Graphics,
                  [=](PassBuilder& b) {
                      useInputs(b);
                      b.use(histogram, Use::UavCompute);
                  },
                  [=, &shaders](PassContext& c) {
                      uint32_t k[32] = {};
                      fill(c, k);
                      k[29] = c.uav(histogram);
                      c.cmd->SetPipelineState(shaders.compute("Passes/FX/FxLights.STEP2"));
                      c.computeConstants(k, 32);
                      c.bindFrameConstants(frameConstants);
                      c.cmd->Dispatch(chunks, 1, 1);
                  });
        g.addPass("fx.lights.cut", QueueType::Graphics,
                  [=](PassBuilder& b) {
                      b.use(histogram, Use::SrvCompute);
                      b.use(cut, Use::UavCompute);
                  },
                  [=, &shaders](PassContext& c) {
                      uint32_t k[32] = {};
                      fill(c, k);
                      k[29] = c.srv(histogram), k[30] = c.uav(cut);
                      c.cmd->SetPipelineState(shaders.compute("Passes/FX/FxLights.STEP4"));
                      c.computeConstants(k, 32);
                      c.bindFrameConstants(frameConstants);
                      c.cmd->Dispatch(1, 1, 1);
                  });
    }
    if (chunks)
        g.addPass("fx.lights.chunks", QueueType::Graphics,
                  [=](PassBuilder& b) {
                      for (const BufferRef& x : { inputs.posAge[0], inputs.posAge[1], inputs.velocity[0], inputs.velocity[1], inputs.dynamic[0], inputs.dynamic[1],
                                                  inputs.emitters, inputs.programs, inputs.curveKeys, inputs.renderRanges })
                          b.use(x, Use::SrvCompute);
                      b.use(chunkBuf, Use::SrvCompute);
                      b.use(partials, Use::UavCompute);
                      if (perParticle) b.use(cut, Use::SrvCompute);
                  },
                  [=, &shaders](PassContext& c) {
                      uint32_t k[32] = {};
                      fill(c, k);
                      k[12] = c.uav(partials);
                      if (perParticle) k[30] = c.srv(cut);
                      c.cmd->SetPipelineState(shaders.compute("Passes/FX/FxLights.STEP0"));
                      c.computeConstants(k, 32);
                      c.bindFrameConstants(frameConstants);
                      c.cmd->Dispatch(chunks, 1, 1);
                  });
    g.addPass("fx.lights.write", QueueType::Graphics,
              [=](PassBuilder& b) {
                  if (chunks) b.use(partials, Use::SrvCompute);
                  b.use(slotBuf, Use::SrvCompute);
                  b.use(lights, Use::UavCompute);
                  b.use(count, Use::UavCompute);
              },
              [=, &shaders](PassContext& c) {
                  uint32_t k[32] = {};
                  fill(c, k);
                  k[12] = chunks ? c.srv(partials) : 0xFFFFFFFFu;
                  c.cmd->SetPipelineState(shaders.compute("Passes/FX/FxLights.STEP1"));
                  c.computeConstants(k, 32);
                  c.bindFrameConstants(frameConstants);
                  c.cmd->Dispatch((std::max(slots, 1u) + 63) / 64, 1, 1);
              });
    if (perParticle)
    {
        // the chunks' first particle light and F (STEP 5), then the chosen particles' lights (STEP 6)
        g.addPass("fx.lights.place", QueueType::Graphics,
                  [=](PassBuilder& b) {
                      b.use(partials, Use::SrvCompute);
                      b.use(cut, Use::UavCompute);
                      b.use(count, Use::UavCompute);
                  },
                  [=, &shaders](PassContext& c) {
                      uint32_t k[32] = {};
                      fill(c, k);
                      k[12] = c.srv(partials), k[30] = c.uav(cut);
                      c.cmd->SetPipelineState(shaders.compute("Passes/FX/FxLights.STEP5"));
                      c.computeConstants(k, 32);
                      c.bindFrameConstants(frameConstants);
                      c.cmd->Dispatch(1, 1, 1);
                  });
        g.addPass("fx.lights.particles", QueueType::Graphics,
                  [=](PassBuilder& b) {
                      useInputs(b);
                      b.use(cut, Use::SrvCompute);
                      b.use(lights, Use::UavCompute);
                  },
                  [=, &shaders](PassContext& c) {
                      uint32_t k[32] = {};
                      fill(c, k);
                      k[30] = c.srv(cut);
                      c.cmd->SetPipelineState(shaders.compute("Passes/FX/FxLights.STEP6"));
                      c.computeConstants(k, 32);
                      c.bindFrameConstants(frameConstants);
                      c.cmd->Dispatch(chunks, 1, 1);
                  });
    }
}
} // namespace unx::render::tracks
