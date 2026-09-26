// unx-kernel: cs_6_6 main
// Cloud march (B5; S_STATUS_KO.md 9): the radiance the cloud layer adds along each pixel's view ray and its
// transmittance, over the ray's span in the layer's altitude shell (cloudShellSpan; clipped at the pixel's opaque
// surface). Steps: midpoints of equal intervals, the interval length the ray's footprint at the span's middle
// (distance x pixel angle x resolution scale, 25..400 m) and at most CLOUD_MARCH_STEPS per span (the structural bound).
// Per step: the density (CloudCommon.hlsli), the sun's single scattering with the transmittance to the sun from the
// deep opacity map (CloudShadowCommon.hlsli), the exact in-interval transmittance (1 - e^(-rho dt)) / rho.
// [Step (b): single scattering of the sun only; multiple scattering and the sky's light are step (d).]
// P[0] = { cloud record SRV, output UAV, depth SRV (UNX_NONE: no surface), scale (pixels of the view per output texel) }
// P[1] = { output width, height, mode (0 = RGBA16F texture: radiance, transmittance; 1 = tests: raw buffer of 8 floats
// per texel { radiance rgb, transmittance, direction xyz, 0 }; 2 = as 1 with the sun's transmittance integrated along
// the sun ray (20 m midpoint steps: attribution of the deep opacity map's error)), max distance (float bits, modes 1-2) };
// b1 = the view.
#include "Passes/Atmosphere/CloudShadowCommon.hlsli"
#include "Frame.hlsli"

#define CLOUD_MARCH_STEPS 256u

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    if (any(id >= P[1].xy)) return;
    const CloudRecord c = cloudLoad(P[0].x);
    const float scale = (float)P[0].w;
    const float2 pixel = (float2(id) + 0.5) * scale - 0.5;  // the view pixel at the texel's centre (worldFromDepth adds 0.5)
    const float3 far = worldFromDepth(pixel, 1e-6);
    const float3 dir = normalize(far - g_cameraPosition);
    const bool test = P[1].z != 0;
    float tMax = test ? asfloat(P[1].w) : 3.0e38;
    if (P[1].z == 0 && P[0].z != UNX_NONE)
    {
        Texture2D<float> depth = ResourceDescriptorHeap[P[0].z];
        const float dd = depth.Load(int3(min(uint2(pixel + 0.5), uint2(g_viewWidth, g_viewHeight) - 1), 0));
        if (dd > 0) tMax = distance(worldFromDepth(pixel, dd), g_cameraPosition);
    }
    float3 L = 0;
    float T = 1;
    float t0, t1;
    if (cloudShellSpan(c, g_cameraPosition, dir, tMax, t0, t1))
    {
        // The pixel angle from the projection (the view's vertical field of view over its height), times the scale.
        const float pixelAngle = length(worldFromDepth(pixel + float2(0, 1), 1e-6) - far) / length(far - g_cameraPosition) * scale;
        const float stepLength = clamp(0.5 * (t0 + t1) * pixelAngle, 25.0, 400.0);
        const uint steps = clamp((uint)ceil((t1 - t0) / stepLength), 1u, CLOUD_MARCH_STEPS);
        const float dt = (t1 - t0) / steps;
        const float phase = cloudPhase(c, dot(dir, c.sunDir));
        [loop] for (uint s = 0; s < steps && T > 1e-4; ++s)
        {
            const float3 x = g_cameraPosition + dir * (t0 + (s + 0.5) * dt);
            const float rho = cloudDensity(c, x);
            if (rho <= 0) continue;
            const float segment = (1 - exp(-rho * dt)) / rho;
            float tauSun = 0;
            if (P[1].z == 2)
            {
                // The CPU reference's sun path: 20 m midpoint steps until the ray leaves the layer.
                [loop] for (float u = 10; u < 50000; u += 20)
                {
                    const float3 y = x + c.sunDir * u;
                    const float a = cloudAltitude(c, y);
                    if (a > c.top || a < c.base - 1) break;
                    tauSun += cloudDensity(c, y) * 20;
                }
            }
            else tauSun = cloudSunTau(c, x);
            L += T * c.albedo * rho * phase * c.sunIlluminance * exp(-tauSun) * segment;
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
        RWTexture2D<float4> dst = ResourceDescriptorHeap[P[0].y];
        dst[id] = float4(L, T);
    }
}
