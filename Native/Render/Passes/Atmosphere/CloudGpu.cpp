// Volumetric clouds, GPU resources (CloudGpu.h).
#include "CloudGpu.h"

#include <cstring>

namespace unx::render::clouds
{
namespace
{
ComPtr<ID3D12Resource> texture(Device& device, D3D12_RESOURCE_DIMENSION dimension, uint32_t w, uint32_t h, uint16_t d, DXGI_FORMAT format, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC1 desc{};
    desc.Dimension = dimension;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = d;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    ComPtr<ID3D12Resource> r;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_COPY_DEST, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)),
          "cloud noise texture");
    r->SetName(name);
    return r;
}

// Copies texel bytes (rows of w x bytesPerTexel, h rows per slice, d slices) into the texture, then moves it to the
// shader-resource layout; waits for the copy (a one-time upload).
void fill(Device& device, ID3D12Resource* dst, const uint8_t* src, uint32_t w, uint32_t h, uint32_t d, uint32_t bytesPerTexel)
{
    const D3D12_RESOURCE_DESC desc = dst->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT rows = 0;
    UINT64 rowBytes = 0, total = 0;
    device.d3d()->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, &rows, &rowBytes, &total);
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC1 bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = total;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> staging;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&staging)),
          "cloud noise staging");
    uint8_t* m = nullptr;
    D3D12_RANGE none{ 0, 0 };
    check(staging->Map(0, &none, reinterpret_cast<void**>(&m)), "map cloud noise staging");
    const size_t row = (size_t)w * bytesPerTexel, slicePitch = (size_t)footprint.Footprint.RowPitch * rows;
    for (uint32_t z = 0; z < d; ++z)
        for (uint32_t y = 0; y < h; ++y)
            std::memcpy(m + footprint.Offset + z * slicePitch + (size_t)y * footprint.Footprint.RowPitch, src + ((size_t)z * h + y) * row, row);
    staging->Unmap(0, nullptr);
    CommandList cl = device.acquireCommandList(QueueType::Graphics);
    D3D12_TEXTURE_COPY_LOCATION to{}, from{};
    to.pResource = dst;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.pResource = staging.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint = footprint;
    cl.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    D3D12_TEXTURE_BARRIER b{ D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_SYNC_NONE, D3D12_BARRIER_ACCESS_COPY_DEST, D3D12_BARRIER_ACCESS_NO_ACCESS,
                             D3D12_BARRIER_LAYOUT_COPY_DEST, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE, dst, { 0xFFFFFFFFu, 0, 0, 0, 0, 0 }, D3D12_TEXTURE_BARRIER_FLAG_NONE };
    D3D12_BARRIER_GROUP g{ D3D12_BARRIER_TYPE_TEXTURE, 1 };
    g.pTextureBarriers = &b;
    cl.list->Barrier(1, &g);
    device.queue(QueueType::Graphics).waitCpu(device.submit(cl));
}

uint32_t srv(Device& device, ID3D12Resource* r, D3D12_SRV_DIMENSION dimension, DXGI_FORMAT format)
{
    const uint32_t index = device.descriptors().allocateResource();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = dimension;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Format = format;
    if (dimension == D3D12_SRV_DIMENSION_TEXTURE3D) sd.Texture3D.MipLevels = 1;
    else sd.Texture2D.MipLevels = 1;
    device.d3d()->CreateShaderResourceView(r, &sd, device.descriptors().resourceCpu(index));
    return index;
}
} // namespace

CloudTextures uploadTextures(Device& device, const CloudNoise& noise)
{
    CloudTextures t;
    t.shape = texture(device, D3D12_RESOURCE_DIMENSION_TEXTURE3D, kShapeSize, kShapeSize, (uint16_t)kShapeSize, DXGI_FORMAT_R8_UNORM, L"S cloud shape noise");
    t.detail = texture(device, D3D12_RESOURCE_DIMENSION_TEXTURE3D, kDetailSize, kDetailSize, (uint16_t)kDetailSize, DXGI_FORMAT_R8_UNORM, L"S cloud detail noise");
    t.weather = texture(device, D3D12_RESOURCE_DIMENSION_TEXTURE2D, kWeatherSize, kWeatherSize, 1, DXGI_FORMAT_R8G8_UNORM, L"S cloud weather map");
    fill(device, t.shape.Get(), noise.shape.data(), kShapeSize, kShapeSize, kShapeSize, 1);
    fill(device, t.detail.Get(), noise.detail.data(), kDetailSize, kDetailSize, kDetailSize, 1);
    fill(device, t.weather.Get(), noise.weather.data(), kWeatherSize, kWeatherSize, 1, 2);
    t.shapeSrv = srv(device, t.shape.Get(), D3D12_SRV_DIMENSION_TEXTURE3D, DXGI_FORMAT_R8_UNORM);
    t.detailSrv = srv(device, t.detail.Get(), D3D12_SRV_DIMENSION_TEXTURE3D, DXGI_FORMAT_R8_UNORM);
    t.weatherSrv = srv(device, t.weather.Get(), D3D12_SRV_DIMENSION_TEXTURE2D, DXGI_FORMAT_R8G8_UNORM);
    return t;
}

void releaseTextures(Device& device, CloudTextures& t)
{
    for (uint32_t i : { t.shapeSrv, t.detailSrv, t.weatherSrv })
        if (i) device.descriptors().freeResource(i);
    device.deferRelease(t.shape);
    device.deferRelease(t.detail);
    device.deferRelease(t.weather);
    t = {};
}

CloudRecord makeRecord(const CloudLayer& layer, const CloudOffsets& o, const CloudTextures& t, double bottomRadius, const float sunDir[3], const float sunIlluminance[3],
                       const float shadowCentre[3], float shadowHalfExtent, uint32_t shadowTexels)
{
    CloudRecord r{};
    r.base = layer.baseAltitude, r.top = layer.topAltitude, r.coverage = layer.coverage, r.sigmaMax = layer.sigmaMax;
    r.albedo = layer.albedo, r.detailStrength = layer.detailStrength, r.g0 = layer.g0, r.g1 = layer.g1;
    r.lobeBlend = layer.lobeBlend, r.invShape = 1 / layer.shapePeriod, r.invDetail = 1 / layer.detailPeriod, r.invWeather = 1 / layer.weatherPeriod;
    for (int k = 0; k < 3; ++k)
    {
        r.shapeOffset[k] = o.shape[k], r.detailOffset[k] = o.detail[k], r.origin[k] = o.origin[k];
        r.sunDir[k] = sunDir[k], r.shadowCentre[k] = shadowCentre[k], r.sunIlluminance[k] = sunIlluminance[k];
    }
    r.bottomRadius = (float)bottomRadius;
    r.weatherOffset[0] = o.weather[0], r.weatherOffset[1] = o.weather[1];
    r.shape = t.shapeSrv, r.detail = t.detailSrv, r.weather = t.weatherSrv;
    r.shadowHalfExtent = shadowHalfExtent, r.shadowTexels = (float)shadowTexels;
    return r;
}
} // namespace unx::render::clouds
