// Track entry points of W (INTERFACES_KO.md 5.2, Tracks.h): waterGeometry() after the simulation and before V (the GPU
// fluids' reconstructed surfaces into V's triangle streams; the sea's camera surface from the view grid,
// FEATURES_GAME 1.8 B), water() in M's shading() after the opaque kernel (the water layer's refraction, absorption and
// reflection: WaterSurface.h, stage 1).
#include "unx/render/GpuScene.h"
#include "unx/render/Tracks.h"
#include "unx/water/FluidSurface.h"
#include "unx/water/Foam.h"
#include "unx/water/Ocean.h"
#include "unx/water/Pool.h"
#include "unx/water/ViewGrid.h"
#include "unx/water/WaterSurface.h"
#include "unx/water/WaterSunMap.h"

#include "unx/core/Log.h"

#include <algorithm>
#include <cmath>
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
    uint64_t topologyId = 0;
};
struct FluidState
{
    std::vector<FluidSlot> slots;
};
uint32_t roundUp(uint32_t v, uint32_t m) { return (v + m - 1) / m * m; }

// The sea's modules (one sea a frame: FrameContext::ocean), kept across frames.
struct OceanState
{
    std::unique_ptr<water::Ocean> ocean;
    std::unique_ptr<water::ViewGrid> grid;
    std::unique_ptr<water::Foam> foam;
    water::OceanDesc desc;
};
} // namespace

