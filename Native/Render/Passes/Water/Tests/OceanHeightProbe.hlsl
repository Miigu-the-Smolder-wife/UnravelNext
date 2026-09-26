// unx-kernel: cs_6_6 main
// Track W height clipmap probe (OceanHeightTests.cpp):
//   mode 0 (build): sample i = (level, x, z) of the clipmap; out = (|x0 + D(x0) - w| / s_l, H - (water level + h(x0)),
//          flags, 0) with x0 the stored rest position and D evaluated here as the build does (same sampler and mips)
//   mode 1 (search): ray i = (origin, tShell | direction, 0); out = (status, t hit, node visits, 0) of oceanRefineCounted
//   mode 2 (timing): a pinhole image; input = (origin, tan(fov x / 2) | forward, width | right, height | up, 0); pixel i
//          searches from tShell 0 like mode 1
// P[0] displacement SRV, height SRV, output UAV (raw), count; P[1] camera x, z, s_0, water level (OceanHeight.hlsli);
// P[2] cascade lengths; P[3] refine parameter SRV (raw), input SRV (raw), mode, slopes SRV
#include "Bindless.hlsli"
#include "../OceanRefine.hlsli"

[numthreads(64, 1, 1)]
void main(uint i : SV_DispatchThreadID)
{
    if (i >= P[0].w) return;
    ByteAddressBuffer input = ResourceDescriptorHeap[P[3].y];
    RWByteAddressBuffer o = ResourceDescriptorHeap[P[0].z];
    float4 r = 0;
    if (P[3].z == 0)
    {
        const uint4 q = input.Load4(16 * i);
        Texture2DArray<float4> height = ResourceDescriptorHeap[P[0].y];
        const float s = ohLevelSpacing(q.x);
        const float2 w = float2(ohOrigin(q.x, asfloat(P[1].xy)) + int2(q.yz)) * s;
        const float4 texel = height.Load(int4(q.yz, q.x, 0));
        const float2 x0 = w + texel.yz;
        const float3 d = ohDisplacement(x0, s);
        r = float4(length(x0 + d.xz - w) / s, texel.x - (asfloat(P[1].w) + d.y), texel.w, 0);
    }
    else
    {
        float4 a, b;
        if (P[3].z == 1) { a = asfloat(input.Load4(32 * i)); b = asfloat(input.Load4(32 * i + 16)); }
        else
        {
            const float4 c0 = asfloat(input.Load4(0)), c1 = asfloat(input.Load4(16)), c2 = asfloat(input.Load4(32)), c3 = asfloat(input.Load4(48));
            const uint width = asuint(c1.w), height = asuint(c2.w);
            const float2 ndc = (float2(i % width, i / width) + 0.5) / float2(width, height) * 2 - 1;
            const float tx = c0.w, ty = c0.w * float(height) / float(width);
            a = float4(c0.xyz, 0);
            b = float4(normalize(c1.xyz + ndc.x * tx * c2.xyz - ndc.y * ty * c3.xyz), 0);
        }
        float tHit;
        uint steps;
        const uint status = oceanRefineCounted(P[3].x, a.xyz, b.xyz, a.w, tHit, steps);
        r = float4(float(status), tHit, float(steps), 0);
    }
    o.Store4(16 * i, asuint(r));
}
