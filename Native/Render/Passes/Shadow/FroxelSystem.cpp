#include "FroxelSystem.h"

#include "SResources.h"
#include "VsmSystem.h"

#include "unx/render/GpuScene.h"
#include "unx/render/Tracks.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace unx::render::shadow
{
using namespace s_detail;

namespace
{
const char* const kStateKey = "s.froxel";
constexpr uint32_t kStatsSlots = 4, kHeaderBytes = 64;
constexpr uint32_t kListMax = 32;  // FROXEL_LIST_MAX (FroxelLists.hlsl)

struct State
{
    ComPtr<ID3D12Resource> statsReadback;
    uint64_t statsFrame[kStatsSlots] = {};
    uint64_t statsFence[kStatsSlots] = {};
    int lastStatsSlot = -1;
    FroxelStats latest;
    bool keep = false;
    bool fullDepth = false;  // tests: integrate every slice (no reader bound)
    // This frame's lists (recordFroxelLists, called by shadowPages).
    uint64_t listsFrame = UINT64_MAX;
    BufferRef lists;
};
} // namespace

FroxelGridCpu froxelGridFor(const QualityConfig& q, uint32_t width, uint32_t height)
{
    FroxelGridCpu g;
    g.tilePx = (uint32_t)q.integer("atmosphere.froxels.tile_px");
    g.slices = (uint32_t)q.integer("atmosphere.froxels.depth_slices");
    g.nearM = (float)q.number("atmosphere.froxels.near_m");
    g.farM = (float)q.number("atmosphere.froxels.far_m");
    g.shadowTexelsPerTile = (float)q.number("atmosphere.froxels.shadow_texels_per_tile");
    if (!(g.shadowTexelsPerTile >= 1)) fail("atmosphere.froxels.shadow_texels_per_tile must be >= 1");
    if (g.tilePx == 0 || g.slices == 0 || g.slices > 64) fail("atmosphere.froxels: tile_px > 0 and 1 <= depth_slices <= 64 (one thread per slice)");
    if (!(g.nearM > 0) || !(g.farM > g.nearM)) fail("atmosphere.froxels: 0 < near_m < far_m");
    g.gridX = (width + g.tilePx - 1) / g.tilePx;
    g.gridY = (height + g.tilePx - 1) / g.tilePx;
    return g;
}

const FroxelStats& froxelStats(TrackState& state) { return state.get<State>(kStateKey).latest; }

void setKeepFroxels(TrackState& state, bool keep) { state.get<State>(kStateKey).keep = keep; }
void setFroxelFullDepth(TrackState& state, bool full) { state.get<State>(kStateKey).fullDepth = full; }

namespace
{
// Lists of one view (its frame constants): begin (header) + lists. Pass names get 'suffix'.
BufferRef recordLists(FramePassContext& fc, const ViewResources& view, uint32_t slotOfLightSrv, TextureRef readers, const std::string& suffix)
{
    const QualityConfig& q = fc.quality;
    const FroxelGridCpu grid = froxelGridFor(q, view.view.width, view.view.height);
    const uint32_t listMax = (uint32_t)q.integer("atmosphere.froxels.lights_max");
    if (listMax == 0 || listMax > kListMax) fail("atmosphere.froxels.lights_max must be in [1, %u] (FROXEL_LIST_MAX)", kListMax);
    const uint64_t froxels = (uint64_t)grid.gridX * grid.gridY * grid.slices;
    const uint32_t stride = (listMax + 1) & ~1u;  // entries per froxel: every list fits (FroxelCommon.hlsli)
    const uint64_t bytes = kHeaderBytes + froxels * 4 + froxels * stride * 2;
    if (froxels * stride >= (1ull << 26)) fail("froxel lists: %llu entries exceed the header's 26-bit first entry", (unsigned long long)(froxels * stride));
    RenderGraph& g = fc.graph;
    const BufferRef lights = g.createBuffer(BufferDesc{ suffix.empty() ? "S froxel light lists" : "S froxel light lists (planar view)", bytes, 0 });
    const D3D12_GPU_VIRTUAL_ADDRESS constants = view.frameConstants;
    ShaderLibrary& sh = fc.shaders;
    ID3D12PipelineState* pb = sh.compute("Passes/Atmosphere/FroxelBegin");
    ID3D12PipelineState* pl = sh.compute("Passes/Atmosphere/FroxelLists");
    uint32_t nearBits, farBits;
    std::memcpy(&nearBits, &grid.nearM, 4);
    std::memcpy(&farBits, &grid.farM, 4);
    g.addPass("s.froxel.begin" + suffix, QueueType::Compute, [&](PassBuilder& b) { b.use(lights, Use::UavCompute); },
              [=](PassContext& ctx) {
                  const uint32_t k[8] = { ctx.uav(lights), grid.gridX, grid.gridY, grid.slices, grid.tilePx, nearBits, farBits, stride };
                  ctx.cmd->SetPipelineState(pb);
                  ctx.computeConstants(k, 8);
                  ctx.cmd->Dispatch(1, 1, 1);
              });
    g.addPass("s.froxel.lists" + suffix, QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(lights, Use::UavCompute);
                  if (readers.valid()) b.use(readers, Use::SrvCompute);
              },
              [=](PassContext& ctx) {
                  const uint32_t k[4] = { ctx.uav(lights), listMax, slotOfLightSrv, readers.valid() ? ctx.srv(readers) : 0xFFFFFFFFu };
                  ctx.cmd->SetPipelineState(pl);
                  ctx.bindFrameConstants(constants);
                  ctx.computeConstants(k, 4);
                  ctx.cmd->Dispatch(grid.gridX, grid.gridY, 1);
              });
    return lights;
}

// Readers per tile (farthest surface, sky; planar views: mirror pixels only), FroxelTileDepth.hlsl; invalid (every slice
// and tile) for tests in full-depth mode or a view without depth.
TextureRef recordReaders(FramePassContext& fc, const ViewResources& view, bool fullDepth, const std::string& suffix)
{
    if (fullDepth || !view.depth.valid()) return {};
    const FroxelGridCpu grid = froxelGridFor(fc.quality, view.view.width, view.view.height);
    RenderGraph& g = fc.graph;
    const TextureRef readers = g.createTexture(TextureDesc{ "S froxel tile readers", grid.gridX, grid.gridY, 1, 1, DXGI_FORMAT_R32G32_FLOAT });
    const TextureRef depth = view.depth, mask = view.view.planarMask, tileMask = view.view.planarTileMask;
    const D3D12_GPU_VIRTUAL_ADDRESS constants = view.frameConstants;
    ID3D12PipelineState* pd = fc.shaders.compute("Passes/Atmosphere/FroxelTileDepth");
    const uint32_t tilePx = grid.tilePx;
    g.addPass("s.froxel.tiledepth" + suffix, QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(depth, Use::SrvCompute);
                  if (mask.valid()) b.use(mask, Use::SrvCompute);
                  if (tileMask.valid()) b.use(tileMask, Use::SrvCompute);
                  b.use(readers, Use::UavCompute);
              },
              [=](PassContext& ctx) {
                  const uint32_t k[8] = { ctx.srv(depth), ctx.uav(readers), tilePx, mask.valid() ? ctx.srv(mask) : 0xFFFFFFFFu,
                                          tileMask.valid() ? ctx.srv(tileMask) : 0xFFFFFFFFu, 0, 0, 0 };
                  ctx.cmd->SetPipelineState(pd);
                  ctx.bindFrameConstants(constants);
                  ctx.computeConstants(k, 8);
                  ctx.cmd->Dispatch(grid.gridX, grid.gridY, 1);
              });
    return readers;
}

