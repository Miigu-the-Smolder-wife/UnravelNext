#include "unx/render/Descriptors.h"

namespace unx::render
{
uint32_t DescriptorHeaps::FreeList::take(const char* what)
{
    uint32_t i;
    if (!released.empty())
    {
        i = released.back();
        released.pop_back();
    }
    else
    {
        if (next >= capacity) fail("%s descriptor heap exhausted (%u)", what, capacity);
        i = next++;
    }
    if (live.size() <= i) live.resize((size_t)i + 1 + i / 2, 0);
    live[i] = 1;
    return i;
}

void DescriptorHeaps::FreeList::give(uint32_t index)
{
    // Two owners of one slot silently redirect a view to another resource: fail at the second free instead.
    if (index >= live.size() || !live[index]) fail("descriptor %u freed twice (or never allocated)", index);
    live[index] = 0;
    released.push_back(index);
}

DescriptorHeaps::DescriptorHeaps(ID3D12Device* device)
{
    D3D12_DESCRIPTOR_HEAP_DESC d{};
    d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    d.NumDescriptors = kResourceCapacity;
    d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    check(device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&m_resource)), "CreateDescriptorHeap(CBV_SRV_UAV)");
    d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    d.NumDescriptors = kSamplerCapacity;
    check(device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&m_sampler)), "CreateDescriptorHeap(SAMPLER)");
    d.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    d.NumDescriptors = kRtvCapacity;
    check(device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&m_rtvHeap)), "CreateDescriptorHeap(RTV)");
    d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    d.NumDescriptors = kDsvCapacity;
    check(device->CreateDescriptorHeap(&d, IID_PPV_ARGS(&m_dsvHeap)), "CreateDescriptorHeap(DSV)");
    m_resourceStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_rtvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    m_dsvStride = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    m_resourceFree.capacity = kResourceCapacity;
    m_rtvFree.capacity = kRtvCapacity;
    m_dsvFree.capacity = kDsvCapacity;
}

uint32_t DescriptorHeaps::allocateResource()
{
    std::lock_guard lock(m_mutex);
    return m_resourceFree.take("CBV/SRV/UAV");
}

void DescriptorHeaps::freeResource(uint32_t index)
{
    std::lock_guard lock(m_mutex);
    m_resourceFree.give(index);
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeaps::resourceCpu(uint32_t index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_resource->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)index * m_resourceStride;
    return h;
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeaps::resourceGpu(uint32_t index) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE h = m_resource->GetGPUDescriptorHandleForHeapStart();
    h.ptr += (UINT64)index * m_resourceStride;
    return h;
}

uint32_t DescriptorHeaps::allocateRtv()
{
    std::lock_guard lock(m_mutex);
    return m_rtvFree.take("RTV");
}

void DescriptorHeaps::freeRtv(uint32_t index)
{
    std::lock_guard lock(m_mutex);
    m_rtvFree.give(index);
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeaps::rtv(uint32_t index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_rtvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)index * m_rtvStride;
    return h;
}

uint32_t DescriptorHeaps::allocateDsv()
{
    std::lock_guard lock(m_mutex);
    return m_dsvFree.take("DSV");
}

void DescriptorHeaps::freeDsv(uint32_t index)
{
    std::lock_guard lock(m_mutex);
    m_dsvFree.give(index);
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeaps::dsv(uint32_t index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE h = m_dsvHeap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += (SIZE_T)index * m_dsvStride;
    return h;
}

uint32_t DescriptorHeaps::resourcesInUse() const
{
    std::lock_guard lock(m_mutex);
    return m_resourceFree.next - (uint32_t)m_resourceFree.released.size();
}
} // namespace unx::render
