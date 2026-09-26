// Track E timing gate: the performance records of A7 (decals, surface state), A15 (debug draw), B9 (particle media,
// heat haze) and B10 (strand hair) at 4K and 1440p. GPU lock kind timing; the harness's warm-up, medians and per-pass
// timestamps; only E's passes are reported (the fixture's own passes - synthetic depth, clears - are listed apart).
// Loads (the design caps of FEATURES_GAME 18 where it has one, else a stated game-profile load):
//   decals     4,096 active projected decals (the cap) on the floor in front of an eye-height camera, 16 per tile cap;
//   surface    64,000 resident bricks of 0.25 m voxels; 1,024 changed bricks uploaded every frame (a steady change set
//              above one World tick's, every frame instead of every tick: an upper bound);
//   debug      typical: 10,000 lines + 2,000 glyphs + the HUD; cap: 1 M lines, 256 k triangles, 64 k glyphs;
//   volume     2,048 smoke/dust media particles (0.3-3 m, 4-60 m away) + 256 heat-haze records;
//   hair       8 bodies x 2,500 guides x 12 nodes, 16 follow strands per guide (40,000 strands, 440,000 segments a
//              body), at 1-40 m; the guide simulation runs every frame here (per tick in the game: report its passes
//              per tick).
//   unx_gate_debug_egate --case decals|surface|debug|debugcap|volume|hair|all [--resolution 4K|1440p|both]
//                        [--frames N] [--out DIR] [--set key=value ...]
#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/debug/DebugDraw.h"
#include "unx/decal/Decals.h"
#include "unx/decal/SurfaceState.h"
#include "unx/hair/Hair.h"
#include "unx/render/FrameRenderer.h"
#include "unx/render/GpuLock.h"
#include "unx/render/GpuScene.h"
#include "unx/render/Harness.h"
#include "unx/render/Tracks.h"
#include "unx/volume/VolumePass.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::render;

namespace
{
constexpr float kPi = 3.14159265358979f;

// Frame constants in an upload ring (FrameRenderer::frameConstants packing; 4 frames x 4 views).
class ConstantsRing
{
public:
    explicit ConstantsRing(Device& device)
    {
        D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC1 d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = 16 * 1024;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        check(device.d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_buffer)),
              "gate frame constants");
        D3D12_RANGE none{ 0, 0 };
        check(m_buffer->Map(0, &none, reinterpret_cast<void**>(&m_mapped)), "map gate frame constants");
    }
    D3D12_GPU_VIRTUAL_ADDRESS put(const GpuScene& scene, const FrameContext& frame, const ViewDesc& view, uint32_t debugDraw, uint32_t slot)
    {
        gpu::FrameConstants c = FrameRenderer::frameConstants(scene, frame, view);
        c.debugDraw = debugDraw;
        const uint32_t i = (uint32_t)(frame.frameIndex % 4) * 4 + slot;
        std::memcpy(m_mapped + (size_t)i * 1024, &c, sizeof c);
        return m_buffer->GetGPUVirtualAddress() + (uint64_t)i * 1024;
    }

private:
    ComPtr<ID3D12Resource> m_buffer;
    uint8_t* m_mapped = nullptr;
};

// A buffer written once from the CPU (default heap, uploaded by a graph copy in the first frame, then imported).
class StaticBuffer
{
public:
    void set(Device& device, const void* data, uint64_t bytes)
    {
        D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_DEFAULT }, up{ D3D12_HEAP_TYPE_UPLOAD };
        D3D12_RESOURCE_DESC1 d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = bytes;
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        // UAV-capable: VolumePass's setup opens given records as a RWStructuredBuffer (a buffer without the flag gets an
        // invalid UAV descriptor, and the device hangs on it without the debug layer - the 2026-09-26 gate TDRs)
        D3D12_RESOURCE_DESC1 dd = d;
        dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        check(device.d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &dd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_buffer)),
              "gate buffer");
        check(device.d3d()->CreateCommittedResource3(&up, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&m_upload)),
              "gate buffer upload");
        void* p = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(m_upload->Map(0, &none, &p), "map gate upload");
        std::memcpy(p, data, bytes);
        m_upload->Unmap(0, nullptr);
        m_bytes = bytes;
        m_pending = true;
    }
    BufferRef import(RenderGraph& g, const char* name, uint32_t stride)
    {
        const BufferRef b = g.importBuffer(m_buffer.Get(), BufferDesc{ name, m_bytes, stride });
        if (m_pending)
        {
            ID3D12Resource* src = m_upload.Get();
            const uint64_t n = m_bytes;
            g.addPass("gate.upload", QueueType::Graphics, [=](PassBuilder& pb) { pb.use(b, Use::CopyDst); },
                      [=](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(b), 0, src, 0, n); });
            m_pending = false;
        }
        return b;
    }

