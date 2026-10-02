// The surface cache on mesh cards (unx/refl/SurfaceCacheCards.h): the card scene's GPU side - records, atlases, the
// capture raster - and the frame's order of work.
#include "unx/refl/SurfaceCacheCards.h"

#include "unx/core/Config.h"
#include "unx/core/Log.h"
#include "unx/render/Device.h"
#include "unx/render/GpuScene.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"

#if UNX_SC_HAS_MATERIAL
#include "unx/material/MaterialSystem.h"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace unx::render::refl
{
namespace
{
constexpr uint32_t kCaptureBytes = 48;        // CardCaptureList.hlsli CC_CAPTURE_BYTES
constexpr uint32_t kPageGroups = 256;         // 16 x 16 groups of 8 x 8 texels: a page of at most 128 x 128
constexpr uint32_t kUploadSlots = 4;
enum Target : uint32_t { TInstanceMap, TMeshCards, TCards, TPages, TPageTable, TCount };
constexpr uint32_t kStride[TCount] = { 4, sizeof(McMeshCardsGpu), sizeof(McCardGpu), sizeof(McCardPageGpu), sizeof(McPageTableGpu) };
const char* const kBufferNames[TCount] = { "R card instance map", "R card mesh cards", "R card cards", "R card pages", "R card page table" };
const wchar_t* const kBufferNamesW[TCount] = { L"R card instance map", L"R card mesh cards", L"R card cards", L"R card pages", L"R card page table" };
const DXGI_FORMAT kAtlasFormats[4] = { DXGI_FORMAT_R16_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R11G11B10_FLOAT };
const char* const kAtlasNames[4] = { "R card depth", "R card albedo", "R card normal", "R card emissive" };
const wchar_t* const kAtlasNamesW[4] = { L"R card depth", L"R card albedo", L"R card normal", L"R card emissive" };

uint32_t bits(float f)
{
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

struct Tracked  // a scene instance as the card scene last saw it
{
    uint32_t mesh = 0xFFFFFFFFu;
    uint32_t scaleBits = 0, transformRevision = 0, materialKey = 0;
    uint8_t state = 0;  // 0 not in the card scene, 1 waiting for its mesh's cards, 2 added
};

struct CopyOp
{
    uint32_t target;
    uint64_t destination, source, bytes;
};

struct Draw  // one DispatchMesh of the capture: a page's view of one submesh
{
    uint32_t k[20];
    uint32_t groups;
    uint32_t rect[4];  // the page's rectangle in the capture atlas: x, y, width, height
};

struct Round  // one update of the cache, staged at record time
{
    std::vector<CopyOp> ops;
    std::vector<Draw> draws;
    uint64_t capturesAt = 0;      // the capture records in the frame's staging
    uint32_t captures = 0;
    bool anyResample = false;
    uint32_t pageCount = 0;
};

float3x4 matrixOf(const gpu::Instance& g)
{
    float3x4 t;
    for (int r = 0; r < 3; ++r)
    {
        t.m[r][0] = g.objectToWorld[r].x;
        t.m[r][1] = g.objectToWorld[r].y;
        t.m[r][2] = g.objectToWorld[r].z;
        t.m[r][3] = g.objectToWorld[r].w;
    }
    return t;
}

uint32_t mix(uint32_t h, uint32_t v)
{
    h ^= v + 0x9e3779b9u + (h << 6) + (h >> 2);
    return h;
}
} // namespace

SurfaceCacheCardSettings SurfaceCacheCardSettings::fromQuality(const QualityConfig& q)
{
    SurfaceCacheCardSettings s;
    auto flag = [&](const char* key, bool fallback) { return q.has(key) ? q.boolean(key) : fallback; };
    auto num = [&](const char* key, double fallback) { return q.has(key) ? q.number(key) : fallback; };
    s.enabled = flag("surface_cache.enabled", false) && flag("surface_cache.mesh_cards", false) && !flag("surface_cache.mesh_cards_test_set", false);
    s.cards.atlasSize = (uint32_t)num("surface_cache.mesh_cards_atlas_size", 4096);
    s.cards.capturesPerFrame = (uint32_t)num("surface_cache.mesh_cards_captures_per_frame", 300);
    s.cards.captureFactor = std::max((uint32_t)num("surface_cache.capture_factor", 64), 1u);
    s.cards.texelDensityScale = (float)num("surface_cache.mesh_cards_texel_density_scale", 100.0);
    s.cards.maxTexelDensity = (float)num("surface_cache.mesh_cards_max_texels_per_m", 20.0);
    s.cards.maxResolution = (uint32_t)num("surface_cache.mesh_cards_max_resolution", 512);
    s.cards.minResolution = (uint32_t)num("surface_cache.mesh_cards_min_resolution", 4);
    s.cards.maxDistance = (float)num("surface_cache.mesh_cards_max_distance_m", 300.0);
    s.cards.minSize = (float)num("surface_cache.mesh_cards_min_size_m", 0.1);
    s.cards.refreshFraction = (float)num("surface_cache.mesh_cards_refresh_fraction", 0.125);
    s.direct = flag("surface_cache.direct_lighting", true);
    s.radiosity = flag("surface_cache.radiosity", true);
    s.shadowRaysOpaque = flag("surface_cache.shadow_rays_opaque", false);
    s.radiosityCap = (float)num("surface_cache.radiosity_max_ray_intensity", 40.0);
    s.radiosityFrames = (float)num("surface_cache.radiosity_max_frames_accumulated", 4.0);
    s.directFactor = std::max((uint32_t)num("surface_cache.direct_update_factor", 32), 1u);
    s.radiosityFactor = std::max((uint32_t)num("surface_cache.radiosity_update_factor", 64), 1u);
    s.depthBias = (float)num("surface_cache.mesh_cards_depth_bias_m", 0.10);
    s.loadRounds = std::clamp((uint32_t)num("surface_cache.mesh_cards_load_rounds", 8), 1u, 64u);
    s.loadLightingRounds = (uint32_t)num("surface_cache.mesh_cards_load_lighting_rounds", 96);
    s.cacheDirectory = q.has("surface_cache.mesh_cards_cache_dir") ? q.string("surface_cache.mesh_cards_cache_dir") : std::string();
    return s;
}

struct SurfaceCacheCards::Impl
{
    Device* device = nullptr;
    SurfaceCacheCardSettings settings;
    std::unique_ptr<MeshCardScene> scene;
    std::unique_ptr<MeshCardCache> cache;
    std::string cacheDirectory;
    std::unique_ptr<CardLighting> lighting;
    std::vector<Tracked> tracked;
    const scene::Scene* source = nullptr;
    uint32_t sceneRevision = 0xFFFFFFFFu;
    uint32_t waiting = 0;  // instances whose cards are not generated yet

    ComPtr<ID3D12Resource> buffers[TCount];
    uint32_t capacity[TCount] = {};
    bool fullUpload[TCount] = {};
    ComPtr<ID3D12Resource> atlas[4];
    uint32_t atlasSize = 0;
    struct UploadSlot
    {
        ComPtr<ID3D12Resource> buffer;
        uint8_t* mapped = nullptr;
        uint64_t capacity = 0;
    } upload[kUploadSlots];
    std::vector<uint8_t> staging;

    uint64_t frameIndex = UINT64_MAX, recordSerial = UINT64_MAX;
    uint32_t updateCounter = 0;
    bool restart = true;       // the card set starts from nothing
    bool loadingState = true;
    uint32_t loadLightingLeft = 0;
    float3 skyRadiance{}, sunIlluminance{};
    SurfaceCacheCardRefs published;
    McStats lastStats;
    uint64_t logFrame = 0;

    void release(ComPtr<ID3D12Resource>& r)
    {
        if (r && device) device->deferRelease(r);
        r.Reset();
    }
    ComPtr<ID3D12Resource> makeBuffer(uint64_t bytes, D3D12_HEAP_TYPE heapType, const wchar_t* name)
    {
        D3D12_HEAP_PROPERTIES heap{ heapType };
        D3D12_RESOURCE_DESC1 d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        d.Width = std::max<uint64_t>((bytes + 15) & ~15ull, 16);
        d.Height = d.DepthOrArraySize = d.MipLevels = 1;
        d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> r;
        check(device->d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)), "mesh card buffer");
        r->SetName(name);
        return r;
    }
    ComPtr<ID3D12Resource> makeAtlas(uint32_t size, DXGI_FORMAT format, const wchar_t* name)
    {
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC1 d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = size;
        d.Height = size;
        d.DepthOrArraySize = d.MipLevels = 1;
        d.Format = format;
        d.SampleDesc.Count = 1;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> r;
        check(device->d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)),
              "mesh card atlas");
        r->SetName(name);
        return r;
    }

    uint32_t materialKeyOf(const GpuScene& gs, const scene::Scene& src, uint32_t instance) const
    {
        const scene::Instance& in = src.instances[instance];
        const scene::Mesh& mesh = src.meshes[in.mesh];
        const std::vector<gpu::Material>& materials = gs.materials();
        uint32_t h = 0x811c9dc5u;
        for (size_t sm = 0; sm < mesh.submeshes.size(); ++sm)
        {
            const uint32_t material = sm < in.materialOverrides.size() ? in.materialOverrides[sm] : mesh.submeshes[sm].material;
            h = mix(h, material);
            if (material < materials.size()) h = mix(h, materials[material].revision);
        }
        return h;
    }

    // The card scene follows the GPU scene: which instances have cards, where they are.
    void sync(FramePassContext& fc)
    {
        const GpuScene& gs = fc.scene;
        const scene::Scene* src = gs.source();
        if (src != source)
        {
            // another scene: everything starts over
            scene->clear();
            cache->clear();
            tracked.clear();
            source = src;
            restart = true;
            loadingState = true;
            sceneRevision = 0xFFFFFFFFu;
            for (bool& f : fullUpload) f = true;
        }
        waiting = 0;
        if (!src) return;
        const std::vector<gpu::Instance>& instances = gs.instances();
        const uint32_t count = (uint32_t)std::min<size_t>({ (size_t)gs.staticInstanceCount(), instances.size(), src->instances.size() });
        const bool materialsMayDiffer = sceneRevision != gs.revision();
        sceneRevision = gs.revision();
        for (uint32_t i = count; i < tracked.size(); ++i)
            if (tracked[i].state == 2) scene->removeInstance(i);
        tracked.resize(count);
        const bool rebased = fc.frame.originShift.x != 0 || fc.frame.originShift.y != 0 || fc.frame.originShift.z != 0;
        const uint32_t excluded = gpu::kInstanceHidden | gpu::kInstanceViewModel | scene::InstanceSkinned | scene::InstanceWind;
        for (uint32_t i = 0; i < count; ++i)
        {
            const gpu::Instance& g = instances[i];
            Tracked& t = tracked[i];
            const scene::Mesh* mesh = g.mesh < src->meshes.size() ? &src->meshes[g.mesh] : nullptr;
            const float sx = g.objectToWorld[0].x, sy = g.objectToWorld[1].x, sz = g.objectToWorld[2].x;
            const float scale = std::sqrt(sx * sx + sy * sy + sz * sz);
            // rigid instances only: a deforming mesh (skin, wind, blend shapes, vertex animation) has no cards, as in the
            // reference - a hit on it reads no cache
            const bool eligible = mesh && (g.flags & excluded) == 0 && g.morph == gpu::kNone && src->instances[i].mesh == g.mesh && !mesh->positions.empty() &&
                                  !mesh->indices.empty() && mesh->blendShapes.empty() && !(mesh->vertexAnimation.framesPerSecond > 0) && scale > 0 && std::isfinite(scale);
            if (!eligible)
            {
                if (t.state == 2) scene->removeInstance(i);
                t = Tracked{};
                continue;
            }
            const uint32_t scaleBits = bits(scale);
            if (t.state != 0 && (t.mesh != g.mesh || t.scaleBits != scaleBits))
            {
                if (t.state == 2) scene->removeInstance(i);
                t.state = 0;
            }
            if (t.state == 0)
            {
                t.mesh = g.mesh;
                t.scaleBits = scaleBits;
                t.materialKey = materialKeyOf(gs, *src, i);
                t.state = 1;
            }
            if (t.state == 1)
            {
                // (the identity: the mesh's storage and size - another mesh under the same index is not taken for it)
                const uint64_t identity = (uint64_t)(uintptr_t)mesh->positions.data() ^ ((uint64_t)mesh->positions.size() << 40) ^ ((uint64_t)mesh->indices.size() << 16);
                const scene::MeshCards* cards = cache->find(*src, g.mesh, scale, identity);
                if (!cards)
                {
                    ++waiting;
                    continue;
                }
                scene->addInstance(i, *cards, matrixOf(g));
                t.state = 2;
                t.transformRevision = g.transformRevision;
                continue;
            }
            if (t.transformRevision != g.transformRevision || rebased)
            {
                scene->setTransform(i, matrixOf(g));
                t.transformRevision = g.transformRevision;
            }
            if (materialsMayDiffer)
            {
                const uint32_t key = materialKeyOf(gs, *src, i);
                if (key != t.materialKey)
                {
                    t.materialKey = key;
                    scene->refreshInstance(i);
                }
            }
        }
    }

    // The record buffers: the page buffers have their fixed size, the others grow with the scene.
    void ensureBuffers()
    {
        const uint32_t wanted[TCount] = { (uint32_t)scene->instanceMap().size(), (uint32_t)scene->meshCardsGpu().size(), (uint32_t)scene->cardsGpu().size(),
                                          settings.cards.maxPages, settings.cards.maxPages };
        const uint32_t floor[TCount] = { 4096, 4096, 16384, settings.cards.maxPages, settings.cards.maxPages };
        for (uint32_t t = 0; t < TCount; ++t)
        {
            if (buffers[t] && capacity[t] >= wanted[t]) continue;
            uint32_t n = std::max(floor[t], capacity[t]);
            while (n < wanted[t]) n += n / 2;
            release(buffers[t]);
            buffers[t] = makeBuffer((uint64_t)n * kStride[t], D3D12_HEAP_TYPE_DEFAULT, kBufferNamesW[t]);
            capacity[t] = n;
            fullUpload[t] = true;
        }
        if (atlasSize != settings.cards.atlasSize || !atlas[0])
        {
            for (int a = 0; a < 4; ++a)
            {
                release(atlas[a]);
                atlas[a] = makeAtlas(settings.cards.atlasSize, kAtlasFormats[a], kAtlasNamesW[a]);
            }
            atlasSize = settings.cards.atlasSize;
        }
    }

    uint64_t stage(const void* data, uint64_t bytes)
    {
        const uint64_t at = staging.size();
        staging.resize(at + ((bytes + 15) & ~15ull));
        if (bytes) std::memcpy(staging.data() + at, data, bytes);
        return at;
    }
    void stageRanges(Round& round, uint32_t target, const void* base, const std::vector<uint32_t>& indices, uint32_t count)
    {
        // runs of consecutive dirty elements become one copy each
        for (size_t i = 0; i < indices.size();)
        {
            size_t j = i + 1;
            while (j < indices.size() && indices[j] == indices[j - 1] + 1) ++j;
            const uint32_t first = indices[i], last = std::min(indices[j - 1] + 1, count);
            if (first < last && last <= capacity[target])
            {
                const uint64_t bytes = (uint64_t)(last - first) * kStride[target];
                round.ops.push_back({ target, (uint64_t)first * kStride[target], stage(static_cast<const uint8_t*>(base) + (size_t)first * kStride[target], bytes), bytes });
            }
            i = j;
        }
    }
    void stageWhole(Round& round, uint32_t target, const void* base, uint32_t count)
    {
        if (count == 0) return;
        const uint64_t bytes = (uint64_t)std::min(count, capacity[target]) * kStride[target];
        round.ops.push_back({ target, 0, stage(base, bytes), bytes });
    }

    // One update's CPU side: resolutions and allocation, then what the GPU needs of it (records that changed, the
    // capture list, the capture's draws).
    void stageRound(FramePassContext& fc, Round& round, uint32_t tableSrv)
    {
        const float3 origins[1] = { fc.frame.mainView.position };
        scene->update(origins);
        MeshCardScene::Dirty dirty = scene->takeDirty();
        const uint32_t counts[TCount] = { (uint32_t)scene->instanceMap().size(), (uint32_t)scene->meshCardsGpu().size(), (uint32_t)scene->cardsGpu().size(),
                                          (uint32_t)scene->pagesGpu().size(), (uint32_t)scene->pageTableGpu().size() };
        const void* bases[TCount] = { scene->instanceMap().data(), scene->meshCardsGpu().data(), scene->cardsGpu().data(), scene->pagesGpu().data(),
                                      scene->pageTableGpu().data() };
        const std::vector<uint32_t>* lists[TCount] = { nullptr, &dirty.meshCards, &dirty.cards, &dirty.pages, &dirty.pages };
        for (uint32_t t = 0; t < TCount; ++t)
        {
            if (fullUpload[t] || (t == TInstanceMap && dirty.instanceMap)) stageWhole(round, t, bases[t], counts[t]);
            else if (lists[t]) stageRanges(round, t, bases[t], *lists[t], counts[t]);
            fullUpload[t] = false;
        }
        round.pageCount = std::min(scene->pageCount(), settings.cards.maxPages);

        const std::vector<McCapture>& captures = scene->captures();
        round.captures = (uint32_t)captures.size();
        std::vector<uint32_t> records((size_t)std::max<size_t>(captures.size(), 1) * (kCaptureBytes / 4), 0u);
        const GpuScene& gs = fc.scene;
        const scene::Scene& src = *source;
        const std::vector<gpu::Instance>& instances = gs.instances();
        const std::vector<gpu::Material>& materials = gs.materials();
        for (size_t c = 0; c < captures.size(); ++c)
        {
            const McCapture& cap = captures[c];
            uint32_t* r = &records[c * (kCaptureBytes / 4)];
            r[0] = cap.page;
            r[1] = cap.card;
            r[2] = (cap.resample ? 1u : 0u) | (cap.refresh ? 2u : 0u);
            r[4] = cap.captureRect[0] | (cap.captureRect[1] << 16);
            r[5] = cap.captureRect[2] | (cap.captureRect[3] << 16);
            r[6] = cap.atlasRect[0] | (cap.atlasRect[1] << 16);
            r[7] = cap.atlasRect[2] | (cap.atlasRect[3] << 16);
            std::memcpy(&r[8], cap.cardUvRect, 16);
            round.anyResample = round.anyResample || cap.resample;
            if (cap.sceneInstance >= instances.size() || cap.sceneInstance >= src.instances.size() || cap.card >= scene->cardsGpu().size()) continue;
            const gpu::Instance& g = instances[cap.sceneInstance];
            const scene::Instance& in = src.instances[cap.sceneInstance];
            if (g.mesh >= src.meshes.size()) continue;
            const scene::Mesh& mesh = src.meshes[g.mesh];
            const McCardGpu& card = scene->cardsGpu()[cap.card];
            const float sx = g.objectToWorld[0].x, sy = g.objectToWorld[1].x, sz = g.objectToWorld[2].x;
            const float scale = std::sqrt(sx * sx + sy * sy + sz * sz);
            for (size_t sm = 0; sm < mesh.submeshes.size(); ++sm)
            {
                const scene::Submesh& sub = mesh.submeshes[sm];
                const uint32_t material = sm < in.materialOverrides.size() ? in.materialOverrides[sm] : sub.material;
                if (material >= materials.size() || sub.indexCount < 3) continue;
                const uint32_t cls = materials[material].classFlags & 0xFFu;
                // (translucent surfaces and strands are not in the cache)
                if (cls == (uint32_t)scene::MaterialClass::Water || cls == (uint32_t)scene::MaterialClass::Glass || cls == (uint32_t)scene::MaterialClass::Hair) continue;
                const bool twoSided = (materials[material].classFlags & (1u << 8)) != 0;
                Draw d{};
                d.k[0] = cap.sceneInstance;
                d.k[1] = sub.indexOffset;
                d.k[2] = sub.indexCount / 3;
                d.k[3] = material;
                d.k[4] = bits(card.origin[0]), d.k[5] = bits(card.origin[1]), d.k[6] = bits(card.origin[2]), d.k[7] = bits(scale);
                d.k[8] = bits(card.extent[0]), d.k[9] = bits(card.extent[1]), d.k[10] = bits(card.extent[2]);
                d.k[11] = (card.packed & 7u) | (twoSided ? 1u << 8 : 0u);
                std::memcpy(&d.k[12], cap.cardUvRect, 16);
                d.k[16] = tableSrv;
                d.groups = (sub.indexCount / 3 + 31) / 32;
                std::memcpy(d.rect, cap.captureRect, sizeof d.rect);
                round.draws.push_back(d);
            }
        }
        round.capturesAt = stage(records.data(), records.size() * 4);
    }
};

