#pragma once
// Track W: one-dimensional work over a two-dimensional dispatch (WaterLinear.hlsli). D3D12 allows at most 65535 thread
// groups per dimension, so G groups go out as (min(G, 1024), ceil(G / 1024)); the kernels index with waterLinear.
#include <algorithm>
#include <cstdint>

namespace unx::water
{
constexpr uint32_t kLinearRow = 1024;  // WaterLinear.hlsli WATER_LINEAR_ROW
template <class CommandList> void dispatchLinear(CommandList* cmd, uint32_t groups)
{
    if (groups) cmd->Dispatch(std::min(groups, kLinearRow), (groups + kLinearRow - 1) / kLinearRow, 1);
}
} // namespace unx::water
