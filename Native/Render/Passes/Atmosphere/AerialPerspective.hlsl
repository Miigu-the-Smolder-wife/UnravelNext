// unx-kernel: cs_6_6 main
// Aerial perspective volume of the main view (ARCHITECTURE 2.3). For each of W x H screen columns and each depth node
// z_n = aerialMaxDistance * (n / S)^2, n = 0..S (quadratic: dense near the camera), it stores per unit solar illuminance
// the phase-free in-scattering ratios K_i = I_i / (1 - T) of
//     I_R = int T sigma_R T_sun dt,  I_M = int T sigma_M T_sun dt,  I_MS = int T (sigma_R + sigma_M) Psi_ms dt
// and the optical depth tau (rgb). K is the source-to-extinction ratio averaged along the ray: it does not depend on
// the density scale, so it stays smooth across the horizon and between nodes; node 0 holds its limit at the camera
// (sigma_i S_i / sigma_t). The lookup applies the pixel's exact Rayleigh / Mie phase. Texture3D depth 4 (S + 1):
// [0, S] K_R, then K_M, K_MS, tau. aerialStepsPerSlice exact-exponential steps per slice. Points below the model's
// surface use the surface air (airLiftToSurface).
// P[0].x params, P[0].y transmittance LUT, P[0].z multi-scatter LUT, P[0].w output UAV (RWTexture3D<float4>)
// P[1].xy volume width/height. Frame constants of the main view.
#include "Bindless.hlsli"
#include "Frame.hlsli"
#include "Passes/Atmosphere/AtmosphereCommon.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    const AtmosphereParams a = airLoadParams(P[0].x);
    const uint2 size = P[1].xy;
    if (any(id >= size)) return;
    const uint tlut = P[0].y, mlut = P[0].z, S = a.aerialSlices, N = S + 1;
    RWTexture3D<float4> output = ResourceDescriptorHeap[P[0].w];
    // Column direction through the centre of its screen cell.
    const float2 uv = (float2(id) + 0.5) / float2(size);
    const float3 dir = airViewDirection(uv);
    const float toRay = 1.0 / max(dot(dir, airViewForward()), 1e-4);  // view depth -> distance along the ray
    const float3 sun = normalize(g_sunDirection);
    {
        const float3 p = airLiftToSurface(a, g_cameraPosition);
        const AirCoefficients c = airCoefficients(a, max(0.0, airAltitude(a, p)));
        const float3 inv = 1 / max(c.extinction, 1e-30);
        const float3 sunT = airSunTransmittance(a, tlut, p, sun);
        output[uint3(id, 0)] = float4(c.rayleigh * sunT * inv, 0);
        output[uint3(id, N)] = float4(c.mie * sunT * inv, 0);
        output[uint3(id, 2 * N)] = float4((c.rayleigh + c.mie) * airMultipleScattering(a, mlut, p, sun) * inv, 0);
        output[uint3(id, 3 * N)] = 0;
    }
    float3 iR = 0, iM = 0, iMS = 0, T = 1, tau = 0;
    float t = 0;
    [loop] for (uint n = 1; n <= S; ++n)
    {
        const float z = a.aerialMaxDistance * (float(n) / S) * (float(n) / S);
        const float tEnd = z * toRay;
        const float dt = (tEnd - t) / a.aerialStepsPerSlice;
        [loop] for (uint k = 0; k < a.aerialStepsPerSlice; ++k)
        {
            const float3 p = airLiftToSurface(a, g_cameraPosition + dir * (t + dt * 0.5));
            const AirCoefficients c = airCoefficients(a, max(0.0, airAltitude(a, p)));
            const float3 w = T * airIntegral(c.extinction, dt);
            const float3 sunT = airSunTransmittance(a, tlut, p, sun);
            iR += w * c.rayleigh * sunT;
            iM += w * c.mie * sunT;
            iMS += w * (c.rayleigh + c.mie) * airMultipleScattering(a, mlut, p, sun);
            tau += c.extinction * dt;
            T = exp(-tau);
            t += dt;
        }
        t = tEnd;
        const float3 inv = 1 / max(airOneMinusExp(tau), 1e-30);
        output[uint3(id, n)] = float4(iR * inv, 0);
        output[uint3(id, N + n)] = float4(iM * inv, 0);
        output[uint3(id, 2 * N + n)] = float4(iMS * inv, 0);
        output[uint3(id, 3 * N + n)] = float4(tau, 0);
    }
}
