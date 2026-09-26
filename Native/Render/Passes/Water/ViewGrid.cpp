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
constexpr uint64_t kParamBytes = 512 + uint64_t(ViewGrid::kMaxRows) * 8;
constexpr uint32_t kNearLevels = 8;        // ViewGrid.hlsli VG_NEAR_LEVELS
constexpr uint32_t kBigCapacity = 65536;   // big-triangle list (48 B records; past it the triangles are counted as lost)

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
    D3D12_INDIRECT_ARGUMENT_DESC argument{};
    argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
    D3D12_COMMAND_SIGNATURE_DESC signature{};
    signature.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
    signature.NumArgumentDescs = 1;
    signature.pArgumentDescs = &argument;
    check(device.d3d()->CreateCommandSignature(&signature, nullptr, IID_PPV_ARGS(&m_dispatch)), "view grid dispatch signature");
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
    if (!c.width || !c.height || !(c.tanX > 0) || !(c.tanY > 0) || !(c.nearPlane > 0) || !(w.nearRadius > 0) || !(w.horizontalBound >= 0) || !(w.verticalBound >= 0) || !(w.extent > w.nearRadius))
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
    const double h = double(c.position[1]) - w.level, margin = w.bound() / w.nearRadius;
    double phi0 = phiLo - margin, phi1 = phiHi + margin;
    double e0 = eLo - margin, e1 = std::min(eHi + margin, 0.0);  // rows at the horizon end at the far ring
    if (h > 0) e0 = std::max(e0, -std::atan(h / w.nearRadius));
    if (w.lake && h > 0)
    {
        // The lake's angular box (its boundary widened by the bound, and its inside) narrows the window.
        double lp0 = 1e9, lp1 = -1e9, le0 = 1e9, le1 = -1e9;
        for (int k = 0; k < 720; ++k)
            for (double r : { double(w.lakeRadius) + w.bound(), 0.5 * w.lakeRadius, 0.0 })
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
            const double flat = theta * (h * h + r * r) / h, crest = r <= 2 * w.verticalBound / theta ? 2.5 * r * theta : 1e30;
            const double step = std::min(flat, crest);
            table.push_back(float(r));
            table.push_back(float(step));
            if (r >= rEnd) break;
            r = std::min(r + step, rEnd);
        }
    }
    out.rows = uint32_t(table.size() / 2);
    if (out.rows > kMaxRows) fail("view grid: %u rows exceed the table's %u", out.rows, kMaxRows);
    out.params.assign(128, 0.0f);  // header (512 B), near levels filled below
    out.params.insert(out.params.end(), table.begin(), table.end());
    float* p = out.params.data();
    const float row0[4] = { c.position[0], c.position[1], c.position[2], w.level };
    const float row1[4] = { c.right[0], c.right[1], c.right[2], c.tanX };
    const float row2[4] = { c.up[0], c.up[1], c.up[2], c.tanY };
    const float row3[4] = { c.forward[0], c.forward[1], c.forward[2], w.extent };
    const float row4[4] = { float(phi0), 0, float(theta), w.nearRadius };
    const uint32_t row5[4] = { out.columns, out.rows, c.width, c.height };
    const float row6[4] = { lengths[0], lengths[1], lengths[2], c.nearPlane };
    const float lake[3] = { w.lakeCentre[0], w.lakeCentre[1], w.lakeRadius };
    const uint32_t lakeOn = w.lake ? 1 : 0;
    std::memcpy(p, row0, 16); std::memcpy(p + 4, row1, 16); std::memcpy(p + 8, row2, 16); std::memcpy(p + 12, row3, 16);
    std::memcpy(p + 16, row4, 16); std::memcpy(p + 20, row5, 16); std::memcpy(p + 24, row6, 16); std::memcpy(p + 28, lake, 12); std::memcpy(p + 31, &lakeOn, 4);
    if (uint64_t(out.columns) * out.rows * 2 >= (1ull << 31)) fail("view grid: %u x %u far-field quads exceed the triangle ids", out.columns, out.rows);
    // Near field (FEATURES_GAME 1.8 B.2): rings of rest distance r_n, r_n / 4, ... down to the camera's height, each a
    // world lattice whose spacing (lambda_min / 2 pi) sqrt(8 e t theta / A_min) at the ring's nearest possible distance
    // to the displaced surface, t = |(max(inner - R, 0), max(h - A, 0))| (at least 5 cm), keeps
    // the linear interpolation error within e = 0.4 px (0.1 px under the definition's 0.5 for the rest of the chain):
    // band k contributes (k s)^2 / 8 A_k with A_k = 0.048 lambda (3 sigma in the saturation range), so the octaves sum to
    // twice the finest band's (lambda_min = two finest-cascade texels) - measured on WARP: the finest band alone gave
    // 0.67 px at the 99.9th percentile against a 0.4 px target.
    if (h > 0)
    {
        const double lambdaMin = 2.0 * lengths[2] / 512, aMin = 0.048 * lambdaMin;
        const double coefficient = lambdaMin / (2 * kPi) * std::sqrt(8 * 0.4 * theta / (2 * aMin));
        std::vector<double> outer;
        for (double r = w.nearRadius;; r /= 4)
        {
            outer.push_back(r);
            if (r <= std::max(h, 0.25) || outer.size() == kNearLevels) break;
        }
        std::reverse(outer.begin(), outer.end());
        const uint32_t levels = uint32_t(outer.size());
        std::memcpy(p + 32, &levels, 4);
        for (uint32_t i = 0; i < levels; ++i)
        {
            const double inner = i ? outer[i - 1] : 0.0;
            const double spacing = coefficient * std::sqrt(std::max(std::hypot(std::max(inner - w.horizontalBound, 0.0), std::max(h - w.verticalBound, 0.0)), 0.05));
            const uint32_t n = 2 * uint32_t(std::ceil((outer[i] + 2 * spacing) / spacing)) + 2;
            if (n - 1 > 11585) fail("view grid: near level %u has %u points per side (the triangle ids hold 11585 quads)", i, n);
            const int32_t origin[2] = { int32_t(std::floor(c.position[0] / spacing)) - int32_t(n / 2), int32_t(std::floor(c.position[2] / spacing)) - int32_t(n / 2) };
            const float row[3] = { float(inner), float(outer[i]), float(spacing) };
            std::memcpy(p + 36 + 8 * i, row, 12);
            std::memcpy(p + 36 + 8 * i + 3, &n, 4);
            std::memcpy(p + 36 + 8 * i + 4, origin, 8);
            out.nearPoints = std::max(out.nearPoints, n);
        }
        out.nearLevels = levels;
    }
    return out;
}

