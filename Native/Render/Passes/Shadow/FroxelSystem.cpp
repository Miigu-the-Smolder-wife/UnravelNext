#include "FroxelSystem.h"

#include "SResources.h"
#include "VsmSystem.h"

#include "unx/render/GpuScene.h"
#include "unx/scene/SceneData.h"
#include "unx/render/Tracks.h"
#if UNX_S_HAS_RAYTRACING
#include "unx/rt/RayPipeline.h"
#include "unx/rt/RayScene.h"
#endif

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
    // Turbid basins (shading.water_turbid; WaterMedia.hlsl): the frame's basin records in an upload ring (raw SRVs).
    std::vector<ComPtr<ID3D12Resource>> waterUploads;
    std::vector<uint32_t> waterSrvs;
    uint64_t waterBytes = 0;
};

// Turbid basin water as a froxel medium (defect queue 13 (75); Passes/Water/WaterMedia.hlsl): the W2 basins of the frame
// whose Water material scatters add their optical depth and single-scattered source to the view's media slices (E's
// particle media layout), combined with E's when present. Returns the slices to integrate (media unchanged when the
// switch is off, the frame has no scattering basin, or there are no lists).
TextureRef recordWaterMedia(FramePassContext& fc, const ViewResources& main, BufferRef lights, TextureRef media, const FroxelGridCpu& grid)
{
    const QualityConfig& q = fc.quality;
    if (!(q.has("shading.water_turbid") && q.boolean("shading.water_turbid")) || !lights.valid()) return media;
    const scene::Scene* src = fc.scene.source();
    if (!src || fc.frame.poolCount == 0) return media;
    struct Rec
    {
        float centre[3], cosYaw, sinYaw, halfX, halfZ, depth, sigmaS[3], g, sigmaA[3], pad;
    };
    static_assert(sizeof(Rec) == 64, "WaterMedia.hlsl basin record");
    std::vector<Rec> recs;
    for (uint32_t i = 0; i < fc.frame.poolCount; ++i)
    {
        const PoolFrame& p = fc.frame.pools[i];
        if (p.material >= src->materials.size()) continue;
        const scene::Material& m = src->materials[p.material];
        if (!(m.waterScattering.x > 0 || m.waterScattering.y > 0 || m.waterScattering.z > 0)) continue;
        Rec r{};
        for (int a = 0; a < 3; ++a) r.centre[a] = (float)p.centre[a];
        r.cosYaw = std::cos(p.yaw);
        r.sinYaw = std::sin(p.yaw);
        r.halfX = 0.5f * p.sizeX;
        r.halfZ = 0.5f * p.sizeZ;
        r.depth = p.depth;
        r.sigmaS[0] = m.waterScattering.x, r.sigmaS[1] = m.waterScattering.y, r.sigmaS[2] = m.waterScattering.z;
        r.g = m.waterAnisotropy;
        r.sigmaA[0] = -std::log(std::max(m.baseColor.x, 1e-6f)), r.sigmaA[1] = -std::log(std::max(m.baseColor.y, 1e-6f)), r.sigmaA[2] = -std::log(std::max(m.baseColor.z, 1e-6f));
        recs.push_back(r);
    }
    if (recs.empty()) return media;
    State& s = fc.state<State>(kStateKey);
    Device& d = fc.device;
    const uint32_t ring = fc.framesInFlight + 1, slot = (uint32_t)(fc.frame.frameIndex % ring);
    const uint64_t bytes = recs.size() * sizeof(Rec);
    if (s.waterUploads.size() != ring || s.waterBytes < bytes)
    {
        for (ComPtr<ID3D12Resource>& u : s.waterUploads) d.deferRelease(u);
        for (uint32_t srv : s.waterSrvs) d.descriptors().freeResource(srv);
        s.waterUploads.clear();
        s.waterSrvs.clear();
        uint64_t cap = 1024;
        while (cap < bytes) cap *= 2;
        s.waterBytes = cap;
        for (uint32_t i = 0; i < ring; ++i)
        {
            s.waterUploads.push_back(createBuffer(d, L"S water media basins", cap, D3D12_HEAP_TYPE_UPLOAD));
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = DXGI_FORMAT_R32_TYPELESS;
            sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Buffer.NumElements = (UINT)(cap / 4);
            sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
            const uint32_t srv = d.descriptors().allocateResource();
            d.d3d()->CreateShaderResourceView(s.waterUploads.back().Get(), &sd, d.descriptors().resourceCpu(srv));
            s.waterSrvs.push_back(srv);
        }
    }
    {
        void* mapped = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(s.waterUploads[slot]->Map(0, &none, &mapped), "map water media basins");
        std::memcpy(mapped, recs.data(), bytes);
        s.waterUploads[slot]->Unmap(0, nullptr);
    }
    RenderGraph& g = fc.graph;
    const TextureRef slices = media.valid() ? media
                                            : g.createTexture(TextureDesc{ "S water media", grid.gridX, grid.gridY, (uint16_t)(2 * grid.slices), 1,
                                                                           DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_DIMENSION_TEXTURE3D });
    VsmFrameRefs vsm;
    const bool shadows = frameRefs(fc, vsm);
    const FrameResources& r = fc.resources;
    const bool sunMap = r.waterSunDepth.valid() && r.waterSunNormal.valid() && r.waterSunMedium.valid() && r.waterSunConstants.valid();
    const bool caustics = sunMap && r.waterSunCaustics.valid(), gi = r.giCache.valid(), functions = r.lightFunctions.valid();
    const bool luts = r.transmittanceLut.valid() && r.multiScatterLut.valid();
    const uint32_t count = (uint32_t)recs.size(), basinSrv = s.waterSrvs[slot], existing = media.valid() ? 1u : 0u;
    uint32_t texelBits = 0;
    std::memcpy(&texelBits, &grid.shadowTexelsPerTile, 4);
    const D3D12_GPU_VIRTUAL_ADDRESS constants = main.frameConstants;
    ID3D12PipelineState* pso = fc.shaders.compute("Passes/Water/WaterMedia");
    g.addPass("s.froxel.watermedia", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(lights, Use::SrvCompute);
                  b.use(slices, Use::UavCompute);
                  if (sunMap)
                  {
                      for (TextureRef t : { r.waterSunDepth, r.waterSunNormal, r.waterSunMedium }) b.use(t, Use::SrvCompute);
                      b.use(r.waterSunConstants, Use::SrvCompute);
                      if (caustics) b.use(r.waterSunCaustics, Use::SrvCompute);
                  }
                  if (gi) b.use(r.giCache, Use::SrvCompute);
                  if (functions) b.use(r.lightFunctions, Use::SrvCompute);
                  if (shadows)
                  {
                      b.use(vsm.table, Use::SrvCompute); b.use(vsm.atlas, Use::SrvCompute);
                      b.use(vsm.blocks, Use::SrvCompute); b.use(vsm.bound, Use::SrvCompute);
                  }
                  if (luts) { b.use(r.transmittanceLut, Use::SrvCompute); b.use(r.multiScatterLut, Use::SrvCompute); }
              },
              [=](PassContext& ctx) {
                  const uint32_t none = 0xFFFFFFFFu;
                  const uint32_t k[20] = { ctx.srv(lights), ctx.uav(slices), count, basinSrv,
                                           sunMap ? ctx.srv(r.waterSunDepth) : none, sunMap ? ctx.srv(r.waterSunNormal) : none, sunMap ? ctx.srv(r.waterSunMedium) : none,
                                           sunMap ? ctx.srv(r.waterSunConstants) : none,
                                           caustics ? ctx.srv(r.waterSunCaustics) : none, gi ? ctx.srv(r.giCache) : none, functions ? ctx.srv(r.lightFunctions) : none, existing,
                                           shadows ? ctx.srv(vsm.table) : none, shadows ? ctx.srv(vsm.atlas) : none, shadows ? ctx.srv(vsm.blocks) : none, shadows ? vsm.constantsCbv : none,
                                           shadows ? ctx.srv(vsm.bound) : none, texelBits, luts ? ctx.srv(r.transmittanceLut) : none, luts ? ctx.srv(r.multiScatterLut) : none };
                  ctx.cmd->SetPipelineState(pso);
                  ctx.bindFrameConstants(constants);
                  ctx.computeConstants(k, 20);
                  ctx.cmd->Dispatch(grid.gridX, grid.gridY, 1);
              });
    return slices;
}
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

