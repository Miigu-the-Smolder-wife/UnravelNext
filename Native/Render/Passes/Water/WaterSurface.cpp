// Water surface shading, stage 1 (track W). See include/unx/water/WaterSurface.h and WaterSurface.hlsli.
#include "unx/water/WaterSurface.h"
#include "unx/water/FluidSurface.h"
#include "unx/water/LinearDispatch.h"

#include "unx/core/Log.h"
#include "unx/render/Device.h"
#include "unx/render/Shaders.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace unx::water
{
using namespace unx::render;
namespace
{
constexpr uint32_t kStatCount = 16, kStatBytes = 64, kRing = 4, kSlots = 64, kRayJobs = 1u << 20;
constexpr uint32_t kSecondaryViews = 8;  // views without identity (planar reflection views) whose water a frame shades
// After the slot rows: the refraction source's pyramid, count + SRV per level (WaterSurface.hlsli WATER_LEVELS_OFFSET).
// Then the calm-water block (WATER_PLANAR_OFFSET): 48 B per candidate plane.
// Then the sea's block (OceanShading.hlsli, WATER_OCEAN_OFFSET): 96 B.
constexpr uint32_t kMaxLevels = 15, kPlanarMax = 4, kOceanOffset = kSlots * 16 + 4 * (1 + kMaxLevels) + 48 * kPlanarMax, kOceanBytes = 96;
// Then the streams' flow block (WaterFlow.hlsli, WATER_FLOW_OFFSET): 80 B per slot.
constexpr uint32_t kFlowOffset = kOceanOffset + kOceanBytes, kFlowBytes = 80;
constexpr uint32_t kTableBytes = kFlowOffset + kSlots * kFlowBytes;
// The cost rule's terms [measured, RTX 4080, 4K W gate (interior scene, basin 3 m / 12 m: 506,640 / 892,079 water
// samples), sums of pass medians over 300 frames, camera on vs off, 2026-09-27]:
//   saved per sample served by the camera (its reflection job in R's ray passes + its share of the surface passes):
//     2.80 ns (3 m: rays 2.214 -> 1.098 ms, surface 4.182 -> 3.875) and 3.31 ns (12 m: 3.459 -> 1.375, 6.608 -> 5.735);
//   the camera's passes (mask, V, M resolve, S planar froxels, M shading): 1.85 ms at 511,000 mask pixels, 2.16 ms at
//     900,758, i.e. 1.44 ms + 0.80 ns per mask pixel; 0.67 ms of the fixed part is S's planar froxel integrate.
// Timestamps around the camera's passes read 3.2 ms at 900,758 pixels (the span also covers other queues' overlapping
// work), so they are not its marginal cost; the terms are re-measured with the gate when the camera's passes change.
constexpr double kPlanarSavedNs = 3.0, kPlanarFixedNs = 1.44e6, kPlanarPixelNs = 0.80;

ComPtr<ID3D12Resource> hostBuffer(Device& device, uint64_t bytes, D3D12_HEAP_TYPE type, const wchar_t* name, uint8_t** mapped)
{
    D3D12_HEAP_PROPERTIES heap{ type };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)), "water surface buffer");
    r->SetName(name);
    D3D12_RANGE none{ 0, 0 };
    check(r->Map(0, type == D3D12_HEAP_TYPE_READBACK ? nullptr : &none, reinterpret_cast<void**>(mapped)), "map water surface buffer");
    return r;
}
// The frame's slot table (upload ring: its SRV indices are known only when the pass executes) and the statistics
// readback ring.
struct SurfaceState
{
    Device* device = nullptr;
    ComPtr<ID3D12Resource> table[kRing], readback;
    ComPtr<ID3D12CommandSignature> signature;  // DispatchIndirect (the record pass over coverageSpecial's header)
    uint8_t* tableMapped[kRing] = {};
    uint8_t* readbackMapped = nullptr;
    uint32_t tableSrv[kRing] = {};
    uint64_t frame[kRing] = { UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX }, last = UINT64_MAX;
    uint32_t planarStream[kRing][kPlanarMax] = {};  // each ring frame's candidate k -> stream index (the counts' owner)
    uint32_t planarViews[kRing] = {};
    ~SurfaceState()
    {
        if (!device) return;
        for (uint32_t i = 0; i < kRing; ++i)
        {
            table[i]->Unmap(0, nullptr);
            device->deferRelease(table[i]);
            device->descriptors().freeResource(tableSrv[i]);
        }
        readback->Unmap(0, nullptr);
        device->deferRelease(readback);
    }
    void ensure(Device& d)
    {
        if (device) return;
        device = &d;
        for (uint32_t i = 0; i < kRing; ++i)
        {
            table[i] = hostBuffer(d, kTableBytes, D3D12_HEAP_TYPE_UPLOAD, L"water slot table", &tableMapped[i]);
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DXGI_FORMAT_R32_TYPELESS;
            sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Buffer.NumElements = kTableBytes / 4;
            sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            tableSrv[i] = d.descriptors().allocateResource();
            d.d3d()->CreateShaderResourceView(table[i].Get(), &sd, d.descriptors().resourceCpu(tableSrv[i]));
        }
        readback = hostBuffer(d, kRing * kStatBytes, D3D12_HEAP_TYPE_READBACK, L"water surface statistics", &readbackMapped);
        D3D12_INDIRECT_ARGUMENT_DESC arg{};
        arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12_COMMAND_SIGNATURE_DESC sig{};
        sig.ByteStride = sizeof(D3D12_DISPATCH_ARGUMENTS);
        sig.NumArgumentDescs = 1;
        sig.pArgumentDescs = &arg;
        check(d.d3d()->CreateCommandSignature(&sig, nullptr, IID_PPV_ARGS(&signature)), "water surface dispatch signature");
    }
};
} // namespace

void addWaterPlane(FramePassContext& fc, const WaterPlane& plane)
{
    WaterPlanes& planes = fc.state<WaterPlanes>("W.planes");
    if (planes.frame != fc.frame.frameIndex)
    {
        planes.frame = fc.frame.frameIndex;
        planes.list.clear();
    }
    planes.list.push_back(plane);
}

