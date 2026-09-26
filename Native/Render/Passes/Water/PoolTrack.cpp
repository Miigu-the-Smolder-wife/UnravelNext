// W2 closed basins in the frame (FEATURES_GAME 1.10; INTERFACES v1.78): FrameContext::pools -> one unx::water::Pool per id
// (track state "W.pools"), evolved in the frames where a view can see the basin or it has sources, and pushed as layer-1
// triangle streams before V. Called by W's waterGeometry.
#include "unx/water/FluidSurface.h"
#include "unx/water/Pool.h"
#include "unx/water/WaterSurface.h"

#include "unx/render/Frame.h"
#include "unx/core/Log.h"

#include <cmath>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace unx::water
{
using namespace unx::render;

namespace
{
struct PoolSlot
{
    std::unique_ptr<Pool> pool;
    PoolDesc desc;
};
struct PoolState
{
    std::unordered_map<uint32_t, PoolSlot> slots;
};
bool sameBasin(const PoolDesc& a, const PoolDesc& b)
{
    return a.sizeX == b.sizeX && a.sizeZ == b.sizeZ && a.depth == b.depth && a.surfaceFilm == b.surfaceFilm;
}
} // namespace

void poolGeometry(FramePassContext& fc)
{
    const FrameContext& frame = fc.frame;
    PoolState& state = fc.state<PoolState>("W.pools");
    std::vector<FluidSurfaceInput::Basin>& basins = fc.state<std::vector<FluidSurfaceInput::Basin>>("W.poolBasins");
    basins.clear();  // this frame's recorded basins (the fluids' seam, WaterTrack waterFluids)
    // A restore (save load, snapshot) starts every basin calm: the ripples are not World state.
    if (frame.discontinuity & kDiscontinuityRestore) state.slots.clear();
    std::unordered_set<uint32_t> present;
    for (uint32_t i = 0; i < frame.poolCount; ++i) present.insert(frame.pools[i].id);
    if (present.size() != frame.poolCount || present.count(0)) fail("W: the frame's %u basins need unique nonzero ids", frame.poolCount);
    for (auto it = state.slots.begin(); it != state.slots.end();)  // basins no longer in the list (the GPU release is deferred)
        it = present.count(it->first) ? std::next(it) : state.slots.erase(it);

    std::vector<PoolSource> sources;
    for (uint32_t i = 0; i < frame.poolCount; ++i)
    {
        const PoolFrame& in = frame.pools[i];
        PoolDesc desc;
        desc.sizeX = in.sizeX;
        desc.sizeZ = in.sizeZ;
        desc.depth = in.depth;
        desc.surfaceFilm = in.surfaceFilm;
        PoolSlot& slot = state.slots[in.id];
        // A new basin, another basin under the same id, or a clock that went back (a host time reset): start calm.
        if (!slot.pool || !sameBasin(slot.desc, desc) || (slot.pool->started() && frame.time < slot.pool->time()))
        {
            slot.pool.reset();
            slot.pool = std::make_unique<Pool>(fc.device, fc.shaders, desc);
            slot.desc = desc;
        }
        PoolPlacement placement;
        placement.centre[0] = in.centre[0], placement.centre[1] = in.centre[1], placement.centre[2] = in.centre[2];
        placement.yaw = in.yaw;
        bool visible = Pool::visible(desc, placement, frame.mainView.viewProj);
        for (const AuxView& a : frame.auxViews) visible = visible || Pool::visible(desc, placement, a.view.viewProj);
        if (!visible && !in.sourceCount) continue;  // evolved exactly when next drawn or disturbed
        if (fc.resources.triangleStreams.size() >= kMaxTriangleStreams) fail("W: basin %u exceeds the frame's %u triangle streams", in.id, kMaxTriangleStreams);
        sources.resize(in.sourceCount);
        for (uint32_t s = 0; s < in.sourceCount; ++s)
        {
            const PoolSourceFrame& f = in.sources[s];
            sources[s] = PoolSource{ f.x, f.z, f.radius, f.impulse, f.volume };
        }
        PoolOutput out = slot.pool->record(fc.graph, frame.frameIndex, placement, frame.time, frame.deltaTime, sources);
        out.stream.material = in.material;
        {
            FluidSurfaceInput::Basin b;
            for (int a = 0; a < 3; ++a) b.centre[a] = float(placement.centre[a]);
            b.cosYaw = float(std::cos(double(placement.yaw)));
            b.sinYaw = float(std::sin(double(placement.yaw)));
            b.sizeX = desc.sizeX;
            b.sizeZ = desc.sizeZ;
            b.field = out.field;
            basins.push_back(b);
        }
        // Calm water (A14): the still level is the surface's rest plane; the reflection camera serves the samples that lie
        // on it (WaterSurface.hlsli's image-shift bound), ripples keep their reflection rays.
        WaterPlane rest;
        rest.stream = uint32_t(fc.resources.triangleStreams.size());
        rest.plane = { 0, 1, 0, -float(placement.centre[1]) };
        const float3 lo = out.stream.boundsMin, hi = out.stream.boundsMax;
        const float y = float(placement.centre[1]);
        rest.corners[0] = { lo.x, y, lo.z }, rest.corners[1] = { hi.x, y, lo.z }, rest.corners[2] = { lo.x, y, hi.z }, rest.corners[3] = { hi.x, y, hi.z };
        addWaterPlane(fc, rest);
        fc.resources.triangleStreams.push_back(out.stream);
    }
}
} // namespace unx::water