private:
    ComPtr<ID3D12Resource> m_buffer, m_upload;
    uint64_t m_bytes = 0;
    bool m_pending = false;
};

// The gate has no later readers: a kept pass that reads a case's outputs, so the graph does not cull the passes
// that produce them (a frame's real consumers are M, S, V).
void consume(RenderGraph& g, std::vector<TextureRef> textures, std::vector<BufferRef> buffers)
{
    g.addPass("gate.consume", QueueType::Graphics,
              [=](PassBuilder& b) {
                  for (TextureRef t : textures)
                      if (t.valid()) b.use(t, Use::SrvCompute);
                  for (BufferRef x : buffers)
                      if (x.valid()) b.use(x, Use::SrvCompute);
                  b.keep();
              },
              [](PassContext&) {});
}

scene::Scene gateScene()
{
    scene::Scene s;
    s.name = "e_gate";
    for (int k = 0; k < 4; ++k)
    {
        scene::Material m;
        m.name = "m" + std::to_string(k);
        m.baseColor = { 0.2f + 0.2f * k, 0.5f, 0.8f - 0.15f * k };
        m.roughness = 0.3f + 0.15f * k;
        s.materials.push_back(m);
    }
    scene::Mesh floor;
    floor.name = "floor";
    const float e = 200;
    floor.positions = { { -e, 0, -e }, { -e, 0, e }, { e, 0, e }, { e, 0, -e } };
    floor.normals = { { 0, 1, 0 }, { 0, 1, 0 }, { 0, 1, 0 }, { 0, 1, 0 } };
    floor.uv0 = { { 0, 0 }, { 0, 1 }, { 1, 1 }, { 1, 0 } };
    floor.indices = { 0, 1, 2, 0, 2, 3 };
    floor.submeshes = { { 0, 6, 0 } };
    s.meshes.push_back(floor);
    scene::Instance in;
    in.mesh = 0;
    s.instances.push_back(in);
    scene::Camera c;
    c.name = "eye";
    c.position = { 0, 1.7f, 0 };
    c.forward = normalize(float3{ 0, -0.3f, -1 });
    c.verticalFov = 60 * kPi / 180;
    s.cameras.push_back(c);
    return s;
}

double passSum(const HarnessResult& r, const std::vector<std::string>& prefixes, std::string* list)
{
    double sum = 0;
    for (const std::string& name : r.passOrder)
    {
        const auto it = r.passMs.find(name);
        if (it == r.passMs.end()) continue;
        for (const std::string& p : prefixes)
            if (name.rfind(p, 0) == 0)
            {
                sum += it->second.median;
                if (list) *list += format(" %s=%.4f", name.c_str(), it->second.median);
                break;
            }
    }
    return sum;
}

struct Gate
{
    Device& device;
    ShaderLibrary& shaders;
    QualityConfig& quality;
    GpuScene& scene;
    scene::Scene sceneData;
    ConstantsRing ring;
    Harness harness;
    std::string out;
    uint32_t frames;
    uint32_t debugUav = 0xFFFFFFFFu;  // this frame's debug-primitive buffer (tracks::debugBegin) for the frame constants

    Gate(Device& d, ShaderLibrary& s, QualityConfig& q, GpuScene& sc, std::string o, uint32_t f)
        : device(d), shaders(s), quality(q), scene(sc), sceneData(gateScene()), ring(d), harness(d, q), out(std::move(o)), frames(f)
    {
        scene.upload(sceneData);
    }

