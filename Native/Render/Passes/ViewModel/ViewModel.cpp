// First-person view model, renderer side (track E, A12). See include/unx/viewmodel/ViewModel.h.
#include "unx/viewmodel/ViewModel.h"

#include "unx/core/Log.h"
#include "unx/render/GpuScene.h"
#include "unx/render/Tracks.h"

#include <cmath>

namespace unx::viewmodel
{
using namespace unx::render;

uint32_t ViewModels::add(uint32_t instance, const float3x4& cameraLocal)
{
    for (const Entry& e : m_entries)
        if (e.live && e.instance == instance) fail("viewmodel: instance %u is already a view model", instance);
    uint32_t id;
    if (!m_free.empty())
    {
        id = m_free.back();
        m_free.pop_back();
    }
    else
    {
        id = (uint32_t)m_entries.size();
        m_entries.emplace_back();
    }
    m_entries[id] = { instance, cameraLocal, true };
    return id;
}

void ViewModels::setPose(uint32_t id, const float3x4& cameraLocal)
{
    if (id >= m_entries.size() || !m_entries[id].live) fail("viewmodel: no view model %u", id);
    m_entries[id].cameraLocal = cameraLocal;
}

void ViewModels::remove(uint32_t id)
{
    if (id >= m_entries.size() || !m_entries[id].live) fail("viewmodel: no view model %u", id);
    m_entries[id].live = false;
    m_removed.push_back(m_entries[id].instance);
    m_free.push_back(id);
}

ViewModels& viewModels(TrackState& state) { return state.get<ViewModels>("viewmodel.set"); }
Stats lastStats(TrackState& state) { return state.get<Stats>("viewmodel.stats"); }

float3x4 cameraToWorld(const ViewDesc& view)
{
    // view = [R | t] (rows: the camera axes in world, rigid): camera -> world = [R^T | -R^T t]
    const float4x4& v = view.view;
    float3x4 c;
    for (int r = 0; r < 3; ++r)
    {
        for (int k = 0; k < 3; ++k) c.m[r][k] = v.m[k][r];
        c.m[r][3] = -(v.m[0][r] * v.m[0][3] + v.m[1][r] * v.m[1][3] + v.m[2][r] * v.m[2][3]);
    }
    return c;
}

float projectionScale(float verticalFov, float viewModelFov)
{
    if (!(viewModelFov > 0)) return 1.0f;
    return std::tan(0.5f * verticalFov) / std::tan(0.5f * viewModelFov);
}

namespace
{
float3x4 compose(const float3x4& a, const float3x4& b)  // a * b (affine)
{
    float3x4 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
        {
            r.m[i][j] = a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j] + a.m[i][2] * b.m[2][j];
            if (j == 3) r.m[i][j] += a.m[i][3];
        }
    return r;
}
} // namespace
} // namespace unx::viewmodel

namespace unx::render::tracks
{
float viewModelPrepare(TrackState& state, GpuScene& scene, const QualityConfig& quality, const FrameContext& frame)
{
    const double overrideDegrees = quality.number("viewmodel.fov_override_degrees");
    if (overrideDegrees < 0 || overrideDegrees >= 180) fail("viewmodel.fov_override_degrees must be 0 (off) or in (0, 180)");
    const float k = viewmodel::projectionScale(frame.mainView.verticalFov, (float)(overrideDegrees * 3.14159265358979 / 180.0));
    viewmodel::ViewModels& set = viewmodel::viewModels(state);
    viewmodel::Stats& stats = state.get<viewmodel::Stats>("viewmodel.stats");
    stats = {};
    stats.frame = frame.frameIndex;
    stats.scale = k;
    for (uint32_t instance : set.takeRemovedInstances())
        if (instance < scene.instances().size()) scene.setInstanceViewModel(instance, false);
    const float3x4 camera = viewmodel::cameraToWorld(frame.mainView);
    std::vector<InstanceTransformUpdate> updates;
    for (const viewmodel::ViewModels::Entry& e : set.entries())
    {
        if (!e.live) continue;
        if (e.instance >= scene.instances().size()) fail("viewmodel: instance %u of %zu", e.instance, scene.instances().size());
        InstanceTransformUpdate u;
        u.instance = e.instance;
        u.objectToWorld = viewmodel::compose(camera, e.cameraLocal);
        u.flags = frame.discontinuity != 0 ? kTransformTeleport : 0u;  // a cut or restore: no motion across it
        updates.push_back(u);
        scene.setInstanceViewModel(e.instance, true);
        ++stats.viewModels;
        // depth range: the bounding sphere's nearest view depth vs the near plane
        const gpu::Instance& gi = scene.instances()[e.instance];
        const float4 sphere = scene.meshes()[gi.mesh].boundsSphere;
        const float3 c = e.cameraLocal.transformPoint({ sphere.x, sphere.y, sphere.z });
        const float s = std::cbrt(std::abs(e.cameraLocal.m[0][0] * (e.cameraLocal.m[1][1] * e.cameraLocal.m[2][2] - e.cameraLocal.m[1][2] * e.cameraLocal.m[2][1]) -
                                           e.cameraLocal.m[0][1] * (e.cameraLocal.m[1][0] * e.cameraLocal.m[2][2] - e.cameraLocal.m[1][2] * e.cameraLocal.m[2][0]) +
                                           e.cameraLocal.m[0][2] * (e.cameraLocal.m[1][0] * e.cameraLocal.m[2][1] - e.cameraLocal.m[1][1] * e.cameraLocal.m[2][0])));
        if (-c.z - sphere.w * s < frame.mainView.nearPlane) ++stats.nearClipped;
    }
    if (!updates.empty()) scene.updateTransforms(frame.frameIndex, updates);
    return k;
}
} // namespace unx::render::tracks
