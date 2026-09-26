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
    // A14 (FEATURES_GAME 8, Requests/20260926_C_per_view_history.md): auxiliary views of FrameContext::auxViews, each a
    // full view with its own history under ViewResources::viewId.
    RenderTexture = 2,      // scope, security camera: linear HDR radiance into a texture materials read
    Mirror = 3,             // a mirror of any plane (clip plane, mirrored winding)
    Portal = 4,             // the paired camera seen through a portal (clip plane, the portal's screen mask)
    Split = 5,              // split screen: another player's view into a rectangle of the output
};
} // namespace unx::render::gpu