    // Runs one case at one resolution: build(fc, view, frame) declares the case's passes for the main view.
    HarnessResult run(const std::string& label, const Resolution& res, TrackState& state,
                      const std::function<void(FramePassContext&, ViewResources&, FrameContext&)>& build)
    {
        FrameContext frame;
        frame.mainView = ViewDesc::fromCamera(sceneData.cameras[0], res.width, res.height, float4x4{});
        frame.mainView.prevViewProj = frame.mainView.viewProj;
        FrameServices services;
        HarnessOptions ho;
        ho.frames = frames;
        ho.label = "e_" + label;
        ho.outputDirectory = out;
        debugUav = 0xFFFFFFFFu;
        return harness.run(res, ho, [&](RenderGraph& graph, const Resolution&, uint64_t f) {
            frame.frameIndex = f;
            frame.time = f / 165.0;
            frame.deltaTime = 1.0f / 165;
            FrameResources resources;
            uint32_t slot = 0;
            FramePassContext fc{ device, graph, shaders, quality, scene, frame, resources, services,
                                 [&](const ViewDesc& v) { return ring.put(scene, frame, v, debugUav, slot++ & 3); }, &state };
            ViewResources view;
            view.view = frame.mainView;
            build(fc, view, frame);
        });
    }
};

// ---------------------------------------------------------------- cases

HarnessResult caseDecals(Gate& g, const Resolution& res)
{
    TrackState state;
    decal::DecalSet& set = decal::decals(state);
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> U(0, 1);
    for (uint32_t i = 0; i < 4096; ++i)
    {
        decal::Decal d;
        const float yaw = 2 * kPi * U(rng), hx = 0.1f + 0.5f * U(rng), hy = 0.1f + 0.5f * U(rng);
        const float3 x{ std::cos(yaw) * hx, 0, std::sin(yaw) * hx }, y{ -std::sin(yaw) * hy, 0, std::cos(yaw) * hy }, z{ 0, 0.2f, 0 };
        const float3 c{ -15 + 30 * U(rng), 0, -40 + 38 * U(rng) };
        const float3 cols[4] = { x, y, z, c };
        for (int col = 0; col < 4; ++col)
        {
            d.box.m[0][col] = cols[col].x;
            d.box.m[1][col] = cols[col].y;
            d.box.m[2][col] = cols[col].z;
        }
        d.material = 1 + (i % 3);
        d.priority = (int32_t)(i % 3);
        d.opacity = 0.8f;
        set.add(d);
    }
    const HarnessResult r = g.run("decals", res, state, [&](FramePassContext& fc, ViewResources& v, FrameContext&) {
        v.frameConstants = fc.frameConstantsFor(v.view);
        v.depth = fc.graph.createTexture(TextureDesc{ "gate.depth", res.width, res.height, 1, 1, DXGI_FORMAT_R32_FLOAT });
        const TextureRef depth = v.depth;
        const D3D12_GPU_VIRTUAL_ADDRESS cb = v.frameConstants;
        ID3D12PipelineState* depthPso = fc.shaders.compute("Passes/Decal/Tests/DecalProbe.MODE0");
        const uint32_t W = res.width, H = res.height;
        fc.graph.addPass("gate.depth", QueueType::Graphics, [&](PassBuilder& b) { b.use(depth, Use::UavCompute); },
                         [=](PassContext& c) {
                             const uint32_t k[8] = { 0, c.uav(depth), 0, 0, 0 /* floor y = 0 */, 0, 0, 0 };
                             c.cmd->SetPipelineState(depthPso);
                             c.bindFrameConstants(cb);
                             c.computeConstants(k, 8);
                             c.cmd->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
                         });
        tracks::decals(fc, v);
    });
    const decal::Stats st = decal::lastStats(state);
    std::string list;
    const double e = passSum(r, { "decal" }, &list);
    std::printf("E_GATE case=decals resolution=%s decals=%u in_view=%u full_tiles=%u e_passes_ms_median=%.4f gpu_frame_ms_median=%.4f p95=%.4f |%s\n",
                res.name.c_str(), 4096u, st.decals, st.fullTiles, e, r.gpuFrameMs.median, r.gpuFrameMs.p95, list.c_str());
    return r;
}

