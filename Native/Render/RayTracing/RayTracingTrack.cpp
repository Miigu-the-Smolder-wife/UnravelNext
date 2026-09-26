// Track entry point of R (acceleration structures, INTERFACES_KO.md 5.2; ARCHITECTURE 2.12, 4.1 C2).
#include "unx/render/Tracks.h"
#include "unx/rt/RayPipeline.h"
#include "unx/rt/RayScene.h"

#include <cstring>

namespace unx::render::tracks
{
void accelerationStructures(FramePassContext& fc)
{
    if (!fc.scene.source())
    {
        pending("R.accelerationStructures (no scene)");
        return;
    }
    rt::RayScene& rays = rt::RayScene::get(fc);
    rays.record(fc);
    // S's rain shadow map (B6, WeatherField.hlsli): published by the atmosphere track (record and texture), traced here
    // once this frame's TLAS refs exist. 512^2 texels (AtmosphereSystem.cpp publishWeather: the same count).
    if (fc.resources.rainShadow.valid() && fc.resources.weather != UINT32_MAX)
    {
        constexpr uint32_t kTexels = 512;
        const TextureRef map = fc.resources.rainShadow;
        const uint32_t record = fc.resources.weather;
        rt::RayPipeline& pipeline = rt::RayPipeline::get(fc.device, fc.shaders, rt::standardRayPipeline("Passes/Atmosphere/RainShadow", { "RainShadowGen" }));
        uint32_t scene[8];
        rays.rootConstants(scene);
        fc.graph.addPass("s.weather.rainmap", QueueType::Compute,
                         [&](PassBuilder& b) {
                             b.use(map, Use::UavCompute);
                             rays.declareTraversal(b);
                         },
                         [&pipeline, map, record, scene](PassContext& c) {
                             uint32_t k[32] = {};
                             k[0] = c.uav(map);
                             k[1] = kTexels;
                             k[2] = record;
                             const float reach = 600.0f;  // the record's word 7 (publishWeather kReach)
                             std::memcpy(&k[3], &reach, 4);
                             std::memcpy(&k[24], scene, sizeof scene);
                             c.computeConstants(k, 32);
                             pipeline.dispatch(c.cmd, 0, kTexels, kTexels, 1);
                         });
    }
}
} // namespace unx::render::tracks
