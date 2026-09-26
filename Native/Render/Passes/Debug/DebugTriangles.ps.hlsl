// unx-kernel: ps_6_6 main
// Debug triangles (E, A15): flat colour times the depth test, premultiplied.
#include "Passes/Debug/DebugCommon.hlsli"

float4 main(DebugVertex v) : SV_Target0
{
    const float4 c = debugUnpackColor(v.color);
    const float alpha = c.a * debugDepthFactor(v.position.xy, v.position.z, v.info);
    if (alpha <= 0) discard;
    return float4(c.rgb * alpha, alpha);
}