SurfaceCacheCards::SurfaceCacheCards() : m(std::make_unique<Impl>()) {}

SurfaceCacheCards::~SurfaceCacheCards()
{
    Impl& s = *m;
    s.cache.reset();  // (joins the generation workers)
    for (auto& u : s.upload)
        if (u.buffer) u.buffer->Unmap(0, nullptr);
}

SurfaceCacheCards& SurfaceCacheCards::get(FramePassContext& fc) { return fc.state<SurfaceCacheCards>("R.cards"); }
SurfaceCacheCards* SurfaceCacheCards::find(TrackState& state) { return &state.get<SurfaceCacheCards>("R.cards"); }
void SurfaceCacheCards::setConstantSky(float3 radiance, float3 sunIlluminance)
{
    m->skyRadiance = radiance;
    m->sunIlluminance = sunIlluminance;
}
const McStats& SurfaceCacheCards::stats() const { return m->lastStats; }
bool SurfaceCacheCards::loading() const { return m->settings.enabled && m->loadingState; }
void SurfaceCacheCards::waitForGeneration()
{
    if (m->cache) m->cache->wait();
}

void SurfaceCacheCards::declareRead(const FramePassContext& fc, PassBuilder& b, Use use) const
{
    const Impl& s = *m;
    if (s.frameIndex != fc.frame.frameIndex || !s.published.valid()) return;
    declareSurfaceCacheCards(b, s.published, use);
}

