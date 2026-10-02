// See-through shadow casters of the sun (shadow.vsm.translucent_tint). Owner: S.
// The casters of the Glass class are not in the pages' height field (V draws the opaque casters' views without them:
// RasterView::materialFilter); what they let through is kept beside it. The tint atlas (VsmConstants::tint: its SRV + 1,
// RGBA16_UNORM) holds VSM_TINT_PAGE^2 texels per physical page, laid out as the page atlas (page p at (p % 128, p / 128)
// x VSM_TINT_PAGE texels): rgb = the product of what every glass surface over the texel lets through
// (VsmTintPixel.ps.hlsl: multiplied in by the blend), a = the stored depth v of the one nearest the sun (the blend's
// maximum; 0 = none). A point takes a texel's rgb when it lies behind that nearest surface by more than the texel's
// tolerance, and 1 otherwise.
// What the one layer cannot say: a point between two glass surfaces takes both; a glass surface steep against the sun
// takes its own tint within two tint texels of height; the texel is VSM_PAGE / VSM_TINT_PAGE texels of the level wide,
// so a tinted shadow's edge is that soft. The local lights' pages keep glass as an opaque caster.
#ifndef UNX_VSM_TINT_HLSLI
#define UNX_VSM_TINT_HLSLI
#include "Passes/Shadow/VsmSample.hlsli"

#define VSM_TINT_PAGE 32u
#define VSM_TINT_LEVELS 4u  // levels tried from the point's own towards coarser ones

float vsmTintLuminance(float3 t) { return dot(t, float3(0.2126, 0.7152, 0.0722)); }
// 11 : 11 : 10 bits (white = 0xFFFFFFFF): the tint as the views' visibility texture carries it (ShadowVisibility.hlsl).
uint vsmTintPack(float3 t)
{
    const uint3 q = uint3(round(saturate(t) * float3(2047.0, 2047.0, 1023.0)));
    return q.x | (q.y << 11) | (q.z << 22);
}
float3 vsmTintUnpack(uint w) { return float3(w & 0x7FFu, (w >> 11) & 0x7FFu, w >> 22) / float3(2047.0, 2047.0, 1023.0); }

// What the sun's glass casters let through to a world point (rgb in [0, 1]; 1 without the atlas or a page): the level of
// 'footprint' or the nearest coarser one with a page, the four tint texels around the point.
float3 vsmSunTint(VsmResources r, float3 worldPos, float footprint)
{
    ConstantBuffer<VsmConstants> c = ResourceDescriptorHeap[r.cbv];
    if (c.tint == 0) return 1;
    uint k = vsmLevelForFootprint(c, footprint);
    uint entry = 0;
    int2 page = 0, texelAbs = 0;
    float3 ls = 0;
    [loop] for (uint up = 0; up < VSM_TINT_LEVELS && entry == 0 && k < VSM_LEVELS; ++up)
    {
        ls = vsmLightSpaceAt(c, worldPos, k);
        texelAbs = vsmAbsTexel(c, ls.xy, k);
        page = vsmAbsPage(texelAbs);
        entry = vsmEntry(r, page, k);
        if (entry == 0) ++k;
    }
    if (entry == 0) return 1;
    const float texel = vsmTexel(k) * float(VSM_PAGE / VSM_TINT_PAGE);  // a tint texel (m)
    // the point among the page's tint texels (centres at integer + 0.5) from its texel of the level (exact integers: far
    // from the origin a float position has no fraction of a texel left), its four neighbours clamped to the page
    const float2 inPage = (float2(texelAbs - page * (int)VSM_PAGE) + 0.5) * (float(VSM_TINT_PAGE) / float(VSM_PAGE)) - 0.5;
    const float2 p0 = clamp(floor(inPage), 0.0, float(VSM_TINT_PAGE - 1));
    const float2 f = saturate(inPage - p0);
    const uint2 q0 = uint2(p0), q1 = min(q0 + 1, VSM_TINT_PAGE - 1);
    const uint phys = entry & VSM_PHYS_MASK;
    const uint2 base = uint2(phys & (VSM_ATLAS_PAGES_PER_ROW - 1), phys >> 7) * VSM_TINT_PAGE;
    Texture2D<float4> atlas = ResourceDescriptorHeap[c.tint - 1];
    // the point's stored depth, and how far behind a texel's nearest glass it must lie (two tint texels of height, and
    // the alpha's 16-bit step)
    const float range = c.level[k].hMax - c.level[k].hMin;
    const float v = (ls.z - c.level[k].hMin) / range;
    const float bias = 2 * texel / range + 2.0 / 65535.0;
    float3 t[4];
    [unroll] for (uint i = 0; i < 4; ++i)
    {
        const float4 s = atlas.Load(int3(base + uint2((i & 1) != 0 ? q1.x : q0.x, (i & 2) != 0 ? q1.y : q0.y), 0));
        t[i] = v < s.a - bias ? s.rgb : float3(1, 1, 1);
    }
    return lerp(lerp(t[0], t[1], f.x), lerp(t[2], t[3], f.x), f.y);
}

#endif
