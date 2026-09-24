// unx-kernel: ps_6_6 main
// S test stand-in for M's G-buffer: world normal and a grey albedo (INTERFACES 7.2 encoding). Test geometry is closed
// with outward normals and seen from outside.
#include "GBuffer.hlsli"

uint2 main(float4 position : SV_Position, float3 normal : NORMAL) : SV_Target0
{
    GBufferSample s;
    s.normal = normalize(normal);
    s.baseColor = 0.5;
    s.roughness = 0.5;
    return encodeGBuffer(s);
}
