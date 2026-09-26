// unx-kernel: ps_6_6 main
// Water stage 2 sun-space map (WaterSunMap.ms): the surface's normal (octahedral, RG16F) and the stream's medium (1 m
// transmittance RGB, IOR; RGBA16F). P[1] the medium (floats).
#include "Bindless.hlsli"
#include "WaterLight.hlsli"

struct PixelOut
{
    float2 normal : SV_Target0;
    float4 medium : SV_Target1;
};

PixelOut main(float4 position : SV_Position, float3 normal : NORMAL)
{
    PixelOut o;
    o.normal = waterOctEncode(normalize(normal));
    o.medium = asfloat(P[1]);
    return o;
}
