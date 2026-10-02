// gi.lumen: the screen-probe final gather (Passes/GI/Lumen/*.hlsl; structure after Unreal's Lumen screen probe gather,
// Docs/Status/LUMEN_GATHER_KO.md). Replaces r.gi.screen and its filters as the maker of view.giIrradiance; the world
// cache keeps updating (its values light the probes' hits until the surface cache's read replaces them).
// Frame: place -> adaptive mark / spawn -> screen data (BRDF density, disocclusion) -> lighting density (last frame's
// probes) -> rays (structured importance sampling) -> trace -> composite -> spatial filter x N -> irradiance and
// bordered radiance -> integrate (per pixel) -> temporal (per pixel).
#include "unx/gi/GiSystem.h"

#include "unx/gi/LumenRadianceCache.h"
#include "unx/gi/LumenTranslucencyVolume.h"
#include "unx/rt/RayPipeline.h"
#if defined(UNX_GI_HAS_SHADING)
#include "unx/shading/Exposure.h"
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace unx::render::gi
{
namespace
{
uint32_t bits(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
} // namespace

void GiSystem::ensureLumen(uint32_t width, uint32_t height, uint32_t atlasX, uint32_t atlasY)
{
    LumenState& st = m_lumen;
    if (st.diffuse[0] && st.width == width && st.height == height) return;
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
    const auto create = [&](ComPtr<ID3D12Resource>& t, DXGI_FORMAT format, uint32_t w, uint32_t h, const wchar_t* name) {
        if (t) m_device.deferRelease(t);
        D3D12_RESOURCE_DESC1 d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = w;
        d.Height = h;
        d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        d.Format = format;
        check(m_device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr, nullptr, 0, nullptr,
                                                       IID_PPV_ARGS(&t)),
              "GI lumen history");
        t->SetName(name);
    };
    for (int k = 0; k < 2; ++k)
    {
        create(st.probeDepth[k], DXGI_FORMAT_R32_FLOAT, atlasX, atlasY, k ? L"R lumen probe depth 1" : L"R lumen probe depth 0");
        create(st.probePosition[k], DXGI_FORMAT_R32G32B32A32_FLOAT, atlasX, atlasY, k ? L"R lumen probe position 1" : L"R lumen probe position 0");
        create(st.probeRadiance[k], DXGI_FORMAT_R16G16B16A16_FLOAT, atlasX * 8, atlasY * 8, k ? L"R lumen probe radiance 1" : L"R lumen probe radiance 0");
        create(st.diffuse[k], DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, k ? L"R lumen diffuse 1" : L"R lumen diffuse 0");
        create(st.specular[k], DXGI_FORMAT_R16G16B16A16_FLOAT, width, height, k ? L"R lumen rough specular 1" : L"R lumen rough specular 0");
        create(st.keys[k], DXGI_FORMAT_R32G32_UINT, width, height, k ? L"R lumen keys 1" : L"R lumen keys 0");
    }
    st.width = width;
    st.height = height;
    st.valid = false;
}

void GiSystem::recordLumen(FramePassContext& fc, ViewResources& view, BufferRef cache, rt::RayScene& rays)
{
    RenderGraph& g = fc.graph;
    const GiSettings::Lumen L = m_settings.lumen;
    ShaderLibrary& shaders = fc.shaders;
    LumenState& st = m_lumen;
    const uint32_t width = view.view.width, height = view.view.height;
    const uint32_t probesX = (width + L.tile - 1) / L.tile, probesY = (height + L.tile - 1) / L.tile;
    const uint32_t uniform = probesX * probesY;
    const uint32_t maxAdaptive = (uint32_t)((float)uniform * L.adaptiveFraction);
    const uint32_t atlasRows = probesY + (maxAdaptive + probesX - 1) / probesX;
    ensureLumen(width, height, probesX, atlasRows);

    const float3 shift = fc.frame.originShift;
    if (fc.scene.revision() != st.revision || m_epoch != st.epoch || fc.frame.discontinuity != 0 || shift.x != 0 || shift.y != 0 || shift.z != 0) st.valid = false;
    st.revision = fc.scene.revision();
    st.epoch = m_epoch;
    const float exposure = 1.0f / (1.2f * std::exp2(view.view.ev100));
    const bool historyValid = st.valid && st.prevExposure > 0;
    const float exposureRatio = historyValid ? exposure / st.prevExposure : 1.0f;
    const float4x4 prevInvViewProj = st.prevInvViewProj;
    const uint32_t temporalIndex = (uint32_t)(fc.frame.frameIndex % 8), prevTemporalIndex = st.prevTemporalIndex;
    const uint32_t rayIndex = (uint32_t)(fc.frame.frameIndex % L.rayDirections);
    const uint32_t prev = st.parity, next = prev ^ 1u;
    st.parity = next;
    st.prevExposure = exposure;
    st.prevInvViewProj = view.view.invViewProj;
    st.prevTemporalIndex = temporalIndex;
    st.valid = true;
    // The frame before this one was a first frame, a cut or a restore (it had no history itself).
    const bool previousFrameWasCut = !st.previousHadHistory;
    st.previousHadHistory = historyValid;
    // Stochastic interpolation (one probe of a pixel's 4 drawn at random) leans on the pixels' history to average the
    // draws: on a cut's first frames there is none (bath, f600: neighbouring pixels 4 x apart, the picture grainy until
    // f603) - until the history holds 4 frames every pixel blends its 4 probes.
    st.framesWithHistory = historyValid ? st.framesWithHistory + 1 : 0;
    const bool stochasticInterpolation = L.stochasticInterpolation && st.framesWithHistory >= 4;

    const auto import = [&](ComPtr<ID3D12Resource>& t, const char* name, uint32_t w, uint32_t h, DXGI_FORMAT format) {
        return g.importTexture(t.Get(), TextureDesc{ name, w, h, 1, 1, format }, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    };
    const uint32_t traceX = probesX * 8, traceY = atlasRows * 8;
    const TextureRef probeDepth = import(st.probeDepth[next], "lumen probe depth", probesX, atlasRows, DXGI_FORMAT_R32_FLOAT);
    const TextureRef probePosition = import(st.probePosition[next], "lumen probe position", probesX, atlasRows, DXGI_FORMAT_R32G32B32A32_FLOAT);
    const TextureRef probeRadiance = import(st.probeRadiance[next], "lumen probe radiance", traceX, traceY, DXGI_FORMAT_R16G16B16A16_FLOAT);
    const TextureRef prevProbeDepth = import(st.probeDepth[prev], "lumen probe depth (previous)", probesX, atlasRows, DXGI_FORMAT_R32_FLOAT);
    const TextureRef prevProbePosition = import(st.probePosition[prev], "lumen probe position (previous)", probesX, atlasRows, DXGI_FORMAT_R32G32B32A32_FLOAT);
    const TextureRef prevProbeRadiance = import(st.probeRadiance[prev], "lumen probe radiance (previous)", traceX, traceY, DXGI_FORMAT_R16G16B16A16_FLOAT);
    const TextureRef diffuse = import(st.diffuse[next], "lumen diffuse", width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    const TextureRef specular = import(st.specular[next], "lumen rough specular", width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    const TextureRef keys = import(st.keys[next], "lumen keys", width, height, DXGI_FORMAT_R32G32_UINT);
    const TextureRef prevDiffuse = import(st.diffuse[prev], "lumen diffuse (previous)", width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    const TextureRef prevSpecular = import(st.specular[prev], "lumen rough specular (previous)", width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
    const TextureRef prevKeys = import(st.keys[prev], "lumen keys (previous)", width, height, DXGI_FORMAT_R32G32_UINT);

    const TextureRef probeNormal = g.createTexture({ "lumen probe normal", probesX, atlasRows, 1, 1, DXGI_FORMAT_R16G16_UNORM });
    const TextureRef mask = g.createTexture({ "lumen adaptive mask", probesX, probesY, 1, 1, DXGI_FORMAT_R32_UINT });
    const uint64_t adaptiveBytes = 16 + (uint64_t)maxAdaptive * 4 + (uint64_t)uniform * 4 * 9;
    const BufferRef adaptive = g.createBuffer({ "lumen adaptive probes", adaptiveBytes, 0 });
    const BufferRef brdf = g.createBuffer({ "lumen BRDF density", (uint64_t)probesX * atlasRows * 40, 0 });
    const TextureRef lightingPdf = g.createTexture({ "lumen lighting density", traceX, traceY, 1, 1, DXGI_FORMAT_R16_FLOAT });
    const TextureRef rayInfo = g.createTexture({ "lumen ray info", traceX, traceY, 1, 1, DXGI_FORMAT_R16_UINT });
    const TextureRef traceRadiance = g.createTexture({ "lumen trace radiance", traceX, traceY, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
    const TextureRef traceWord = g.createTexture({ "lumen trace word", traceX, traceY, 1, 1, DXGI_FORMAT_R32_UINT });
    const TextureRef radianceA = g.createTexture({ "lumen probe radiance A", traceX, traceY, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
    const TextureRef radianceB = g.createTexture({ "lumen probe radiance B", traceX, traceY, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
    const TextureRef hitDistance = g.createTexture({ "lumen probe hit distance", traceX, traceY, 1, 1, DXGI_FORMAT_R16_UNORM });
    const TextureRef probeMoving = g.createTexture({ "lumen probe moving", probesX, atlasRows, 1, 1, DXGI_FORMAT_R8_UNORM });
    const TextureRef irradiance = g.createTexture({ "lumen probe irradiance", traceX, traceY, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
    const TextureRef radianceBorder = g.createTexture({ "lumen probe radiance (border)", probesX * 10, atlasRows * 10, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
    const TextureRef newDiffuse = g.createTexture({ "lumen diffuse (frame)", width, height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
    // Foliage's back side (the reference's backface diffuse: the probes at the reversed normal, in the diffuse's history)
    bool foliage = false;
    for (const gpu::Material& material : fc.scene.materials()) foliage = foliage || (material.classFlags & 0xFFu) == 1u;  // scene::MaterialClass::Foliage
    foliage = foliage && view.materialWord.valid();
    TextureRef backface, prevBackface, newBackface;
    bool backfaceHistory = false;
    if (foliage)
    {
        if (!st.backface[0] || st.backfaceWidth != width || st.backfaceHeight != height)
        {
            D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC1 d{};
            d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            d.Width = width;
            d.Height = height;
            d.DepthOrArraySize = d.MipLevels = 1;
            d.SampleDesc.Count = 1;
            d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            d.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            for (int k = 0; k < 2; ++k)
            {
                if (st.backface[k]) m_device.deferRelease(st.backface[k]);
                check(m_device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr, nullptr, 0, nullptr,
                                                               IID_PPV_ARGS(st.backface[k].ReleaseAndGetAddressOf())),
                      "GI lumen backface history");
                st.backface[k]->SetName(k ? L"R lumen backface 1" : L"R lumen backface 0");
            }
            st.backfaceWidth = width;
            st.backfaceHeight = height;
            st.backfaceHistory = false;
        }
        backfaceHistory = st.backfaceHistory && historyValid;
        st.backfaceHistory = true;
        backface = import(st.backface[next], "lumen backface", width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
        prevBackface = import(st.backface[prev], "lumen backface (previous)", width, height, DXGI_FORMAT_R16G16B16A16_FLOAT);
        newBackface = g.createTexture({ "lumen backface (frame)", width, height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
    }
    else
        st.backfaceHistory = false;
    const TextureRef materialWord = view.materialWord;
    const TextureRef newSpecular = g.createTexture({ "lumen rough specular (frame)", width, height, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });

    const TextureRef depth = view.depth, gbuffer = view.gbuffer, visId = view.visId;
    const BufferRef visibleClusters = view.visibleClusters;
    const bool motion = visId.valid() && visibleClusters.valid();
    const D3D12_GPU_VIRTUAL_ADDRESS frameConstants = view.frameConstants;

    // The common block, P[8..11] (LgCommon.hlsli); the per-pass words [42] adaptive, [43] probe depth, [44] probe normal,
    // [45] probe position are filled by each pass with the view it binds (SRV or UAV).
    struct Common
    {
        uint32_t k[16];
    };
    Common common{};
    common.k[0] = width;
    common.k[1] = height;
    common.k[2] = probesX;
    common.k[3] = probesY;
    common.k[4] = atlasRows;
    common.k[5] = L.tile;
    common.k[6] = temporalIndex | (rayIndex << 8) | (historyValid ? 0x10000u : 0u);
    common.k[7] = (uint32_t)fc.frame.frameIndex;
    common.k[8] = uniform;
    common.k[9] = maxAdaptive;
    common.k[14] = bits(exposureRatio);

    const auto surface = [=](PassBuilder& b) {
        b.use(depth, Use::SrvCompute);
        b.use(gbuffer, Use::SrvCompute);
        if (motion)
        {
            b.use(visId, Use::SrvCompute);
            b.use(visibleClusters, Use::SrvCompute);
        }
    };
    const auto surfaceWords = [=](PassContext& c, uint32_t* k) {
        k[0] = c.srv(depth);
        k[1] = c.srv(gbuffer);
        k[2] = motion ? c.srv(visId) : 0xFFFFFFFFu;
        k[3] = motion ? c.srv(visibleClusters) : 0xFFFFFFFFu;
    };
    const uint32_t tilesGx = (probesX + 7) / 8, tilesGy = (probesY + 7) / 8;

    g.addPass("r.gi.lg.place", QueueType::Compute,
              [&](PassBuilder& b) {
                  surface(b);
                  b.use(probeDepth, Use::UavCompute);
                  b.use(probeNormal, Use::UavCompute);
                  b.use(probePosition, Use::UavCompute);
                  b.use(adaptive, Use::UavCompute);
              },
              [=, &shaders](PassContext& c) {
                  uint32_t k[48] = {};
                  surfaceWords(c, k);
                  k[4] = c.uav(probeDepth);
                  k[5] = c.uav(probeNormal);
                  k[6] = c.uav(probePosition);
                  std::memcpy(&k[32], common.k, sizeof common.k);
                  k[42] = c.uav(adaptive);
                  c.cmd->SetPipelineState(shaders.compute("Passes/GI/Lumen/LgPlace"));
                  c.computeConstants(k, 48);
                  c.bindFrameConstants(frameConstants);
                  c.cmd->Dispatch(tilesGx, tilesGy, 1);
              });
    if (maxAdaptive > 0)
    {
        g.addPass("r.gi.lg.adaptive.mark", QueueType::Compute,
                  [&](PassBuilder& b) {
                      surface(b);
                      b.use(probeDepth, Use::SrvCompute);
                      b.use(probePosition, Use::SrvCompute);
                      b.use(mask, Use::UavCompute);
                  },
                  [=, &shaders](PassContext& c) {
                      uint32_t k[48] = {};
                      surfaceWords(c, k);
                      k[4] = c.uav(mask);
                      std::memcpy(&k[32], common.k, sizeof common.k);
                      k[43] = c.srv(probeDepth);
                      k[45] = c.srv(probePosition);
                      c.cmd->SetPipelineState(shaders.compute("Passes/GI/Lumen/LgAdaptiveMark"));
                      c.computeConstants(k, 48);
                      c.bindFrameConstants(frameConstants);
                      c.cmd->Dispatch(tilesGx, tilesGy, 1);
                  });
        g.addPass("r.gi.lg.adaptive.spawn", QueueType::Compute,
                  [&](PassBuilder& b) {
                      surface(b);
                      b.use(mask, Use::SrvCompute);
                      b.use(probeDepth, Use::UavCompute);
                      b.use(probeNormal, Use::UavCompute);
                      b.use(probePosition, Use::UavCompute);
                      b.use(adaptive, Use::UavCompute);
                  },
                  [=, &shaders](PassContext& c) {
                      uint32_t k[48] = {};
                      surfaceWords(c, k);
                      k[4] = c.uav(probeDepth);
                      k[5] = c.uav(probeNormal);
                      k[6] = c.uav(probePosition);
                      k[7] = c.srv(mask);
                      std::memcpy(&k[32], common.k, sizeof common.k);
                      k[42] = c.uav(adaptive);
                      c.cmd->SetPipelineState(shaders.compute("Passes/GI/Lumen/LgAdaptiveSpawn"));
                      c.computeConstants(k, 48);
                      c.bindFrameConstants(frameConstants);
                      c.cmd->Dispatch(tilesGx, tilesGy, 1);
                  });
    }
    // The words every later pass reads the probes through.
    const auto probes = [=](PassBuilder& b) {
        b.use(adaptive, Use::SrvCompute);
        b.use(probeDepth, Use::SrvCompute);
        b.use(probeNormal, Use::SrvCompute);
        b.use(probePosition, Use::SrvCompute);
    };
    const auto probeWords = [=](PassContext& c, uint32_t* k) {
        std::memcpy(&k[32], common.k, sizeof common.k);
        k[42] = c.srv(adaptive);
        k[43] = c.srv(probeDepth);
        k[44] = c.srv(probeNormal);
        k[45] = c.srv(probePosition);
    };
    // A's far-field radiance cache (lumen.radiance_cache): cleared and marked from the screen by Begin, marked by the
    // probes here, updated (allocation, probe rays, filter) before the probes' own rays read it.
    LumenRcFrame rc = lumenRadianceCacheBegin(fc, view);
    if (rc.on)
    {
        const TextureRef indirection = rc.indirection;
        const uint32_t rcParams = rc.params;
        g.addPass("r.gi.lg.rcmark", QueueType::Compute,
                  [&](PassBuilder& b) {
                      probes(b);
                      b.use(indirection, Use::UavCompute);
                  },
                  [=, &shaders](PassContext& c) {
                      uint32_t k[48] = {};
                      k[4] = c.uav(indirection);
                      k[5] = rcParams;
                      probeWords(c, k);
                      c.cmd->SetPipelineState(shaders.compute("Passes/GI/Lumen/LgRcMark"));
                      c.computeConstants(k, 48);
                      c.cmd->Dispatch((probesX + 7) / 8, (atlasRows + 7) / 8, 1);
                  });
        LumenRcInputs in;
        in.cards = L.hitSurfaceCache ? fc.resources.cards : SurfaceCacheCardRefs{};
        // (with the cards and without gi.lumen_hit_fallback the hits read no world cache)
        in.worldCache = in.cards.valid() && !L.hitFallback ? BufferRef{} : cache;
        // the translucency volume's cells mark the probes their rays end in, before the cache allocates and traces
        lumenTranslucencyVolumeMark(fc, view, rc);
        in.skyRadiance = m_skyRadiance;
        in.sunIlluminance = m_sunIlluminance;
        in.experiment = m_settings.experimentDisable;
        lumenRadianceCacheUpdate(fc, view, rays, in, rc);
    }
    {
        // lumen.translucency_volume: indirect light for air, particles, water and glass (after the cache's update)
        LumenTvInputs tv;
        tv.cards = L.hitSurfaceCache ? fc.resources.cards : SurfaceCacheCardRefs{};
        tv.skyRadiance = m_skyRadiance;
        tv.sunIlluminance = m_sunIlluminance;
        lumenTranslucencyVolume(fc, view, rays, tv, rc);
    }
    const bool farField = rc.on;
    // lumen.radiance_cache_far_field: hits past the mesh cards take the sky's light (GiSky.hlsli giFarSkyIrradiance; the
    // trace's flags carry the start in whole metres)
    const QualityConfig& fq = fc.quality;
    const uint32_t farStartMetres = fq.has("lumen.radiance_cache_far_field") && fq.boolean("lumen.radiance_cache_far_field")
                                        ? std::min<uint32_t>((uint32_t)fq.number("surface_cache.mesh_cards_max_distance_m"), 65535u)
                                        : 0u;
    // surface_cache.feedback_gather: the probes' hits read the cards' high levels and report (the reference's gather
    // reads the resident level and reports nothing: default false)
    const bool hiResHits = fq.has("surface_cache.feedback_gather") && fq.boolean("surface_cache.feedback_gather");
    const TextureRef rcIndirection = rc.indirection, rcAtlas = rc.atlas, rcDepth = rc.depth;
    const uint32_t rcParamsSrv = rc.params;
    g.addPass("r.gi.lg.screendata", QueueType::Compute,
              [&](PassBuilder& b) {
                  surface(b);
                  probes(b);
                  b.use(prevDiffuse, Use::SrvCompute);
                  b.use(prevKeys, Use::SrvCompute);
                  b.use(brdf, Use::UavCompute);
              },
              [=, &shaders](PassContext& c) {
                  uint32_t k[48] = {};
                  surfaceWords(c, k);
                  k[4] = c.uav(brdf);
                  k[8] = c.srv(prevDiffuse);
                  k[9] = bits(L.disocclusionMaxFrames);
                  k[10] = bits(L.disocclusionFraction);
                  k[11] = c.srv(prevKeys);
                  k[12] = bits(L.temporalDistanceThreshold);
                  std::memcpy(&k[16], &prevInvViewProj, 64);
                  probeWords(c, k);
                  c.cmd->SetPipelineState(shaders.compute("Passes/GI/Lumen/LgScreenData"));
                  c.computeConstants(k, 48);
                  c.bindFrameConstants(frameConstants);
                  c.cmd->Dispatch(probesX, atlasRows, 1);
              });
    if (L.importanceSampleLighting)
        g.addPass("r.gi.lg.lightingpdf", QueueType::Compute,
                  [&](PassBuilder& b) {
                      surface(b);
                      probes(b);
                      b.use(prevProbeDepth, Use::SrvCompute);
                      b.use(prevProbePosition, Use::SrvCompute);
                      b.use(prevProbeRadiance, Use::SrvCompute);
                      b.use(lightingPdf, Use::UavCompute);
                      if (farField)
                      {
                          b.use(rcIndirection, Use::SrvCompute);
                          b.use(rcAtlas, Use::SrvCompute);
                      }
                  },
                  [=, &shaders](PassContext& c) {
                      uint32_t k[48] = {};
                      surfaceWords(c, k);
                      k[12] = farField ? rcParamsSrv : 0xFFFFFFFFu;
                      k[13] = farField ? c.srv(rcIndirection) : 0xFFFFFFFFu;
                      k[14] = farField ? c.srv(rcAtlas) : 0xFFFFFFFFu;
                      k[4] = c.uav(lightingPdf);
                      k[5] = c.srv(prevProbeDepth);
                      k[6] = c.srv(prevProbePosition);
                      k[7] = c.srv(prevProbeRadiance);
                      k[8] = prevTemporalIndex;
                      k[9] = bits(1.0f / exposureRatio);
                      probeWords(c, k);
                      c.cmd->SetPipelineState(shaders.compute("Passes/GI/Lumen/LgLightingPdf"));
                      c.computeConstants(k, 48);
                      c.bindFrameConstants(frameConstants);
                      c.cmd->Dispatch(probesX, atlasRows, 1);
                  });
    const bool lightingDensity = L.importanceSampleLighting;
    g.addPass("r.gi.lg.rays", QueueType::Compute,
              [&](PassBuilder& b) {
                  probes(b);
                  b.use(brdf, Use::SrvCompute);
                  if (lightingDensity) b.use(lightingPdf, Use::SrvCompute);
                  b.use(rayInfo, Use::UavCompute);
              },
              [=, &shaders](PassContext& c) {
                  uint32_t k[48] = {};
                  k[4] = c.uav(rayInfo);
                  k[5] = c.srv(brdf);
                  k[6] = lightingDensity ? c.srv(lightingPdf) : 0xFFFFFFFFu;
                  k[7] = bits(L.minPdfToTrace);
                  probeWords(c, k);
                  c.cmd->SetPipelineState(shaders.compute("Passes/GI/Lumen/LgGenerateRays"));
                  c.computeConstants(k, 48);
                  c.cmd->Dispatch(probesX, atlasRows, 1);
              });

    // Trace: the GI cache rays' sky and sun (record()), the ray scene, hit lighting from the world cache.
    uint32_t scene[8];
    rays.rootConstants(scene);
    const FrameResources& fr = fc.resources;
    const bool atmosphere = fr.transmittanceLut.valid() && fr.multiScatterLut.valid() && fr.skyViewLut.valid() && fr.aerialPerspective.valid();
    const TextureRef luts[4] = { fr.transmittanceLut, fr.multiScatterLut, fr.skyViewLut, fr.aerialPerspective };
    const std::string traceKernel = std::string("Passes/GI/Lumen/LgTrace.SKY") + (atmosphere ? "0" : "1");
    rt::RayPipeline& pipeline = rt::RayPipeline::get(fc.device, shaders, rt::standardRayPipeline(traceKernel, { "LgTraceGen" }));
    const float3 sky = m_skyRadiance, sun = m_sunIlluminance;
    const float skyBand = m_skyBand, rayLength = m_settings.rayLength;
    const uint32_t experiment = m_settings.experimentDisable;
    const uint32_t raysPerDispatch = L.raysPerDispatch;
    // gi.lumen_hit_surface_cache: the hits read the mesh-card surface cache (tracks::surfaceCache updated it before GI).
    const SurfaceCacheCardRefs cards = L.hitSurfaceCache ? fc.resources.cards : SurfaceCacheCardRefs{};
    // Screen traces before the world rays (gi.lumen_screen_traces): the shared depth pyramid and last frame's colour
    // (tracks::screenTraceInputs published them before GI; without a colour history the walk is skipped).
    const TextureRef pyramid = fc.resources.screenTraceHzb, prevColor = view.prevSceneColor;
    // The cut frame itself has no colour history (the upscaler's reset; Unreal: the previous view info is reset on a
    // cut, so its screen-trace input is invalid) and skips the walk. gi.lumen_screen_trace_skip_after_cut (default
    // false, not an Unreal rule) also skips it in the frame after, which reads the cut frame's colour.
    const bool screenTraced = L.screenTraces && pyramid.valid() && prevColor.valid() && !(L.screenTraceSkipAfterCut && previousFrameWasCut);
    if (screenTraced)
    {
        const FrameContext::Upscale up = fc.frame.upscale;
        // (output.screen_trace_source = 0: the previous colour's alpha is its frame's depth - the history depth test)
        const bool historyDepth = fc.quality.has("output.screen_trace_source") && fc.quality.integer("output.screen_trace_source") == 0;
        const TextureRef words = view.materialWord;  // (the Foliage test at a screen hit)
        g.addPass("r.gi.lg.screentrace", QueueType::Compute,
                  [&](PassBuilder& b) {
                      surface(b);
                      probes(b);
                      b.use(rayInfo, Use::SrvCompute);
                      b.use(pyramid, Use::SrvCompute);
                      b.use(prevColor, Use::SrvCompute);
                      if (words.valid()) b.use(words, Use::SrvCompute);
                      b.use(traceRadiance, Use::UavCompute);
                      b.use(traceWord, Use::UavCompute);
                      if (farField) b.use(rcIndirection, Use::SrvCompute);
                  },
                  [=, &shaders](PassContext& c) {
                      uint32_t k[48] = {};
                      surfaceWords(c, k);
                      k[4] = c.srv(rayInfo);
                      k[5] = c.uav(traceRadiance);
                      k[6] = c.uav(traceWord);
                      k[7] = c.srv(pyramid);
                      k[8] = c.srv(prevColor);
                      k[9] = words.valid() ? c.srv(words) : 0xFFFFFFFFu;  // (the colour's size: the kernel reads the texture's own)
                      k[10] = farField ? rcParamsSrv : 0xFFFFFFFFu;
                      k[11] = bits(up.exposureRatio);
                      k[12] = (L.screenTraceIterations & 0xFFFFu) | ((L.screenTraceThicknessSteps & 0x7FFFu) << 16) | (historyDepth ? 0x80000000u : 0u);
                      k[13] = bits(L.screenTraceThickness);
                      k[14] = farField ? c.srv(rcIndirection) : 0xFFFFFFFFu;
                      k[15] = bits(rayLength);
                      for (int row = 0; row < 4; ++row)
                          for (int col = 0; col < 4; ++col) k[16 + 4 * row + col] = bits(up.prevViewProj.m[row][col]);
                      probeWords(c, k);
                      k[47] = bits(L.movingSpeed);
                      c.cmd->SetPipelineState(shaders.compute("Passes/GI/Lumen/LgScreenTrace"));
                      c.computeConstants(k, 48);
                      c.bindFrameConstants(frameConstants);
                      c.cmd->Dispatch(probesX, atlasRows, 1);
                  });
    }
    g.addPass("r.gi.lg.trace", QueueType::Compute,
              [&](PassBuilder& b) {
                  if (cache.valid()) b.use(cache, Use::SrvGraphics);  // (gi.lumen_only: none)
                  b.use(adaptive, Use::SrvGraphics);
                  b.use(probeDepth, Use::SrvGraphics);
                  b.use(probeNormal, Use::SrvGraphics);
                  b.use(probePosition, Use::SrvGraphics);
                  b.use(rayInfo, Use::SrvGraphics);
                  b.use(traceRadiance, Use::UavGraphics);
                  b.use(traceWord, Use::UavGraphics);
                  declareSurfaceCacheCards(b, cards, Use::SrvGraphics);
                  if (farField)
                  {
                      b.use(rcIndirection, Use::SrvGraphics);
                      b.use(rcAtlas, Use::SrvGraphics);
                      b.use(rcDepth, Use::SrvGraphics);
                  }
                  rays.declareTraversal(b);
                  rays.declareDecals(b);
                  if (atmosphere)
                      for (const TextureRef& t : luts) b.use(t, Use::SrvGraphics);
              },
              [=, &pipeline](PassContext& c) {
                  uint32_t k[48] = {};
                  k[0] = cache.valid() ? c.srv(cache) : 0xFFFFFFFFu;
                  k[1] = c.srv(rayInfo);
                  k[2] = c.uav(traceRadiance);
                  k[3] = c.uav(traceWord);
                  k[4] = bits(sky.x);
                  k[5] = bits(sky.y);
                  k[6] = bits(sky.z);
                  k[7] = bits(rayLength);
                  for (int i = 0; i < 4; ++i) k[8 + i] = atmosphere ? c.srv(luts[i]) : 0xFFFFFFFFu;
                  k[12] = bits(sun.x);
                  k[13] = bits(sun.y);
                  k[14] = bits(sun.z);
                  k[15] = experiment;
                  k[16] = bits(skyBand);
                  k[17] = (screenTraced ? 1u : 0u) | (!cache.valid() || (!L.hitFallback && cards.valid()) ? 2u : 0u) | (hiResHits ? 4u : 0u) | farStartMetres << 16;
                  k[18] = bits(L.normalBias);
                  k[19] = bits(L.movingSpeed);
                  k[20] = cards.valid() ? c.srv(cards.frame) : 0xFFFFFFFFu;
                  k[21] = farField ? rcParamsSrv : 0xFFFFFFFFu;
                  k[22] = farField ? c.srv(rcIndirection) : 0xFFFFFFFFu;
                  k[23] = farField ? c.srv(rcAtlas) : 0xFFFFFFFFu;
                  std::memcpy(&k[24], scene, sizeof scene);
                  std::memcpy(&k[32], common.k, sizeof common.k);
                  k[42] = c.srv(adaptive);
                  k[43] = c.srv(probeDepth);
                  k[44] = c.srv(probeNormal);
                  k[45] = c.srv(probePosition);
                  k[46] = farField ? c.srv(rcDepth) : 0xFFFFFFFFu;
                  c.bindFrameConstants(frameConstants);
                  // Bands of rows, each its own DispatchRays of at most raysPerDispatch rays (a structural bound on one
                  // dispatch's work: the atlas grows with the resolution, a dispatch does not).
                  // (a thread traces at most 3 rays: the probe ray and, at a hit without cards, the sun's shadow ray and
                  // a local-light sample's)
                  const uint32_t bandRows = std::max(1u, raysPerDispatch / 3u / std::max(traceX, 1u));
                  for (uint32_t row = 0; row < traceY; row += bandRows)
                  {
                      k[47] = row;
                      c.computeConstants(k, 48);
                      pipeline.dispatch(c.cmd, 0, traceX, std::min(bandRows, traceY - row), 1);
                  }
              });
    // Probe radiance stages after the composite: the optional probe-space temporal blend, then the spatial filter passes.
    // They alternate between two transient textures; the last one writes the persistent texture (next frame's history).
    const uint32_t stages = (L.temporalFilterProbes ? 1u : 0u) + L.filterPasses;
    // The intensity cap's exposure on a snap frame (gi.lumen_cap_snap_exposure, LgMeter.hlsl): the frame's exposure
    // was not metered on what it shows, so the cap is taken in the exposure metered on the frame's own traces.
    BufferRef capReference;
#if defined(UNX_GI_HAS_SHADING)
    if (L.capSnapExposure && shading::exposureSnapping(fc))
    {
        const QualityConfig& q = fc.quality;
        const float grey = (float)q.number("shading.exposure_target_grey"), cutDark = (float)q.number("shading.exposure_cut_dark");
        const float cutBright = (float)q.number("shading.exposure_cut_bright"), minEv = (float)q.number("shading.exposure_min_ev");
        const float maxEv = (float)q.number("shading.exposure_max_ev"), compensation = fc.frame.exposureCompensation;
        const float centreSigma = (float)q.number("shading.exposure_centre_sigma");
        const BufferRef histogram = g.createBuffer({ "lumen meter histogram", 256, 0 });
        capReference = g.createBuffer({ "lumen cap exposure", 16, 0 });
        fc.resources.lumenCapReference = capReference;  // (R's reflection filters: the same reference)
        const BufferRef reference = capReference;
        static const char* const meterNames[3] = { "r.gi.lg.meter.clear", "r.gi.lg.meter.bins", "r.gi.lg.meter" };
        for (uint32_t mode = 0; mode < 3; ++mode)
            g.addPass(meterNames[mode], QueueType::Compute,
                      [&](PassBuilder& b) {
                          probes(b);
                          b.use(histogram, mode == 2 ? Use::SrvCompute : Use::UavCompute);
                          if (mode == 1) b.use(traceRadiance, Use::SrvCompute);
                          if (mode == 1) b.use(rayInfo, Use::SrvCompute);
                          if (mode == 2) b.use(reference, Use::UavCompute);
                      },
                      [=, &shaders](PassContext& c) {
                          uint32_t k[48] = {};
                          k[0] = mode == 1 ? c.srv(traceRadiance) : 0xFFFFFFFFu;
                          k[1] = mode == 2 ? c.srv(histogram) : c.uav(histogram);
                          k[2] = mode == 2 ? c.uav(reference) : 0xFFFFFFFFu;
                          k[3] = mode;
                          k[4] = bits(grey);
                          k[5] = bits(cutDark);
                          k[6] = bits(cutBright);
                          k[7] = bits(compensation);
                          k[8] = bits(minEv);
                          k[9] = bits(maxEv);
                          k[10] = bits(centreSigma);
                          k[11] = mode == 1 ? c.srv(rayInfo) : 0xFFFFFFFFu;
                          probeWords(c, k);
                          c.cmd->SetPipelineState(shaders.compute("Passes/GI/Lumen/LgMeter"));
                          c.computeConstants(k, 48);
                          c.bindFrameConstants(frameConstants);
                          if (mode == 1) c.cmd->Dispatch(probesX, atlasRows, 1);
                          else c.cmd->Dispatch(1, 1, 1);
                      });
    }
#endif
    g.addPass("r.gi.lg.composite", QueueType::Compute,
              [&](PassBuilder& b) {
                  probes(b);
                  if (capReference.valid()) b.use(capReference, Use::SrvCompute);
                  b.use(traceRadiance, Use::SrvCompute);
                  b.use(traceWord, Use::SrvCompute);
                  b.use(rayInfo, Use::SrvCompute);
                  b.use(stages == 0 ? probeRadiance : radianceA, Use::UavCompute);
                  b.use(hitDistance, Use::UavCompute);
                  b.use(probeMoving, Use::UavCompute);
              },
              [=, &shaders](PassContext& c) {
                  uint32_t k[48] = {};
                  k[0] = c.srv(traceRadiance);
                  k[1] = c.srv(traceWord);
                  k[2] = c.srv(rayInfo);
                  k[3] = capReference.valid() ? c.srv(capReference) : 0xFFFFFFFFu;
                  k[4] = c.uav(stages == 0 ? probeRadiance : radianceA);
                  k[5] = c.uav(hitDistance);
                  k[6] = c.uav(probeMoving);
                  k[7] = bits(L.maxRayIntensity);
                  probeWords(c, k);
                  c.cmd->SetPipelineState(shaders.compute("Passes/GI/Lumen/LgComposite"));
                  c.computeConstants(k, 48);
                  c.bindFrameConstants(frameConstants);
                  c.cmd->Dispatch(probesX, atlasRows, 1);
              });
    // Spatial filter: A -> B -> A ..., the last pass into the persistent texture (next frame's lighting density).
    static const char* const filterNames[4] = { "r.gi.lg.filter0", "r.gi.lg.filter1", "r.gi.lg.filter2", "r.gi.lg.filter3" };
    TextureRef filterIn = radianceA;
    uint32_t stage = 0;
    const auto stageTarget = [&]() {
        const TextureRef t = stage + 1 == stages ? probeRadiance : ((stage & 1u) == 0 ? radianceB : radianceA);
        ++stage;
        return t;
    };
    if (L.temporalFilterProbes)
    {
        const TextureRef blended = stageTarget();
        g.addPass("r.gi.lg.probetemporal", QueueType::Compute,
                  [&](PassBuilder& b) {
                      surface(b);
                      probes(b);
                      b.use(filterIn, Use::SrvCompute);
                      b.use(prevProbeDepth, Use::SrvCompute);
                      b.use(prevProbePosition, Use::SrvCompute);
                      b.use(prevProbeRadiance, Use::SrvCompute);
                      b.use(blended, Use::UavCompute);
                  },
                  [=, &shaders](PassContext& c) {
                      uint32_t k[48] = {};
                      surfaceWords(c, k);
                      k[4] = c.srv(filterIn);
                      k[5] = c.uav(blended);
                      k[6] = c.srv(prevProbeDepth);
                      k[7] = c.srv(prevProbePosition);
                      k[8] = c.srv(prevProbeRadiance);
                      k[9] = prevTemporalIndex;
                      k[10] = bits(L.temporalFilterProbesWeight);
                      k[11] = bits(exposureRatio);
                      probeWords(c, k);
                      c.cmd->SetPipelineState(shaders.compute("Passes/GI/Lumen/LgProbeTemporal"));
                      c.computeConstants(k, 48);
                      c.bindFrameConstants(frameConstants);
                      c.cmd->Dispatch(probesX, atlasRows, 1);
                  });
        filterIn = blended;
    }
    for (uint32_t pass = 0; pass < L.filterPasses; ++pass)
    {
        const TextureRef filterOut = stageTarget();
        g.addPass(filterNames[pass], QueueType::Compute,
                  [&, filterIn, filterOut](PassBuilder& b) {
                      probes(b);
                      b.use(filterIn, Use::SrvCompute);
                      b.use(hitDistance, Use::SrvCompute);
                      b.use(probeMoving, Use::SrvCompute);
                      b.use(brdf, Use::SrvCompute);
                      b.use(filterOut, Use::UavCompute);
                  },
                  [=, &shaders](PassContext& c) {
                      uint32_t k[48] = {};
                      k[0] = c.srv(filterIn);
                      k[1] = c.srv(hitDistance);
                      k[2] = c.srv(probeMoving);
                      k[3] = c.srv(brdf);
                      k[4] = c.uav(filterOut);
                      k[8] = bits(L.filterMaxHitAngleDeg * 3.14159265f / 180.0f);
                      k[9] = bits(L.filterPositionWeight);
                      k[10] = pass;
                      probeWords(c, k);
                      c.cmd->SetPipelineState(shaders.compute("Passes/GI/Lumen/LgFilter"));
                      c.computeConstants(k, 48);
                      c.bindFrameConstants(frameConstants);
                      c.cmd->Dispatch(probesX, atlasRows, 1);
                  });
        filterIn = filterOut;
    }
    g.addPass("r.gi.lg.irradiance", QueueType::Compute,
              [&](PassBuilder& b) {
                  probes(b);
                  b.use(probeRadiance, Use::SrvCompute);
                  b.use(irradiance, Use::UavCompute);
                  b.use(radianceBorder, Use::UavCompute);
              },
              [=, &shaders](PassContext& c) {
                  uint32_t k[48] = {};
                  k[0] = c.srv(probeRadiance);
                  k[4] = c.uav(irradiance);
                  k[5] = c.uav(radianceBorder);
                  probeWords(c, k);
                  c.cmd->SetPipelineState(shaders.compute("Passes/GI/Lumen/LgIrradiance"));
                  c.computeConstants(k, 48);
                  c.cmd->Dispatch(probesX, atlasRows, 1);
              });
    // A's short-range AO and bent normal (lumen.short_range_ao; recorded before GI's record by the track: invalid = off).
    const float aoMaxAlbedo = fc.quality.has("lumen.short_range_ao_max_multibounce_albedo") ? (float)fc.quality.number("lumen.short_range_ao_max_multibounce_albedo") : 0.5f;
    g.addPass("r.gi.lg.integrate", QueueType::Compute,
              [&](PassBuilder& b) {
                  surface(b);
                  probes(b);
                  b.use(irradiance, Use::SrvCompute);
                  b.use(radianceBorder, Use::SrvCompute);
                  b.use(probeMoving, Use::SrvCompute);
                  b.use(newDiffuse, Use::UavCompute);
                  b.use(newSpecular, Use::UavCompute);
                  if (foliage)
                  {
                      b.use(newBackface, Use::UavCompute);
                      b.use(materialWord, Use::SrvCompute);
                  }
              },
              [=, &shaders](PassContext& c) {
                  uint32_t k[48] = {};
                  surfaceWords(c, k);
                  k[14] = foliage ? c.uav(newBackface) : 0xFFFFFFFFu;
                  k[15] = foliage ? c.srv(materialWord) : 0xFFFFFFFFu;
                  k[4] = c.uav(newDiffuse);
                  k[5] = c.uav(newSpecular);
                  k[6] = c.srv(irradiance);
                  k[7] = c.srv(radianceBorder);
                  k[8] = bits(L.jitterWidth);
                  k[9] = stochasticInterpolation ? 1u : 0u;
                  k[10] = bits(L.maxRoughnessRoughSpecular);
                  k[11] = c.srv(probeMoving);
                  // (lumen.short_range_ao is applied after the pixel filter, in M's composite: Unreal's default,
                  // ShortRangeAO.ApplyDuringIntegration 0 - the half-resolution AO's noise stays out of the GI history)
                  k[12] = 0xFFFFFFFFu;
                  k[13] = bits(aoMaxAlbedo);
                  probeWords(c, k);
                  c.cmd->SetPipelineState(shaders.compute("Passes/GI/Lumen/LgIntegrate"));
                  c.computeConstants(k, 48);
                  c.bindFrameConstants(frameConstants);
                  c.cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
              });
    g.addPass("r.gi.lg.temporal", QueueType::Compute,
              [&](PassBuilder& b) {
                  surface(b);
                  b.use(newDiffuse, Use::SrvCompute);
                  b.use(newSpecular, Use::SrvCompute);
                  b.use(prevDiffuse, Use::SrvCompute);
                  b.use(prevSpecular, Use::SrvCompute);
                  b.use(prevKeys, Use::SrvCompute);
                  b.use(diffuse, Use::UavCompute);
                  b.use(specular, Use::UavCompute);
                  b.use(keys, Use::UavCompute);
                  if (foliage)
                  {
                      b.use(newBackface, Use::SrvCompute);
                      b.use(prevBackface, Use::SrvCompute);
                      b.use(backface, Use::UavCompute);
                  }
                  b.keep();
              },
              [=, &shaders](PassContext& c) {
                  uint32_t k[48] = {};
                  surfaceWords(c, k);
                  k[4] = c.srv(newDiffuse);
                  k[5] = c.srv(newSpecular);
                  k[6] = c.uav(diffuse);
                  k[7] = c.uav(specular);
                  k[8] = c.uav(keys);
                  k[9] = c.srv(prevDiffuse);
                  k[10] = c.srv(prevSpecular);
                  k[11] = c.srv(prevKeys);
                  k[12] = bits(L.temporalMaxFrames);
                  k[13] = bits(L.temporalDistanceThreshold);
                  k[14] = bits(L.temporalFastFraction);
                  k[15] = bits(L.temporalMaxFast);
                  std::memcpy(&k[16], &prevInvViewProj, 64);
                  std::memcpy(&k[32], common.k, sizeof common.k);
                  // (P[10].z, P[10].w, P[11].x: the probe words elsewhere - here the backface's frame value, history and output)
                  k[42] = foliage ? c.srv(newBackface) : 0xFFFFFFFFu;
                  k[43] = foliage && backfaceHistory ? c.srv(prevBackface) : 0xFFFFFFFFu;
                  k[44] = foliage ? c.uav(backface) : 0xFFFFFFFFu;
                  c.cmd->SetPipelineState(shaders.compute("Passes/GI/Lumen/LgTemporal"));
                  c.computeConstants(k, 48);
                  c.bindFrameConstants(frameConstants);
                  c.cmd->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
              });
    view.giIrradiance = diffuse;
    view.giRoughSpecular = specular;
    view.giBackfaceIrradiance = backface;
}
} // namespace unx::render::gi
