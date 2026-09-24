// unx-kernel: cs_6_6 main
// Sky view LUT = the previous engine's far-field sky table at the camera altitude (ARCHITECTURE 2.3): unoccluded
// atmosphere in-scattering along a complete ray to space or to the ground, per unit solar illuminance. Integrand:
// single scattering with LUT sun transmittance + the LUT multiple-scattering source + the Lambertian ground.
// skySegments quadratic segments (dense near the camera) x Gauss-Legendre 4 points; within a segment the
// transmittance uses the segment-midpoint extinction. Rebuilt when the sun or the camera altitude changes.
// P[0].x params, P[0].y transmittance LUT, P[0].z multi-scatter LUT, P[0].w output UAV (RWTexture2D<float4>)
// Frame constants: g_cameraPosition, g_sunDirection.
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Passes/Atmosphere/AtmosphereCommon.hlsli"

static const float4 kGlPoints = float4(-0.8611363115940526, -0.3399810435848563, 0.3399810435848563, 0.8611363115940526);
static const float4 kGlWeights = float4(0.3478548451374539, 0.6521451548625461, 0.6521451548625461, 0.3478548451374539);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    const AtmosphereParams a = airLoadParams(P[0].x);
    if (any(id >= a.skyViewSize)) return;
    const uint tlut = P[0].y, mlut = P[0].z;
    const uint half = a.skyViewSize.y / 2;
    const uint side = id.y / half, row = id.y % half;
    const float altitude = max(0.0, airAltitude(a, g_cameraPosition));
    const float3 up = airUp(a, g_cameraPosition);
    const float mus = clamp(dot(up, normalize(g_sunDirection)), -1.0, 1.0);
    const float elevation = airSkyElevation(a, altitude, side, float(row) / (half - 1));
    const float azimuth = float(id.x) / (a.skyViewSize.x - 1) * ATMO_PI;
    const float ce = cos(elevation);
    // Local frame: y up at the camera, the sun in the xy plane.
    const float3 sun = float3(sqrt(saturate(1 - mus * mus)), mus, 0);
    const float3 d = float3(ce * cos(azimuth), sin(elevation), ce * sin(azimuth));
    const float3 origin = float3(0, altitude, 0);
    const float2 span = airInterval(a, origin, d, 3.402823466e38);
    const float nu = dot(d, sun), rayleighPhase = airRayleighPhase(nu), miePhase = airMiePhase(nu, a.mieG);
    const float extent = max(0.0, span.y - span.x);
    const float n2 = float(a.skySegments * a.skySegments);
    float3 radiance = 0, transmittance = 1;
    [loop] for (uint n = 0; n < a.skySegments && extent > 0; ++n)
    {
        const float s0 = span.x + extent * float(n * n) / n2, s1 = span.x + extent * float((n + 1) * (n + 1)) / n2;
        const float width = s1 - s0;
        const AirCoefficients middle = airCoefficients(a, airAltitude(a, origin + d * (s0 + width * 0.5)));
        [unroll] for (uint j = 0; j < 4; ++j)
        {
            const float t = width * 0.5 * (1 + kGlPoints[j]);
            const float3 p = origin + d * (s0 + t);
            const AirCoefficients c = airCoefficients(a, airAltitude(a, p));
            const float3 source = (c.rayleigh * rayleighPhase + c.mie * miePhase) * airSunTransmittance(a, tlut, p, sun) +
                                  (c.rayleigh + c.mie) * airMultipleScattering(a, mlut, p, sun);
            radiance += transmittance * exp(-middle.extinction * t) * source * (width * 0.5 * kGlWeights[j]);
        }
        transmittance *= exp(-middle.extinction * width);
    }
    if (airHitsGround(a, origin, d))
    {
        const float3 p = origin + d * span.y, gup = airUp(a, p);
        radiance += transmittance * a.groundAlbedo *
                    (saturate(dot(gup, sun)) * airSunTransmittance(a, tlut, p + gup * 0.01, sun) / ATMO_PI + airMultipleScattering(a, mlut, p, sun));
    }
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[0].w];
    output[id] = float4(radiance, 0);
}