HarnessResult caseSurface(Gate& g, const Resolution& res)
{
    TrackState state;
    surface::SurfaceField& field = surface::surfaceField(state);
    field.setCapacity((uint32_t)g.quality.integer("surface.max_bricks"));
    std::mt19937 rng(11);
    std::uniform_real_distribution<float> U(0, 1);
    std::vector<surface::BrickInput> bricks(64000);
    for (uint32_t i = 0; i < bricks.size(); ++i)
    {
        surface::BrickInput& b = bricks[i];
        std::memset(&b, 0, sizeof b);
        b.key[0] = (int32_t)(i % 256) - 128;
        b.key[1] = (int32_t)(i / 65536);
        b.key[2] = (int32_t)((i / 256) % 256) - 128;
        for (float& v : b.value) v = U(rng);
    }
    field.apply(bricks.data(), bricks.size(), nullptr, 0);
    std::vector<surface::BrickInput> changed(1024);
    uint32_t cursor = 0;
    const HarnessResult r = g.run("surface", res, state, [&](FramePassContext& fc, ViewResources&, FrameContext&) {
        for (surface::BrickInput& c : changed)
        {
            c = bricks[(cursor = (cursor * 1103515245u + 12345u)) % bricks.size()];
            c.value[0] = U(rng);
        }
        field.apply(changed.data(), changed.size(), nullptr, 0);
        tracks::surfaceState(fc);
    });
    std::string list;
    const double e = passSum(r, { "surface" }, &list);
    std::printf("E_GATE case=surface resolution=%s bricks=%zu changed_per_frame=%zu e_passes_ms_median=%.4f gpu_frame_ms_median=%.4f p95=%.4f |%s\n",
                res.name.c_str(), field.bricks(), changed.size(), e, r.gpuFrameMs.median, r.gpuFrameMs.p95, list.c_str());
    return r;
}

