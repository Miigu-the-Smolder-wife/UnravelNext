// W2 closed basins in the frame (FEATURES_GAME 1.10; INTERFACES v1.78): FrameContext::pools -> one unx::water::Pool per id
// (track state "W.pools"), evolved in the frames where a view can see the basin or it has sources, and pushed as layer-1
// triangle streams before V. Called by W's waterGeometry.
#include "unx/water/FluidSurface.h"
#include "unx/water/Pool.h"
#include "unx/water/RoundPool.h"
#include "unx/water/WaterSurface.h"

#include "unx/render/Frame.h"
#include "unx/core/Log.h"

#include <algorithm>
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
struct RoundSlot
{
    std::unique_ptr<RoundPool> pool;
    RoundPoolDesc desc;
};
struct PoolState
{
    std::unordered_map<uint32_t, PoolSlot> slots;
    std::unordered_map<uint32_t, RoundSlot> roundSlots;  // W2-R (v1.92): the round basins (PoolFrame::shape 1)
    std::vector<std::pair<uint32_t, PoolStats>> stats;  // the latest valid statistics of the basins in the set (render thread)
};
bool sameBasin(const PoolDesc& a, const PoolDesc& b)
{
    return a.sizeX == b.sizeX && a.sizeZ == b.sizeZ && a.depth == b.depth && a.surfaceFilm == b.surfaceFilm;
}

// Rain on a basin (the frame's weather record x PoolFrame::rainExposure): the drops that reach the surface in this frame
// as sources - each a vertical impulse m v at a place of its own. The rain is taken as drops of one size, the
// mass-weighted mean diameter of the Marshall-Palmer distribution at the rate R mm/h (D = 4 / Lambda, Lambda = 4.1
// R^-0.21 per mm: 1.6 mm at 10 mm/h), falling at 3.78 D^0.67 m/s (Atlas & Ulbrich; not over 9.2), as many of them as
// carry the rate's water: R / 3.6e6 m/s x the basin's area over a drop's volume - so the momentum the rain brings is the
// rate's, in drops of the size that carries most of it (the small drops' ripples are below the basin's samples
// anyway: a drop's footprint is the sample spacing, PoolSplat.hlsl). At most 'room' sources a frame: beyond that fewer
// drops carry the same momentum. Places from a hash of the frame, the basin and the drop: the same frame gives the
// same rain. Returns world (x, z) and the impulse of each (basin coordinates as Pool.cpp / RoundPool.cpp map them).
struct RainDrop
{
    double x, z;
    float radius, impulse;
};
void rainOnBasin(const FrameContext& frame, const PoolFrame& in, uint32_t room, std::vector<RainDrop>& drops)
{
    drops.clear();
    const double rate = (double)frame.weather.rainRate * std::clamp(in.rainExposure, 0.0f, 1.0f);  // mm/h on the surface
    const double dt = std::clamp((double)frame.deltaTime, 0.0, 0.1);
    if (!(rate > 0) || !(dt > 0) || room == 0) return;
    const bool round = in.shape == 1;
    const double area = round ? 0.785398163 * in.sizeX * in.sizeX : (double)in.sizeX * in.sizeZ;
    const double diameter = 4.0 / (4.1 * std::pow(rate, -0.21)) * 1e-3;                // m
    const double speed = std::min(3.78 * std::pow(diameter * 1e3, 0.67), 9.2);         // m/s
    const double mass = 1000.0 * 0.523598776 * diameter * diameter * diameter;         // kg
    const double expected = rate / 3.6e6 * 1000.0 * area / mass * dt;                  // drops this frame
    auto unit = [&](uint32_t i, uint32_t salt) {
        uint64_t h = frame.frameIndex * 0x9E3779B97F4A7C15ull ^ ((uint64_t)in.id << 32 | i) * 0xC2B2AE3D27D4EB4Full ^ salt * 0x165667B19E3779F9ull;
        h ^= h >> 33, h *= 0xFF51AFD7ED558CCDull, h ^= h >> 33, h *= 0xC4CEB9FE1A85EC53ull, h ^= h >> 33;
        return (double)(h >> 11) * (1.0 / 9007199254740992.0);
    };
    // (the fraction of a drop: one more drop with that probability, so the mean over frames is the rate's)
    const uint64_t whole = (uint64_t)std::floor(expected) + (unit(0xFFFFFFFFu, 3) < expected - std::floor(expected) ? 1u : 0u);
    if (whole == 0) return;
    const uint32_t count = (uint32_t)std::min<uint64_t>(whole, std::min<uint32_t>(room, 256u));
    const float impulse = (float)(mass * speed * (double)whole / count);
    const double c = std::cos((double)in.yaw), sn = std::sin((double)in.yaw);
    for (uint32_t i = 0; i < count; ++i)
    {
        RainDrop d;
        d.radius = (float)diameter;
        d.impulse = impulse;
        if (round)
        {
            // uniform over the disk, kept off the wall (a source's centre lies inside the basin)
            const double rho = 0.49 * in.sizeX * std::sqrt(unit(i, 1)), phi = 6.283185307179586 * unit(i, 2);
            d.x = in.centre[0] + rho * std::cos(phi);
            d.z = in.centre[2] + rho * std::sin(phi);
        }
        else
        {
            // local (a, b) from the centre along the basin's axes: x -> (cos, 0, -sin), z -> (sin, 0, cos)
            const double a = (unit(i, 1) - 0.5) * 0.98 * in.sizeX, b = (unit(i, 2) - 0.5) * 0.98 * in.sizeZ;
            d.x = in.centre[0] + a * c + b * sn;
            d.z = in.centre[2] - a * sn + b * c;
        }
        drops.push_back(d);
    }
}
} // namespace