// Air volume of one view from its lists (FroxelIntegrate.hlsl; a view with a clip plane integrates from the plane on).
TextureRef recordIntegration(FramePassContext& fc, const ViewResources& view, BufferRef lights, bool keepVolume, TextureRef readers, const std::string& suffix)
{
    const QualityConfig& q = fc.quality;
    const FroxelGridCpu grid = froxelGridFor(q, view.view.width, view.view.height);
    RenderGraph& g = fc.graph;
    // Air volume: in-scattering, optical depth, sun transmittance; nodes 0..S each (FroxelIntegrate.hlsl).
    const TextureRef volume = g.createTexture(TextureDesc{ suffix.empty() ? "S air volume" : "S air volume (planar view)", grid.gridX, grid.gridY, (uint16_t)(3 * (grid.slices + 1) + 1), 1,
                                                           DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_DIMENSION_TEXTURE3D });
    // Substep altitude: the configured bound, and at most 1/12 of the medium's shortest scale height (midpoint error
    // (dh / H)^2 / 24 <= 0.03 %: mist with H_Mie 300 m steps at 25 m).
    const scene::Atmosphere medium = fc.scene.source() ? fc.scene.source()->atmosphere : scene::Atmosphere{};
    const float stepAltitude = std::min((float)q.number("atmosphere.froxels.air_step_altitude_m"),
                                        std::min(medium.rayleighScaleHeight, medium.mieScaleHeight) / 12.0f);
    const uint32_t experiment = (uint32_t)q.integer("atmosphere.froxels.experiment_disable");  // cost attribution only
    if (!(stepAltitude > 0)) fail("atmosphere.froxels.air_step_altitude_m must be > 0");
    const bool walkStats = q.integer("atmosphere.froxels.walk_stats") != 0;  // measurement only
    const TextureRef tlut = fc.resources.transmittanceLut, mlut = fc.resources.multiScatterLut;
    if (!tlut.valid() || !mlut.valid()) fail("S.froxels: the atmosphere LUTs were not recorded this frame");
    VsmFrameRefs vsm;
    const bool shadows = frameRefs(fc, vsm);
    const D3D12_GPU_VIRTUAL_ADDRESS constants = view.frameConstants;
    const uint32_t localLights = fc.resources.vsmLocalLights, slotOfLight = fc.resources.vsmSlotOfLight;
    ID3D12PipelineState* pi = fc.shaders.compute("Passes/Atmosphere/FroxelIntegrate");
    // Readers per tile: the integration stops where no reader reaches (FroxelIntegrate.hlsl).
    const bool bounded = readers.valid();
    g.addPass("s.froxel.integrate" + suffix, QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(lights, Use::SrvCompute);
                  if (bounded) b.use(readers, Use::SrvCompute);
                  b.use(tlut, Use::SrvCompute);
                  b.use(mlut, Use::SrvCompute);
                  b.use(volume, Use::UavCompute);
                  if (keepVolume) b.keep();
                  if (shadows)
                  {
                      b.use(vsm.table, Use::SrvCompute);
                      b.use(vsm.pool, Use::SrvCompute);
                      b.use(vsm.blocks, Use::SrvCompute);
                      b.use(vsm.bound, Use::SrvCompute);
                      if (walkStats) b.use(vsm.stats, Use::UavCompute);
                      if (vsm.use.valid()) b.use(vsm.use, Use::UavCompute);  // shadow.vsm.use_stats (vsmEntry)
                  }
              },
              [=](PassContext& ctx) {
                  uint32_t k[16] = { ctx.srv(lights), ctx.uav(volume), ctx.srv(tlut), ctx.srv(mlut), 0, 0, 0, 0xFFFFFFFFu, 0, 0, 0, 0, localLights, slotOfLight,
                                     bounded ? ctx.srv(readers) : 0xFFFFFFFFu, 0xFFFFFFFFu };
                  if (shadows)
                  {
                      k[4] = ctx.srv(vsm.table);
                      k[5] = ctx.srv(vsm.pool);
                      k[6] = ctx.srv(vsm.blocks);
                      k[7] = vsm.constantsCbv;
                      k[8] = ctx.srv(vsm.bound);
                      std::memcpy(&k[9], &grid.shadowTexelsPerTile, 4);
                      if (walkStats) k[15] = ctx.uav(vsm.stats);
                  }
                  std::memcpy(&k[10], &stepAltitude, 4);
                  k[11] = experiment;
                  ctx.cmd->SetPipelineState(pi);
                  ctx.bindFrameConstants(constants);
                  ctx.computeConstants(k, 16);
                  ctx.cmd->Dispatch(grid.gridX, grid.gridY, 1);
              });
    if (!shadows) tracks::pending("S.froxels: sun shadows of the air (shadowPages not recorded this frame)");
    return volume;
}
} // namespace

