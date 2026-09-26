// Water view grid (track W, B7). See include/unx/water/ViewGrid.h and ViewGrid.hlsli.
#include "unx/water/LinearDispatch.h"
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
constexpr uint32_t kNearBlocks = 262144;   // near-field blocks per level list (past it the parent level draws)
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
    {
        // The ocean bounds pyramid (OceanBounds.hlsl): 3 cascades x 512^2, 10 mips, a UAV per mip.
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC1 d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = d.Height = 512;
        d.DepthOrArraySize = 3;
        d.MipLevels = 10;
        d.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_COMMON, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_pyramid)),
              "ocean bounds pyramid");
        m_pyramid->SetName(L"ocean bounds pyramid");
        // Diagnostics' drawn-block list (count, then 16 B per block, 2^20 blocks), with a fixed UAV for the parameters.
        D3D12_RESOURCE_DESC1 b{};
        b.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        b.Width = 16 + (uint64_t(1) << 20) * 16;
        b.Height = b.DepthOrArraySize = b.MipLevels = 1;
        b.SampleDesc.Count = 1;
        b.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        b.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &b, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_drawn)),
              "view grid drawn blocks");
        m_drawn->SetName(L"view grid drawn blocks");
        m_drawnUav = device.descriptors().allocateResource();
        D3D12_UNORDERED_ACCESS_VIEW_DESC du{};
        du.Format = DXGI_FORMAT_R32_TYPELESS;
        du.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        du.Buffer.NumElements = UINT(b.Width / 4);
        du.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        device.d3d()->CreateUnorderedAccessView(m_drawn.Get(), nullptr, &du, device.descriptors().resourceCpu(m_drawnUav));
        for (uint32_t m = 0; m < 10; ++m)
        {
            m_pyramidUav[m] = device.descriptors().allocateResource();
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
            ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            ud.Texture2DArray.MipSlice = m;
            ud.Texture2DArray.ArraySize = 3;
            device.d3d()->CreateUnorderedAccessView(m_pyramid.Get(), nullptr, &ud, device.descriptors().resourceCpu(m_pyramidUav[m]));
        }
    }
    for (uint32_t s = 0; s <= framesInFlight; ++s)
    {
        D3D12_HEAP_PROPERTIES rheap{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC1 rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = 3 * 256;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> rb;
        check(device.d3d()->CreateCommittedResource3(&rheap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&rb)),
              "view grid bounds readback");
        m_boundsReadback.push_back(rb);
        m_boundsFrame.push_back(0);
    }
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
    m_device.deferRelease(m_pyramid);
    m_device.deferRelease(m_drawn);
    for (auto& rb : m_boundsReadback) m_device.deferRelease(rb);
    m_device.descriptors().freeResource(m_drawnUav);
    for (uint32_t uav : m_pyramidUav) m_device.descriptors().freeResource(uav);
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
    // Near field (FEATURES_GAME 1.8 B.2, adaptive): world lattices with spacing s_0 2^l; each 8 x 8-quad block takes the
    // coarsest level whose spacing fits s(t) = coefficient sqrt(t) at the block's nearest possible distance t to the
    // displaced surface (bounded on the GPU from the ocean bounds pyramid). coefficient = (lambda_min / 2 pi)
    // sqrt(8 e theta / (2 A_min)): the linear interpolation error within e = 0.4 px, the octaves' sum twice the finest
    // band's (lambda_min = two finest-cascade texels, 3 sigma height A_min = 0.048 lambda_min in the saturation range;
    // measured on WARP: the finest band alone gave 0.67 px at the 99.9th percentile against a 0.4 px target).
    // s_0 = s(t_floor) with t_floor the near plane (at least 2 cm); the top level fits the farthest possible surface.
    // Level origins are aligned so a block's children are whole blocks one level down; ids hold 8191 quads per axis.
    const float window[4] = { out.window[0], out.window[1], out.window[2], out.window[3] };
    std::memcpy(p + 100, window, 16);
    if (h > 0)
    {
        const double lambdaMin = 2.0 * lengths[2] / 512, aMin = 0.048 * lambdaMin;
        const double coefficient = lambdaMin / (2 * kPi) * std::sqrt(8 * 0.4 * theta / (2 * aMin));
        const double tFloor = std::max(double(c.nearPlane), 0.02), s0 = coefficient * std::sqrt(tFloor);
        const double tFar = std::hypot(double(w.nearRadius) + w.horizontalBound, h + w.verticalBound);
        const uint32_t levels = std::min<uint32_t>(kNearLevels, uint32_t(std::max(1.0, std::ceil(std::log2(coefficient * std::sqrt(tFar) / s0)) + 1)));
        const float header[3] = { 0, float(coefficient), float(tFloor) };
        std::memcpy(p + 32, &levels, 4);
        std::memcpy(p + 33, &header[1], 8);
        int64_t origin[kNearLevels][2] = {};
        for (int32_t l = int32_t(levels) - 1; l >= 0; --l)
        {
            const double spacing = s0 * double(1u << l);
            for (int a = 0; a < 2; ++a)
            {
                const double camera = a ? c.position[2] : c.position[0];
                if (l == int32_t(levels) - 1) origin[l][a] = int64_t(std::floor((camera / spacing - 4096) / 8)) * 8;
                else origin[l][a] = 2 * origin[l + 1][a] + 8 * int64_t(std::llround((camera / spacing - 4096 - 2.0 * origin[l + 1][a]) / 8));
            }
            if (std::abs(origin[l][0]) > (1ll << 30) || std::abs(origin[l][1]) > (1ll << 30)) fail("view grid: near level %d origin out of range", l);
            const float row[3] = { 0, w.nearRadius, float(spacing) };
            const uint32_t points = 8192;
            const int32_t o[2] = { int32_t(origin[l][0]), int32_t(origin[l][1]) };
            std::memcpy(p + 36 + 8 * l, row, 12);
            std::memcpy(p + 36 + 8 * l + 3, &points, 4);
            std::memcpy(p + 36 + 8 * l + 4, o, 8);
        }
        // The top level's blocks covering the near disk (with a cell of overlap into the far field).
        const double top = s0 * double(1u << (levels - 1));
        const double reach = w.nearRadius + 2 * top;
        int32_t first[2];
        for (int a = 0; a < 2; ++a)
        {
            const double camera = a ? c.position[2] : c.position[0];
            first[a] = int32_t(std::floor(((camera - reach) / top - double(origin[levels - 1][a])) / 8));
        }
        const uint32_t width = uint32_t(std::ceil(2 * reach / (8 * top))) + 2;
        if (first[0] < 1 || first[1] < 1 || (first[0] + int32_t(width)) * 8 + 9 >= 8191 || (first[1] + int32_t(width)) * 8 + 9 >= 8191)
            fail("view grid: the near disk does not fit the top level's id range");
        out.nearLevels = levels;
        out.nearFirst[0] = first[0];
        out.nearFirst[1] = first[1];
        out.nearWidth = width;
    }
    return out;
}

