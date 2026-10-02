// The sky's celestial objects (B4, FEATURES_GAME 11; owner S): the moon's disk, the stars and the airglow for a sky
// pixel, radiance before exposure (Celestial.h: record layout, models and conditions). Consumer: M's sky pass (and any
// view that draws the sky), which adds atmosphereCelestial to the atmosphere's sky radiance and, when the record's flags
// bit 0 is set (the frame's directional light is the moon), leaves out its uniform solar disk.
//   moon     a Lambert sphere of the record's albedo lit by the true sun, 4 x 4 subsamples of the pixel's footprint:
//            radiance albedo E_sun max(n . s, 0) / pi at each subsample inside the disk (phase and terminator exact for
//            the model; the disk's edge to 1/16 of a pixel);
//   stars    each star's illuminance E spread by a Gaussian of 0.7 pixel (in the pixel grid of D, Dx, Dy): the sum over
//            the pixels is E within 4e-4 at any sub-pixel position [measured, NightSkyTests] (the grid samples a Gaussian
//            of that width closely), so a moving sky does not flicker; cells of a cube grid in equatorial coordinates;
//   airglow  an emission layer at 90 km: the zenith radiance x van Rhijn's slant factor - here unless the record's flags
//            bit 3 says the far-field sky has it (SkyView.hlsl: every sky reader then takes it, the escaping rays too);
// each times the atmosphere's transmittance to space along the pixel direction (none without the LUT).
#ifndef UNX_ATMOSPHERE_CELESTIAL_HLSLI
#define UNX_ATMOSPHERE_CELESTIAL_HLSLI
#include "Passes/Atmosphere/Atmosphere.hlsli"

#define CELESTIAL_STAR_SIGMA 0.7

struct CelestialRecord
{
    float3 moonDirection;
    float moonRadius;
    float3 sunDirection;
    float sunIlluminance;
    float3 sunColor;
    float moonAlbedo;
    float3 row0, row1, row2;  // equatorial -> world rotation rows
    float airglow;
    uint flags, starCount, cellsPerFace, starsSrv;
};

CelestialRecord celestialLoad(uint srv)
{
    ByteAddressBuffer b = ResourceDescriptorHeap[srv];
    CelestialRecord c;
    const float4 w0 = asfloat(b.Load4(0)), w1 = asfloat(b.Load4(16)), w2 = asfloat(b.Load4(32));
    c.moonDirection = w0.xyz;
    c.moonRadius = w0.w;
    c.sunDirection = w1.xyz;
    c.sunIlluminance = w1.w;
    c.sunColor = w2.xyz;
    c.moonAlbedo = w2.w;
    c.row0 = asfloat(b.Load3(48));
    c.row1 = asfloat(b.Load3(64));
    c.row2 = asfloat(b.Load3(80));
    const uint4 w6 = b.Load4(96);
    c.airglow = asfloat(w6.x);
    c.flags = w6.y;
    c.starCount = w6.z;
    c.cellsPerFace = w6.w;
    c.starsSrv = b.Load(112);
    return c;
}

// Whether the frame's directional light is the moon (the sky pass leaves out its uniform solar disk).
bool celestialMoonHoldsLight(uint srv)
{
    if (srv == UNX_NONE) return false;
    ByteAddressBuffer b = ResourceDescriptorHeap[srv];
    return (b.Load(100) & 1u) != 0;
}

// Cube cell of an equatorial direction (Celestial.cpp cellOf: the same mapping).
uint celestialCell(float3 e, uint n)
{
    const float3 a = abs(e);
    uint face;
    float2 uv;
    if (a.x >= a.y && a.x >= a.z) { face = e.x > 0 ? 0 : 1; uv = float2(e.y, e.z) / a.x; }
    else if (a.y >= a.z) { face = e.y > 0 ? 2 : 3; uv = float2(e.x, e.z) / a.y; }
    else { face = e.z > 0 ? 4 : 5; uv = float2(e.x, e.y) / a.z; }
    const uint2 ij = min(uint2(max((uv + 1) * 0.5 * n, 0.0)), n - 1);
    return (face * n + ij.y) * n + ij.x;
}

