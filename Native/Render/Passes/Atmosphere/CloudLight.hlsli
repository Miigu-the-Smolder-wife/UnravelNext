// The cloud's light in a frame: one definition for the layer's march (CloudMarch.hlsl modes 0 and 3) and for the cloud in
// front of surfaces (FogIntegrate.hlsl), so the cloud on a ridge is the cloud the sky pixel beside it shows. Owner: S.
// APPROXIMATION, NOT EXACT (CloudCommon.hlsli cloudMsSun / cloudSkyRamp: the octave series and the sky's height ramp,
// fitted to the CPU path tracer; cloudGroundRamp: not fitted).
#ifndef UNX_CLOUD_LIGHT_HLSLI
#define UNX_CLOUD_LIGHT_HLSLI
#include "Passes/Atmosphere/CloudShadowCommon.hlsli"
#include "Passes/Atmosphere/AtmosphereCommon.hlsli"
#include "Frame.hlsli"

struct CloudFrameLight
{
    // The sun's illuminance at the layer's base and at its top (lux): top-of-atmosphere through the air. A sample takes
    // them by its height in the layer - the air under the layer's top reddens a low sun more for the base than for the
    // top, and at dusk the base is in the planet's shadow first (the reference's per-sample atmospheric light
    // transmittance, bUsePerSampleAtmosphericLightTransmittance, by two lookups per ray).
    float3 sunBase, sunTop;
    float3 sky;     // the sky's radiance over the upper hemisphere (nits): the sky's irradiance on level ground / pi
    float3 ground;  // the ground's radiance over the lower one (nits; 0: atmosphere.clouds.ground_light off)
    CloudMsPhases ms;
    float3 viewDir; // the view ray (the lightning flash's phase function)
};
// at: a point of the view ray's span in the layer (its middle: the light is taken once per ray); dir: the view ray.
CloudFrameLight cloudFrameLight(CloudRecord c, uint transmittanceLut, float3 at, float3 dir, bool groundLight)
{
    CloudFrameLight l;
    const AtmosphereParams ap = airParamsFromTexels(transmittanceLut);
    const float3 E = g_sunIlluminance * g_sunColor;
    const float3 up = airUp(ap, at);
    const float mus = dot(up, c.sunDir);
    const float3 indirect = airGroundIndirect(ap, transmittanceLut, mus);
    const float altitude = cloudAltitude(c, at);
    l.sunBase = E * airSunTransmittance(ap, transmittanceLut, at + up * (c.base - altitude), c.sunDir);
    l.sunTop = E * airSunTransmittance(ap, transmittanceLut, at + up * (c.top - altitude), c.sunDir);
    l.sky = E * indirect * (1 / 3.14159265);
    l.ground = 0;
    if (groundLight)
    {
        // the ground under the point: the sun on it through the air and through the layer above (the sun map: the whole
        // column's optical depth along the sun's ray), and the sky's light; a Lambert ground of the atmosphere's albedo
        const float3 foot = at - up * max(altitude, 0.0);
        const float3 direct = airSunTransmittance(ap, transmittanceLut, foot, c.sunDir) * (max(mus, 0.0) * exp(-cloudSunTau(c, foot)));
        l.ground = ap.groundAlbedo * E * (direct + indirect) * (1 / 3.14159265);
    }
    l.ms = cloudMsPhases(c, dot(dir, c.sunDir));
    l.viewDir = dir;
    return l;
}
// A lightning flash's light at a point of the cloud, per unit of the cloud's density and of its albedo (nits per (1/m)):
// the source's luminous intensity I (cd) at distance r through the cloud between -
//   direct    I / r^2 x exp(-rho r) through the layer's phase function, and
//   diffused  the light that has scattered on the way: in a medium of albedo near 1 it is not lost but spreads, and far
//             from the source its fluence is the diffusion solution of a point source, 3 mu_tr I exp(-mu_eff r) / r with
//             mu_tr = rho (1 - albedo g) (the transport coefficient) and mu_eff = sqrt(3 mu_a mu_tr), mu_a = rho (1 -
//             albedo): a glow that falls as 1 / r, not 1 / r^2, isotropic (fluence / 4 pi);
// rho the mean density between the point and the source (the noise at the mip of their distance). An infinite medium's
// solution: a flash near the layer's edge loses light the formula keeps - APPROXIMATION. No nearer than the channel's
// radius (the source is a line of that size, not a point).
float3 cloudFlashSource(CloudRecord c, CloudFrameLight l, float3 x)
{
    const float3 toFlash = c.flashPosition - x;
    const float distance = length(toFlash), r = max(distance, c.flashRadius);
    const float rho = cloudDensityFiltered(c, x + 0.5 * toFlash, r);
    const float g = (1 - c.lobeBlend) * c.g0 + c.lobeBlend * c.g1;
    const float transport = rho * (1 - c.albedo * g), absorption = rho * (1 - c.albedo);
    const float direct = cloudPhase(c, dot(l.viewDir, toFlash / max(distance, 1e-3))) * exp(-rho * r) / (r * r);
    const float diffused = 3 * transport * exp(-sqrt(3 * absorption * transport) * r) / (r * (4 * 3.14159265));
    return c.flashIntensity * (direct + diffused);
}
// What a point of the cloud scatters toward the viewer per metre and per unit of its density (nits): the sun through the
// octaves, the sky's and the ground's light by their height ramps. tauSun: the sun's optical depth at the point (its
// magnitude is taken: cloudSunTauMarch's sign marks a capped path).
float3 cloudFrameSource(CloudRecord c, CloudFrameLight l, float3 x, float tauSun)
{
    const float3 sun = lerp(l.sunBase, l.sunTop, saturate((cloudAltitude(c, x) - c.base) / (c.top - c.base)));
    float3 source = cloudMsSun(l.ms, abs(tauSun), c.powder) * sun + cloudSkyRamp(c, x) * l.sky + cloudGroundRamp(c, x) * l.ground;
    if (any(c.flashIntensity > 0)) source += cloudFlashSource(c, l, x);
    return c.albedo * source;
}

