#include "AtmosphereSystem.h"
#include "CloudSystem.h"

#include "Celestial.h"
#include "SResources.h"

#include "unx/render/GpuScene.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <utility>

namespace unx::render::atmosphere
{
using namespace s_detail;

namespace
{
const char* const kStateKey = "s.atmosphere";
uint32_t u32(double v) { return (uint32_t)v; }

struct State
{
    AtmosphereParams params{};
    bool valid = false;
    ComPtr<ID3D12Resource> paramsBuffer, staging;
    ComPtr<ID3D12Resource> transmittance, multiScatter, skyView;
    bool paramsPending = false;   // staging holds params not yet copied by a recorded frame
    bool lutPending = false;      // transmittance + multi-scatter need a build
    bool recordPending = false;   // only the record row needs rewriting (B5 cloud words)
    // Inputs of the last sky view build.
    float3 skySun{};
    float skyAltitude = -1;
    float skyAirglow = 0;  // the airglow radiance the sky view was built with (atmosphere.night_sky_in_lut; 0: none)
    bool skyValid = false;
    AtmosphereStats stats;
    // Celestial objects (B4, Celestial.h): the star buffer (static, built at the first frame that draws stars) and the
    // per-frame record ring (upload heap, kRingSlots x 128 B, raw SRVs).
    static constexpr uint32_t kRingSlots = 4;
    ComPtr<ID3D12Resource> stars, starsStaging, ring;
    uint32_t starsSrv = UINT32_MAX, starCount = 0, ringSrv[kRingSlots] = {};
    uint64_t starsBytes = 0;
    bool starsPending = false;
    uint8_t* ringMapped = nullptr;
    // Wind cache (B6, WindCache.hlsli): the 64^3 texture, and per-frame header (64 B) and record rings.
    ComPtr<ID3D12Resource> windCache, windRing;
    uint32_t windCacheSrv = UINT32_MAX, windHeaderSrv[kRingSlots] = {}, windRecordsSrv[kRingSlots] = {}, windCapacity = 0;
    uint8_t* windMapped = nullptr;
    // Weather (B6, WeatherField.hlsli): the per-frame record ring (96 B slots) and the rain shadow map (512^2 R32F).
    ComPtr<ID3D12Resource> weatherRing, rainMap;
    uint32_t weatherSrv[kRingSlots] = {}, rainMapSrv = UINT32_MAX;
    uint8_t* weatherMapped = nullptr;
    Device* device = nullptr;
    ~State()
    {
        if (!device) return;
        if (ring) ring->Unmap(0, nullptr);
        if (windRing) windRing->Unmap(0, nullptr);
        if (weatherRing) weatherRing->Unmap(0, nullptr);
        for (ComPtr<ID3D12Resource>* r : { std::addressof(stars), std::addressof(starsStaging), std::addressof(ring), std::addressof(windCache), std::addressof(windRing),
                                           std::addressof(weatherRing), std::addressof(rainMap) })
            if (*r) device->deferRelease(*r);
        DescriptorHeaps* h = &device->descriptors();
        for (uint32_t k = 0; k < kRingSlots; ++k)
            for (uint32_t srv : { windHeaderSrv[k], windRecordsSrv[k] })
                if (srv) device->deferCall([h, srv] { h->freeResource(srv); });
        if (windCacheSrv != UINT32_MAX) device->deferCall([h, srv = windCacheSrv] { h->freeResource(srv); });
        for (uint32_t srv : weatherSrv)
            if (srv) device->deferCall([h, srv] { h->freeResource(srv); });
        if (rainMapSrv != UINT32_MAX) device->deferCall([h, srv = rainMapSrv] { h->freeResource(srv); });
        for (uint32_t srv : ringSrv)
            if (srv) device->deferCall([h, srv] { h->freeResource(srv); });
        if (starsSrv != UINT32_MAX) device->deferCall([h, srv = starsSrv] { h->freeResource(srv); });
    }
};

TextureDesc desc(const char* name, uint32_t w, uint32_t h, uint16_t d, D3D12_RESOURCE_DIMENSION dim, DXGI_FORMAT format);  // below

uint32_t rawSrv(Device& device, ID3D12Resource* buffer, uint64_t firstByte, uint64_t bytes)
{
    DescriptorHeaps& h = device.descriptors();
    const uint32_t index = h.allocateResource();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Format = DXGI_FORMAT_R32_TYPELESS;
    sd.Buffer.FirstElement = firstByte / 4;  // 16-byte aligned (raw views)
    sd.Buffer.NumElements = (UINT)(bytes / 4);
    sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device.d3d()->CreateShaderResourceView(buffer, &sd, h.resourceCpu(index));
    return index;
}

// The wind cache (B6): this tick's records (FrameContext::wind) and header into the rings, the 64^3 grid of windAt around
// the camera into the cache texture; publishes fc.resources.wind / windCache (nothing without records).
void publishWind(FramePassContext& fc, State& s)
{
    const WindFrame& w = fc.frame.wind;
    if (w.count == 0 || !w.records) return;
    s.device = &fc.device;
    // The header slot is one record long so the records' structured view starts on an element (80 B: a 64 B header put
    // the view's first element inside it -- NaN winds on the GPU, WindCacheTests).
    constexpr uint32_t kCells = 64, kRecordBytes = 80, kHeaderBytes = 80;
    constexpr float kSpacing = 8;
    if (!s.windCache)
    {
        s.windCache = createTexture(fc.device, L"S wind cache", D3D12_RESOURCE_DIMENSION_TEXTURE3D, kCells, kCells, (uint16_t)kCells, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                    D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
        DescriptorHeaps& h = fc.device.descriptors();
        s.windCacheSrv = h.allocateResource();
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        sd.Texture3D.MipLevels = 1;
        fc.device.d3d()->CreateShaderResourceView(s.windCache.Get(), &sd, h.resourceCpu(s.windCacheSrv));
    }
    if (w.count > s.windCapacity)
    {
        // Rings of kRingSlots slots: header then the records (StructuredBuffer<WindRecord> view).
        if (s.windRing)
        {
            s.windRing->Unmap(0, nullptr);
            fc.device.deferRelease(s.windRing);
            DescriptorHeaps* h = &fc.device.descriptors();
            for (uint32_t k = 0; k < State::kRingSlots; ++k)
                for (uint32_t srv : { s.windHeaderSrv[k], s.windRecordsSrv[k] }) fc.device.deferCall([h, srv] { h->freeResource(srv); });
        }
        s.windCapacity = std::max(256u, w.count);
        const uint64_t slotBytes = kHeaderBytes + (uint64_t)s.windCapacity * kRecordBytes;  // a multiple of 16
        s.windRing = createBuffer(fc.device, L"S wind rings", State::kRingSlots * slotBytes, D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RANGE none{ 0, 0 };
        check(s.windRing->Map(0, &none, reinterpret_cast<void**>(&s.windMapped)), "map wind rings");
        DescriptorHeaps& h = fc.device.descriptors();
        for (uint32_t k = 0; k < State::kRingSlots; ++k)
        {
            s.windHeaderSrv[k] = rawSrv(fc.device, s.windRing.Get(), k * slotBytes, kHeaderBytes);
            s.windRecordsSrv[k] = h.allocateResource();
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Format = DXGI_FORMAT_UNKNOWN;
            sd.Buffer.FirstElement = (k * slotBytes + kHeaderBytes) / kRecordBytes;
            sd.Buffer.NumElements = s.windCapacity;
            sd.Buffer.StructureByteStride = kRecordBytes;
            fc.device.d3d()->CreateShaderResourceView(s.windRing.Get(), &sd, h.resourceCpu(s.windRecordsSrv[k]));
        }
    }
    const uint64_t slotBytes = kHeaderBytes + (uint64_t)s.windCapacity * kRecordBytes;
    const uint32_t slot = (uint32_t)(fc.frame.frameIndex % State::kRingSlots);
    uint8_t* dst = s.windMapped + slot * slotBytes;
    std::memcpy(dst + kHeaderBytes, w.records, (size_t)w.count * kRecordBytes);
    const float3 camera = fc.frame.mainView.position;
    const float3 gridOrigin{ std::floor(camera.x / kSpacing) * kSpacing - kSpacing * kCells / 2, std::floor(camera.y / kSpacing) * kSpacing - kSpacing * kCells / 2,
                             std::floor(camera.z / kSpacing) * kSpacing - kSpacing * kCells / 2 };
    const float header[8] = { (float)w.reference[0], (float)w.reference[1], (float)w.reference[2], (float)w.time, gridOrigin.x, gridOrigin.y, gridOrigin.z, kSpacing };
    const uint32_t tail[4] = { s.windCacheSrv, s.windRecordsSrv[slot], w.count, kCells };
    std::memcpy(dst, header, 32);
    std::memcpy(dst + 32, tail, 16);
    const TextureRef cache = fc.graph.importTexture(s.windCache.Get(), desc("S wind cache", kCells, kCells, (uint16_t)kCells, D3D12_RESOURCE_DIMENSION_TEXTURE3D,
                                                                             DXGI_FORMAT_R16G16B16A16_FLOAT),
                                                     D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
    const uint32_t headerSrv = s.windHeaderSrv[slot];
    ID3D12PipelineState* pso = fc.shaders.compute("Passes/Atmosphere/WindCacheBuild");
    fc.graph.addPass("s.wind.cache", QueueType::Compute, [&](PassBuilder& b) { b.use(cache, Use::UavCompute); },
                     [=](PassContext& c) {
                         const uint32_t k[4] = { headerSrv, c.uav(cache), 0, 0 };
                         c.cmd->SetPipelineState(pso);
                         c.computeConstants(k, 4);
                         c.cmd->Dispatch(kCells / 4, kCells / 4, kCells / 4);
                     });
    fc.resources.wind = headerSrv;
    fc.resources.windCache = cache;
}

// The weather (B6): the World's weather row and the rain shadow map (while it rains, snows or the ground is wet) into the
// frame's record; publishes fc.resources.weather / rainShadow (nothing when every value is 0).
void publishWeather(FramePassContext& fc, State& s)
{
    const WeatherFrame& w = fc.frame.weather;
    const bool rain = w.rainRate > 0 || w.wetness > 0 || w.snowRate > 0 || w.snowDepth > 0;
    if (!rain && w.fogDensity <= 0 && w.cloudCover <= 0) return;
    s.device = &fc.device;
    constexpr uint32_t kRecordBytes = 96, kTexels = 512;
    constexpr float kCell = 0.25f, kTop = 400, kReach = 600;
    if (!s.weatherRing)
    {
        s.weatherRing = createBuffer(fc.device, L"S weather records", State::kRingSlots * kRecordBytes, D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RANGE none{ 0, 0 };
        check(s.weatherRing->Map(0, &none, reinterpret_cast<void**>(&s.weatherMapped)), "map weather records");
        for (uint32_t k = 0; k < State::kRingSlots; ++k) s.weatherSrv[k] = rawSrv(fc.device, s.weatherRing.Get(), k * (uint64_t)kRecordBytes, kRecordBytes);
    }
    const uint32_t slot = (uint32_t)(fc.frame.frameIndex % State::kRingSlots);
    const float3 d = normalize(w.rainDirection);
    const float3 helper = std::fabs(d.x) < 0.9f ? float3{ 1, 0, 0 } : float3{ 0, 0, 1 };
    const float3 u = normalize(cross(helper, d)), v = cross(d, u);
    // The map's plane: normal to the rain, kTop above the camera along it, centred on the camera snapped to whole texels.
    const float3 c = fc.frame.mainView.position;
    const float cu = std::floor(dot(c, u) / kCell) * kCell, cv = std::floor(dot(c, v) / kCell) * kCell, cd = dot(c, d) - kTop;
    const float3 origin = u * (cu - kCell * kTexels / 2) + v * (cv - kCell * kTexels / 2) + d * cd;
    bool map = false;
#if UNX_HAS_RAYTRACING
    map = rain;
#endif
    if (map && !s.rainMap)
    {
        s.rainMap = createTexture(fc.device, L"S rain shadow map", D3D12_RESOURCE_DIMENSION_TEXTURE2D, kTexels, kTexels, 1, DXGI_FORMAT_R32_FLOAT, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
        DescriptorHeaps& h = fc.device.descriptors();
        s.rainMapSrv = h.allocateResource();
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Format = DXGI_FORMAT_R32_FLOAT;
        sd.Texture2D.MipLevels = 1;
        fc.device.d3d()->CreateShaderResourceView(s.rainMap.Get(), &sd, h.resourceCpu(s.rainMapSrv));
    }
    float head[24] = { w.rainRate, w.wetness, w.snowRate, w.snowDepth, w.fogDensity, w.cloudCover, 0, kReach, d.x, d.y, d.z, 0, origin.x, origin.y, origin.z, kCell,
                       u.x, u.y, u.z, 0, v.x, v.y, v.z, 0 };
    const uint32_t mapSrv = map ? s.rainMapSrv : UINT32_MAX, texels = kTexels;
    std::memcpy(&head[11], &mapSrv, 4);
    std::memcpy(&head[19], &texels, 4);
    std::memcpy(s.weatherMapped + slot * (uint64_t)kRecordBytes, head, sizeof head);
    const uint32_t recordSrv = s.weatherSrv[slot];
    fc.resources.weather = recordSrv;
    // The map is traced by R right after its ray scene record (RayTracingTrack.cpp: the frame's TLAS refs exist then).
    if (map)
        fc.resources.rainShadow = fc.graph.importTexture(s.rainMap.Get(), desc("S rain shadow map", kTexels, kTexels, 1, D3D12_RESOURCE_DIMENSION_TEXTURE2D, DXGI_FORMAT_R32_FLOAT),
                                                         D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
}

// This frame's celestial record (FrameContext::celestial) into the ring, and the star buffer the first time stars are
// drawn; publishes fc.resources.celestial (UINT32_MAX when nothing is drawn).
void publishCelestial(FramePassContext& fc, State& s)
{
    const CelestialFrame& f = fc.frame.celestial;
    if ((f.flags & 6u) == 0 && f.airglowRadiance <= 0) return;
    s.device = &fc.device;
    if ((f.flags & 4u) != 0 && !s.stars)
    {
        // A statistically real field until a catalogue is supplied (Celestial.h syntheticStars).
        const std::vector<uint32_t> words = sky::packStars(sky::syntheticStars());
        s.starCount = (uint32_t)sky::syntheticStars().size();
        s.starsBytes = (words.size() * 4 + 15) & ~15ull;
        s.stars = createBuffer(fc.device, L"S star cells and records", s.starsBytes);
        s.starsStaging = createBuffer(fc.device, L"S star staging", s.starsBytes, D3D12_HEAP_TYPE_UPLOAD);
        void* mapped = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(s.starsStaging->Map(0, &none, &mapped), "map star staging");
        std::memcpy(mapped, words.data(), words.size() * 4);
        s.starsStaging->Unmap(0, nullptr);
        s.starsSrv = rawSrv(fc.device, s.stars.Get(), 0, s.starsBytes);
        s.starsPending = true;
    }
    if (s.starsPending)
    {
        const BufferRef starsRef = fc.graph.importBuffer(s.stars.Get(), BufferDesc{ "S star cells and records", s.starsBytes, 0 });
        ID3D12Resource* staging = s.starsStaging.Get();
        const uint64_t bytes = s.starsBytes;
        fc.graph.addPass("s.atmosphere.stars", QueueType::Graphics,
                         [&](PassBuilder& b) {
                             b.use(starsRef, Use::CopyDst);
                             b.keep();
                         },
                         [starsRef, staging, bytes](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(starsRef), 0, staging, 0, bytes); });
        s.starsPending = false;  // the staging buffer is kept (released with the state)
    }
    if (!s.ring)
    {
        s.ring = createBuffer(fc.device, L"S celestial record ring", State::kRingSlots * 128, D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RANGE none{ 0, 0 };
        check(s.ring->Map(0, &none, reinterpret_cast<void**>(&s.ringMapped)), "map celestial ring");
        for (uint32_t k = 0; k < State::kRingSlots; ++k) s.ringSrv[k] = rawSrv(fc.device, s.ring.Get(), k * 128ull, 128);
    }
    // Slot of frame % kRingSlots: that slot's frame has completed (frames in flight <= kRingSlots).
    const uint32_t slot = (uint32_t)(fc.frame.frameIndex % State::kRingSlots);
    uint32_t words[32];
    sky::packCelestialFrame(f, s.stars ? s.starCount : 0, s.stars ? s.starsSrv : UINT32_MAX, words);
    if (s.skyValid && s.skyAirglow > 0) words[25] |= 8u;  // (the sky view holds the airglow: Celestial.hlsli leaves it out)
    std::memcpy(s.ringMapped + slot * 128ull, words, 128);
    fc.resources.celestial = s.ringSrv[slot];
}

TextureDesc desc(const char* name, uint32_t w, uint32_t h, uint16_t d, D3D12_RESOURCE_DIMENSION dim, DXGI_FORMAT format = DXGI_FORMAT_R32G32B32A32_FLOAT)
{
    TextureDesc t;
    t.name = name;
    t.width = w;
    t.height = h;
    t.depthOrArraySize = d;
    t.format = format;
    t.dimension = dim;
    return t;
}
} // namespace

float aerialStartM(const QualityConfig& q)
{
    const double start = q.has("atmosphere.aerial_start_m") ? q.number("atmosphere.aerial_start_m") : 100.0;
    if (!(start >= 0 && start <= 1.0e6)) fail("atmosphere.aerial_start_m in [0, 1e6] m");
    return (float)start;
}

AtmosphereParams makeParams(const scene::Atmosphere& a, const QualityConfig& q, uint32_t mainHeight)
{
    AtmosphereParams p{};
    p.viewStartM = aerialStartM(q);
    p.bottomRadius = a.bottomRadius;
    p.topRadius = a.topRadius;
    p.rayleighScaleHeight = a.rayleighScaleHeight;
    p.mieScaleHeight = a.mieScaleHeight;
    p.rayleighScattering = a.rayleighScattering;
    p.mieG = a.mieG;
    p.mieScattering = a.mieScattering;
    p.ozoneCenter = a.ozoneCenter;
    p.mieAbsorption = a.mieAbsorption;
    p.ozoneWidth = a.ozoneWidth;
    p.ozoneAbsorption = a.ozoneAbsorption;
    p.groundAlbedo = a.groundAlbedo;
    const std::vector<double> t = q.numbers("atmosphere.transmittance_lut"), m = q.numbers("atmosphere.multiscatter_table"),
                              s = q.numbers("atmosphere.sky_view_lut");
    if (t.size() != 2 || s.size() != 2) fail("atmosphere: LUT size keys need [w, h]");
    if (m.size() != 4) fail("atmosphere.multiscatter_table needs [nu, mu_s, mu, r]");
    p.transmittanceSize[0] = u32(t[0]);
    p.transmittanceSize[1] = u32(t[1]);
    for (int i = 0; i < 4; ++i) p.multiScatterSize[i] = u32(m[i]);
    p.multiScatterOrders = (uint32_t)q.integer("atmosphere.multiscatter_orders");
    p.multiScatterShOrder = (uint32_t)q.integer("atmosphere.multiscatter_sh_order");
    const std::vector<double> grid = q.numbers("atmosphere.multiscatter_sh_grid");
    if (grid.size() != 2) fail("atmosphere.multiscatter_sh_grid needs [elevation nodes per half, azimuth nodes]");
    p.multiScatterShGrid[0] = u32(grid[0]);
    p.multiScatterShGrid[1] = u32(grid[1]);
    p.skyViewSize[0] = u32(s[0]);
    p.skyViewSize[1] = u32(s[1]);
    p.froxelSlices = (uint32_t)q.integer("atmosphere.froxels.depth_slices");
    p.froxelFarM = (float)q.number("atmosphere.froxels.far_m");
    p.froxelTilePx = froxelTilePx(q, mainHeight);  // (Frame.h: the frame's tile size)
    p.froxelNearM = (float)q.number("atmosphere.froxels.near_m");
    p.transmittanceSteps = (uint32_t)q.integer("atmosphere.transmittance_steps");
    p.multiScatterDirections = (uint32_t)q.integer("atmosphere.multiscatter_directions");
    p.multiScatterSteps = (uint32_t)q.integer("atmosphere.multiscatter_steps");
    p.skySegments = (uint32_t)q.integer("atmosphere.sky_view_segments");
    if (p.skyViewSize[1] % 2 || p.skyViewSize[1] < 4) fail("atmosphere.sky_view_lut height must be even (two halves split at the horizon)");
    if (p.transmittanceSize[0] < 10 || p.transmittanceSize[1] < 2)
        fail("atmosphere: the transmittance LUT needs >= 2 rows and >= 10 columns (the parameter row)");
    if (p.multiScatterSize[0] < 2 || p.multiScatterSize[1] < 2 || p.multiScatterSize[2] < 4 || p.multiScatterSize[2] % 2 || p.multiScatterSize[3] < 2 ||
        p.multiScatterSize[1] > 2048 || p.multiScatterSize[2] > 2048 || p.multiScatterSize[0] * p.multiScatterSize[3] > 2048)
        fail("atmosphere.multiscatter_table: >= 2 texels per axis, mu even and >= 4 (two halves split at the horizon), mu_s, mu and nu x r <= 2048");
    if (p.multiScatterOrders < 2 || p.multiScatterDirections < 2 || p.multiScatterSteps < 1 || p.multiScatterShOrder > 48 ||
        p.multiScatterShGrid[0] < 1 || p.multiScatterShGrid[1] < 1 || p.multiScatterShGrid[1] > 256)
        fail("atmosphere: multiscatter_orders >= 2, directions >= 2, steps >= 1, sh_order <= 48 (MS_SH_MAX), 1 <= sh_grid, azimuths <= 256 (MS_RING_MAX)");
    if (!(p.bottomRadius > 0 && p.topRadius > p.bottomRadius && p.rayleighScaleHeight > 0 && p.mieScaleHeight > 0 && std::abs(p.mieG) < 1 && p.ozoneWidth > 0))
        fail("atmosphere: invalid shell, scale heights or Mie g");
    return p;
}

const AtmosphereStats& stats(TrackState& state) { return state.get<State>(kStateKey).stats; }

namespace
{
// The multiple-scattering source table J_ms by iterating orders (MsBuild.hlsl): PASS 0 (L_1), PASS 3 (E_1), then per
// order n = 2..N PASS 5 (spherical-harmonic projection of L_{n-1}), PASS 1 (J_n), PASS 2 (L_n), PASS 3 (E_n), and PASS 4
// (tail, the log-domain table, ground irradiance row). The build tables are transient (graph resources of the building
// frame): 3 x R16G16B16A16_UNORM log tables, the J_acc buffer and the coefficients (N_mus N_r (L + 1)(L + 2) / 2 x 16 B).
void recordMultipleScattering(RenderGraph& g, ShaderLibrary& sh, const AtmosphereParams& p, BufferRef params, TextureRef tlut, TextureRef mlut)
{
    const uint32_t W = p.multiScatterSize[0] * p.multiScatterSize[1], H = p.multiScatterSize[2], D = p.multiScatterSize[3];
    const uint64_t texels = (uint64_t)W * H * D;
    TextureDesc td = desc("S ms radiance", p.multiScatterSize[1], H, (uint16_t)(p.multiScatterSize[0] * D), D3D12_RESOURCE_DIMENSION_TEXTURE3D,
                          DXGI_FORMAT_R16G16B16A16_UNORM);
    const TextureRef radiance = g.createTexture(td);
    td.name = "S ms source A";
    const TextureRef sourceA = g.createTexture(td);
    td.name = "S ms source B";
    const TextureRef sourceB = g.createTexture(td);
    const BufferRef acc = g.createBuffer(BufferDesc{ "S ms source sum", texels * 16, 16 });
    const BufferRef irradiance = g.createBuffer(BufferDesc{ "S ms ground irradiance", 2ull * p.transmittanceSize[0] * 16, 16 });
    const uint32_t shL = p.multiScatterShOrder, shCount = (shL + 1) * (shL + 2) / 2;
    const BufferRef coefficients = g.createBuffer(BufferDesc{ "S ms density coefficients", (uint64_t)p.multiScatterSize[1] * D * shCount * 16, 16 });
    ID3D12PipelineState* pso[6];
    for (int i = 0; i < 6; ++i) pso[i] = sh.compute(format("Passes/Atmosphere/MsBuild.PASS%d", i));
    const uint32_t nMus = p.multiScatterSize[1];
    const uint32_t gx = groups(W, 64), ge = groups(p.transmittanceSize[0], 64);
    auto irradiancePass = [&](bool first) {
        ID3D12PipelineState* ps = pso[3];
        g.addPass("s.atmosphere.ms.irradiance", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(params, Use::SrvCompute);
                      b.use(tlut, Use::SrvCompute);
                      b.use(radiance, Use::SrvCompute);
                      b.use(irradiance, Use::UavCompute);
                  },
                  [=](PassContext& c) {
                      const uint32_t k[8] = { c.srv(params), c.srv(tlut), c.srv(radiance), c.uav(irradiance), first ? 1u : 0u, p.multiScatterDirections, 0, 0 };
                      c.cmd->SetPipelineState(ps);
                      c.computeConstants(k, 8);
                      c.cmd->Dispatch(ge, 1, 1);
                  });
    };
    g.addPass("s.atmosphere.ms.single", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(params, Use::SrvCompute);
                  b.use(tlut, Use::SrvCompute);
                  b.use(radiance, Use::UavCompute);
              },
              [=](PassContext& c) {
                  c.cmd->SetPipelineState(pso[0]);
                  for (uint32_t z = 0; z < D; ++z)
                  {
                      const uint32_t k[8] = { c.srv(params), c.srv(tlut), c.uav(radiance), 0, 0, 0, 0, z };
                      c.computeConstants(k, 8);
                      c.cmd->Dispatch(gx, H, 1);
                  }
              });
    irradiancePass(true);
    TextureRef current = sourceA, previous = sourceB;
    for (uint32_t n = 2; n <= p.multiScatterOrders; ++n)
    {
        if (n > 2) std::swap(current, previous);  // J_n alternates between A and B (order 2 writes A)
        const TextureRef jn = current;
        g.addPass("s.atmosphere.ms.project", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(params, Use::SrvCompute);
                      b.use(tlut, Use::SrvCompute);
                      b.use(radiance, Use::SrvCompute);
                      b.use(coefficients, Use::UavCompute);
                  },
                  [=](PassContext& c) {
                      c.cmd->SetPipelineState(pso[5]);
                      for (uint32_t z = 0; z < D; ++z)
                      {
                          const uint32_t k[8] = { c.srv(params), c.srv(tlut), c.srv(radiance), c.uav(coefficients), shL, p.multiScatterShGrid[0], p.multiScatterShGrid[1], z };
                          c.computeConstants(k, 8);
                          c.cmd->Dispatch(nMus, 1, 1);
                      }
                  });
        g.addPass("s.atmosphere.ms.density", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(params, Use::SrvCompute);
                      b.use(tlut, Use::SrvCompute);
                      b.use(coefficients, Use::SrvCompute);
                      b.use(jn, Use::UavCompute);
                      b.use(acc, Use::UavCompute);
                  },
                  [=](PassContext& c) {
                      c.cmd->SetPipelineState(pso[1]);
                      for (uint32_t z = 0; z < D; ++z)
                      {
                          const uint32_t k[8] = { c.srv(params), c.srv(tlut), c.srv(coefficients), c.uav(jn), c.uav(acc), shL, n == 2 ? 1u : 0u, z };
                          c.computeConstants(k, 8);
                          c.cmd->Dispatch(nMus, 1, 1);
                      }
                  });
        g.addPass("s.atmosphere.ms.radiance", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(params, Use::SrvCompute);
                      b.use(tlut, Use::SrvCompute);
                      b.use(jn, Use::SrvCompute);
                      b.use(irradiance, Use::SrvCompute);
                      b.use(radiance, Use::UavCompute);
                  },
                  [=](PassContext& c) {
                      c.cmd->SetPipelineState(pso[2]);
                      for (uint32_t z = 0; z < D; ++z)
                      {
                          const uint32_t k[8] = { c.srv(params), c.srv(tlut), c.srv(jn), c.uav(radiance), c.srv(irradiance), 0, 0, z };
                          c.computeConstants(k, 8);
                          c.cmd->Dispatch(gx, H, 1);
                      }
                  });
        irradiancePass(false);
    }
    const bool tail = p.multiScatterOrders >= 3;
    const TextureRef last = current, before = previous;
    g.addPass("s.atmosphere.ms.final", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(params, Use::SrvCompute);
                  b.use(tlut, Use::UavCompute);
                  b.use(acc, Use::SrvCompute);
                  b.use(last, Use::SrvCompute);
                  if (tail) b.use(before, Use::SrvCompute);
                  b.use(irradiance, Use::SrvCompute);
                  b.use(mlut, Use::UavCompute);
                  b.keep();
              },
              [=](PassContext& c) {
                  c.cmd->SetPipelineState(pso[4]);
                  for (uint32_t z = 0; z < D; ++z)
                  {
                      const uint32_t k[8] = { c.srv(params), c.uav(tlut), c.srv(acc), c.srv(last), tail ? c.srv(before) : 0xFFFFFFFFu, c.uav(mlut), c.srv(irradiance), z };
                      c.computeConstants(k, 8);
                      c.cmd->Dispatch(std::max(gx, ge), H, 1);
                  }
              });
}
} // namespace

void record(FramePassContext& fc)
{
    State& s = fc.state<State>(kStateKey);
    const scene::Scene* src = fc.scene.source();
    const scene::Atmosphere atm = src ? src->atmosphere : scene::Atmosphere{};
    AtmosphereParams p = makeParams(atm, fc.quality, fc.frame.mainView.height);
    cloudsPrepare(fc, p.clouds);  // B5: the cloud layer's record in the atmosphere's (0 without clouds)

    // atmosphere.rebuild_every_frame (measurement only): the transmittance LUT and the J_ms table are rebuilt every frame,
    // so a gate times the build (it otherwise runs only when the medium changes).
    const bool rebuildEveryFrame = fc.quality.integer("atmosphere.rebuild_every_frame") != 0;
    if (rebuildEveryFrame && s.valid) s.lutPending = true;
    // Only the record's cloud word or the air's start changed (clouds turned on or off, the view resized, a test's
    // override): the record row is rewritten and the LUTs stay.
    bool recordOnly = false;
    if (s.valid && std::memcmp(&p, &s.params, sizeof p) != 0)
    {
        AtmosphereParams a = p, b = s.params;
        a.clouds = b.clouds = 0;
        a.viewStartM = b.viewStartM = 0;
        recordOnly = std::memcmp(&a, &b, sizeof a) == 0;
    }
    if (recordOnly)
    {
        if (s.staging) fc.device.deferRelease(s.staging);
        s.staging = createBuffer(fc.device, L"S atmosphere params staging", 256, D3D12_HEAP_TYPE_UPLOAD);
        void* mapped = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(s.staging->Map(0, &none, &mapped), "map atmosphere params");
        std::memcpy(mapped, &p, sizeof p);
        s.staging->Unmap(0, nullptr);
        s.params = p;
        s.paramsPending = true;
        s.recordPending = true;
    }
    else if (!s.valid || std::memcmp(&p, &s.params, sizeof p) != 0)
    {
        const bool sizes = !s.valid || std::memcmp(p.transmittanceSize, s.params.transmittanceSize, 8) || std::memcmp(p.multiScatterSize, s.params.multiScatterSize, 16) ||
                           std::memcmp(p.skyViewSize, s.params.skyViewSize, 8);
        if (sizes)
        {
            for (ComPtr<ID3D12Resource>* r : { std::addressof(s.transmittance), std::addressof(s.multiScatter), std::addressof(s.skyView) })
                if (*r) fc.device.deferRelease(*r);
            const auto T2 = D3D12_RESOURCE_DIMENSION_TEXTURE2D, T3 = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
            const auto L = D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
            s.transmittance = createTexture(fc.device, L"S transmittance LUT", T2, p.transmittanceSize[0], p.transmittanceSize[1] + 2, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, L);
            s.multiScatter = createTexture(fc.device, L"S multiple-scattering table", T3, p.multiScatterSize[1], p.multiScatterSize[2],
                                           (uint16_t)(p.multiScatterSize[0] * p.multiScatterSize[3]), DXGI_FORMAT_R16G16B16A16_UNORM, L);
            s.skyView = createTexture(fc.device, L"S sky view LUT", T2, p.skyViewSize[0], 3 * p.skyViewSize[1], 1, DXGI_FORMAT_R32G32B32A32_FLOAT, L);
        }
        if (!s.paramsBuffer) s.paramsBuffer = createBuffer(fc.device, L"S atmosphere params", 256);
        if (s.staging) fc.device.deferRelease(s.staging);
        s.staging = createBuffer(fc.device, L"S atmosphere params staging", 256, D3D12_HEAP_TYPE_UPLOAD);
        void* mapped = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(s.staging->Map(0, &none, &mapped), "map atmosphere params");
        std::memcpy(mapped, &p, sizeof p);
        s.staging->Unmap(0, nullptr);
        s.params = p;
        s.valid = true;
        s.paramsPending = true;
        s.lutPending = true;
        s.skyValid = false;
    }

    RenderGraph& g = fc.graph;
    const auto L = D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
    const uint32_t msW = p.multiScatterSize[1], msH = p.multiScatterSize[2];
    const uint16_t msD = (uint16_t)(p.multiScatterSize[0] * p.multiScatterSize[3]);
    const TextureRef tlut = g.importTexture(s.transmittance.Get(), desc("S transmittance LUT", p.transmittanceSize[0], p.transmittanceSize[1] + 2, 1, D3D12_RESOURCE_DIMENSION_TEXTURE2D), L);
    const TextureRef mlut = g.importTexture(s.multiScatter.Get(), desc("S multiple-scattering table", msW, msH, msD, D3D12_RESOURCE_DIMENSION_TEXTURE3D, DXGI_FORMAT_R16G16B16A16_UNORM), L);
    const TextureRef sky = g.importTexture(s.skyView.Get(), desc("S sky view LUT", p.skyViewSize[0], 3 * p.skyViewSize[1], 1, D3D12_RESOURCE_DIMENSION_TEXTURE2D), L);
    const BufferRef params = g.importBuffer(s.paramsBuffer.Get(), BufferDesc{ "S atmosphere params", 256, 0 });
    fc.resources.transmittanceLut = tlut;
    fc.resources.multiScatterLut = mlut;
    fc.resources.skyViewLut = sky;

    if (s.paramsPending)
    {
        ID3D12Resource* staging = s.staging.Get();
        g.addPass("s.atmosphere.params", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(params, Use::CopyDst);
                      b.keep();
                  },
                  [params, staging](PassContext& c) { c.cmd->CopyBufferRegion(c.resource(params), 0, staging, 0, 256); });
        s.paramsPending = false;
    }

