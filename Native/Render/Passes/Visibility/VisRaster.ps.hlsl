// unx-kernel: ps_6_6 main
// Band A visibility buffer pixel: writes the primitive's vis id; the depth test (GREATER_EQUAL, reversed Z) runs early.
uint main(float4 position : SV_Position, nointerpolation uint visId : VISID) : SV_Target0
{
    return visId;
}
