// unx-kernel: cs_6_6 main
// The angular quadrature depends only on probe resolution, never on the scene.
// P0={weights UAV, radiance resolution, 0, 0}; sample-major, 36 normals/sample.
#include "Bindless.hlsli"
#include "Passes/GI/LumenRadianceCache.hlsli"
[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    const uint normals = LRC_IRRADIANCE_RES * LRC_IRRADIANCE_RES;
    const uint res = P[0].y, sample = i / normals, normalIndex = i % normals;
    if (sample >= res * res) return;
    const float3 normal = lrcUvToDirection((float2(normalIndex % LRC_IRRADIANCE_RES, normalIndex / LRC_IRRADIANCE_RES) + 0.5) / float(LRC_IRRADIANCE_RES));
    const float3 direction = lrcUvToDirection((float2(sample % res, sample / res) + 0.5) / float(res));
    RWByteAddressBuffer weights = ResourceDescriptorHeap[P[0].x];
    weights.Store(i * 4, asuint(max(dot(direction, normal), 0.0)));
}
