#pragma once
// RPP-1 environment tracks (rpp1_manifest.json "environment"): the astronomical sun (latitude 37.5 N, 2026-09-26, solar
// time 15:30 + t minutes), the weather states and their 14 s transitions (log-space smoothstep for coefficients, linear
// for albedo and g), and the wind. Evaluated at any path time t (s); the path samples them every tick.
#include "unx/scene/SceneData.h"

namespace unx::rpp
{
struct SunState
{
    float elevationDeg = 0, azimuthCompassDeg = 0;
    float3 direction{};  // unit, ground -> sun (SceneGen convention: x = cos e cos a, z = cos e sin a, a = compass - 90)
};
SunState sunAt(float t);

enum class Weather { Clear, Rain, Mist };
scene::Atmosphere weatherAtmosphere(Weather w);
// The medium at path time t and the state names around it ("rain", "rain->clear", ...).
scene::Atmosphere atmosphereAt(float t, const char** label = nullptr);

struct WindState
{
    float3 direction{};
    float speed = 0;
};
WindState windAt(float t);

float ev100At(float t);  // camera exposure track (section values, 2 s ramps)
} // namespace unx::rpp
