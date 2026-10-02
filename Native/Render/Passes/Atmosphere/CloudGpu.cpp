// Volumetric clouds, GPU resources (CloudGpu.h).
#include "CloudGpu.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace unx::render::clouds
{
namespace
{
ComPtr<ID3D12Resource> texture(Device& device, D3D12_RESOURCE_DIMENSION dimension, uint32_t w, uint32_t h, uint16_t d, DXGI_FORMAT format, const wchar_t* name,
                               uint16_t mips = 1)
{
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC1 desc{};
    desc.Dimension = dimension;
    desc.Width = w;
    desc.Height = h;
    desc.DepthOrArraySize = d;
    desc.MipLevels = mips;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    ComPtr<ID3D12Resource> r;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_BARRIER_LAYOUT_COPY_DEST, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)),
          "cloud noise texture");
    r->SetName(name);
    return r;
}

// Copies texel bytes (per mip: rows of its width x bytesPerTexel, its height in rows per slice, its depth in slices; mip
// m of a w x h x d texture is max(w >> m, 1) x ...) into the texture, then moves it to the shader-resource layout; waits
// for the copy (a one-time upload).
void fill(Device& device, ID3D12Resource* dst, const std::vector<const uint8_t*>& mips, uint32_t w, uint32_t h, uint32_t d, uint32_t bytesPerTexel)
{
    const D3D12_RESOURCE_DESC desc = dst->GetDesc();
    const uint32_t count = (uint32_t)mips.size();
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(count);
    std::vector<UINT> rowCounts(count);
    UINT64 total = 0;
    device.d3d()->GetCopyableFootprints(&desc, 0, count, 0, footprints.data(), rowCounts.data(), nullptr, &total);
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
    for (uint32_t level = 0; level < count; ++level)
    {
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint = footprints[level];
        const uint32_t lw = std::max(w >> level, 1u), lh = std::max(h >> level, 1u), ld = std::max(d >> level, 1u);
        const size_t row = (size_t)lw * bytesPerTexel, slicePitch = (size_t)footprint.Footprint.RowPitch * rowCounts[level];
        for (uint32_t z = 0; z < ld; ++z)
            for (uint32_t y = 0; y < lh; ++y)
                std::memcpy(m + footprint.Offset + z * slicePitch + (size_t)y * footprint.Footprint.RowPitch, mips[level] + ((size_t)z * lh + y) * row, row);
    }
    staging->Unmap(0, nullptr);
    CommandList cl = device.acquireCommandList(QueueType::Graphics);
    for (uint32_t level = 0; level < count; ++level)
    {
        D3D12_TEXTURE_COPY_LOCATION to{}, from{};
        to.pResource = dst;
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.SubresourceIndex = level;
        from.pResource = staging.Get();
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint = footprints[level];
        cl.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
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
    if (dimension == D3D12_SRV_DIMENSION_TEXTURE3D) sd.Texture3D.MipLevels = (UINT)-1;  // (every mip the texture has)
    else sd.Texture2D.MipLevels = 1;
    device.d3d()->CreateShaderResourceView(r, &sd, device.descriptors().resourceCpu(index));
    return index;
}
// The mips of a noise volume (n^3, R8) for the kernels' filtered density (CloudCommon.hlsli cloudDensityFiltered), two
// bytes per texel: the mean of the level-0 texels under the texel, and their standard deviation x 2 (a value in [0, 1]
// has a deviation of at most 0.5). Level 0: the texel itself, deviation 0 - its first channel is the R8 texture's value,
// so a kernel that reads one channel at mip 0 (cloudDensity, the tests' twins) reads what it read before. n: a power of
// two; down to 1^3.
std::vector<std::vector<uint8_t>> noiseMips(const std::vector<uint8_t>& texels, uint32_t n)
{
    std::vector<std::vector<uint8_t>> levels;
    // per level: the mean and the mean of the squares of the level-0 values under each texel
    std::vector<float> mean(texels.size()), square(texels.size());
    for (size_t i = 0; i < texels.size(); ++i)
    {
        mean[i] = texels[i] / 255.0f;
        square[i] = mean[i] * mean[i];
    }
    for (uint32_t size = n;; size /= 2)
    {
        std::vector<uint8_t> level((size_t)size * size * size * 2);
        for (size_t i = 0; i < (size_t)size * size * size; ++i)
        {
            const float deviation = std::sqrt(std::max(square[i] - mean[i] * mean[i], 0.0f));
            level[2 * i] = size == n ? texels[i] : (uint8_t)std::lround(std::clamp(mean[i], 0.0f, 1.0f) * 255.0f);
            level[2 * i + 1] = size == n ? (uint8_t)0 : (uint8_t)std::lround(std::clamp(2.0f * deviation, 0.0f, 1.0f) * 255.0f);
        }
        levels.push_back(std::move(level));
        if (size == 1) break;
        const uint32_t half = size / 2;
        std::vector<float> nextMean((size_t)half * half * half), nextSquare(nextMean.size());
        for (uint32_t z = 0; z < half; ++z)
            for (uint32_t y = 0; y < half; ++y)
                for (uint32_t x = 0; x < half; ++x)
                {
                    float m = 0, q = 0;
                    for (uint32_t c = 0; c < 8; ++c)
                    {
                        const size_t at = ((size_t)(2 * z + (c >> 2)) * size + (2 * y + ((c >> 1) & 1))) * size + (2 * x + (c & 1));
                        m += mean[at];
                        q += square[at];
                    }
                    nextMean[((size_t)z * half + y) * half + x] = m * 0.125f;
                    nextSquare[((size_t)z * half + y) * half + x] = q * 0.125f;
                }
        mean = std::move(nextMean);
        square = std::move(nextSquare);
    }
    return levels;
}
std::vector<const uint8_t*> pointers(const std::vector<std::vector<uint8_t>>& levels)
{
    std::vector<const uint8_t*> p;
    for (const std::vector<uint8_t>& l : levels) p.push_back(l.data());
    return p;
}
} // namespace

