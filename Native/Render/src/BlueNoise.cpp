// Blue noise tile (unx/render/BlueNoise.h): void and cluster.
#include "unx/render/BlueNoise.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace unx::render
{
namespace
{
constexpr uint32_t kSize = kBlueNoiseSize, kCount = kSize * kSize;

uint32_t hash(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

// One pattern: the rank of every texel (a permutation of 0..kCount-1).
std::vector<uint16_t> voidAndCluster(uint32_t seed)
{
    // the energy a set texel adds at every toroidal offset (Gaussian, sigma 1.9: Ulichney's value for a sharp blue spectrum)
    std::vector<float> kernel(kCount);
    const float sigma = 1.9f;
    for (uint32_t y = 0; y < kSize; ++y)
        for (uint32_t x = 0; x < kSize; ++x)
        {
            const float dx = (float)std::min(x, kSize - x), dy = (float)std::min(y, kSize - y);
            kernel[y * kSize + x] = std::exp(-(dx * dx + dy * dy) / (2 * sigma * sigma));
        }
    std::vector<uint8_t> set(kCount, 0);
    std::vector<float> energy(kCount, 0.0f);
    auto apply = [&](uint32_t at, float sign) {
        const uint32_t ax = at % kSize, ay = at / kSize;
        for (uint32_t y = 0; y < kSize; ++y)
        {
            const float* row = &kernel[((y + kSize - ay) % kSize) * kSize];
            float* out = &energy[y * kSize];
            for (uint32_t x = 0; x < kSize; ++x) out[x] += sign * row[(x + kSize - ax) % kSize];
        }
    };
    auto tightestCluster = [&](const std::vector<uint8_t>& pattern) {
        uint32_t best = 0;
        float value = -1e30f;
        for (uint32_t i = 0; i < kCount; ++i)
            if (pattern[i] && energy[i] > value) value = energy[i], best = i;
        return best;
    };
    auto largestVoid = [&](const std::vector<uint8_t>& pattern) {
        uint32_t best = 0;
        float value = 1e30f;
        for (uint32_t i = 0; i < kCount; ++i)
            if (!pattern[i] && energy[i] < value) value = energy[i], best = i;
        return best;
    };
    // the initial pattern: a tenth of the texels at random, then relaxed until the tightest cluster is the largest void
    const uint32_t ones = kCount / 10;
    uint32_t placed = 0, state = seed * 0x9E3779B1u + 0x85EBCA77u;
    while (placed < ones)
    {
        state = hash(state + placed + 1);
        const uint32_t at = state % kCount;
        if (set[at]) continue;
        set[at] = 1;
        apply(at, 1.0f);
        ++placed;
    }
    for (uint32_t guard = 0; guard < kCount * 4; ++guard)
    {
        const uint32_t cluster = tightestCluster(set);
        set[cluster] = 0;
        apply(cluster, -1.0f);
        const uint32_t hole = largestVoid(set);
        set[hole] = 1;
        apply(hole, 1.0f);
        if (hole == cluster) break;
    }
    std::vector<uint16_t> rank(kCount, 0);
    // phase 1: the initial pattern's texels, from the tightest cluster down - ranks ones-1 .. 0
    {
        std::vector<uint8_t> pattern = set;
        const std::vector<float> saved = energy;
        for (int r = (int)ones - 1; r >= 0; --r)
        {
            const uint32_t cluster = tightestCluster(pattern);
            pattern[cluster] = 0;
            apply(cluster, -1.0f);
            rank[cluster] = (uint16_t)r;
        }
        energy = saved;
    }
    // phases 2 and 3: the largest void takes the next rank until every texel has one
    for (uint32_t r = ones; r < kCount; ++r)
    {
        const uint32_t hole = largestVoid(set);
        set[hole] = 1;
        apply(hole, 1.0f);
        rank[hole] = (uint16_t)r;
    }
    return rank;
}
} // namespace

const std::vector<uint16_t>& blueNoiseTile()
{
    static const std::vector<uint16_t> tile = [] {
        std::vector<uint16_t> t((size_t)kCount * 4);
        for (uint32_t c = 0; c < 4; ++c)
        {
            const std::vector<uint16_t> rank = voidAndCluster(c + 1);
            for (uint32_t i = 0; i < kCount; ++i) t[(size_t)i * 4 + c] = (uint16_t)std::min(65535.0, std::floor(((double)rank[i] + 0.5) / kCount * 65536.0));
        }
        return t;
    }();
    return tile;
}

BlueNoiseTexture createBlueNoise(Device& device)
{
    const std::vector<uint16_t>& tile = blueNoiseTile();
    BlueNoiseTexture out;
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT }, upload{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = kSize;
    d.Height = kSize;
    d.DepthOrArraySize = d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
    d.SampleDesc.Count = 1;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_COPY_DEST, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&out.texture)),
          "blue noise texture");
    out.texture->SetName(L"blue noise tile");
    const D3D12_RESOURCE_DESC legacy = out.texture->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    uint64_t bytes = 0;
    device.d3d()->GetCopyableFootprints(&legacy, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
    D3D12_RESOURCE_DESC1 b{};
    b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    b.Width = bytes;
    b.Height = b.DepthOrArraySize = b.MipLevels = 1;
    b.SampleDesc.Count = 1;
    b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> staging;
    check(device.d3d()->CreateCommittedResource3(&upload, D3D12_HEAP_FLAG_NONE, &b, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&staging)),
          "blue noise staging");
    uint8_t* mapped = nullptr;
    D3D12_RANGE none{ 0, 0 };
    check(staging->Map(0, &none, reinterpret_cast<void**>(&mapped)), "map blue noise staging");
    for (uint32_t y = 0; y < kSize; ++y) std::memcpy(mapped + footprint.Offset + (size_t)y * footprint.Footprint.RowPitch, &tile[(size_t)y * kSize * 4], (size_t)kSize * 8);
    staging->Unmap(0, nullptr);

    CommandList cl = device.acquireCommandList(QueueType::Graphics);
    D3D12_TEXTURE_COPY_LOCATION to{}, from{};
    to.pResource = out.texture.Get();
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.pResource = staging.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint = footprint;
    cl.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    D3D12_BARRIER_GROUP group{};
    D3D12_TEXTURE_BARRIER barrier{};
    barrier.SyncBefore = D3D12_BARRIER_SYNC_COPY;
    barrier.SyncAfter = D3D12_BARRIER_SYNC_NONE;
    barrier.AccessBefore = D3D12_BARRIER_ACCESS_COPY_DEST;
    barrier.AccessAfter = D3D12_BARRIER_ACCESS_NO_ACCESS;
    barrier.LayoutBefore = D3D12_BARRIER_LAYOUT_COPY_DEST;
    barrier.LayoutAfter = D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
    barrier.pResource = out.texture.Get();
    barrier.Subresources.IndexOrFirstMipLevel = 0xFFFFFFFFu;
    group.Type = D3D12_BARRIER_TYPE_TEXTURE;
    group.NumBarriers = 1;
    group.pTextureBarriers = &barrier;
    cl.list->Barrier(1, &group);
    const uint64_t fence = device.submit(cl);
    device.queue(QueueType::Graphics).waitCpu(fence);

    out.srv = device.descriptors().allocateResource();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Format = DXGI_FORMAT_R16G16B16A16_UNORM;
    sd.Texture2D.MipLevels = 1;
    device.d3d()->CreateShaderResourceView(out.texture.Get(), &sd, device.descriptors().resourceCpu(out.srv));
    return out;
}
} // namespace unx::render
