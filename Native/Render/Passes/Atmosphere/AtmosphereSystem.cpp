#include "AtmosphereSystem.h"

#include "SResources.h"

#include "unx/render/GpuScene.h"

#include <cmath>
#include <cstring>
#include <memory>

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

TextureDesc desc(const char* name, uint32_t w, uint32_t h, uint16_t d, D3D12_RESOURCE_DIMENSION dim)
{
    TextureDesc t;
    t.name = name;
    t.width = w;
    t.height = h;
    t.depthOrArraySize = d;
    t.format = DXGI_FORMAT_R32G32B32A32_FLOAT;
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
    const std::vector<double> t = q.numbers("atmosphere.transmittance_lut"), m = q.numbers("atmosphere.multiscatter_lut"),
                              s = q.numbers("atmosphere.sky_view_lut");
    if (t.size() != 2 || m.size() != 2 || s.size() != 2) fail("atmosphere: LUT size keys need [w, h]");
    p.transmittanceSize[0] = u32(t[0]);
    p.transmittanceSize[1] = u32(t[1]);
    p.multiScatterSize[0] = u32(m[0]);
    p.multiScatterSize[1] = u32(m[1]);
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
    if (p.transmittanceSize[0] < 2 || p.transmittanceSize[1] < 2 || p.multiScatterSize[0] < 2 || p.multiScatterSize[1] < 2 || p.multiScatterSize[0] < 9)
        fail("atmosphere: LUTs need >= 2 texels per axis (multi-scatter width >= 9 for the parameter row)");
    if (!(p.bottomRadius > 0 && p.topRadius > p.bottomRadius && p.rayleighScaleHeight > 0 && p.mieScaleHeight > 0 && std::abs(p.mieG) < 1 && p.ozoneWidth > 0))
        fail("atmosphere: invalid shell, scale heights or Mie g");
    return p;
}

const AtmosphereStats& stats(TrackState& state) { return state.get<State>(kStateKey).stats; }

void record(FramePassContext& fc)
{
    State& s = fc.state<State>(kStateKey);
    const scene::Scene* src = fc.scene.source();
    const scene::Atmosphere atm = src ? src->atmosphere : scene::Atmosphere{};
    const AtmosphereParams p = makeParams(atm, fc.quality);

    if (!s.valid || std::memcmp(&p, &s.params, sizeof p) != 0)
    {
        const bool sizes = !s.valid || std::memcmp(p.transmittanceSize, s.params.transmittanceSize, 8) || std::memcmp(p.multiScatterSize, s.params.multiScatterSize, 8) ||
                           std::memcmp(p.skyViewSize, s.params.skyViewSize, 8);
        if (sizes)
        {
            for (ComPtr<ID3D12Resource>* r : { std::addressof(s.transmittance), std::addressof(s.multiScatter), std::addressof(s.skyView) })
                if (*r) fc.device.deferRelease(*r);
            const auto T2 = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            const auto L = D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
            s.transmittance = createTexture(fc.device, L"S transmittance LUT", T2, p.transmittanceSize[0], p.transmittanceSize[1], 1, DXGI_FORMAT_R32G32B32A32_FLOAT, L);
            s.multiScatter = createTexture(fc.device, L"S multiple scattering LUT", T2, p.multiScatterSize[0], p.multiScatterSize[1] + 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT, L);
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
    const TextureRef tlut = g.importTexture(s.transmittance.Get(), desc("S transmittance LUT", p.transmittanceSize[0], p.transmittanceSize[1], 1, D3D12_RESOURCE_DIMENSION_TEXTURE2D), L);
    const TextureRef mlut = g.importTexture(s.multiScatter.Get(), desc("S multi-scatter LUT", p.multiScatterSize[0], p.multiScatterSize[1] + 1, 1, D3D12_RESOURCE_DIMENSION_TEXTURE2D), L);
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
        ID3D12PipelineState* pm = sh.compute("Passes/Atmosphere/MultiScatter");
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
        g.addPass("s.atmosphere.multiscatter", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(params, Use::SrvCompute);
                      b.use(tlut, Use::SrvCompute);
                      b.use(mlut, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& c) {
                      const uint32_t k[4] = { c.srv(params), c.srv(tlut), c.uav(mlut), 0 };
                      c.cmd->SetPipelineState(pm);
                      c.computeConstants(k, 4);
                      c.cmd->Dispatch(p.multiScatterSize[0], p.multiScatterSize[1], 1);  // one group per texel (MultiScatter.hlsl)
                  });
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
                      c.cmd->Dispatch(groups(p.skyViewSize[0], 8), groups(p.skyViewSize[1], 8), 1);
                  });
        s.skySun = sun;
        s.skyAltitude = altitude;
        s.skyValid = true;
        ++s.stats.skyViewBuilds;
    }
}
} // namespace unx::render::atmosphere
