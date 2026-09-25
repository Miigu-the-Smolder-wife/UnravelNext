#include "AtmosphereSystem.h"

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
    // Inputs of the last sky view build.
    float3 skySun{};
    float skyAltitude = -1;
    bool skyValid = false;
    AtmosphereStats stats;
};

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

AtmosphereParams makeParams(const scene::Atmosphere& a, const QualityConfig& q)
{
    AtmosphereParams p{};
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
    p.froxelTilePx = (uint32_t)q.integer("atmosphere.froxels.tile_px");
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
        p.multiScatterShGrid[0] < 1 || p.multiScatterShGrid[1] < 1)
        fail("atmosphere: multiscatter_orders >= 2, directions >= 2, steps >= 1, sh_order <= 48 (MS_SH_MAX), sh_grid >= 1");
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
    const AtmosphereParams p = makeParams(atm, fc.quality);

    // atmosphere.rebuild_every_frame (measurement only): the transmittance LUT and the J_ms table are rebuilt every frame,
    // so a gate times the build (it otherwise runs only when the medium changes).
    const bool rebuildEveryFrame = fc.quality.integer("atmosphere.rebuild_every_frame") != 0;
    if (rebuildEveryFrame && s.valid) s.lutPending = true;
    if (!s.valid || std::memcmp(&p, &s.params, sizeof p) != 0)
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
            s.skyView = createTexture(fc.device, L"S sky view LUT", T2, p.skyViewSize[0], p.skyViewSize[1], 1, DXGI_FORMAT_R32G32B32A32_FLOAT, L);
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
    const TextureRef sky = g.importTexture(s.skyView.Get(), desc("S sky view LUT", p.skyViewSize[0], p.skyViewSize[1], 1, D3D12_RESOURCE_DIMENSION_TEXTURE2D), L);
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

    if (!s.skyValid || std::memcmp(&sun, &s.skySun, sizeof sun) != 0 || altitude != s.skyAltitude)
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
                      const uint32_t k[4] = { c.srv(params), c.srv(tlut), c.srv(mlut), c.uav(sky) };
                      c.cmd->SetPipelineState(ps);
                      c.bindFrameConstants(cb);
                      c.computeConstants(k, 4);
                      c.cmd->Dispatch(p.skyViewSize[0], p.skyViewSize[1], 1);  // one group per texel (SkyView.hlsl)
                  });
        s.skySun = sun;
        s.skyAltitude = altitude;
        s.skyValid = true;
        ++s.stats.skyViewBuilds;
    }
}
} // namespace unx::render::atmosphere
