// unx-kernel: cs_6_6 main
// Multiple-scattering LUT (Hillaire 2020 section 5.5, the previous engine's formulation): for a point at altitude h
// with sun cosine mu_s, the isotropic second-order radiance L2 and the scattered energy fraction f_ms over a uniform
// sphere of directions close the series Psi_ms = L2 / (1 - f_ms). 1 - f_ms is accumulated directly as the absorbed
// and escaped energy (better conditioned than 1 - f_ms when the medium is thick). The ground is Lambertian.
// Row size.y of the output texture holds a copy of AtmosphereParams (9 float4) for the public lookups.
// P[0].x params, P[0].y transmittance LUT SRV, P[0].z output UAV (RWTexture2D<float4>, height = size.y + 1)
#include "Bindless.hlsli"
#include "Passes/Atmosphere/AtmosphereCommon.hlsli"

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    const AtmosphereParams a = airLoadParams(P[0].x);
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[0].z];
    if (id.y == 0 && id.x < 9)
    {
        ByteAddressBuffer raw = ResourceDescriptorHeap[P[0].x];
        output[uint2(id.x, a.multiScatterSize.y)] = asfloat(raw.Load4(id.x * 16));
    }
    if (any(id >= a.multiScatterSize)) return;
    const uint lut = P[0].y;
    const float h = float(id.y) / (a.multiScatterSize.y - 1) * (a.topRadius - a.bottomRadius);
    const float mu = float(id.x) / (a.multiScatterSize.x - 1) * 2 - 1;
    const float3 origin = float3(0, h, 0), sun = float3(sqrt(saturate(1 - mu * mu)), mu, 0);
    const uint directions = a.multiScatterDirections, steps = a.multiScatterSteps;
    float3 second = 0, loss = 0;
    [loop] for (uint n = 0; n < directions; ++n)
    {
        // Fibonacci sphere: uniform solid angle, deterministic.
        const float z = 1 - 2 * (n + 0.5) / directions, phi = n * 2.399963229728653;
        const float s = sqrt(1 - z * z);
        const float3 d = float3(s * cos(phi), z, s * sin(phi));
        const float2 span = airInterval(a, origin, d, 3.402823466e38);
        const float extent = max(0.0, span.y - span.x);
        float3 T = 1;
        // Quadratic spacing t = extent u^2: dense near the origin where the density along the ray is highest.
        [loop] for (uint j = 0; j < steps; ++j)
        {
            const float u0 = float(j) / steps, u1 = float(j + 1) / steps;
            const float t0 = extent * u0 * u0, step = extent * (u1 * u1 - u0 * u0);
            const float3 p = origin + d * (span.x + t0 + 0.5 * step);
            const AirCoefficients c = airCoefficients(a, airAltitude(a, p));
            const float3 integral = airIntegral(c.extinction, step);
            second += T * integral * (c.rayleigh + c.mie) * airSunTransmittance(a, lut, p, sun);
            loss += T * integral * max(0.0, c.extinction - c.rayleigh - c.mie);
            T *= exp(-c.extinction * step);
        }
        if (airHitsGround(a, origin, d))
        {
            const float3 p = origin + d * span.y, up = airUp(a, p);
            second += 4 * T * a.groundAlbedo * saturate(dot(up, sun)) * airSunTransmittance(a, lut, p + up * 0.01, sun);
            loss += T * (1 - a.groundAlbedo);
        }
        else
            loss += T;
    }
    // second / directions = mean over the sphere; L2 = that / 4 pi (isotropic phase); 1 - f_ms = loss / directions.
    output[id] = float4(second / (4 * ATMO_PI * loss), 0);
}
