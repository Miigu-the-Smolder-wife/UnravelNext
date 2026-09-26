#pragma once
// CPU reference of ShadingCommon.hlsli shFilm (the default tone curve, U2): the same operations in float.
#include <algorithm>
#include <cmath>

namespace unx::test
{
inline void filmCurve(float c[3], float peak)
{
    const float toSrgb[3][3] = { { 1.7050510f, -0.6217921f, -0.0832589f }, { -0.1302564f, 1.1408047f, -0.0105483f }, { -0.0240033f, -0.1289690f, 1.1529723f } };
    const float toAp1[3][3] = { { 0.6130973f, 0.3395229f, 0.0473793f }, { 0.0701942f, 0.9163556f, 0.0134526f }, { 0.0206156f, 0.1095698f, 0.8698151f } };
    const float y[3] = { 0.2722287f, 0.6740818f, 0.0536895f };
    const float slope = 0.88f, toe = 0.55f, shoulder = 0.26f, blackClip = 0.0f, whiteClip = 0.04f;
    float a[3];
    for (int i = 0; i < 3; ++i) a[i] = toAp1[i][0] * c[0] + toAp1[i][1] * c[1] + toAp1[i][2] * c[2];
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