FogView fogViewFor(const QualityConfig& q, uint32_t width, uint32_t height)
{
    FogView f;
    if (!q.has("atmosphere.fog.enabled") || !q.boolean("atmosphere.fog.enabled")) return f;
    const float scale = (float)q.number("atmosphere.fog.extinction_scale");
    f.density = (float)q.number("atmosphere.fog.density_per_m") * scale;
    f.falloff = (float)q.number("atmosphere.fog.height_falloff_per_m");
    f.height = (float)q.number("atmosphere.fog.height_m");
    f.g = (float)q.number("atmosphere.fog.phase_g");
    f.start = (float)q.number("atmosphere.fog.start_distance_m");
    f.indirect = q.boolean("atmosphere.fog.indirect_light");
    f.skyAmount = (float)q.number("atmosphere.fog.sky_amount");
    f.historyWeight = (float)q.number("atmosphere.fog.history_weight");
    f.shadowTexelsPerCell = (float)q.number("atmosphere.fog.shadow_texels_per_cell");
    const std::vector<double> albedo = q.numbers("atmosphere.fog.albedo");
    if (albedo.size() != 3 || !(scale > 0) || !(f.density >= 0) || !(f.falloff >= 0) || !(f.g > -1 && f.g < 1) || !(f.start >= 0))
        fail("atmosphere.fog: albedo of 3 numbers, extinction_scale > 0, density and falloff >= 0, phase_g in (-1, 1), start distance >= 0");
    for (int k = 0; k < 3; ++k) f.albedo[k] = (float)albedo[k] / scale;
    f.cellPx = (uint32_t)q.integer("atmosphere.fog.cell_px");
    f.gridZ = (uint32_t)q.integer("atmosphere.fog.depth_slices");
    f.farM = (float)q.number("atmosphere.fog.volumetric_distance_m");
    if (f.cellPx < 4 || f.cellPx > 64 || (f.cellPx & (f.cellPx - 1)) != 0) fail("atmosphere.fog.cell_px must be 4, 8, 16, 32 or 64");
    if (f.gridZ < 8 || f.gridZ > 256 || !(f.farM > 1)) fail("atmosphere.fog: depth_slices in [8, 256], volumetric_distance_m > 1");
    if (!(f.skyAmount >= 0 && f.skyAmount <= 1) || !(f.historyWeight >= 0 && f.historyWeight < 1) || !(f.shadowTexelsPerCell >= 0.25f))
        fail("atmosphere.fog: sky_amount in [0, 1], history_weight in [0, 1), shadow_texels_per_cell >= 0.25");
    f.gridX = (width + f.cellPx - 1) / f.cellPx;
    f.gridY = (height + f.cellPx - 1) / f.cellPx;
    f.k = 32.0f / f.farM;                                // (the reference's depth distribution scale)
    f.b = (float)f.gridZ / std::log2(33.0f);             // slice(farM) = gridZ
    f.on = f.density > 0;
    return f;
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

// The air volume's own fog medium (Fog.hlsli fogMedium in FroxelIntegrate.hlsl): the first fog. The fog has its own
// volume now (recordFogVolume, FogVolume.hlsli) and this stays off - the kernels' medium path is kept for the particle
// media it shares its code with.
struct FogSettings
{
    bool on = false, ambient = true;
    float density = 0, falloff = 0, height = 0, g = 0, start = 0;
    float albedo[3] = { 1, 1, 1 };
};

// Air volume of one view from its lists (FroxelIntegrate.hlsl; a view with a clip plane integrates from the plane on).
// fluence, moment: the view's sampled local light (shading.mega_lights_volume; invalid: none) - the fog's local lights.
TextureRef recordIntegration(FramePassContext& fc, const ViewResources& view, BufferRef lights, bool keepVolume, TextureRef readers, const std::string& suffix,
                              TextureRef media = {}, TextureRef sampledLocal = {}, TextureRef fluence = {}, TextureRef moment = {})
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
    // L4 (RENDERER_REDESIGN_V2 14.4) bounded walk omission: its own key, the same bit the experiment mask carries (1024; A/B)
    const bool walkOmission = q.has("atmosphere.froxels.walk_omission") && q.boolean("atmosphere.froxels.walk_omission");
    const uint32_t experiment = (uint32_t)q.integer("atmosphere.froxels.experiment_disable") | (walkOmission ? 1024u : 0u);  // cost attribution only (bit 1024: L4)
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
    // the height fog (Fog.hlsli): its local light from the sampled fluence and moment, its indirect light from the
    // previous frame's translucency volume (this frame's is built after the air)
    const FogSettings fog{};
    const bool clipAtSurface = q.boolean("atmosphere.froxels.clip_at_surface");
    const bool fogLocal = fog.on && fluence.valid() && moment.valid();
    const bool fogAmbient = fog.on && fog.ambient && fc.resources.translucencyGiPrevParams != 0xFFFFFFFFu && fc.resources.translucencyGiPrevAmbient.valid() &&
                            fc.resources.translucencyGiPrevDirectional.valid();
    const TextureRef fogAmbientA = fc.resources.translucencyGiPrevAmbient, fogAmbientD = fc.resources.translucencyGiPrevDirectional;
    const uint32_t fogAmbientParams = fc.resources.translucencyGiPrevParams;
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
        if (sampledLocal.valid()) b.use(sampledLocal, Use::SrvCompute);  // shading.mega_lights_volume (P[5].y)
        if (fogLocal)
        {
            b.use(fluence, Use::SrvCompute);
            b.use(moment, Use::SrvCompute);
        }
        if (fogAmbient)
        {
            b.use(fogAmbientA, Use::SrvCompute);
            b.use(fogAmbientD, Use::SrvCompute);
        }
        if (functions.valid()) b.use(functions, Use::SrvCompute);
        b.use(tlut, Use::SrvCompute); b.use(mlut, Use::SrvCompute);
        if (shadows)
        {
            b.use(vsm.table, Use::SrvCompute); b.use(vsm.atlas, Use::SrvCompute);
            b.use(vsm.blocks, Use::SrvCompute); b.use(vsm.bound, Use::SrvCompute);
            if (vsm.clsBlocks.valid()) b.use(vsm.clsBlocks, Use::SrvCompute);  // L3 classification blocks
            b.use(vsm.stats, Use::UavCompute);
            if (vsm.use.valid()) b.use(vsm.use, Use::UavCompute);
        }
    };
    auto bind = [=](PassContext& ctx, uint32_t workDescriptor, uint32_t airDescriptor) {
        uint32_t k[36] = { ctx.srv(lights), workDescriptor == 0xFFFFFFFFu ? ctx.uav(volume) : 0xFFFFFFFFu, ctx.srv(tlut), ctx.srv(mlut), 0, 0, 0, 0xFFFFFFFFu, 0, 0, 0, 0, localLights, slotOfLight,
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
        k[20] = shadows && vsm.clsBlocks.valid() ? ctx.srv(vsm.clsBlocks) : 0xFFFFFFFFu;  // P[5].x: L3 classification blocks (14.4 lit segments)
        k[21] = sampledLocal.valid() ? ctx.srv(sampledLocal) : 0xFFFFFFFFu;  // P[5].y: the local lights' sampled in-scattering
        k[22] = k[23] = 0;
        // P[6..8]: the height fog (Fog.hlsli)
        k[24] = (fog.on ? 1u : 0u) | (clipAtSurface ? 2u : 0u);  // (bit 1: FroxelSlice.hlsli FROXEL_CLIP_AT_SURFACE)
        k[25] = fogAmbient ? fogAmbientParams : 0xFFFFFFFFu;
        k[26] = fogLocal ? ctx.srv(fluence) : 0xFFFFFFFFu;
        k[27] = fogLocal ? ctx.srv(moment) : 0xFFFFFFFFu;
        std::memcpy(&k[28], &fog.density, 4);
        std::memcpy(&k[29], &fog.falloff, 4);
        std::memcpy(&k[30], &fog.height, 4);
        std::memcpy(&k[31], &fog.g, 4);
        std::memcpy(&k[32], fog.albedo, 12);
        std::memcpy(&k[35], &fog.start, 4);
        ctx.bindFrameConstants(constants); ctx.computeConstants(k, 36);
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

namespace
{
// shading.mega_lights_volume: a view's sampled local light (defined below; invalid members = off)
struct SampledLocal
{
    TextureRef inScattering, fluence, moment;
};
SampledLocal recordSampledLocal(FramePassContext& fc, const ViewResources& view, BufferRef lights, TextureRef readers, bool persistent);
} // namespace

namespace
{
// The fog's volume of the main view (FogVolume.hlsli): s.fog.scatter and s.fog.integrate; publishes
// FrameResources::fog / fogVolume / fogFarSource for M's m.fog. lights: the view's froxel lists (the air grid the sampled
// local light lives on).
struct FogState
{
    Device* device = nullptr;
    ComPtr<ID3D12Resource> scatter[2];
    uint32_t x = 0, y = 0, z = 0, parity = 0, revision = 0xFFFFFFFFu;
    bool fresh = true;
    ~FogState()
    {
        if (!device) return;
        for (auto& t : scatter)
            if (t) device->deferRelease(t);
    }
};
float fogHalton(uint32_t index, uint32_t base)
{
    float f = 1, r = 0;
    for (uint32_t i = index; i > 0; i /= base)
    {
        f /= (float)base;
        r += f * (float)(i % base);
    }
    return r;
}
void recordFogVolume(FramePassContext& fc, const ViewResources& main, BufferRef lights, const SampledLocal& sampled);
} // namespace

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
    // shading.mega_lights: the view's own sampled local light (no history: the view has no identity between frames) - its
    // air takes it in place of the loop over the lists, its lit particles read the fluence and moment.
    const SampledLocal sampled = recordSampledLocal(fc, view, lists, readers, false);
    view.localFluence = sampled.fluence;
    view.localMoment = sampled.moment;
    view.airVolume = recordIntegration(fc, view, lists, false, readers, ".planar", TextureRef{}, sampled.inScattering, sampled.fluence, sampled.moment);
}

namespace
{
// shading.mega_lights with shading.mega_lights_volume (MegaLightsVolume.hlsl; owner A): a view's local in-scattering per
// froxel from light samples with shadow rays, and the same samples' fluence and direction moment. The main view keeps its
// history (ping-pong, in this track's state); other views (planar reflections) use transients without history. Invalid:
// off, or no ray scene this frame - the integration then keeps its loop over the lists and the VSM walks.
struct SampledLocalState
{
    Device* device = nullptr;
    ComPtr<ID3D12Resource> volume[2], fluence[2], moment[2];  // (fluence, moment: the same samples' light for lit particles)
    uint32_t x = 0, y = 0, z = 0, parity = 0, revision = 0xFFFFFFFFu;
    bool fresh = true;
    float exposure = 0;
    ~SampledLocalState()
    {
        if (!device) return;
        for (auto* set : { volume, fluence, moment })
            for (int k = 0; k < 2; ++k)
                if (set[k]) device->deferRelease(set[k]);
    }
};

// One dispatch holds at most this many shadow rays (the structural bound of a dispatch's work, DISPATCH_BOUNDS_KO.md):
// the froxel grid goes in bands of slices.
constexpr uint32_t kSampledLocalRaysPerDispatch = 262144;

SampledLocal recordSampledLocal(FramePassContext& fc, const ViewResources& view, BufferRef lights, TextureRef readers, bool persistent)
{
    SampledLocal out;
#if UNX_S_HAS_RAYTRACING
    const QualityConfig& q = fc.quality;
    if (!q.has("shading.mega_lights") || !q.boolean("shading.mega_lights") || !q.boolean("shading.mega_lights_volume")) return out;
    const FrameResources r = fc.resources;
    if (!r.tlasStatic.valid() || !r.transmittanceLut.valid() || !fc.trackState) return out;
    const FroxelGridCpu grid = froxelGridFor(q, view.view.width, view.view.height);
    RenderGraph& g = fc.graph;
    const TextureDesc desc{ "S ml volume", grid.gridX, grid.gridY, (uint16_t)grid.slices, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_DIMENSION_TEXTURE3D };
    const TextureDesc fluenceDesc{ "S ml volume fluence", grid.gridX, grid.gridY, (uint16_t)grid.slices, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_DIMENSION_TEXTURE3D };
    const TextureDesc momentDesc{ "S ml volume moment", grid.gridX, grid.gridY, (uint16_t)grid.slices, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_DIMENSION_TEXTURE3D };
    TextureRef previous, prevFluence, prevMoment, output, fluence, moment;
    float ratio = 1.0f;
    if (persistent)
    {
        // the main view: its own history (ping-pong, in this track's state)
        SampledLocalState& st = fc.state<SampledLocalState>("S.froxels.sampledLocal");
        if (!st.volume[0] || st.x != grid.gridX || st.y != grid.gridY || st.z != grid.slices)
        {
            st.device = &fc.device;
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC1 rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
            rd.Width = grid.gridX;
            rd.Height = grid.gridY;
            rd.DepthOrArraySize = (UINT16)grid.slices;
            rd.MipLevels = 1;
            rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            rd.SampleDesc.Count = 1;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            for (auto* set : { st.volume, st.fluence, st.moment })
                for (int k = 0; k < 2; ++k)
                {
                    if (set[k]) fc.device.deferRelease(set[k]);
                    set[k].Reset();
                    check(fc.device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr, nullptr, 0, nullptr,
                                                                    IID_PPV_ARGS(&set[k])),
                          "S sampled local in-scattering");
                    set[k]->SetName(set == st.volume ? L"S ml volume" : (set == st.fluence ? L"S ml volume fluence" : L"S ml volume moment"));
                }
            st.x = grid.gridX; st.y = grid.gridY; st.z = grid.slices;
            st.fresh = true;
        }
        const float3 shift = fc.frame.originShift;
        const bool valid = !st.fresh && st.revision == fc.scene.revision() && fc.frame.discontinuity == 0 && shift.x == 0 && shift.y == 0 && shift.z == 0;
        st.fresh = false;
        st.revision = fc.scene.revision();
        const float exposure = 1.0f / (1.2f * std::exp2(view.view.ev100));
        ratio = valid && st.exposure > 0 ? exposure / st.exposure : 1.0f;
        st.exposure = exposure;
        const uint32_t prev = st.parity, next = prev ^ 1u;
        st.parity = next;
        if (valid)
        {
            previous = g.importTexture(st.volume[prev].Get(), desc, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
            prevFluence = g.importTexture(st.fluence[prev].Get(), fluenceDesc, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
            prevMoment = g.importTexture(st.moment[prev].Get(), momentDesc, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
        }
        output = g.importTexture(st.volume[next].Get(), desc, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
        fluence = g.importTexture(st.fluence[next].Get(), fluenceDesc, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
        moment = g.importTexture(st.moment[next].Get(), momentDesc, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    }
    else
    {
        // a view without identity between frames (planar reflection views): the graph's transients, no history
        output = g.createTexture(desc);
        fluence = g.createTexture(fluenceDesc);
        moment = g.createTexture(momentDesc);
    }
    out.inScattering = output;
    out.fluence = fluence;
    out.moment = moment;
    const TextureRef tlut = r.transmittanceLut;
    const BufferRef functions = r.lightFunctions, fxLights = r.fxLights;
    const uint32_t samples = (uint32_t)q.integer("shading.mega_lights_volume_samples");
    if (samples != 1 && samples != 2 && samples != 4) fail("shading.mega_lights_volume_samples must be 1, 2 or 4");
    const float minWeight = (float)q.number("shading.mega_lights_volume_min_sample_weight"), cap = (float)q.number("shading.mega_lights_max_shading_weight");
    const float bias = (float)q.number("shading.mega_lights_ray_bias_m"), endBias = (float)q.number("shading.mega_lights_ray_end_bias_m");
    const float frames = (float)q.number("shading.mega_lights_volume_max_frames");
    if (!(frames >= 1 && frames <= 64)) fail("shading.mega_lights_volume_max_frames must be in [1, 64]");
    rt::RayScene* rays = &rt::RayScene::get(fc);
    rt::RayPipeline& pipeline = rt::RayPipeline::get(fc.device, fc.shaders, rt::standardRayPipeline("Passes/Atmosphere/MegaLightsVolume", { "MegaLightsVolumeGen" }));
    const D3D12_GPU_VIRTUAL_ADDRESS constants = view.frameConstants;
    const uint32_t slicesPerDispatch = std::max<uint32_t>(1, kSampledLocalRaysPerDispatch / std::max<uint32_t>(grid.gridX * grid.gridY * samples, 1));
    g.addPass("s.ml.volume", QueueType::Compute,
              [&](PassBuilder& b) {
                  rays->declareTraversal(b);
                  b.use(lights, Use::SrvCompute);
                  if (fxLights.valid()) b.use(fxLights, Use::SrvCompute);
                  b.use(tlut, Use::SrvCompute);
                  if (functions.valid()) b.use(functions, Use::SrvCompute);
                  if (readers.valid()) b.use(readers, Use::SrvCompute);
                  if (previous.valid())
                      for (TextureRef t : { previous, prevFluence, prevMoment }) b.use(t, Use::SrvCompute);
                  for (TextureRef t : { output, fluence, moment }) b.use(t, Use::UavCompute);
                  if (persistent) b.keep();  // persistent state: the next frame's history
              },
              [=, &pipeline](PassContext& ctx) {
                  auto bits = [](float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; };
                  uint32_t k[32] = { ctx.srv(lights), ctx.uav(output), ctx.srv(tlut), functions.valid() ? ctx.srv(functions) : 0xFFFFFFFFu,
                                     previous.valid() ? ctx.srv(previous) : 0xFFFFFFFFu, samples, readers.valid() ? ctx.srv(readers) : 0xFFFFFFFFu, 0,
                                     bits(minWeight), bits(bias), bits(endBias), bits(ratio),
                                     bits(cap), bits(frames), 0, 0,
                                     ctx.uav(fluence), ctx.uav(moment), previous.valid() ? ctx.srv(prevFluence) : 0xFFFFFFFFu, previous.valid() ? ctx.srv(prevMoment) : 0xFFFFFFFFu };
                  rays->rootConstants(k + 24);
                  ctx.bindFrameConstants(constants);
                  for (uint32_t first = 0; first < grid.slices; first += slicesPerDispatch)
                  {
                      k[14] = first;  // P[3].z: the dispatch's first slice
                      ctx.computeConstants(k, 32);
                      pipeline.dispatch(ctx.cmd, 0, grid.gridX, grid.gridY, std::min(slicesPerDispatch, grid.slices - first));
                  }
              });
#else
    (void)fc; (void)view; (void)lights; (void)readers; (void)persistent;
#endif
    return out;
}

void recordFogVolume(FramePassContext& fc, const ViewResources& main, BufferRef lights, const SampledLocal& sampled)
{
    fc.resources.fog = FogView{};
    fc.resources.fogVolume = {};
    fc.resources.fogFarSource = {};
    const FogView f = fogViewFor(fc.quality, main.view.width, main.view.height);
    const FrameResources r = fc.resources;
    if (!f.on || !fc.trackState || !main.hiz.valid() || !r.transmittanceLut.valid()) return;
    RenderGraph& g = fc.graph;
    FogState& st = fc.state<FogState>("S.fog.volume");
    const TextureDesc desc{ "S fog scatter", f.gridX, f.gridY, (uint16_t)f.gridZ, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_DIMENSION_TEXTURE3D };
    if (!st.scatter[0] || st.x != f.gridX || st.y != f.gridY || st.z != f.gridZ)
    {
        st.device = &fc.device;
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC1 rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        rd.Width = f.gridX;
        rd.Height = f.gridY;
        rd.DepthOrArraySize = (UINT16)f.gridZ;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        for (auto& t : st.scatter)
        {
            if (t) fc.device.deferRelease(t);
            t.Reset();
            check(fc.device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&t)),
                  "S fog scatter volume");
            t->SetName(L"S fog scatter");
        }
        st.x = f.gridX; st.y = f.gridY; st.z = f.gridZ;
        st.fresh = true;
    }
    const float3 shift = fc.frame.originShift;
    const bool valid = !st.fresh && st.revision == fc.scene.revision() && fc.frame.discontinuity == 0 && shift.x == 0 && shift.y == 0 && shift.z == 0;
    st.fresh = false;
    st.revision = fc.scene.revision();
    const uint32_t prev = st.parity, next = prev ^ 1u;
    st.parity = next;
    TextureRef history;
    if (valid && f.historyWeight > 0) history = g.importTexture(st.scatter[prev].Get(), desc, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    const TextureRef scatter = g.importTexture(st.scatter[next].Get(), desc, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    const TextureRef integrated =
        g.createTexture({ "S fog volume", f.gridX, f.gridY, (uint16_t)f.gridZ, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, D3D12_RESOURCE_DIMENSION_TEXTURE3D });
    const TextureRef farSource = g.createTexture({ "S fog far source", f.gridX, f.gridY, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });

    VsmFrameRefs vsm;
    const bool shadows = frameRefs(fc, vsm);
    const TextureRef hiz = main.hiz, tlut = r.transmittanceLut, fluence = sampled.fluence, moment = sampled.moment;
    const bool local = fluence.valid() && moment.valid();
    const bool ambient = f.indirect && r.translucencyGiPrevParams != 0xFFFFFFFFu && r.translucencyGiPrevAmbient.valid() && r.translucencyGiPrevDirectional.valid();
    const TextureRef ambientA = r.translucencyGiPrevAmbient, ambientD = r.translucencyGiPrevDirectional;
    const uint32_t ambientParams = r.translucencyGiPrevParams;
    const bool clip = fc.quality.boolean("atmosphere.froxels.clip_at_surface");
    // the frame's jitter of the cells' sample points (16 frames of the Halton points 2, 3, 5)
    const uint32_t index = (uint32_t)(fc.frame.frameIndex % 16u) + 1u;
    const float jitter[3] = { fogHalton(index, 2), fogHalton(index, 3), fogHalton(index, 5) };
    auto bits = [](float v) { uint32_t u; std::memcpy(&u, &v, 4); return u; };
    const uint32_t grid0 = f.gridX | f.gridY << 16, grid1 = f.gridZ | f.cellPx << 16;
    const D3D12_GPU_VIRTUAL_ADDRESS constants = main.frameConstants;
    ID3D12PipelineState* ps = fc.shaders.compute("Passes/Atmosphere/FogScatter");
    ID3D12PipelineState* pi = fc.shaders.compute("Passes/Atmosphere/FogIntegrate");
    g.addPass("s.fog.scatter", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(hiz, Use::SrvCompute);
                  b.use(tlut, Use::SrvCompute);
                  if (local)
                  {
                      b.use(lights, Use::SrvCompute);
                      b.use(fluence, Use::SrvCompute);
                      b.use(moment, Use::SrvCompute);
                  }
                  if (ambient)
                  {
                      b.use(ambientA, Use::SrvCompute);
                      b.use(ambientD, Use::SrvCompute);
                  }
                  if (shadows)
                  {
                      b.use(vsm.table, Use::SrvCompute); b.use(vsm.atlas, Use::SrvCompute);
                      b.use(vsm.blocks, Use::SrvCompute); b.use(vsm.bound, Use::SrvCompute);
                      b.use(vsm.stats, Use::UavCompute);
                  }
                  if (history.valid()) b.use(history, Use::SrvCompute);
                  b.use(scatter, Use::UavCompute);
                  b.keep();  // (the next frame's history)
              },
              [=](PassContext& ctx) {
                  const uint32_t none = 0xFFFFFFFFu;
                  uint32_t k[36] = { grid0, grid1, bits(f.farM), bits(f.k),
                                     bits(f.b), ctx.uav(scatter), history.valid() ? ctx.srv(history) : none, clip ? 1u : 0u,
                                     bits(f.density), bits(f.falloff), bits(f.height), bits(f.g),
                                     bits(f.albedo[0]), bits(f.albedo[1]), bits(f.albedo[2]), bits(f.start),
                                     shadows ? ctx.srv(vsm.table) : none, shadows ? ctx.srv(vsm.atlas) : none, shadows ? ctx.srv(vsm.blocks) : none,
                                     shadows ? vsm.constantsCbv : none,
                                     shadows ? ctx.srv(vsm.bound) : none, bits(f.shadowTexelsPerCell), local ? ctx.srv(lights) : none, ctx.srv(hiz),
                                     local ? ctx.srv(fluence) : none, local ? ctx.srv(moment) : none, ambient ? ambientParams : none, ctx.srv(tlut),
                                     bits(jitter[0]), bits(jitter[1]), bits(jitter[2]), bits(f.historyWeight),
                                     shadows ? ctx.uav(vsm.stats) : none, 0, 0, 0 };
                  ctx.cmd->SetPipelineState(ps);
                  ctx.bindFrameConstants(constants);
                  ctx.computeConstants(k, 36);
                  ctx.cmd->Dispatch((f.gridX + 3) / 4, (f.gridY + 3) / 4, (f.gridZ + 3) / 4);
              });
    g.addPass("s.fog.integrate", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(scatter, Use::SrvCompute);
                  b.use(tlut, Use::SrvCompute);
                  if (ambient)
                  {
                      b.use(ambientA, Use::SrvCompute);
                      b.use(ambientD, Use::SrvCompute);
                  }
                  b.use(integrated, Use::UavCompute);
                  b.use(farSource, Use::UavCompute);
              },
              [=](PassContext& ctx) {
                  const uint32_t none = 0xFFFFFFFFu;
                  uint32_t k[28] = { grid0, grid1, bits(f.farM), bits(f.k),
                                     bits(f.b), ctx.srv(scatter), ctx.uav(integrated), ctx.uav(farSource),
                                     bits(f.density), bits(f.falloff), bits(f.height), bits(f.g),
                                     bits(f.albedo[0]), bits(f.albedo[1]), bits(f.albedo[2]), bits(f.start),
                                     0, 0, 0, 0, 0, 0, 0, 0,
                                     none, none, ambient ? ambientParams : none, ctx.srv(tlut) };
                  ctx.cmd->SetPipelineState(pi);
                  ctx.bindFrameConstants(constants);
                  ctx.computeConstants(k, 28);
                  ctx.cmd->Dispatch((f.gridX + 7) / 8, (f.gridY + 7) / 8, 1);
              });
    fc.resources.fog = f;
    fc.resources.fogVolume = integrated;
    fc.resources.fogFarSource = farSource;
}
} // namespace

void recordFroxels(FramePassContext& fc, const ViewResources& main)
{
    State& s = fc.state<State>(kStateKey);
    if (s.listsFrame != fc.frame.frameIndex) recordFroxelLists(fc, main, 0xFFFFFFFFu);  // no shadowPages this frame
    RenderGraph& g = fc.graph;
    const BufferRef lights = s.lists;
    // shading.mega_lights_volume: the sampled local light first - it needs the lists and the readers only, and the lit
    // particle media below read its fluence and moment volumes (FrameResources::localFluence / localMoment).
    const TextureRef readers = recordReaders(fc, main, s.fullDepth, "");
    const SampledLocal sampled = recordSampledLocal(fc, main, lights, readers, true);
    const TextureRef sampledLocal = sampled.inScattering;
    fc.resources.localFluence = sampled.fluence;  // lit particles and particle media (FxLayerSetup.hlsl, VolumeSetup.hlsl)
    fc.resources.localMoment = sampled.moment;
    // E's particle media (smoke, fire) on this grid, between the lists and the integration (invalid: none this frame); a
    // view whose volumeSlices a producer already set keeps them (tests: FroxelTests 7).
    ViewResources mediaView = main;
    const TextureRef particleMedia = main.volumeSlices.valid() ? main.volumeSlices : tracks::volumeMedia(fc, mediaView, lights);
    // Turbid basins (defect queue 13 (75), shading.water_turbid): added to the media slices (or their own) - WaterMedia.hlsl.
    const TextureRef media = recordWaterMedia(fc, main, lights, particleMedia, froxelGridFor(fc.quality, main.view.width, main.view.height));
    const TextureRef volume = recordIntegration(fc, main, lights, s.keep, readers, "", media, sampledLocal, sampled.fluence, sampled.moment);
    recordFogVolume(fc, main, lights, sampled);
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
