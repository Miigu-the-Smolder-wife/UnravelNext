// unx-kernel: ps_6_6 main
// unx-variants: MODE=0,1
// A6 translucent layer pixels (v1.67; VisRaster.ms.ALPHA1 over LIST_T_BACK / LIST_T_NONE, all entries of both cull
// phases). The material's alpha test runs first (AlphaTest.hlsli; a material without it passes), then
//   MODE=0 count: depth test against band A only (no depth write), +1 in the pixel's count (P[2].x, RWTexture2D<uint>):
//          the number of translucent surfaces in front of band A at the pixel centre;
//   MODE=1 nearest: depth test and write over a copy of band A's depth: the vis id (VisBuffer.hlsli, the view's
//          visible list) and the linear view depth of the nearest translucent surface in front of band A.
#include "Frame.hlsli"
#include "Passes/Visibility/AlphaTest.hlsli"

#if MODE == 0
[earlydepthstencil]
void main(float4 position : SV_Position, float2 uv : TEXCOORD0, nointerpolation uint visId : VISID, nointerpolation uint material : MATERIAL)
{
    if (!alphaTestCoveredAt(material, uv, position.xy)) discard;
    RWTexture2D<uint> count = ResourceDescriptorHeap[P[2].x];
    InterlockedAdd(count[uint2(position.xy)], 1u);
}
#else
struct Out
{
    uint visId : SV_Target0;
    float depth : SV_Target1;
};

Out main(float4 position : SV_Position, float2 uv : TEXCOORD0, nointerpolation uint visId : VISID, nointerpolation uint material : MATERIAL)
{
    if (!alphaTestCoveredAt(material, uv, position.xy)) discard;
    Out o;
    o.visId = visId;
    o.depth = linearDepth(position.z);
    return o;
}
#endif
