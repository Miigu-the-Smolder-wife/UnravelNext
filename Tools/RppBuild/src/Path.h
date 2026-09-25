#pragma once
// RPP-1 120 s path (rpp1_manifest.json "camera_track", "events", "environment"): one sample per 60 Hz tick of the camera
// (position, forward, up, FOV, EV100), sun, medium, wind, the active section and the events (section swaps, combat,
// gravity, VFX peak). Written as rpp1_path.json; both hosts (native gate, Unity Player) replay it verbatim.
#include "World.h"

#include <string>
#include <vector>

namespace unx::rpp
{
constexpr uint32_t kTickHz = 60, kPathTicks = 120 * kTickHz;
constexpr uint32_t kSwapTicks[3] = { 1500, 3300, 5220 };  // city->forest, forest->waterside, waterside->interior

struct CameraSample
{
    float3 position{}, forward{ 0, 0, -1 }, up{ 0, 1, 0 };
    float fovDeg = 60, ev100 = 13;
    bool cut = false;  // this tick starts a new shot (history discontinuity for the renderer, INTERFACES 5.5.2)
};

struct PathReport
{
    float minTrunkClearance = 1e30f, minCrownClearance = 1e30f, minStaticClearance = 1e30f;
    uint32_t worstTrunkTick = 0, worstCrownTick = 0, worstStaticTick = 0;
    std::string worstStatic;
    float maxSpeed = 0;
    uint32_t maxSpeedTick = 0;
};

std::vector<CameraSample> buildCameraTrack(const World& world, PathReport& report);
// The whole path file (camera, environment, sections, events) as JSON text.
std::string pathJson(const World& world, const std::vector<CameraSample>& camera, const PathReport& report);
} // namespace unx::rpp