ViewGridOutput ViewGrid::record(RenderGraph& g, uint64_t frame, const OceanOutput& fields, const float lengths[3], const ViewGridCamera& camera, const ViewGridWater& water,
                                bool diagnostics)
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
    const float bound = water.bound();
    const float window[4] = { l.window[0], l.window[1], l.window[2], l.window[3] };
    const BufferRef big = g.createBuffer({ "view grid big triangles", uint64_t(kBigCapacity) * 48, 0 });
    const BufferRef prefix = g.createBuffer({ "view grid big prefix", uint64_t(kBigCapacity) * 4, 0 });
    const BufferRef arguments = g.createBuffer({ "view grid big dispatch", 256, 0 });
    auto scatterConstants = [=](PassContext& c, uint32_t (&k)[12]) {
        k[0] = paramSrv; k[1] = c.srv(displacement); k[2] = c.uav(keys); k[3] = c.uav(counters);
        std::memcpy(&k[4], &bound, 4);
        std::memcpy(&k[5], window, 16);
        k[9] = c.uav(big);
        k[10] = kBigCapacity;
        k[11] = c.srv(slopes);
    };
    auto scatterUses = [&](PassBuilder& pb) {
        pb.use(displacement, Use::SrvCompute); pb.use(slopes, Use::SrvCompute); pb.use(keys, Use::UavCompute); pb.use(counters, Use::UavCompute); pb.use(big, Use::UavCompute);
    };
    ID3D12PipelineState* scatter = m_shaders.compute("Passes/Water/ViewGridScatter");
    if (groupsY)
        g.addPass("view grid scatter", QueueType::Graphics, scatterUses, [=](PassContext& c) {
            uint32_t k[12];
            scatterConstants(c, k);
            c.cmd->SetPipelineState(scatter);
            c.computeConstants(k, 12);
            c.cmd->Dispatch(groupsX, groupsY, 1);
        });
    if (l.nearLevels)
    {
        ID3D12PipelineState* nearScatter = m_shaders.compute("Passes/Water/ViewGridNearScatter");
        const uint32_t nearGroups = (l.nearPoints - 1 + 7) / 8, levels = l.nearLevels;
        g.addPass("view grid near scatter", QueueType::Graphics, scatterUses, [=](PassContext& c) {
            uint32_t k[12];
            scatterConstants(c, k);
            c.cmd->SetPipelineState(nearScatter);
            c.computeConstants(k, 12);
            c.cmd->Dispatch(nearGroups, nearGroups, levels);
        });
    }
    // Big triangles: prefix of their 8 x 8 tiles, then one thread per tile (indirect).
    ID3D12PipelineState* bigScan = m_shaders.compute("Passes/Water/ViewGridBigScan");
    g.addPass("view grid big scan", QueueType::Graphics,
              [&](PassBuilder& pb) { pb.use(counters, Use::UavCompute); pb.use(big, Use::UavCompute); pb.use(prefix, Use::UavCompute); pb.use(arguments, Use::UavCompute); },
              [=](PassContext& c) {
                  const uint32_t k[8] = { c.uav(counters), c.uav(big), c.uav(prefix), kBigCapacity, width, height, c.uav(arguments), 0 };
                  c.cmd->SetPipelineState(bigScan);
                  c.computeConstants(k, 8);
                  c.cmd->Dispatch(1, 1, 1);
              });
    ID3D12PipelineState* bigRaster = m_shaders.compute("Passes/Water/ViewGridBigRaster");
    ID3D12CommandSignature* signature = m_dispatch.Get();
    g.addPass("view grid big raster", QueueType::Graphics,
              [&](PassBuilder& pb) {
                  pb.use(counters, Use::SrvCompute); pb.use(big, Use::SrvCompute); pb.use(prefix, Use::SrvCompute); pb.use(arguments, Use::IndirectArgs);
                  pb.use(keys, Use::UavCompute);
              },
              [=](PassContext& c) {
                  const uint32_t k[8] = { c.srv(counters), c.srv(big), c.srv(prefix), kBigCapacity, width, height, c.uav(keys), 0 };
                  c.cmd->SetPipelineState(bigRaster);
                  c.computeConstants(k, 8);
                  c.cmd->ExecuteIndirect(signature, 1, c.resource(arguments), 0, nullptr, 0);
              });
    ID3D12PipelineState* resolve = m_shaders.compute("Passes/Water/ViewGridResolve");
    if (diagnostics) out.error = g.createTexture(TextureDesc{ "view grid error", width, height, 1, 1, DXGI_FORMAT_R32_FLOAT });
    const TextureRef error = out.error;
    g.addPass("view grid resolve", QueueType::Graphics,
              [&](PassBuilder& pb) {
                  pb.use(displacement, Use::SrvCompute); pb.use(slopes, Use::SrvCompute); pb.use(keys, Use::SrvCompute);
                  pb.use(surface, Use::UavCompute); pb.use(depth, Use::UavCompute);
                  if (diagnostics) pb.use(error, Use::UavCompute);
              },
              [=](PassContext& c) {
                  const uint32_t k[8] = { paramSrv, 0, c.srv(keys), c.srv(displacement), c.srv(slopes), c.uav(surface), c.uav(depth), diagnostics ? c.uav(error) : 0 };
                  c.cmd->SetPipelineState(resolve);
                  c.computeConstants(k, 8);
                  c.cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
              });
    return out;
}
} // namespace unx::water
