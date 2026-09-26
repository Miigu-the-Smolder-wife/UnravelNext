// Water view grid (track W, B7). See include/unx/water/ViewGrid.h and ViewGrid.hlsli.
#include "unx/water/ViewGrid.h"

#include "unx/core/Log.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace unx::water
{
using namespace unx::render;

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr uint64_t kParamBytes = 128 + uint64_t(ViewGrid::kMaxRows) * 8;

double wrapNear(double phi, double reference)
{
    while (phi - reference > kPi) phi -= 2 * kPi;
    while (phi - reference < -kPi) phi += 2 * kPi;
    return phi;
}
} // namespace

ViewGrid::ViewGrid(Device& device, ShaderLibrary& shaders, uint32_t framesInFlight) : m_device(device), m_shaders(shaders)
{
    if (!framesInFlight) fail("view grid: no frames in flight");
    for (uint32_t s = 0; s < framesInFlight; ++s)
    {
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC1 d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = kParamBytes;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;
        check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)),
              "view grid parameters");
        r->SetName(L"view grid parameters");
        uint8_t* mapped = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(r->Map(0, &none, (void**)&mapped), "map view grid parameters");
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements = UINT(kParamBytes / 4);
        sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        const uint32_t srv = device.descriptors().allocateResource();
        device.d3d()->CreateShaderResourceView(r.Get(), &sd, device.descriptors().resourceCpu(srv));
        m_upload.push_back(r);
        m_mapped.push_back(mapped);
        m_srv.push_back(srv);
    }
}
ViewGrid::~ViewGrid()
{
    for (auto& u : m_upload)
    {
        u->Unmap(0, nullptr);
        m_device.deferRelease(u);
    }
    for (uint32_t srv : m_srv) m_device.descriptors().freeResource(srv);
}

ViewGridLayout ViewGrid::layout(const ViewGridCamera& c, const ViewGridWater& w, const float lengths[3])
{
    if (!c.width || !c.height || !(c.tanX > 0) || !(c.tanY > 0) || !(w.nearRadius > 0) || !(w.bound >= 0) || !(w.extent > w.nearRadius))
        fail("view grid: invalid camera or water description");
    ViewGridLayout out;
    const double theta = 2.0 * c.tanX / c.width;  // one pixel at the image centre
    const double yaw = std::atan2(double(c.forward[2]), double(c.forward[0]));
    // The screen's angular window (world azimuth, elevation) from its border pixels.
    double phiLo = 1e9, phiHi = -1e9, eLo = 1e9, eHi = -1e9;
    auto add = [&](double px, double py) {
        const double sx = px / c.width * 2 - 1, sy = 1 - py / c.height * 2;
        double d[3];
        for (int a = 0; a < 3; ++a) d[a] = c.forward[a] + sx * c.tanX * c.right[a] + sy * c.tanY * c.up[a];
        const double l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        const double phi = wrapNear(std::atan2(d[2], d[0]), yaw), e = std::asin(d[1] / l);
        phiLo = std::min(phiLo, phi); phiHi = std::max(phiHi, phi);
        eLo = std::min(eLo, e); eHi = std::max(eHi, e);
    };
    for (uint32_t x = 0; x <= c.width; x += 4) { add(x, 0); add(x, c.height); }
    for (uint32_t y = 0; y <= c.height; y += 4) { add(0, y); add(c.width, y); }
    out.window[0] = float(phiLo); out.window[1] = float(phiHi); out.window[2] = float(eLo); out.window[3] = float(eHi);
    const double h = double(c.position[1]) - w.level, margin = w.bound / w.nearRadius;
    double phi0 = phiLo - margin, phi1 = phiHi + margin;
    double e0 = eLo - margin, e1 = std::min(eHi + margin, 0.0);  // rows at the horizon end at the far ring
    if (h > 0) e0 = std::max(e0, -std::atan(h / w.nearRadius));
    if (w.lake && h > 0)
    {
        // The lake's angular box (its boundary widened by the bound, and its inside) narrows the window.
        double lp0 = 1e9, lp1 = -1e9, le0 = 1e9, le1 = -1e9;
        for (int k = 0; k < 720; ++k)
            for (double r : { double(w.lakeRadius) + w.bound, 0.5 * w.lakeRadius, 0.0 })
            {
                const double a = 2 * kPi * k / 720;
                const double x = w.lakeCentre[0] + r * std::cos(a) - c.position[0], z = w.lakeCentre[1] + r * std::sin(a) - c.position[2];
                const double phi = wrapNear(std::atan2(z, x), yaw), e = -std::atan(h / std::max(std::hypot(x, z), 1e-3));
                lp0 = std::min(lp0, phi); lp1 = std::max(lp1, phi); le0 = std::min(le0, e); le1 = std::max(le1, e);
            }
        phi0 = std::max(phi0, lp0 - margin); phi1 = std::min(phi1, lp1 + margin);
        e0 = std::max(e0, le0 - margin); e1 = std::min(e1, le1 + margin);
    }
    out.columns = uint32_t(std::max(0.0, std::ceil((phi1 - phi0) / theta))) + 1;
    // Rows by rest distance: spacing min(one pixel row on the still plane, 2.5 footprints while a crest of the bound's
    // height can reach 0.5 px there, r <= 2 A / theta), from the window's nearest to its farthest distance.
    std::vector<float> table;
    if (h > 0 && e1 > e0)
    {
        const double rStart = std::max(double(w.nearRadius), h / std::tan(std::min(-e0, 0.5 * kPi - 1e-6)));
        const double rEnd = e1 >= 0 ? double(w.extent) : std::min(double(w.extent), h / std::tan(-e1));
        for (double r = rStart;;)
        {
            const double flat = theta * (h * h + r * r) / h, crest = r <= 2 * w.bound / theta ? 2.5 * r * theta : 1e30;
            const double step = std::min(flat, crest);
            table.push_back(float(r));
            table.push_back(float(step));
            if (r >= rEnd) break;
            r = std::min(r + step, rEnd);
        }
    }
    out.rows = uint32_t(table.size() / 2);
    if (out.rows > kMaxRows) fail("view grid: %u rows exceed the table's %u", out.rows, kMaxRows);
    out.params.assign(32, 0.0f);
    out.params.insert(out.params.end(), table.begin(), table.end());
    float* p = out.params.data();
    const float row0[4] = { c.position[0], c.position[1], c.position[2], w.level };
    const float row1[4] = { c.right[0], c.right[1], c.right[2], c.tanX };
    const float row2[4] = { c.up[0], c.up[1], c.up[2], c.tanY };
    const float row3[4] = { c.forward[0], c.forward[1], c.forward[2], w.extent };
    const float row4[4] = { float(phi0), 0, float(theta), w.nearRadius };
    const uint32_t row5[4] = { out.columns, out.rows, c.width, c.height };
    const float row6[4] = { lengths[0], lengths[1], lengths[2], 0 };
    const float lake[3] = { w.lakeCentre[0], w.lakeCentre[1], w.lakeRadius };
    const uint32_t lakeOn = w.lake ? 1 : 0;
    std::memcpy(p, row0, 16); std::memcpy(p + 4, row1, 16); std::memcpy(p + 8, row2, 16); std::memcpy(p + 12, row3, 16);
    std::memcpy(p + 16, row4, 16); std::memcpy(p + 20, row5, 16); std::memcpy(p + 24, row6, 16); std::memcpy(p + 28, lake, 12); std::memcpy(p + 31, &lakeOn, 4);
    return out;
}

