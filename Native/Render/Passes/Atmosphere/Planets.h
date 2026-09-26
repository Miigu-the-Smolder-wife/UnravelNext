#pragma once
// Planet atmospheres for the one atmosphere model (B4, FEATURES_GAME 11 "other planets"; owner S). The model (Scene::
// Atmosphere: a sphere of the bottom radius, Rayleigh and Mie layers with exponential profiles, an ozone tent, ground
// albedo) covers planets with a thin atmosphere over a solid surface; every consumer (LUTs, J_ms, froxels, GI and
// reflection sky, the reference path tracer) reads the same parameters, so a preset is only data. Values are
// literature estimates [authored], recorded with their derivation:
//   earth  the scene defaults (Hillaire 2020 / Bruneton): sea level, clear air.
//   mars   radius 3389.5 km, top +80 km; CO2 Rayleigh = Earth's x 0.0207 (number density 8.3e-3 x cross-section 2.5),
//          scale height 11.1 km; dust (optical depth 0.5, scale height 11 km, g 0.76, single-scattering albedo
//          0.97 / 0.90 / 0.75: dust absorbs blue -- the butterscotch day sky and the blue sunset), no ozone, ground
//          albedo 0.35 / 0.20 / 0.12; the sun at 1.524 AU: 43 % of the illuminance, 0.66 x the disk.
#include "unx/scene/SceneData.h"

#include <string>

namespace unx::render::sky
{
inline bool planetPreset(const std::string& name, scene::Atmosphere& atmosphere, scene::Sun& sun)
{
    if (name == "earth")
    {
        atmosphere = scene::Atmosphere{};
        sun.illuminance = scene::Sun{}.illuminance;
        sun.angularRadius = scene::Sun{}.angularRadius;
        return true;
    }
    if (name == "mars")
    {
        scene::Atmosphere a;
        a.bottomRadius = 3389500;
        a.topRadius = 3389500 + 80000;
        a.rayleighScaleHeight = 11100;
        a.rayleighScattering = float3{ 5.802e-6f, 13.558e-6f, 33.100e-6f } * 0.0207f;
        const float extinction = 0.5f / 11000;  // optical depth 0.5 over the dust's scale height
        const float3 albedo{ 0.97f, 0.90f, 0.75f };
        a.mieScaleHeight = 11000;
        a.mieScattering = albedo * extinction;
        a.mieAbsorption = (float3{ 1, 1, 1 } - albedo) * extinction;
        a.mieG = 0.76f;
        a.ozoneAbsorption = { 0, 0, 0 };
        a.groundAlbedo = { 0.35f, 0.20f, 0.12f };
        atmosphere = a;
        const float au = 1.524f;
        sun.illuminance = scene::Sun{}.illuminance / (au * au);
        sun.angularRadius = scene::Sun{}.angularRadius / au;
        return true;
    }
    return false;
}
} // namespace unx::render::sky
