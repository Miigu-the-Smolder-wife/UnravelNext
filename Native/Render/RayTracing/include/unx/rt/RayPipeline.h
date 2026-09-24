#pragma once
// DispatchRays pipelines of the R track (ARCHITECTURE 1.3-5, 2.5, 2.6): one state object per ray library (a lib_6_6 kernel
// file, INTERFACES 2.1 naming), the device's bindless global root signature, no local root signatures (every resource is
// bindless, passed in root constants), and a shader table uploaded once to default memory.
#include "unx/render/Device.h"
#include "unx/render/Shaders.h"

#include <string>
#include <vector>

namespace unx::render::rt
{
struct RayHitGroup
{
    std::string name;
    std::string closestHit;  // empty = none
    std::string anyHit;      // empty = none
};

struct RayPipelineDesc
{
    std::string library;                 // kernel name of a lib_6_6 DXIL, e.g. "Passes/GI/GiTrace"
    std::vector<std::string> rayGen;     // ray generation exports; dispatch() selects one by index
    std::vector<std::string> miss;       // miss table order = MissShaderIndex
    std::vector<RayHitGroup> hitGroups;  // hit table order = hit group index
    uint32_t payloadBytes = 32;          // RtHit
    uint32_t attributeBytes = 8;         // triangle barycentrics
    uint32_t maxRecursion = 1;           // TraceRay only from ray generation
};

// The shared hit/miss set of RayShaders.hlsli.
RayPipelineDesc standardRayPipeline(std::string library, std::vector<std::string> rayGen);

class RayPipeline
{
public:
    RayPipeline(Device& device, ShaderLibrary& shaders, const RayPipelineDesc& desc);
    ~RayPipeline();
    RayPipeline(const RayPipeline&) = delete;
    RayPipeline& operator=(const RayPipeline&) = delete;

    // Binds the state object and dispatches ray generation shader 'rayGen'. Root constants and the frame CBV are the
    // caller's (PassContext::computeConstants / bindFrameConstants); DispatchRays uses the compute root arguments.
    void dispatch(ID3D12GraphicsCommandList7* cmd, uint32_t rayGen, uint32_t width, uint32_t height, uint32_t depth = 1) const;
    double createMs() const { return m_createMs; }

    // Pipelines are created once per (device, library) and live as long as the device.
    static RayPipeline& get(Device& device, ShaderLibrary& shaders, const RayPipelineDesc& desc);
    // Drops the pipelines of a device before it is destroyed (tests that create several devices).
    static void releaseDevice(Device& device);

private:
    Device& m_device;
    ComPtr<ID3D12StateObject> m_state;
    ComPtr<ID3D12Resource> m_table;
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> m_rayGen;
    D3D12_GPU_VIRTUAL_ADDRESS m_miss = 0, m_hit = 0;
    uint64_t m_missStride = 0, m_missBytes = 0, m_hitStride = 0, m_hitBytes = 0;
    double m_createMs = 0;
};
} // namespace unx::render::rt