void recordFroxelLists(FramePassContext& fc, const ViewResources& main, uint32_t slotOfLightSrv)
{
    State& s = fc.state<State>(kStateKey);
    const QualityConfig& q = fc.quality;
    const uint32_t listMax = (uint32_t)q.integer("atmosphere.froxels.lights_max");
    if (listMax == 0 || listMax > kListMax) fail("atmosphere.froxels.lights_max must be in [1, %u] (FROXEL_LIST_MAX)", kListMax);
    const scene::Scene* src = fc.scene.source();
    if (src && src->lights.size() > 0x7FFF) fail("froxel lists hold 15-bit light indices (bit 15: shadow slot): %zu lights", src->lights.size());

    // Harvest completed stats (no stall).
    if (!s.statsReadback) s.statsReadback = createBuffer(fc.device, L"S froxel stats readback", (uint64_t)kStatsSlots * kHeaderBytes, D3D12_HEAP_TYPE_READBACK);
    const uint64_t completed = fc.device.queue(QueueType::Graphics).completed();
    if (s.lastStatsSlot >= 0) s.statsFence[s.lastStatsSlot] = fc.graph.lastFence(QueueType::Graphics);
    for (uint32_t i = 0; i < kStatsSlots; ++i)
    {
        if (s.statsFence[i] == 0 || s.statsFence[i] > completed || s.statsFrame[i] <= s.latest.frame) continue;
        uint32_t* p = nullptr;
        D3D12_RANGE r{ i * kHeaderBytes, (i + 1) * kHeaderBytes };
        check(s.statsReadback->Map(0, &r, reinterpret_cast<void**>(&p)), "map froxel stats");
        const uint32_t* w = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(p) + i * kHeaderBytes);
        s.latest = { s.statsFrame[i], w[11], w[12], w[13], w[14], w[15] };
        D3D12_RANGE none{ 0, 0 };
        s.statsReadback->Unmap(0, &none);
    }

    const BufferRef lights = recordLists(fc, main, slotOfLightSrv, TextureRef{}, "");
    fc.resources.froxelLights = lights;
    s.lists = lights;
    s.listsFrame = fc.frame.frameIndex;
}

