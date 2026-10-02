#pragma once
// CPU reference of ShadingCommon.hlsli shFilm (the default tone curve, U2): the same operations in float, in the
// kernel's order - gamut expansion, blue correction, glow and red modifier (in AP0), the 0.96 desaturation, the toe /
// straight / shoulder curve per channel, the 0.93 desaturation, the blue correction's inverse, Rec.709, and above a
// peak of 1 the shoulder's expansion.
#include <algorithm>
#include <cmath>

namespace unx::test
{
inline void filmCurve(float c[3], float peak)
{
    const float toSrgb[3][3] = { { 1.7050510f, -0.6217921f, -0.0832589f }, { -0.1302564f, 1.1408047f, -0.0105483f }, { -0.0240033f, -0.1289690f, 1.1529723f } };
    const float toAp1[3][3] = { { 0.6130973f, 0.3395229f, 0.0473793f }, { 0.0701942f, 0.9163556f, 0.0134526f }, { 0.0206156f, 0.1095698f, 0.8698151f } };
    const float ap1ToAp0[3][3] = { { 0.6954522f, 0.1406787f, 0.1638691f }, { 0.0447946f, 0.8596711f, 0.0955343f }, { -0.0055259f, 0.0040252f, 1.0015007f } };
    const float ap0ToAp1[3][3] = { { 1.4514393f, -0.2365107f, -0.2149286f }, { -0.0765538f, 1.1762297f, -0.0996759f }, { 0.0083161f, -0.0060324f, 0.9977163f } };
    const float expand[3][3] = { { 1.3704124f, -0.3292922f, -0.0636831f }, { -0.0834335f, 1.0970927f, -0.0108614f }, { -0.0257933f, -0.0986258f, 1.2036949f } };
    const float y[3] = { 0.2722287f, 0.6740818f, 0.0536895f };
    const float slope = 0.88f, toe = 0.55f, shoulder = 0.26f, blackClip = 0.0f, whiteClip = 0.04f;
    const float blueCorrection = 0.6f, expandGamut = 1.0f;
    auto mul = [](const float m[3][3], const float v[3], float out[3]) {
        for (int i = 0; i < 3; ++i) out[i] = m[i][0] * v[0] + m[i][1] * v[1] + m[i][2] * v[2];
    };
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    float a[3];
    mul(toAp1, c, a);
    {
        // gamut expansion: bright saturated colours move toward the wider gamut
        const float luma = y[0] * a[0] + y[1] * a[1] + y[2] * a[2];
        float chroma2 = 0;
        for (float v : a)
        {
            const float ch = v / std::max(luma, 1e-10f) - 1.0f;
            chroma2 += ch * ch;
        }
        const float amount = (1 - std::exp2(-4 * chroma2)) * (1 - std::exp2(-4 * expandGamut * luma * luma));
        float e[3];
        mul(expand, a, e);
        for (int i = 0; i < 3; ++i) a[i] = lerp(a[i], e[i], amount);
    }
    // blue correction (in AP1)
    a[0] = lerp(a[0], 0.9386394f * a[0] + 0.0613606f * a[2], blueCorrection);
    a[1] = lerp(a[1], 0.8307941f * a[1] + 0.1692059f * a[2], blueCorrection);
    {
        // glow (dark saturated colours gain up to 5 %) and red modifier (saturated reds toward 0.03), in AP0
        float c0[3];
        mul(ap1ToAp0, a, c0);
        const float lowest = std::min({ c0[0], c0[1], c0[2] }), highest = std::max({ c0[0], c0[1], c0[2] });
        const float saturation = (std::max(highest, 1e-10f) - std::max(lowest, 1e-10f)) / std::max(highest, 1e-2f);
        const float yc =
            (c0[0] + c0[1] + c0[2] + 1.75f * std::sqrt(std::max(c0[2] * (c0[2] - c0[1]) + c0[1] * (c0[1] - c0[0]) + c0[0] * (c0[0] - c0[2]), 0.0f))) / 3.0f;
        const float x = (saturation - 0.4f) / 0.2f, t = std::max(1 - std::fabs(0.5f * x), 0.0f);
        const float sign = x > 0 ? 1.0f : (x < 0 ? -1.0f : 0.0f);
        const float gain = 0.05f * 0.5f * (1 + sign * (1 - t * t));
        const float glowMid = 0.08f;
        const float glow = yc <= 2.0f / 3.0f * glowMid ? gain : (yc >= 2 * glowMid ? 0.0f : gain * (glowMid / yc - 0.5f));
        for (float& v : c0) v *= 1 + glow;
        float hue = (c0[0] == c0[1] && c0[1] == c0[2]) ? 0.0f : std::atan2(1.7320508f * (c0[1] - c0[2]), 2 * c0[0] - c0[1] - c0[2]) * (180.0f / 3.14159265f);
        hue = hue > 180 ? hue - 360 : hue;  // (centred on red: hue 0)
        const float w = std::clamp(1 - std::fabs(2 * hue / 135.0f), 0.0f, 1.0f);
        const float hueWeight = w * w * (3 - 2 * w);
        c0[0] += hueWeight * hueWeight * saturation * (0.03f - c0[0]) * (1 - 0.82f);
        mul(ap0ToAp1, c0, a);
        for (float& v : a) v = std::max(v, 0.0f);
    }
    float luma = y[0] * a[0] + y[1] * a[1] + y[2] * a[2];
    for (float& v : a) v = std::max(luma + (v - luma) * 0.96f, 0.0f);
    const float toeScale = 1 + blackClip - toe, shoulderScale = 1 + whiteClip - shoulder;
    const float bt = (0.18f + blackClip) / toeScale - 1;
    const float toeMatch = std::log10(0.18f) - 0.5f * std::log((1 + bt) / (1 - bt)) * (toeScale / slope);
    const float straightMatch = (1 - toe) / slope - toeMatch;
    const float shoulderMatch = shoulder / slope - straightMatch;
    for (float& v : a)
    {
        const float l = std::log10(std::max(v, 1e-10f));
        const float straight = slope * (l + straightMatch);
        float toeColor = -blackClip + 2 * toeScale / (1 + std::exp((-2 * slope / toeScale) * (l - toeMatch)));
        float shoulderColor = (1 + whiteClip) - 2 * shoulderScale / (1 + std::exp((2 * slope / shoulderScale) * (l - shoulderMatch)));
        toeColor = l < toeMatch ? toeColor : straight;
        shoulderColor = l > shoulderMatch ? shoulderColor : straight;
        float t = std::clamp((l - toeMatch) / (shoulderMatch - toeMatch), 0.0f, 1.0f);
        t = shoulderMatch < toeMatch ? 1 - t : t;
        t = (3 - 2 * t) * t * t;
        v = toeColor + (shoulderColor - toeColor) * t;
    }
    luma = y[0] * a[0] + y[1] * a[1] + y[2] * a[2];
    for (float& v : a) v = std::max(luma + (v - luma) * 0.93f, 0.0f);
    // the blue correction's inverse
    a[0] = lerp(a[0], 1.0653749f * a[0] - 0.0653710f * a[2], blueCorrection);
    a[1] = lerp(a[1], 1.2036635f * a[1] - 0.2036677f * a[2], blueCorrection);
    for (int i = 0; i < 3; ++i) c[i] = std::max(toSrgb[i][0] * a[0] + toSrgb[i][1] * a[1] + toSrgb[i][2] * a[2], 0.0f);
    if (peak > 1)
    {
        const float knee = 0.8f, top = 1 + whiteClip, r = (top * peak - knee) / (top - knee);
        for (int i = 0; i < 3; ++i)
            if (c[i] > knee)
            {
                const float u = std::min((c[i] - knee) / (top - knee), 0.999999f);
                c[i] = knee + (top - knee) * (u / (1 - u * (1 - 1 / r)));
            }
    }
}
} // namespace unx::test