// B7 (FEATURES_GAME 1.8 B): the frame's sea. FrameContext::ocean -> the FFT cascades at the frame's time, the camera's
// surface on the main view's pixels (the view grid, with the view's own sub-pixel jitter; FrameResources::oceanDepth /
// waterSurface: V merges the depth into the water layer as COV_OCEAN_ID) and the foam clipmap. What the surface pass
// shades the sea's pixels with stays in the track state for this frame (WaterSurface.h OceanSurfaceFrame).
// The view grid draws the sea from above: a camera under the still level gets no sea (the sea seen from inside and the
// medium of a camera in it are not there yet). The sea is in no other view, no ray scene and not in the sun's water map
// (the bed under it is lit as in air).
static void waterOcean(FramePassContext& fc)
{
    water::OceanSurfaceFrame& published = fc.state<water::OceanSurfaceFrame>("W.oceanFrame");
    published = {};
    const OceanFrame* in = fc.frame.ocean;
    const QualityConfig& q = fc.quality;
    OceanState& st = fc.state<OceanState>("W.ocean");
    // (the foam's windows follow the frame's coordinates: an origin shift is a whole number of every level's texels)
    const float3 shift = fc.frame.originShift;
    if (st.foam && (shift.x != 0 || shift.z != 0)) st.foam->rebase(shift.x, shift.z);
    if (!in || (q.has("shading.water_ocean") && !q.boolean("shading.water_ocean"))) return;
    const ViewDesc& view = fc.frame.mainView;
    if (!(view.position.y > in->level) || !view.width || !view.height) return;
    water::OceanDesc desc;
    desc.windSpeed = std::max(in->windSpeed, 0.5f);  // (a sea without wind: the spectrum's peak is at infinity - a light air's ripples instead)
    desc.windDirection = in->windDirection;
    desc.fetch = in->fetch;
    desc.spread = in->spread;
    desc.seed = in->seed;
    if (!st.ocean)
    {
        st.ocean = std::make_unique<water::Ocean>(fc.device, fc.shaders, desc);
        st.grid = std::make_unique<water::ViewGrid>(fc.device, fc.shaders, fc.framesInFlight);
    }
    else if (st.desc.windSpeed != desc.windSpeed || st.desc.windDirection != desc.windDirection || st.desc.fetch != desc.fetch || st.desc.spread != desc.spread ||
             st.desc.seed != desc.seed)
        st.ocean->setDesc(desc);
    st.desc = desc;
    RenderGraph& g = fc.graph;
    const water::OceanOutput fields = st.ocean->record(g, fc.frame.time);
    // the main view's camera: its basis from the view matrix's rows, the projection's tangents and its centre offset
    // (FroxelCommon.hlsli froxelRayAt reads the same terms: a view direction's tangents are (ndc + offset) / proj)
    water::ViewGridCamera camera;
    auto row = [&](int r) { return normalize(float3{ view.view.m[r][0], view.view.m[r][1], view.view.m[r][2] }); };
    const float3 right = row(0), up = row(1), back = row(2);
    camera.position[0] = view.position.x, camera.position[1] = view.position.y, camera.position[2] = view.position.z;
    camera.right[0] = right.x, camera.right[1] = right.y, camera.right[2] = right.z;
    camera.up[0] = up.x, camera.up[1] = up.y, camera.up[2] = up.z;
    camera.forward[0] = -back.x, camera.forward[1] = -back.y, camera.forward[2] = -back.z;
    camera.tanX = 1.0f / view.proj.m[0][0];
    camera.tanY = 1.0f / view.proj.m[1][1];
    camera.offset[0] = view.proj.m[0][2] - view.proj.m[0][3];
    camera.offset[1] = view.proj.m[1][2] - view.proj.m[1][3];
    camera.width = view.width;
    camera.height = view.height;
    camera.nearPlane = view.nearPlane;
    water::ViewGridWater body;
    body.level = in->level;
    if (in->horizontalBound > 0) body.horizontalBound = in->horizontalBound;
    if (in->verticalBound > 0) body.verticalBound = in->verticalBound;
    body.lake = in->lake != 0;
    if (body.lake)
    {
        body.lakeCentre[0] = in->lakeCentre[0], body.lakeCentre[1] = in->lakeCentre[1];
        body.lakeRadius = in->lakeRadius;
        // (the rows end where the water does: its far side from the camera)
        const float away = std::hypot(view.position.x - in->lakeCentre[0], view.position.z - in->lakeCentre[1]);
        body.extent = std::max(away + in->lakeRadius + body.bound(), 2 * body.nearRadius);
    }
    const water::ViewGridOutput grid = st.grid->record(g, fc.frame.frameIndex, fields, desc.lengths, camera, body);
    fc.resources.oceanDepth = grid.depth;
    fc.resources.waterSurface = grid.surface;
    water::FoamOutput foam;
    const bool foamOn = !q.has("shading.water_ocean_foam") || q.boolean("shading.water_ocean_foam");
    if (foamOn)
    {
        if (!st.foam) st.foam = std::make_unique<water::Foam>(fc.device, fc.shaders, water::FoamDesc{}, fc.framesInFlight);
        foam = st.foam->record(g, fc.frame.frameIndex, fc.frame.time, fields, desc, view.position.x, view.position.z);
    }
    published.frame = fc.frame.frameIndex;
    published.surface = grid.surface;
    published.displacement = fields.displacement;
    published.slopes = fields.slopes;
    published.foam = foam.foam;
    published.foamParams = foamOn ? foam.paramSrv : 0xFFFFFFFFu;
    published.material = in->material;
    for (int a = 0; a < 3; ++a) published.lengths[a] = desc.lengths[a];
    // the JONSWAP sea state's tail (Ocean.hlsli oceanVariance: alpha, the peak frequency) for the unresolved slopes
    const double gravity = 9.81, U = desc.windSpeed, F = desc.fetch;
    const double omegaPeak = 22.0 * std::cbrt(gravity * gravity / (U * F));
    published.alpha = (float)(0.076 * std::pow(U * U / (F * gravity), 0.22));
    published.peakWavenumber = (float)(omegaPeak * omegaPeak / gravity);
    published.finestWavenumber = 3.14159265f * (float)water::Ocean::kN / desc.lengths[2];
    published.windSpeed = desc.windSpeed;
}

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
            if (fc.quality.has("shading.water_turbid") && fc.quality.boolean("shading.water_turbid"))  // v1.92: the beam's extinction includes sigma_s
                for (int a = 0; a < 3; ++a) w.transmittance[a] *= std::exp(-(a == 0 ? m.waterScattering.x : a == 1 ? m.waterScattering.y : m.waterScattering.z));
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
            d.refittableTail = true;
            slot.surface = std::make_unique<water::FluidSurface>(fc.device, fc.shaders, d);
            slot.topologyId = allocateTriangleStreamTopologyId();
            std::copy(nodes, nodes + 3, slot.nodes);
            slot.h = h;
            slot.particles = d.maxParticles;
        }
        // An anchored domain moves during the tick (NP_FluidGpuView2): the start buffer's cells are from startOrigin, the
        // current one's from origin, so the blended point is lerp(startOrigin, origin, alpha) + dx lerp(x_start, x, alpha) -
        // the grid at the blended origin (physics a342d694: 0.5 m off at 30 m/s without it).
        const bool blend = in.start && in.startValid;
        float origin[3];
        for (int a = 0; a < 3; ++a) origin[a] = float(blend ? in.startOrigin[a] + (in.origin[a] - in.startOrigin[a]) * in.alpha : in.origin[a]);
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
        std::copy(std::begin(in.frameVelocity), std::end(in.frameVelocity), input.frameVelocity);  // relative -> world velocity
        // The fluid's particles and origin are in the physics World's axes, which are the particle streams' (the Unity host's
        // World is the renderer's mirrored in z: FrameContext::streamAxes); the surface is written in renderer axes.
        for (int a = 0; a < 3; ++a) input.axes[a] = frame.streamAxes[a] < 0 ? -1.0f : 1.0f;
        {
            // W3 seam: the basins recorded this frame whose footprint meets the fluid's domain (at most two)
            float lo[3], hi[3];
            slot.surface->bounds(lo, hi);
            for (int a = 0; a < 3; ++a)
                if (input.axes[a] < 0) { const float l = -hi[a]; hi[a] = -lo[a]; lo[a] = l; }
            for (const water::FluidSurfaceInput::Basin& b : fc.state<std::vector<water::FluidSurfaceInput::Basin>>("W.poolBasins"))
            {
                const float r = 0.5f * std::sqrt(b.sizeX * b.sizeX + b.sizeZ * b.sizeZ);
                if (b.centre[0] + r < lo[0] || b.centre[0] - r > hi[0] || b.centre[2] + r < lo[2] || b.centre[2] - r > hi[2]) continue;
                input.basins.push_back(b);
            }
        }
        const water::FluidSurfaceOutput out = slot.surface->record(g, input);
        TriangleStream stream;
        stream.vertices = out.vertices;
        stream.drawArgs = out.draw;
        stream.velocities = out.velocities;
        stream.material = in.material;
        stream.maxTriangles = slot.surface->desc().maxTriangles;
        stream.fixedTopologyId = slot.topologyId; // unused slots are finite DXR-active points, never NaN-inactive
        float lo[3], hi[3];
        slot.surface->bounds(lo, hi);
        for (int a = 0; a < 3; ++a)
            if (input.axes[a] < 0)
            {
                const float l = -hi[a];
                hi[a] = -lo[a];
                lo[a] = l;
            }
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
    prepareTriangleStreamDraws(fc);
    waterSunMap(fc);
    waterOcean(fc);  // B7: the sea's surface on the main view's pixels (no stream: the water layer's ocean slot)
}
void water(FramePassContext& fc, ViewResources& view) { water::waterSurface(fc, view); }
} // namespace unx::render::tracks
