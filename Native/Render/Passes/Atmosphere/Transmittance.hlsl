// unx-kernel: cs_6_6 main
// Transmittance LUT (ARCHITECTURE 2.3): optical depth from (altitude, view cosine) to the top of the atmosphere,
// Gauss-Legendre 4 points on each of transmittanceSteps segments. Stored as optical depth (RGBA32F) so lookups
// interpolate the exponent. Built when the atmosphere parameters change.
// P[0].x params (raw buffer, AtmosphereParams), P[0].y output UAV (RWTexture2D<float4>)
#include "Bindless.hlsli"
#include "Passes/Atmosphere/AtmosphereCommon.hlsli"

static const float4 kGlPoints = float4(-0.8611363115940526, -0.3399810435848563, 0.3399810435848563, 0.8611363115940526);
static const float4 kGlWeights = float4(0.3478548451374539, 0.6521451548625461, 0.6521451548625461, 0.3478548451374539);

[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID)
{
    const AtmosphereParams a = airLoadParams(P[0].x);
    if (any(id >= a.transmittanceSize)) return;
    float altitude, cosine;
    airTransmittanceParams(a, float2(id) / float2(a.transmittanceSize - 1), altitude, cosine);
    const float3 p = float3(0, altitude, 0), d = float3(sqrt(saturate(1 - cosine * cosine)), cosine, 0);
    const float2 span = airInterval(a, p, d, 3.402823466e38);
    const float step = max(0.0, span.y - span.x) / a.transmittanceSteps;
    float3 tau = 0;
    [loop] for (uint i = 0; i < a.transmittanceSteps; ++i)
        [unroll] for (uint j = 0; j < 4; ++j)
        {
            const float t = span.x + (i + 0.5 + 0.5 * kGlPoints[j]) * step;
            tau += airCoefficients(a, airAltitude(a, p + d * t)).extinction * (step * 0.5 * kGlWeights[j]);
        }
    RWTexture2D<float4> output = ResourceDescriptorHeap[P[0].y];
    output[id] = float4(tau, 0);
}
