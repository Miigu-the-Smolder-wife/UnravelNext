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
    float3 sun;     // the sun's illuminance at the cloud (lux): top-of-atmosphere through the air
    float3 sky;     // the sky's radiance over the upper hemisphere (nits): the sky's irradiance on level ground / pi
    float3 ground;  // the ground's radiance over the lower one (nits; 0: atmosphere.clouds.ground_light off)
    CloudMsPhases ms;
};
// at: a point of the view ray's span in the layer (its middle: the light is taken once per ray); dir: the view ray.
CloudFrameLight cloudFrameLight(CloudRecord c, uint transmittanceLut, float3 at, float3 dir, bool groundLight)
{
    CloudFrameLight l;
    const AtmosphereParams ap = airParamsFromTexels(transmittanceLut);
    const float3 E = g_sunIlluminance * g_sunColor;
    const float mus = dot(airUp(ap, at), c.sunDir);
    const float3 indirect = airGroundIndirect(ap, transmittanceLut, mus);
    l.sun = E * airSunTransmittance(ap, transmittanceLut, at, c.sunDir);
    l.sky = E * indirect * (1 / 3.14159265);
    l.ground = 0;
    if (groundLight)
    {
        // the ground under the point: the sun on it through the air and through the layer above (the sun map: the whole
        // column's optical depth along the sun's ray), and the sky's light; a Lambert ground of the atmosphere's albedo
        const float3 foot = at - airUp(ap, at) * max(airAltitude(ap, at), 0.0);
        const float3 direct = airSunTransmittance(ap, transmittanceLut, foot, c.sunDir) * (max(mus, 0.0) * exp(-cloudSunTau(c, foot)));
        l.ground = ap.groundAlbedo * E * (direct + indirect) * (1 / 3.14159265);
    }
    l.ms = cloudMsPhases(c, dot(dir, c.sunDir));
    return l;
}
// What a point of the cloud scatters toward the viewer per metre and per unit of its density (nits): the sun through the
// octaves, the sky's and the ground's light by their height ramps. tauSun: the sun's optical depth at the point (its
// magnitude is taken: cloudSunTauMarch's sign marks a capped path).
float3 cloudFrameSource(CloudRecord c, CloudFrameLight l, float3 x, float tauSun)
{
    return c.albedo * (cloudMsSun(l.ms, abs(tauSun)) * l.sun + cloudSkyRamp(c, x) * l.sky + cloudGroundRamp(c, x) * l.ground);
}
// The sun's optical depth at a sample of the frame's cloud: sunSteps marched steps and the sun map beyond
// (atmosphere.clouds.sun_steps; 0: the whole path in 20 m steps, the exact one).
float cloudFrameSunTau(CloudRecord c, float3 x, uint sunSteps, bool filtered)
{
    if (sunSteps == 0) return cloudSunTauMarch(c, x);
    return cloudSunTauNear(c, x, sunSteps, filtered);
}
#endif
