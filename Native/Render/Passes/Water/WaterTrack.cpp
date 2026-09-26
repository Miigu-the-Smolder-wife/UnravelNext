// Track entry points of W (INTERFACES_KO.md 5.2, Tracks.h): waterGeometry() after the simulation and before V (the GPU
// fluids' reconstructed surfaces into V's triangle streams; the ocean's camera surface follows with the view grid,
// FEATURES_GAME 1.8 B), water() in M's shading() after the opaque kernel (the water layer's refraction, absorption and
// reflection: WaterSurface.h, stage 1).
#include "unx/render/GpuScene.h"
#include "unx/render/Tracks.h"
#include "unx/water/FluidSurface.h"
#include "unx/water/Pool.h"
#include "unx/water/WaterSurface.h"
#include "unx/water/WaterSunMap.h"

#include "unx/core/Log.h"

#include <algorithm>
#include <memory>
#include <vector>

namespace unx::render::tracks
{
namespace
{
// One reconstruction per fluid slot of the frame (FrameContext::fluids), rebuilt when the fluid's domain, cell size or
// particle capacity changes.
struct FluidSlot
{
    std::unique_ptr<water::FluidSurface> surface;
    uint32_t nodes[3] = {}, particles = 0;
    float h = 0;
};
struct FluidState
{
    std::vector<FluidSlot> slots;
};
uint32_t roundUp(uint32_t v, uint32_t m) { return (v + m - 1) / m * m; }
} // namespace

// Stage 2 (FEATURES_GAME 1.9): the sun-space map of W's streams, for band A under water (WaterLight.hlsli).
static void waterSunMap(FramePassContext& fc)
{
    const scene::Scene* source = fc.scene.source();
    if (!source) return;
    std::vector<water::WaterSunStream> streams;
    for (uint32_t i = 0; i < (uint32_t)fc.resources.triangleStreams.size() && i < kMaxTriangleStreams; ++i)
    {
        const TriangleStream& t = fc.resources.triangleStreams[i];
        if (!t.vertices.valid()) continue;
        water::WaterSunStream w;
        w.stream = t;
        if (t.material < source->materials.size())
        {
            const scene::Material& m = source->materials[t.material];
            w.transmittance[0] = m.baseColor.x, w.transmittance[1] = m.baseColor.y, w.transmittance[2] = m.baseColor.z;
            if (m.ior > 1.0001f) w.ior = m.ior;
        }
        streams.push_back(w);
    }
    if (streams.empty()) return;
    auto& map = fc.state<std::unique_ptr<water::WaterSunMap>>("W.sunMap");
    if (!map) map = std::make_unique<water::WaterSunMap>(fc.device);
    const water::WaterSunMapOutput out = map->record(fc.graph, fc.shaders, fc.frame.frameIndex, streams, source->sun.direction);
    if (!out.texels) return;
    fc.resources.waterSunDepth = out.depth;
    fc.resources.waterSunNormal = out.normal;
    fc.resources.waterSunMedium = out.medium;
    fc.resources.waterSunConstants = out.constants;
    fc.resources.waterSunCaustics = out.caustics;
}
static void waterFluids(FramePassContext& fc)
{
    const FrameContext& frame = fc.frame;
    if (!frame.fluidCount) return;
    if (frame.fluidCount + fc.resources.triangleStreams.size() > kMaxTriangleStreams)
        fail("W: %u fluids exceed the frame's %u triangle streams", frame.fluidCount, kMaxTriangleStreams);
    FluidState& state = fc.state<FluidState>("W.fluids");
    if (state.slots.size() < frame.fluidCount) state.slots.resize(frame.fluidCount);
    RenderGraph& g = fc.graph;
    for (uint32_t i = 0; i < frame.fluidCount; ++i)
    {
        const FluidFrame& in = frame.fluids[i];
        if (!in.current || !(in.dx > 0) || !in.domainCells[0] || !in.domainCells[1] || !in.domainCells[2] || in.stride < 48)
            fail("W: fluid %u has no particles, cell size or domain", i);
        if (!in.count) continue;
        FluidSlot& slot = state.slots[i];
        // Two nodes per cell (FluidSurfaceDesc::scale 2), the grid a multiple of the 8^3 block.
        const uint32_t nodes[3] = { roundUp(2 * in.domainCells[0], 8), roundUp(2 * in.domainCells[1], 8), roundUp(2 * in.domainCells[2], 8) };
        const float h = 0.5f * in.dx;
        if (!slot.surface || !std::equal(nodes, nodes + 3, slot.nodes) || slot.h != h || in.count > slot.particles)
        {
            water::FluidSurfaceDesc d;
            std::copy(nodes, nodes + 3, d.nodes);
            d.scale = 2.0f;
            d.h = h;
            d.maxParticles = std::max<uint32_t>(4096, std::max(in.count, in.startCount) * 5 / 4);
            d.maxTriangles = 2 * d.maxParticles;  // surface triangles per particle stay near 1 (FEATURES_GAME 0.B: 0.2 M at 250 k)
            slot.surface = std::make_unique<water::FluidSurface>(fc.device, fc.shaders, d);
            std::copy(nodes, nodes + 3, slot.nodes);
            slot.h = h;
            slot.particles = d.maxParticles;
        }
        const float origin[3] = { float(in.origin[0]), float(in.origin[1]), float(in.origin[2]) };
        slot.surface->setOrigin(origin);
        water::FluidSurfaceInput input;
        input.particles = g.importBuffer(in.current, { "W fluid particles", in.current->GetDesc().Width, 0 });
        if (in.start && in.startValid)
        {
            input.previous = g.importBuffer(in.start, { "W fluid particles (tick start)", in.start->GetDesc().Width, 0 });
            input.previousSlotOffset = 44;  // NP_FluidParticle.origin: the particle's slot at the tick's start
        }
        input.count = in.count;
        input.stride = in.stride;
        input.alpha = in.alpha;
        input.velocityOffset = 16;       // NP_FluidParticle velocity, cells per second
        input.velocityScale = in.dx;
        const water::FluidSurfaceOutput out = slot.surface->record(g, input);
        TriangleStream stream;
        stream.vertices = out.vertices;
        stream.drawArgs = out.draw;
        stream.velocities = out.velocities;
        stream.material = in.material;
        stream.maxTriangles = slot.surface->desc().maxTriangles;
        float lo[3], hi[3];
        slot.surface->bounds(lo, hi);
        stream.boundsMin = { lo[0], lo[1], lo[2] };
        stream.boundsMax = { hi[0], hi[1], hi[2] };
        // The water layer (v1.63): one sample per pixel with edge records - a fluid seen from close by fills the screen,
        // where coverage records cost ~5.5 ms at 4K (ea6ff3d); W's water shading refracts through it (FEATURES_GAME 1.9).
        stream.layer = 1;
        fc.resources.triangleStreams.push_back(stream);
    }
}
void waterGeometry(FramePassContext& fc)
{
    water::poolGeometry(fc);  // W2 closed basins (FEATURES_GAME 1.10): before the sun map, which takes every layer-1 stream
    waterFluids(fc);
    waterSunMap(fc);
}
void water(FramePassContext& fc, ViewResources& view) { water::waterSurface(fc, view); }
} // namespace unx::render::tracks
