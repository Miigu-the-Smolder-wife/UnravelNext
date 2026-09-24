#pragma once
// Resource helpers shared by the S modules (a copy in each module folder; modules do not link each other).
#include "unx/render/Device.h"


namespace unx::render::s_detail
{
inline ComPtr<ID3D12Resource> createTexture(Device& device, const wchar_t* name, D3D12_RESOURCE_DIMENSION dimension, uint32_t width, uint32_t height,
                                            uint16_t depth, DXGI_FORMAT format, D3D12_BARRIER_LAYOUT layout, bool uav = true)
{
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = dimension;
    d.Width = width;
    d.Height = height;
    d.DepthOrArraySize = depth;
    d.MipLevels = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> r;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, layout, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)), "S texture");
    r->SetName(name);
    return r;
}

inline ComPtr<ID3D12Resource> createBuffer(Device& device, const wchar_t* name, uint64_t bytes, D3D12_HEAP_TYPE type = D3D12_HEAP_TYPE_DEFAULT)
{
    D3D12_HEAP_PROPERTIES heap{ type };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = type == D3D12_HEAP_TYPE_DEFAULT ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> r;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)), "S buffer");
    r->SetName(name);
    return r;
}

inline uint32_t groups(uint32_t n, uint32_t size) { return (n + size - 1) / size; }
} // namespace unx::render::s_detail
