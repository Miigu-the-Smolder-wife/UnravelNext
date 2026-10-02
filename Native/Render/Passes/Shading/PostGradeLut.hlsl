// unx-kernel: cs_6_6 main
// The combined grading LUT (Post.cpp gradeLut; the reference's PostProcessCombineLUTs): scene-referred colour grading
// and the tone curve baked into one N^3 table (shading.post_grading_lut_size, the reference's r.LUT.Size 32) that the
// final pass reads once per pixel in place of evaluating the curve. Built when a parameter changes, in four dispatches
// over the table (the three ranges' parameters do not fit one dispatch's constants):
//   modes 0, 1, 2   the table's scene colour (the inverse of the final pass's log2 code: 14 stops around grey 0.18 at
//                   444 / 1023), white-balanced and in ACEScg (AP1) primaries, graded with the shadows', midtones' or
//                   highlights' parameters - saturation about the luma, contrast about grey 0.18, gamma, gain, offset
//                   (each the range's value combined with the global one) - times the range's weight by the luma
//                   (shadows 1 - smoothstep(0, shadows max), highlights smoothstep(highlights min, highlights max),
//                   midtones the rest), summed into the working table;
//   mode 3          the graded colour back in Rec.709 under the display's tone curve (ShadingCommon.hlsli shFilm or the
//                   PBR Neutral curve, over the display's range), stored as its square root (the table is interpolated
//                   linearly: the root keeps the dark end's steps even).
// The reference grades after its gamut expansion; here the expansion is inside shFilm, after the grading.
// P[0] = { working UAV (Texture3D RGBA16F), LUT UAV (Texture3D RGBA16F; mode 3), size, mode }
// modes 0 .. 2: P[1..3] = rows of linear Rec.709 -> white-balanced AP1 (xyz), P[4] = saturation (rgb), P[5] = contrast,
//               P[6] = 1 / gamma, P[7] = gain, P[8] = offset, P[9] = { asuint(shadows max), asuint(highlights min),
//               asuint(highlights max), 0 }
// mode 3:       P[1] = { tone curve (0 film, 1 PBR Neutral), asuint(display peak; 0: SDR), 0, 0 }
#include "Bindless.hlsli"
#include "Passes/Shading/ShadingCommon.hlsli"

[numthreads(4, 4, 4)]
void main(uint3 id : SV_DispatchThreadID)
{
    const uint n = P[0].z;
    if (any(id >= n)) return;
    RWTexture3D<float4> working = ResourceDescriptorHeap[P[0].x];
    const uint mode = P[0].w;
    if (mode < 3u)
    {
        const float3 code = float3(id) / (float)(n - 1u);
        const float3 scene = (exp2((code - 444.0 / 1023.0) * 14.0) - exp2(-444.0 / 1023.0 * 14.0)) * 0.18;
        float3 c = float3(dot(asfloat(P[1].xyz), scene), dot(asfloat(P[2].xyz), scene), dot(asfloat(P[3].xyz), scene));
        const float luma = dot(c, float3(0.2722287, 0.6740818, 0.0536895));
        const float shadows = 1.0 - smoothstep(0.0, asfloat(P[9].x), luma), highlights = smoothstep(asfloat(P[9].y), asfloat(P[9].z), luma);
        const float weight = mode == 0u ? shadows : (mode == 2u ? highlights : 1.0 - shadows - highlights);
        c = max(lerp(float3(luma, luma, luma), c, asfloat(P[4].xyz)), 0.0);
        c = pow(max(c, 1e-10) * (1.0 / 0.18), asfloat(P[5].xyz)) * 0.18;
        c = pow(max(c, 1e-10), asfloat(P[6].xyz));
        c = c * asfloat(P[7].xyz) + asfloat(P[8].xyz);
        float3 sum = c * weight;
        if (mode != 0u) sum += working[id].rgb;
        working[id] = float4(sum, 0);
        return;
    }
    RWTexture3D<float4> lut = ResourceDescriptorHeap[P[0].y];
    const float3x3 toSrgb = float3x3(1.7050510, -0.6217921, -0.0832589, -0.1302564, 1.1408047, -0.0105483, -0.0240033, -0.1289690, 1.1529723);
    float3 graded = mul(toSrgb, working[id].rgb);
    graded = all(isfinite(graded)) ? max(graded, 0.0) : float3(0, 0, 0);
    const float peak = asfloat(P[1].y), range = peak > 0 ? peak : 1.0;
    float3 d;
    if (P[1].x == 1u) d = peak > 0 ? shPbrNeutralPeak(graded, peak) / peak : shPbrNeutral(graded);
    else d = shFilm(graded, range) / range;
    lut[id] = float4(sqrt(saturate(d)), 1);
}
