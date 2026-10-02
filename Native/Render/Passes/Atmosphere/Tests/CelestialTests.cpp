// Time of day (Celestial.h, B4) against published values (CPU only, no GPU):
//  1. Julian day: Meeus example 7.a (1957 Oct 4.81 = JD 2436116.31) and J2000.0 (2000 Jan 1.5 = 2451545.0).
//  2. Greenwich mean sidereal time: Meeus example 12.a (1987 Apr 10, 0h UT: 13h10m46.3668s = 197.693195 deg).
//  3. Sun: Meeus example 25.a (1992 Oct 13.0: apparent RA 198.38083 deg, Dec -7.78507 deg); the low-precision series
//     claims 0.01 deg (it has no nutation or aberration: 0.006 deg of that budget).
//  4. Moon: Meeus example 47.a (1992 Apr 12.0: geocentric longitude 133.162655, latitude -3.229126 deg, distance
//     368409.7 km); the six-term series claims ~0.3 deg in longitude, ~0.2 deg in latitude.
//  5. Solstice noon in Seoul (37.5665 N, 126.978 E, 2026 Jun 21): the day's highest sun at 90 - 37.5665 + 23.43 deg.
//  6. Phase: the full moon of 2024 Oct 17 11:26 UT and the new moon of 2024 Oct 2 18:49 UT.
//  7. The directional light: the sun above civil twilight's end, the moon (a fraction of a lux at full) below.
#include "../Celestial.h"
#include "unx/core/Log.h"

#include <cmath>
#include <algorithm>
#include <cstdio>

using namespace unx;
using namespace unx::render;

namespace
{
constexpr double kDeg = 3.14159265358979323846 / 180;
double raDeg(float3 e) { const double a = std::atan2((double)e.y, (double)e.x) / kDeg; return a < 0 ? a + 360 : a; }
double decDeg(float3 e) { return std::asin((double)e.z) / kDeg; }
double angleDiff(double a, double b) { double d = std::fmod(a - b + 540.0, 360.0) - 180.0; return std::fabs(d); }
bool check(bool ok, const char* what, double got, double want, double tolerance)
{
    logf("  %-46s %12.6f against %12.6f (|diff| %.6f, allowed %.6f) %s\n", what, got, want, std::fabs(got - want), tolerance, ok ? "ok" : "FAIL");
    return ok;
}
} // namespace

