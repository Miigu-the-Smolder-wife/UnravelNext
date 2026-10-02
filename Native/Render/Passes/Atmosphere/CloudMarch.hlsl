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
// Mode 0 (the frame, CloudSystem.cpp): the whole view ray, whatever the pixel shows - the layer's texels do not know
// the scene's depth, so their upsampling carries nothing across geometry edges. Sky pixels take the layer whole
// (Atmosphere.hlsli airApplyClouds); a surface takes the part of the cloud in front of it from the fog's volume, which
// marches that part along its own columns with this light (FogIntegrate.hlsl, atmosphere.clouds.veil). The sun's
// illuminance at the cloud through the atmosphere (transmittance LUT at the span's middle), rows [P[1].w, P[1].w + 64) of the output (the
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
// P[3].w (modes 0 and 3) = the sun path's marched steps (cloudSunTauNear; 0: the whole path, cloudSunTauMarch) | flags
// << 8: bit 0 atmosphere.clouds.filtered_steps (a step's density is its mean: CloudCommon.hlsli cloudDensityFiltered -
// the view's steps and the sun path's long ones), bit 1 atmosphere.clouds.ground_light (CloudLight.hlsli).
// Modes 0 and 3 start the span with short steps: the first is the ray's footprint where the span begins (not under
// CLOUD_STEP_MIN), each next one CLOUD_STEP_GROWTH times longer until the equal steps' length is reached - a span that
// begins at the camera (the camera inside the layer) is not sampled first at half a 200 m step from it. At most
// CLOUD_RAMP_STEPS more steps than the equal steps would be. The tests' modes keep the equal steps of their CPU twins.
// P[3].z bits 8.. (mode 0 with a history) = the dispatch's phase: 1 = a thread per 2 x 2 block marches the block's texel of
// this frame (every lane of a wave marches: a dispatch over all texels with a quarter of its lanes marching takes as
// long as one with all of them); 2 = a thread per texel fills the block's other texels (the frame's own texel is left).
// P[3] (mode 0) = { last frame's layer SRV (UNX_NONE: every texel is marched), last frame's distance SRV, this frame's
// texel of each 2 x 2 block (0..3: x | y << 1), 0 }: a texel that is not this frame's takes last frame's value at the
// place the previous view (g_prevViewProj) saw its direction - a cloud's direction places it, kilometres away - and is
// marched only where that place lies outside the previous view (atmosphere.clouds.temporal; the reference rebuilds its
// quarter-resolution trace the same way). Mode 3 fills the rows from P[1].w on (the dome is refreshed in bands).
// Mode 0 stores the layer with the air in front of it folded in (the readers then need one fetch): with in(0, d_c) and
// T_air(0, d_c) the air's in-scattering and transmittance from the camera to the cloud distance d_c (CLOUD_AIR_STEPS
// midpoint steps: sun single scattering through the transmittance LUT + the J_ms multiple scattering; the casters'
// shadows in that air are not included), rgb = in(0, d_c) (1 - T_c) + T_air(0, d_c) L_c and a = T_c, so a pixel beyond
// the cloud - a sky pixel - has in' = rgb + T_c in(0, d_s) and T' = T_c T_air(0, d_s) (Atmosphere.hlsli airApplyClouds).
// b1 = the view.
#include "Passes/Atmosphere/CloudLight.hlsli"