ViewGridOutput ViewGrid::record(RenderGraph& g, uint64_t frame, const OceanOutput& fields, const float lengths[3], const ViewGridCamera& camera, const ViewGridWater& water,
                                bool diagnostics)
{
    // The ocean's measured bounds: the slot written framesInFlight + 1 records ago (its GPU work has completed: the caller
    // waited for frame - framesInFlight before recording this one).
    const uint32_t ring = uint32_t(m_boundsReadback.size()), readSlot = uint32_t((frame + 1) % ring), writeSlot = uint32_t(frame % ring);
    if (!fields.previousValid) m_seaChanged = frame + 1;
    if (m_boundsFrame[readSlot] && m_boundsFrame[readSlot] + ring - 1 == frame + 1 && m_boundsFrame[readSlot] >= m_seaChanged)
    {
        const uint8_t* m = nullptr;
        D3D12_RANGE all{ 0, 3 * 256 };
        check(m_boundsReadback[readSlot]->Map(0, &all, (void**)&m), "map view grid bounds");
        float r = 0, top = 0, bottom = 0;
        for (uint32_t c = 0; c < 3; ++c)
        {
            float b[4];
            std::memcpy(b, m + 256 * c, 16);
            top += b[0];
            bottom += b[1];
            r += b[2];
        }
        D3D12_RANGE none{ 0, 0 };
        m_boundsReadback[readSlot]->Unmap(0, &none);
        m_measuredBounds[0] = r;
        m_measuredBounds[1] = std::max(top, -bottom);
        m_measured = true;
        m_measuredFrame = m_boundsFrame[readSlot];
    }
    ViewGridWater effective = water;
    const bool current = m_measured && m_measuredFrame >= m_seaChanged;  // measured after the latest sea state
    if (current)
    {
        effective.horizontalBound = 1.25f * m_measuredBounds[0];
        effective.verticalBound = 1.25f * m_measuredBounds[1];
    }
    else if (m_measured)
    {
        effective.horizontalBound = std::max(water.horizontalBound, 1.25f * m_measuredBounds[0]);
        effective.verticalBound = std::max(water.verticalBound, 1.25f * m_measuredBounds[1]);
    }
    ViewGridLayout l = layout(camera, effective, lengths);
    const uint32_t drawnUav = diagnostics ? m_drawnUav : 0;
    std::memcpy(&l.params[104], &drawnUav, 4);  // byte 416
    const uint32_t slot = uint32_t(frame % m_upload.size());
    std::memcpy(m_mapped[slot], l.params.data(), l.params.size() * 4);
    const uint32_t paramSrv = m_srv[slot], width = camera.width, height = camera.height, pixels = width * height;
    ViewGridOutput out;
    out.columns = l.columns;
    out.rows = l.rows;
    out.layout = l;
    out.water = effective;
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
                  unx::water::dispatchLinear(c.cmd, (pixels + 63) / 64);
              });
    // The ocean bounds pyramid (the adaptive near field's distances; its top mip is the measured R and A).
    const TextureRef pyramid = g.importTexture(m_pyramid.Get(), TextureDesc{ "ocean bounds pyramid", 512, 512, 3, 10, DXGI_FORMAT_R32G32B32A32_FLOAT }, D3D12_BARRIER_LAYOUT_COMMON);
    ID3D12PipelineState* bounds = m_shaders.compute("Passes/Water/OceanBounds");
    const float texels[3] = { lengths[0] / 512, lengths[1] / 512, lengths[2] / 512 };
    for (uint32_t mip = 0; mip < 10; ++mip)
    {
        const uint32_t dst = m_pyramidUav[mip], src = mip ? m_pyramidUav[mip - 1] : 0, size = 512u >> mip;
        g.addPass("ocean bounds pyramid", QueueType::Graphics,
                  [&](PassBuilder& pb) { pb.use(displacement, Use::SrvCompute); pb.use(slopes, Use::SrvCompute); pb.use(pyramid, Use::UavCompute); },
                  [=](PassContext& c) {
                      uint32_t k[12] = { c.srv(displacement), c.srv(slopes), dst, src, mip };
                      std::memcpy(&k[8], texels, 12);
                      c.cmd->SetPipelineState(bounds);
                      c.computeConstants(k, 12);
                      c.cmd->Dispatch((size + 7) / 8, (size + 7) / 8, 3);
                  });
    }
    ID3D12Resource* readback = m_boundsReadback[writeSlot].Get();
    g.addPass("ocean bounds readback", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(pyramid, Use::CopySrc); pb.keep(); },
              [=](PassContext& c) {
                  for (uint32_t cascade = 0; cascade < 3; ++cascade)
                  {
                      D3D12_TEXTURE_COPY_LOCATION dst{ readback, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                      dst.PlacedFootprint.Offset = 256 * cascade;
                      dst.PlacedFootprint.Footprint = { DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 1, 1, 256 };
                      D3D12_TEXTURE_COPY_LOCATION src{ c.resource(pyramid), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                      src.SubresourceIndex = 9 + 10 * cascade;
                      c.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                  }
              });
    m_boundsFrame[writeSlot] = frame + 1;
    const uint32_t groupsX = (l.columns + 7) / 8, groupsY = (l.rows + 7) / 8;
    const float bound = effective.bound();
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
        // Adaptive near field: level L - 1 over the rectangle and each finer level over the blocks its parent level split
        // (indirect), ping-ponging two lists.
        const BufferRef lists[2] = { g.createBuffer({ "view grid near blocks A", 16 + uint64_t(kNearBlocks) * 8, 0 }),
                                     g.createBuffer({ "view grid near blocks B", 16 + uint64_t(kNearBlocks) * 8, 0 }) };
        const BufferRef levelArgs = g.createBuffer({ "view grid near dispatch", 256, 0 });
        const BufferRef firstList = lists[0];
        const BufferRef drawn = g.importBuffer(m_drawn.Get(), { "view grid drawn blocks", m_drawn->GetDesc().Width, 0 });
        out.nearDrawn = diagnostics ? drawn : BufferRef{};
        const uint32_t drawnUavFixed = m_drawnUav;
        g.addPass("view grid near list clear", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(firstList, Use::UavCompute); pb.use(drawn, Use::UavCompute); },
                  [=](PassContext& c) {
                      c.cmd->SetPipelineState(clear);  // ViewGridClear: the counts only
                      const uint32_t k[4] = { 0, 0, c.uav(firstList), 4 };
                      c.computeConstants(k, 4);
                      c.cmd->Dispatch(1, 1, 1);
                      const uint32_t d[4] = { 0, 0, drawnUavFixed, 4 };
                      c.computeConstants(d, 4);
                      c.cmd->Dispatch(1, 1, 1);
                  });
        ID3D12PipelineState* adaptive = m_shaders.compute("Passes/Water/ViewGridAdaptive");
        ID3D12PipelineState* args = m_shaders.compute("Passes/Water/ViewGridAdaptiveArgs");
        ID3D12CommandSignature* signature = m_dispatch.Get();
        const int32_t rect[2] = { l.nearFirst[0], l.nearFirst[1] };
        const uint32_t rectWidth = l.nearWidth;
        for (int32_t level = int32_t(l.nearLevels) - 1; level >= 0; --level)
        {
            const bool top = level == int32_t(l.nearLevels) - 1;
            const uint32_t step = uint32_t(int32_t(l.nearLevels) - 1 - level);  // passes since the top
            const BufferRef in = lists[(step + 1) & 1], outList = lists[step & 1];
            if (!top)
                g.addPass("view grid near args", QueueType::Graphics, [&](PassBuilder& pb) { pb.use(in, Use::UavCompute); pb.use(levelArgs, Use::UavCompute); pb.use(outList, Use::UavCompute); },
                          [=](PassContext& c) {
                              const uint32_t k[4] = { c.uav(in), c.uav(levelArgs), kNearBlocks, c.uav(outList) };
                              c.cmd->SetPipelineState(args);
                              c.computeConstants(k, 4);
                              c.cmd->Dispatch(1, 1, 1);
                          });
            g.addPass("view grid near level", QueueType::Graphics,
                      [&](PassBuilder& pb) {
                          scatterUses(pb);
                          pb.use(pyramid, Use::SrvCompute);
                          pb.use(drawn, Use::UavCompute);
                          pb.use(outList, Use::UavCompute);
                          if (!top) { pb.use(in, Use::SrvCompute); pb.use(levelArgs, Use::IndirectArgs); }
                      },
                      [=](PassContext& c) {
                          uint32_t k[16] = { paramSrv, c.srv(displacement), c.uav(keys), c.uav(counters), c.uav(big), kBigCapacity, c.srv(slopes), c.srv(pyramid),
                                             uint32_t(level), top ? 0u : c.srv(in), c.uav(outList), kNearBlocks };
                          std::memcpy(&k[12], rect, 8);
                          k[14] = rectWidth;
                          k[15] = top ? 1u : 0u;
                          c.cmd->SetPipelineState(adaptive);
                          c.computeConstants(k, 16);
                          if (top) c.cmd->Dispatch(rectWidth, rectWidth, 1);
                          else c.cmd->ExecuteIndirect(signature, 1, c.resource(levelArgs), 0, nullptr, 0);
                      });
        }
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
