// Water stage 2 sun-space map (track W). See include/unx/water/WaterSunMap.h and WaterLight.hlsli.
#include "unx/water/WaterSunMap.h"

#include "unx/core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace unx::water
{
using namespace unx::render;

WaterSunMap::WaterSunMap(Device& device) : m_device(device)
{
    for (uint32_t i = 0; i < kRing; ++i)
    {
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC1 d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = 256;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_upload[i])),
              "water sun map constants");
        m_upload[i]->SetName(L"water sun map constants");
        D3D12_RANGE none{ 0, 0 };
        check(m_upload[i]->Map(0, &none, reinterpret_cast<void**>(&m_mapped[i])), "map water sun map constants");
    }
}
WaterSunMap::~WaterSunMap()
{
    for (auto& u : m_upload)
    {
        u->Unmap(0, nullptr);
        m_device.deferRelease(u);
    }
}

WaterSunMapOutput WaterSunMap::record(RenderGraph& g, ShaderLibrary& shaders, uint64_t frame, const std::vector<WaterSunStream>& streams, float3 sunDirection)
{
    WaterSunMapOutput out;
    // The union of the streams' bounds.
    double lo[3] = { 1e30, 1e30, 1e30 }, hi[3] = { -1e30, -1e30, -1e30 };
    std::vector<const WaterSunStream*> drawn;
    for (const WaterSunStream& s : streams)
    {
        const float3 a = s.stream.boundsMin, b = s.stream.boundsMax;
        if (!s.stream.vertices.valid() || !s.stream.drawArgs.valid() || !s.stream.maxTriangles || !(a.x <= b.x && a.y <= b.y && a.z <= b.z)) continue;
        lo[0] = std::min(lo[0], (double)a.x), lo[1] = std::min(lo[1], (double)a.y), lo[2] = std::min(lo[2], (double)a.z);
        hi[0] = std::max(hi[0], (double)b.x), hi[1] = std::max(hi[1], (double)b.y), hi[2] = std::max(hi[2], (double)b.z);
        drawn.push_back(&s);
    }
    if (drawn.empty()) return out;
    // The sun's frame: s towards the sun, right and up across it.
    double s[3] = { sunDirection.x, sunDirection.y, sunDirection.z };
    const double sl = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
    if (!(sl > 0)) fail("W: the sun direction is zero");
    for (double& v : s) v /= sl;
    const double ref[3] = { std::fabs(s[1]) < 0.99 ? 0.0 : 1.0, std::fabs(s[1]) < 0.99 ? 1.0 : 0.0, 0.0 };
    double r[3] = { ref[1] * s[2] - ref[2] * s[1], ref[2] * s[0] - ref[0] * s[2], ref[0] * s[1] - ref[1] * s[0] };
    const double rl = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    for (double& v : r) v /= rl;
    const double u[3] = { s[1] * r[2] - s[2] * r[1], s[2] * r[0] - s[0] * r[2], s[0] * r[1] - s[1] * r[0] };
    double minR = 1e30, maxR = -1e30, minU = 1e30, maxU = -1e30, minS = 1e30, maxS = -1e30;
    for (int c = 0; c < 8; ++c)
    {
        const double p[3] = { (c & 1) ? hi[0] : lo[0], (c & 2) ? hi[1] : lo[1], (c & 4) ? hi[2] : lo[2] };
        const double pr = p[0] * r[0] + p[1] * r[1] + p[2] * r[2], pu = p[0] * u[0] + p[1] * u[1] + p[2] * u[2], ps = p[0] * s[0] + p[1] * s[1] + p[2] * s[2];
        minR = std::min(minR, pr), maxR = std::max(maxR, pr), minU = std::min(minU, pu), maxU = std::max(maxU, pu), minS = std::min(minS, ps), maxS = std::max(maxS, ps);
    }
    const double side = std::max({ maxR - minR, maxU - minU, 1e-3 });
    uint32_t n = 256;
    while (n < 2048 && side / n > 0.002) n *= 2;
    // One texel of margin on each side (the rasteriser's pixel-centre rule at the bounds' edges).
    const double texel = side / (n - 2);
    minR -= texel, minU -= texel;
    const double extent = texel * n, range = std::max(maxS - minS, 1e-3) * (1 + 1e-4);
    const uint32_t head[4] = { 1, n, 0, 0 };
    const float rows[16] = { (float)r[0], (float)r[1], (float)r[2], (float)minR, (float)u[0], (float)u[1], (float)u[2], (float)minU,
                             (float)s[0], (float)s[1], (float)s[2], (float)minS, (float)(1 / extent), (float)(1 / extent), (float)range, 0 };
    uint8_t* mapped = m_mapped[frame % kRing];
    std::memcpy(mapped, head, 16);
    std::memcpy(mapped + 16, rows, 64);

    out.texels = n;
    out.depth = g.createTexture(TextureDesc{ "w.sun map depth", n, n, 1, 1, DXGI_FORMAT_D32_FLOAT });
    out.normal = g.createTexture(TextureDesc{ "w.sun map normal", n, n, 1, 1, DXGI_FORMAT_R16G16_FLOAT });
    out.medium = g.createTexture(TextureDesc{ "w.sun map medium", n, n, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
    out.constants = g.createBuffer({ "w.sun map constants", kConstantBytes, 0 });
    const BufferRef constants = out.constants;
    ID3D12Resource* upload = m_upload[frame % kRing].Get();
    g.addPass("w.sun map constants", QueueType::Graphics, [&](PassBuilder& b) { b.use(constants, Use::CopyDst); },
              [=](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(constants), 0, upload, 0, kConstantBytes); });
    MeshPipelineDesc d;
    d.meshShader = "Passes/Water/WaterSunMap.ms";
    d.pixelShader = "Passes/Water/WaterSunMap.ps";
    d.renderTargets = { DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R16G16B16A16_FLOAT };
    d.depthFormat = DXGI_FORMAT_D32_FLOAT;
    d.depthFunc = D3D12_COMPARISON_FUNC_GREATER;
    d.cull = D3D12_CULL_MODE_NONE;
    ID3D12PipelineState* pso = shaders.mesh("w.sun map", d);
    const TextureRef depth = out.depth, normal = out.normal, medium = out.medium;
    struct Draw
    {
        BufferRef vertices, args;
        uint32_t capacity;
        float medium[4];
    };
    std::vector<Draw> draws;
    for (const WaterSunStream* w : drawn)
        draws.push_back({ w->stream.vertices, w->stream.drawArgs, w->stream.maxTriangles, { w->transmittance[0], w->transmittance[1], w->transmittance[2], w->ior } });
    g.addPass("w.sun map raster", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(normal, Use::RenderTarget);
                  b.use(medium, Use::RenderTarget);
                  b.use(depth, Use::DepthWrite);
                  b.use(constants, Use::SrvGraphics);
                  for (const Draw& dr : draws)
                  {
                      b.use(dr.vertices, Use::SrvGraphics);
                      b.use(dr.args, Use::SrvGraphics);
                  }
              },
              [=](PassContext& c) {
                  const D3D12_CPU_DESCRIPTOR_HANDLE rtv[2] = { c.rtv(normal), c.rtv(medium) }, dsv = c.dsv(depth);
                  const float zero[4] = {};
                  c.cmd->ClearRenderTargetView(rtv[0], zero, 0, nullptr);
                  c.cmd->ClearRenderTargetView(rtv[1], zero, 0, nullptr);
                  c.cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
                  c.cmd->OMSetRenderTargets(2, rtv, FALSE, &dsv);
                  D3D12_VIEWPORT vp{ 0, 0, (float)n, (float)n, 0, 1 };
                  D3D12_RECT sc{ 0, 0, (LONG)n, (LONG)n };
                  c.cmd->RSSetViewports(1, &vp);
                  c.cmd->RSSetScissorRects(1, &sc);
                  c.cmd->SetPipelineState(pso);
                  for (const Draw& dr : draws)
                  {
                      uint32_t k[8] = { c.srv(dr.vertices), c.srv(dr.args), dr.capacity, c.srv(constants) };
                      std::memcpy(&k[4], dr.medium, 16);
                      c.graphicsConstants(k, 8);
                      const uint32_t groups = (dr.capacity + 31) / 32;
                      c.cmd->DispatchMesh(std::min(65535u, groups), (groups + 65534) / 65535, 1);
                  }
              });
    // Caustics (WaterCaustics.hlsl): the map's texels' refracted sunlight splatted onto the slices.
    const uint32_t nc = std::min(n, kCausticMax);
    out.caustics = g.createTexture(TextureDesc{ "w.sun map caustics", nc, nc, uint16_t(kCausticSlices), 1, DXGI_FORMAT_R32_UINT });
    const TextureRef caustics = out.caustics;
    ID3D12PipelineState* zero = shaders.compute("Passes/Water/WaterCausticsClear");
    ID3D12PipelineState* splat = shaders.compute("Passes/Water/WaterCaustics");
    g.addPass("w.sun map caustics clear", QueueType::Graphics, [&](PassBuilder& b) { b.use(caustics, Use::UavCompute); },
              [=](PassContext& c) {
                  const uint32_t k[4] = { c.uav(caustics), nc, kCausticSlices, 0 };
                  c.cmd->SetPipelineState(zero);
                  c.computeConstants(k, 4);
                  c.cmd->Dispatch((nc + 7) / 8, (nc + 7) / 8, kCausticSlices);
              });
    g.addPass("w.sun map caustics", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(depth, Use::SrvCompute);
                  b.use(normal, Use::SrvCompute);
                  b.use(medium, Use::SrvCompute);
                  b.use(constants, Use::SrvCompute);
                  b.use(caustics, Use::UavCompute);
              },
              [=](PassContext& c) {
                  const uint32_t k[8] = { c.srv(depth), c.srv(normal), c.srv(medium), c.srv(constants), c.uav(caustics), 0, 0, 0 };
                  c.cmd->SetPipelineState(splat);
                  c.computeConstants(k, 8);
                  c.cmd->Dispatch((n + 7) / 8, (n + 7) / 8, 1);
              });
    return out;
}
} // namespace unx::water