// Radiance of the moon, the stars and the airglow in the pixel whose ray is D (unnormalised) with differentials Dx, Dy
// to the neighbouring pixels (M's mPixelRay), before exposure. celestialSrv = FrameResources::celestial (UNX_NONE: 0).
float3 atmosphereCelestial(AtmosphereSrvs atm, uint celestialSrv, float3 D, float3 Dx, float3 Dy)
{
    if (celestialSrv == UNX_NONE) return 0;
    const CelestialRecord c = celestialLoad(celestialSrv);
    const float3 d = normalize(D);
    const float3 ex = normalize(D + Dx) - d, ey = normalize(D + Dy) - d;  // one pixel along each axis, in angle
    const float omega = max(length(cross(ex, ey)), 1e-20);                 // the pixel's solid angle
    float3 L = 0;
    if (c.airglow > 0 && (c.flags & 8u) == 0)  // (flags bit 3: the sky view LUT holds the airglow - atmosphere.night_sky_in_lut)
    {
        const float k = 6360.0 / 6450.0;  // emission layer at 90 km over the planet's surface
        L += c.airglow / sqrt(max(1 - k * k * (1 - d.y * d.y), 1e-4));
    }
    if ((c.flags & 2u) != 0 && dot(d, c.moonDirection) > cos(min(c.moonRadius + 2 * (length(ex) + length(ey)), 3.14159)))
    {
        float3 t, b;
        const float3 m = c.moonDirection;
        const float3 up = abs(m.y) < 0.9 ? float3(0, 1, 0) : float3(1, 0, 0);
        t = normalize(cross(up, m));
        b = cross(m, t);
        const float sinR = sin(c.moonRadius);
        float lit = 0;
        [unroll] for (uint k2 = 0; k2 < 16; ++k2)
        {
            const float2 f = (float2(k2 % 4, k2 / 4) + 0.5) / 4 - 0.5;
            const float3 s = normalize(D + Dx * f.x + Dy * f.y);
            const float3 o = s - m * dot(s, m);
            const float x = dot(o, t) / sinR, y = dot(o, b) / sinR, r2 = x * x + y * y;
            if (r2 < 1 && dot(s, m) > 0) lit += max(dot(t * x + b * y - m * sqrt(1 - r2), c.sunDirection), 0.0);
        }
        L += c.moonAlbedo * c.sunIlluminance * c.sunColor * (lit / 16) / ATMO_PI;
    }
    if ((c.flags & 4u) != 0 && c.starCount > 0 && c.starsSrv != UNX_NONE)
    {
        // World -> equatorial (the transpose of the record's rotation), the pixel's axes too.
        const float3 e = c.row0 * d.x + c.row1 * d.y + c.row2 * d.z;
        const float3 qx = c.row0 * ex.x + c.row1 * ex.y + c.row2 * ex.z, qy = c.row0 * ey.x + c.row1 * ey.y + c.row2 * ey.z;
        const float a11 = dot(qx, qx), a12 = dot(qx, qy), a22 = dot(qy, qy), inv = 1 / max(a11 * a22 - a12 * a12, 1e-30);
        ByteAddressBuffer stars = ResourceDescriptorHeap[c.starsSrv];
        const uint n = c.cellsPerFace, cell = celestialCell(e, n);
        const uint2 range = stars.Load2(8 * cell);
        const uint records = 8 * 6 * n * n;
        const float norm = 1 / (2 * ATMO_PI * CELESTIAL_STAR_SIGMA * CELESTIAL_STAR_SIGMA * omega);
        [loop] for (uint k3 = 0; k3 < range.y; ++k3)
        {
            const uint at = records + 32 * (range.x + k3);
            const float3 v = asfloat(stars.Load3(at)) - e;
            const float b1 = dot(v, qx), b2 = dot(v, qy);
            const float px = (a22 * b1 - a12 * b2) * inv, py = (a11 * b2 - a12 * b1) * inv;
            const float r2 = px * px + py * py;
            if (r2 < 25 * CELESTIAL_STAR_SIGMA * CELESTIAL_STAR_SIGMA && dot(asfloat(stars.Load3(at)), e) > 0)
                L += asfloat(stars.Load3(at + 16)) * (exp(-r2 / (2 * CELESTIAL_STAR_SIGMA * CELESTIAL_STAR_SIGMA)) * norm);
        }
    }
    if (atm.transmittance != UNX_NONE)
    {
        const AtmosphereParams a = airParamsFromTexels(atm.transmittance);
        L *= airSunTransmittance(a, atm.transmittance, g_cameraPosition, d);
    }
    return L;
}
#endif
