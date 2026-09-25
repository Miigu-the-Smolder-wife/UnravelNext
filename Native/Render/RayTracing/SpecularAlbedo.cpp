#include "unx/rt/SpecularAlbedo.h"

#include "unx/scene/MaterialModel.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>

namespace unx::render::rt
{
namespace model = scene::model;

namespace
{
float radicalInverse(uint32_t bits)
{
    bits = (bits << 16u) | (bits >> 16u);
    bits = ((bits & 0x55555555u) << 1u) | ((bits & 0xAAAAAAAAu) >> 1u);
    bits = ((bits & 0x33333333u) << 2u) | ((bits & 0xCCCCCCCCu) >> 2u);
    bits = ((bits & 0x0F0F0F0Fu) << 4u) | ((bits & 0xF0F0F0F0u) >> 4u);
    bits = ((bits & 0x00FF00FFu) << 8u) | ((bits & 0xFF00FF00u) >> 8u);
    return (float)bits * 2.3283064365386963e-10f;
}

std::vector<float> buildTable()
{
    const uint32_t n = model::kAlbedoTableSize, samples = 4096;
    std::vector<float> table(2 * n * n);
    for (uint32_t ri = 0; ri < n; ++ri)
        for (uint32_t mi = 0; mi < n; ++mi)
        {
            const float r = (float)ri / (n - 1);
            const float mu = std::max((float)mi / (n - 1), 1e-4f);
            const float alpha = model::alphaFromRoughness(r);
            const float3 v{ std::sqrt(1 - mu * mu), 0, mu };
            const float3 vh = normalize(float3{ alpha * v.x, alpha * v.y, v.z });
            const float lensq = vh.x * vh.x + vh.y * vh.y;
            const float3 t1 = lensq > 0 ? float3{ -vh.y, vh.x, 0 } / std::sqrt(lensq) : float3{ 1, 0, 0 };
            const float3 t2 = cross(vh, t1);
            const float g1v = 2 * mu / (mu + std::sqrt(alpha * alpha + (1 - alpha * alpha) * mu * mu));
            double a = 0, b = 0;
            for (uint32_t s = 0; s < samples; ++s)
            {
                const float u1 = (s + 0.5f) / samples, u2 = radicalInverse(s);
                const float radius = std::sqrt(u1), phi = 2 * model::kPi * u2;
                const float p1 = radius * std::cos(phi);
                const float sBlend = 0.5f * (1 + vh.z);
                const float p2 = (1 - sBlend) * std::sqrt(std::max(0.0f, 1 - p1 * p1)) + sBlend * radius * std::sin(phi);
                const float3 nh = t1 * p1 + t2 * p2 + vh * std::sqrt(std::max(0.0f, 1 - p1 * p1 - p2 * p2));
                const float3 h = normalize(float3{ alpha * nh.x, alpha * nh.y, std::max(0.0f, nh.z) });
                const float VoH = dot(v, h);
                const float3 l = h * (2 * VoH) - v;
                const float NoL = l.z;
                if (NoL <= 0) continue;
                const double weight = 4.0 * model::visibilitySmithGgxCorrelated(mu, NoL, alpha) * NoL * mu / g1v;
                const double w = std::pow(1.0 - std::clamp((double)VoH, 0.0, 1.0), 5.0);
                a += weight * (1 - w);
                b += weight * w;
            }
            table[2 * (ri * n + mi)] = (float)(a / samples);
            table[2 * (ri * n + mi) + 1] = (float)(b / samples);
        }
    return table;
}

struct Lut
{
    ComPtr<ID3D12Resource> buffer;
    uint32_t srv = 0xFFFFFFFFu;
};
std::mutex g_mutex;
std::map<Device*, Lut> g_luts;
} // namespace

const std::vector<float>& specularAlbedoTable()
{
    static const std::vector<float> t = buildTable();
    return t;
}

uint32_t specularAlbedoSrv(Device& device)
{
    std::lock_guard lock(g_mutex);
    Lut& lut = g_luts[&device];
    if (lut.buffer) return lut.srv;
    const std::vector<float>& t = specularAlbedoTable();
    const uint64_t bytes = t.size() * sizeof(float);
    D3D12_HEAP_PROPERTIES def{ D3D12_HEAP_TYPE_DEFAULT }, up{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC1 bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bytes;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    check(device.d3d()->CreateCommittedResource3(&def, D3D12_HEAP_FLAG_NONE, &bd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&lut.buffer)),
          "R specular albedo LUT");
    lut.buffer->SetName(L"R specular albedo LUT");
    ComPtr<ID3D12Resource> staging;
    check(device.d3d()->CreateCommittedResource3(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&staging)),
          "R specular LUT staging");
    void* p = nullptr;
    D3D12_RANGE none{ 0, 0 };
    check(staging->Map(0, &none, &p), "map R specular LUT staging");
    std::memcpy(p, t.data(), bytes);
    staging->Unmap(0, nullptr);
    CommandList cl = device.acquireCommandList(QueueType::Graphics);
    cl.list->CopyBufferRegion(lut.buffer.Get(), 0, staging.Get(), 0, bytes);
    device.queue(QueueType::Graphics).waitCpu(device.submit(cl));
    lut.srv = device.descriptors().allocateResource();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Format = DXGI_FORMAT_UNKNOWN;
    sd.Buffer.NumElements = (UINT)(t.size() / 2);
    sd.Buffer.StructureByteStride = 8;
    device.d3d()->CreateShaderResourceView(lut.buffer.Get(), &sd, device.descriptors().resourceCpu(lut.srv));
    return lut.srv;
}

void releaseSpecularAlbedo(Device& device)
{
    std::lock_guard lock(g_mutex);
    const auto it = g_luts.find(&device);
    if (it == g_luts.end()) return;
    device.waitIdle();
    if (it->second.srv != 0xFFFFFFFFu) device.descriptors().freeResource(it->second.srv);
    g_luts.erase(it);
}
} // namespace unx::render::rt
