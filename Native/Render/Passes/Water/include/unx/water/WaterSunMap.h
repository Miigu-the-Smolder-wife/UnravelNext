#pragma once
// Water stage 2 (FEATURES_GAME 1.9; INTERFACES v1.77): the sun-space water map of W's triangle streams, read by band A
// shading through Passes/Water/WaterLight.hlsli waterSunLight. Orthographic along the sun over the union of the streams'
// bounds; texel = the larger side / N with N the power of two giving <= 2 mm, clamped to 256 .. 2048.
#include "unx/render/Device.h"
#include "unx/render/FrameResources.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"

#include <cstdint>
#include <vector>

namespace unx::water
{
struct WaterSunStream
{
    render::TriangleStream stream;
    float transmittance[3] = { 1, 1, 1 };  // the medium's transmittance over 1 m (the material's baseColor)
    float ior = 1.33f;
};
struct WaterSunMapOutput
{
    render::TextureRef depth, normal, medium;
    render::TextureRef caustics;  // R32_UINT array: WaterLight.hlsli WATER_CAUSTIC_SLICES slices of min(texels, 1024)^2
    render::BufferRef causticOverflow;  // uint: beam triangles spread wider than WATER_CAUSTIC_SPAN texels, splatted as points
    render::BufferRef constants;
    uint32_t texels = 0;  // per side; 0 = no map (no streams)
};
// Per-frame constants upload (a ring over frames in flight); owned by the caller's track state.
class WaterSunMap
{
public:
    static constexpr uint32_t kRing = 4, kConstantBytes = 80;
    static constexpr uint32_t kCausticSlices = 5, kCausticMax = 1024;  // WaterLight.hlsli WATER_CAUSTIC_SLICES / _MAX
    explicit WaterSunMap(render::Device& device);
    ~WaterSunMap();
    WaterSunMap(const WaterSunMap&) = delete;
    WaterSunMap& operator=(const WaterSunMap&) = delete;
    // sunDirection: towards the sun (normalised here).
    WaterSunMapOutput record(render::RenderGraph& graph, render::ShaderLibrary& shaders, uint64_t frame, const std::vector<WaterSunStream>& streams,
                             float3 sunDirection);

private:
    render::Device& m_device;
    render::ComPtr<ID3D12Resource> m_upload[kRing];
    uint8_t* m_mapped[kRing] = {};
};
} // namespace unx::water
