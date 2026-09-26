// unx-kernel: cs_6_6 main
// Cloud march (B5; S_STATUS_KO.md 9): the radiance the cloud layer adds along each pixel's view ray and its
// transmittance, over the ray's span in the layer's altitude shell (cloudShellSpan; clipped at the pixel's opaque
// surface). Steps: midpoints of equal intervals, the interval length the ray's footprint at the span's middle
// (distance x pixel angle x resolution scale, 25..400 m) and at most CLOUD_MARCH_STEPS per span (the structural bound).
// Per step: the density (CloudCommon.hlsli), the sun's single scattering with the sun's transmittance integrated along
// the sun ray (cloudSunTauMarch; the map only past its structural bound), the exact in-interval transmittance.
// Modes 0, 3 and 4 add multiple scattering and the sky's light by an APPROXIMATION, NOT EXACT (user decision 2026-09-27
// 06:40; CloudCommon.hlsli cloudMsSun / cloudSkyRamp, fitted to the CPU path tracer: sun mean 47 %, sky mean 17 %
// relative error, S_STATUS_KO.md 9); the exact grid solve replaces it after the weekly reset. Modes 1 and 2 stay single
// scattering (the tests' exact comparison). The sky's radiance at the cloud is the atmosphere's horizontal-ground sky
// irradiance / pi (airGroundIndirect: at the ground, not at the layer's altitude; part of the approximation).
// Mode 0 (the frame, CloudSystem.cpp): the whole view ray (no depth clip: the readers apply the layer per full-resolution
// pixel where the surface lies beyond the cloud, so no upsampling halo at geometry edges), the sun's illuminance at the
// cloud through the atmosphere (transmittance LUT at the span's middle), rows [P[1].w, P[1].w + 64) of the output (the
// dispatch is split into 64-row bands: each band's worst time is bounded), counters of the step bounds reached in the
// stats UAV P[2].y (uint 0: pixels whose span needed more than CLOUD_MARCH_STEPS, 1: sun paths past CLOUD_SUN_MAX_STEPS).
// P[0] = { cloud record SRV, output UAV, depth SRV (tests: UNX_NONE), scale (pixels of the view per output texel) }
// P[1] = { output width, height, mode (0 = RGBA16F texture: radiance, transmittance; 1 = tests: raw buffer of 8 floats
// per texel { radiance rgb, transmittance, direction xyz, 0 }; 2 = as 1 with the sun's transmittance from the deep
// opacity map alone (attribution); 4 = as 1 with the approximate multiple scattering and the record's test sky radiance
// (CloudModel.cpp referenceApproximate is its CPU twin); 3 = the sky dome: texel (u, v) is the direction cloudDomeDir from the camera, output
// as mode 0 without distance or counters (R's escaping rays: atmosphereSkyRadianceCloudy)), max distance (float bits,
// modes 1-2) or first row (mode 0) };
// P[2] = { transmittance LUT SRV (mode 0), stats UAV (mode 0), distance UAV (mode 0: R16F, km, extinction-weighted mean),
// multiple-scattering table SRV (mode 0) }.
// Mode 0 stores the layer with the air in front of it folded in (the readers then need one fetch): with in(0, d_c) and
// T_air(0, d_c) the air's in-scattering and transmittance from the camera to the cloud distance d_c (CLOUD_AIR_STEPS
// midpoint steps: sun single scattering through the transmittance LUT + the J_ms multiple scattering; the casters'
// shadows in that air are not included), rgb = in(0, d_c) (1 - T_c) + T_air(0, d_c) L_c and a = T_c, so a pixel beyond
// the cloud has in' = rgb + T_c in(0, d_s) and T' = T_c T_air(0, d_s) (Atmosphere.hlsli airApplyClouds).
// b1 = the view.
#include "Passes/Atmosphere/CloudShadowCommon.hlsli"
#include "Passes/Atmosphere/AtmosphereCommon.hlsli"
#include "Frame.hlsli"