namespace
{
// The frame's count of views without identity (planar reflection views) whose water was shaded: each takes a state of
// its own (the slot table's upload ring is written when its passes execute).
struct SecondaryCount
{
    uint64_t frame = UINT64_MAX;
    uint32_t used = 0;
};
} // namespace

// The main view; and (shading.water_secondary_views) the frame's other views with a water layer - A14 views and planar
// reflection views: a pool seen in a mirror is water there too. Their samples take the stage 1 terms: no reflection
// camera of their own and no ray jobs (R's service traces for the main view), so the mirror lobe is the GI source's and
// a refracted ray that is not followed takes the straight view's stand-in; their water is not a medium of their air
// (the closed-form scattering instead); and where the view has no coverage records the interior pass shades the layer's
// edge pixels too, one sample each. The sea is the main view's alone (the view grid is its camera's).
void waterSurface(FramePassContext& fc, ViewResources& view)
{
    const bool mainView = view.view.kind == gpu::ViewKind::Main;
    if (!mainView && fc.quality.has("shading.water_secondary_views") && !fc.quality.boolean("shading.water_secondary_views")) return;
    RenderGraph& g = fc.graph;
    const FrameResources& r = fc.resources;
    // W's streams (fluids: triangle streams with vertices; slot = index in the frame's list, 63 is the sea's).
    std::vector<uint32_t> slots;
    for (uint32_t i = 0; i < (uint32_t)r.triangleStreams.size() && i < kSlots - 1; ++i)
        if (r.triangleStreams[i].vertices.valid()) slots.push_back(i);
    const bool interiorPass = view.waterVis.valid() && view.waterDepth.valid();
    const bool recordPass = view.coverageSpecial.valid() && view.coverageRecordRadiance.valid() && view.coverageRecords.valid() && view.coverageTileList.valid();
    // B7: the sea of this frame (W's waterGeometry: the view grid's surface, V's water layer holds its pixels as
    // COV_OCEAN_ID) is shaded by its own kernel in the interior pass's bands (WaterOcean.hlsl).
    const OceanSurfaceFrame sea = fc.trackState ? fc.state<OceanSurfaceFrame>("W.oceanFrame") : OceanSurfaceFrame{};
    const bool ocean = mainView && interiorPass && sea.frame == fc.frame.frameIndex && sea.surface.valid() && r.oceanDepth.valid();
    const bool anyStreams = !slots.empty();
    if ((!anyStreams && !ocean) || (!interiorPass && !recordPass)) return;
    if (!fc.trackState) fail("W: water surface shading needs the renderer's track state");
    const uint32_t W = view.view.width, H = view.view.height;
    const TextureRef colour = view.color, bandARadiance = view.bandARadiance;
    if (interiorPass)
    {
        const DXGI_FORMAT format = g.desc(colour).format;
        if (format != DXGI_FORMAT_R16G16B16A16_FLOAT && format != DXGI_FORMAT_R32G32B32A32_FLOAT)
        {
            if (!mainView) return;  // (a view shaded straight into a display target: its water is left out)
            fail("W: the water layer needs the shaded colour as exposed linear float (M postActive), got format %u", (unsigned)format);
        }
    }
    std::string stateKey = "W.surface";
    if (!mainView)
    {
        if (view.viewId != 0) stateKey += ".view" + std::to_string(view.viewId);
        else
        {
            SecondaryCount& n = fc.state<SecondaryCount>("W.surface.secondary");
            if (n.frame != fc.frame.frameIndex)
            {
                n.frame = fc.frame.frameIndex;
                n.used = 0;
            }
            if (n.used == kSecondaryViews) return;  // (more such views than states: the others show no water)
            stateKey += ".planar" + std::to_string(n.used++);
        }
    }
    SurfaceState& st = fc.state<SurfaceState>(stateKey);
    st.ensure(fc.device);
    // (the tests' switches and the reflection cameras are the main view's)
    WaterSurfaceDebug unused;
    unused.planar = 0;
    WaterSurfaceDebug& debug = mainView ? fc.state<WaterSurfaceDebug>("W.surface.debug") : unused;
    const uint32_t ring = uint32_t(fc.frame.frameIndex % kRing);
    const bool shadeEdges = !mainView && !recordPass;  // (WaterInterior.hlsl P[7].w: no records to shade the layer's edges)

    // The refraction source: band A's shaded radiance without the particle layer (M keeps it in bandARadiance), copied
    // (the interior pass overwrites the water pixels of the texture it reads).
    const TextureRef from = bandARadiance.valid() ? bandARadiance : colour;
    const TextureDesc fromDesc = g.desc(from);
    const TextureRef source = g.createTexture(TextureDesc{ "w.surface.source", fromDesc.width, fromDesc.height, 1, 1, fromDesc.format });
    g.addPass("w.surface.source", QueueType::Graphics, [&](PassBuilder& b) { b.use(from, Use::CopySrc); b.use(source, Use::CopyDst); },
              [=](PassContext& c) { c.cmd->CopyResource(c.resource(source), c.resource(from)); });
    // Its box pyramid (WaterSourceMips.hlsl; WaterFootprint.hlsli integrates the refracted pixel footprint over it): level
    // k + 1 = the 2 x 2 means of level k, down to 1 x 1 (at most kMaxLevels levels, the source being level 0).
    std::vector<TextureRef> levels{ source };
    {
        ID3D12PipelineState* mips = fc.shaders.compute("Passes/Water/WaterSourceMips");
        uint32_t lw = fromDesc.width, lh = fromDesc.height;
        while ((lw > 1 || lh > 1) && levels.size() < kMaxLevels)
        {
            lw = std::max(1u, (lw + 1) / 2);
            lh = std::max(1u, (lh + 1) / 2);
            const TextureRef prev = levels.back(), next = g.createTexture(TextureDesc{ "w.surface.source level", lw, lh, 1, 1, fromDesc.format });
            const uint32_t w2 = lw, h2 = lh;
            g.addPass("w.surface.source level", QueueType::Graphics, [&](PassBuilder& b) { b.use(prev, Use::SrvCompute); b.use(next, Use::UavCompute); },
                      [=](PassContext& c) {
                          const uint32_t k[4] = { c.srv(prev), c.uav(next), w2, h2 };
                          c.cmd->SetPipelineState(mips);
                          c.computeConstants(k, 4);
                          c.cmd->Dispatch((w2 + 7) / 8, (h2 + 7) / 8, 1);
                      });
            levels.push_back(next);
        }
    }
    const BufferRef stats = g.createBuffer({ "w.surface.stats", kStatBytes, 0 });
    ID3D12PipelineState* clear = fc.shaders.compute("Passes/Water/ViewGridClear");
    g.addPass("w.surface.stats clear", QueueType::Graphics, [&](PassBuilder& b) { b.use(stats, Use::UavCompute); },
              [=](PassContext& c) {
                  const uint32_t k[4] = { 0, 0, c.uav(stats), kStatBytes / 4 };  // ViewGridClear: the counter words only
                  c.cmd->SetPipelineState(clear);
                  c.computeConstants(k, 4);
                  c.cmd->Dispatch(1, 1, 1);
              });
    TextureRef status, marchImage;
    if (debug.status && interiorPass)
    {
        status = g.createTexture(TextureDesc{ "w.surface.status", W, H, 1, 1, DXGI_FORMAT_R8_UINT });
        debug.image = status;
        marchImage = g.createTexture(TextureDesc{ "w.surface.march", W, H, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT });
        debug.march = marchImage;
        ID3D12PipelineState* zero = fc.shaders.compute("Passes/Water/WaterStatusClear");
        g.addPass("w.surface.status clear", QueueType::Graphics, [&](PassBuilder& b) { b.use(status, Use::UavCompute); },
                  [=](PassContext& c) {
                      const uint32_t k[4] = { c.uav(status), W, H, 0 };
                      c.cmd->SetPipelineState(zero);
                      c.computeConstants(k, 4);
                      c.cmd->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
                  });
        debug.status = false;
    }
    std::vector<BufferRef> vertices;
    std::vector<uint32_t> materials;
    for (uint32_t s : slots)
    {
        vertices.push_back(r.triangleStreams[s].vertices);
        materials.push_back(r.triangleStreams[s].material);
    }
    const TextureRef vis = view.waterVis, waterDepth = view.waterDepth, depth = view.depth;

    // Calm water (A14; WaterSurface.h WaterPlane): each candidate plane's mask over its rectangle (always: its pixel count
    // is the cost rule's input), and its reflection camera where the rule picks it: C_planar = fixed + drawn pixels x
    // ns/px against the reflection jobs and surface work it replaces (kPlanar*: measured), from the mask counts of the
    // frame that completed last (a new candidate is drawn: no count yet).
    struct PlanarUse
    {
        uint32_t stream, k, rect[4];
        float4 plane;
        TextureRef mask, colour, depth;
    };
    std::vector<PlanarUse> planar;
    uint32_t planarViews = 0;
    if (interiorPass && debug.planar != 0 && fc.services.renderView)
    {
        WaterPlanes& wp = fc.state<WaterPlanes>("W.planes");
        if (wp.frame != fc.frame.frameIndex) wp.list.clear();
        if (fc.framesInFlight >= kRing) fail("W: %u frames in flight need more statistics ring slots", fc.framesInFlight);
        const uint64_t done = fc.frame.frameIndex >= fc.framesInFlight ? fc.frame.frameIndex - fc.framesInFlight : UINT64_MAX;
        const uint32_t doneRing = uint32_t(done % kRing);
        const bool history = done != UINT64_MAX && st.frame[doneRing] == done;
        uint32_t counts[kStatCount] = {};
        if (history) std::memcpy(counts, st.readbackMapped + doneRing * kStatBytes, sizeof counts);
        const float4x4& vp = view.view.viewProj;
        const float3 eye = view.view.position;
        ID3D12PipelineState* pass0 = fc.shaders.compute("Passes/Water/WaterPlanarMask.PASS0");
        ID3D12PipelineState* pass1 = fc.shaders.compute("Passes/Water/WaterPlanarMask.PASS1");
        for (const WaterPlane& p : wp.list)
        {
            if (std::find(slots.begin(), slots.end(), p.stream) == slots.end() || planar.size() == kPlanarMax) continue;
            const BufferRef streamVertices = r.triangleStreams[p.stream].vertices;
            // The plane's normal is the air side's: a camera under it sees the water from inside (not this path).
            const float len = length(float3{ p.plane.x, p.plane.y, p.plane.z });
            const float4 plane{ p.plane.x / len, p.plane.y / len, p.plane.z / len, p.plane.w / len };
            if (plane.x * eye.x + plane.y * eye.y + plane.z * eye.z + plane.w <= 0) continue;
            // The rectangle: the region's corners projected (a corner behind the eye takes the whole view), 1 px apron.
            float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
            bool behind = false;
            for (const float3& c : p.corners)
            {
                float h[4];
                for (int row = 0; row < 4; ++row) h[row] = vp.m[row][0] * c.x + vp.m[row][1] * c.y + vp.m[row][2] * c.z + vp.m[row][3];
                if (h[3] <= 1e-6f)
                {
                    behind = true;
                    break;
                }
                const float px = (h[0] / h[3] * 0.5f + 0.5f) * W, py = (0.5f - h[1] / h[3] * 0.5f) * H;
                x0 = std::min(x0, px), x1 = std::max(x1, px), y0 = std::min(y0, py), y1 = std::max(y1, py);
            }
            if (behind) x0 = 0, y0 = 0, x1 = (float)W, y1 = (float)H;
            const int ix0 = std::max(0, (int)std::floor(x0) - 1), iy0 = std::max(0, (int)std::floor(y0) - 1);
            int ix1 = std::min((int)W, (int)std::ceil(x1) + 1), iy1 = std::min((int)H, (int)std::ceil(y1) + 1);
            if (ix1 <= ix0 || iy1 <= iy0) continue;
            // The size in 64-pixel steps (within the view): the reflection camera's chain takes it, and the render graph's
            // plan key its textures' sizes (the mask keeps the camera to the water's pixels).
            ix1 = std::min((int)W, ix0 + (ix1 - ix0 + 63) / 64 * 64);
            iy1 = std::min((int)H, iy0 + (iy1 - iy0 + 63) / 64 * 64);
            PlanarUse u{ p.stream, (uint32_t)planar.size(), { (uint32_t)ix0, (uint32_t)iy0, (uint32_t)(ix1 - ix0), (uint32_t)(iy1 - iy0) }, plane };
            const uint32_t rw = u.rect[2], rh = u.rect[3], k = u.k;
            const TextureRef scratch = g.createTexture(TextureDesc{ "w.planar scratch", rw, rh, 1, 1, DXGI_FORMAT_R8_UINT });
            const TextureRef tiles = g.createTexture(TextureDesc{ "w.planar tile mask", (rw + 7) / 8, (rh + 7) / 8, 1, 1, DXGI_FORMAT_R8_UINT });
            u.mask = g.createTexture(TextureDesc{ "w.planar mask", rw, rh, 1, 1, DXGI_FORMAT_R8_UINT });
            const TextureRef mask = u.mask;
            uint32_t k0[16] = {};
            std::memcpy(&k0[4], &plane, 16);
            std::memcpy(&k0[8], u.rect, 16);
            k0[12] = p.stream, k0[13] = k;
            const D3D12_GPU_VIRTUAL_ADDRESS mainCb = view.frameConstants;
            g.addPass("w.planar mask", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          b.use(vis, Use::SrvCompute);
                          b.use(streamVertices, Use::SrvCompute);
                          b.use(scratch, Use::UavCompute);
                          b.use(stats, Use::UavCompute);
                      },
                      [=](PassContext& c) {
                          uint32_t kk[16];
                          std::memcpy(kk, k0, sizeof kk);
                          kk[0] = c.srv(vis), kk[1] = c.srv(streamVertices), kk[2] = c.uav(scratch), kk[3] = c.uav(stats);
                          c.cmd->SetPipelineState(pass0);
                          c.bindFrameConstants(mainCb);
                          c.computeConstants(kk, 16);
                          c.cmd->Dispatch((rw + 7) / 8, (rh + 7) / 8, 1);
                      });
            g.addPass("w.planar mask apron", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          b.use(scratch, Use::SrvCompute);
                          b.use(mask, Use::UavCompute);
                          b.use(tiles, Use::UavCompute);
                      },
                      [=](PassContext& c) {
                          uint32_t kk[16];
                          std::memcpy(kk, k0, sizeof kk);
                          kk[0] = c.srv(scratch), kk[1] = c.uav(mask), kk[2] = c.uav(tiles), kk[3] = gpu::kNone;
                          c.cmd->SetPipelineState(pass1);
                          c.bindFrameConstants(mainCb);
                          c.computeConstants(kk, 16);
                          c.cmd->Dispatch((rw + 7) / 8, (rh + 7) / 8, 1);
                      });
            const bool known = history && st.planarStream[doneRing][k] == p.stream;
            st.planarStream[ring][k] = p.stream;
            const double pixels = known ? counts[12 + k] : 0;
            if (debug.planar == 1 || !known || pixels * kPlanarSavedNs > kPlanarFixedNs + pixels * kPlanarPixelNs)
            {
                ViewDesc d = ViewDesc::planarReflection(view.view, plane, u.rect[0], u.rect[1], rw, rh);
                d.planarMask = mask;
                d.planarTileMask = tiles;
                const ViewResources reflection = fc.services.renderView(fc, d);
                if (!reflection.color.valid() || !reflection.depth.valid()) fail("W: the reflection camera returned no colour or depth");
                u.colour = reflection.color;
                u.depth = reflection.depth;
                ++planarViews;
            }
            planar.push_back(u);  // without a camera: the mask's count only
        }
    }
    st.planarViews[ring] = planarViews;
    // The streams whose water is a medium of the view's air volume this frame (slot row word 3; WaterSurface.hlsli
    // waterSlotMedium): the basins S took (FroxelSystem.cpp recordWaterMedia) among the basins' streams (PoolTrack.cpp).
    uint64_t mediumSlots = 0;
    if (mainView)
    {
        const std::vector<uint64_t>& taken = fc.state<std::vector<uint64_t>>("W.mediaPools");
        const std::vector<uint64_t>& streams = fc.state<std::vector<uint64_t>>("W.poolStreams");
        if (!taken.empty() && !streams.empty() && taken[0] == fc.frame.frameIndex && streams[0] == fc.frame.frameIndex)
            for (size_t i = 1; i < streams.size(); ++i)
                if ((streams[i] >> 32) < kSlots && std::find(taken.begin() + 1, taken.end(), streams[i] & 0xFFFFFFFFull) != taken.end())
                    mediumSlots |= 1ull << (streams[i] >> 32);
    }
    const TextureRef particleLayer = view.particleLayer;
    const BufferRef particleEdges = view.particleEdges;
    uint8_t* tableMapped = st.tableMapped[ring];
    const uint32_t tableSrv = st.tableSrv[ring];
    const D3D12_GPU_VIRTUAL_ADDRESS cb = view.frameConstants;
    const uint32_t none = gpu::kNone;
    // Inputs both passes read (the shading function's), declared and bound alike.
    auto shadingUses = [&, vertices, levels, planar](PassBuilder& b) {
        for (const TextureRef& l : levels) b.use(l, Use::SrvCompute);
        b.use(depth, Use::SrvCompute);
        if (vis.valid()) b.use(vis, Use::SrvCompute);
        if (waterDepth.valid()) b.use(waterDepth, Use::SrvCompute);
        b.use(stats, Use::UavCompute);
        for (const BufferRef& v : vertices) b.use(v, Use::SrvCompute);
        declareGiSource(b, giSource(r), Use::SrvCompute);
        for (const TextureRef& t : { r.transmittanceLut, r.multiScatterLut, view.airVolume })
            if (t.valid()) b.use(t, Use::SrvCompute);
        declareFog(b, r, Use::SrvCompute);
        for (const BufferRef& x : { r.vsmPageTable, r.vsmBlocks, r.vsmSearchBound, r.vsmLayers })
            if (x.valid()) b.use(x, Use::SrvCompute);
        if (r.vsmAtlas.valid()) b.use(r.vsmAtlas, Use::SrvCompute);
        for (const PlanarUse& u : planar)
            if (u.colour.valid())
                for (const TextureRef& t : { u.colour, u.depth, u.mask }) b.use(t, Use::SrvCompute);
        if (ocean)
            for (const TextureRef& t : { sea.surface, sea.displacement, sea.slopes, sea.foam, r.skyViewLut })
                if (t.valid()) b.use(t, Use::SrvCompute);
    };
    struct Frame
    {
        GiSource gi;
        BufferRef pageTable, blocks, searchBound, layers;
        TextureRef transmittance, multiScatter, airVolume;
        uint32_t vsmConstants;
    };
    const Frame fr{ giSource(r), r.vsmPageTable, r.vsmBlocks, r.vsmSearchBound, r.vsmLayers, r.transmittanceLut, r.multiScatterLut, view.airVolume, r.vsmConstants };
    // the sea's block: its mirror rays' reach and the shore's foam from the quality file
    const QualityConfig& q = fc.quality;
    const float seaRayReach = q.has("shading.water_ocean_ray_distance_m") ? (float)q.number("shading.water_ocean_ray_distance_m") : 400.0f;
    const float shoreFoam = q.has("shading.water_shore_foam") ? (float)q.number("shading.water_shore_foam") : 0.8f;
    const float shoreFoamDepth = q.has("shading.water_shore_foam_depth_m") ? (float)q.number("shading.water_shore_foam_depth_m") : 0.3f;
    if (!(seaRayReach >= 0) || !(shoreFoam >= 0 && shoreFoam <= 1) || !(shoreFoamDepth > 0))
        fail("shading: water_ocean_ray_distance_m >= 0, water_shore_foam in [0, 1], water_shore_foam_depth_m > 0");
    const TextureRef skyView = r.skyViewLut;
    // the basins' flow rows of this frame (PoolTrack.cpp)
    const WaterFlows& flowState = fc.state<WaterFlows>("W.poolFlows");
    const std::vector<WaterFlowRow> flowRows = flowState.frame == fc.frame.frameIndex ? flowState.rows : std::vector<WaterFlowRow>{};
    // P[0..4] of both kernels (WaterInterior.hlsl, WaterRecords.hlsl); the first execute also fills the slot table.
    auto shadingConstants = [=](PassContext& c, uint32_t k[24], uint32_t first) {
        if (first)
        {
            std::memset(tableMapped, 0xFF, kSlots * 16);  // UNX_NONE: not a W stream
            for (size_t i = 0; i < slots.size(); ++i)
            {
                const uint32_t row[2] = { c.srv(vertices[i]), materials[i] };
                std::memcpy(tableMapped + 16 * slots[i], row, 8);
                const uint32_t medium = (mediumSlots >> slots[i] & 1ull) != 0 ? 1u : gpu::kNone;
                std::memcpy(tableMapped + 16 * slots[i] + 12, &medium, 4);
            }
            uint32_t pyramid[1 + kMaxLevels] = { uint32_t(levels.size()) };
            for (size_t l = 0; l < levels.size(); ++l) pyramid[1 + l] = c.srv(levels[l]);
            std::memcpy(tableMapped + kSlots * 16, pyramid, sizeof pyramid);
            // Calm water: slot row word 2 = candidate k with a camera; the planar block (WaterSurface.hlsli).
            for (const PlanarUse& u : planar)
            {
                if (!u.colour.valid()) continue;
                std::memcpy(tableMapped + 16 * u.stream + 8, &u.k, 4);
                uint8_t* row = tableMapped + kSlots * 16 + 4 * (1 + kMaxLevels) + 48 * u.k;
                const uint32_t srv[4] = { c.srv(u.colour), c.srv(u.depth), c.srv(u.mask), 0 };
                std::memcpy(row, &u.plane, 16);
                std::memcpy(row + 16, u.rect, 16);
                std::memcpy(row + 32, srv, 16);
            }
            // The sea's block (OceanShading.hlsli); zero: no sea this frame.
            uint32_t block[kOceanBytes / 4] = {};
            if (ocean)
            {
                block[0] = c.srv(sea.surface), block[1] = c.srv(sea.displacement), block[2] = c.srv(sea.slopes);
                block[3] = sea.foam.valid() ? c.srv(sea.foam) : gpu::kNone;
                block[4] = sea.foamParams, block[5] = sea.material, block[6] = 1;
                const float numbers[14] = { sea.lengths[0], sea.lengths[1], sea.lengths[2], 0, 0, 0, sea.alpha, sea.peakWavenumber,
                                            sea.finestWavenumber, sea.windSpeed, seaRayReach, shoreFoamDepth, shoreFoam, 0.85f };
                std::memcpy(&block[8], numbers, sizeof numbers);
            }
            std::memcpy(tableMapped + kOceanOffset, block, sizeof block);
            // The streams' flow (WaterFlow.hlsli); zero (no wave length): the stream has none.
            std::memset(tableMapped + kFlowOffset, 0, kSlots * kFlowBytes);
            for (const WaterFlowRow& row : flowRows)
                if (row.stream < kSlots) std::memcpy(tableMapped + kFlowOffset + row.stream * kFlowBytes, row.values, kFlowBytes);
        }
        const bool shadows = fr.pageTable.valid() && fr.vsmConstants != UINT32_MAX;
        const uint32_t values[20] = { first, c.srv(source), c.srv(depth), c.uav(stats),
                                      vis.valid() ? c.srv(vis) : none, waterDepth.valid() ? c.srv(waterDepth) : none, tableSrv,
                                      giSourceWord(c, fr.gi),
                                      fr.transmittance.valid() ? c.srv(fr.transmittance) : none, fr.multiScatter.valid() ? c.srv(fr.multiScatter) : none,
                                      fr.airVolume.valid() ? c.srv(fr.airVolume) : none, none,
                                      shadows ? c.srv(fr.pageTable) : none, shadows ? c.srv(fr.blocks) : none, shadows && fr.searchBound.valid() ? c.srv(fr.searchBound) : none,
                                      shadows ? fr.vsmConstants : none, shadows && fr.layers.valid() ? c.srv(fr.layers) : none, none, none, none };
        std::memcpy(k, values, sizeof values);
        k[20] = k[21] = k[22] = k[23] = none;
    };

    // Stage 3: R's ray service. Lists sized for a band of rows (2 jobs per pixel: a reflection and, for a fallback, a
    // refraction) and reused by the record rounds; without the service nothing is written and each pass runs once.
    ID3D12CommandSignature* signature = st.signature.Get();
    const bool rays = mainView && static_cast<bool>(fc.services.traceRefractions) && (!interiorPass || bandARadiance.valid());
    const uint32_t jobCapacity = std::max(debug.rayJobCapacity ? debug.rayJobCapacity : kRayJobs, 2 * W);
    const uint32_t bandRows = rays ? std::min(H, jobCapacity / (2 * W)) : H, sampleCapacity = W * bandRows, jobs = 2 * sampleCapacity;
    BufferRef jobList, results, samples, applyArgs;
    ID3D12PipelineState* heads = rays ? fc.shaders.compute("Passes/Water/ViewGridClear") : nullptr;
    ID3D12PipelineState* argsKernel = rays ? fc.shaders.compute("Passes/Water/WaterRayArgs") : nullptr;
    ID3D12PipelineState* applyKernel = rays ? fc.shaders.compute("Passes/Water/WaterRayApply") : nullptr;
    if (rays)
    {
        jobList = g.createBuffer({ "w.surface.ray jobs", 16 + 48ull * jobs, 0 });
        results = g.createBuffer({ "w.surface.ray results", 8ull * jobs, 0 });
        samples = g.createBuffer({ "w.surface.ray samples", 16 + 80ull * sampleCapacity, 0 });  // (WATER_RAY_SAMPLE_BYTES)
        applyArgs = g.createBuffer({ "w.surface.ray apply args", 16, 0 });
    }
    auto clearLists = [&] {
        g.addPass("w.surface.ray clear", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(jobList, Use::UavCompute);
                      b.use(samples, Use::UavCompute);
                  },
                  [=](PassContext& c) {
                      c.cmd->SetPipelineState(heads);
                      const uint32_t a[4] = { 0, 0, c.uav(jobList), 4 }, s[4] = { 0, 0, c.uav(samples), 4 };  // the heads only
                      c.computeConstants(a, 4);
                      c.cmd->Dispatch(1, 1, 1);
                      c.computeConstants(s, 4);
                      c.cmd->Dispatch(1, 1, 1);
                  });
    };
    // After a band's or round's shading pass: the apply dispatch's size, R's rays, the apply pass (pixels: band A radiance
    // and colour; records: coverageRecordRadiance).
    auto traceAndApply = [&](BufferRef recordRadiance) {
        g.addPass("w.surface.ray args", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(samples, Use::UavCompute);
                      b.use(applyArgs, Use::UavCompute);
                  },
                  [=](PassContext& c) {
                      const uint32_t k[4] = { c.uav(samples), c.uav(applyArgs), sampleCapacity, 0 };
                      c.cmd->SetPipelineState(argsKernel);
                      c.computeConstants(k, 4);
                      c.cmd->Dispatch(1, 1, 1);
                  });
        fc.services.traceRefractions(fc, jobList, results, jobs);
        const bool pixels = !recordRadiance.valid();
        g.addPass("w.surface.ray apply", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(samples, Use::SrvCompute);
                      b.use(applyArgs, Use::IndirectArgs);
                      b.use(results, Use::SrvCompute);
                      b.use(stats, Use::UavCompute);
                      if (pixels)
                      {
                          b.use(bandARadiance, Use::UavCompute);
                          b.use(colour, Use::UavCompute);
                          if (particleLayer.valid()) b.use(particleLayer, Use::SrvCompute);
                          if (particleEdges.valid()) b.use(particleEdges, Use::SrvCompute);
                      }
                      else b.use(recordRadiance, Use::UavCompute);
                  },
                  [=](PassContext& c) {
                      const uint32_t k[8] = { c.srv(samples), c.srv(results), pixels ? c.uav(bandARadiance) : none, pixels ? c.uav(colour) : none,
                                              pixels && particleLayer.valid() ? c.srv(particleLayer) : none,
                                              pixels && particleLayer.valid() && particleEdges.valid() ? c.srv(particleEdges) : none, c.uav(stats),
                                              pixels ? none : c.uav(recordRadiance) };
                      c.cmd->SetPipelineState(applyKernel);
                      c.computeConstants(k, 8);
                      c.cmd->ExecuteIndirect(signature, 1, c.resource(applyArgs), 0, nullptr, 0);
                  });
    };
    uint32_t rounds = 0;

    if (interiorPass)
    {
        ID3D12PipelineState* kernel = fc.shaders.compute("Passes/Water/WaterInterior");
        ID3D12PipelineState* seaKernel = ocean ? fc.shaders.compute("Passes/Water/WaterOcean") : nullptr;
        uint32_t bands = 0;
        for (uint32_t row0 = 0; row0 < H; row0 += bandRows, ++bands)
        {
            const uint32_t rows = std::min(bandRows, H - row0), first = row0 == 0 ? 1 : 0;
            if (rays) clearLists();
            // the sea's pixels of the band (every one: the sea has no edge records), into the band's lists with the
            // streams' (a pixel is a stream's or the sea's: the band's capacities hold both)
            if (ocean)
                g.addPass("w.surface.ocean", QueueType::Graphics,
                          [&](PassBuilder& b) {
                              shadingUses(b);
                              b.use(colour, Use::UavCompute);
                              if (bandARadiance.valid()) b.use(bandARadiance, Use::UavCompute);
                              if (status.valid()) b.use(status, Use::UavCompute);
                              if (particleLayer.valid()) b.use(particleLayer, Use::SrvCompute);
                              if (particleEdges.valid()) b.use(particleEdges, Use::SrvCompute);
                              if (rays)
                                  for (const BufferRef& x : { jobList, results, samples }) b.use(x, Use::UavCompute);
                          },
                          [=](PassContext& c) {
                              uint32_t k[32];
                              shadingConstants(c, k, first);  // (the band's first pass: it fills the slot table)
                              k[0] = c.uav(colour);
                              k[11] = status.valid() ? c.uav(status) : none;
                              k[20] = bandARadiance.valid() ? c.uav(bandARadiance) : none;
                              k[21] = particleLayer.valid() ? c.srv(particleLayer) : none;
                              k[22] = particleLayer.valid() && particleEdges.valid() ? c.srv(particleEdges) : none;
                              k[23] = skyView.valid() ? c.srv(skyView) : none;
                              k[24] = rays ? c.uav(jobList) : none;
                              k[25] = rays ? c.uav(results) : none;
                              k[26] = rays ? c.uav(samples) : none;
                              k[27] = jobs;
                              k[28] = row0, k[29] = rows, k[30] = sampleCapacity, k[31] = 0;
                              c.cmd->SetPipelineState(seaKernel);
                              c.bindFrameConstants(cb);
                              c.computeConstants(k, 32);
                              c.cmd->Dispatch((W + 7) / 8, (rows + 7) / 8, 1);
                          });
            if (anyStreams)
                g.addPass("w.surface.interior", QueueType::Graphics,
                          [&](PassBuilder& b) {
                              shadingUses(b);
                              b.use(colour, Use::UavCompute);
                              if (bandARadiance.valid()) b.use(bandARadiance, Use::UavCompute);
                              if (status.valid()) b.use(status, Use::UavCompute);
                              if (marchImage.valid()) b.use(marchImage, Use::UavCompute);
                              if (particleLayer.valid()) b.use(particleLayer, Use::SrvCompute);
                              if (particleEdges.valid()) b.use(particleEdges, Use::SrvCompute);
                              if (rays)
                                  for (const BufferRef& x : { jobList, results, samples }) b.use(x, Use::UavCompute);
                          },
                          [=](PassContext& c) {
                              uint32_t k[32];
                              shadingConstants(c, k, ocean ? 0u : first);  // (with a sea its pass came first and filled the table)
                              k[0] = c.uav(colour);
                              k[11] = status.valid() ? c.uav(status) : none;
                              k[17] = marchImage.valid() ? c.uav(marchImage) : none;
                              k[20] = bandARadiance.valid() ? c.uav(bandARadiance) : none;
                              k[21] = particleLayer.valid() ? c.srv(particleLayer) : none;
                              k[22] = particleLayer.valid() && particleEdges.valid() ? c.srv(particleEdges) : none;
                              k[24] = rays ? c.uav(jobList) : none;
                              k[25] = rays ? c.uav(results) : none;
                              k[26] = rays ? c.uav(samples) : none;
                              k[27] = jobs;
                              k[28] = row0, k[29] = rows, k[30] = sampleCapacity, k[31] = shadeEdges ? 1u : 0u;
                              c.cmd->SetPipelineState(kernel);
                              c.bindFrameConstants(cb);
                              c.computeConstants(k, 32);
                              c.cmd->Dispatch((W + 7) / 8, (rows + 7) / 8, 1);
                          });
            if (rays) traceAndApply(BufferRef{});
        }
        debug.rayBands = rays ? bands : 0;
    }
    if (recordPass && anyStreams)
    {
        ID3D12PipelineState* kernel = fc.shaders.compute("Passes/Water/WaterRecords");
        const BufferRef special = view.coverageSpecial, records = view.coverageRecords, tileList = view.coverageTileList, radiance = view.coverageRecordRadiance;
        // Without rays one indirect pass over the list's header; with them rounds of sampleCapacity entries over the list's
        // capacity (its count is known only on the GPU: rounds past it do nothing).
        const uint32_t specialEntries = uint32_t((g.desc(special).size / 4 - 4) / 2);  // (CoverageLayer.hlsli COV_SPECIAL_HEADER = 4)
        const uint32_t roundSize = rays ? std::max(1u, std::min(sampleCapacity, specialEntries)) : UINT32_MAX;
        for (uint32_t base = 0; base == 0 || (rays && base < specialEntries); base += roundSize, ++rounds)
        {
            const uint32_t fillsTable = !interiorPass && base == 0 ? 1 : 0;
            if (rays) clearLists();
            g.addPass("w.surface.records", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          shadingUses(b);
                          b.use(special, Use::SrvCompute);
                          if (!rays) b.use(special, Use::IndirectArgs);
                          b.use(records, Use::SrvCompute);
                          b.use(tileList, Use::SrvCompute);
                          b.use(radiance, Use::UavCompute);
                          if (rays)
                              for (const BufferRef& x : { jobList, results, samples }) b.use(x, Use::UavCompute);
                      },
                      [=](PassContext& c) {
                          uint32_t k[32];
                          shadingConstants(c, k, fillsTable);
                          k[0] = 0;
                          k[17] = c.srv(special);
                          k[18] = c.srv(records);
                          k[19] = c.srv(tileList);
                          k[20] = c.uav(radiance);
                          k[24] = rays ? c.uav(jobList) : none;
                          k[25] = rays ? c.uav(results) : none;
                          k[26] = rays ? c.uav(samples) : none;
                          k[27] = jobs;
                          k[28] = base, k[29] = roundSize, k[30] = sampleCapacity, k[31] = 0;
                          c.cmd->SetPipelineState(kernel);
                          c.bindFrameConstants(cb);
                          c.computeConstants(k, 32);
                          if (rays) dispatchLinear(c.cmd, (roundSize + 63) / 64);
                          else c.cmd->ExecuteIndirect(signature, 1, c.resource(special), 4, nullptr, 0);  // header words 1..3
                      });
            if (rays) traceAndApply(radiance);
        }
    }
    // The waterline at the lens (WaterLine.hlsl; shading.water_waterline): a basin whose still level the near plane's
    // rectangle can reach, with the camera over it - the surface meets the lens.
    if (mainView && interiorPass && (!q.has("shading.water_waterline") || q.boolean("shading.water_waterline")) && fc.frame.poolCount)
    {
        const float width = (q.has("shading.water_waterline_mm") ? (float)q.number("shading.water_waterline_mm") : 1.0f) * 1e-3f;
        const float strength = q.has("shading.water_waterline_strength") ? (float)q.number("shading.water_waterline_strength") : 0.6f;
        if (!(width > 0) || !(strength >= 0 && strength <= 1)) fail("shading: water_waterline_mm > 0, water_waterline_strength in [0, 1]");
        // (the near plane's corners lie this far from the eye at most; the ripples' and the meniscus' reach on top)
        const float tanX = 1.0f / view.view.proj.m[0][0], tanY = 1.0f / view.view.proj.m[1][1];
        const float reach = view.view.nearPlane * std::sqrt(1.0f + tanX * tanX + tanY * tanY) + 0.05f + 8 * width;
        const float3 eye = view.view.position;
        struct Line
        {
            float centre[3];
            TextureRef field;
            float cosYaw, sinYaw, sizeX, sizeZ;
        };
        std::vector<Line> lines;
        const std::vector<FluidSurfaceInput::Basin>& fields = fc.state<std::vector<FluidSurfaceInput::Basin>>("W.poolBasins");
        for (uint32_t i = 0; i < fc.frame.poolCount && lines.size() < 4; ++i)
        {
            const PoolFrame& p = fc.frame.pools[i];
            const float dx = eye.x - (float)p.centre[0], dy = eye.y - (float)p.centre[1], dz = eye.z - (float)p.centre[2];
            if (std::abs(dy) > reach) continue;
            const float c = std::cos(p.yaw), sn = std::sin(p.yaw);
            const float lx = dx * c - dz * sn, lz = dx * sn + dz * c;
            const bool round = p.shape == 1;
            if (round ? std::hypot(lx, lz) > 0.5f * p.sizeX + reach : (std::abs(lx) > 0.5f * p.sizeX + reach || std::abs(lz) > 0.5f * p.sizeZ + reach)) continue;
            Line line{ { (float)p.centre[0], (float)p.centre[1], (float)p.centre[2] }, TextureRef{}, c, sn, p.sizeX, round ? 0.0f : p.sizeZ };
            // (a rectangular basin's ripples: this frame's field, as the fluids' seam finds it - by its place and size)
            for (const FluidSurfaceInput::Basin& b : fields)
                if (!round && b.centre[0] == line.centre[0] && b.centre[1] == line.centre[1] && b.centre[2] == line.centre[2] && b.sizeX == p.sizeX && b.sizeZ == p.sizeZ)
                    line.field = b.field;
            lines.push_back(line);
        }
        if (!lines.empty())
        {
            ID3D12PipelineState* lineKernel = fc.shaders.compute("Passes/Water/WaterLine");
            g.addPass("w.surface.waterline", QueueType::Graphics,
                      [&](PassBuilder& b) {
                          b.use(colour, Use::UavCompute);
                          if (bandARadiance.valid()) b.use(bandARadiance, Use::UavCompute);
                          for (const Line& l : lines)
                              if (l.field.valid()) b.use(l.field, Use::SrvCompute);
                      },
                      [=](PassContext& c) {
                          uint32_t k[48] = {};
                          k[0] = c.uav(colour);
                          k[1] = bandARadiance.valid() ? c.uav(bandARadiance) : none;
                          k[2] = (uint32_t)lines.size();
                          std::memcpy(&k[4], &width, 4);
                          std::memcpy(&k[5], &strength, 4);
                          for (size_t i = 0; i < lines.size(); ++i)
                          {
                              const Line& l = lines[i];
                              std::memcpy(&k[8 + 8 * i], l.centre, 12);
                              k[8 + 8 * i + 3] = l.field.valid() ? c.srv(l.field) : none;
                              const float axes[4] = { l.cosYaw, l.sinYaw, l.sizeX, l.sizeZ };
                              std::memcpy(&k[12 + 8 * i], axes, 16);
                          }
                          c.cmd->SetPipelineState(lineKernel);
                          c.bindFrameConstants(cb);
                          c.computeConstants(k, 8 + 8 * (uint32_t)lines.size());
                          c.cmd->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
                      });
        }
    }
    ID3D12Resource* readback = st.readback.Get();
    g.addPass("w.surface.stats read", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(stats, Use::CopySrc);
                  b.keep();
              },
              [=](PassContext& c) { c.cmd->CopyBufferRegion(readback, ring * kStatBytes, c.resource(stats), 0, 4 * kStatCount); });
    debug.rayRounds = rounds;
    if (debug.copyRadiance && bandARadiance.valid())
    {
        // (M's edge composite, after tracks::water, rewrites band A radiance at outline pixels)
        const TextureDesc rd = g.desc(bandARadiance);
        const TextureRef copy = g.createTexture(TextureDesc{ "w.surface.radiance copy", rd.width, rd.height, 1, 1, rd.format });
        g.addPass("w.surface.radiance copy", QueueType::Graphics, [&](PassBuilder& b) { b.use(bandARadiance, Use::CopySrc); b.use(copy, Use::CopyDst); },
                  [=](PassContext& c) { c.cmd->CopyResource(c.resource(copy), c.resource(bandARadiance)); });
        debug.radiance = copy;
        debug.copyRadiance = false;
    }
    st.frame[ring] = fc.frame.frameIndex;
    st.last = fc.frame.frameIndex;
}

WaterSurfaceStats latestWaterSurfaceStats(TrackState& state)
{
    SurfaceState& st = state.get<SurfaceState>("W.surface");
    WaterSurfaceStats out;
    if (st.last == UINT64_MAX || !st.readbackMapped) return out;
    const uint32_t ring = uint32_t(st.last % kRing);
    uint32_t w[kStatCount];
    std::memcpy(w, st.readbackMapped + ring * kStatBytes, sizeof w);
    out.frameIndex = st.frame[ring];
    out.shaded = w[0], out.offscreen = w[1], out.exited = w[2], out.occluded = w[3], out.steps = w[4], out.inside = w[5], out.unlit = w[6];
    out.rayOverflow = w[7], out.reflectJobs = w[8], out.refractJobs = w[9], out.traced = w[10];
    out.planar = w[11];
    for (uint32_t k = 0; k < kPlanarMax; ++k) out.planarMask[k] = w[12 + k];
    out.planarViews = st.planarViews[ring];
    return out;
}
} // namespace unx::water