int main()
{
    bool pass = true;
    {
        const double a = sky::julianDay(1957, 10, 4, 0.81 * 24), b = sky::julianDay(2000, 1, 1, 12);
        pass &= check(std::fabs(a - 2436116.31) < 1e-6, "Julian day 1957 Oct 4.81", a, 2436116.31, 1e-6);
        pass &= check(std::fabs(b - 2451545.0) < 1e-9, "Julian day 2000 Jan 1.5", b, 2451545.0, 1e-9);
    }
    {
        sky::CelestialTime t;
        t.year = 1987, t.month = 4, t.day = 10, t.hoursUt = 0;
        const double g = sky::celestial(t).gmstDeg;
        pass &= check(angleDiff(g, 197.693195) < 1e-5, "GMST 1987 Apr 10 0h UT (deg)", g, 197.693195, 1e-5);
    }
    {
        sky::CelestialTime t;
        t.year = 1992, t.month = 10, t.day = 13, t.hoursUt = 0;
        const sky::CelestialState s = sky::celestial(t);
        pass &= check(angleDiff(raDeg(s.sunEquatorial), 198.38083) < 0.01, "sun RA 1992 Oct 13.0 (deg)", raDeg(s.sunEquatorial), 198.38083, 0.01);
        pass &= check(std::fabs(decDeg(s.sunEquatorial) + 7.78507) < 0.01, "sun Dec 1992 Oct 13.0 (deg)", decDeg(s.sunEquatorial), -7.78507, 0.01);
    }
    {
        sky::CelestialTime t;
        t.year = 1992, t.month = 4, t.day = 12, t.hoursUt = 0;
        const sky::CelestialState s = sky::celestial(t);
        // Back to ecliptic coordinates (obliquity 23.44 deg at 1992).
        const double eps = 23.4406 * kDeg;
        const float3 e = s.moonEquatorial;
        const double x = e.x, y = std::cos(eps) * e.y + std::sin(eps) * e.z, z = -std::sin(eps) * e.y + std::cos(eps) * e.z;
        double lambda = std::atan2(y, x) / kDeg;
        if (lambda < 0) lambda += 360;
        const double beta = std::asin(z) / kDeg;
        pass &= check(angleDiff(lambda, 133.162655) < 0.3, "moon longitude 1992 Apr 12.0 (deg)", lambda, 133.162655, 0.3);
        pass &= check(std::fabs(beta + 3.229126) < 0.2, "moon latitude 1992 Apr 12.0 (deg)", beta, -3.229126, 0.2);
        pass &= check(std::fabs(s.moonGeocentricDistanceKm - 368409.7) < 1000, "moon distance 1992 Apr 12.0 (km)", s.moonGeocentricDistanceKm, 368409.7, 1000);
    }
    {
        sky::CelestialTime t;
        t.year = 2026, t.month = 6, t.day = 21;
        double best = -90;
        for (double h = 0; h < 24; h += 1.0 / 600) {  // 6 s steps
            t.hoursUt = h;
            best = std::max(best, sky::celestial(t).sunAltitudeDeg);
        }
        const double want = 90 - 37.5665 + 23.4358;  // obliquity at 2026 (23.4358 deg), solstice declination
        pass &= check(std::fabs(best - want) < 0.03, "Seoul solstice noon altitude (deg)", best, want, 0.03);
    }
    {
        sky::CelestialTime full, fresh;
        full.year = 2024, full.month = 10, full.day = 17, full.hoursUt = 11 + 26 / 60.0;
        fresh.year = 2024, fresh.month = 10, fresh.day = 2, fresh.hoursUt = 18 + 49 / 60.0;
        const sky::CelestialState a = sky::celestial(full), b = sky::celestial(fresh);
        pass &= check(a.moonPhaseAngleDeg < 6 && a.moonIlluminatedFraction > 0.997, "full moon 2024 Oct 17: phase angle (deg)", a.moonPhaseAngleDeg, 0, 6);
        pass &= check(b.moonPhaseAngleDeg > 174 && b.moonIlluminatedFraction < 0.003, "new moon 2024 Oct 2: phase angle (deg)", b.moonPhaseAngleDeg, 180, 6);
        // The light slot: noon (sun), and the full moon's night in Seoul (moon, a fraction of a lux).
        scene::Sun top;
        sky::CelestialTime noon = full;
        noon.latitudeDeg = 37.5665, noon.longitudeDeg = 126.978, noon.hoursUt = 3.5;
        const sky::DirectionalLight day = sky::directionalLight(sky::celestial(noon), top);
        sky::CelestialTime night = noon;
        night.hoursUt = 15.0;  // midnight local
        const sky::CelestialState ns = sky::celestial(night);
        const sky::DirectionalLight moon = sky::directionalLight(ns, top);
        pass &= check(!day.moon && day.sun.illuminance == top.illuminance, "noon: the sun holds the slot (lux)", day.sun.illuminance, top.illuminance, 0);
        pass &= check(moon.moon && moon.sun.illuminance > 0.15f && moon.sun.illuminance < 0.3f && ns.moonAltitudeDeg > 20,
                      "full-moon midnight: the moon holds the slot (lux)", moon.sun.illuminance, 0.21, 0.08);
        logf("  full-moon midnight: moon altitude %.1f deg, disk radius %.4f deg\n", ns.moonAltitudeDeg, moon.sun.angularRadius / kDeg);
        // Through twilight (the same full moon, the sun's altitude set by hand): the sun keeps the slot while its twilight
        // gives the ground more than the moon does (0.45 lux at 8 degrees of depression against the full moon's 0.2 lux
        // or less), the moon takes it once it gives more (0.022 lux at 11 degrees), and never with the moon down.
        sky::CelestialState tw = ns;
        tw.sunAltitudeDeg = -8;
        const sky::DirectionalLight at8 = sky::directionalLight(tw, top);
        tw.sunAltitudeDeg = -11;
        const sky::DirectionalLight at11 = sky::directionalLight(tw, top);
        tw.sunAltitudeDeg = -30;
        tw.moonAltitudeDeg = -5;
        const sky::DirectionalLight moonDown = sky::directionalLight(tw, top);
        pass &= check(!at8.moon && at8.sun.illuminance == top.illuminance, "sun 8 deg down, full moon up: the sun holds the slot (lux)", at8.sun.illuminance,
                      top.illuminance, 0);
        pass &= check(at11.moon && at11.sun.illuminance < 0.3f, "sun 11 deg down, full moon up: the moon holds the slot (lux)", at11.sun.illuminance, 0.21, 0.09);
        pass &= check(!moonDown.moon, "sun 30 deg down, moon below the horizon: the sun keeps the slot (moon flag)", moonDown.moon ? 1.0 : 0.0, 0, 0);
        pass &= check(std::fabs(sky::twilightIlluminance(6) / 3.4 - 1) < 0.01 && std::fabs(sky::twilightIlluminance(12) / 8.0e-3 - 1) < 0.01 &&
                          sky::twilightIlluminance(40) == sky::twilightIlluminance(18),
                      "twilight illuminance at the end of civil twilight (lux)", sky::twilightIlluminance(6), 3.4, 0.034);
    }
    logf("RESULT %s\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}
