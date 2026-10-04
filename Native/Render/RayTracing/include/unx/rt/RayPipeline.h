#pragma once
// DispatchRays pipelines of the R track (ARCHITECTURE 1.3-5, 2.5, 2.6): one state object per ray library (a lib_6_6 kernel
// file, INTERFACES 2.1 naming), the device's bindless global root signature, no local root signatures (every resource is
// bindless, passed in root constants), and a shader table uploaded once to default memory.
#include "unx/render/Device.h"
#include "unx/render/Shaders.h"
#include "unx/render/RenderGraph.h"

#include <string>
#include <vector>

namespace unx::render::rt
{
struct RayHitGroup
{
    std::string name;
    std::string closestHit;  // empty = none
    std::string anyHit;      // empty = none
    std::string intersection;  // non-empty: a procedural-primitive hit group with this intersection shader
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
    struct IndirectBatch
    {
        BufferRef arguments;
        uint32_t capacity = 0;
        ID3D12CommandSignature* signature = nullptr; // owned by this pipeline
    };
    RayPipeline(Device& device, ShaderLibrary& shaders, const RayPipelineDesc& desc);
    ~RayPipeline();
    RayPipeline(const RayPipeline&) = delete;
    RayPipeline& operator=(const RayPipeline&) = delete;

    // Binds the state object and dispatches ray generation shader 'rayGen'. Root constants and the frame CBV are the
    // caller's (PassContext::computeConstants / bindFrameConstants); DispatchRays uses the compute root arguments.
    void dispatch(ID3D12GraphicsCommandList7* cmd, uint32_t rayGen, uint32_t width, uint32_t height, uint32_t depth = 1) const;
    // The dispatch description of ray generation shader 'rayGen' (shader table records) for indirect dispatches whose
    // size a kernel writes: the caller stores it once in an argument buffer and a kernel rewrites Width/Height/Depth.
    D3D12_DISPATCH_RAYS_DESC dispatchDesc(uint32_t rayGen, uint32_t width, uint32_t height, uint32_t depth) const;
    // ExecuteIndirect of one D3D12_DISPATCH_RAYS_DESC at 'offset' of 'arguments' (the pass declares it IndirectArgs).
    void dispatchIndirect(ID3D12GraphicsCommandList7* cmd, ID3D12Resource* arguments, uint64_t offset) const;
    // GPU-sized command stream: one root constant (first thread) and one ray
    // dispatch per nonempty chunk. No CPU capacity loop, empty commands, or
    // per-chunk template copies. The consumer declares arguments IndirectArgs.
    // counter contains an item count; multiplier expands each item to threads.
    // rowWidth preserves a 2D ray grid (root constant = first row). Otherwise
    // rootDivisor converts the first thread to a packed band/index, with
    // rootBits ORed in; callers keep flag bits disjoint from the index bits.
    IndirectBatch prepareBatch(RenderGraph& graph, ShaderLibrary& shaders, std::string_view name,
                               BufferRef counter, uint32_t capacity, uint32_t threadsPerCommand,
                               uint32_t firstThreadConstant, uint32_t multiplier = 1,
                               uint32_t counterOffset = 0, uint32_t rayGen = 0, uint32_t rowWidth = 0,
                               uint32_t rootBits = 0, uint32_t rootDivisor = 1);
    void dispatchBatch(PassContext& context, const IndirectBatch& batch) const;
    // The descriptions of the ray generation shaders with Width = Height = Depth = 0, kDispatchDescStride apart (shader i
    // at i x kDispatchDescStride), in an upload buffer that lives as long as the pipeline: a pass whose dispatch sizes a
    // kernel writes copies one per chunk into its argument buffer (CopyBufferRegion; RayTracing/CompactDispatch.hlsl
    // writes the sizes of a compacted list's chunks).
    static constexpr uint32_t kDispatchDescStride = (uint32_t)((sizeof(D3D12_DISPATCH_RAYS_DESC) + 7) / 8 * 8);
    ID3D12Resource* dispatchTemplate() const { return m_template.Get(); }
    double createMs() const { return m_createMs; }

    // Pipelines are created once per (device, library) and live as long as the device.
    static RayPipeline& get(Device& device, ShaderLibrary& shaders, const RayPipelineDesc& desc);
    // Drops the pipelines of a device before it is destroyed (tests that create several devices).
    static void releaseDevice(Device& device);

private:
    Device& m_device;
    ComPtr<ID3D12StateObject> m_state;
    ComPtr<ID3D12CommandSignature> m_indirect;
    std::array<ComPtr<ID3D12CommandSignature>, Device::kRootConstantCount> m_batchSignatures;
    ComPtr<ID3D12Resource> m_table;
    ComPtr<ID3D12Resource> m_template;
    std::vector<D3D12_GPU_VIRTUAL_ADDRESS> m_rayGen;
    D3D12_GPU_VIRTUAL_ADDRESS m_miss = 0, m_hit = 0;
    uint64_t m_missStride = 0, m_missBytes = 0, m_hitStride = 0, m_hitBytes = 0;
    double m_createMs = 0;
};
} // namespace unx::render::rt
