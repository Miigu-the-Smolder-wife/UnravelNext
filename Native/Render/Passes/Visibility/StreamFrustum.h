#pragma once
#include "unx/render/FrameResources.h"
#include "unx/render/ViewDesc.h"
#include <algorithm>
#include <cmath>

namespace unx::visibility::detail
{
// TriangleStream's world AABB is supplied for culling. Keep unknown bounds and
// a two-pixel guard for conservative coverage, projection jitter and roundoff.
// Reject only if every corner lies strictly outside the same clip half-space.
inline bool streamInView(const render::TriangleStream& stream, const render::ViewDesc& view)
{
    const float3 lo = stream.boundsMin, hi = stream.boundsMax;
    if (!(lo.x <= hi.x && lo.y <= hi.y && lo.z <= hi.z) ||
        !(hi.x > lo.x || hi.y > lo.y || hi.z > lo.z)) return true;
    const double padX = 4.0 / std::max(view.width, 1u), padY = 4.0 / std::max(view.height, 1u);
    uint32_t outside[5] = {};
    for (uint32_t corner = 0; corner < 8; ++corner)
    {
        const double point[3] = { corner & 1 ? hi.x : lo.x, corner & 2 ? hi.y : lo.y, corner & 4 ? hi.z : lo.z };
        double clip[4] = {};
        for (uint32_t row = 0; row < 4; ++row)
        {
            clip[row] = view.viewProj.m[row][3];
            for (uint32_t column = 0; column < 3; ++column) clip[row] += view.viewProj.m[row][column] * point[column];
            if (!std::isfinite(clip[row])) return true;
        }
        const double epsilon = 1e-5 * std::max({ 1.0, std::abs(clip[0]), std::abs(clip[1]), std::abs(clip[3]) });
        outside[0] += clip[0] < -(1 + padX) * clip[3] - epsilon;
        outside[1] += clip[0] >  (1 + padX) * clip[3] + epsilon;
        outside[2] += clip[1] < -(1 + padY) * clip[3] - epsilon;
        outside[3] += clip[1] >  (1 + padY) * clip[3] + epsilon;
        outside[4] += clip[3] < -epsilon;
    }
    for (uint32_t count : outside) if (count == 8) return false;
    return true;
}
}