    ShaderLibrary& sh = fc.shaders;
    if (s.recordPending && !s.lutPending)
    {
        ID3D12PipelineState* pr = sh.compute("Passes/Atmosphere/AtmosphereRecord");
        g.addPass("s.atmosphere.record", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(params, Use::SrvCompute);
                      b.use(tlut, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& c) {
                      const uint32_t k[4] = { c.srv(params), c.uav(tlut), 0, 0 };
                      c.cmd->SetPipelineState(pr);
                      c.computeConstants(k, 4);
                      c.cmd->Dispatch(1, 1, 1);
                  });
    }
    s.recordPending = false;
    if (s.lutPending)
    {
        ID3D12PipelineState* pt = sh.compute("Passes/Atmosphere/Transmittance");
        g.addPass("s.atmosphere.transmittance", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(params, Use::SrvCompute);
                      b.use(tlut, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& c) {
                      const uint32_t k[4] = { c.srv(params), c.uav(tlut), 0, 0 };
                      c.cmd->SetPipelineState(pt);
                      c.computeConstants(k, 4);
                      c.cmd->Dispatch(groups(p.transmittanceSize[0], 8), groups(p.transmittanceSize[1], 8), 1);
                  });
        recordMultipleScattering(g, sh, p, params, tlut, mlut);
        s.lutPending = false;
        ++s.stats.lutBuilds;
    }