ViewGridOutput ViewGrid::record(RenderGraph& g, uint64_t frame, const OceanOutput& fields, const float lengths[3], const ViewGridCamera& camera, const ViewGridWater& water)
{
    const ViewGridLayout l = layout(camera, water, lengths);
    const uint32_t slot = uint32_t(frame % m_upload.size());
    std::memcpy(m_mapped[slot], l.params.data(), l.params.size() * 4);
    const uint32_t paramSrv = m_srv[slot], width = camera.width, height = camera.height, pixels = width * height;
    ViewGridOutput out;
    out.columns = l.columns;
    out.rows = l.rows;
    out.keys = g.createBuffer({ "view grid keys", uint64_t(pixels) * 8, 0 });
    out.counters = g.createBuffer({ "view grid counters", 256, 0 });
    out.surface = g.createTexture(TextureDesc{ "view grid surface", width, height, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT });
    out.depth = g.createTexture(TextureDesc{ "water depth", width, height, 1, 1, DXGI_FORMAT_R32_FLOAT });
    const BufferRef keys = out.keys, counters = out.counters;
    const TextureRef surface = out.surface, depth = out.depth, displacement = fields.displacement, slopes = fields.slopes;
    ID3D12PipelineState* clear = m_shaders.compute("Passes/Water/ViewGridClear");
    g.addPass("view grid clear", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(keys, Use::UavCompute); pb.use(counters, Use::UavCompute); },
              [=](PassContext& c) {
                  const uint32_t k[4] = { c.uav(keys), pixels, c.uav(counters), 64 };
                  c.cmd->SetPipelineState(clear);
                  c.computeConstants(k, 4);
                  c.cmd->Dispatch((pixels + 63) / 64, 1, 1);
              });
    const uint32_t groupsX = (l.columns + 7) / 8, groupsY = (l.rows + 7) / 8;
    const float bound = water.bound;
    const float window[4] = { l.window[0], l.window[1], l.window[2], l.window[3] };
    ID3D12PipelineState* scatter = m_shaders.compute("Passes/Water/ViewGridScatter");
    if (groupsY)
        g.addPass("view grid scatter", QueueType::Graphics,
                  [&](PassBuilder& pb) { pb.use(displacement, Use::SrvCompute); pb.use(keys, Use::UavCompute); pb.use(counters, Use::UavCompute); },
                  [=](PassContext& c) {
                      uint32_t k[12] = { paramSrv, c.srv(displacement), c.uav(keys), c.uav(counters) };
                      std::memcpy(&k[4], &bound, 4);
                      std::memcpy(&k[5], window, 16);
                      c.cmd->SetPipelineState(scatter);
                      c.computeConstants(k, 12);
                      c.cmd->Dispatch(groupsX, groupsY, 1);
                  });
    ID3D12PipelineState* resolve = m_shaders.compute("Passes/Water/ViewGridResolve");
    g.addPass("view grid resolve", QueueType::Graphics,
              [&](PassBuilder& pb) {
                  pb.use(displacement, Use::SrvCompute); pb.use(slopes, Use::SrvCompute); pb.use(keys, Use::SrvCompute);
                  pb.use(surface, Use::UavCompute); pb.use(depth, Use::UavCompute);
              },
              [=](PassContext& c) {
                  const uint32_t k[8] = { paramSrv, 0, c.srv(keys), c.srv(displacement), c.srv(slopes), c.uav(surface), c.uav(depth), 0 };
                  c.cmd->SetPipelineState(resolve);
                  c.computeConstants(k, 8);
                  c.cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
              });
    return out;
}
} // namespace unx::water
