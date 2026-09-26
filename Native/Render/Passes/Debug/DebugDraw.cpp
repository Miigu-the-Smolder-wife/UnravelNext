// Debug drawing, buffer visualization and HUD (track E, A15). See include/unx/debug/DebugDraw.h and DebugDraw.hlsli.
#include "unx/debug/DebugDraw.h"

#include "unx/core/Log.h"
#include "unx/render/GpuProfiler.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"
#include "unx/render/Tracks.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>

namespace unx::debug
{
using namespace unx::render;

namespace
{
constexpr uint32_t kOff = 0xFFFFFFFFu;
constexpr uint32_t kHeaderBytes = 256, kPerGroup = 32;
constexpr uint32_t kArgsLines = 32, kArgsTriangles = 44, kArgsGlyphs = 56;
// Font atlas: printable ASCII 32..126 in 16 x 6 cells of 32 x 64 texels (1:2, DEBUG_GLYPH_ASPECT), the glyph inside a
// 4-texel empty border (1/8 of the cell height: mips 0..3 do not bleed across cells).
constexpr uint32_t kAtlasColumns = 16, kAtlasRows = 6, kCellW = 32, kCellH = 64, kAtlasMips = 4;
constexpr uint32_t kAtlasW = kAtlasColumns * kCellW, kAtlasH = kAtlasRows * kCellH;

const std::vector<std::string> kViews = { "visId", "instance", "depth", "hiz", "normal", "albedo", "roughness", "shadowSun", "reflection",
                                          "particleLayer", "distortion", "volumeTau", "volumeSource", "airTransmittance", "coverage" };

uint32_t widthFlags(float widthPx, uint32_t flags)
{
    const uint32_t w = (uint32_t)std::min(65535.0f, std::round(std::max(widthPx, 0.0f) * 256.0f));
    return w | (flags << 16);
}

ComPtr<ID3D12Resource> makeBuffer(Device& device, uint64_t bytes, D3D12_HEAP_TYPE type, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES heap{ type };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (type == D3D12_HEAP_TYPE_DEFAULT) d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)),
          "debug buffer");
    r->SetName(name);
    return r;
}

// Printable ASCII rasterized by GDI from the system's monospace font (Consolas; nothing is redistributed), grayscale
// antialiasing, centred in each cell; then a box-filtered mip chain. Returns the texels of all mips, mip 0 first.
std::vector<std::vector<uint8_t>> rasterizeFont()
{
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = (LONG)kAtlasW;
    bi.bmiHeader.biHeight = -(LONG)kAtlasH;  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC dc = CreateCompatibleDC(nullptr);
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!dc || !bitmap || !bits) fail("debug font: GDI bitmap");
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    std::memset(bits, 0, (size_t)kAtlasW * kAtlasH * 4);
    // Em height 40 px: Consolas advances 0.55 em (22 px) and spans ~1.17 em (47 px), inside the 24 x 56 interior.
    HFONT font = CreateFontW(-40, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_ONLY_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY,
                             FIXED_PITCH | FF_MODERN, L"Consolas");
    if (!font) fail("debug font: CreateFont");
    HGDIOBJ oldFont = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    for (uint32_t code = 32; code <= 126; ++code)
    {
        const uint32_t i = code - 32, cx = (i % kAtlasColumns) * kCellW, cy = (i / kAtlasColumns) * kCellH;
        const wchar_t ch = (wchar_t)code;
        SIZE size{};
        GetTextExtentPoint32W(dc, &ch, 1, &size);
        const int x = (int)cx + ((int)kCellW - size.cx) / 2, y = (int)cy + ((int)kCellH - tm.tmHeight) / 2;
        RECT clip{ (LONG)cx + 4, (LONG)cy + 4, (LONG)(cx + kCellW - 4), (LONG)(cy + kCellH - 4) };
        ExtTextOutW(dc, x, y, ETO_CLIPPED, &clip, &ch, 1, nullptr);
    }
    GdiFlush();
    std::vector<std::vector<uint8_t>> mips(kAtlasMips);
    mips[0].resize((size_t)kAtlasW * kAtlasH);
    const uint8_t* src = (const uint8_t*)bits;
    for (size_t p = 0; p < mips[0].size(); ++p) mips[0][p] = src[p * 4 + 1];  // green: white text, grayscale coverage
    for (uint32_t m = 1; m < kAtlasMips; ++m)
    {
        const uint32_t w = kAtlasW >> m, h = kAtlasH >> m, pw = kAtlasW >> (m - 1);
        mips[m].resize((size_t)w * h);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                const std::vector<uint8_t>& p = mips[m - 1];
                const uint32_t s = p[(2 * y) * pw + 2 * x] + p[(2 * y) * pw + 2 * x + 1] + p[(2 * y + 1) * pw + 2 * x] + p[(2 * y + 1) * pw + 2 * x + 1];
                mips[m][(size_t)y * w + x] = (uint8_t)((s + 2) / 4);
            }
    }
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBitmap);
    DeleteObject(font);
    DeleteObject(bitmap);
    DeleteDC(dc);
    return mips;
}

