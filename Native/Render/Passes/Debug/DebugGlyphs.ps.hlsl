// unx-kernel: ps_6_6 main
// Debug text (E, A15): the glyph's coverage from the font atlas (R8 with mips, cells 1:2 with a 1/8-cell empty border so
// mips up to 3 do not bleed), at the level whose cell height matches the glyph's pixel height; DEBUG_SHADOW adds the
// coverage shifted one pixel down-right behind it in black at 80 %. Premultiplied.
#include "Passes/Debug/DebugCommon.hlsli"

float debugGlyphCoverage(uint code, float2 uv, float lod)
{
    if (code < 32u || code > 126u || any(uv < 0) || any(uv > 1)) return 0;
    Texture2D<float> atlas = ResourceDescriptorHeap[P[0].z];
    const uint columns = P[0].w, rows = P[1].x, index = code - 32u;
    const float2 cellOrigin = float2(index % columns, index / columns);
    return atlas.SampleLevel(g_linearClamp, (cellOrigin + uv) / float2(columns, rows), lod);
}

float4 main(DebugVertex v) : SV_Target0
{
    const uint code = v.info & 0xFFu, flags = v.info >> 16;
    const float size = v.extra;
    const float lod = clamp(log2((float)P[1].y / size), 0.0f, (float)P[1].z - 1.0f);
    const float text = debugGlyphCoverage(code, v.uv, lod);
    const float2 pixelUv = float2(1.0f / (size * DEBUG_GLYPH_ASPECT), 1.0f / size);
    const float shadow = (flags & DEBUG_SHADOW) ? 0.8f * debugGlyphCoverage(code, v.uv - pixelUv, lod) : 0.0f;
    const float4 c = debugUnpackColor(v.color);
    const float depth = debugDepthFactor(v.position.xy, v.position.z, flags);
    const float a = (text + shadow * (1 - text)) * c.a * depth;
    if (a <= 0) discard;
    return float4(c.rgb * text * c.a * depth, a);
}