void recordPlanarFroxels(FramePassContext& fc, ViewResources& view)
{
    if (!fc.resources.transmittanceLut.valid() || !fc.resources.multiScatterLut.valid())
    {
        tracks::pending("S.froxels: planar view without the atmosphere LUTs");
        return;
    }
    // Lists of the view's own frustum (the virtual camera's; froxels before the mirror hold lights too, conservatively),
    // then the air from the mirror plane on (FroxelIntegrate.hlsl: g_clipPlane of the view's frame constants).
    // Readers first: tiles without mirror pixels around them get empty lists and no air.
    const TextureRef readers = recordReaders(fc, view, fc.state<State>(kStateKey).fullDepth, ".planar");
    const BufferRef lists = recordLists(fc, view, fc.resources.vsmSlotOfLight, readers, ".planar");
    view.froxelLights = lists;
    view.airVolume = recordIntegration(fc, view, lists, false, readers, ".planar");
}

void recordFroxels(FramePassContext& fc, const ViewResources& main)
{
    State& s = fc.state<State>(kStateKey);
    if (s.listsFrame != fc.frame.frameIndex) recordFroxelLists(fc, main, 0xFFFFFFFFu);  // no shadowPages this frame
    RenderGraph& g = fc.graph;
    const BufferRef lights = s.lists;
    const TextureRef volume = recordIntegration(fc, main, lights, s.keep, recordReaders(fc, main, s.fullDepth, ""), "");
    fc.resources.froxels = volume;
    fc.resources.aerialPerspective = volume;  // atmosphereAerial / atmosphereAirView read it (Atmosphere.hlsli)

    // Header (counters) to the readback ring; harvested at a later record once the GPU passed this frame.
    const uint32_t slot = (uint32_t)(fc.frame.frameIndex % kStatsSlots);
    ID3D12Resource* rb = s.statsReadback.Get();
    g.addPass("s.froxel.stats", QueueType::Graphics,
              [&](PassBuilder& b) {
                  b.use(lights, Use::CopySrc);
                  b.keep();
              },
              [=](PassContext& ctx) { ctx.cmd->CopyBufferRegion(rb, (uint64_t)slot * kHeaderBytes, ctx.resource(lights), 0, kHeaderBytes); });
    s.statsFrame[slot] = fc.frame.frameIndex;
    s.statsFence[slot] = 0;
    s.lastStatsSlot = (int)slot;
}
} // namespace unx::render::shadow
