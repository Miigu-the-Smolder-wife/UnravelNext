#pragma once
// Local lights of INTERFACES_KO.md 8.2 for next-event estimation and MIS.
//   Every light's contribution is windowed to zero at 'range' (w(d) = saturate(1 - (d/range)^4)^2, d = distance to
//   the light centre), so the lights that can contribute at a point are exactly those whose range sphere contains
//   it. A uniform grid lists them per cell; one of them is chosen proportionally to an importance that is positive
//   whenever the contribution can be positive (so the estimator stays unbiased) and zero only where it is exactly
//   zero (behind a one-sided emitter, outside a spot cone, beyond range).
//   Point/spot: delta lights. Rect/disk: uniform area sampling. Sphere: uniform cone of the visible cap. Tube
//   (capsule): uniform area over the capsule surface. All area pdfs are converted to solid angle.
#include "Atmosphere.h"

#include "unx/scene/SceneData.h"

#include <cstdint>
#include <vector>

namespace unx::reference
{
struct LightSample
{
    float3 wi;          // unit, from the shading point towards the light
    float distance = 0; // to the sampled point (delta lights: to the light position)
    Rgb L;              // radiance (area) or irradiance at normal incidence (delta), windowed; excludes visibility
    float pdf = 0;      // solid-angle pdf of wi given this light (1 for delta lights)
    bool delta = false;
};

struct LightCandidates
{
    std::vector<uint32_t> lights;
    std::vector<float> cumulative;  // running sum of importances
    float total = 0;
};

class LightSet
{
public:
    explicit LightSet(const scene::Scene& scene);
    bool empty() const { return m_lights.empty(); }
    const scene::Light& light(uint32_t i) const { return m_lights[i]; }

    void gather(float3 x, LightCandidates& out) const;
    float importance(uint32_t light, float3 x) const;
    // Chooses a candidate proportionally to importance; returns its index into scene lights and the probability.
    static uint32_t choose(const LightCandidates& c, float u, float& probability);

    bool sample(uint32_t light, float3 x, float u1, float u2, LightSample& s) const;
    // Ray from o along unit d hits the emitting side of an area light before tmax: distance, radiance, solid-angle pdf.
    bool intersect(uint32_t light, float3 o, float3 d, float tmax, float& t, Rgb& L, float& pdf) const;

    // Grid and normalised lights (GPU tracer upload).
    size_t count() const { return m_lights.size(); }
    float3 up(uint32_t i) const { return m_up[i]; }
    float spotScale(uint32_t i) const { return m_spotScale[i]; }
    float spotOffset(uint32_t i) const { return m_spotOffset[i]; }
    float3 gridMin() const { return m_min; }
    float3 gridCell() const { return m_cell; }
    const uint32_t* gridDim() const { return m_dim; }
    const std::vector<uint32_t>& cellStart() const { return m_cellStart; }
    const std::vector<uint32_t>& cellLights() const { return m_cellLights; }

private:
    float window(const scene::Light& l, float3 x) const;
    std::vector<scene::Light> m_lights;
    std::vector<float3> m_up;        // rect: second axis (forward x right)
    std::vector<float> m_spotScale, m_spotOffset;
    float3 m_min{}, m_cell{};
    uint32_t m_dim[3] = { 0, 0, 0 };
    std::vector<uint32_t> m_cellStart, m_cellLights;
};
} // namespace unx::reference