    const ViewDesc& mv = fc.frame.mainView;
    const float3 sun = src ? src->sun.direction : scene::Sun{}.direction;
    // Camera altitude above the spherical surface (the sky view is built for it).
    const double R = p.bottomRadius;
    const double px = mv.position.x, py = mv.position.y, pz = mv.position.z;
    const double h2 = px * px + py * py + pz * pz + 2 * R * py;
    const float altitude = (float)(h2 / (std::sqrt(std::max(0.0, R * R + h2)) + R));
    D3D12_GPU_VIRTUAL_ADDRESS frameConstants = 0;
    auto constants = [&]() {
        if (!frameConstants) frameConstants = fc.frameConstantsFor(mv);
        return frameConstants;
    };

    // The camera altitude changes the sky over the atmosphere's scale heights (1.2 - 8 km) and moves the horizon by
    // 1 / sqrt(2 h R) rad per metre (2e-4 rad/m at 1.8 m): a rebuild for every vertical step (walking, head bob, stairs,
    // the curvature term away from the origin) changed no displayed value. The view is rebuilt when the altitude moved
    // by more than max(0.25 m, 1e-4 altitude) since its build: below 1e-4 of the radiance and 0.1 px of horizon at
    // 960 px (the readers already take it for points tens of metres from the camera).
    const float altitudeTolerance = std::max(0.25f, 1e-4f * std::fabs(altitude));
    // atmosphere.night_sky_in_lut: the frame's airglow (FrameContext::celestial) goes into the sky view (SkyView.hlsl), so
    // the rays that escape to the sky - GI, reflections, the cloud dome's background - have the night's floor of light,
    // not only the sky pixels; the view is rebuilt when the value changes.
    const bool nightInLut = !fc.quality.has("atmosphere.night_sky_in_lut") || fc.quality.boolean("atmosphere.night_sky_in_lut");
    const float airglow = nightInLut ? std::max(fc.frame.celestial.airglowRadiance, 0.0f) : 0.0f;
    if (!s.skyValid || std::memcmp(&sun, &s.skySun, sizeof sun) != 0 || !(std::fabs(altitude - s.skyAltitude) <= altitudeTolerance) || airglow != s.skyAirglow)
    {
        ID3D12PipelineState* ps = sh.compute("Passes/Atmosphere/SkyView");
        const D3D12_GPU_VIRTUAL_ADDRESS cb = constants();
        g.addPass("s.atmosphere.skyview", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(params, Use::SrvCompute);
                      b.use(tlut, Use::SrvCompute);
                      b.use(mlut, Use::SrvCompute);
                      b.use(sky, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& c) {
                      uint32_t k[8] = { c.srv(params), c.srv(tlut), c.srv(mlut), c.uav(sky), 0, 0, 0, 0 };
                      std::memcpy(&k[4], &airglow, 4);  // P[1].x: the night sky's own light (nits at the zenith)
                      c.cmd->SetPipelineState(ps);
                      c.bindFrameConstants(cb);
                      c.computeConstants(k, 8);
                      c.cmd->Dispatch(p.skyViewSize[0], p.skyViewSize[1], 1);  // one group per texel (SkyView.hlsl)
                  });
        s.skySun = sun;
        s.skyAltitude = altitude;
        s.skyAirglow = airglow;
        s.skyValid = true;
        ++s.stats.skyViewBuilds;
    }
    publishCelestial(fc, s);
    publishWind(fc, s);
    publishWeather(fc, s);
    cloudsRecord(fc, tlut);  // B5 (CloudSystem.cpp): nothing without clouds
}
} // namespace unx::render::atmosphere