struct Capacities
{
    uint32_t lines = 0, triangles = 0, glyphs = 0;
    uint64_t trianglesOffset() const { return kHeaderBytes + (uint64_t)lines * sizeof(Line); }
    uint64_t glyphsOffset() const { return trianglesOffset() + (uint64_t)triangles * sizeof(Triangle); }
    uint64_t bytes() const { return glyphsOffset() + (uint64_t)glyphs * sizeof(Glyph); }
};

struct Settings
{
    bool draw = false, hud = false;
    std::string view;
    uint32_t viewIndex = 0;
    int32_t probe[2] = { -1, -1 };
    Capacities caps;
};
Settings readSettings(const QualityConfig& q)
{
    Settings s;
    s.draw = q.boolean("debug.draw");
    s.hud = q.boolean("debug.hud");
    s.view = q.string("debug.view");
    const int64_t index = q.integer("debug.view_index");
    const std::vector<double> probe = q.numbers("debug.view_probe");
    const int64_t lines = q.integer("debug.max_lines"), triangles = q.integer("debug.max_triangles"), glyphs = q.integer("debug.max_glyphs");
    if (index < 0 || probe.size() != 2) fail("debug.view_index >= 0 and debug.view_probe = [x, y] ([-1, -1] = none)");
    // 32 primitives per mesh group, at most 65535 groups in one dimension (DebugArgs.hlsl writes y = 1).
    const int64_t maxCap = 65535ll * kPerGroup;
    if (lines < 1 || lines > maxCap || triangles < 1 || triangles > maxCap || glyphs < 1 || glyphs > maxCap)
        fail("debug.max_lines / max_triangles / max_glyphs must be in [1, %lld]", (long long)maxCap);
    s.viewIndex = (uint32_t)index;
    s.probe[0] = (int32_t)probe[0];
    s.probe[1] = (int32_t)probe[1];
    s.caps = { (uint32_t)lines, (uint32_t)triangles, (uint32_t)glyphs };
    return s;
}

class DebugPass
{
public:
    explicit DebugPass(Device& device) : m_device(device)
    {
        D3D12_INDIRECT_ARGUMENT_DESC arg{};
        arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH_MESH;
        D3D12_COMMAND_SIGNATURE_DESC d{ sizeof(D3D12_DISPATCH_MESH_ARGUMENTS), 1, &arg, 0 };
        check(device.d3d()->CreateCommandSignature(&d, nullptr, IID_PPV_ARGS(&m_meshSignature)), "debug mesh signature");
    }
    ~DebugPass()
    {
        m_device.waitIdle();
        for (Slot& s : m_slots)
        {
            if (s.upload && s.uploadMapped) s.upload->Unmap(0, nullptr);
            if (s.readback && s.readbackMapped) s.readback->Unmap(0, nullptr);
        }
        if (m_uav != kOff) m_device.descriptors().freeResource(m_uav);
    }

