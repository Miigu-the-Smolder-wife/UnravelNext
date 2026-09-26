#pragma once
// First-person view model, renderer side (track E, A12; FEATURES_GAME 3). Owner: E.
//
// A view model (hands, tools) is an ordinary scene instance - skinned like any character, in the TLAS, a VSM caster,
// lit by the GI cache - whose transform is its pose in the camera's frame (view space: x right, y up, looking down -z)
// composed with the camera of each rendered frame. So it follows the 165 Hz camera without the 60 Hz tick's lag, its
// motion relative to the camera is zero (motion vectors, the camera-rotation blur: FEATURES_GAME 3.2), and shadows,
// reflections and GI see the same geometry as the screen (one camera, one depth buffer; no depth bias - wall
// clipping is gameplay's). tracks::viewModelPrepare runs first in the frame, before the GPU scene's flush: it writes
// the transforms (GpuScene::updateTransforms; a history discontinuity teleports) and the view-model flag.
//
// Projection remap (viewmodel.fov_override_degrees, 0 = off, the default): a separate view-model field of view is a
// main-view raster remap of the view-model instances' clip coordinates, clip.xy x k with k = tan(fov / 2) /
// tan(fov_vm / 2) (depth unchanged: the view model stays where it is in the depth buffer), FrameConstants::
// viewModelScale; V applies it (ViewModel.hlsli). Shadows, reflections and GI keep the true geometry, so with the
// option on the view model's screen image and its shadows / reflections differ by that remap (FEATURES_GAME 3.2:
// condition recorded, outside the gates).
// Depth range: the reversed-Z fp32 depth covers the view model from the near plane (1 cm) on; a view model whose
// bounding sphere comes nearer than the near plane is clipped by it - counted in Stats::nearClipped (a content /
// camera defect the host sees, never hidden by a depth trick).
#include "unx/render/Frame.h"

#include <cstdint>
#include <vector>

namespace unx::render
{
class GpuScene;
}

namespace unx::viewmodel
{
class ViewModels
{
public:
    // instance: a scene instance; cameraLocal: its object -> camera (view space) transform.
    uint32_t add(uint32_t instance, const float3x4& cameraLocal);
    void setPose(uint32_t id, const float3x4& cameraLocal);
    void remove(uint32_t id);

    struct Entry
    {
        uint32_t instance;
        float3x4 cameraLocal;
        bool live;
    };
    const std::vector<Entry>& entries() const { return m_entries; }
    std::vector<uint32_t> takeRemovedInstances() { return std::move(m_removed); }

private:
    std::vector<Entry> m_entries;
    std::vector<uint32_t> m_free, m_removed;
};

ViewModels& viewModels(render::TrackState& state);

struct Stats
{
    uint64_t frame = UINT64_MAX;
    uint32_t viewModels = 0;
    uint32_t nearClipped = 0;  // view models whose bounding sphere reached in front of the near plane this frame
    float scale = 1.0f;        // the projection remap k
};
Stats lastStats(render::TrackState& state);

// The camera-to-world transform of a view (inverse of its rigid view matrix).
float3x4 cameraToWorld(const render::ViewDesc& view);
// k = tan(fov / 2) / tan(fovViewModel / 2); 1 when fovViewModel <= 0.
float projectionScale(float verticalFov, float viewModelFov);
} // namespace unx::viewmodel