#define CLOUD_MARCH_STEPS 256u
#define CLOUD_AIR_STEPS 16u
#define CLOUD_STEP_MIN 25.0
#define CLOUD_STEP_GROWTH 1.15
#define CLOUD_RAMP_STEPS 32u     // (25 m x 1.15^20 = 409 m: the ramp reaches a 400 m equal step in 20 steps; a span cut to
                                 // CLOUD_MARCH_STEPS has longer ones - 2 km in 32)

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    const bool test = P[1].z == 1 || P[1].z == 2 || P[1].z == 4, dome = P[1].z == 3, approximate = P[1].z == 0 || P[1].z >= 3;
    const uint block = P[3].z & 3u, phase = P[1].z == 0 ? P[3].z >> 8 : 0u;
    if (phase == 1) id = id * 2 + uint2(block & 1u, block >> 1);
    if (P[1].z == 0 || P[1].z == 3) id.y += P[1].w;
    if (any(id >= P[1].xy)) return;
    if (phase == 2 && ((id.x & 1u) | ((id.y & 1u) << 1)) == block) return;
    const CloudRecord c = cloudLoad(P[0].x);
    const float scale = (float)P[0].w;
    const float2 pixel = (float2(id) + 0.5) * scale - 0.5;  // the view pixel at the texel's centre (worldFromDepth adds 0.5)
    const float3 far = worldFromDepth(pixel, 1e-6);
    const float3 dir = dome ? cloudDomeDir((float2(id) + 0.5) / float2(P[1].xy)) : normalize(far - g_cameraPosition);
    if (P[1].z == 0 && P[3].x != 0xFFFFFFFFu && ((id.x & 1u) | ((id.y & 1u) << 1)) != block)
    {
        const float4 clip = mul(g_prevViewProj, float4(g_cameraPosition + dir * 1.0e5, 1));
        const float2 uv = float2(clip.x, -clip.y) / max(clip.w, 1e-6) * 0.5 + 0.5;
        const float2 margin = 1.0 / float2(P[1].xy);  // (a texel: the lookup's whole footprint inside the previous view)
        if (clip.w > 0 && all(uv > margin) && all(uv < 1 - margin))
        {
            Texture2D<float4> previous = ResourceDescriptorHeap[P[3].x];
            Texture2D<float> previousDistance = ResourceDescriptorHeap[P[3].y];
            RWTexture2D<float4> out4 = ResourceDescriptorHeap[P[0].y];
            RWTexture2D<float> out1 = ResourceDescriptorHeap[P[2].z];
            out4[id] = previous.SampleLevel(g_linearClamp, uv, 0);
            out1[id] = previousDistance.SampleLevel(g_linearClamp, uv, 0);
            return;
        }
    }
    const float tMax = test ? asfloat(P[1].w) : 3.0e38;
    const uint sunSteps = P[3].w & 0xFFu;
    const bool filtered = !test && (P[3].w & 0x100u) != 0;
    float3 L = 0, sunIlluminance = c.sunIlluminance, skyRadiance = P[1].z == 4 ? c.skyRadianceTest : 0;
    float distanceSum = 0, distanceWeight = 0;
    uint capped = 0, sunCapped = 0;
    float T = 1;
    float t0, t1;
    if (cloudShellSpan(c, g_cameraPosition, dir, tMax, t0, t1))
    {
        // The pixel angle from the projection (the view's vertical field of view over its height), times the scale.
        const float pixelAngle = dome ? 6.2831853 / P[1].x : length(worldFromDepth(pixel + float2(0, 1), 1e-6) - far) / length(far - g_cameraPosition) * scale;
        const float stepLength = clamp(0.5 * (t0 + t1) * pixelAngle, CLOUD_STEP_MIN, 400.0);
        const uint wanted = (uint)ceil((t1 - t0) / stepLength), steps = clamp(wanted, 1u, CLOUD_MARCH_STEPS);
        capped = wanted > CLOUD_MARCH_STEPS ? 1u : 0u;
        // The frame's light (the tests' modes: the record's): the sun at the cloud through the air to the span's middle,
        // the sky's and the ground's radiance there.
        CloudFrameLight light = (CloudFrameLight)0;
        if (!test) light = cloudFrameLight(c, P[2].x, g_cameraPosition + dir * (0.5 * (t0 + t1)), dir, (P[3].w & 0x200u) != 0);
        const float dt = (t1 - t0) / steps;
        const float phase = cloudPhase(c, dot(dir, c.sunDir));
        const CloudMsPhases ms = cloudMsPhases(c, dot(dir, c.sunDir));
        // (the frame: short steps first - the footprint where the span begins, growing to dt)
        const uint bound = test ? steps : steps + CLOUD_RAMP_STEPS;
        float t = t0, next = clamp(t0 * pixelAngle, CLOUD_STEP_MIN, dt);
        [loop] for (uint s = 0; s < bound && T > 1e-4; ++s)
        {
            float d = dt, at = t0 + (s + 0.5) * dt;
            if (!test)
            {
                if (!(t < t1)) break;
                d = min(next, t1 - t);
                at = t + 0.5 * d;
                t += d;
                next = min(next * CLOUD_STEP_GROWTH, dt);
            }
            const float3 x = g_cameraPosition + dir * at;
            const float rho = filtered ? cloudDensityFiltered(c, x, d) : cloudDensity(c, x);
            if (rho <= 0) continue;
            const float segment = (1 - exp(-rho * d)) / rho;
            const float w = T * (1 - exp(-rho * d));
            if (test)
            {
                // Mode 2 (attribution): the map alone.
                const float tauSun = P[1].z == 2 ? cloudSunTau(c, x) : cloudSunTauMarch(c, x);
                if (tauSun < 0) sunCapped = 1;
                const float sun = approximate ? cloudMsSun(ms, abs(tauSun)) : phase * exp(-abs(tauSun));
                L += T * c.albedo * rho * (sun * sunIlluminance + (approximate ? cloudSkyRamp(c, x) : 0.0) * skyRadiance) * segment;
            }
            else
            {
                // (modes 0 and 3 with sun steps: the near field marched, the sun map beyond - atmosphere.clouds.sun_steps)
                const float tauSun = cloudFrameSunTau(c, x, sunSteps, filtered);
                if (tauSun < 0) sunCapped = 1;
                L += T * rho * cloudFrameSource(c, light, x, tauSun) * segment;
            }
            distanceSum += w * at;
            distanceWeight += w;
            T *= exp(-rho * d);
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