    uint32_t begin(FramePassContext& fc, DrawList& list)
    {
        const Settings s = readSettings(fc.quality);
        m_frame = fc.frame.frameIndex;
        m_on = false;
        m_buffer = {};
        m_settings = s;
        ensureSlots(fc.framesInFlight);
        collectStats();
        if (!s.draw && !s.hud && s.view == "none" && list.empty()) return kOff;
        m_on = true;
        ensureBuffer(s.caps);
        ensureAtlas();

        DrawList hud;
        if (s.hud) drawHud(hud, fc.frame.timing, fc.frame.mainView);
        const DrawList* lists[2] = { &hud, &list };  // HUD first: the caller's primitives draw over it
        uint32_t counts[3] = {};
        for (const DrawList* l : lists)
        {
            counts[0] += (uint32_t)l->lines.size();
            counts[1] += (uint32_t)l->triangles.size();
            counts[2] += (uint32_t)l->glyphs.size();
        }
        const Capacities& c = m_caps;
        const uint32_t stored[3] = { std::min(counts[0], c.lines), std::min(counts[1], c.triangles), std::min(counts[2], c.glyphs) };
        uint32_t header[16] = {};
        header[0] = counts[0];
        header[1] = counts[1];
        header[2] = counts[2];
        header[3] = (counts[0] > c.lines ? LinesFull : 0u) | (counts[1] > c.triangles ? TrianglesFull : 0u) | (counts[2] > c.glyphs ? GlyphsFull : 0u);
        header[4] = c.lines;
        header[5] = c.triangles;
        header[6] = c.glyphs;
        header[7] = counts[0];
        const uint64_t bytes = kHeaderBytes + (uint64_t)stored[0] * sizeof(Line) + (uint64_t)stored[1] * sizeof(Triangle) + (uint64_t)stored[2] * sizeof(Glyph);
        Slot& slot = m_slots[m_frame % m_slots.size()];
        ensureUpload(slot, bytes);
        uint8_t* p = slot.uploadMapped;
        std::memset(p, 0, kHeaderBytes);
        std::memcpy(p, header, sizeof header);
        uint64_t at = kHeaderBytes;
        auto put = [&](auto member, uint32_t limit) {
            uint32_t written = 0;
            for (const DrawList* l : lists)
            {
                const auto& v = l->*member;
                const uint32_t n = (uint32_t)std::min<size_t>(v.size(), limit - written);
                if (n) std::memcpy(p + at + (uint64_t)written * sizeof(v[0]), v.data(), (size_t)n * sizeof(v[0]));
                written += n;
            }
            at += (uint64_t)written * sizeof((lists[0]->*member)[0]);
        };
        put(&DrawList::lines, stored[0]);
        put(&DrawList::triangles, stored[1]);
        put(&DrawList::glyphs, stored[2]);
        list.clear();

        m_buffer = fc.graph.importBuffer(m_resource.Get(), BufferDesc{ "debug.primitives", c.bytes(), 0 });
        const BufferRef buf = m_buffer;
        ID3D12Resource* upload = slot.upload.Get();
        const uint64_t lineBytes = (uint64_t)stored[0] * sizeof(Line), triangleBytes = (uint64_t)stored[1] * sizeof(Triangle),
                       glyphBytes = (uint64_t)stored[2] * sizeof(Glyph);
        const uint64_t trianglesAt = c.trianglesOffset(), glyphsAt = c.glyphsOffset();
        fc.graph.addPass("debug.begin", QueueType::Graphics, [=](PassBuilder& b) { b.use(buf, Use::CopyDst); },
                         [=](PassContext& ctx) {
                             ID3D12Resource* dst = ctx.resource(buf);
                             ctx.cmd->CopyBufferRegion(dst, 0, upload, 0, kHeaderBytes);
                             if (lineBytes) ctx.cmd->CopyBufferRegion(dst, kHeaderBytes, upload, kHeaderBytes, lineBytes);
                             if (triangleBytes) ctx.cmd->CopyBufferRegion(dst, trianglesAt, upload, kHeaderBytes + lineBytes, triangleBytes);
                             if (glyphBytes) ctx.cmd->CopyBufferRegion(dst, glyphsAt, upload, kHeaderBytes + lineBytes + triangleBytes, glyphBytes);
                         });
        if (m_atlasPending) uploadAtlas(fc.graph);
        return m_uav;
    }

    void overlay(FramePassContext& fc, ViewResources& view, const std::map<std::string, ViewFn>& registered)
    {
        if (!m_on || fc.frame.frameIndex != m_frame || !view.color.valid()) return;
        const DXGI_FORMAT format = fc.graph.desc(view.color).format;
        const bool linear = format != DXGI_FORMAT_R10G10B10A2_UNORM && format != DXGI_FORMAT_R10G10B10A2_TYPELESS;
        if (m_settings.view != "none") visualize(fc, view, linear, registered);
        draw(fc, view, format, linear);
    }

    BufferRef buffer(uint64_t frame) const { return m_on && frame == m_frame ? m_buffer : BufferRef{}; }
    Stats stats() const { return m_stats; }

private:
    struct Slot
    {
        ComPtr<ID3D12Resource> upload, readback;
        uint8_t* uploadMapped = nullptr;
        uint8_t* readbackMapped = nullptr;
        uint64_t uploadBytes = 0;
        uint64_t readbackFrame = UINT64_MAX;
    };