#define CLOUD_MARCH_STEPS 256u
#define CLOUD_AIR_STEPS 16u

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    const bool test = P[1].z == 1 || P[1].z == 2 || P[1].z == 4, dome = P[1].z == 3, approximate = P[1].z == 0 || P[1].z >= 3;
    if (P[1].z == 0) id.y += P[1].w;
    if (any(id >= P[1].xy)) return;
    const CloudRecord c = cloudLoad(P[0].x);
    const float scale = (float)P[0].w;
    const float2 pixel = (float2(id) + 0.5) * scale - 0.5;  // the view pixel at the texel's centre (worldFromDepth adds 0.5)
    const float3 far = worldFromDepth(pixel, 1e-6);
    const float3 dir = dome ? cloudDomeDir((float2(id) + 0.5) / float2(P[1].xy)) : normalize(far - g_cameraPosition);
    const float tMax = test ? asfloat(P[1].w) : 3.0e38;
    float3 L = 0, sunIlluminance = c.sunIlluminance, skyRadiance = P[1].z == 4 ? c.skyRadianceTest : 0;
    float distanceSum = 0, distanceWeight = 0;
    uint capped = 0, sunCapped = 0;
    float T = 1;
    float t0, t1;
    if (cloudShellSpan(c, g_cameraPosition, dir, tMax, t0, t1))
    {
        // The pixel angle from the projection (the view's vertical field of view over its height), times the scale.
        const float pixelAngle = dome ? 6.2831853 / P[1].x : length(worldFromDepth(pixel + float2(0, 1), 1e-6) - far) / length(far - g_cameraPosition) * scale;
        const float stepLength = clamp(0.5 * (t0 + t1) * pixelAngle, 25.0, 400.0);
        const uint wanted = (uint)ceil((t1 - t0) / stepLength), steps = clamp(wanted, 1u, CLOUD_MARCH_STEPS);
        capped = wanted > CLOUD_MARCH_STEPS ? 1u : 0u;
        if (!test)
        {
            // The sun at the cloud: top-of-atmosphere illuminance through the air to the span's middle.
            const AtmosphereParams ap = airParamsFromTexels(P[2].x);
            const float3 mid = g_cameraPosition + dir * (0.5 * (t0 + t1));
            sunIlluminance = g_sunIlluminance * g_sunColor * airSunTransmittance(ap, P[2].x, mid, c.sunDir);
            skyRadiance = g_sunIlluminance * g_sunColor * airGroundIndirect(ap, P[2].x, dot(airUp(ap, mid), c.sunDir)) * (1 / 3.14159265);
        }
        const float dt = (t1 - t0) / steps;
        const float phase = cloudPhase(c, dot(dir, c.sunDir));
        const CloudMsPhases ms = cloudMsPhases(c, dot(dir, c.sunDir));
        [loop] for (uint s = 0; s < steps && T > 1e-4; ++s)
        {
            const float3 x = g_cameraPosition + dir * (t0 + (s + 0.5) * dt);
            const float rho = cloudDensity(c, x);
            if (rho <= 0) continue;
            const float segment = (1 - exp(-rho * dt)) / rho;
            // Mode 2 (attribution): the map alone.
            const float tauSun = P[1].z == 2 ? cloudSunTau(c, x) : cloudSunTauMarch(c, x);
            if (tauSun < 0) sunCapped = 1;
            const float w = T * (1 - exp(-rho * dt));
            const float sun = approximate ? cloudMsSun(ms, abs(tauSun)) : phase * exp(-abs(tauSun));
            L += T * c.albedo * rho * (sun * sunIlluminance + (approximate ? cloudSkyRamp(c, x) : 0.0) * skyRadiance) * segment;
            distanceSum += w * (t0 + (s + 0.5) * dt);
            distanceWeight += w;
            T *= exp(-rho * dt);
        }
    }
    if (test)
    {
        RWByteAddressBuffer dst = ResourceDescriptorHeap[P[0].y];
        const uint at = (id.y * P[1].x + id.x) * 32;
        dst.Store4(at, asuint(float4(L, T)));
        dst.Store4(at + 16, asuint(float4(dir, 0)));
    }
    else
    {
        const float dc = distanceWeight > 0 ? distanceSum / distanceWeight : 65000.0;
        float3 folded = L;
        if (T < 0.9999)
        {
            // The air between the camera and the cloud.
            const AtmosphereParams ap = airParamsFromTexels(P[2].x);
            const float3 sunDir = normalize(g_sunDirection), E = g_sunIlluminance * g_sunColor;
            const float nu = dot(dir, sunDir);
            float3 inC = 0, TaC = 1;
            const float ds = dc / CLOUD_AIR_STEPS;
            [loop] for (uint k = 0; k < CLOUD_AIR_STEPS; ++k)
            {
                const float3 x = g_cameraPosition + dir * ((k + 0.5) * ds);
                const AirCoefficients ac = airCoefficients(ap, airAltitude(ap, x));
                const float3 source = (ac.rayleigh * airRayleighPhase(nu) + ac.mie * airMiePhase(nu, ap.mieG)) * airSunTransmittance(ap, P[2].x, x, sunDir) +
                                      (ac.rayleigh + ac.mie) * airMultipleScattering(ap, P[2].w, x, dir, sunDir);
                inC += TaC * source * E * airIntegral(ac.extinction, ds);
                TaC *= exp(-ac.extinction * ds);
            }
            folded = inC * (1 - T) + TaC * L;
        }
        RWTexture2D<float4> dst = ResourceDescriptorHeap[P[0].y];
        dst[id] = float4(folded, T);
        if (dome) return;
        RWTexture2D<float> dist = ResourceDescriptorHeap[P[2].z];
        dist[id] = dc * 1e-3;  // km
        const uint cappedLanes = WaveActiveCountBits(capped != 0), sunLanes = WaveActiveCountBits(sunCapped != 0);
        if (WaveIsFirstLane() && (cappedLanes | sunLanes))
        {
            RWByteAddressBuffer stats = ResourceDescriptorHeap[P[2].y];
            if (cappedLanes) stats.InterlockedAdd(0, cappedLanes);
            if (sunLanes) stats.InterlockedAdd(4, sunLanes);
        }
    }
}
