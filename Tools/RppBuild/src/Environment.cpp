#include "Environment.h"

#include <algorithm>
#include <cmath>

namespace unx::rpp
{
namespace
{
constexpr double kPiD = 3.14159265358979323846;
float smooth(float a, float b, float x)
{
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3 - 2 * t);
}
float3 lerp3(float3 a, float3 b, float t) { return a + (b - a) * t; }
float logLerp(float a, float b, float t) { return std::exp(std::log(a) + (std::log(b) - std::log(a)) * t); }
float3 logLerp3(float3 a, float3 b, float t) { return { logLerp(a.x, b.x, t), logLerp(a.y, b.y, t), logLerp(a.z, b.z, t) }; }

scene::Atmosphere mix(Weather from, Weather to, float t)
{
    const scene::Atmosphere a = weatherAtmosphere(from), b = weatherAtmosphere(to);
    scene::Atmosphere m = a;
    m.mieScattering = logLerp3(a.mieScattering, b.mieScattering, t);
    m.mieAbsorption = logLerp3(a.mieAbsorption, b.mieAbsorption, t);
    m.mieScaleHeight = logLerp(a.mieScaleHeight, b.mieScaleHeight, t);
    m.mieG = a.mieG + (b.mieG - a.mieG) * t;
    m.groundAlbedo = lerp3(a.groundAlbedo, b.groundAlbedo, t);
    return m;
}

struct Span
{
    float t0, t1;
    Weather from, to;
    const char* label;
};
// manifest environment.weather_track
constexpr Span kWeather[] = {
    { 0, 24, Weather::Rain, Weather::Rain, "rain" },          { 24, 38, Weather::Rain, Weather::Clear, "rain->clear" },
    { 38, 52, Weather::Clear, Weather::Clear, "clear" },      { 52, 66, Weather::Clear, Weather::Mist, "clear->mist" },
    { 66, 86, Weather::Mist, Weather::Mist, "mist" },         { 86, 100, Weather::Mist, Weather::Clear, "mist->clear" },
    { 100, 1e9f, Weather::Clear, Weather::Clear, "clear" },
};
} // namespace

SunState sunAt(float t)
{
    // NOAA declination series for day 269; hour angle from local solar time 15:30 + t minutes.
    const double lat = 37.5 * kPiD / 180, g = 2 * kPiD / 365 * (269 - 1);
    const double decl = 0.006918 - 0.399912 * std::cos(g) + 0.070257 * std::sin(g) - 0.006758 * std::cos(2 * g) + 0.000907 * std::sin(2 * g) -
                        0.002697 * std::cos(3 * g) + 0.00148 * std::sin(3 * g);
    const double solarHours = 15.5 + t / 60.0;
    const double H = (15.0 * (solarHours - 12.0)) * kPiD / 180;
    const double el = std::asin(std::sin(lat) * std::sin(decl) + std::cos(lat) * std::cos(decl) * std::cos(H));
    double az = std::atan2(std::sin(H), std::cos(H) * std::sin(lat) - std::tan(decl) * std::cos(lat)) + kPiD;  // from north, clockwise
    az = std::fmod(az, 2 * kPiD);
    const double a = az - kPiD / 2;
    SunState s;
    s.elevationDeg = (float)(el * 180 / kPiD);
    s.azimuthCompassDeg = (float)(az * 180 / kPiD);
    s.direction = normalize(float3{ (float)(std::cos(el) * std::cos(a)), (float)std::sin(el), (float)(std::cos(el) * std::sin(a)) });
    return s;
}

scene::Atmosphere weatherAtmosphere(Weather w)
{
    scene::Atmosphere a;  // Rayleigh, ozone, radii: SceneData defaults (manifest environment.atmosphere_fixed)
    switch (w)
    {
    case Weather::Clear:
        a.mieScattering = { 3.996e-6f, 3.996e-6f, 3.996e-6f };
        a.mieAbsorption = { 0.444e-6f, 0.444e-6f, 0.444e-6f };
        a.mieScaleHeight = 1200;
        a.mieG = 0.8f;
        a.groundAlbedo = { 0.060f, 0.090f, 0.050f };
        break;
    case Weather::Rain:
        a.mieScattering = { 9.27e-4f, 9.27e-4f, 9.27e-4f };
        a.mieAbsorption = { 2.7e-5f, 2.7e-5f, 2.7e-5f };
        a.mieScaleHeight = 1200;
        a.mieG = 0.829f;
        a.groundAlbedo = { 0.050f, 0.078f, 0.042f };
        break;
    case Weather::Mist:
        a.mieScattering = { 9.76e-4f, 9.76e-4f, 9.76e-4f };
        a.mieAbsorption = { 2.0e-6f, 2.0e-6f, 2.0e-6f };
        a.mieScaleHeight = 300;
        a.mieG = 0.85f;
        a.groundAlbedo = { 0.055f, 0.085f, 0.046f };
        break;
    }
    return a;
}

scene::Atmosphere atmosphereAt(float t, const char** label)
{
    for (const Span& s : kWeather)
        if (t < s.t1 || s.t1 >= 1e9f)
        {
            if (label) *label = s.label;
            if (s.from == s.to) return weatherAtmosphere(s.from);
            return mix(s.from, s.to, smooth(s.t0, s.t1, t));
        }
    return weatherAtmosphere(Weather::Clear);
}

WindState windAt(float t)
{
    // manifest environment.wind_track: holds and 14 s smoothstep transitions (direction re-normalised).
    const float3 d0 = normalize(float3{ 0.8f, 0, 0.6f }), d1 = normalize(float3{ 0.6f, 0, 0.8f });
    WindState w;
    if (t < 24) w = { d0, 5.0f };
    else if (t < 38) w = { d0, 5.0f + (3.0f - 5.0f) * smooth(24, 38, t) };
    else if (t < 52) w = { d0, 3.0f };
    else if (t < 66)
    {
        const float k = smooth(52, 66, t);
        w = { normalize(lerp3(d0, d1, k)), 3.0f + (1.5f - 3.0f) * k };
    }
    else if (t < 100) w = { d1, 1.5f };
    else w = { d1, 1.5f + 0.5f * smooth(100, 102, t) };
    return w;
}

float ev100At(float t)
{
    // city 13.0 | forest 12.5 in the stand, 14 above the canopy (50..63 s) | waterside 13.0 | interior 9.5; 2 s ramps.
    struct Key { float t, ev; };
    constexpr Key keys[] = { { 0, 13.0f }, { 30, 13.0f }, { 32, 12.5f }, { 50, 12.5f }, { 52, 14.0f }, { 62, 14.0f }, { 64, 13.0f }, { 91, 13.0f }, { 93, 9.5f }, { 120, 9.5f } };
    if (t <= keys[0].t) return keys[0].ev;
    for (size_t i = 1; i < std::size(keys); ++i)
        if (t <= keys[i].t) return keys[i - 1].ev + (keys[i].ev - keys[i - 1].ev) * smooth(keys[i - 1].t, keys[i].t, t);
    return keys[std::size(keys) - 1].ev;
}
} // namespace unx::rpp
