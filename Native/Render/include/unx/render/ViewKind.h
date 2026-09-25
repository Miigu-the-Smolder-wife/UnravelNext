#pragma once
// View kinds of the frame constants (GpuSceneLayout.h FrameConstants::viewKind) and ViewDesc. Split from
// GpuSceneLayout.h (v1.42) so ViewDesc does not recompile with the GPU scene layouts.
#include <cstdint>

namespace unx::render::gpu
{
enum class ViewKind : uint32_t
{
    Main = 0,
    PlanarReflection = 1,   // rendered through FrameServices::renderView by R (INTERFACES_KO.md 5.4)
};
} // namespace unx::render::gpu
