// unx-kernel: cs_6_6 main
// A10 glass, R-1 / R-2 (TranslucentComposite JOBS=1): one thread per record of a band. The pixel's composited value
// (sun specular, and the straight path of a pane) gains w_reflect x R's reflection result and, on a solid body,
// w_refract x R's refraction result plus the air in front of the glass; a job R left untraced (alpha 0) takes its
// fallback (the GI cache's lobe, the straight path). Results are exposed linear radiance (RGBA16F, 8 B per job).
// P[0] = { records SRV (raw: header 32 B { count, groups x, y, z, ... }, 48 B records), results SRV (raw), colour UAV,
//          max records }
#include "Bindless.hlsli"

float3 unpackHalf3(uint rg, uint b) { return float3(f16tof32(rg), f16tof32(rg >> 16), f16tof32(b)); }

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    ByteAddressBuffer records = ResourceDescriptorHeap[P[0].x];
    if (i >= min(records.Load(0), P[0].w)) return;
    const uint at = 32 + i * 48;
    const uint4 a = records.Load4(at), b = records.Load4(at + 16), c = records.Load4(at + 32);
    const uint2 pixel = uint2(a.x & 0xFFFFu, a.x >> 16);
    const uint first = a.y & 0x7FFFFFFFu;
    const bool refract = (a.y >> 31) != 0;
    ByteAddressBuffer results = ResourceDescriptorHeap[P[0].y];
    RWTexture2D<float4> colour = ResourceDescriptorHeap[P[0].z];
    float3 add;
    const uint2 reflected = results.Load2(first * 8);
    if ((reflected.y >> 16) != 0) add = unpackHalf3(a.z, a.w) * unpackHalf3(reflected.x, reflected.y & 0xFFFFu);
    else add = unpackHalf3(b.z, b.w);
    if (refract)
    {
        const uint2 refracted = results.Load2((first + 1) * 8);
        if ((refracted.y >> 16) != 0) add += unpackHalf3(b.x, b.y) * unpackHalf3(refracted.x, refracted.y & 0xFFFFu) + unpackHalf3(c.z, c.w);
        else add += unpackHalf3(c.x, c.y);
    }
    colour[pixel] = float4(colour[pixel].rgb + add, 1);
}
