#pragma once
// Reflections of the R track (ARCHITECTURE 2.6), main view. One instance per FrameRenderer (track state "R.reflection").
// Per frame (after GI: the screen probes are the G path's control variate and the K path's source):
//   r.refl.begin -> r.refl.classify -> r.refl.args -> r.refl.trace (indirect DispatchRays) -> r.refl.resolve
// K pixels are left to M (Reflection.hlsli); planar mirrors (reflection camera raster through FrameServices::renderView)
// and the exact set (original BLASes of characters and wind foliage near mirrors) are not implemented yet.
#include "unx/gi/GiSystem.h"
#include "unx/render/Frame.h"
#include "unx/rt/RayScene.h"

namespace unx::render::refl
{
struct ReflectionSettings  // from Config/quality/reflection.toml
{
    float kHalfAngle = 0;        // radians: narrow-lobe half-angle at or above which the K path applies
    float mirrorRoughness = 0;   // perceptual roughness below which a pixel takes one ray (M path)
    uint32_t raysPerSample = 0;  // G path rays per sample
    uint32_t maxSpacing = 0;     // G sample spacing bound in px (tile-limited to 8)
    static ReflectionSettings fromQuality(const QualityConfig& q);
};

class ReflectionSystem
{
public:
    static ReflectionSystem& get(FramePassContext& fc);
    ReflectionSystem(Device& device, ShaderLibrary& shaders, const QualityConfig& quality);
    ~ReflectionSystem();
    ReflectionSystem(const ReflectionSystem&) = delete;
    ReflectionSystem& operator=(const ReflectionSystem&) = delete;

    // Declares the reflection passes of the main view and creates view.reflection. Needs this frame's GI
    // (view.screenProbes, FrameResources::giCache) and ray scene.
    void record(FramePassContext& fc, ViewResources& main, rt::RayScene& rays);
    // Constant sky radiance (nits) and sun illuminance (lux) when S's atmosphere LUTs are absent (tests).
    void setConstantSky(float3 radiance, float3 sunIlluminance)
    {
        m_skyRadiance = radiance;
        m_sunIlluminance = sunIlluminance;
    }
    const ReflectionSettings& settings() const { return m_settings; }
    // This frame's per-pixel mode texture (ReflectionInternal.hlsli), for tests and diagnostics.
    TextureRef modes() const { return m_modes; }

private:
    void ensureHistory(uint32_t width, uint32_t height);
    Device& m_device;
    ReflectionSettings m_settings;
    ComPtr<ID3D12Resource> m_history;   // R16_FLOAT reflection hit distance of the last frame (G spacing)
    uint32_t m_historyWidth = 0, m_historyHeight = 0;
    ComPtr<ID3D12Resource> m_arguments;  // raw: job counter, then the two indirect dispatch descriptions (SKY0, SKY1)
    float3 m_skyRadiance{}, m_sunIlluminance{};
    TextureRef m_modes;
};
} // namespace unx::render::refl
