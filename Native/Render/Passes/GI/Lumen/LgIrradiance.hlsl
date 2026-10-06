// unx-kernel: cs_6_6 main
// gi.lumen, r.gi.lg.irradiance: the filtered probe radiance into what the pixels read. One group per probe.
//   - The radiance map's 3-band SH (per channel: 4 pi / 64 x sum of radiance x basis over the 64 equal-area texels),
//     then the irradiance E(n) = SH . clamped cosine lobe(n) for the 6 x 6 directions of the probe's irradiance map,
//     written with a 1-texel border (8 x 8 per probe: the pixels sample it bilinearly across the octahedral edges).
//   - The radiance map itself with a 1-texel border (10 x 10 per probe), for the rough specular lobes.
// P[0] = { radiance SRV (atlas x 8), 0, 0, 0 }, P[1] = { irradiance UAV (RGBA16F, atlas x 8), radiance with border UAV
// (RGBA16F, atlas x 10), 0, 0 }, P[10].z adaptive SRV, P[10].w probe depth SRV.
#include "Passes/GI/Lumen/LgCommon.hlsli"

groupshared float3 gs_radiance[64];
groupshared float gs_coefficient[27];
groupshared float gs_basis[9][64];

float lgBasisAt(float3 d, uint k)
{
    const LgSh s = lgShBasis(d);
    return k < 4 ? s.a[k] : (k < 8 ? s.b[k - 4] : s.c);
}

[numthreads(8, 8, 1)]
void main(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    const uint2 atlas = group.xy;
    const uint probe = lgProbeIndex(atlas);
    ByteAddressBuffer adaptive = ResourceDescriptorHeap[P[10].z];
    Texture2D<float> probeDepth = ResourceDescriptorHeap[P[10].w];
    if (atlas.x >= lgProbeViewSize().x || probe >= lgProbeCount(adaptive)) return;
    Texture2D<float4> radiance = ResourceDescriptorHeap[P[0].x];
    RWTexture2D<float4> irradianceOut = ResourceDescriptorHeap[P[1].x];
    RWTexture2D<float4> borderOut = ResourceDescriptorHeap[P[1].y];
    const uint index = thread.y * 8 + thread.x;
    const bool live = probeDepth[atlas] > 0;
    gs_radiance[index] = live ? radiance[atlas * LG_GATHER_RES + thread.xy].rgb : float3(0, 0, 0);
    // Decode each equal-area direction once for all coefficients/channels.
    const LgSh basis = lgShBasis(lgSphere((float2(thread.xy) + 0.5) / 8.0));
    [unroll] for (uint k = 0; k < 9; ++k)
        gs_basis[k][index] = k < 4 ? basis.a[k] : (k < 8 ? basis.b[k - 4] : basis.c);
    GroupMemoryBarrierWithGroupSync();
    if (index < 27)
    {
        const uint k = index / 3, channel = index % 3;
        float sum = 0;
        [loop] for (uint i = 0; i < 64; ++i)
            sum += gs_radiance[i][channel] * gs_basis[k][i];
        gs_coefficient[index] = sum * (4 * LG_PI / 64.0);
    }
    GroupMemoryBarrierWithGroupSync();
    // irradiance map: texel (x, y) of the 8 x 8 block = interior texel lgOctWrap of the 6 x 6 map
    {
        const uint2 inner = lgOctWrap((int2)thread.xy, LG_IRRADIANCE_RES, 1);
        const float3 n = lgSphere((float2(inner) + 0.5) / (float)LG_IRRADIANCE_RES);
        const LgSh lobe = lgShCosineLobe(n);
        float3 e = 0;
        [unroll] for (uint k = 0; k < 9; ++k)
        {
            const float w = k < 4 ? lobe.a[k] : (k < 8 ? lobe.b[k - 4] : lobe.c);
            e += float3(gs_coefficient[k * 3], gs_coefficient[k * 3 + 1], gs_coefficient[k * 3 + 2]) * w;
        }
        irradianceOut[atlas * 8 + thread.xy] = float4(max(e, 0.0), 1);
    }
    // radiance with border: 10 x 10 texels, each thread the texels of its residue
    [loop] for (uint y = thread.y; y < LG_GATHER_RES + 2; y += 8)
        [loop] for (uint x = thread.x; x < LG_GATHER_RES + 2; x += 8)
        {
            const uint2 inner = lgOctWrap(int2(x, y), LG_GATHER_RES, 1);
            borderOut[atlas * (LG_GATHER_RES + 2) + uint2(x, y)] = float4(gs_radiance[inner.y * 8 + inner.x], 1);
        }
}