CloudTextures uploadTextures(Device& device, const CloudNoise& noise)
{
    CloudTextures t;
    // shape and detail: R8G8 with every mip (noiseMips: mean, deviation); the weather map: the two channels it has, no mips
    const std::vector<std::vector<uint8_t>> shape = noiseMips(noise.shape, kShapeSize), detail = noiseMips(noise.detail, kDetailSize);
    t.shape = texture(device, D3D12_RESOURCE_DIMENSION_TEXTURE3D, kShapeSize, kShapeSize, (uint16_t)kShapeSize, DXGI_FORMAT_R8G8_UNORM, L"S cloud shape noise",
                      (uint16_t)shape.size());
    t.detail = texture(device, D3D12_RESOURCE_DIMENSION_TEXTURE3D, kDetailSize, kDetailSize, (uint16_t)kDetailSize, DXGI_FORMAT_R8G8_UNORM, L"S cloud detail noise",
                       (uint16_t)detail.size());
    t.weather = texture(device, D3D12_RESOURCE_DIMENSION_TEXTURE2D, kWeatherSize, kWeatherSize, 1, DXGI_FORMAT_R8G8_UNORM, L"S cloud weather map");
    fill(device, t.shape.Get(), pointers(shape), kShapeSize, kShapeSize, kShapeSize, 2);
    fill(device, t.detail.Get(), pointers(detail), kDetailSize, kDetailSize, kDetailSize, 2);
    fill(device, t.weather.Get(), { noise.weather.data() }, kWeatherSize, kWeatherSize, 1, 2);
    t.shapeSrv = srv(device, t.shape.Get(), D3D12_SRV_DIMENSION_TEXTURE3D, DXGI_FORMAT_R8G8_UNORM);
    t.detailSrv = srv(device, t.detail.Get(), D3D12_SRV_DIMENSION_TEXTURE3D, DXGI_FORMAT_R8G8_UNORM);
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