void poolStatsSnapshot(TrackState& state, std::vector<std::pair<uint32_t, PoolStats>>& out)
{
    out = state.get<PoolState>("W.pools").stats;
}

void poolGeometry(FramePassContext& fc)
{
    const FrameContext& frame = fc.frame;
    PoolState& state = fc.state<PoolState>("W.pools");
    std::vector<FluidSurfaceInput::Basin>& basins = fc.state<std::vector<FluidSurfaceInput::Basin>>("W.poolBasins");
    basins.clear();  // this frame's recorded basins (the fluids' seam, WaterTrack waterFluids)
    // this frame's basin streams: { frame index, stream slot << 32 | pool id ... } (the surface pass marks the streams
    // whose water S put into the air volume: "W.mediaPools", FroxelSystem.cpp recordWaterMedia)
    std::vector<uint64_t>& streams = fc.state<std::vector<uint64_t>>("W.poolStreams");
    streams.assign(1, frame.frameIndex);
    // A restore (save load, snapshot) starts every basin calm: the ripples are not World state.
    if (frame.discontinuity & kDiscontinuityRestore)
    {
        state.slots.clear();
        state.roundSlots.clear();
    }
    std::unordered_set<uint32_t> present;
    for (uint32_t i = 0; i < frame.poolCount; ++i) present.insert(frame.pools[i].id);
    if (present.size() != frame.poolCount || present.count(0)) fail("W: the frame's %u basins need unique nonzero ids", frame.poolCount);
    for (auto it = state.slots.begin(); it != state.slots.end();)  // basins no longer in the list (the GPU release is deferred)
        it = present.count(it->first) ? std::next(it) : state.slots.erase(it);
    for (auto it = state.roundSlots.begin(); it != state.roundSlots.end();)
        it = present.count(it->first) ? std::next(it) : state.roundSlots.erase(it);
    std::erase_if(state.stats, [&](const auto& e) { return !present.count(e.first); });

    std::vector<PoolSource> sources;
    std::vector<RainDrop> rain;
    // A basin out of the main view may still be in a reflection view of the frame (a mirror behind the camera's back shows
    // it; those views are made later, in the shading) and on the reflection rays: with shading.water_secondary_views a
    // basin whose centre is within 30 m of the camera is drawn as a visible one.
    const bool secondary = !fc.quality.has("shading.water_secondary_views") || fc.quality.boolean("shading.water_secondary_views");
    auto nearCamera = [&](const PoolFrame& p) {
        if (!secondary) return false;
        const double dx = p.centre[0] - frame.mainView.position.x, dy = p.centre[1] - frame.mainView.position.y, dz = p.centre[2] - frame.mainView.position.z;
        return dx * dx + dy * dy + dz * dz < 30.0 * 30.0;
    };
    for (uint32_t i = 0; i < frame.poolCount; ++i)
    {
        const PoolFrame& in = frame.pools[i];
        if (in.shape == 1)
        {
            // W2-R round basin (defect queue 13 (74); RoundPool): Bessel modes in a circle of diameter sizeX. No fluid
            // seam (FluidSurface clips to rectangles) and no statistics yet.
            state.slots.erase(in.id);  // (a basin that changed shape)
            RoundPoolDesc rd;
            rd.radius = 0.5f * in.sizeX;
            rd.depth = in.depth;
            rd.surfaceFilm = in.surfaceFilm;
            RoundSlot& rs = state.roundSlots[in.id];
            if (!rs.pool || rs.desc.radius != rd.radius || rs.desc.depth != rd.depth || rs.desc.surfaceFilm != rd.surfaceFilm || (rs.pool->started() && frame.time < rs.pool->time()))
            {
                rs.pool.reset();
                rs.pool = std::make_unique<RoundPool>(fc.device, fc.shaders, rd);
                rs.desc = rd;
            }
            PoolDesc visDesc;
            visDesc.sizeX = visDesc.sizeZ = in.sizeX;
            visDesc.depth = in.depth;
            PoolPlacement vp;
            vp.centre[0] = in.centre[0], vp.centre[1] = in.centre[1], vp.centre[2] = in.centre[2];
            vp.yaw = in.yaw;
            bool rvisible = Pool::visible(visDesc, vp, frame.mainView.viewProj) || nearCamera(in);
            for (const AuxView& a : frame.auxViews) rvisible = rvisible || Pool::visible(visDesc, vp, a.view.viewProj);
            if (!rvisible && !in.sourceCount) continue;
            if (fc.resources.triangleStreams.size() >= kMaxTriangleStreams) fail("W: basin %u exceeds the frame's %u triangle streams", in.id, kMaxTriangleStreams);
            std::vector<RoundPoolSource> rsources(in.sourceCount);
            for (uint32_t s = 0; s < in.sourceCount; ++s)
            {
                const PoolSourceFrame& f = in.sources[s];
                rsources[s] = RoundPoolSource{ f.x, f.z, f.radius, f.impulse, f.volume };
            }
            // (the rain's drops while the basin is drawn: a basin out of view keeps its calm, as without sources)
            rainOnBasin(frame, in, rd.maxSources > in.sourceCount ? rd.maxSources - in.sourceCount : 0u, rain);
            for (const RainDrop& d : rain) rsources.push_back(RoundPoolSource{ d.x, d.z, d.radius, d.impulse, 0.0f });
            RoundPoolPlacement rp;
            rp.centre[0] = in.centre[0], rp.centre[1] = in.centre[1], rp.centre[2] = in.centre[2];
            rp.yaw = in.yaw;
            RoundPoolOutput rout = rs.pool->record(fc.graph, frame.frameIndex, rp, frame.time, frame.deltaTime, rsources);
            rout.stream.material = in.material;
            WaterPlane rest;
            rest.stream = uint32_t(fc.resources.triangleStreams.size());
            rest.plane = { 0, 1, 0, -float(in.centre[1]) };
            const float3 lo = rout.stream.boundsMin, hi = rout.stream.boundsMax;
            const float y = float(in.centre[1]);
            rest.corners[0] = { lo.x, y, lo.z }, rest.corners[1] = { hi.x, y, lo.z }, rest.corners[2] = { lo.x, y, hi.z }, rest.corners[3] = { hi.x, y, hi.z };
            addWaterPlane(fc, rest);
            streams.push_back((uint64_t)fc.resources.triangleStreams.size() << 32 | in.id);
            fc.resources.triangleStreams.push_back(rout.stream);
            continue;
        }
        state.roundSlots.erase(in.id);  // (a basin that changed shape)
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
        bool visible = Pool::visible(desc, placement, frame.mainView.viewProj) || nearCamera(in);
        for (const AuxView& a : frame.auxViews) visible = visible || Pool::visible(desc, placement, a.view.viewProj);
        if (!visible && !in.sourceCount) continue;  // evolved exactly when next drawn or disturbed
        if (fc.resources.triangleStreams.size() >= kMaxTriangleStreams) fail("W: basin %u exceeds the frame's %u triangle streams", in.id, kMaxTriangleStreams);
        sources.resize(in.sourceCount);
        for (uint32_t s = 0; s < in.sourceCount; ++s)
        {
            const PoolSourceFrame& f = in.sources[s];
            sources[s] = PoolSource{ f.x, f.z, f.radius, f.impulse, f.volume };
        }
        // (the rain's drops while the basin is drawn: a basin out of view keeps its calm, as without sources)
        rainOnBasin(frame, in, desc.maxSources > in.sourceCount ? desc.maxSources - in.sourceCount : 0u, rain);
        for (const RainDrop& d : rain) sources.push_back(PoolSource{ d.x, d.z, d.radius, d.impulse, 0.0f });
        PoolOutput out = slot.pool->record(fc.graph, frame.frameIndex, placement, frame.time, frame.deltaTime, sources);
        out.stream.material = in.material;
        {
            const PoolStats& st = slot.pool->latestStats();
            if (st.valid)
            {
                auto found = std::find_if(state.stats.begin(), state.stats.end(), [&](const auto& e) { return e.first == in.id; });
                if (found == state.stats.end()) state.stats.push_back({ in.id, st });
                else found->second = st;
            }
        }
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
        streams.push_back((uint64_t)fc.resources.triangleStreams.size() << 32 | in.id);
        fc.resources.triangleStreams.push_back(out.stream);
    }
}
} // namespace unx::water
