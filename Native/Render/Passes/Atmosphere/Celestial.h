#pragma once
// Time of day (FEATURES_GAME 11, B4; owner S, time and place from the World): the sun's and the moon's directions, the
// moon's phase and the star field's rotation for a date, a time and a place on Earth, and the one directional light of
// the frame (the VSM path has one): the sun, or the moon once the sun is below civil twilight's end.
//
// Models (low-precision series of the Astronomical Almanac / Meeus, "Astronomical Algorithms" 2nd ed.):
//   sun   ecliptic longitude L + 1.915 sin g + 0.020 sin 2g, obliquity 23.439 - 4e-7 n: 0.01 deg (1950-2050);
//   moon  the six largest longitude terms, four latitude terms, the parallax series: ~0.3 deg in longitude, ~0.2 deg in
//         latitude (the disk is 0.5 deg wide: its place is right to about half its size), topocentric by the parallax;
//   stars equatorial directions turned by the local sidereal time (GMST, IAU 1982 linear terms: 0.1 s over decades).
// No refraction (0.5 deg at the horizon: the atmosphere LUT has no refraction either), no precession of the catalogue
// (J2000 positions: 0.35 deg in 2026 along the ecliptic, the same order as the moon's model).
// World frame: +x east, +y up, -z north (the scene's y-up convention; north is a game's authored choice of -z).
#include "unx/core/Math.h"
#include "unx/render/FrameContext.h"
#include "unx/scene/SceneData.h"

#include <string>
#include <vector>

namespace unx::render::sky
{
struct CelestialTime
{
    int year = 2026, month = 6, day = 21;
    double hoursUt = 12;          // Universal Time (hours; fractional; may be outside [0, 24): days carry)
    double latitudeDeg = 37.5665;  // north positive
    double longitudeDeg = 126.978; // east positive
};

struct CelestialState
{
    float3 sun{ 0, 1, 0 };   // unit, towards the sun
    float3 moon{ 0, 1, 0 };  // unit, towards the moon (topocentric)
    double sunAltitudeDeg = 0, moonAltitudeDeg = 0;
    double moonPhaseAngleDeg = 0;   // angle sun - moon - observer (0 = full, 180 = new)
    double moonIlluminatedFraction = 0;
    double moonDistanceKm = 384400;
    float3x4 equatorialToWorld;     // rotation (3 x 3 part): J2000 equatorial unit vectors (x to RA 0, z to the pole) -> world
    double julianDay = 0;
    double gmstDeg = 0;                                // Greenwich mean sidereal time
    float3 sunEquatorial{}, moonEquatorial{};          // geocentric equatorial unit vectors (tests, star-field work)
    double moonGeocentricDistanceKm = 384400;
};

double julianDay(int year, int month, int day, double hoursUt);
CelestialState celestial(const CelestialTime& t);

// The frame's directional light (the scene's Sun record): the sun while it is up, and after sunset while the twilight
// it still gives level ground (twilightIlluminance: the standard clear-sky values, scaled by the scene's sun over
// 128 klx) is at least what the moon gives it directly; then the moon - with a full moon high in the sky near 9 degrees
// of depression, later for a thin or low moon, never with the moon down (the sun's twilight is in the atmosphere's
// tables to 23.6 degrees below the horizon; the slot flipped at -6 degrees before, where the twilight sky still gave
// ten times the full moon's light and went dark with the flip). Moon illuminance at the observer: a Lambert sphere of albedo 'moonAlbedo' lit by the sun,
// E = albedo x E_sun x (R_moon / d)^2 x (2 / 3) x phi(alpha), phi(alpha) = (sin alpha + (pi - alpha) cos alpha) / pi,
// no opposition surge (the full moon ~0.2 lux here against 0.25-0.3 lux measured: condition recorded).
struct DirectionalLight
{
    scene::Sun sun;       // what to put into Scene::sun
    bool moon = false;    // the slot holds the moon (the sky draws the moon's disk with its phase, not a uniform disk)
};
DirectionalLight directionalLight(const CelestialState& s, const scene::Sun& sunAtTop, float moonAlbedo = 0.12f);
// The twilight sky's illuminance on level ground under a clear sky, the sun depressionDeg below the horizon (lux).
double twilightIlluminance(double depressionDeg);

// The sky's celestial objects for a frame (FrameContext::celestial; Celestial.hlsli atmosphereCelestial): the moon's disk
// (a Lambert sphere lit by the true sun: its phase and terminator exact for that model), the stars (point sources spread
// by a pixel-scale Gaussian, energy exact), the airglow (a constant emission layer at 90 km, van Rhijn's slant factor),
// each times the atmosphere's transmittance to space. celestialFrame sets flags 2 | 4 (moon and stars), and 1 when the
// moon holds the directional light.
using CelestialFrame = unx::render::CelestialFrame;
CelestialFrame celestialFrame(const CelestialState& s, const DirectionalLight& light, const scene::Sun& sunAtTop, float moonAlbedo = 0.12f,
                              float airglowRadiance = 2.0e-4f);

// A star: J2000 equatorial unit vector and its illuminance above the atmosphere (RGB lux, V magnitude m:
// Y = 2.54e-6 x 10^(-0.4 m), colour of a black body at the B-V temperature).
struct Star
{
    float3 equatorial;
    float3 illuminance;
};
// A statistically real field until a catalogue is supplied (the Yale Bright Star Catalogue is the design's source; it
// needs the user's approval to download): counts N(< m) = 10^(0.5 m + 0.7) to m 6.5 (~8,900 stars), density toward
// the galactic plane (1 + 2 exp(-|b| / 15 deg)), B-V drawn from 0 to 1.6. Deterministic in 'seed'.
std::vector<Star> syntheticStars(uint64_t seed = 1);
// Binary catalogue: records of { RA deg, Dec deg, V mag, B-V } float32 (converted from any source); empty on failure.
std::vector<Star> loadStarCatalogue(const std::string& path);

// GPU layout (Celestial.hlsli): the frame record (32 words, a raw buffer per frame; word 28 = the star buffer's SRV), and
// the star buffer (static): cells (6 cube faces x n x n cells of { first record, count }) then records ({ equatorial xyz,
// 0 }, { illuminance rgb, 0 }), cell by cell. A star goes into every cell within kStarMargin of it (its spread never
// crosses a cell it is not in).
constexpr uint32_t kStarCellsPerFace = 32;
constexpr float kStarMargin = 0.0087f;  // 0.5 deg: 3 x the spread at a 0.17 deg pixel
std::vector<uint32_t> packStars(const std::vector<Star>& stars);
void packCelestialFrame(const CelestialFrame& f, uint32_t starCount, uint32_t starBufferSrv, uint32_t words[32]);
} // namespace unx::render::sky