void SurfaceCacheCards::record(FramePassContext& fc, ViewResources& main, rt::RayScene& rays)
{
    Impl& s = *m;
    const uint64_t serial = fc.trackState ? fc.trackState->recordSerial() : 0;
    if (s.frameIndex == fc.frame.frameIndex && s.recordSerial == serial)
    {
        fc.resources.cards = s.published;
        return;
    }
    s.frameIndex = fc.frame.frameIndex;
    s.recordSerial = serial;
    s.published = SurfaceCacheCardRefs{};
    fc.resources.cards = SurfaceCacheCardRefs{};
    s.settings = SurfaceCacheCardSettings::fromQuality(fc.quality);
    if (!s.settings.enabled) return;
    s.device = &fc.device;
    if (!s.scene || !(s.scene->settings() == s.settings.cards))
    {
        s.scene = std::make_unique<MeshCardScene>(s.settings.cards);
        s.tracked.clear();
        s.restart = true;
        s.loadingState = true;
        for (bool& f : s.fullUpload) f = true;
    }
    if (!s.cache || s.cacheDirectory != s.settings.cacheDirectory)
    {
        s.cacheDirectory = s.settings.cacheDirectory;
        std::filesystem::path directory;
        if (s.cacheDirectory.empty()) directory = defaultMeshCardDirectory();
        else if (s.cacheDirectory != "none") directory = s.cacheDirectory;
        s.cache = std::make_unique<MeshCardCache>(directory);
        // (instances waiting for cards ask the new cache again)
        for (Tracked& t : s.tracked)
            if (t.state == 1) t.state = 0;
    }
    if (!s.lighting) s.lighting = std::make_unique<CardLighting>(fc.device);
    // a restore moves anything anywhere: the cache catches up at the load's pace
    if (fc.frame.discontinuity & kDiscontinuityRestore) s.loadingState = true;

    s.sync(fc);
    s.ensureBuffers();

    uint32_t tableSrv = 0xFFFFFFFFu;
#if UNX_SC_HAS_MATERIAL
    tableSrv = material::textureTable(fc);
#endif

    // ---- the frame's updates, staged
    const uint32_t rounds = s.loadingState ? s.settings.loadRounds : 1u;
    auto staged = std::make_shared<std::vector<Round>>(rounds);
    s.staging.clear();
    for (Round& round : *staged) s.stageRound(fc, round, tableSrv);
    s.lastStats = s.scene->stats();
    const bool generating = s.waiting > 0 || s.cache->pending() > 0;
    if (s.loadingState)
    {
        if (generating || s.lastStats.pending > 0) s.loadLightingLeft = s.settings.loadLightingRounds;
        else if (s.loadLightingLeft > rounds) s.loadLightingLeft -= rounds;
        else
        {
            s.loadLightingLeft = 0;
            s.loadingState = false;
            logf("surface cache: loaded - %u mesh card sets, %u cards (%u visible), %u pages, atlas %.1f %% in use\n", s.lastStats.meshCards, s.lastStats.cards,
                 s.lastStats.visibleCards, s.lastStats.mappedPages,
                 100.0 * (double)s.lastStats.allocatedTexels / ((double)s.settings.cards.atlasSize * s.settings.cards.atlasSize));
        }
    }
    Impl::UploadSlot& slot = s.upload[fc.frame.frameIndex % std::min(std::max(fc.framesInFlight, 1u), kUploadSlots)];
    if (slot.capacity < s.staging.size() || !slot.buffer)
    {
        // (the slot's last frame has completed: the caller waited for it before recording this one)
        if (slot.buffer) slot.buffer->Unmap(0, nullptr);
        s.release(slot.buffer);
        slot.capacity = std::max<uint64_t>(s.staging.size() + s.staging.size() / 2, 1u << 20);
        slot.buffer = s.makeBuffer(slot.capacity, D3D12_HEAP_TYPE_UPLOAD, L"R card upload");
        D3D12_RANGE none{ 0, 0 };
        check(slot.buffer->Map(0, &none, reinterpret_cast<void**>(&slot.mapped)), "map the mesh card upload buffer");
    }
    if (!s.staging.empty()) std::memcpy(slot.mapped, s.staging.data(), s.staging.size());
    ID3D12Resource* upload = slot.buffer.Get();

    // ---- the frame's graph resources
    RenderGraph& g = fc.graph;
    ShaderLibrary& shaders = fc.shaders;
    const uint32_t atlasSize = s.atlasSize, pageCapacity = s.settings.cards.maxPages, captureSize = s.scene->captureAtlasSize();
    BufferRef buffers[TCount];
    for (uint32_t t = 0; t < TCount; ++t) buffers[t] = g.importBuffer(s.buffers[t].Get(), { kBufferNames[t], (uint64_t)s.capacity[t] * kStride[t], 0 });
    TextureRef atlas[4];
    for (int a = 0; a < 4; ++a)
        atlas[a] = g.importTexture(s.atlas[a].Get(), { kAtlasNames[a], atlasSize, atlasSize, 1, 1, kAtlasFormats[a] }, D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS);
    const CardLightingRefs light = s.lighting->begin(fc, atlasSize, pageCapacity);

    CardSet set;
    set.instanceMap = buffers[TInstanceMap], set.meshCards = buffers[TMeshCards], set.cards = buffers[TCards], set.cardPages = buffers[TPages], set.pageTable = buffers[TPageTable];
    set.depth = atlas[0], set.albedo = atlas[1], set.normal = atlas[2], set.emissive = atlas[3];
    set.atlasSize = atlasSize;
    set.cardPageCapacity = pageCapacity;
    set.instances = (uint32_t)s.scene->instanceMap().size();
    set.generation = 0;

    // ---- the lighting's shared inputs: sky and sun (GiSky.hlsli), the ray scene
    uint32_t sceneWords[8];
    rays.rootConstants(sceneWords);
    const FrameResources& fr = fc.resources;
    const bool atmosphere = fr.transmittanceLut.valid() && fr.multiScatterLut.valid() && fr.skyViewLut.valid() && fr.aerialPerspective.valid();
    const TextureRef luts[4] = { fr.transmittanceLut, fr.multiScatterLut, fr.skyViewLut, fr.aerialPerspective };
    const float rayLength = fc.quality.has("gi.ray_length_m") ? (float)fc.quality.number("gi.ray_length_m") : 1000.0f;
    const float3 sky = s.skyRadiance, sun = s.sunIlluminance;
    rt::RayScene* rayScene = &rays;
    const BufferRef fxLights = fr.fxLights, fxLightCount = fr.fxLightCount;
    auto declareShared = [rayScene, atmosphere, luts, fxLights, fxLightCount](PassBuilder& b) {
        rayScene->declareTraversal(b);
        if (atmosphere)
            for (const TextureRef& t : luts) b.use(t, Use::SrvGraphics);
        if (fxLights.valid()) b.use(fxLights, Use::SrvGraphics);
        if (fxLightCount.valid()) b.use(fxLightCount, Use::SrvGraphics);
    };
    std::vector<uint32_t> sceneCopy(sceneWords, sceneWords + 8);
    auto sharedConstants = [atmosphere, luts, sky, sun, rayLength, sceneCopy](PassContext& c, uint32_t* k) {
        k[4] = bits(sky.x), k[5] = bits(sky.y), k[6] = bits(sky.z), k[7] = bits(rayLength);
        for (int i = 0; i < 4; ++i) k[8 + i] = atmosphere ? c.srv(luts[i]) : 0xFFFFFFFFu;
        k[12] = bits(sun.x), k[13] = bits(sun.y), k[14] = bits(sun.z);
        std::memcpy(&k[24], sceneCopy.data(), 32);
    };

    MeshPipelineDesc pipeline;
    pipeline.meshShader = "Passes/SurfaceCache/CardCapture.ms";
    pipeline.pixelShader = "Passes/SurfaceCache/CardCapture.ps";
    pipeline.renderTargets = { DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R11G11B10_FLOAT };
    pipeline.depthFormat = DXGI_FORMAT_D32_FLOAT;
    pipeline.depthFunc = D3D12_COMPARISON_FUNC_LESS;
    pipeline.cull = D3D12_CULL_MODE_NONE;
    ID3D12PipelineState* capturePipeline = shaders.mesh("r.card.capture", pipeline);
    const D3D12_GPU_VIRTUAL_ADDRESS frameConstants = main.frameConstants;
    const bool restart = s.restart;
    s.restart = false;
    const float depthBias = s.settings.depthBias, resampleFrames = s.settings.radiosityFrames;

    for (uint32_t r = 0; r < rounds; ++r)
    {
        const Round& round = (*staged)[r];
        set.cardPageCount = round.pageCount;
        set.valid = round.pageCount > 0;
        const uint32_t counter = ++s.updateCounter;
        // the card frame as the records stand before this round's upload (the resample reads the cards' old pages)
        s.lighting->recordFrame(fc, set, restart && r == 0, depthBias, counter);

        const TextureRef captureAlbedo = g.createTexture({ "r.card capture albedo", captureSize, captureSize, 1, 1, DXGI_FORMAT_R8G8B8A8_UNORM });
        const TextureRef captureNormal = g.createTexture({ "r.card capture normal", captureSize, captureSize, 1, 1, DXGI_FORMAT_R8G8_UNORM });
        const TextureRef captureEmissive = g.createTexture({ "r.card capture emissive", captureSize, captureSize, 1, 1, DXGI_FORMAT_R11G11B10_FLOAT });
        const TextureRef captureDepth = g.createTexture({ "r.card capture depth", captureSize, captureSize, 1, 1, DXGI_FORMAT_D32_FLOAT });
        const TextureRef resampledDirect = g.createTexture({ "r.card resampled direct", captureSize, captureSize, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
        const TextureRef resampledIndirect = g.createTexture({ "r.card resampled indirect", captureSize, captureSize, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT });
        const BufferRef captureList = g.createBuffer({ "r.card captures", (uint64_t)std::max(round.captures, 1u) * kCaptureBytes, 0 });

        g.addPass("r.card.capture", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(captureAlbedo, Use::RenderTarget);
                      b.use(captureNormal, Use::RenderTarget);
                      b.use(captureEmissive, Use::RenderTarget);
                      b.use(captureDepth, Use::DepthWrite);
                      b.use(captureList, Use::CopyDst);
                  },
                  [staged, r, capturePipeline, frameConstants, captureAlbedo, captureNormal, captureEmissive, captureDepth, captureList, upload, captureSize](PassContext& c) {
                      const Round& round = (*staged)[r];
                      if (round.captures == 0) return;
                      c.cmd->CopyBufferRegion(c.resource(captureList), 0, upload, round.capturesAt, (uint64_t)round.captures * kCaptureBytes);
                      const D3D12_CPU_DESCRIPTOR_HANDLE rtv[3] = { c.rtv(captureAlbedo), c.rtv(captureNormal), c.rtv(captureEmissive) }, dsv = c.dsv(captureDepth);
                      const float zero[4] = {}, half[4] = { 0.5f, 0.5f, 0, 0 };
                      c.cmd->ClearRenderTargetView(rtv[0], zero, 0, nullptr);
                      c.cmd->ClearRenderTargetView(rtv[1], half, 0, nullptr);
                      c.cmd->ClearRenderTargetView(rtv[2], zero, 0, nullptr);
                      c.cmd->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
                      c.cmd->OMSetRenderTargets(3, rtv, FALSE, &dsv);
                      c.cmd->SetPipelineState(capturePipeline);
                      c.bindFrameConstants(frameConstants);
                      for (const Draw& d : round.draws)
                      {
                          const D3D12_VIEWPORT vp{ (float)d.rect[0], (float)d.rect[1], (float)d.rect[2], (float)d.rect[3], 0, 1 };
                          const D3D12_RECT sc{ (LONG)d.rect[0], (LONG)d.rect[1], (LONG)(d.rect[0] + d.rect[2]), (LONG)(d.rect[1] + d.rect[3]) };
                          c.cmd->RSSetViewports(1, &vp);
                          c.cmd->RSSetScissorRects(1, &sc);
                          c.graphicsConstants(d.k, 20);
                          c.cmd->DispatchMesh(std::min(65535u, d.groups), (d.groups + 65534) / 65535, 1);
                      }
                      (void)captureSize;
                  });
        g.addPass("r.card.resample", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(captureList, Use::SrvCompute);
                      b.use(light.frame, Use::SrvCompute);
                      for (uint32_t t = 0; t < TCount; ++t) b.use(buffers[t], Use::SrvCompute);
                      b.use(atlas[0], Use::SrvCompute);
                      b.use(light.direct, Use::SrvCompute);
                      b.use(light.indirect, Use::SrvCompute);
                      b.use(light.frames, Use::SrvCompute);
                      b.use(resampledDirect, Use::UavCompute);
                      b.use(resampledIndirect, Use::UavCompute);
                  },
                  [staged, r, &shaders, captureList, light, resampledDirect, resampledIndirect](PassContext& c) {
                      const Round& round = (*staged)[r];
                      if (round.captures == 0 || !round.anyResample) return;
                      const uint32_t k[8] = { c.srv(captureList), round.captures, c.srv(light.frame), c.srv(light.frames), c.uav(resampledDirect), c.uav(resampledIndirect), 0, 0 };
                      c.cmd->SetPipelineState(shaders.compute("Passes/SurfaceCache/CardResample"));
                      c.computeConstants(k, 8);
                      c.cmd->Dispatch(kPageGroups, round.captures, 1);
                  });
        const std::array<BufferRef, TCount> bufferRefs = { buffers[0], buffers[1], buffers[2], buffers[3], buffers[4] };
        g.addPass("r.card.upload", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      for (const BufferRef& ref : bufferRefs) b.use(ref, Use::CopyDst);
                      b.keep();
                  },
                  [staged, r, bufferRefs, upload](PassContext& c) {
                      for (const CopyOp& op : (*staged)[r].ops) c.cmd->CopyBufferRegion(c.resource(bufferRefs[op.target]), op.destination, upload, op.source, op.bytes);
                  });
        g.addPass("r.card.copy", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(captureList, Use::SrvCompute);
                      b.use(captureDepth, Use::SrvCompute);
                      b.use(captureAlbedo, Use::SrvCompute);
                      b.use(captureNormal, Use::SrvCompute);
                      b.use(captureEmissive, Use::SrvCompute);
                      b.use(resampledDirect, Use::SrvCompute);
                      b.use(resampledIndirect, Use::SrvCompute);
                      for (const TextureRef& t : atlas) b.use(t, Use::UavCompute);
                      b.use(light.direct, Use::UavCompute);
                      b.use(light.indirect, Use::UavCompute);
                      b.use(light.final, Use::UavCompute);
                      b.use(light.frames, Use::UavCompute);
                      b.use(light.uniformBits, Use::UavCompute);
                      b.use(light.pageLight, Use::UavCompute);
                      b.keep();
                  },
                  [staged, r, &shaders, captureList, captureDepth, captureAlbedo, captureNormal, captureEmissive, resampledDirect, resampledIndirect, light, atlasSize, resampleFrames,
                   a0 = atlas[0], a1 = atlas[1], a2 = atlas[2], a3 = atlas[3]](PassContext& c) {
                      const Round& round = (*staged)[r];
                      if (round.captures == 0) return;
                      const uint32_t k[20] = { c.srv(captureList), round.captures, c.srv(captureDepth), c.srv(captureAlbedo),
                                               c.srv(captureNormal), c.srv(captureEmissive), c.uav(a0), c.uav(a1),
                                               c.uav(a2), c.uav(a3), c.uav(light.direct), c.uav(light.indirect),
                                               c.uav(light.final), c.uav(light.frames), c.uav(light.uniformBits), c.uav(light.pageLight),
                                               c.srv(resampledDirect), c.srv(resampledIndirect), atlasSize, bits(resampleFrames) };
                      c.cmd->SetPipelineState(shaders.compute("Passes/SurfaceCache/CardCopy"));
                      c.computeConstants(k, 20);
                      c.cmd->Dispatch(kPageGroups, round.captures, 1);
                  });

        if (!set.valid) continue;
        CardLightingInputs in;
        in.set = set;
        in.frame = counter;
        in.skyVariant = atmosphere ? 0u : 1u;
        in.frameConstants = frameConstants;
        in.direct = s.settings.direct;
        in.radiosity = s.settings.radiosity;
        in.shadowRaysOpaque = s.settings.shadowRaysOpaque;
        in.radiosityCap = s.settings.radiosityCap;
        in.radiosityFrames = s.settings.radiosityFrames;
        in.directFactor = s.settings.directFactor;
        in.radiosityFactor = s.settings.radiosityFactor;
        in.depthBias = depthBias;
        in.declareShared = declareShared;
        in.sharedConstants = sharedConstants;
        s.lighting->recordLighting(fc, in);
    }

    if (set.valid)
    {
        SurfaceCacheCardRefs out;
        out.frame = light.frame;
        out.instanceMap = buffers[TInstanceMap], out.meshCards = buffers[TMeshCards], out.cards = buffers[TCards], out.cardPages = buffers[TPages], out.pageTable = buffers[TPageTable];
        out.depth = atlas[0], out.albedo = atlas[1], out.normal = atlas[2], out.emissive = atlas[3];
        out.direct = light.direct, out.indirect = light.indirect, out.final = light.final;
        s.published = out;
        fc.resources.cards = out;
    }
    if (s.loadingState && fc.frame.frameIndex >= s.logFrame)
    {
        s.logFrame = fc.frame.frameIndex + 30;
        logf("surface cache: loading - %u instances wait for cards (%u meshes generating), %u cards to capture, %u pages, %u rounds a frame\n", s.waiting,
             s.cache->pending(), s.lastStats.pending, s.lastStats.mappedPages, rounds);
    }
}
} // namespace unx::render::refl
