#pragma once

#include "unx/render/GpuSceneLayout.h"
#include "unx/scene/SceneData.h"

#include <span>
#include <vector>

namespace unx::render::detail
{
// Built from a validated upload. Palette membership is fixed until the next
// upload: setInstances rejects skinned edits and runtime instances are rigid.
// Ascending insertion preserves updateSkeleton's original instance order.
inline std::vector<std::vector<uint32_t>> skeletonInstances(
    size_t skeletonCount, std::span<const scene::Instance> source, std::span<const gpu::Instance> packed)
{
    std::vector<std::vector<uint32_t>> result(skeletonCount);
    for (uint32_t i = 0; i < packed.size(); ++i)
        if (packed[i].bonePalette != gpu::kNone) result[source[i].skeleton].push_back(i);
    return result;
}
} // namespace unx::render::detail