HarnessResult caseDebug(Gate& g, const Resolution& res, bool cap)
{
    TrackState state;
    g.quality.applyOverride("debug.hud=true");
    std::mt19937 rng(5);
    std::uniform_real_distribution<float> U(0, 1);
    const uint32_t lines = cap ? 1000000 : 10000, triangles = cap ? 262144 : 0, glyphs = cap ? 65536 : 2000;
    const HarnessResult r = g.run(cap ? "debugcap" : "debug", res, state, [&](FramePassContext& fc, ViewResources& v, FrameContext&) {
        debug::DrawList& list = debug::drawList(*fc.trackState);
        list.clear();
        std::mt19937 frameRng(5);  // the same primitives every frame (immediate mode)
        std::uniform_real_distribution<float> R(0, 1);
        auto p = [&]() { return float3{ -20 + 40 * R(frameRng), 3 * R(frameRng), -40 + 38 * R(frameRng) }; };
        for (uint32_t i = 0; i < lines; ++i) list.line(p(), p(), debug::rgba(uint8_t(255), uint8_t(200), uint8_t(40)));
        for (uint32_t i = 0; i < triangles; ++i)
        {
            const float3 a = p();
            list.triangle(a, a + float3{ 0.2f, 0, 0 }, a + float3{ 0, 0.2f, 0 }, debug::rgba(uint8_t(40), uint8_t(200), uint8_t(255), uint8_t(128)));
        }
        for (uint32_t i = 0; i < glyphs / 8; ++i) list.text(p(), "12345678", debug::rgba(uint8_t(235), uint8_t(235), uint8_t(235)), 14.0f);
        g.debugUav = tracks::debugBegin(fc);
        v.frameConstants = fc.frameConstantsFor(v.view);
        v.color = fc.graph.createTexture(TextureDesc{ "gate.colour", res.width, res.height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
        v.depth = fc.graph.createTexture(TextureDesc{ "gate.depth", res.width, res.height, 1, 1, DXGI_FORMAT_D32_FLOAT });
        const TextureRef colour = v.color, depth = v.depth;
        fc.graph.addPass("gate.clear", QueueType::Graphics,
                         [&](PassBuilder& b) {
                             b.use(colour, Use::RenderTarget);
                             b.use(depth, Use::DepthWrite);
                         },
                         [=](PassContext& c) {
                             const float bg[4] = { 0.1f, 0.12f, 0.15f, 1 };
                             c.cmd->ClearRenderTargetView(c.rtv(colour), bg, 0, nullptr);
                             c.cmd->ClearDepthStencilView(c.dsv(depth), D3D12_CLEAR_FLAG_DEPTH, 0.001f, 0, 0, nullptr);
                         });
        tracks::debugOverlay(fc, v);
        consume(fc.graph, { v.color }, {});
    });
    g.quality.applyOverride("debug.hud=false");
    const debug::Stats st = debug::lastStats(state);
    std::string list;
    const double e = passSum(r, { "debug" }, &list);
    std::printf("E_GATE case=%s resolution=%s lines=%u triangles=%u glyphs=%u (drawn %u lines, %u glyphs) e_passes_ms_median=%.4f gpu_frame_ms_median=%.4f p95=%.4f |%s\n",
                cap ? "debugcap" : "debug", res.name.c_str(), lines, triangles, glyphs, st.lines, st.glyphs, e, r.gpuFrameMs.median, r.gpuFrameMs.p95, list.c_str());
    return r;
}

struct VolumeRecord  // VolumeCommon.hlsli
{
    float centre[3], radius, a[3], mass, b[3];
    uint32_t kind;
};
static_assert(sizeof(VolumeRecord) == 48);

uint32_t g_volumeMedia = 2048, g_volumeHaze = 256;
bool g_dry = false;

HarnessResult caseVolume(Gate& g, const Resolution& res)
{
    TrackState state;
    uint32_t gridX, gridY, slices, tilePx;
    volume::froxelGridSize(g.quality, res.width, res.height, gridX, gridY, slices, tilePx);
    const float nearM = (float)g.quality.number("atmosphere.froxels.near_m"), farM = (float)g.quality.number("atmosphere.froxels.far_m");
    std::printf("volume grid %ux%ux%u tile %u px, near %.3f far %.1f\n", gridX, gridY, slices, tilePx, nearM, farM);
    if (!gridX || !gridY || !slices || !tilePx) fail("volume gate: empty froxel grid");
    if (g_dry) return {};
    // S's lists for a view without local lights (FroxelCommon.hlsli header + empty lists)
    const uint32_t froxels = gridX * gridY * slices;
    std::vector<uint8_t> header(64 + 4 * (size_t)froxels + 64, 0);
    {
        uint32_t u[16] = {};
        u[0] = gridX; u[1] = gridY; u[2] = slices; u[3] = tilePx;
        const float f[4] = { nearM, farM, std::log2(farM / nearM), 0 };
        std::memcpy(&u[4], f, 16);
        u[8] = 64; u[9] = 64 + 4 * froxels;
        std::memcpy(header.data(), u, 64);
    }
    const float3 fwd = normalize(g.sceneData.cameras[0].forward), right = normalize(cross(fwd, float3{ 0, 1, 0 })), up = cross(right, fwd);
    std::mt19937 rng(3);
    std::uniform_real_distribution<float> U(0, 1);
    std::vector<VolumeRecord> recs;
    for (uint32_t i = 0; i < g_volumeMedia; ++i)
    {
        const float d = 4 + 56 * U(rng), sx = (U(rng) - 0.5f) * d * 1.2f, sy = (U(rng) - 0.3f) * d * 0.5f;
        const float3 rel = fwd * d + right * sx + up * sy;
        VolumeRecord x{};
        x.centre[0] = rel.x; x.centre[1] = rel.y; x.centre[2] = rel.z;
        x.radius = 0.3f + 2.7f * U(rng);
        x.mass = 0.2f + 2 * U(rng);
        for (int k = 0; k < 3; ++k) { x.a[k] = 0.4f + 0.4f * U(rng); x.b[k] = 20 * U(rng); }
        x.kind = 1;
        recs.push_back(x);
    }
    for (uint32_t i = 0; i < g_volumeHaze; ++i)
    {
        const float d = 3 + 30 * U(rng);
        const float3 rel = fwd * d + right * ((U(rng) - 0.5f) * d) + up * ((U(rng) - 0.3f) * d * 0.4f);
        VolumeRecord x{};
        x.centre[0] = rel.x; x.centre[1] = rel.y; x.centre[2] = rel.z;
        x.radius = 0.4f + 1.6f * U(rng);
        const bool shell = i % 4 == 0;
        x.a[0] = shell ? 5e-4f : -1.4e-4f;
        x.a[1] = shell ? 0.15f : 0.0f;
        x.a[2] = shell ? 1.0f : 0.0f;
        x.mass = shell ? x.radius * (1 + 4 * 0.15f) : 3 * x.radius;
        x.kind = 2;
        recs.push_back(x);
    }
    StaticBuffer froxelBuf, recordBuf;
    froxelBuf.set(g.device, header.data(), header.size());
    recordBuf.set(g.device, recs.data(), recs.size() * sizeof(VolumeRecord));
    volume::VolumePass pass(g.device);
    const HarnessResult r = g.run("volume", res, state, [&](FramePassContext& fc, ViewResources& v, FrameContext& frame) {
        const BufferRef lists = froxelBuf.import(fc.graph, "gate.froxels", 0);
        const BufferRef rb = recordBuf.import(fc.graph, "gate.records", sizeof(VolumeRecord));
        volume::VolumeFrame f;
        f.view = &frame.mainView;
        f.frameConstants = fc.frameConstantsFor(v.view);
        f.froxelLights = lists;
        f.media = true;
        f.haze = true;
        const volume::VolumeOutput o = pass.recordRecords(rb, (uint32_t)recs.size(), fc.graph, fc.shaders, fc.quality, f);
        if (!o.valid) fail("volume gate: the pass declared nothing");
        consume(fc.graph, { o.volumeSlices, o.distortionOffset, o.distortionDepth }, {});
    });
    std::string list;
    const double e = passSum(r, { "volume" }, &list);
    std::printf("E_GATE case=volume resolution=%s media=%u haze=%u grid=%ux%ux%u e_passes_ms_median=%.4f gpu_frame_ms_median=%.4f p95=%.4f |%s\n", res.name.c_str(), g_volumeMedia, g_volumeHaze, gridX,
                gridY, slices, e, r.gpuFrameMs.median, r.gpuFrameMs.p95, list.c_str());
    return r;
}

HarnessResult caseHair(Gate& g, const Resolution& res)
{
    TrackState state;
    hair::HairSystem& hs = hair::hairSystem(state);
    const uint32_t N = 12, G = 2500, F = 16;
    const float distances[8] = { 1.0f, 2, 3, 5, 8, 12, 20, 40 };
    std::vector<uint32_t> bodies;
    std::vector<float3x4> joints;
    std::mt19937 rng(9);
    std::uniform_real_distribution<float> U(0, 1);
    const float3 eye = g.sceneData.cameras[0].position, fwd = normalize(g.sceneData.cameras[0].forward);
    for (float d : distances)
    {
        hair::BodyDesc b;
        b.nodesPerStrand = N;
        b.joints = 1;
        for (uint32_t k = 0; k < G; ++k)
        {
            // roots on the upper scalp (radius 0.1 m), strands falling outwards and down, 2.5 cm segments
            const float phi = 2 * kPi * U(rng), c = 0.1f + 0.9f * U(rng), s = std::sqrt(1 - c * c);
            const float3 n{ s * std::cos(phi), c, s * std::sin(phi) };
            const float3 dir = normalize(n + float3{ 0, -1.5f, 0 });
            for (uint32_t i = 0; i < N; ++i) b.restPositions.push_back(n * 0.1f + dir * (0.025f * i));
            b.guideJoint.push_back(0);
            for (uint32_t f = 0; f < F; ++f) b.follows.push_back({ k, float3{ 0, 0.003f * (U(rng) - 0.5f), 0.003f * (U(rng) - 0.5f) }, 1.2f });
        }
        bodies.push_back(hs.addBody(b));
        float3x4 j;
        const float3 head = eye + fwd * d;
        j.m[0][3] = head.x;
        j.m[1][3] = head.y;
        j.m[2][3] = head.z;
        joints.push_back(j);
    }
    const HarnessResult r = g.run("hair", res, state, [&](FramePassContext& fc, ViewResources& v, FrameContext&) {
        for (size_t k = 0; k < bodies.size(); ++k)
        {
            const hair::Capsule head{ float3{ joints[k].m[0][3], joints[k].m[1][3] - 0.05f, joints[k].m[2][3] }, 0.095f,
                                      float3{ joints[k].m[0][3], joints[k].m[1][3] + 0.02f, joints[k].m[2][3] } };
            hs.tick(bodies[k], &joints[k], 1, &head, 1, float3{ 1, 0, 0 }, 1.0f / 60);
        }
        hs.setFrameFraction(1.0f);
        v.frameConstants = fc.frameConstantsFor(v.view);
        tracks::hair(fc, v);
        consume(fc.graph, {}, { fc.resources.hairSegments, fc.resources.hairBodies });
    });
    std::string list;
    const double e = passSum(r, { "hair" }, &list);
    std::printf("E_GATE case=hair resolution=%s bodies=8 guides=%u follows=%u segments_per_body=%u e_passes_ms_median=%.4f gpu_frame_ms_median=%.4f p95=%.4f |%s\n",
                res.name.c_str(), G, G * F, G * F * (N - 1), e, r.gpuFrameMs.median, r.gpuFrameMs.p95, list.c_str());
    return r;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        std::string which = "all", resolution = "both", out;
        uint32_t frames = 600;
        std::vector<std::string> overrides;
        bool debugLayer = false;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            auto next = [&]() -> std::string { if (i + 1 >= argc) fail("missing value after %s", a.c_str()); return argv[++i]; };
            if (a == "--case") which = next();
            else if (a == "--resolution") resolution = next();
            else if (a == "--frames") frames = (uint32_t)std::stoul(next());
            else if (a == "--out") out = next();
            else if (a == "--set") overrides.push_back(next());
            else if (a == "--volume-media") g_volumeMedia = (uint32_t)std::stoul(next());
            else if (a == "--volume-haze") g_volumeHaze = (uint32_t)std::stoul(next());
            else if (a == "--dry") g_dry = true;
            else if (a == "--debug-layer") debugLayer = true;
            else fail("unknown option %s", a.c_str());
        }
        if (!g_dry) requireGpuLock("e.gate");
        QualityConfig quality = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
        for (const auto& o : overrides) quality.applyOverride(o);
        DeviceOptions opts;
        opts.debugLayer = debugLayer;  // correctness checks of the fixture only; timings without it
        Device device(opts);
        ShaderLibrary shaders(device, executableDirectory() / "shaders");
        GpuScene scene(device);
        Gate gate(device, shaders, quality, scene, out, frames);
        std::vector<std::string> resolutions = resolution == "both" ? std::vector<std::string>{ "4K", "1440p" } : std::vector<std::string>{ resolution };
        std::vector<std::string> cases = which == "all" ? std::vector<std::string>{ "decals", "surface", "debug", "debugcap", "volume", "hair" } : std::vector<std::string>{ which };
        for (const std::string& c : cases)
            for (const std::string& rn : resolutions)
            {
                const Resolution res = resolutionFromString(rn, quality);
                if (c == "decals") caseDecals(gate, res);
                else if (c == "surface") caseSurface(gate, res);
                else if (c == "debug") caseDebug(gate, res, false);
                else if (c == "debugcap") caseDebug(gate, res, true);
                else if (c == "volume") caseVolume(gate, res);
                else if (c == "hair") caseHair(gate, res);
                else fail("unknown case %s", c.c_str());
                std::fflush(stdout);
            }
        if (debugLayer) std::printf("debug-layer errors: %u\n", device.drainDebugMessages());
        std::printf("e gate done\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
}
