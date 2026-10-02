// unx-kernel: ps_6_6 main
// unx-variants: ALPHA=0,1
// Band A visibility buffer pixel: writes the primitive's vis id; the depth test (GREATER_EQUAL, reversed Z) runs early
// (ALPHA=0). ALPHA=1 first applies the material's alpha test (AlphaTest.hlsli), which moves the depth test late.
#include "Passes/Visibility/AlphaTest.hlsli"

#if ALPHA
uint main(float4 position : SV_Position, float2 uv : TEXCOORD0, nointerpolation uint visId : VISID, nointerpolation uint material : MATERIAL) : SV_Target0
{
    if (!alphaTestCoveredAt(material, uv, position.xy)) discard;
    return visId;
}
#else
uint main(float4 position : SV_Position, nointerpolation uint visId : VISID) : SV_Target0
{
    return visId;
}
#endif