// The cirrus sheet's light toward the viewer and its transmittance along a view ray that meets it at x (CloudCommon.hlsli
// cloudCirrusAt: vertical optical depth tau, |cosine| mu of the ray to the sheet's normal). Single scattering in a thin
// slab, exact for it: with a = tau / mu0 along the sun's ray and b = tau / mu along the view ray, the sun's light E
// through the phase function x b (exp(-b) - exp(-a)) / (a - b) when the sun and the viewer are on opposite sides (the
// light goes through the sheet), x b (1 - exp(-(a + b))) / (a + b) when they are on the same side (it is turned back) -
// a sun under the sheet's horizon lights it from below, as after sunset. The phase function: ice crystals, a forward
// lobe and a weak backward one (g 0.8 and -0.1, 10 % back; no halo). The sky's and the ground's light: the mean of the
// two hemispheres' radiances x the sheet's opacity. No multiple scattering (tau <= 0.3: under 15 % of the single).
#define CIRRUS_G0 0.8
#define CIRRUS_G1 (-0.1)
#define CIRRUS_BACK 0.1
void cloudCirrusLight(CloudRecord c, CloudFrameLight l, uint transmittanceLut, float3 x, float3 dir, float tau, float mu, out float3 L, out float T)
{
    const AtmosphereParams ap = airParamsFromTexels(transmittanceLut);
    const float3 up = airUp(ap, x);
    const float mus = dot(up, c.sunDir);
    const float a = tau / max(abs(mus), 0.05), b = tau / mu;
    T = exp(-b);
    const bool through = (dot(up, dir) > 0) == (mus > 0);
    const float slab = through ? (abs(a - b) > 1e-4 ? (exp(-b) - exp(-a)) / (a - b) : exp(-a)) : (1 - exp(-(a + b))) / (a + b);
    const float cosTheta = dot(dir, c.sunDir);
    const float phase = (1 - CIRRUS_BACK) * cloudHg(CIRRUS_G0, cosTheta) + CIRRUS_BACK * cloudHg(CIRRUS_G1, cosTheta);
    const float3 E = g_sunIlluminance * g_sunColor * airSunTransmittance(ap, transmittanceLut, x, c.sunDir);
    L = E * (phase * b * slab) + 0.5 * (l.sky + l.ground) * (1 - T);
}
// The sun's optical depth at a sample of the frame's cloud: sunSteps marched steps and the sun map beyond
// (atmosphere.clouds.sun_steps; 0: the whole path in 20 m steps, the exact one).
float cloudFrameSunTau(CloudRecord c, float3 x, uint sunSteps, bool filtered)
{
    if (sunSteps == 0) return cloudSunTauMarch(c, x);
    return cloudSunTauNear(c, x, sunSteps, filtered);
}
#endif
