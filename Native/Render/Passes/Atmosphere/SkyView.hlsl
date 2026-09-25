// unx-kernel: cs_6_6 main
// Sky view LUT = the previous engine's far-field sky table at the camera altitude (ARCHITECTURE 2.3): unoccluded
// atmosphere in-scattering along a complete ray to space or to the ground, per unit solar illuminance. Integrand:
// single scattering with LUT sun transmittance + the multiple-scattering source table J_ms (along this ray's direction)
// + the Lambertian ground lit by the sun and by the scattered light (its indirect irradiance). J_ms (8 fetches) is read at
// the segment boundaries and interpolated linearly inside a segment (second order: J_ms varies on the scale heights, a
// segment is a small part of it); the single scattering keeps its 4 Gauss-Legendre points.
// Three parts in one texture (height 3 x size.y): rows [0, H) multiple scattering + ground, [H, 2H) the Rayleigh and
// [2H, 3H) the Mie single scattering without their phase functions (column integrals of sigma T_sun T): the lookup
// multiplies them by the phases of its own direction, so the Mie aureole around the sun (g = 0.8, varying within a
// degree) is exact instead of interpolated between texels 0.94 deg apart.
// skySegments quadratic segments (dense near the camera) x Gauss-Legendre 4 points; within a segment the
// transmittance uses the segment-midpoint extinction. Rebuilt when the sun or the camera altitude changes.
// One group per texel: its SKY_THREADS threads take contiguous runs of segments, integrate them with the transmittance
// from their run's start, and a scan of the runs' optical depths joins them (fixed order: deterministic).
// Dispatch: size.x x size.y groups.
// P[0].x params, P[0].y transmittance LUT, P[0].z multiple-scattering table (J_ms), P[0].w output UAV (RWTexture2D<float4>,
// height 3 x size.y)
// Frame constants: g_cameraPosition, g_sunDirection.
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Passes/Atmosphere/AtmosphereCommon.hlsli"

static const float4 kGlPoints = float4(-0.8611363115940526, -0.3399810435848563, 0.3399810435848563, 0.8611363115940526);
static const float4 kGlWeights = float4(0.3478548451374539, 0.6521451548625461, 0.6521451548625461, 0.3478548451374539);

#define SKY_THREADS 32u
groupshared float3 gs_tau[SKY_THREADS];
groupshared float3 gs_radiance[SKY_THREADS];
groupshared float3 gs_rayleigh[SKY_THREADS];
groupshared float3 gs_mie[SKY_THREADS];

[numthreads(SKY_THREADS, 1, 1)]
void main(uint3 gid : SV_GroupID, uint lane : SV_GroupIndex)
{
    const AtmosphereParams a = airLoadParams(P[0].x);
    const uint2 id = gid.xy;
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
    const float extent = max(0.0, span.y - span.x);
    const float n2 = float(a.skySegments * a.skySegments);
    // This thread's run of segments; radiance and optical depth relative to the run's start.
    const uint perThread = (a.skySegments + SKY_THREADS - 1) / SKY_THREADS;
    const uint first = lane * perThread, last = min(first + perThread, a.skySegments);
    float3 radiance = 0, tau = 0, rayleighCol = 0, mieCol = 0;
    float3 msStart = 0;
    if (first < last && extent > 0) msStart = airMultipleScattering(a, mlut, origin + d * (span.x + extent * float(first * first) / n2), d, sun);
    [loop] for (uint n = first; n < last && extent > 0; ++n)
    {
        const float s0 = span.x + extent * float(n * n) / n2, s1 = span.x + extent * float((n + 1) * (n + 1)) / n2;
        const float width = s1 - s0;
        const AirCoefficients middle = airCoefficients(a, airAltitude(a, origin + d * (s0 + width * 0.5)));
        const float3 transmittance = exp(-tau);
        const float3 msEnd = airMultipleScattering(a, mlut, origin + d * s1, d, sun);
        [unroll] for (uint j = 0; j < 4; ++j)
        {
            const float f = 0.5 * (1 + kGlPoints[j]), t = width * f;
            const float3 p = origin + d * (s0 + t);
            const AirCoefficients c = airCoefficients(a, airAltitude(a, p));
            const float3 w = transmittance * exp(-middle.extinction * t) * (width * 0.5 * kGlWeights[j]);
            const float3 sunT = airSunTransmittance(a, tlut, p, sun);
            rayleighCol += w * c.rayleigh * sunT;
            mieCol += w * c.mie * sunT;
            radiance += w * (c.rayleigh + c.mie) * lerp(msStart, msEnd, f);
        }
        msStart = msEnd;
        tau += middle.extinction * width;
    }
    // Exclusive scan of the runs' optical depths (Hillis-Steele), then the runs' radiance summed in lane order.
    gs_tau[lane] = tau;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint w = 1; w < SKY_THREADS; w <<= 1)
    {
        const float3 add = lane >= w ? gs_tau[lane - w] : 0;
        GroupMemoryBarrierWithGroupSync();
        gs_tau[lane] += add;
        GroupMemoryBarrierWithGroupSync();
    }
    const float3 before = exp(-(gs_tau[lane] - tau));
    gs_radiance[lane] = before * radiance;
    gs_rayleigh[lane] = before * rayleighCol;
    gs_mie[lane] = before * mieCol;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint width2 = SKY_THREADS / 2; width2 > 0; width2 >>= 1)
    {
        if (lane < width2)
        {
            gs_radiance[lane] += gs_radiance[lane + width2];
            gs_rayleigh[lane] += gs_rayleigh[lane + width2];
            gs_mie[lane] += gs_mie[lane + width2];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (lane != 0) return;
    float3 total = gs_radiance[0];
    if (airHitsGround(a, origin, d))
    {
        const float3 transmittance = exp(-gs_tau[SKY_THREADS - 1]);
        const float3 p = origin + d * span.y, gup = airUp(a, p);
        const float mus = dot(gup, sun);
        total += transmittance * a.groundAlbedo / ATMO_PI *
                 (saturate(mus) * airSunTransmittance(a, tlut, p + gup * 0.01, sun) + airGroundIndirect(a, tlut, mus));
    }
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[0].w];
    output[id] = float4(total, 0);
    output[uint2(id.x, id.y + a.skyViewSize.y)] = float4(gs_rayleigh[0], 0);
    output[uint2(id.x, id.y + 2 * a.skyViewSize.y)] = float4(gs_mie[0], 0);
}
