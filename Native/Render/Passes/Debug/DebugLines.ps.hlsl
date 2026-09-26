// unx-kernel: ps_6_6 main
// Debug lines (E, A15): coverage of the pixel by the capsule of half width h around the segment, box-filtered over the
// pixel along the distance (saturate(h + 0.5 - d)), times the colour's opacity and the depth test; premultiplied.
#include "Passes/Debug/DebugCommon.hlsli"

float4 main(DebugVertex v) : SV_Target0
{
    const float2 p = v.position.xy, a = v.segment.xy, ab = v.segment.zw - a;
    const float t = saturate(dot(p - a, ab) / max(dot(ab, ab), 1e-12f));
    const float distance = length(p - (a + t * ab));
    const float4 c = debugUnpackColor(v.color);
    const float alpha = c.a * saturate(v.extra + 0.5f - distance) * debugDepthFactor(p, v.position.z, v.info);
    if (alpha <= 0) discard;
    return float4(c.rgb * alpha, alpha);
}
