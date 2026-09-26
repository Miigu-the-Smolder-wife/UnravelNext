// First-person view model (A12). Owner: E. Readers: V's main-view raster (mesh shaders), M (the rotation-blur mask).
// See Passes/ViewModel/include/unx/viewmodel/ViewModel.h.
#ifndef UNX_VIEW_MODEL_HLSLI
#define UNX_VIEW_MODEL_HLSLI
#include "Frame.hlsli"
#include "Scene.hlsli"

bool viewModelInstance(GpuInstance inst) { return (inst.flags & INSTANCE_VIEW_MODEL) != 0; }

// The main view's clip position of a view-model vertex under viewmodel.fov_override_degrees: clip.xy x k (depth and w
// unchanged, so the view model keeps its place in the depth buffer). k = 1 (the default) and non-main views leave it.
// V calls it on the current and the previous clip position of view-model vertices (the motion vector stays the
// view model's own motion).
float4 viewModelClip(GpuInstance inst, float4 clip)
{
    if (viewModelInstance(inst)) clip.xy *= g_viewModelScale;
    return clip;
}
#endif
