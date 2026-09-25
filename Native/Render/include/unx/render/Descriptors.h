#pragma once
#include "unx/render/D3D12.h"

#include <mutex>
#include <vector>

namespace unx::render
{
// Bindless descriptor storage. One shader-visible CBV/SRV/UAV heap indexed from HLSL through
// ResourceDescriptorHeap[] (SM 6.6), one sampler heap, and CPU-only RTV/DSV heaps. Indices are stable for the
// lifetime of the view; shaders receive them as root constants.
class DescriptorHeaps
{
public:
    static constexpr uint32_t kResourceCapacity = 1'000'000;
    static constexpr uint32_t kSamplerCapacity = 2048;
    static constexpr uint32_t kRtvCapacity = 4096;
    static constexpr uint32_t kDsvCapacity = 1024;

    explicit DescriptorHeaps(ID3D12Device* device);

    uint32_t allocateResource();
    void freeResource(uint32_t index);
    D3D12_CPU_DESCRIPTOR_HANDLE resourceCpu(uint32_t index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE resourceGpu(uint32_t index) const;

    uint32_t allocateRtv();
    void freeRtv(uint32_t index);
    D3D12_CPU_DESCRIPTOR_HANDLE rtv(uint32_t index) const;
    uint32_t allocateDsv();
    void freeDsv(uint32_t index);
    D3D12_CPU_DESCRIPTOR_HANDLE dsv(uint32_t index) const;

    ID3D12DescriptorHeap* resourceHeap() const { return m_resource.Get(); }
    ID3D12DescriptorHeap* samplerHeap() const { return m_sampler.Get(); }
    uint32_t resourcesInUse() const;

private:
    struct FreeList
    {
        uint32_t capacity = 0;
        uint32_t next = 0;
        std::vector<uint32_t> released;
        std::vector<uint8_t> live;  // per index: allocated (a double free or a free of a free slot fails)
        uint32_t take(const char* what);
        void give(uint32_t index);
    };
    ComPtr<ID3D12DescriptorHeap> m_resource, m_sampler, m_rtvHeap, m_dsvHeap;
    uint32_t m_resourceStride = 0, m_rtvStride = 0, m_dsvStride = 0;
    mutable std::mutex m_mutex;
    FreeList m_resourceFree, m_rtvFree, m_dsvFree;
};
} // namespace unx::render
