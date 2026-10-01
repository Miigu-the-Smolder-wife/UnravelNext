#include "FroxelSystem.h"

#include "SResources.h"
#include "VsmSystem.h"

#include "unx/render/GpuScene.h"
#include "unx/render/Tracks.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace unx::render::shadow
{
using namespace s_detail;

namespace
{
const char* const kStateKey = "s.froxel";
constexpr uint32_t kStatsSlots = 4, kHeaderBytes = 64;
constexpr uint32_t kSortedMax = 96;  // FROXEL_SORTED_MAX (FroxelLists.hlsl): the ordered head of a list
constexpr uint32_t kScanBlock = 2048;  // froxels per FroxelScan block (two levels: at most 2048 x 2048 froxels)

struct State
{
    ComPtr<ID3D12Resource> statsReadback;
    ComPtr<ID3D12CommandSignature> dispatchSignature;
    uint64_t statsFrame[kStatsSlots] = {};
    uint64_t statsFence[kStatsSlots] = {};
    uint32_t statsBound[kStatsSlots] = {};      // the scene bound that sized the frame's main-view buffer
    uint64_t statsAllowance[kStatsSlots] = {};  // the FX allowance it held
    int lastStatsSlot = -1;
    FroxelStats latest;
    // Lists buffer capacities (entries): the scene bound + the FX allowance, a power of two with hysteresis (no plan
    // churn around a boundary).
    uint64_t capacityMain = 0, capacityPlanar = 0;
    uint64_t fxAllowance = 0;  // entries for the FX particle lights (their reach is computed on the GPU): grows from
                               // the measured overage (needed - capacity) of a frame that listed the scene lights only
    uint32_t boundNow = 0;     // the main view's scene bound and FX allowance of the frame being recorded
    uint64_t allowanceNow = 0;
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

namespace
{
uint32_t sortedHead(const QualityConfig& q)
{
    const uint32_t listMax = (uint32_t)q.integer("atmosphere.froxels.lights_max");
    if (listMax == 0 || listMax > kSortedMax || (listMax & 1)) fail("atmosphere.froxels.lights_max must be even and in [2, %u] (FROXEL_SORTED_MAX)", kSortedMax);
    return listMax;
}
constexpr uint64_t kFxAllowancePerLight = 2048;  // entries per FX light slot the allowance starts from [예상: a spark light
                                                 // of 1 m range 2 m from the camera reaches about 400 tiles x 5 slices]
constexpr uint64_t kCapacityMin = 1ull << 16;

// The capacity of a view's lists buffer this frame (entries, even): the scene bound + the FX allowance rounded up to a
// power of two, kept while the need stays above a quarter of it.
uint32_t listCapacity(const QualityConfig& q, State& s, bool planar, uint64_t bound, uint64_t fxAllowance)
{
    const uint64_t need = bound + fxAllowance;
    uint64_t target = kCapacityMin;
    while (target < need) target <<= 1;
    uint64_t& capacity = planar ? s.capacityPlanar : s.capacityMain;
    if (target > capacity || target * 4 <= capacity) capacity = target;
    uint64_t result = capacity;
    const uint64_t forced = (uint64_t)q.integer("atmosphere.froxels.list_capacity_forced");  // tests: a frame over it falls back
    if (forced > 0) result = (forced + 1) & ~1ull;
    if (result >= (1ull << 31)) fail("froxel lists: capacity %llu entries exceeds the 31-bit entry index", (unsigned long long)result);
    return (uint32_t)result;
}

struct D3 { double x, y, z; };
D3 d3(float3 v) { return { v.x, v.y, v.z }; }
double dot3(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
// Tiles (of n, unit-depth lateral extent u0 + [0, n] x w, w of either sign) whose extent meets [lo, hi], plus one tile of
// margin on each side for the float arithmetic of the GPU test; 0 when the interval misses the grid.
uint64_t tilesMeeting(double lo, double hi, double u0, double w, uint32_t n)
{
    if (!(hi >= lo)) return 0;
    double a = (lo - u0) / w, b = (hi - u0) / w;
    if (a > b) std::swap(a, b);
    const double i0 = std::floor(a) - 1, i1 = std::floor(b) + 1;
    if (i1 < 0 || i0 > (double)n - 1) return 0;
    return (uint64_t)(std::min(i1, (double)n - 1) - std::max(i0, 0.0)) + 1;
}
} // namespace

uint64_t froxelListBound(const FroxelGridCpu& g, const ViewDesc& view, const std::vector<gpu::Light>& lights)
{
    // Camera basis as FroxelLists.hlsl uses it (froxelForward = -normalize(g_view[2].xyz); the view matrix's rows) and
    // the rays through the image corners scaled to unit view depth (froxelRayAt): their lateral coordinates are affine
    // in the pixel position, so tile tx spans unit-depth lateral [ux0 + tx wx, ux0 + (tx + 1) wx] (wx per tile).
    auto row = [&](int r) { return normalize(float3{ view.view.m[r][0], view.view.m[r][1], view.view.m[r][2] }); };
    const D3 right = d3(row(0)), up = d3(row(1)), forward = d3(-row(2)), cam = d3(view.position);
    auto rayAt = [&](double px, double py) {
        const double ndc[4] = { px / view.width * 2 - 1, 1 - py / view.height * 2, 1, 1 };
        double p[4];
        for (int r = 0; r < 4; ++r)
        {
            p[r] = 0;
            for (int c = 0; c < 4; ++c) p[r] += (double)view.invViewProj.m[r][c] * ndc[c];
        }
        const D3 w{ p[0] / p[3] - cam.x, p[1] / p[3] - cam.y, p[2] / p[3] - cam.z };
        const double z = dot3(w, forward);  // = nearPlane for a point of device depth 1 (froxelRayAt divides by it)
        return D3{ w.x / z, w.y / z, w.z / z };
    };
    const D3 r00 = rayAt(0, 0), r10 = rayAt(view.width, 0), r01 = rayAt(0, view.height);
    const double ux0 = dot3(r00, right), uy0 = dot3(r00, up);
    const double wx = (dot3(r10, right) - ux0) / view.width * g.tilePx, wy = (dot3(r01, up) - uy0) / view.height * g.tilePx;
    const double uxMax = std::max(std::abs(ux0), std::abs(ux0 + wx * g.gridX)), uyMax = std::max(std::abs(uy0), std::abs(uy0 + wy * g.gridY));
    const double secant = std::sqrt(1 + uxMax * uxMax + uyMax * uyMax);  // >= sqrt(1 + a^2) of every tile-edge plane
    const double logRatio = std::log2((double)g.farM / g.nearM);
    auto node = [&](uint32_t n) { return n == 0 ? 0.0 : g.nearM * std::exp2(logRatio * n / g.slices); };
    // Per slice: the depth span, and twice the largest bounding-sphere radius of its froxels (the half diagonal of a
    // world-axis box of a set of diameter D is at most sqrt(3) / 2 D; D at most the diagonal of the view-aligned box of
    // the outermost tile's froxel: lateral extent over both depths, depth extent).
    struct Slice { double z0, z1, zb, rho2; };
    std::vector<Slice> slices(g.slices);
    for (uint32_t sIdx = 0; sIdx < g.slices; ++sIdx)
    {
        const bool last = sIdx + 1 == g.slices;
        const double z0 = node(sIdx), z1 = last ? 3.0e38 : node(sIdx + 1), zb = last ? std::max((double)g.farM, 2 * z0) : z1;
        const double ex = std::abs(wx) * zb + uxMax * (zb - z0), ey = std::abs(wy) * zb + uyMax * (zb - z0), ez = zb - z0;
        const double diam = std::sqrt(ex * ex + ey * ey + ez * ez);
        slices[sIdx] = { z0, z1, zb, std::sqrt(3.0) * diam };  // 2 rho_max
    }
    uint64_t total = 0;
    for (const gpu::Light& l : lights)
    {
        const uint32_t type = l.typeFlags & 0xFFu;
        const double extent = type == 5 ? 0.5 * l.size.x + l.size.y : (type == 2 ? 0.5 * std::sqrt((double)l.size.x * l.size.x + (double)l.size.y * l.size.y)
                                                                      : (type == 3 || type == 4 ? l.size.x : 0.0));
        const double r = (double)l.range + extent;  // froxelLightRadius
        const D3 v{ l.position.x - cam.x, l.position.y - cam.y, l.position.z - cam.z };
        const double lx = dot3(v, right), ly = dot3(v, up), lz = dot3(v, forward);
        // Tile frustum test of the lists pass (plane distances >= -r): in unit-depth lateral coordinates, the light's
        // projection widened by r secant / lz; a light within r of the camera passes every tile.
        double fx0 = -1e300, fx1 = 1e300, fy0 = -1e300, fy1 = 1e300;
        if (lz > r)
        {
            const double m = r * secant / lz;
            fx0 = lx / lz - m, fx1 = lx / lz + m, fy0 = ly / lz - m, fy1 = ly / lz + m;
        }
        // Slices the light's depth span [lz - r, lz + r] meets (the explicit test below stays): slice s spans nodes s, s + 1.
        if (lz + r < 0) continue;
        auto coord = [&](double z) { return std::log2(std::max(z, 1e-300) / g.nearM) / logRatio * g.slices; };
        const uint32_t sMax = lz + r < g.nearM ? 0u : (uint32_t)std::min<double>(g.slices - 1, std::floor(coord(lz + r)));
        const uint32_t sMin = lz - r <= 0 ? 0u : (uint32_t)std::min<double>(g.slices - 1, std::max(0.0, std::ceil(coord(lz - r)) - 1));
        for (uint32_t sIdx = sMin; sIdx <= sMax; ++sIdx)
        {
            const Slice& sl = slices[sIdx];
            const bool last = sIdx + 1 == g.slices;
            if (lz + r < sl.z0 || (!last && lz - r > sl.z1)) continue;
            uint64_t nx = tilesMeeting(fx0, fx1, ux0, wx, g.gridX), ny = tilesMeeting(fy0, fy1, uy0, wy, g.gridY);
            if (!last)
            {
                // Sphere test (light radius against the froxel's bounding sphere): every corner of a froxel that passes
                // lies within r + 2 rho of the light, so the froxel's lateral span over its depths meets [l - R, l + R].
                const double R = r + sl.rho2, z0 = std::max(sl.z0, 1e-9);
                auto span = [&](double lo, double hi, double u0, double w, uint32_t n) {
                    const double bl = std::min(lo / z0, lo / sl.zb), ah = std::max(hi / z0, hi / sl.zb);
                    return tilesMeeting(bl, ah, u0, w, n);
                };
                nx = std::min(nx, span(lx - R, lx + R, ux0, wx, g.gridX));
                ny = std::min(ny, span(ly - R, ly + R, uy0, wy, g.gridY));
            }
            total += nx * ny;
        }
    }
    return total;
}

uint64_t froxelListBytes(const FroxelGridCpu& grid, uint64_t capacity)
{
    const uint64_t froxels = (uint64_t)grid.gridX * grid.gridY * grid.slices;
    return kHeaderBytes + froxels * 8 + capacity * 2;
}

uint32_t froxelListCapacity(TrackState& state) { return (uint32_t)state.get<State>(kStateKey).latest.capacityNow; }

void setKeepFroxels(TrackState& state, bool keep) { state.get<State>(kStateKey).keep = keep; }
void setFroxelFullDepth(TrackState& state, bool full) { state.get<State>(kStateKey).fullDepth = full; }

namespace
{
// Lists of one view (its frame constants): begin (header) + lists. Pass names get 'suffix'.
BufferRef recordLists(FramePassContext& fc, const ViewResources& view, uint32_t slotOfLightSrv, TextureRef readers, const std::string& suffix)
{
    const QualityConfig& q = fc.quality;
    const FroxelGridCpu grid = froxelGridFor(q, view.view.width, view.view.height);
    const uint32_t listMax = sortedHead(q);
    const uint64_t froxels = (uint64_t)grid.gridX * grid.gridY * grid.slices;
    State& s = fc.state<State>(kStateKey);
    const bool planar = !suffix.empty();
    const uint64_t bound = froxelListBound(grid, view.view, fc.scene.lights());
    const uint32_t fxSlots = fc.scene.fxLightRange().capacity;
    if (fxSlots > 0) s.fxAllowance = std::max(s.fxAllowance, (uint64_t)fxSlots * kFxAllowancePerLight);
    const uint64_t fxAllowance = fxSlots > 0 ? s.fxAllowance : 0;
    const uint32_t capacity = listCapacity(q, s, planar, bound, fxAllowance);
    if (!planar)
    {
        s.latest.capacityNow = capacity;
        s.boundNow = (uint32_t)std::min<uint64_t>(bound, 0xFFFFFFFFu);
        s.allowanceNow = fxAllowance;
    }
    const uint32_t blocks = (uint32_t)((froxels + kScanBlock - 1) / kScanBlock);
    if (blocks > kScanBlock) fail("froxel lists: %llu froxels exceed the two-level scan (%u x %u)", (unsigned long long)froxels, kScanBlock, kScanBlock);
    const uint64_t bytes = froxelListBytes(grid, capacity);
    const uint32_t fallbackForced = (uint32_t)q.integer("atmosphere.froxels.list_fallback_forced");  // tests: scene lights only this frame
    RenderGraph& g = fc.graph;
    const BufferRef lights = g.createBuffer(BufferDesc{ suffix.empty() ? "S froxel light lists" : "S froxel light lists (planar view)", bytes, 0 });
    const BufferRef blockSums = g.createBuffer(BufferDesc{ suffix.empty() ? "S froxel list block sums" : "S froxel list block sums (planar view)", (uint64_t)kScanBlock * 4 * 2, 0 });
    // The scene lights' own allocation (FroxelScan's second prefix sum): the runs of a frame whose need exceeds the capacity.
    const BufferRef sceneAlloc = g.createBuffer(BufferDesc{ suffix.empty() ? "S froxel list scene allocation" : "S froxel list scene allocation (planar view)", froxels * 4, 0 });
    const D3D12_GPU_VIRTUAL_ADDRESS constants = view.frameConstants;
    ShaderLibrary& sh = fc.shaders;
    ID3D12PipelineState* pb = sh.compute("Passes/Atmosphere/FroxelBegin");
    ID3D12PipelineState* pc = sh.compute("Passes/Atmosphere/FroxelLists.MODE0");
    ID3D12PipelineState* ps0 = sh.compute("Passes/Atmosphere/FroxelScan.MODE0");
    ID3D12PipelineState* ps1 = sh.compute("Passes/Atmosphere/FroxelScan.MODE1");
    ID3D12PipelineState* pl = sh.compute("Passes/Atmosphere/FroxelLists.MODE1");
    uint32_t nearBits, farBits;
    std::memcpy(&nearBits, &grid.nearM, 4);
    std::memcpy(&farBits, &grid.farM, 4);
    g.addPass("s.froxel.begin" + suffix, QueueType::Compute, [&](PassBuilder& b) { b.use(lights, Use::UavCompute); },
              [=](PassContext& ctx) {
                  const uint32_t k[8] = { ctx.uav(lights), grid.gridX, grid.gridY, grid.slices, grid.tilePx, nearBits, farBits, capacity };
                  ctx.cmd->SetPipelineState(pb);
                  ctx.computeConstants(k, 8);
                  ctx.cmd->Dispatch(1, 1, 1);
              });
    // Count, scan (two levels), fill: every list is allocated and stored within this frame (RENDERER_REDESIGN_V2 14.1).
    auto cullInputs = [&](PassBuilder& b) {
        b.use(lights, Use::UavCompute);
        if (readers.valid()) b.use(readers, Use::SrvCompute);
        // A3: the FX light tail (read through the scene's SRVs; declared so the FX writer runs first).
        if (fc.resources.fxLights.valid()) b.use(fc.resources.fxLights, Use::SrvCompute);
        if (fc.resources.fxLightCount.valid()) b.use(fc.resources.fxLightCount, Use::SrvCompute);
    };
    g.addPass("s.froxel.count" + suffix, QueueType::Compute, [&](PassBuilder& b) { cullInputs(b); },
              [=](PassContext& ctx) {
                  const uint32_t k[8] = { ctx.uav(lights), listMax, slotOfLightSrv, readers.valid() ? ctx.srv(readers) : 0xFFFFFFFFu, 0xFFFFFFFFu, 0, 0, 0 };
                  ctx.cmd->SetPipelineState(pc);
                  ctx.bindFrameConstants(constants);
                  ctx.computeConstants(k, 8);
                  ctx.cmd->Dispatch(grid.gridX, grid.gridY, 1);
              });
    g.addPass("s.froxel.scan.blocks" + suffix, QueueType::Compute,
              [&](PassBuilder& b) { b.use(lights, Use::UavCompute); b.use(blockSums, Use::UavCompute); b.use(sceneAlloc, Use::UavCompute); },
              [=](PassContext& ctx) {
                  const uint32_t k[4] = { ctx.uav(lights), ctx.uav(blockSums), (uint32_t)froxels, ctx.uav(sceneAlloc) };
                  ctx.cmd->SetPipelineState(ps0);
                  ctx.computeConstants(k, 4);
                  ctx.cmd->Dispatch(blocks, 1, 1);
              });
    g.addPass("s.froxel.scan.top" + suffix, QueueType::Compute,
              [&](PassBuilder& b) { b.use(lights, Use::UavCompute); b.use(blockSums, Use::UavCompute); },
              [=](PassContext& ctx) {
                  const uint32_t k[4] = { ctx.uav(lights), ctx.uav(blockSums), blocks, 0 };
                  ctx.cmd->SetPipelineState(ps1);
                  ctx.computeConstants(k, 4);
                  ctx.cmd->Dispatch(1, 1, 1);
              });
    g.addPass("s.froxel.lists" + suffix, QueueType::Compute,
              [&](PassBuilder& b) { cullInputs(b); b.use(blockSums, Use::SrvCompute); b.use(sceneAlloc, Use::SrvCompute); },
              [=](PassContext& ctx) {
                  const uint32_t k[8] = { ctx.uav(lights), listMax, slotOfLightSrv, readers.valid() ? ctx.srv(readers) : 0xFFFFFFFFu, ctx.srv(blockSums), fallbackForced, ctx.srv(sceneAlloc), 0 };
                  ctx.cmd->SetPipelineState(pl);
                  ctx.bindFrameConstants(constants);
                  ctx.computeConstants(k, 8);
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
TextureRef recordIntegration(FramePassContext& fc, const ViewResources& view, BufferRef lights, bool keepVolume, TextureRef readers, const std::string& suffix,
                              TextureRef media = {})
{
    const QualityConfig& q = fc.quality;
    const FroxelGridCpu grid = froxelGridFor(q, view.view.width, view.view.height);
    RenderGraph& g = fc.graph;
    // Air volume: in-scattering, optical depth, sun transmittance; nodes 0..S each (FroxelIntegrate.hlsl).
    const TextureRef volume = g.createTexture(TextureDesc{ suffix.empty() ? "S air volume" : "S air volume (planar view)", grid.gridX, grid.gridY, (uint16_t)(3 * (grid.slices + 1) + 2), 1,
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
    const bool queued = !q.has("atmosphere.froxels.integration_queue") || q.boolean("atmosphere.froxels.integration_queue");
    ID3D12PipelineState* pi = fc.shaders.compute(queued ? "Passes/Atmosphere/FroxelIntegrate.QUEUED1" : "Passes/Atmosphere/FroxelIntegrate.QUEUED0");
    // Readers per tile: the integration stops where no reader reaches (FroxelIntegrate.hlsl).
    const bool bounded = readers.valid();
    const BufferRef functions = fc.resources.lightFunctions;  // E's light functions (A8; invalid: none)
    BufferRef work, workArgs, air;
    ID3D12CommandSignature* signature = nullptr;
    if (queued)
    {
        const uint64_t count = (uint64_t)grid.gridX * grid.gridY * grid.slices;
        if (count * 64 >= (1ull << 32)) fail("froxel air scratch exceeds 32-bit raw buffer addressing");
        work = g.createBuffer({ "S froxel integration queue", 16 + count * 4, 0 });
        workArgs = g.createBuffer({ "S froxel integration args", 16, 0 });
        air = g.createBuffer({ "S froxel FP32 air slices", count * 64, 0 });
        State& state = fc.state<State>(kStateKey);
        if (!state.dispatchSignature)
        {
            D3D12_INDIRECT_ARGUMENT_DESC arg{};
            arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
            D3D12_COMMAND_SIGNATURE_DESC desc{};
            desc.ByteStride = 16; desc.NumArgumentDescs = 1; desc.pArgumentDescs = &arg;
            check(fc.device.d3d()->CreateCommandSignature(&desc, nullptr, IID_PPV_ARGS(&state.dispatchSignature)), "froxel queue dispatch signature");
        }
        signature = state.dispatchSignature.Get();
    }
    auto inputs = [=](PassBuilder& b) {
        b.use(lights, Use::SrvCompute);
        if (bounded) b.use(readers, Use::SrvCompute);
        if (media.valid()) b.use(media, Use::SrvCompute);
        if (functions.valid()) b.use(functions, Use::SrvCompute);
        b.use(tlut, Use::SrvCompute); b.use(mlut, Use::SrvCompute);
        if (shadows)
        {
            b.use(vsm.table, Use::SrvCompute); b.use(vsm.atlas, Use::SrvCompute);
            b.use(vsm.blocks, Use::SrvCompute); b.use(vsm.bound, Use::SrvCompute);
            b.use(vsm.stats, Use::UavCompute);
            if (vsm.use.valid()) b.use(vsm.use, Use::UavCompute);
        }
    };
    auto bind = [=](PassContext& ctx, uint32_t workDescriptor, uint32_t airDescriptor) {
        uint32_t k[20] = { ctx.srv(lights), workDescriptor == 0xFFFFFFFFu ? ctx.uav(volume) : 0xFFFFFFFFu, ctx.srv(tlut), ctx.srv(mlut), 0, 0, 0, 0xFFFFFFFFu, 0, 0, 0, 0, localLights, slotOfLight,
                           bounded ? ctx.srv(readers) : 0xFFFFFFFFu, 0xFFFFFFFFu, media.valid() ? ctx.srv(media) : 0xFFFFFFFFu,
                           functions.valid() ? ctx.srv(functions) : 0xFFFFFFFFu, workDescriptor, airDescriptor };
        if (shadows)
        {
            k[4] = ctx.srv(vsm.table); k[5] = ctx.srv(vsm.atlas); k[6] = ctx.srv(vsm.blocks);
            k[7] = vsm.constantsCbv; k[8] = ctx.srv(vsm.bound); k[15] = ctx.uav(vsm.stats);
            std::memcpy(&k[9], &grid.shadowTexelsPerTile, 4);
        }
        std::memcpy(&k[10], &stepAltitude, 4);
        k[11] = experiment | (walkStats ? 0x10000u : 0u);
        ctx.bindFrameConstants(constants); ctx.computeConstants(k, 20);
    };
    if (queued)
    {
        ID3D12PipelineState* begin = fc.shaders.compute("Passes/Atmosphere/FroxelQueueArgs.MODE0");
        ID3D12PipelineState* prepare = fc.shaders.compute("Passes/Atmosphere/FroxelQueuePrepare");
        ID3D12PipelineState* args = fc.shaders.compute("Passes/Atmosphere/FroxelQueueArgs.MODE1");
        ID3D12PipelineState* integrate = fc.shaders.compute("Passes/Atmosphere/FroxelQueueIntegrate");
        g.addPass("s.froxel.queue.begin" + suffix, QueueType::Compute,
                  [&](PassBuilder& b) { b.use(work, Use::UavCompute); },
                  [=](PassContext& ctx) {
                      const uint32_t k[4] = { ctx.uav(work), 0, 0, 0 };
                      ctx.cmd->SetPipelineState(begin); ctx.computeConstants(k, 4); ctx.cmd->Dispatch(1, 1, 1);
                  });
        g.addPass("s.froxel.queue.prepare" + suffix, QueueType::Compute,
                  [&](PassBuilder& b) { inputs(b); b.use(work, Use::UavCompute); b.use(air, Use::UavCompute); },
                  [=](PassContext& ctx) {
                      ctx.cmd->SetPipelineState(prepare); bind(ctx, ctx.uav(work), ctx.uav(air));
                      ctx.cmd->Dispatch(grid.gridX, grid.gridY, 1);
                  });
        g.addPass("s.froxel.queue.args" + suffix, QueueType::Compute,
                  [&](PassBuilder& b) { b.use(work, Use::UavCompute); b.use(workArgs, Use::UavCompute); },
                  [=](PassContext& ctx) {
                      const uint32_t k[4] = { ctx.uav(work), ctx.uav(workArgs), 0, 0 };
                      ctx.cmd->SetPipelineState(args); ctx.computeConstants(k, 4); ctx.cmd->Dispatch(1, 1, 1);
                  });
        g.addPass("s.froxel.queue.integrate" + suffix, QueueType::Compute,
                  [&](PassBuilder& b) { inputs(b); b.use(work, Use::SrvCompute); b.use(workArgs, Use::IndirectArgs); b.use(air, Use::UavCompute); },
                  [=](PassContext& ctx) {
                      ctx.cmd->SetPipelineState(integrate); bind(ctx, ctx.srv(work), ctx.uav(air));
                      ctx.cmd->ExecuteIndirect(signature, 1, ctx.resource(workArgs), 0, nullptr, 0);
                  });
    }
    g.addPass("s.froxel.integrate" + suffix, QueueType::Compute,
              [&](PassBuilder& b) {
                  inputs(b); b.use(volume, Use::UavCompute);
                  if (queued) b.use(air, Use::SrvCompute);
                  if (keepVolume) b.keep();
              },
              [=](PassContext& ctx) {
                  ctx.cmd->SetPipelineState(pi); bind(ctx, 0xFFFFFFFFu, queued ? ctx.srv(air) : 0xFFFFFFFFu);
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
    (void)sortedHead(q);
    const scene::Scene* src = fc.scene.source();
    if (src && src->lights.size() > 0x7FFF) fail("froxel lists hold 15-bit light indices (bit 15: shadow slot): %zu lights", src->lights.size());

    // Harvest completed stats (no stall).
    if (!s.statsReadback) s.statsReadback = createBuffer(fc.device, L"S froxel stats readback", (uint64_t)kStatsSlots * kHeaderBytes, D3D12_HEAP_TYPE_READBACK);
    const uint64_t completed = fc.device.queue(QueueType::Graphics).completed();
    if (s.lastStatsSlot >= 0) s.statsFence[s.lastStatsSlot] = fc.graph.lastFence(QueueType::Graphics);
    for (uint32_t i = 0; i < kStatsSlots; ++i)
    {
        // Only frames at least framesInFlight old (the host has waited for those): the newest read is then always frame -
        // framesInFlight, not whichever frame the GPU happened to finish (the counts size buffers: runs differed).
        if (s.statsFence[i] == 0 || s.statsFence[i] > completed || s.statsFrame[i] <= s.latest.frame ||
            s.statsFrame[i] + fc.framesInFlight > fc.frame.frameIndex)
            continue;
        uint32_t* p = nullptr;
        D3D12_RANGE r{ i * kHeaderBytes, (i + 1) * kHeaderBytes };
        check(s.statsReadback->Map(0, &r, reinterpret_cast<void**>(&p)), "map froxel stats");
        const uint32_t* w = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(p) + i * kHeaderBytes);
        const uint32_t capacityNow = s.latest.capacityNow;
        s.latest = { s.statsFrame[i], w[11], w[12], w[13], w[14], w[15], w[7], w[10], s.statsBound[i] };
        s.latest.capacityNow = capacityNow;
        if (s.latest.needed > s.latest.capacity)
        {
            // Scene lights only that frame (FroxelLists.hlsl): the FX particle lights exceeded their allowance (the scene's
            // lights are inside the bound). Grow it for the frames from now on; the gate reports the cut lists.
            const uint64_t overage = s.latest.needed - s.latest.capacity;
            s.fxAllowance = std::max(s.fxAllowance, s.statsAllowance[i] + overage + overage / 2);
            logf("S froxel lists: frame %llu needed %u entries over the capacity %u (scene bound %u, FX allowance %llu): scene lights only, %u lists cut, %u FX entries lost; the allowance grows to %llu\n",
                 (unsigned long long)s.latest.frame, s.latest.needed, s.latest.capacity, s.latest.sceneBound, (unsigned long long)s.statsAllowance[i], s.latest.overflowLists,
                 s.latest.droppedLights, (unsigned long long)s.fxAllowance);
        }
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
    // E's particle media (smoke, fire) on this grid, between the lists and the integration (invalid: none this frame); a
    // view whose volumeSlices a producer already set keeps them (tests: FroxelTests 7).
    ViewResources mediaView = main;
    const TextureRef media = main.volumeSlices.valid() ? main.volumeSlices : tracks::volumeMedia(fc, mediaView, lights);
    const TextureRef volume = recordIntegration(fc, main, lights, s.keep, recordReaders(fc, main, s.fullDepth, ""), "", media);
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
    s.statsBound[slot] = s.boundNow;
    s.statsAllowance[slot] = s.allowanceNow;
    s.lastStatsSlot = (int)slot;
}
} // namespace unx::render::shadow