    void ensureSlots(uint32_t framesInFlight)
    {
        if (m_slots.size() == std::max(framesInFlight, 1u)) return;
        m_device.waitIdle();
        for (Slot& s : m_slots)
        {
            if (s.upload && s.uploadMapped) s.upload->Unmap(0, nullptr);
            if (s.readback && s.readbackMapped) s.readback->Unmap(0, nullptr);
        }
        m_slots.clear();
        m_slots.resize(std::max(framesInFlight, 1u));
        for (Slot& s : m_slots)
        {
            s.readback = makeBuffer(m_device, 16, D3D12_HEAP_TYPE_READBACK, L"debug stats readback");
            D3D12_RANGE all{ 0, 16 };
            check(s.readback->Map(0, &all, reinterpret_cast<void**>(&s.readbackMapped)), "map debug stats");
        }
    }
    // The slot of this frame was last written framesInFlight frames ago, and the caller waited for its fences.
    void collectStats()
    {
        Slot& s = m_slots[m_frame % m_slots.size()];
        if (s.readbackFrame == UINT64_MAX) return;
        uint32_t h[4];
        std::memcpy(h, s.readbackMapped, sizeof h);
        if (m_stats.frame == UINT64_MAX || s.readbackFrame > m_stats.frame) m_stats = { s.readbackFrame, h[0], h[1], h[2], h[3] };
        s.readbackFrame = UINT64_MAX;
    }
    void ensureUpload(Slot& s, uint64_t bytes)
    {
        if (s.upload && s.uploadBytes >= bytes) return;
        if (s.upload)
        {
            s.upload->Unmap(0, nullptr);
            m_device.deferRelease(s.upload);  // a frame in flight may still copy from it
        }
        s.uploadBytes = std::max<uint64_t>(bytes, 64 * 1024);
        s.upload = makeBuffer(m_device, s.uploadBytes, D3D12_HEAP_TYPE_UPLOAD, L"debug primitives upload");
        D3D12_RANGE none{ 0, 0 };
        check(s.upload->Map(0, &none, reinterpret_cast<void**>(&s.uploadMapped)), "map debug upload");
    }
    void ensureBuffer(const Capacities& caps)
    {
        if (m_resource && caps.lines == m_caps.lines && caps.triangles == m_caps.triangles && caps.glyphs == m_caps.glyphs) return;
        if (m_resource) m_device.deferRelease(m_resource);
        m_caps = caps;
        m_resource = makeBuffer(m_device, caps.bytes(), D3D12_HEAP_TYPE_DEFAULT, L"debug primitives");
        if (m_uav == kOff) m_uav = m_device.descriptors().allocateResource();
        D3D12_UNORDERED_ACCESS_VIEW_DESC u{};
        u.Format = DXGI_FORMAT_R32_TYPELESS;
        u.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        u.Buffer.NumElements = (UINT)(caps.bytes() / 4);
        u.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        m_device.d3d()->CreateUnorderedAccessView(m_resource.Get(), nullptr, &u, m_device.descriptors().resourceCpu(m_uav));
    }
    void ensureAtlas()
    {
        if (m_atlas) return;
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC1 d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = kAtlasW;
        d.Height = kAtlasH;
        d.DepthOrArraySize = 1;
        d.MipLevels = kAtlasMips;
        d.Format = DXGI_FORMAT_R8_UNORM;
        d.SampleDesc.Count = 1;
        check(m_device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_COMMON, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_atlas)),
              "debug font atlas");
        m_atlas->SetName(L"debug font atlas");
        const std::vector<std::vector<uint8_t>> mips = rasterizeFont();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[kAtlasMips];
        UINT rows[kAtlasMips];
        UINT64 rowBytes[kAtlasMips], total = 0;
        const D3D12_RESOURCE_DESC legacy = m_atlas->GetDesc();
        m_device.d3d()->GetCopyableFootprints(&legacy, 0, kAtlasMips, 0, fp, rows, rowBytes, &total);
        m_atlasUpload = makeBuffer(m_device, total, D3D12_HEAP_TYPE_UPLOAD, L"debug font atlas upload");
        uint8_t* p = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(m_atlasUpload->Map(0, &none, reinterpret_cast<void**>(&p)), "map debug atlas upload");
        for (uint32_t m = 0; m < kAtlasMips; ++m)
            for (UINT y = 0; y < rows[m]; ++y) std::memcpy(p + fp[m].Offset + (uint64_t)y * fp[m].Footprint.RowPitch, mips[m].data() + (size_t)y * (kAtlasW >> m), kAtlasW >> m);
        m_atlasUpload->Unmap(0, nullptr);
        std::memcpy(m_atlasFootprints, fp, sizeof fp);
        m_atlasPending = true;
    }
    TextureRef importAtlas(RenderGraph& g) const
    {
        return g.importTexture(m_atlas.Get(), TextureDesc{ "debug.font", kAtlasW, kAtlasH, 1, (uint16_t)kAtlasMips, DXGI_FORMAT_R8_UNORM }, D3D12_BARRIER_LAYOUT_COMMON);
    }
    void uploadAtlas(RenderGraph& g)
    {
        m_atlasRef = importAtlas(g);
        m_atlasRefFrame = m_frame;
        const TextureRef atlas = m_atlasRef;
        ID3D12Resource* upload = m_atlasUpload.Get();
        std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, kAtlasMips> fp;
        std::memcpy(fp.data(), m_atlasFootprints, sizeof m_atlasFootprints);
        g.addPass("debug.font.upload", QueueType::Graphics, [=](PassBuilder& b) { b.use(atlas, Use::CopyDst); },
                  [=](PassContext& c) {
                      for (uint32_t m = 0; m < kAtlasMips; ++m)
                      {
                          D3D12_TEXTURE_COPY_LOCATION dst{ c.resource(atlas), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
                          dst.SubresourceIndex = m;
                          D3D12_TEXTURE_COPY_LOCATION src{ upload, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
                          src.PlacedFootprint = fp[m];
                          c.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                      }
                  });
        m_atlasPending = false;
    }

    void drawHud(DrawList& hud, const FrameTiming* timing, const ViewDesc& view)
    {
        const std::vector<std::string> lines = hudLines(timing, m_stats);
        const float size = 16, lineHeight = 18, margin = 8;
        size_t longest = 0;
        for (const std::string& l : lines) longest = std::max(longest, l.size());
        const float w = longest * size * 0.5f + 2 * margin, h = lines.size() * lineHeight + 2 * margin;
        const float3 a{ margin, margin, 0 }, b{ margin + w, margin, 0 }, c{ margin + w, margin + h, 0 }, d{ margin, margin + h, 0 };
        const uint32_t back = rgba(uint8_t(0), uint8_t(0), uint8_t(0), uint8_t(160));
        hud.triangle(a, b, c, back, Screen);
        hud.triangle(a, c, d, back, Screen);
        for (size_t i = 0; i < lines.size(); ++i)
            hud.text(float3{ 2 * margin, 2 * margin + i * lineHeight, 0 }, lines[i], rgba(uint8_t(235), uint8_t(235), uint8_t(235)), size, Screen | Shadow);
        (void)view;
    }

    void visualize(FramePassContext& fc, ViewResources& view, bool linear, const std::map<std::string, ViewFn>& registered)
    {
        const std::string& name = m_settings.view;
        if (auto it = registered.find(name); it != registered.end())
        {
            it->second(fc, view, view.color, linear);
            return;
        }
        const auto found = std::find(kViews.begin(), kViews.end(), name);
        if (found == kViews.end())
        {
            std::string all;
            for (const std::string& v : kViews) all += " " + v;
            for (const auto& [n, f] : registered) all += " " + n;
            fail("debug.view = \"%s\": no such view (none%s)", name.c_str(), all.c_str());
        }
        const uint32_t mode = (uint32_t)(found - kViews.begin());
        TextureRef source;
        BufferRef clusters;
        switch (mode)
        {
        case 0: source = view.visId; break;
        case 1: source = view.visId; clusters = view.visibleClusters; break;
        case 2: source = view.depth; break;
        case 3: source = view.hiz; break;
        case 4: case 5: case 6: source = view.gbuffer; break;
        case 7: source = view.shadowVisibility; break;
        case 8: source = view.reflection; break;
        case 9: source = view.particleLayer; break;
        case 10: source = view.distortionOffset; break;
        case 11: case 12: source = view.volumeSlices; break;
        case 13: source = view.airVolume; break;
        case 14: source = view.coverageDepthRange; break;
        }
        const bool available = source.valid() && (mode != 1 || clusters.valid());
        const uint32_t tilePx = (uint32_t)fc.quality.integer("atmosphere.froxels.tile_px"), slices = (uint32_t)fc.quality.integer("atmosphere.froxels.depth_slices");
        const uint32_t probeX = m_settings.probe[0] >= 0 ? (uint32_t)m_settings.probe[0] : kOff, probeY = m_settings.probe[1] >= 0 ? (uint32_t)m_settings.probe[1] : kOff;
        const uint32_t W = view.view.width, H = view.view.height, index = m_settings.viewIndex;
        const TextureRef colour = view.color;
        const BufferRef buf = m_buffer;
        const D3D12_GPU_VIRTUAL_ADDRESS constants = view.frameConstants;
        ID3D12PipelineState* pso = fc.shaders.compute("Passes/Debug/DebugView");
        fc.graph.addPass("debug.view", QueueType::Graphics,
                         [=](PassBuilder& b) {
                             if (available) b.use(source, Use::SrvCompute);
                             if (clusters.valid()) b.use(clusters, Use::SrvCompute);
                             b.use(colour, Use::UavCompute);
                             b.use(buf, Use::UavCompute);
                         },
                         [=](PassContext& c) {
                             const uint32_t k[16] = { available ? mode : 255u, available ? c.srv(source) : kOff, clusters.valid() ? c.srv(clusters) : kOff, c.uav(colour),
                                                      linear ? 1u : 0u, index, probeX, probeY, tilePx, slices, 0, 0, W, H, 0, 0 };
                             c.cmd->SetPipelineState(pso);
                             c.bindFrameConstants(constants);
                             c.computeConstants(k, 16);
                             c.cmd->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
                         });
    }

    void draw(FramePassContext& fc, ViewResources& view, DXGI_FORMAT format, bool linear)
    {
        RenderGraph& g = fc.graph;
        const BufferRef buf = m_buffer;
        const uint32_t W = view.view.width, H = view.view.height;
        ID3D12PipelineState* args = fc.shaders.compute("Passes/Debug/DebugArgs");
        g.addPass("debug.args", QueueType::Graphics, [=](PassBuilder& b) { b.use(buf, Use::UavCompute); },
                  [=](PassContext& c) {
                      // Appends from passes that did not declare the buffer (any graphics-queue kernel) complete and are
                      // visible before the counts are read.
                      D3D12_GLOBAL_BARRIER gb{ D3D12_BARRIER_SYNC_ALL, D3D12_BARRIER_SYNC_COMPUTE_SHADING, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS,
                                               D3D12_BARRIER_ACCESS_UNORDERED_ACCESS };
                      D3D12_BARRIER_GROUP group{ D3D12_BARRIER_TYPE_GLOBAL, 1 };
                      group.pGlobalBarriers = &gb;
                      c.cmd->Barrier(1, &group);
                      const uint32_t k[4] = { c.uav(buf), 0, 0, 0 };
                      c.cmd->SetPipelineState(args);
                      c.computeConstants(k, 4);
                      c.cmd->Dispatch(1, 1, 1);
                  });

        const TextureRef overlay = g.createTexture(TextureDesc{ "debug.overlay", W, H, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
        const TextureRef depth = view.depth;
        const TextureRef atlas = (m_atlasRefFrame == m_frame) ? m_atlasRef : importAtlas(g);
        m_atlasRef = atlas;
        m_atlasRefFrame = m_frame;
        auto pipeline = [&](const char* name, const char* ms, const char* ps) {
            MeshPipelineDesc d;
            d.meshShader = ms;
            d.pixelShader = ps;
            d.renderTargets = { DXGI_FORMAT_R16G16B16A16_FLOAT };
            d.depthFormat = DXGI_FORMAT_UNKNOWN;
            d.depthWrite = false;
            d.cull = D3D12_CULL_MODE_NONE;
            d.premultipliedBlend = true;
            return fc.shaders.mesh(name, d);
        };
        ID3D12PipelineState* triangles = pipeline("debug.triangles", "Passes/Debug/DebugTriangles.ms", "Passes/Debug/DebugTriangles.ps");
        ID3D12PipelineState* lines = pipeline("debug.lines", "Passes/Debug/DebugLines.ms", "Passes/Debug/DebugLines.ps");
        ID3D12PipelineState* glyphs = pipeline("debug.glyphs", "Passes/Debug/DebugGlyphs.ms", "Passes/Debug/DebugGlyphs.ps");
        ID3D12CommandSignature* sig = m_meshSignature.Get();
        const D3D12_GPU_VIRTUAL_ADDRESS constants = view.frameConstants;
        g.addPass("debug.draw", QueueType::Graphics,
                  [=](PassBuilder& b) {
                      b.use(overlay, Use::RenderTarget);
                      b.use(buf, Use::IndirectArgs);
                      b.use(buf, Use::SrvGraphics);
                      if (depth.valid()) b.use(depth, Use::SrvGraphics);
                      b.use(atlas, Use::SrvGraphics);
                  },
                  [=](PassContext& c) {
                      const D3D12_CPU_DESCRIPTOR_HANDLE rtv = c.rtv(overlay);
                      const float zero[4] = {};
                      c.cmd->ClearRenderTargetView(rtv, zero, 0, nullptr);
                      c.cmd->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
                      D3D12_VIEWPORT vp{ 0, 0, (float)W, (float)H, 0, 1 };
                      D3D12_RECT sc{ 0, 0, (LONG)W, (LONG)H };
                      c.cmd->RSSetViewports(1, &vp);
                      c.cmd->RSSetScissorRects(1, &sc);
                      c.bindFrameConstants(constants);
                      const uint32_t k[8] = { c.srv(buf), depth.valid() ? c.srv(depth) : kOff, c.srv(atlas), kAtlasColumns, kAtlasRows, kCellH, kAtlasMips, 0 };
                      c.graphicsConstants(k, 8);
                      ID3D12Resource* args = c.resource(buf);
                      c.cmd->SetPipelineState(triangles);
                      c.cmd->ExecuteIndirect(sig, 1, args, kArgsTriangles, nullptr, 0);
                      c.cmd->SetPipelineState(lines);
                      c.cmd->ExecuteIndirect(sig, 1, args, kArgsLines, nullptr, 0);
                      c.cmd->SetPipelineState(glyphs);
                      c.cmd->ExecuteIndirect(sig, 1, args, kArgsGlyphs, nullptr, 0);
                  });

        const TextureRef colour = view.color;
        const TextureRef copy = g.createTexture(TextureDesc{ "debug.colour.copy", W, H, 1, 1, format == DXGI_FORMAT_R10G10B10A2_TYPELESS ? DXGI_FORMAT_R10G10B10A2_UNORM : format });
        g.addPass("debug.copy", QueueType::Graphics,
                  [=](PassBuilder& b) {
                      b.use(colour, Use::CopySrc);
                      b.use(copy, Use::CopyDst);
                  },
                  [=](PassContext& c) { c.cmd->CopyResource(c.resource(copy), c.resource(colour)); });
        ID3D12PipelineState* composite = fc.shaders.compute("Passes/Debug/DebugComposite");
        g.addPass("debug.composite", QueueType::Graphics,
                  [=](PassBuilder& b) {
                      b.use(overlay, Use::SrvCompute);
                      b.use(copy, Use::SrvCompute);
                      b.use(colour, Use::UavCompute);
                  },
                  [=](PassContext& c) {
                      const uint32_t k[8] = { c.srv(overlay), c.srv(copy), c.uav(colour), linear ? 1u : 0u, W, H, 0, 0 };
                      c.cmd->SetPipelineState(composite);
                      c.computeConstants(k, 8);
                      c.cmd->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
                  });

        Slot& slot = m_slots[m_frame % m_slots.size()];
        slot.readbackFrame = m_frame;
        ID3D12Resource* readback = slot.readback.Get();
        g.addPass("debug.stats", QueueType::Graphics,
                  [=](PassBuilder& b) {
                      b.use(buf, Use::CopySrc);
                      b.keep();
                  },
                  [=](PassContext& c) { c.cmd->CopyBufferRegion(readback, 0, c.resource(buf), 0, 16); });
    }

    Device& m_device;
    ComPtr<ID3D12CommandSignature> m_meshSignature;
    ComPtr<ID3D12Resource> m_resource, m_atlas, m_atlasUpload;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_atlasFootprints[kAtlasMips] = {};
    bool m_atlasPending = false;
    TextureRef m_atlasRef;
    uint64_t m_atlasRefFrame = UINT64_MAX;
    uint32_t m_uav = kOff;
    Capacities m_caps;
    std::vector<Slot> m_slots;
    Settings m_settings;
    uint64_t m_frame = UINT64_MAX;
    bool m_on = false;
    BufferRef m_buffer;
    Stats m_stats;
};

DebugPass& pass(FramePassContext& fc)
{
    auto& p = fc.state<std::unique_ptr<DebugPass>>("debug.pass");
    if (!p) p = std::make_unique<DebugPass>(fc.device);
    return *p;
}
std::map<std::string, ViewFn>& views(TrackState& state) { return state.get<std::map<std::string, ViewFn>>("debug.views"); }
} // namespace

uint32_t rgba(float r, float g, float b, float a)
{
    auto u = [](float x) { return (uint8_t)std::lround(std::clamp(x, 0.0f, 1.0f) * 255.0f); };
    return rgba(u(r), u(g), u(b), u(a));
}

void DrawList::line(float3 a, float3 b, uint32_t color, float widthPx, uint32_t flags) { lines.push_back({ a, color, b, widthFlags(widthPx, flags) }); }
void DrawList::triangle(float3 a, float3 b, float3 c, uint32_t color, uint32_t flags) { triangles.push_back({ a, color, b, flags, c, 0 }); }
void DrawList::box(float3 lo, float3 hi, uint32_t color, float widthPx, uint32_t flags)
{
    for (uint32_t e = 0; e < 12; ++e)
    {
        const uint32_t axis = e / 4, k = e % 4, u = (axis + 1) % 3, v = (axis + 2) % 3;
        float p[3] = { lo.x, lo.y, lo.z }, q[3] = { lo.x, lo.y, lo.z };
        const float h[3] = { hi.x, hi.y, hi.z };
        if (k & 1) p[u] = q[u] = h[u];
        if (k & 2) p[v] = q[v] = h[v];
        q[axis] = h[axis];
        line({ p[0], p[1], p[2] }, { q[0], q[1], q[2] }, color, widthPx, flags);
    }
}
void DrawList::sphere(float3 centre, float radius, uint32_t color, float widthPx, uint32_t flags, uint32_t segments)
{
    segments = std::max(segments, 3u);
    const float3 axes[3][2] = { { { 1, 0, 0 }, { 0, 1, 0 } }, { { 0, 1, 0 }, { 0, 0, 1 } }, { { 0, 0, 1 }, { 1, 0, 0 } } };
    for (const auto& ax : axes)
        for (uint32_t i = 0; i < segments; ++i)
        {
            const float t0 = 6.283185307f * i / segments, t1 = 6.283185307f * (i + 1) / segments;
            line(centre + (ax[0] * std::cos(t0) + ax[1] * std::sin(t0)) * radius, centre + (ax[0] * std::cos(t1) + ax[1] * std::sin(t1)) * radius, color, widthPx, flags);
        }
}
void DrawList::arrow(float3 from, float3 to, uint32_t color, float widthPx, uint32_t flags, float headFraction)
{
    line(from, to, color, widthPx, flags);
    const float3 d = to - from;
    const float len = length(d);
    if (!(len > 0)) return;
    const float3 n = d / len;
    const float3 helper = std::abs(n.y) < 0.9f ? float3{ 0, 1, 0 } : float3{ 1, 0, 0 };
    const float3 u = normalize(cross(n, helper)), v = cross(n, u);
    const float3 base = to - d * headFraction;
    const float r = len * headFraction * 0.35f;
    for (const float3& o : { u * r, u * -r, v * r, v * -r }) line(to, base + o, color, widthPx, flags);
}
void DrawList::text(float3 anchor, std::string_view s, uint32_t color, float sizePx, uint32_t flags, float2 offsetPx)
{
    const uint32_t size = (uint32_t)std::clamp(std::lround(sizePx), 1l, 255l);
    float x = offsetPx.x, y = offsetPx.y;
    for (char ch : s)
    {
        if (ch == '\n')
        {
            x = offsetPx.x;
            y += size * 1.125f;
            continue;
        }
        const uint8_t code = (uint8_t)ch;
        if (code >= 32 && code <= 126 && code != 32) glyphs.push_back({ anchor, color, { x, y }, code | (size << 8) | (flags << 16), 0 });
        x += size * 0.5f;
    }
}
void DrawList::clear()
{
    lines.clear();
    triangles.clear();
    glyphs.clear();
}

DrawList& drawList(TrackState& state) { return state.get<DrawList>("debug.list"); }
Stats lastStats(TrackState& state)
{
    auto& p = state.get<std::unique_ptr<DebugPass>>("debug.pass");
    return p ? p->stats() : Stats{};
}
BufferRef buffer(FramePassContext& fc)
{
    if (!fc.trackState) return {};
    auto& p = fc.trackState->get<std::unique_ptr<DebugPass>>("debug.pass");
    return p ? p->buffer(fc.frame.frameIndex) : BufferRef{};
}
void registerView(TrackState& state, const std::string& name, ViewFn fn)
{
    if (name == "none" || std::find(kViews.begin(), kViews.end(), name) != kViews.end()) fail("debug::registerView: '%s' is a built-in view", name.c_str());
    views(state)[name] = std::move(fn);
}
const std::vector<std::string>& builtInViews() { return kViews; }

std::vector<std::string> hudLines(const FrameTiming* timing, const Stats& stats)
{
    std::vector<std::string> out;
    char line[160];
    if (!timing)
        out.push_back("GPU: no timings (FrameContext::timing)");
    else
    {
        std::snprintf(line, sizeof line, "GPU frame %llu  %.3f ms", (unsigned long long)timing->frame, timing->gpuFrameMs);
        out.push_back(line);
        const double minor = 0.01 * timing->gpuFrameMs;
        std::map<std::string, std::pair<double, uint32_t>> groups;
        for (const PassTiming& p : timing->passes)
        {
            const double ms = p.durationMs();
            if (ms >= minor)
            {
                std::snprintf(line, sizeof line, "%-32.32s %7.3f", p.name.c_str(), ms);
                out.push_back(line);
                continue;
            }
            const size_t dot = p.name.find('.');
            auto& g = groups[p.name.substr(0, dot)];
            g.first += ms;
            g.second += 1;
        }
        for (const auto& [prefix, g] : groups)
        {
            const std::string name = prefix + ".* (" + std::to_string(g.second) + ")";
            std::snprintf(line, sizeof line, "%-32.32s %7.3f", name.c_str(), g.first);
            out.push_back(line);
        }
    }
    if (stats.frame != UINT64_MAX)
    {
        std::snprintf(line, sizeof line, "debug: %u lines, %u triangles, %u glyphs%s%s%s", stats.lines, stats.triangles, stats.glyphs,
                      (stats.status & LinesFull) ? " LINES FULL" : "", (stats.status & TrianglesFull) ? " TRIANGLES FULL" : "",
                      (stats.status & GlyphsFull) ? " GLYPHS FULL" : "");
        out.push_back(line);
    }
    return out;
}
} // namespace unx::debug

namespace unx::render::tracks
{
uint32_t debugBegin(FramePassContext& fc)
{
    if (!fc.trackState) return 0xFFFFFFFFu;
    return debug::pass(fc).begin(fc, debug::drawList(*fc.trackState));
}
void debugOverlay(FramePassContext& fc, ViewResources& view)
{
    if (!fc.trackState) return;
    auto& p = fc.trackState->get<std::unique_ptr<debug::DebugPass>>("debug.pass");
    if (p) p->overlay(fc, view, debug::views(*fc.trackState));
}
} // namespace unx::render::tracks
