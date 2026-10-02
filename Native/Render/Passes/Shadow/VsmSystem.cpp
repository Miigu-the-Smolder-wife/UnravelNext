#include "VsmSystem.h"

#include "FroxelSystem.h"
#include "SResources.h"

#include "unx/render/GpuScene.h"
#include "unx/render/PassChain.h"
#include "unx/render/Tracks.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace unx::render::shadow
{
using namespace s_detail;

namespace
{
const char* const kStateKey = "s.vsm";
constexpr uint32_t kRingSlots = 16, kRingStride = 2048;  // per-frame constants; frames in flight must be < kRingSlots
constexpr uint32_t kStatsSlots = 4, kStatsBytes = 512;  // VSM stats words (128; VsmBegin clears them; 64.. L3 / L4 counters)
constexpr uint64_t kOverflowMinWords = 1u << 18;  // 1 MB overflow list at least (INTERFACES 7.3)
constexpr uint32_t kErrRasterOverflow = 0x20;  // VsmCommon.hlsli VSM_ERR_RASTER_OVERFLOW
constexpr uint32_t kMetaBytes = 48;  // VsmPageMeta
constexpr uint32_t kBlockEntries = 341;  // VSM_BLOCK_ENTRIES: blocks of 8, 16, 32, 64 and 128 texels of a page
constexpr uint32_t kBlockBytes = kBlockEntries * 32;  // x VsmBlock

struct State
{
    bool initialized = false;
    bool needsInit = false;  // atlas (re)created: the next frame resets the table, requests, metadata and layers
    uint32_t atlasPages = 0;  // physical pages (128 per atlas row)
    ComPtr<ID3D12Resource> atlas;  // D32 page atlas (FrameResources::vsmAtlas)
    ComPtr<ID3D12Resource> table, requests, meta, blocks, pageList, cullMask, atlasSlots, groups, args, stats, ring;
    ComPtr<ID3D12Resource> layers;  // transmittance layer (VsmLayer.hlsli): per physical page words, then layer pages
    uint64_t layersBytes = 0;
    ComPtr<ID3D12CommandSignature> dispatchSignature;
    uint32_t atlasSrv = UINT32_MAX;  // the atlas SRV every lookup reads (VsmConstants::atlasSrv)
    // shadow.vsm.static_separate: the pages' static copies (the casters that are not movable; a page at the same place as
    // in the sampled atlas) and the kept pages whose movable casters are drawn anew this frame (VsmScan MODE 2).
    ComPtr<ID3D12Resource> atlasStatic, dynamicList;
    bool separate = false;  // the state was created with the static atlas
    // shadow.vsm.static_hzb_cull: the static copies' HZB (VsmStaticHzb.hlsl: kBlockEntries floats per physical page) and
    // the raw SRVs V's cull kernels read it and the sun's atlas slots through (DepthRasterRequest::tileOccluders).
    ComPtr<ID3D12Resource> staticHzb;
    uint32_t staticHzbSrv = UINT32_MAX, atlasSlotsSrv = UINT32_MAX;
    bool hzbValid = false;  // the kept pages' static copies have their HZB (built in the frames that drew them)
    // shadow.vsm.static_occlusion_two_phase: per sun page drawn anew, the kept coarser page that stands for its occluders
    // in the static casters' first cull phase (VsmCullMask.hlsl; DepthRasterRequest::tileGuess) and its raw SRV.
    ComPtr<ID3D12Resource> occluderGuess;
    uint32_t occluderGuessSrv = UINT32_MAX;
    uint32_t ringCbv[kRingSlots] = {};  // constant buffer view of each ring slot (ConstantBuffer<VsmConstants>)
    uint8_t* ringMapped = nullptr;
    // Stats readback ring: slot i holds the counters of frame statsFrame[i], complete once the graphics queue passes
    // statsFence[i] (the fence of the frame after it was recorded is known at the next record).
    ComPtr<ID3D12Resource> statsReadback;
    uint64_t statsFrame[kStatsSlots] = {};
    uint64_t statsFence[kStatsSlots] = {};
    int lastStatsSlot = -1;
    uint32_t errorBitsSeen = 0;  // OR of the error words of every harvested frame (INTERFACES 3.6)
    // V's raster runs of the pages (s.vsm.raster, s.vsm.localraster<k>): the newest frame whose overflow was handled, and
    // whether this frame draws every page anew because of one (no sun page kept; every local slot's generation moved).
    uint64_t rasterOverflowFrame = UINT64_MAX;
    uint32_t rasterOverflows = 0;  // overflowed frames handled (logged up to 8)
    uint32_t rasterRequests[2] = { UINT32_MAX, UINT32_MAX };  // the last frame's sun and local raster requests (logged on change)
    bool rasterRedraw = false;
    VsmStats latest;
    uint32_t prevSlotsUsed = 0;  // slots the previous frame scanned (a shrinking local-light set clears its old slots)
    uint64_t frames = 0;
    VsmConstantsCpu constants{};
    uint32_t constantsOffset = 0;
    // This frame's graph handles (valid between shadowPages and the end of the frame's recording).
    uint64_t recordedFrame = UINT64_MAX;
    TextureRef atlasRef;
    BufferRef tableRef, metaRef, boundRef, blocksRef, statsRef, layersRef, useRef;
    ComPtr<ID3D12Resource> use;  // shadow.vsm.use_stats read bits (created on first use)
    uint32_t useUav = UINT32_MAX;
    bool pagesRecorded = false;
    bool debugPaths = false;
    // Atlas growth: a frame whose requests exceeded it sets the next size (never shrinks while running).
    uint32_t poolTarget = 0;
    uint64_t grownAtFrame = 0;
    // Overflow list capacity in words (power of two, at least kOverflowMinWords): 1.5 x the last completed frame's need.
    uint32_t overflowCapacity = 0;
    // Local-light shadows: shadow slot -> scene light + 1 (0 = free), generation, geometry signature; per-frame uploads.
    uint32_t localLight[kLocalLights] = {};
    uint32_t localGen[kLocalLights] = {};
    float localSig[kLocalLights][6] = {};
    VsmLocalLightCpu localData[kLocalLights] = {};
    std::vector<uint32_t> slotOfLight;   // scene light -> shadow slot or 0xFFFF
    std::vector<uint32_t> localActive;   // shadow slots whose light meets the main view (raster views this frame)
    uint32_t localUsed = 0;              // highest assigned slot + 1
    uint32_t localSceneRevision = UINT32_MAX;
    uint32_t localWithoutSlot = 0;
    ComPtr<ID3D12Resource> localRing, localMask, localSlots;
    ComPtr<ID3D12Resource> clsAtlas;  // L3: classification pages (VsmCls.hlsli), kLocalLights x 6 pages of 128^2 x 4 B
    ComPtr<ID3D12Resource> clsTwin;   // stage 2: the exact-raster twin (umbra), the same layout
    uint32_t clsTwinUav = UINT32_MAX;
    BufferRef clsBlocksRef;  // this frame's block maxima (frameRefs); invalid when the classification is off
    uint32_t clsActiveCount = 0;
    uint64_t clsAtlasBytes = 0;
    uint32_t clsAtlasUav = UINT32_MAX;  // raw UAV descriptor (V's pixel kernel root constant)
    uint8_t* localMapped = nullptr;
    uint32_t localStride = 0, localCap = 0;
    uint32_t localLightsSrv[kRingSlots] = {}, localSlotOfSrv[kRingSlots] = {}, localActiveSrv[kRingSlots] = {};
    uint32_t localLightsNow = UINT32_MAX, slotOfNow = UINT32_MAX, activeNow = UINT32_MAX;
    // Sun page cache (shadow.vsm.cache, VsmCache.hlsl): per-instance caster state (uint4: revisions, cast flags), the
    // caster height range the kept pages were drawn with (kept while the frame's range fits in it), and what makes a
    // frame cacheable (consecutive frames, same sun, range, scene revision and atlas).
    ComPtr<ID3D12Resource> casterState;
    uint32_t casterStateCount = 0;
    uint64_t cacheFrame = UINT64_MAX;
    float3 cacheSun{};
    float cacheHMin = 0, cacheHMax = 0;
    bool cacheRange = false;
    uint32_t cacheRevision = UINT32_MAX;
    float cacheWind[4] = { -1, 0, 0, 0 };  // the scene wind the kept pages' wind casters were bounded with
};

constexpr uint32_t kAtlasPagesPerRow = 128;                                        // VsmCommon.hlsli VSM_ATLAS_PAGES_PER_ROW
constexpr uint32_t kLocalFaceWords = 173, kLocalLightWords = 6 * kLocalFaceWords;  // VsmLocal.hlsli
constexpr uint32_t kLocalViewWordOffset[kLocalMips] = { 0, 1, 2, 3, 5, 13, 45 };
constexpr uint32_t kScanGroupSlots = 1024;  // VsmScan.hlsl
constexpr uint32_t kScanGroupsMax = (kTotalSlots + kScanGroupSlots - 1) / kScanGroupSlots;
constexpr uint32_t kCacheWords = 24;  // VsmCache.hlsl root constants P[0..5]

// Views of the resources created once (constants ring).
void createFixedViews(Device& device, State& s)
{
    DescriptorHeaps& h = device.descriptors();
    for (uint32_t i = 0; i < kRingSlots; ++i)
    {
        s.ringCbv[i] = h.allocateResource();
        D3D12_CONSTANT_BUFFER_VIEW_DESC cd{ s.ring->GetGPUVirtualAddress() + (uint64_t)i * kRingStride, kRingStride };
        device.d3d()->CreateConstantBufferView(&cd, h.resourceCpu(s.ringCbv[i]));
    }
}

// Page-count-dependent resources (atlas, metadata, blocks, page list, layers); the first call also creates the fixed
// ones. A regrown atlas: the next frame resets the table (VsmInit); every frame draws all its pages anyway.
void createState(FramePassContext& fc, State& s, uint32_t pages, bool separate)
{
    const QualityConfig& q = fc.quality;
    if (q.integer("shadow.vsm.virtual_resolution") != kVirtual || q.integer("shadow.vsm.page_texels") != kPage || q.integer("shadow.vsm.clipmap_levels") != kLevels)
        fail("shadow.vsm: virtual_resolution / page_texels / clipmap_levels are compiled into the S kernels (16384 / 128 / 20)");
    if (pages == 0 || pages % kAtlasPagesPerRow || pages / kAtlasPagesPerRow * kPage > 16384)
        fail("VSM atlas: %u pages is not a positive multiple of %u within one 16384^2 atlas", pages, kAtlasPagesPerRow);
    Device& d = fc.device;
    for (ComPtr<ID3D12Resource>* r : { std::addressof(s.atlas), std::addressof(s.meta), std::addressof(s.blocks), std::addressof(s.pageList), std::addressof(s.layers),
                                       std::addressof(s.atlasStatic), std::addressof(s.dynamicList), std::addressof(s.staticHzb) })
        if (*r)
        {
            d.deferRelease(*r);
            r->Reset();
        }
    s.atlasPages = pages;
    s.separate = separate;
    {
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC1 rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = kAtlasPagesPerRow * kPage;
        rd.Height = pages / kAtlasPagesPerRow * kPage;
        rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_R32_TYPELESS;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE clear{};
        clear.Format = DXGI_FORMAT_D32_FLOAT;
        clear.DepthStencil.Depth = 0.0f;  // no caster
        check(d.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE, &clear, nullptr, 0, nullptr,
                                                IID_PPV_ARGS(&s.atlas)),
              "S VSM atlas");
        s.atlas->SetName(L"S VSM atlas");
        if (separate)
        {
            // (the same size and format: a page's static copy is at its sampled page's place)
            check(d.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_SHADER_RESOURCE, &clear, nullptr, 0, nullptr,
                                                    IID_PPV_ARGS(&s.atlasStatic)),
                  "S VSM static atlas");
            s.atlasStatic->SetName(L"S VSM static atlas");
        }
        DescriptorHeaps& h = d.descriptors();
        s.atlasSrv = h.allocateResource();
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_FLOAT;
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        d.d3d()->CreateShaderResourceView(s.atlas.Get(), &sd, h.resourceCpu(s.atlasSrv));
    }
    s.meta = createBuffer(d, L"S VSM page metadata", (uint64_t)pages * kMetaBytes);
    s.blocks = createBuffer(d, L"S VSM page blocks", (uint64_t)pages * kBlockBytes);
    s.pageList = createBuffer(d, L"S VSM page list", 8 + (uint64_t)pages * 8);
    s.dynamicList = createBuffer(d, L"S VSM dynamic page list", 8 + (uint64_t)pages * 8);
    // A raw SRV of a buffer for readers that take the index from a record, not from their pass (V's cull views).
    auto rawSrv = [&](ID3D12Resource* buffer, uint64_t bytes) {
        DescriptorHeaps& h = d.descriptors();
        const uint32_t index = h.allocateResource();
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Buffer.NumElements = (UINT)(bytes / 4);
        sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
        d.d3d()->CreateShaderResourceView(buffer, &sd, h.resourceCpu(index));
        return index;
    };
    if (separate)
    {
        s.staticHzb = createBuffer(d, L"S VSM static HZB", (uint64_t)pages * kBlockEntries * 4);
        s.staticHzbSrv = rawSrv(s.staticHzb.Get(), (uint64_t)pages * kBlockEntries * 4);
    }
    // Transmittance layer: the per-page words (0 = no layer) until V's coverage-mode raster fills layer pages (v1.26).
    s.layersBytes = ((uint64_t)pages * 4 + 255) & ~255ull;
    s.layers = createBuffer(d, L"S VSM transmittance layer", s.layersBytes);
    if (!s.table)
    {
        s.table = createBuffer(d, L"S VSM page table", (uint64_t)kTotalSlots * 8);
        s.requests = createBuffer(d, L"S VSM requests", (uint64_t)kTotalSlots * 4);
        s.localMask = createBuffer(d, L"S VSM local cull mask", (uint64_t)kLocalLights * kLocalLightWords * 4);
        s.localSlots = createBuffer(d, L"S VSM local atlas slots", (uint64_t)kLocalLights * kLocalLightWords * 32 * 4);
        // L3: the classification atlas, 48 pages per row (VSM_CLS_PAGES_PER_ROW), rows for kLocalLights x 6 pages
        s.clsAtlasBytes = (uint64_t)48 * 128 * ((kLocalLights * 6 + 47) / 48) * 128 * 4;
        s.clsAtlas = createBuffer(d, L"S VSM classification atlas", s.clsAtlasBytes);
        {
            DescriptorHeaps& h = d.descriptors();
            s.clsAtlasUav = h.allocateResource();
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_R32_TYPELESS;
            ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            ud.Buffer.NumElements = (UINT)(s.clsAtlasBytes / 4);
            ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            d.d3d()->CreateUnorderedAccessView(s.clsAtlas.Get(), nullptr, &ud, h.resourceCpu(s.clsAtlasUav));
            s.clsTwin = createBuffer(d, L"S VSM classification twin", s.clsAtlasBytes);
            s.clsTwinUav = h.allocateResource();
            d.d3d()->CreateUnorderedAccessView(s.clsTwin.Get(), nullptr, &ud, h.resourceCpu(s.clsTwinUav));
        }
        // (two sets of sun mask words and slots: every caster / the static casters, then the movable casters - VsmCullMask)
        s.cullMask = createBuffer(d, L"S VSM cull mask", (uint64_t)kSlots / 8 * 2);
        s.atlasSlots = createBuffer(d, L"S VSM atlas slots", (uint64_t)kSlots * 4 * 2);
        s.atlasSlotsSrv = rawSrv(s.atlasSlots.Get(), (uint64_t)kSlots * 4 * 2);
        s.occluderGuess = createBuffer(d, L"S VSM occluder guess", (uint64_t)kSlots * 8);
        s.occluderGuessSrv = rawSrv(s.occluderGuess.Get(), (uint64_t)kSlots * 8);
        s.groups = createBuffer(d, L"S VSM scan groups", (uint64_t)kScanGroupsMax * 4);
        s.args = createBuffer(d, L"S VSM indirect args", 32);  // the page list's dispatch, then the dynamic page list's
        s.stats = createBuffer(d, L"S VSM stats", kStatsBytes);
        s.ring = createBuffer(d, L"S VSM constants ring", (uint64_t)kRingSlots * kRingStride, D3D12_HEAP_TYPE_UPLOAD);
        s.statsReadback = createBuffer(d, L"S VSM stats readback", (uint64_t)kStatsSlots * kStatsBytes, D3D12_HEAP_TYPE_READBACK);
        D3D12_RANGE none{ 0, 0 };
        check(s.ring->Map(0, &none, reinterpret_cast<void**>(&s.ringMapped)), "map VSM ring");
        D3D12_INDIRECT_ARGUMENT_DESC arg{};
        arg.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH;
        D3D12_COMMAND_SIGNATURE_DESC sig{};
        sig.ByteStride = 12;
        sig.NumArgumentDescs = 1;
        sig.pArgumentDescs = &arg;
        check(d.d3d()->CreateCommandSignature(&sig, nullptr, IID_PPV_ARGS(&s.dispatchSignature)), "VSM dispatch signature");
        createFixedViews(d, s);
    }
    s.initialized = false;
    s.needsInit = true;
}

// Light basis: z towards the sun, x horizontal, y completing a right-handed frame.
void lightBasis(float3 z, float3& x, float3& y)
{
    const float3 up = std::abs(z.y) < 0.999f ? float3{ 0, 1, 0 } : float3{ 1, 0, 0 };
    x = normalize(cross(up, z));
    y = cross(z, x);
}

// Light-space height range of every shadow caster (bounding spheres), with a margin for dynamic motion.
void casterHeightRange(const GpuScene& scene, float3 z, float& lo, float& hi)
{
    lo = 1e30f;
    hi = -1e30f;
    const auto& inst = scene.instances();
    const auto& meshes = scene.meshes();
    for (const gpu::Instance& i : inst)
    {
        if ((i.flags & scene::InstanceCastShadow) == 0) continue;
        const gpu::Mesh& m = meshes[i.mesh];
        const float3 c{ m.boundsSphere.x, m.boundsSphere.y, m.boundsSphere.z };
        const float3 w{ i.objectToWorld[0].x * c.x + i.objectToWorld[0].y * c.y + i.objectToWorld[0].z * c.z + i.objectToWorld[0].w,
                        i.objectToWorld[1].x * c.x + i.objectToWorld[1].y * c.y + i.objectToWorld[1].z * c.z + i.objectToWorld[1].w,
                        i.objectToWorld[2].x * c.x + i.objectToWorld[2].y * c.y + i.objectToWorld[2].z * c.z + i.objectToWorld[2].w };
        const float scale = length(float3{ i.objectToWorld[0].x, i.objectToWorld[0].y, i.objectToWorld[0].z });
        const float h = dot(w, z), r = m.boundsSphere.w * scale;
        lo = std::min(lo, h - r);
        hi = std::max(hi, h + r);
    }
    if (lo > hi) lo = hi = 0;
}

float4x4 levelViewProj(const VsmConstantsCpu& c, uint32_t k)
{
    const VsmLevelCpu& L = c.level[k];
    const float t = std::ldexp(1.0f, (int)k - 10), V = (float)kVirtual, range = L.hMax - L.hMin;
    const float ox = (float)L.origin[0] * kPage, oy = (float)L.origin[1] * kPage;
    float4x4 m;
    m.m[0][0] = 2 * L.lightX.x / (t * V); m.m[0][1] = 2 * L.lightX.y / (t * V); m.m[0][2] = 2 * L.lightX.z / (t * V); m.m[0][3] = -2 * ox / V - 1;
    m.m[1][0] = -2 * L.lightY.x / (t * V); m.m[1][1] = -2 * L.lightY.y / (t * V); m.m[1][2] = -2 * L.lightY.z / (t * V); m.m[1][3] = 1 + 2 * oy / V;
    // Depth v = (h - hMin) / (hMax - hMin): reversed-Z (the surface nearest the sun has the largest value), 0 = cleared.
    m.m[2][0] = L.lightZ.x / range; m.m[2][1] = L.lightZ.y / range; m.m[2][2] = L.lightZ.z / range; m.m[2][3] = -L.hMin / range;
    m.m[3][0] = 0; m.m[3][1] = 0; m.m[3][2] = 0; m.m[3][3] = 1;
    return m;
}

uint32_t groups(uint64_t n, uint32_t size) { return (uint32_t)((n + size - 1) / size); }

// The visible-list bound of V's raster runs (coordinator 10-01: the lists must not overflow by construction, train
// lounge frame 0 needed 1.67 M entries in one run of 1 M). A run's visible list holds one entry per (cluster, view) it
// draws, and a view draws one LOD cut of an instance's hierarchy, so an instance adds at most its mesh's cut bound (the
// builder's ClusterData::MeshRange::cutBound: its leaf clusters) to a view it can reach. Summed over the shadow-casting instances a view can reach - by
// their bounding spheres, from the scene's structure, not from what the frame draws - this bounds the view's entries,
// and the requests are packed so that their views' bounds sum to at most the list capacity.
// Per view (defect queue 1b, shadow.vsm.raster_lod_bound): the cut a view draws depends on its texel size - a sun level
// of 2^k times the texel, or a local face's coarser mip, draws a coarser cut - so an instance adds
// MeshRange::cutBoundAt(lo, hi) with the view's object-space LOD thresholds over the instance (V's test: error x
// instance scale x pixels per metre / distance against visibility.lod_error_px; orthographic views without the
// distance). Counting the leaves in every view packed the train lounge's 20 sun levels into 14 requests and the bath
// lounge's 75 lights into 22 (one cull chain each, +0.5-0.8 ms per frame [measured 2026-10-01]).
struct CasterBounds
{
    std::vector<float4> spheres;  // world bounding sphere per placed caster
    std::vector<uint32_t> instance;   // its scene instance (ascending)
    std::vector<uint32_t> clusters;
    std::vector<uint32_t> mesh;       // per placed caster: its mesh (kNone: no per-view bound, 'clusters' in every view it reaches),
    std::vector<float> errorScale;    // V's instanceScale (the error's object-to-world factor),
    std::vector<float4> lodSpheres;   // and the world sphere holding its LOD spheres (the distances V projects errors at)
    std::vector<uint8_t> movable;     // per placed caster: gpu::instanceMovable or a run-time instance (V's instance sets)
    uint64_t everywhere = 0;  // casters whose geometry moves past its bind-pose sphere (skinned, morphs, wind) and the
                              // GPU-written instances' capacity (any mesh): counted in every view
    struct Moving
    {
        uint32_t mesh;   // kNone: no table (a runtime mesh)
        float errorScale;
        uint32_t whole;  // its cut bound
    };
    std::vector<Moving> everywhereMeshes;  // those casters: orthographic views bound them by LOD
    uint64_t everywhereFixed = 0;                              // the part of 'everywhere' without a mesh entry (GPU-written instances)
    const std::vector<render::ClusterData::MeshRange>* ranges = nullptr;
    float thresholdPx = 1;
    bool lod = false;
    // shadow.vsm.min_caster_texels: V leaves a placed caster out of a view where its sphere is under this many texels in
    // radius (RasterView::minInstanceTexels), so it adds nothing to that view's bound. The spheres here are not smaller
    // than V's (the largest axis scale, x 1.001 + 1 mm), so a caster left out of the bound is left out by V too.
    float minTexels = 0;
};

// windSpeed: the frame's (m/s). A wind-moved caster stays within its sphere grown by the wind's bound
// (Deformation.hlsli windOffsetBound, as V's culling takes it): it counts in the views it reaches, not in every view -
// a forest of 1.1 M such trees stood at 4e8 cluster entries in level 0's 16 m window.
CasterBounds casterBounds(const GpuScene& scene, bool lod, float thresholdPx, float windSpeed, float minTexels)
{
    CasterBounds b;
    b.ranges = &scene.clusters().meshes;
    b.lod = lod;
    b.thresholdPx = thresholdPx;
    b.minTexels = minTexels;
    const auto& meshes = scene.meshes();
    const auto& ranges = scene.clusters().meshes;  // the builder's cut bounds (runtime meshes: their whole hierarchies)
    auto cut = [&](uint32_t mesh) { return mesh < ranges.size() && ranges[mesh].cutBound != 0 ? ranges[mesh].cutBound : meshes[mesh].clusterCount; };
    uint32_t largest = 0;
    for (const gpu::Mesh& m : meshes) largest = std::max(largest, m.clusterCount);
    for (const gpu::Instance& inst : scene.instances())
    {
        if ((inst.flags & gpu::kInstanceHidden) != 0 || (inst.flags & scene::InstanceCastShadow) == 0 || inst.mesh >= meshes.size()) continue;
        const gpu::Mesh& m = meshes[inst.mesh];
        const float4* r = inst.objectToWorld;
        const float errorScale = std::sqrt(r[0].x * r[0].x + r[0].y * r[0].y + r[0].z * r[0].z);  // VisibilityCommon.hlsli instanceScale
        if ((inst.flags & scene::InstanceSkinned) != 0 || inst.morph != gpu::kNone)
        {
            b.everywhere += cut(inst.mesh);
            b.everywhereMeshes.push_back({ inst.mesh < ranges.size() && ranges[inst.mesh].cutErrorBase > 0 ? inst.mesh : gpu::kNone, errorScale, cut(inst.mesh) });
            continue;
        }
        const float3 c = { m.boundsSphere.x, m.boundsSphere.y, m.boundsSphere.z };
        const float3 w = { r[0].x * c.x + r[0].y * c.y + r[0].z * c.z + r[0].w, r[1].x * c.x + r[1].y * c.y + r[1].z * c.z + r[1].w,
                           r[2].x * c.x + r[2].y * c.y + r[2].z * c.z + r[2].w };
        const float scale = std::max({ std::sqrt(r[0].x * r[0].x + r[0].y * r[0].y + r[0].z * r[0].z), std::sqrt(r[1].x * r[1].x + r[1].y * r[1].y + r[1].z * r[1].z),
                                       std::sqrt(r[2].x * r[2].x + r[2].y * r[2].y + r[2].z * r[2].z) });
        float radius = m.boundsSphere.w;
        if ((inst.flags & scene::InstanceWind) != 0 && inst.windStiffness > 0)
        {
            const float h = std::max(c.y + m.boundsSphere.w - inst.windAnchor, 0.0f);
            radius += 0.002f / inst.windStiffness * h * h * windSpeed * windSpeed;
        }
        b.spheres.push_back({ w.x, w.y, w.z, radius * scale * 1.001f + 1e-3f });
        b.instance.push_back((uint32_t)(&inst - scene.instances().data()));
        b.movable.push_back(gpu::instanceMovable(inst) || b.instance.back() >= scene.staticInstanceCount() ? 1 : 0);
        b.clusters.push_back(cut(inst.mesh));
        // per-view bound: not for terrain patches (C5: their replaced rectangles force the source clusters)
        const bool table = lod && inst.patch == gpu::kNone && inst.mesh < ranges.size() && ranges[inst.mesh].cutErrorBase > 0;
        b.mesh.push_back(table ? inst.mesh : gpu::kNone);
        b.errorScale.push_back(errorScale);
        float4 ls{ w.x, w.y, w.z, 0 };
        if (table)
        {
            const float4 o = ranges[inst.mesh].lodBounds;
            ls = { r[0].x * o.x + r[0].y * o.y + r[0].z * o.z + r[0].w, r[1].x * o.x + r[1].y * o.y + r[1].z * o.z + r[1].w,
                   r[2].x * o.x + r[2].y * o.y + r[2].z * o.z + r[2].w, o.w * scale * 1.001f + 1e-3f };
        }
        b.lodSpheres.push_back(ls);
    }
    b.everywhereFixed = (uint64_t)scene.gpuInstanceRange().capacity * largest;
    b.everywhere += b.everywhereFixed;
    return b;
}

// A caster's clusters in a view whose object-space LOD thresholds over it are [lo, hi] (1e-4 of slack for V's float
// evaluation of the same products).
uint32_t casterCut(const CasterBounds& b, size_t i, float lo, float hi)
{
    if (b.mesh[i] == gpu::kNone) return b.clusters[i];
    return std::min((*b.ranges)[b.mesh[i]].cutBoundAt(lo * (1 - 1e-4f), hi * (1 + 1e-4f)), b.clusters[i]);
}

// The bound of an orthographic sun level view (levelViewProj: a sphere reaches it when its NDC x, y interval meets
// [-1, 1]; every depth: the level's caster range follows the casters).
// A placed caster in a view of an instance set (RasterView::instanceSet: 0 every instance, 1 not movable, 2 movable).
bool inSet(const CasterBounds& b, size_t i, uint32_t set) { return set == 0 || (set == 2) == (b.movable[i] != 0); }

// The casters counted in every view (CasterBounds::everywhere) at a level's texel: skinned and morphed ones and the
// GPU-written instances, all movable - a view of the instances that are not movable has none of them.
uint64_t everywhereBound(const CasterBounds& b, float pixelsPerMetre, uint32_t set)
{
    if (set == 1) return 0;
    if (!b.lod) return b.everywhere;
    const float threshold = b.thresholdPx / pixelsPerMetre;
    uint64_t n = b.everywhereFixed;
    for (const CasterBounds::Moving& m : b.everywhereMeshes)
    {
        const float t = threshold / std::max(m.errorScale, 1e-12f);
        n += m.mesh == gpu::kNone ? m.whole : std::min((*b.ranges)[m.mesh].cutBoundAt(t * (1 - 1e-4f), t * (1 + 1e-4f)), m.whole);
    }
    return n;
}
// A level whose bound is over the list capacity, as instance batches (RasterView::instanceFirst / instanceEnd): the placed
// casters the view reaches, in instance order, cut into ranges whose bounds fit beside the casters counted in every
// view (those are drawn in the range their index falls in: counted in each). Empty: no such split (the every-view
// casters alone, or one caster, are over the capacity).
struct LevelBatch
{
    uint32_t first, end;
};
std::vector<LevelBatch> levelBatches(const CasterBounds& b, const float4x4& vp, float pixelsPerMetre, uint64_t capacity, uint32_t set)
{
    std::vector<LevelBatch> out;
    const uint64_t every = everywhereBound(b, pixelsPerMetre, set);
    if (every >= capacity) return out;
    const uint64_t room = capacity - every;
    const float threshold = b.thresholdPx / pixelsPerMetre;
    const float sx = std::sqrt(vp.m[0][0] * vp.m[0][0] + vp.m[0][1] * vp.m[0][1] + vp.m[0][2] * vp.m[0][2]);
    const float sy = std::sqrt(vp.m[1][0] * vp.m[1][0] + vp.m[1][1] * vp.m[1][1] + vp.m[1][2] * vp.m[1][2]);
    uint64_t sum = 0;
    uint32_t first = 0;
    for (size_t i = 0; i < b.spheres.size(); ++i)
    {
        const float4& q = b.spheres[i];
        const float x = vp.m[0][0] * q.x + vp.m[0][1] * q.y + vp.m[0][2] * q.z + vp.m[0][3];
        const float y = vp.m[1][0] * q.x + vp.m[1][1] * q.y + vp.m[1][2] * q.z + vp.m[1][3];
        if (!(std::abs(x) <= 1 + q.w * sx && std::abs(y) <= 1 + q.w * sy)) continue;
        if (b.minTexels > 0 && q.w * pixelsPerMetre < b.minTexels) continue;  // (under the level's smallest caster: not drawn)
        if (!inSet(b, i, set)) continue;
        const float t = threshold / std::max(b.errorScale[i], 1e-12f);
        const uint64_t cut = b.lod ? casterCut(b, i, t, t) : b.clusters[i];
        if (cut > room) return {};
        if (sum + cut > room)
        {
            out.push_back({ first, b.instance[i] });
            first = b.instance[i];
            sum = 0;
        }
        sum += cut;
    }
    out.push_back({ first, 0xFFFFFFFFu });
    return out;
}

uint64_t levelBound(const CasterBounds& b, const float4x4& vp, float pixelsPerMetre, uint32_t set)
{
    // (the moving casters too: their cut depends on the level's texel, not on where they are)
    uint64_t n = everywhereBound(b, pixelsPerMetre, set);
    const float threshold = b.thresholdPx / pixelsPerMetre;  // world-space error at the LOD threshold (every distance: orthographic)
    const float sx = std::sqrt(vp.m[0][0] * vp.m[0][0] + vp.m[0][1] * vp.m[0][1] + vp.m[0][2] * vp.m[0][2]);
    const float sy = std::sqrt(vp.m[1][0] * vp.m[1][0] + vp.m[1][1] * vp.m[1][1] + vp.m[1][2] * vp.m[1][2]);
    for (size_t i = 0; i < b.spheres.size(); ++i)
    {
        const float4& q = b.spheres[i];
        const float x = vp.m[0][0] * q.x + vp.m[0][1] * q.y + vp.m[0][2] * q.z + vp.m[0][3];
        const float y = vp.m[1][0] * q.x + vp.m[1][1] * q.y + vp.m[1][2] * q.z + vp.m[1][3];
        if (!(std::abs(x) <= 1 + q.w * sx && std::abs(y) <= 1 + q.w * sy)) continue;
        if (b.minTexels > 0 && q.w * pixelsPerMetre < b.minTexels) continue;  // (under the level's smallest caster: not drawn)
        if (!inSet(b, i, set)) continue;
        const float t = threshold / std::max(b.errorScale[i], 1e-12f);
        n += b.lod ? casterCut(b, i, t, t) : b.clusters[i];
    }
    return n;
}

// The bounds of a local light's six cube faces (each shared by the face's mip views): the casters its reach sphere (range +
// emitter radius) meets that also meet the face's 90-degree frustum (the four side planes (axis +- right) / sqrt 2,
// (axis +- up) / sqrt 2, and in front of the light) - a caster in one face's quarter of space is not counted in the others.
void cubeBasis(uint32_t face, float3& right, float3& up, float3& axis);
// Per mip (kLocalMips views of a face, pixels per metre at distance 1 = 64 x 2^mip): the caster's cut at that mip's
// thresholds over the distances from the light to its LOD spheres (V: max(distance - radius, near 1e-4)).
void localBounds(const CasterBounds& b, const VsmLocalLightCpu& l, uint64_t (&faces)[6][kLocalMips])
{
    const float reach = l.farM + l.radius, h = 0.70710678f;
    float3 right[6], up[6], axis[6];
    for (uint32_t f = 0; f < 6; ++f)
    {
        cubeBasis(f, right[f], up[f], axis[f]);
        for (uint32_t mip = 0; mip < kLocalMips; ++mip) faces[f][mip] = b.everywhere;
    }
    for (size_t i = 0; i < b.spheres.size(); ++i)
    {
        const float4& q = b.spheres[i];
        const float3 d = { q.x - l.position.x, q.y - l.position.y, q.z - l.position.z };
        const float rr = reach + q.w;
        if (dot(d, d) > rr * rr) continue;
        // (under a mip's smallest caster: not drawn into its views; V takes the radius over the distance to the sphere's
        // nearest point, at least its near plane 1e-4)
        bool drawn[kLocalMips];
        const float nearestPoint = std::max(std::sqrt(dot(d, d)) - q.w, 1e-4f);
        for (uint32_t mip = 0; mip < kLocalMips; ++mip) drawn[mip] = !(b.minTexels > 0 && q.w * 0.5f * (float)(kPage << mip) / nearestPoint < b.minTexels);
        for (uint32_t f = 0; f < 6; ++f)
        {
            const float a = dot(d, axis[f]), x = dot(d, right[f]), y = dot(d, up[f]);
            if (a < -q.w || (a - x) * h < -q.w || (a + x) * h < -q.w || (a - y) * h < -q.w || (a + y) * h < -q.w) continue;
            if (!b.lod || b.mesh[i] == gpu::kNone)
            {
                for (uint32_t mip = 0; mip < kLocalMips; ++mip)
                    if (drawn[mip]) faces[f][mip] += b.clusters[i];
                continue;
            }
            const float4& ls = b.lodSpheres[i];
            const float3 e = { ls.x - l.position.x, ls.y - l.position.y, ls.z - l.position.z };
            const float centre = std::sqrt(dot(e, e)), nearest = std::max(centre - ls.w, 1e-4f), farthest = centre + ls.w;
            for (uint32_t mip = 0; mip < kLocalMips; ++mip)
            {
                if (!drawn[mip]) continue;
                const float perMetre = b.thresholdPx / (0.5f * (float)(kPage << mip) * std::max(b.errorScale[i], 1e-12f));
                faces[f][mip] += casterCut(b, i, perMetre * nearest, perMetre * farthest);
            }
        }
    }
}

// Cube face bases (VsmLocal.hlsli vsmCubeBasis): right = up x axis.
void cubeBasis(uint32_t face, float3& right, float3& up, float3& axis)
{
    static const float3 axes[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    static const float3 ups[6] = { { 0, 1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 }, { 0, 1, 0 }, { 0, 1, 0 } };
    axis = axes[face];
    up = ups[face];
    right = cross(up, axis);
}

// View-projection of a local face: the face's tangent square [-1, 1]^2 onto the view's res x res viewport (res = 128 x
// 2^mip, the mip's tile grid in V's atlas mode), reversed-Z depth d = n (f - z) / ((f - n) z) (near 1, far 0;
// VsmCommon.hlsli vsmLocalKeyOfDepth inverts it).
float4x4 localViewProj(const VsmLocalLightCpu& l, uint32_t face)
{
    float3 right, up, axis;
    cubeBasis(face, right, up, axis);
    const float n = l.nearM, f = l.farM, A = -n / (f - n), B = f * n / (f - n);
    const float3 rows[4] = { right, up, axis * A, axis };
    float4x4 m;
    for (int i = 0; i < 4; ++i)
    {
        m.m[i][0] = rows[i].x;
        m.m[i][1] = rows[i].y;
        m.m[i][2] = rows[i].z;
        m.m[i][3] = -dot(rows[i], l.position);
    }
    m.m[2][3] += B;
    return m;
}

// Radius of the light's emitter seen from receivers (VsmLocal.hlsli): sphere/disk radius, rect half-diagonal, tube half
// length plus radius; 0 for points and spots.
float emitterRadius(const scene::Light& l)
{
    switch (l.type)
    {
    case scene::LightType::Sphere:
    case scene::LightType::Disk: return l.size.x;
    case scene::LightType::Rect: return 0.5f * std::sqrt(l.size.x * l.size.x + l.size.y * l.size.y);
    case scene::LightType::Tube: return 0.5f * l.size.x + l.size.y;
    default: return 0.0f;
    }
}

// Sphere against the main view's frustum (clip = viewProj x p: -w <= x, y <= w, z <= w for reversed-Z near).
bool sphereInView(const float4x4& vp, float3 c, float radius)
{
    // Planes w + x, w - x, w + y, w - y, w - z (rows of viewProj combined).
    const int rowA[5] = { 3, 3, 3, 3, 3 }, rowB[5] = { 0, 0, 1, 1, 2 };
    const float sign[5] = { 1, -1, 1, -1, -1 };
    for (int i = 0; i < 5; ++i)
    {
        float p[4];
        for (int j = 0; j < 4; ++j) p[j] = vp.m[rowA[i]][j] + sign[i] * vp.m[rowB[i]][j];
        const float len = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]);
        if (p[0] * c.x + p[1] * c.y + p[2] * c.z + p[3] < -radius * len) return false;
    }
    return true;
}
} // namespace

const VsmStats& stats(TrackState& state)
{
    State& s = state.get<State>(kStateKey);
    return s.latest;
}

void setDebugPaths(TrackState& state, bool enabled) { state.get<State>(kStateKey).debugPaths = enabled; }

const VsmConstantsCpu& lastConstants(TrackState& state) { return state.get<State>(kStateKey).constants; }

bool frameRefs(FramePassContext& fc, VsmFrameRefs& out)
{
    State& s = fc.state<State>(kStateKey);
    if (!s.pagesRecorded || s.recordedFrame != fc.frame.frameIndex) return false;
    out.atlas = s.atlasRef;
    out.table = s.tableRef;
    out.blocks = s.blocksRef;
    out.bound = s.boundRef;
    out.constantsCbv = s.ringCbv[s.constantsOffset / kRingStride];
    out.stats = s.statsRef;
    out.use = s.useRef;
    out.clsBlocks = s.clsBlocksRef;
    out.clsActive = s.clsActiveCount;
    return true;
}

namespace
{
// Local lights of this frame: shadow slots (persistent while a light keeps casting; a slot's generation changes when its
// light changes or moves, which releases its pages), the lights meeting the main view (raster views), and the uploads
// (local lights, scene light -> slot, active slots) in this frame's ring slice.
// shading.mega_lights (Passes/Shading/MegaLights.hlsli; owner A): the local lights' shadows are the light samples' rays, so
// S assigns no local slot, draws no local page and its per-pixel passes skip the local lights (the sun's maps stay). The
// same condition as M's (the switch, the R track in the build, a ray scene this frame). The slots' other readers (coverage
// fragments, planar views, particles, the A9 lobe kernel) then light without local shadows until they take samples too.
bool megaLightsOwnLocalShadows(FramePassContext& fc)
{
#if UNX_S_HAS_RAYTRACING
    return fc.quality.has("shading.mega_lights") && fc.quality.boolean("shading.mega_lights") && fc.resources.tlasStatic.valid();
#else
    (void)fc;
    return false;
#endif
}

void updateLocalLights(FramePassContext& fc, State& s, const ViewResources& main)
{
    const bool noSlots = megaLightsOwnLocalShadows(fc);
    const scene::Scene* src = fc.scene.source();
    const std::vector<scene::Light> none;
    const std::vector<scene::Light>& lights = src ? src->lights : none;
    const uint32_t n = (uint32_t)lights.size();
    if (fc.scene.revision() != s.localSceneRevision)
    {
        // A new scene: every slot's light may be another now.
        for (uint32_t i = 0; i < kLocalLights; ++i)
            if (s.localLight[i] != 0) ++s.localGen[i];
        std::fill(std::begin(s.localLight), std::end(s.localLight), 0u);
        s.localSceneRevision = fc.scene.revision();
    }
    s.slotOfLight.assign(n, 0xFFFFu);
    for (uint32_t i = 0; i < kLocalLights; ++i)
    {
        const uint32_t li = s.localLight[i];
        if (li == 0) continue;
        if (!noSlots && li - 1 < n && lights[li - 1].castShadow) s.slotOfLight[li - 1] = i;
        else
        {
            s.localLight[i] = 0;
            ++s.localGen[i];
        }
    }
    // Slots by priority (game request 09-30: past kLocalLights shadowed lights the first-come order left the lights the view
    // needs without shadows). Score: in the main view first (the range sphere meets the frustum or holds the eye), then the
    // light's reach at the eye, intensity x luminance x (range / max(distance, range))^2. Lights without a slot take the
    // free slots best first, then replace the weakest holder when they score 1.25 x more (at most 8 replacements a frame:
    // a replaced slot's pages render anew). The rest (localWithoutSlot) light without shadows until a slot frees.
    struct Priority
    {
        bool inView = false;
        float value = 0;
        bool beats(const Priority& o, float margin) const { return inView != o.inView ? inView : value > margin * o.value; }
    };
    auto priority = [&](uint32_t li) {
        const scene::Light& l = lights[li];
        const float3 eye = main.view.position;
        const float3 d = { l.position.x - eye.x, l.position.y - eye.y, l.position.z - eye.z };
        const float dist2 = dot(d, d), reach = l.range + emitterRadius(l);
        // (the view's fade of a light toward its draw distance - scene::Light::maxDrawDistance, Scene.hlsli
        // lightViewFade: a light faded out lights nothing and waits behind every other)
        float fade = 1.0f;
        if (l.maxDrawDistance > 0)
        {
            const float dist = std::sqrt(dist2);
            fade = l.maxDistanceFadeRange > 0 ? std::min(std::max((l.maxDrawDistance - dist) / l.maxDistanceFadeRange, 0.0f), 1.0f) : (dist < l.maxDrawDistance ? 1.0f : 0.0f);
        }
        Priority p;
        p.inView = fade > 0 && (dist2 <= reach * reach || sphereInView(main.view.viewProj, l.position, reach));
        const float lum = std::max(0.2126f * l.color.x + 0.7152f * l.color.y + 0.0722f * l.color.z, 1e-6f);
        p.value = std::max(l.intensity, 0.0f) * lum * fade * reach * reach / std::max(std::max(dist2, reach * reach), 1e-6f);
        return p;
    };
    std::vector<std::pair<Priority, uint32_t>> waiting;  // lights without a slot, best first
    for (uint32_t li = 0; li < n; ++li)
        if (!noSlots && lights[li].castShadow && s.slotOfLight[li] == 0xFFFFu) waiting.push_back({ priority(li), li });
    std::sort(waiting.begin(), waiting.end(), [](const auto& a, const auto& b) { return a.first.beats(b.first, 1.0f) || (!b.first.beats(a.first, 1.0f) && a.second < b.second); });
    uint32_t freeSlot = 0, replaced = 0;
    for (const auto& [pr, li] : waiting)
    {
        while (freeSlot < kLocalLights && s.localLight[freeSlot] != 0) ++freeSlot;
        uint32_t slot = freeSlot;
        if (slot == kLocalLights && replaced < 8)
        {
            // the weakest holder, replaced when this light is clearly more important (1.25 x, or in view against out of view)
            Priority weakest;
            weakest.inView = true;
            weakest.value = 3.0e38f;
            for (uint32_t i = 0; i < kLocalLights; ++i)
            {
                const Priority h = priority(s.localLight[i] - 1);
                if (weakest.beats(h, 1.0f))
                {
                    weakest = h;
                    slot = i;
                }
            }
            if (slot == kLocalLights || !pr.beats(weakest, 1.25f)) slot = kLocalLights;
            else
            {
                s.slotOfLight[s.localLight[slot] - 1] = 0xFFFFu;
                ++replaced;
            }
        }
        // No free slot and no replacement (this light does not beat the weakest holder by 1.25 x, or the frame's 8 are
        // used): the waiting lights after it rank no higher and the weakest holder is unchanged, so none can take a slot.
        if (slot == kLocalLights) break;
        s.localLight[slot] = li + 1;
        ++s.localGen[slot];
        s.slotOfLight[li] = slot;
    }
    s.localWithoutSlot = 0;
    for (uint32_t li = 0; li < n; ++li)
        if (lights[li].castShadow && s.slotOfLight[li] == 0xFFFFu) ++s.localWithoutSlot;
    s.localUsed = 0;
    s.localActive.clear();
    for (uint32_t i = 0; i < kLocalLights; ++i)
    {
        VsmLocalLightCpu& d = s.localData[i];
        d = {};
        if (s.localLight[i] == 0) continue;
        const scene::Light& l = lights[s.localLight[i] - 1];
        const float radius = emitterRadius(l);
        const float sig[6] = { l.position.x, l.position.y, l.position.z, l.range, radius, (float)l.type };
        if (std::memcmp(sig, s.localSig[i], sizeof sig) != 0)
        {
            ++s.localGen[i];  // moved or reshaped: its pages are released and rendered anew
            std::memcpy(s.localSig[i], sig, sizeof sig);
        }
        d.position = l.position;
        d.radius = radius;
        d.nearM = std::max(0.05f, radius);
        d.farM = l.range + radius;
        d.lightIndex = s.localLight[i] - 1;
        d.generation = s.localGen[i];
        d.active = 1;
        d.activeIndex = 0xFFFFFFFFu;
        s.localUsed = i + 1;
        const float3 eye = main.view.position;
        const float3 toLight = { d.position.x - eye.x, d.position.y - eye.y, d.position.z - eye.z };
        if (dot(toLight, toLight) <= d.farM * d.farM || sphereInView(main.view.viewProj, d.position, d.farM))
        {
            d.activeIndex = (uint32_t)s.localActive.size();
            s.localActive.push_back(i);
        }
    }
    s.latest.localAssigned = 0;
    for (uint32_t i = 0; i < kLocalLights; ++i) s.latest.localAssigned += s.localLight[i] != 0 ? 1u : 0u;
    s.latest.localActive = (uint32_t)s.localActive.size();
    s.latest.localWithoutSlot = s.localWithoutSlot;

    // Upload ring: [local lights 128 x 48 B][active slots 128 x 4 B][scene light -> slot, cap x 4 B].
    const uint32_t cap = std::max(n, 1u);
    if (!s.localRing || cap > s.localCap)
    {
        if (s.localRing)
        {
            fc.device.deferRelease(s.localRing);
            DescriptorHeaps* old = &fc.device.descriptors();
            for (uint32_t r = 0; r < kRingSlots; ++r)
                for (uint32_t srv : { s.localLightsSrv[r], s.localActiveSrv[r], s.localSlotOfSrv[r] }) fc.device.deferCall([old, srv] { old->freeResource(srv); });
        }
        s.localCap = std::max(cap, 256u);
        // A multiple of 768 = lcm(48, 256): each slice's structured views start at a whole element (FirstElement = byte
        // offset / stride). A stride of 256 alone put the 48-byte records' view of two slices in three 16 or 32 bytes early
        // once more than 256 scene lights had grown the slot map, and the local lights read garbage there (S_STATUS 9e).
        s.localStride = (kLocalLights * 48 + kLocalLights * 4 + s.localCap * 4 + 767) / 768 * 768;
        s.localRing = createBuffer(fc.device, L"S VSM local lights ring", (uint64_t)kRingSlots * s.localStride, D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RANGE nothing{ 0, 0 };
        check(s.localRing->Map(0, &nothing, reinterpret_cast<void**>(&s.localMapped)), "map VSM local ring");
        DescriptorHeaps& h = fc.device.descriptors();
        for (uint32_t r = 0; r < kRingSlots; ++r)
        {
            auto view = [&](uint64_t offset, uint32_t elements, uint32_t stride) {
                if ((r * (uint64_t)s.localStride + offset) % stride != 0) fail("VSM local ring: slice %u view at byte %llu is not a whole %u-byte element", r,
                                                                               (unsigned long long)(r * (uint64_t)s.localStride + offset), stride);
                const uint32_t index = h.allocateResource();
                D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sd.Format = DXGI_FORMAT_UNKNOWN;
                sd.Buffer.FirstElement = (UINT64)(r * (uint64_t)s.localStride + offset) / stride;
                sd.Buffer.NumElements = elements;
                sd.Buffer.StructureByteStride = stride;
                fc.device.d3d()->CreateShaderResourceView(s.localRing.Get(), &sd, h.resourceCpu(index));
                return index;
            };
            s.localLightsSrv[r] = view(0, kLocalLights, 48);
            s.localActiveSrv[r] = view(kLocalLights * 48, kLocalLights, 4);
            s.localSlotOfSrv[r] = view(kLocalLights * 52, s.localCap, 4);
        }
    }
    const uint32_t r = (uint32_t)(fc.frame.frameIndex % kRingSlots);
    uint8_t* base = s.localMapped + (uint64_t)r * s.localStride;
    std::memcpy(base, s.localData, sizeof s.localData);
    std::vector<uint32_t> active(kLocalLights, 0);
    std::copy(s.localActive.begin(), s.localActive.end(), active.begin());
    std::memcpy(base + kLocalLights * 48, active.data(), kLocalLights * 4);
    std::vector<uint32_t> slotOf(s.localCap, 0xFFFFu);
    std::copy(s.slotOfLight.begin(), s.slotOfLight.end(), slotOf.begin());
    std::memcpy(base + kLocalLights * 52, slotOf.data(), (uint64_t)s.localCap * 4);
    s.localLightsNow = s.localLightsSrv[r];
    s.activeNow = s.localActiveSrv[r];
    s.slotOfNow = s.localSlotOfSrv[r];
}
} // namespace

void recordPages(FramePassContext& fc, const ViewResources& main)
{
    State& s = fc.state<State>(kStateKey);
    const QualityConfig& q = fc.quality;

    // Harvest completed stats (no stall): the newest slot whose frame the GPU has finished.
    const uint64_t completed = fc.device.queue(QueueType::Graphics).completed();
    if (s.lastStatsSlot >= 0) s.statsFence[s.lastStatsSlot] = fc.graph.lastFence(QueueType::Graphics);
    for (uint32_t i = 0; s.statsReadback && i < kStatsSlots; ++i)
    {
        // Only frames at least framesInFlight old (the host has waited for those): the newest read is then always frame -
        // framesInFlight, not whichever frame the GPU happened to finish (the counts size buffers: runs differed).
        if (s.statsFence[i] == 0 || s.statsFence[i] > completed || s.statsFrame[i] <= s.latest.frame ||
            s.statsFrame[i] + fc.framesInFlight > fc.frame.frameIndex)
            continue;
        uint32_t* p = nullptr;
        D3D12_RANGE r{ i * kStatsBytes, i * kStatsBytes + kStatsBytes };
        check(s.statsReadback->Map(0, &r, reinterpret_cast<void**>(&p)), "map VSM stats");
        const uint32_t* w = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(p) + i * kStatsBytes);
        const VsmStats keep = s.latest;
        s.latest = { s.statsFrame[i], w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[8], w[9], w[10], w[11], w[12], w[13], w[14] };
        s.latest.localAssigned = keep.localAssigned;
        s.latest.localActive = keep.localActive;
        s.latest.localWithoutSlot = keep.localWithoutSlot;
        s.latest.overflowCapacity = keep.overflowCapacity;
        s.latest.overflowWords = w[16];
        s.latest.overflowOverTiles = w[17];
        s.latest.overflowOverPixels = w[18];
        s.latest.overflowLights = w[19];
        s.latest.airSlices = w[20];
        s.latest.airSlicesMixed = w[21];
        s.latest.airBlocks32 = w[22];
        s.latest.airBlocks8 = w[23];
        s.latest.airTexels = w[24];
        s.latest.localAirEntries = w[54];
        s.latest.localAirCells = w[55];
        s.latest.localAirMaxCells = w[56];
        s.latest.localAirRuns = w[57];
        s.latest.localAirWaveCells = w[58];
        s.latest.localAirWaveEntries = w[59];
        s.latest.fragmentPixels = w[25];
        s.latest.fragmentPairs = w[26];
        s.latest.fragmentChecked = w[27];
        s.latest.fragmentMismatch = w[28];
        s.latest.fragmentMaxDiff = w[29];
        s.latest.fragmentFirstPixel = w[30];
        s.latest.fragmentFirstValues = w[31];
        for (uint32_t k = 0; k < kLevels; ++k) s.latest.levelPages[k] = w[32 + k];
        s.latest.sampledSubtiles = w[53];
        s.latest.surfacePixels = w[61];
        s.latest.backfacePixels = w[62];
        s.latest.backfaceMixed = w[63];
        s.latest.clsTiles = w[64];
        s.latest.clsPairs = w[65];
        s.latest.clsLitPairs = w[66];
        s.latest.clsUmbraPairs = w[67];
        s.latest.airClsLit = w[68];
        s.latest.airOmitted = w[69];
        s.latest.airWalked = w[70];
        s.latest.staleSpared = w[72];
        s.latest.dynamicPages = w[71];
        s.latest.errorBits = w[15];
        if (w[15] & ~s.errorBitsSeen)
            logf("S VSM: error bits 0x%x (frame %llu): a shader loop reached its hard cap (INTERFACES 3.6; VsmCommon.hlsli VSM_ERR_*)\n", w[15],
                 (unsigned long long)s.statsFrame[i]);
        s.errorBitsSeen |= w[15];
        s.latest.errorBitsSeen = s.errorBitsSeen;
        D3D12_RANGE none{ 0, 0 };
        s.statsReadback->Unmap(0, &none);
    }

    // Atlas size: shadow.vsm.pool_pages_per_mpixel of the main view (at least pool_pages), grown to 1.25 x the requests
    // of a completed frame that exceeded it, so requests are not dropped in steady state (a dropped page is a missing
    // shadow). The frame that exceeds renders without those pages; the frames after the growth have them.
    if (s.latest.exhausted > 0 && s.latest.frame > s.grownAtFrame)
    {
        s.poolTarget = std::max(s.atlasPages + kAtlasPagesPerRow, (uint32_t)((uint64_t)s.latest.requested * 5 / 4));
        s.grownAtFrame = s.frames + 1;  // stats of frames recorded with the old atlas do not count
        logf("S VSM: %u page requests exceeded the %u-page atlas (frame %llu): the atlas grows to %u pages\n", s.latest.exhausted, s.atlasPages,
             (unsigned long long)s.latest.frame, (s.poolTarget + kAtlasPagesPerRow - 1) / kAtlasPagesPerRow * kAtlasPagesPerRow);
    }
    // V's raster runs of the pages: a run whose visible or tile-pair list overflowed dropped casters from the pages it drew,
    // and the page cache would keep them (train lounge, frame 0 -> 60: 104,460 pixels without their sun shadow [measured]).
    // Their statistics come back framesInFlight frames later; then every page is drawn anew - no sun page is kept and every
    // local slot's generation moves - and VSM_ERR_RASTER_OVERFLOW fails the gates (INTERFACES 3.6: a structural bound was
    // exceeded, a defect). The runs' bound keeps them inside their lists; this is the guard behind it.
    s.rasterRedraw = false;
    {
        for (const auto& [name, o] : fc.state<DepthRasterOverflows>(kDepthRasterOverflowKey))
        {
            if (name.rfind("s.vsm.", 0) != 0 || o.frame == UINT64_MAX) continue;
            if (s.rasterOverflowFrame != UINT64_MAX && o.frame <= s.rasterOverflowFrame) continue;
            s.rasterOverflowFrame = std::max(o.frame, s.rasterOverflowFrame == UINT64_MAX ? 0 : s.rasterOverflowFrame);
            s.rasterRedraw = true;
            if (++s.rasterOverflows <= 8)  // (each frame it recurs: a full redraw that overflows again shows here)
                logf("S VSM: V's raster run '%s' overflowed its lists in frame %llu (bits 0x%x): every page is drawn anew (error bit 0x%x)\n", name.c_str(),
                     (unsigned long long)o.frame, o.bits, kErrRasterOverflow);
            s.errorBitsSeen |= kErrRasterOverflow;
            s.latest.errorBits |= kErrRasterOverflow;
            s.latest.errorBitsSeen = s.errorBitsSeen;
        }
        if (s.rasterRedraw)
            for (uint32_t i = 0; i < kLocalLights; ++i)
                if (s.localLight[i] != 0) ++s.localGen[i];  // its pages are released and drawn anew
    }
    // Local lights first: their count sizes the atlas's initial budget with the view's pixels.
    updateLocalLights(fc, s, main);
    const double mpixels = (double)main.view.width * main.view.height / 1e6;
    const double budget = mpixels * q.number("shadow.vsm.pool_pages_per_mpixel") + s.latest.localAssigned * q.number("shadow.vsm.pool_pages_per_local_light");
    uint32_t pages = std::max((uint32_t)q.integer("shadow.vsm.pool_pages"), (uint32_t)budget);
    pages = (std::max(pages, s.poolTarget) + kAtlasPagesPerRow - 1) / kAtlasPagesPerRow * kAtlasPagesPerRow;
    pages = std::min(pages, 16384u / kPage * kAtlasPagesPerRow);  // one 16384^2 atlas (16,384 pages)
    // shadow.vsm.static_separate (with the page cache): a static copy of every sun page beside the sampled atlas; the
    // switch changing recreates the state (the table is reset: its dynamic flags mean nothing to the other path).
    const bool separate = q.has("shadow.vsm.cache") && q.boolean("shadow.vsm.cache") && q.has("shadow.vsm.static_separate") && q.boolean("shadow.vsm.static_separate");
    if (!s.atlas || s.atlasPages < pages || s.separate != separate) createState(fc, s, std::max(pages, s.atlasPages), separate);

    // One path (S request 20260926_S_vsm_one_path): every level on the current sun and this frame's caster height range;
    // every requested page is drawn this frame, so there is no basis age, no stale page and no dirty rule.
    const scene::Scene* src = fc.scene.source();
    const float3 sunDir = normalize(src ? src->sun.direction : scene::Sun{}.direction);
    const float margin = (float)q.number("shadow.vsm.height_margin_m");
    const float tanSun = std::tan(src ? src->sun.angularRadius : scene::Sun{}.angularRadius);
    // shadow.vsm.use_stats (measurement only): the per-slot read bits and their persistent UAV (VsmSample.hlsli vsmEntry).
    const bool useStats = q.integer("shadow.vsm.use_stats") != 0;
    if (useStats && !s.use)
    {
        s.use = createBuffer(fc.device, L"S VSM read bits", (uint64_t)kSlots * 4);
        DescriptorHeaps& h = fc.device.descriptors();
        s.useUav = h.allocateResource();
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
        ud.Format = DXGI_FORMAT_R32_TYPELESS;
        ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = kSlots;
        ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
        fc.device.d3d()->CreateUnorderedAccessView(s.use.Get(), nullptr, &ud, h.resourceCpu(s.useUav));
    }
    VsmConstantsCpu& c = s.constants;
    c = {};
    c.useStats = useStats ? s.useUav + 1 : 0;
    c.atlasSrv = s.atlasSrv;
    c.fragmentCheck = (uint32_t)q.integer("shadow.vsm.fragment_check");
    const float3 windDir = src ? src->windDirection : float3{};
    c.windSpeed = std::max(src ? src->windSpeed : 0.0f, 0.0f);
    c.windDirection = c.windSpeed > 0 ? normalize(windDir) : float3{};
    c.lightZ = sunDir;
    lightBasis(sunDir, c.lightX, c.lightY);
    // Sun page cache (shadow.vsm.cache): the kept pages hold heights mapped with the range they were drawn with, so the
    // range stays while this frame's casters fit in it (a wider range costs float precision of the depth only); when
    // they leave it (or the sun turns) it widens by a quarter on each side and the frame redraws every page.
    const bool cacheOn = q.has("shadow.vsm.cache") && q.boolean("shadow.vsm.cache");
    bool rangeKept = false;
    {
        float lo, hi;
        casterHeightRange(fc.scene, sunDir, lo, hi);
        c.hMin = lo - margin;
        c.hMax = hi + margin;
        const bool sameSun = s.cacheRange && sunDir.x == s.cacheSun.x && sunDir.y == s.cacheSun.y && sunDir.z == s.cacheSun.z;
        if (cacheOn && sameSun && c.hMin >= s.cacheHMin && c.hMax <= s.cacheHMax)
        {
            c.hMin = s.cacheHMin;
            c.hMax = s.cacheHMax;
            rangeKept = true;
        }
        else if (cacheOn)
        {
            const float pad = 0.25f * (c.hMax - c.hMin) + margin;
            c.hMin -= pad;
            c.hMax += pad;
            s.cacheHMin = c.hMin;
            s.cacheHMax = c.hMax;
            s.cacheSun = sunDir;
            s.cacheRange = true;
        }
        else
            s.cacheRange = false;
    }
    c.tanSunRadius = tanSun;
    c.poolPagesX = kAtlasPagesPerRow;
    c.poolPagesY = s.atlasPages / kAtlasPagesPerRow;
    c.frame = (uint32_t)++s.frames;
    c.time = (float)fc.frame.time;
    c.lodBias = (float)q.number("shadow.vsm.lod_bias");
    // (+ the visible surface's own LOD error: a pixel's cut may lie visibility.lod_error_px pixels inside the casters', and
    // a pixel is up to receiver_lod_texels texels of its level - the level's texel is the largest not above the footprint)
    c.receiverBiasTexels = (float)q.number("shadow.vsm.receiver_bias_texels") +
                           (float)(q.number("shadow.vsm.receiver_lod_texels") * q.number("visibility.lod_error_px"));
    c.maxReceiverSlope = (float)q.number("shadow.vsm.max_receiver_slope");
    c.instanceCount = (uint32_t)fc.scene.instances().size();
    const int64_t searchTaps = q.integer("shadow.vsm.search_taps"), filterTaps = q.integer("shadow.vsm.filter_taps");
    // Structural per-item bound for immediate and queued visibility. Preserve
    // every configured tap; reject unsupported counts rather than clamping.
    if (searchTaps < 1 || searchTaps > 64 || filterTaps < 1 || filterTaps > 64)
        fail("shadow.vsm search_taps and filter_taps must be in [1, 64]");
    c.searchTaps = (uint32_t)searchTaps;
    c.filterTaps = (uint32_t)filterTaps;
    const float3 cam = main.view.position;
    c.cameraUV[0] = dot(cam, c.lightX);
    c.cameraUV[1] = dot(cam, c.lightY);
    for (uint32_t k = 0; k < kLevels; ++k)
    {
        VsmLevelCpu& L = c.level[k];
        L.lightX = c.lightX;
        L.lightY = c.lightY;
        L.lightZ = c.lightZ;
        L.basis = 1;  // one basis for every level (nested grids)
        L.hMin = c.hMin;
        L.hMax = c.hMax;
        L.cameraU = c.cameraUV[0];
        L.cameraV = c.cameraUV[1];
        const float pageSize = std::ldexp(1.0f, (int)k - 10) * kPage;
        L.origin[0] = (int32_t)std::floor(L.cameraU / pageSize) - (int32_t)kTable / 2;
        L.origin[1] = (int32_t)std::floor(L.cameraV / pageSize) - (int32_t)kTable / 2;
    }
    s.constantsOffset = (uint32_t)(fc.frame.frameIndex % kRingSlots) * kRingStride;
    std::memcpy(s.ringMapped + s.constantsOffset, &c, sizeof c);
    s.initialized = true;

    RenderGraph& g = fc.graph;
    // shadow.vsm.fold_small_passes: the page update's bookkeeping dispatches share passes (PassChain.h): the counters'
    // clear with the cache's reset (s.vsm.begin), and the request propagation, the cache's keep and free list and the
    // three scan steps (s.vsm.scan) - each a group or a few, all writing the same buffers as UAVs, each reading what the
    // one before wrote.
    const bool foldSmall = !q.has("shadow.vsm.fold_small_passes") || q.boolean("shadow.vsm.fold_small_passes");
    PassChain chain(g, QueueType::Compute, foldSmall);
    const TextureRef atlas = g.importTexture(s.atlas.Get(),
                                             TextureDesc{ "S VSM atlas", kAtlasPagesPerRow * kPage, s.atlasPages / kAtlasPagesPerRow * kPage, 1, 1, DXGI_FORMAT_D32_FLOAT },
                                             D3D12_BARRIER_LAYOUT_SHADER_RESOURCE);
    const BufferRef table = g.importBuffer(s.table.Get(), BufferDesc{ "S VSM page table", (uint64_t)kTotalSlots * 8, 0 });
    const BufferRef requests = g.importBuffer(s.requests.Get(), BufferDesc{ "S VSM requests", (uint64_t)kTotalSlots * 4, 0 });
    const BufferRef localMask = g.importBuffer(s.localMask.Get(), BufferDesc{ "S VSM local cull mask", (uint64_t)kLocalLights * kLocalLightWords * 4, 0 });
    const BufferRef localSlots = g.importBuffer(s.localSlots.Get(), BufferDesc{ "S VSM local atlas slots", (uint64_t)kLocalLights * kLocalLightWords * 32 * 4, 0 });
    // Slots the scan covers: the sun's and those of the assigned local lights (and last frame's, so a shrinking set of
    // local lights clears its old slots).
    const uint32_t slotsUsed = kSlots + s.localUsed * kLocalLightSlots;
    const uint32_t scanSlots = std::max(slotsUsed, s.prevSlotsUsed);
    s.prevSlotsUsed = slotsUsed;
    const uint32_t scanGroups = groups(scanSlots, kScanGroupSlots);
    const uint32_t localLightsSrv = s.localLightsNow, slotOfSrv = s.slotOfNow, activeSrv = s.activeNow;
    const uint32_t activeLocal = (uint32_t)s.localActive.size();
    // Froxel light lists (with each entry's shadow-slot bit): the local page marks and the visibility slots read them.
    recordFroxelLists(fc, main, slotOfSrv, s.localUsed > 0);
    const BufferRef froxelLists = fc.resources.froxelLights;
    const uint32_t pagesNow = s.atlasPages;
    const BufferRef meta = g.importBuffer(s.meta.Get(), BufferDesc{ "S VSM page metadata", (uint64_t)pagesNow * kMetaBytes, kMetaBytes });
    const BufferRef blocks = g.importBuffer(s.blocks.Get(), BufferDesc{ "S VSM page blocks", (uint64_t)pagesNow * kBlockBytes, 0 });
    s.blocksRef = blocks;
    const BufferRef pageList = g.importBuffer(s.pageList.Get(), BufferDesc{ "S VSM page list", 8 + (uint64_t)pagesNow * 8, 0 });
    const BufferRef layers = g.importBuffer(s.layers.Get(), BufferDesc{ "S VSM transmittance layer", s.layersBytes, 0 });
    s.layersRef = layers;
    fc.resources.vsmLayers = layers;  // v1.26: ShadowSrvs.layers (the pad1 word)
    const BufferRef mask = g.importBuffer(s.cullMask.Get(), BufferDesc{ "S VSM cull mask", (uint64_t)kSlots / 8 * 2, 0 });
    const BufferRef atlasSlots = g.importBuffer(s.atlasSlots.Get(), BufferDesc{ "S VSM atlas slots", (uint64_t)kSlots * 4 * 2, 0 });
    const TextureRef atlasStatic = separate ? g.importTexture(s.atlasStatic.Get(),
                                                              TextureDesc{ "S VSM static atlas", kAtlasPagesPerRow * kPage, s.atlasPages / kAtlasPagesPerRow * kPage, 1, 1,
                                                                           DXGI_FORMAT_D32_FLOAT },
                                                              D3D12_BARRIER_LAYOUT_SHADER_RESOURCE)
                                            : TextureRef{};
    const BufferRef dynamicList = g.importBuffer(s.dynamicList.Get(), BufferDesc{ "S VSM dynamic page list", 8 + (uint64_t)pagesNow * 8, 0 });
    // shadow.vsm.static_hzb_cull (with static_separate): the movable casters' views are culled against the static copies' HZB
    const bool hzbCull = separate && (!q.has("shadow.vsm.static_hzb_cull") || q.boolean("shadow.vsm.static_hzb_cull"));
    const BufferRef staticHzb = hzbCull ? g.importBuffer(s.staticHzb.Get(), BufferDesc{ "S VSM static HZB", (uint64_t)pagesNow * kBlockEntries * 4, 0 }) : BufferRef{};
    const uint32_t staticHzbSrv = s.staticHzbSrv, atlasSlotsSrv = s.atlasSlotsSrv;
    // (the cull switched on over kept pages: their static copies were drawn without an HZB - this frame draws every page anew)
    const bool hzbFresh = hzbCull && !s.hzbValid;
    s.hzbValid = hzbCull;
    // shadow.vsm.static_occlusion_two_phase (with static_hzb_cull): the static casters' views are culled in two phases -
    // against a kept coarser page's HZB first, then against the HZB of what that phase drew (V's two-phase tile occluders)
    const bool twoPhase = hzbCull && (!q.has("shadow.vsm.static_occlusion_two_phase") || q.boolean("shadow.vsm.static_occlusion_two_phase"));
    const BufferRef occluderGuess = twoPhase ? g.importBuffer(s.occluderGuess.Get(), BufferDesc{ "S VSM occluder guess", (uint64_t)kSlots * 8, 0 }) : BufferRef{};
    const uint32_t occluderGuessSrv = s.occluderGuessSrv;
    const BufferRef scanGroupsBuf = g.importBuffer(s.groups.Get(), BufferDesc{ "S VSM scan groups", (uint64_t)kScanGroupsMax * 4, 0 });
    const BufferRef args = g.importBuffer(s.args.Get(), BufferDesc{ "S VSM indirect args", 32, 0 });
    const BufferRef statsBuf = g.importBuffer(s.stats.Get(), BufferDesc{ "S VSM stats", kStatsBytes, 0 });
    s.statsRef = statsBuf;
    s.useRef = c.useStats ? g.importBuffer(s.use.Get(), BufferDesc{ "S VSM read bits", (uint64_t)kSlots * 4, 0 }) : BufferRef{};
    s.recordedFrame = fc.frame.frameIndex;
    s.atlasRef = atlas;
    s.tableRef = table;
    s.metaRef = meta;
    s.pagesRecorded = true;
    // FrameResources (v1.18, v1.43): other tracks read the VSM through ShadowVisibility.hlsli (ShadowSrvs); the lookups
    // take the atlas SRV from the VSM constants. vsmPool stays valid (the blocks buffer, a read-only S buffer) until the
    // readers declare vsmAtlas (their ShadowSrvs.pool word is no longer read).
    fc.resources.vsmAtlas = atlas;
    fc.resources.vsmPool = blocks;
    fc.resources.vsmBlocks = blocks;
    fc.resources.vsmConstants = s.ringCbv[s.constantsOffset / kRingStride];
    fc.resources.vsmLocalLights = s.localLightsNow;  // v1.19: ShadowSrvs.lights / .pad0 (shadowVisibilityDirect)
    fc.resources.vsmSlotOfLight = s.slotOfNow;
    fc.resources.vsmPageTable = table;

    ShaderLibrary& sh = fc.shaders;
    const uint32_t ring = s.ringCbv[s.constantsOffset / kRingStride], off = 0;
    const D3D12_GPU_VIRTUAL_ADDRESS mainConstants = main.frameConstants;

    // Sun page cache: this frame keeps last frame's pages where no changed caster touched them only when nothing global
    // changed - consecutive recorded frames, the same sun and caster height range, scene revision and atlas (a new atlas:
    // needsInit), no origin rebase and no restore (VsmCache.hlsl). Otherwise every requested page is drawn (the one path).
    const uint32_t instanceCount = (uint32_t)fc.scene.instances().size();
    bool stateFresh = false;
    if (cacheOn && (!s.casterState || s.casterStateCount < std::max(instanceCount, 1u)))
    {
        if (s.casterState) fc.device.deferRelease(s.casterState);
        s.casterStateCount = std::max(instanceCount, 1u) + 256;
        s.casterState = createBuffer(fc.device, L"S VSM caster state", (uint64_t)s.casterStateCount * 16);
        stateFresh = true;
    }
    const bool originShift = fc.frame.originShift.x != 0 || fc.frame.originShift.y != 0 || fc.frame.originShift.z != 0;
    // (the wind casters' spheres are bounded with this frame's wind: a changed scene wind redraws every page)
    const bool sameWind = s.cacheWind[0] == c.windSpeed && s.cacheWind[1] == c.windDirection.x && s.cacheWind[2] == c.windDirection.y && s.cacheWind[3] == c.windDirection.z;
    const bool cacheable = cacheOn && rangeKept && sameWind && !stateFresh && !s.needsInit && !s.rasterRedraw && !hzbFresh && s.cacheFrame != UINT64_MAX &&
                           s.cacheFrame + 1 == fc.frame.frameIndex && s.cacheRevision == fc.scene.revision() && !originShift &&
                           (fc.frame.discontinuity & kDiscontinuityRestore) == 0;
    s.cacheFrame = fc.frame.frameIndex;
    s.cacheRevision = fc.scene.revision();
    s.cacheWind[0] = c.windSpeed;
    s.cacheWind[1] = c.windDirection.x;
    s.cacheWind[2] = c.windDirection.y;
    s.cacheWind[3] = c.windDirection.z;

    if (s.needsInit)
    {
        ID3D12PipelineState* pso = sh.compute("Passes/Shadow/VsmInit");
        g.addPass("s.vsm.init", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(table, Use::UavCompute);
                      b.use(requests, Use::UavCompute);
                      b.use(meta, Use::UavCompute);
                      b.use(layers, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[8] = { ctx.uav(table), ctx.uav(requests), ctx.uav(meta), 0, kTotalSlots, pagesNow, ctx.uav(layers), 0 };
                      ctx.cmd->SetPipelineState(pso);
                      ctx.computeConstants(k, 8);
                      ctx.cmd->Dispatch(groups(std::max(kTotalSlots, pagesNow), 256), 1, 1);
                  });
        s.needsInit = false;
    }
    if (c.frame > 1)
    {
        // Counters of the previous frame (page updates and every visibility pass) to the readback ring, before VsmBegin
        // clears them; read at a later record once the GPU passed this frame.
        const uint32_t slot = (uint32_t)(c.frame % kStatsSlots);
        ID3D12Resource* rb = s.statsReadback.Get();
        g.addPass("s.vsm.stats", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(statsBuf, Use::CopySrc);
                      b.keep();
                  },
                  [=](PassContext& ctx) { ctx.cmd->CopyBufferRegion(rb, slot * (uint64_t)kStatsBytes, ctx.resource(statsBuf), 0, kStatsBytes); });
        s.statsFrame[slot] = c.frame - 1;
        s.statsFence[slot] = 0;
        s.lastStatsSlot = (int)slot;
    }
    {
        ID3D12PipelineState* pso = sh.compute("Passes/Shadow/VsmBegin");
        chain.add("s.vsm.begin",
                  [&](PassBuilder& b) {
                      b.use(statsBuf, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[4] = { 0, ctx.uav(statsBuf), 0, 0 };
                      ctx.cmd->SetPipelineState(pso);
                      ctx.computeConstants(k, 4);
                      ctx.cmd->Dispatch(1, 1, 1);
                  });
    }
    // Sun page cache (VsmCache.hlsl): the changed casters' spheres (skinned first: MODE 1 reads last frame's caster state
    // before MODE 0 rewrites it), then the resident pages under them become stale. The used-page bitmap and the free list
    // are built after the marks (MODE 3, MODE 4).
    const uint32_t skinCount = cacheOn && fc.resources.skinBounds.valid() ? fc.resources.skinCount : 0;
    const uint32_t changedCapacity = 2 * (instanceCount + skinCount) + 64;
    const BufferRef changed = g.createBuffer(BufferDesc{ "S VSM changed casters", 16 + (uint64_t)changedCapacity * 16, 0 });
    // (the page bitmap, then the local lights' changed bits: VsmCache.hlsl localChangedOffset)
    const BufferRef usedPages = g.createBuffer(BufferDesc{ "S VSM used pages", (uint64_t)(pagesNow + 31) / 32 * 4 + kLocalLights / 32 * 4, 0 });
    const BufferRef freePages = g.createBuffer(BufferDesc{ "S VSM free pages", 16 + (uint64_t)pagesNow * 4, 0 });
    const BufferRef casterStateRef = cacheOn ? g.importBuffer(s.casterState.Get(), BufferDesc{ "S VSM caster state", (uint64_t)s.casterStateCount * 16, 16 }) : BufferRef{};
    const BufferRef skinBounds = fc.resources.skinBounds;
    const uint32_t skinInstancesSrv = fc.resources.skinInstancesSrv;
    // (the kernels' root constants: VsmCache.hlsl)
    // Local lights (MODE 3, 6): their data this frame and the shadow slots in use; a light's pages are kept while its
    // generation holds and no changed caster meets its range.
    const uint32_t localSlotsUsed = s.localUsed;
    // what a change is worth at a level (VsmCache.hlsl): the least change in the level's texels, and how far the wind
    // moves a point in this frame's time as a share of its bound (Deformation.hlsli windChangeFactor)
    const float minChange = q.has("shadow.vsm.cache_min_change_texels") ? (float)q.number("shadow.vsm.cache_min_change_texels") : 0.0f;
    const float frameSeconds = fc.frame.deltaTime > 0 ? fc.frame.deltaTime : 1.0f / 60.0f;
    const float windChange = 0.4f * std::min(2.0f, 1.7f * frameSeconds);
    uint32_t minChangeBits, windChangeBits;
    std::memcpy(&minChangeBits, &minChange, 4);
    std::memcpy(&windChangeBits, &windChange, 4);
    const uint64_t frameIndex = fc.frame.frameIndex;
    const uint32_t staticInstances = fc.scene.staticInstanceCount();  // (static_separate: the instances from there on are movable)
    auto cacheWords = [=](PassContext& ctx, uint32_t (&k)[kCacheWords]) {
        const uint32_t w[kCacheWords] = { casterStateRef.valid() ? ctx.uav(casterStateRef) : 0xFFFFFFFFu, ctx.uav(changed), instanceCount, ring,
                                          changedCapacity, ctx.uav(table), ctx.uav(requests), ctx.uav(usedPages),
                                          ctx.uav(freePages), pagesNow, scanSlots, cacheable ? 1u : 0u,
                                          skinCount ? ctx.srv(skinBounds) : 0xFFFFFFFFu, skinInstancesSrv, skinCount, ctx.uav(statsBuf),
                                          localSlotsUsed ? localLightsSrv : 0xFFFFFFFFu, localSlotsUsed, 0xFFFFFFFFu, separate ? 1u : 0u,
                                          minChangeBits, windChangeBits, (uint32_t)frameIndex, staticInstances };
        std::memcpy(k, w, sizeof w);
    };
    {
        ID3D12PipelineState* pso = sh.compute("Passes/Shadow/VsmCache.MODE5");
        chain.add("s.vsm.cache.reset",
                  [&](PassBuilder& b) {
                      b.use(changed, Use::UavCompute);
                      b.use(usedPages, Use::UavCompute);
                  },
                  [=](PassContext& ctx) {
                      uint32_t k[kCacheWords];
                      cacheWords(ctx, k);
                      ctx.cmd->SetPipelineState(pso);
                      ctx.computeConstants(k, kCacheWords);
                      ctx.cmd->Dispatch(1, 1, 1);
                  });
    }
    chain.flush("s.vsm.begin");
    if (cacheOn)
    {
        if (skinCount > 0)
        {
            ID3D12PipelineState* pso = sh.compute("Passes/Shadow/VsmCache.MODE1");
            g.addPass("s.vsm.cache.skinned", QueueType::Compute,
                      [&](PassBuilder& b) {
                          b.use(casterStateRef, Use::UavCompute);
                          b.use(changed, Use::UavCompute);
                          b.use(skinBounds, Use::SrvCompute);
                      },
                      [=](PassContext& ctx) {
                          uint32_t k[kCacheWords];
                          cacheWords(ctx, k);
                          ctx.cmd->SetPipelineState(pso);
                          ctx.bindFrameConstants(mainConstants);
                          ctx.computeConstants(k, kCacheWords);
                          ctx.cmd->Dispatch(groups(skinCount, 64), 1, 1);
                      });
        }
        {
            ID3D12PipelineState* pso = sh.compute("Passes/Shadow/VsmCache.MODE0");
            g.addPass("s.vsm.cache.casters", QueueType::Compute,
                      [&](PassBuilder& b) {
                          b.use(casterStateRef, Use::UavCompute);
                          b.use(changed, Use::UavCompute);
                          b.keep();  // (the caster state carries over to the next frame)
                      },
                      [=](PassContext& ctx) {
                          uint32_t k[kCacheWords];
                          cacheWords(ctx, k);
                          ctx.cmd->SetPipelineState(pso);
                          ctx.bindFrameConstants(mainConstants);
                          ctx.computeConstants(k, kCacheWords);
                          ctx.cmd->Dispatch(groups(std::max(instanceCount, 1u), 64), 1, 1);
                      });
        }
        if (localSlotsUsed > 0)
        {
            // the local lights whose range meets a changed caster (their pages are drawn anew this frame)
            ID3D12PipelineState* pso = sh.compute("Passes/Shadow/VsmCache.MODE6");
            g.addPass("s.vsm.cache.locallights", QueueType::Compute,
                      [&](PassBuilder& b) {
                          b.use(changed, Use::UavCompute);
                          b.use(usedPages, Use::UavCompute);
                      },
                      [=](PassContext& ctx) {
                          uint32_t k[kCacheWords];
                          cacheWords(ctx, k);
                          ctx.cmd->SetPipelineState(pso);
                          ctx.computeConstants(k, kCacheWords);
                          ctx.cmd->Dispatch(groups(localSlotsUsed, 64), 1, 1);
                      });
        }
        {
            ID3D12PipelineState* pso = sh.compute("Passes/Shadow/VsmCache.MODE2");
            // shadow.vsm.cache_hzb_filter: a changed caster under a page's stored surface leaves the page as it is
            // (VsmCache.hlsl sphereUnderPage; the blocks are last frame's, of the pages as they were last drawn)
            const bool hzbFilter = !q.has("shadow.vsm.cache_hzb_filter") || q.boolean("shadow.vsm.cache_hzb_filter");
            g.addPass("s.vsm.cache.stale", QueueType::Compute,
                      [&](PassBuilder& b) {
                          b.use(changed, Use::UavCompute);
                          b.use(table, Use::UavCompute);
                          if (hzbFilter)
                          {
                              b.use(blocks, Use::SrvCompute);
                              b.use(statsBuf, Use::UavCompute);
                          }
                      },
                      [=](PassContext& ctx) {
                          uint32_t k[kCacheWords];
                          cacheWords(ctx, k);
                          if (hzbFilter) k[18] = ctx.srv(blocks);
                          ctx.cmd->SetPipelineState(pso);
                          ctx.computeConstants(k, kCacheWords);
                          ctx.cmd->Dispatch(kLevels, 32, 1);  // VsmCache.hlsl STALE_GROUPS_PER_LEVEL
                      });
        }
    }
    if (main.depth.valid())
    {
        ID3D12PipelineState* pso = sh.compute("Passes/Shadow/VsmMark");
        const bool subtileStats = q.integer("shadow.vsm.subtile_stats") != 0;  // measurement only
        // shadow.vsm.page_dilation: pixels near a page border also request the page across it (VsmMark.hlsl)
        const float dilation = q.has("shadow.vsm.page_dilation") ? (float)q.number("shadow.vsm.page_dilation") : 0.0f;
        if (!(dilation >= 0 && dilation <= 0.5f)) fail("shadow.vsm.page_dilation = %g: a fraction of a page in [0, 0.5]", dilation);
        uint32_t dilationBits;
        std::memcpy(&dilationBits, &dilation, 4);
        const TextureRef depth = main.depth;
        const uint32_t w = main.view.width, h = main.view.height;
        g.addPass("s.vsm.mark", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(depth, Use::SrvCompute);
                      b.use(requests, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[8] = { ctx.srv(depth), ctx.uav(requests), ring, subtileStats ? 1u : 0u, dilationBits, 0, 0, 0 };
                      ctx.cmd->SetPipelineState(pso);
                      ctx.bindFrameConstants(mainConstants);
                      ctx.computeConstants(k, 8);
                      ctx.cmd->Dispatch(groups(w, 8), groups(h, 8), 1);
                  });
    }
    {
        // Air of the froxel integration (VsmMarkAir, VsmAir.hlsli): after the pixel marks, which store plainly.
        ID3D12PipelineState* pso = sh.compute("Passes/Shadow/VsmMarkAir");
        const FroxelGridCpu grid = froxelGridFor(q, main.view.width, main.view.height);
        uint32_t nearBits, farBits, texelBits;
        std::memcpy(&nearBits, &grid.nearM, 4);
        std::memcpy(&farBits, &grid.farM, 4);
        std::memcpy(&texelBits, &grid.shadowTexelsPerTile, 4);
        const bool fogOn = false;  // (the fog has its own volume and page requests: s.vsm.markfog below)
        g.addPass("s.vsm.markair", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(requests, Use::UavCompute);
                      b.use(statsBuf, Use::UavCompute);  // error word (INTERFACES 3.6)
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[12] = { ctx.uav(requests), ring, grid.gridX | grid.gridY << 16, grid.slices | grid.tilePx << 16, nearBits, farBits, texelBits,
                                               ctx.uav(statsBuf), fogOn ? 1u : 0u, 0, 0, 0 };  // P[2].x: the fog needs the near slices' pages
                      ctx.cmd->SetPipelineState(pso);
                      ctx.bindFrameConstants(mainConstants);
                      ctx.computeConstants(k, 12);
                      ctx.cmd->Dispatch(grid.gridX, grid.gridY, 1);
                  });
        // The fog's volume (FogVolume.hlsli): the pages its cells' segments cross (VsmMarkFog.hlsl).
        const FogView fog = fogViewFor(q, fc.frame, main.view.width, main.view.height);
        if (fog.on && fog.cells)  // (a volume that holds the cloud layer alone has no cells to shadow)
        {
            ID3D12PipelineState* pf = sh.compute("Passes/Shadow/VsmMarkFog");
            const TextureRef hiz = main.hiz, fogDepth = main.depth;
            uint32_t fb[4];
            std::memcpy(&fb[0], &fog.farM, 4);
            std::memcpy(&fb[1], &fog.k, 4);
            std::memcpy(&fb[2], &fog.b, 4);
            std::memcpy(&fb[3], &fog.shadowTexelsPerCell, 4);
            g.addPass("s.vsm.markfog", QueueType::Compute,
                      [&](PassBuilder& b) {
                          if (hiz.valid()) b.use(hiz, Use::SrvCompute);
                          if (fogDepth.valid()) b.use(fogDepth, Use::SrvCompute);
                          b.use(requests, Use::UavCompute);
                          b.use(statsBuf, Use::UavCompute);
                          b.keep();
                      },
                      [=](PassContext& ctx) {
                          const uint32_t k[12] = { ctx.uav(requests), ring, fog.gridX | fog.gridY << 16, fog.gridZ | fog.cellPx << 16, fb[0], fb[1], fb[2], fb[3],
                                                   hiz.valid() ? ctx.srv(hiz) : 0xFFFFFFFFu, ctx.uav(statsBuf), fogDepth.valid() ? ctx.srv(fogDepth) : 0xFFFFFFFFu, 0 };
                          ctx.cmd->SetPipelineState(pf);
                          ctx.bindFrameConstants(mainConstants);
                          ctx.computeConstants(k, 12);
                          ctx.cmd->Dispatch((fog.gridX + 3) / 4, (fog.gridY + 3) / 4, (fog.gridZ + 3) / 4);
                      });
        }
    }
    if (main.coverageDepthRange.valid())
    {
        // The coverage layer's fragments (VsmMarkFragments): the pages their segments cross on their records' levels.
        ID3D12PipelineState* pso = sh.compute("Passes/Shadow/VsmMarkFragments");
        const TextureRef ranges = main.coverageDepthRange;
        const uint32_t w = main.view.width, h = main.view.height;
        g.addPass("s.vsm.markfragments", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(ranges, Use::SrvCompute);
                      b.use(requests, Use::UavCompute);
                      b.use(statsBuf, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[4] = { ctx.srv(ranges), ctx.uav(requests), ring, ctx.uav(statsBuf) };
                      ctx.cmd->SetPipelineState(pso);
                      ctx.bindFrameConstants(mainConstants);
                      ctx.computeConstants(k, 4);
                      ctx.cmd->Dispatch(groups(w, 8), groups(h, 8), 1);
                  });
    }
    if (s.localUsed > 0 && main.depth.valid())
    {
        // Local-light page requests (VsmLocalMark) through the froxel lists.
        ID3D12PipelineState* pso = sh.compute("Passes/Shadow/VsmLocalMark");
        const TextureRef depth = main.depth;
        const uint32_t w = main.view.width, h = main.view.height;
        g.addPass("s.vsm.localmark", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(depth, Use::SrvCompute);
                      b.use(froxelLists, Use::SrvCompute);
                      b.use(requests, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t lists = ctx.srv(froxelLists);
                      const uint32_t k[8] = { ctx.srv(depth), ctx.uav(requests), localLightsSrv, slotOfSrv, lists, lists, 0, 0 };
                      ctx.cmd->SetPipelineState(pso);
                      ctx.bindFrameConstants(mainConstants);
                      ctx.computeConstants(k, 8);
                      ctx.cmd->Dispatch(groups(w, 8), groups(h, 8), 1);
                  });
    }
    if (s.localUsed > 0)
    {
        // The air the froxel integration shadows with the local lights (VsmLocalMarkAir).
        ID3D12PipelineState* pso = sh.compute("Passes/Shadow/VsmLocalMarkAir");
        const FroxelGridCpu grid = froxelGridFor(q, main.view.width, main.view.height);
        uint32_t texelBits;
        std::memcpy(&texelBits, &grid.shadowTexelsPerTile, 4);
        g.addPass("s.vsm.localmarkair", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(froxelLists, Use::SrvCompute);
                      b.use(requests, Use::UavCompute);
                      b.use(statsBuf, Use::UavCompute);  // error word (INTERFACES 3.6)
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[8] = { ctx.uav(requests), ctx.srv(froxelLists), localLightsSrv, slotOfSrv, texelBits, ctx.uav(statsBuf), 0, 0 };
                      ctx.cmd->SetPipelineState(pso);
                      ctx.bindFrameConstants(mainConstants);
                      ctx.computeConstants(k, 8);
                      ctx.cmd->Dispatch(grid.gridX, grid.gridY, 1);
                  });
    }
    // shadow.vsm.coarse_pages: the pages around the camera on the coarse levels are requested every frame (VsmMarkCoarse):
    // a lookup without a page on its own level finds one of them.
    const uint32_t coarsePages = q.has("shadow.vsm.coarse_pages") ? (uint32_t)q.integer("shadow.vsm.coarse_pages") : 0u;
    if (coarsePages > 0)
    {
        const int64_t first = q.integer("shadow.vsm.coarse_level_first"), last = q.integer("shadow.vsm.coarse_level_last");
        if (first < 0 || last < first || last >= kLevels || coarsePages > 8)
            fail("shadow.vsm coarse pages: levels %lld .. %lld of 0 .. %u, %u pages per axis (at most 8)", (long long)first, (long long)last, kLevels - 1, coarsePages);
        ID3D12PipelineState* pso = sh.compute("Passes/Shadow/VsmMarkCoarse");
        const uint32_t word = (uint32_t)first | (uint32_t)last << 8 | coarsePages << 16;
        const uint32_t threads = (uint32_t)(last - first + 1) * coarsePages * coarsePages;
        chain.add("s.vsm.markcoarse",
                  [&](PassBuilder& b) {
                      b.use(requests, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[4] = { ctx.uav(requests), ring, word, 0 };
                      ctx.cmd->SetPipelineState(pso);
                      ctx.computeConstants(k, 4);
                      ctx.cmd->Dispatch(groups(threads, 64), 1, 1);
                  });
    }
    {
        ID3D12PipelineState* pso = sh.compute("Passes/Shadow/VsmPropagate");
        chain.add("s.vsm.propagate",
                  [&](PassBuilder& b) {
                      b.use(requests, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[4] = { ctx.uav(requests), ring, off, 0 };
                      ctx.cmd->SetPipelineState(pso);
                      ctx.computeConstants(k, 4);
                      ctx.cmd->Dispatch(groups(kSlots, 256), 1, 1);
                  });
    }
    {
        // Sun page cache: the requests that keep their page (cacheable frames), then the free physical pages in order.
        ID3D12PipelineState* keep = sh.compute("Passes/Shadow/VsmCache.MODE3");
        ID3D12PipelineState* free = sh.compute("Passes/Shadow/VsmCache.MODE4");
        chain.add("s.vsm.cache.keep",
                  [&](PassBuilder& b) {
                      b.use(changed, Use::UavCompute);
                      b.use(table, Use::UavCompute);
                      b.use(requests, Use::UavCompute);
                      b.use(usedPages, Use::UavCompute);
                      b.use(statsBuf, Use::UavCompute);
                  },
                  [=](PassContext& ctx) {
                      uint32_t k[kCacheWords];
                      cacheWords(ctx, k);
                      ctx.cmd->SetPipelineState(keep);
                      ctx.computeConstants(k, kCacheWords);
                      ctx.cmd->Dispatch(cacheable ? groups(scanSlots, 256) : 1, 1, 1);  // (sun and local slots)
                  });
        chain.add("s.vsm.cache.free",
                  [&](PassBuilder& b) {
                      b.use(usedPages, Use::UavCompute);
                      b.use(freePages, Use::UavCompute);
                  },
                  [=](PassContext& ctx) {
                      uint32_t k[kCacheWords];
                      cacheWords(ctx, k);
                      ctx.cmd->SetPipelineState(free);
                      ctx.computeConstants(k, kCacheWords);
                      ctx.cmd->Dispatch(1, 1, 1);
                  });
    }
    ID3D12CommandSignature* signature = s.dispatchSignature.Get();
    {
        // Deterministic page assignment (VsmScan): count per group, prefix, assign.
        ID3D12PipelineState* p0 = sh.compute("Passes/Shadow/VsmScan.MODE0");
        ID3D12PipelineState* p1 = sh.compute("Passes/Shadow/VsmScan.MODE1");
        ID3D12PipelineState* p2 = sh.compute("Passes/Shadow/VsmScan.MODE2");
        auto words = [=](PassContext& ctx, uint32_t k[16]) {
            const uint32_t w[16] = { ctx.uav(requests), ctx.uav(table), ctx.uav(scanGroupsBuf), ctx.uav(statsBuf), scanSlots, pagesNow, ring, scanGroups,
                                     ctx.uav(pageList), ctx.uav(meta), localLightsSrv, ctx.uav(args), ctx.uav(freePages),
                                     separate ? ctx.uav(dynamicList) : 0xFFFFFFFFu, 0, 0 };
            std::memcpy(k, w, sizeof w);
        };
        chain.add("s.vsm.scan.count",
                  [&](PassBuilder& b) {
                      b.use(requests, Use::UavCompute);
                      b.use(scanGroupsBuf, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      uint32_t k[16];
                      const uint32_t w[16] = { ctx.uav(requests), 0, ctx.uav(scanGroupsBuf), 0, scanSlots, pagesNow, ring, scanGroups, 0, 0, 0, 0, 0, 0, 0, 0 };
                      std::memcpy(k, w, sizeof w);
                      ctx.cmd->SetPipelineState(p0);
                      ctx.computeConstants(k, 16);
                      ctx.cmd->Dispatch(scanGroups, 1, 1);
                  });
        chain.add("s.vsm.scan.prefix",
                  [&](PassBuilder& b) {
                      b.use(scanGroupsBuf, Use::UavCompute);
                      b.use(statsBuf, Use::UavCompute);
                      b.use(pageList, Use::UavCompute);
                      b.use(args, Use::UavCompute);
                      b.use(freePages, Use::UavCompute);
                      if (separate) b.use(dynamicList, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[16] = { 0, 0, ctx.uav(scanGroupsBuf), ctx.uav(statsBuf), scanSlots, pagesNow, ring, scanGroups, ctx.uav(pageList), 0, 0,
                                               ctx.uav(args), ctx.uav(freePages), separate ? ctx.uav(dynamicList) : 0xFFFFFFFFu, 0, 0 };
                      ctx.cmd->SetPipelineState(p1);
                      ctx.computeConstants(k, 16);
                      ctx.cmd->Dispatch(1, 1, 1);
                  });
        chain.add("s.vsm.scan.assign",
                  [&](PassBuilder& b) {
                      b.use(requests, Use::UavCompute);
                      b.use(table, Use::UavCompute);
                      b.use(scanGroupsBuf, Use::UavCompute);
                      b.use(statsBuf, Use::UavCompute);
                      b.use(pageList, Use::UavCompute);
                      b.use(meta, Use::UavCompute);
                      b.use(freePages, Use::UavCompute);
                      if (separate)
                      {
                          b.use(dynamicList, Use::UavCompute);
                          b.use(args, Use::UavCompute);
                      }
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      uint32_t k[16];
                      words(ctx, k);
                      ctx.cmd->SetPipelineState(p2);
                      ctx.computeConstants(k, 16);
                      ctx.cmd->Dispatch(scanGroups, 1, 1);
                  });
    }
    chain.flush("s.vsm.scan");
    {
        ID3D12PipelineState* pm = sh.compute("Passes/Shadow/VsmCullMask");
        g.addPass("s.vsm.cullmask", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(table, Use::SrvCompute);
                      b.use(mask, Use::UavCompute);
                      b.use(atlasSlots, Use::UavCompute);
                      if (twoPhase) b.use(occluderGuess, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[8] = { ctx.srv(table), ctx.uav(mask), ring, ctx.uav(atlasSlots), separate ? 1u : 0u, twoPhase ? ctx.uav(occluderGuess) : 0xFFFFFFFFu, 0, 0 };
                      ctx.cmd->SetPipelineState(pm);
                      ctx.computeConstants(k, 8);
                      ctx.cmd->Dispatch(groups(kSlots / 32, 64), 1, 1);
                  });
    }
    if (fc.services.rasterizeDepth && activeLocal > 0)
    {
        ID3D12PipelineState* pm = sh.compute("Passes/Shadow/VsmLocalCullMask");
        g.addPass("s.vsm.localcullmask", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(table, Use::SrvCompute);
                      b.use(localMask, Use::UavCompute);
                      b.use(localSlots, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[8] = { ctx.srv(table), ctx.uav(localMask), activeLocal, activeSrv, ctx.uav(localSlots), 0, 0, 0 };
                      ctx.cmd->SetPipelineState(pm);
                      ctx.computeConstants(k, 8);
                      ctx.cmd->Dispatch(groups(activeLocal * kLocalLightWords, 64), 1, 1);
                  });
    }
    {
        // The atlas: cleared as a whole when every page is drawn (an uncacheable frame); with kept pages only the pages of
        // this frame's list (VsmClearPages: one quad per page at depth 0, depth test ALWAYS). One pass either way (the
        // render graph's plan key).
        MeshPipelineDesc d;
        d.meshShader = "Passes/Shadow/VsmClearPages.ms";
        d.depthFormat = DXGI_FORMAT_D32_FLOAT;
        d.depthWrite = true;
        d.depthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
        d.cull = D3D12_CULL_MODE_NONE;
        ID3D12PipelineState* clearPages = cacheOn ? sh.mesh("s.vsm.clearpages", d) : nullptr;
        const uint32_t atlasW = kAtlasPagesPerRow * kPage, atlasH = pagesNow / kAtlasPagesPerRow * kPage;
        const bool whole = !cacheable;
        // static_separate: the pages drawn anew are cleared in both atlases (the movable casters' raster starts from an
        // empty sampled page; the merge brings the static copy in), the kept pages whose movable casters are drawn anew
        // in the sampled atlas alone.
        g.addPass("s.vsm.clear", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(atlas, Use::DepthWrite);
                      b.use(pageList, Use::SrvGraphics);
                      if (separate)
                      {
                          b.use(atlasStatic, Use::DepthWrite);
                          b.use(dynamicList, Use::SrvGraphics);
                      }
                  },
                  [=](PassContext& ctx) {
                      if (whole || !clearPages)
                      {
                          ctx.cmd->ClearDepthStencilView(ctx.dsv(atlas), D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
                          if (separate) ctx.cmd->ClearDepthStencilView(ctx.dsv(atlasStatic), D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr);
                          return;
                      }
                      D3D12_VIEWPORT vp{ 0, 0, (float)atlasW, (float)atlasH, 0, 1 };
                      D3D12_RECT sc{ 0, 0, (LONG)atlasW, (LONG)atlasH };
                      ctx.cmd->RSSetViewports(1, &vp);
                      ctx.cmd->RSSetScissorRects(1, &sc);
                      ctx.cmd->SetPipelineState(clearPages);
                      const uint32_t groupCount = (pagesNow + 31) / 32;  // VsmClearPages.ms: 32 pages per group, the list's count
                      auto clearList = [&](TextureRef target, BufferRef list) {
                          const D3D12_CPU_DESCRIPTOR_HANDLE dsv = ctx.dsv(target);
                          ctx.cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
                          const uint32_t k[4] = { ctx.srv(list), atlasW, atlasH, 0 };
                          ctx.graphicsConstants(k, 4);
                          ctx.cmd->DispatchMesh(groupCount, 1, 1);
                      };
                      clearList(atlas, pageList);
                      if (separate)
                      {
                          clearList(atlasStatic, pageList);
                          clearList(atlas, dynamicList);
                      }
                  });
    }

    // Every requested page through V's cluster pipeline into its atlas slot (tile atlas, hardware depth, no pixel
    // kernel): one orthographic view per level over its whole window; the tile mask limits the raster to the pages
    // with a slot this frame.
    // The runs' visible-list bound (casterBounds): the views are packed into requests whose bounds sum to at most the
    // list capacity; a single view over it stops the frame (a structural limit of this scene, never a silent drop).
    // shadow.vsm.raster_split false (A/B): the old packing - one sun request, 6 lights per local request - whose lists can
    // overflow; the bound is then infinite.
    const bool split = !q.has("shadow.vsm.raster_split") || q.boolean("shadow.vsm.raster_split");
    const uint64_t listCapacity = split ? (uint64_t)q.integer("visibility.max_visible_clusters") : UINT64_MAX;
    // shadow.vsm.raster_lod_bound false (A/B): the leaf count of every caster in every view it reaches
    const bool lodBound = !q.has("shadow.vsm.raster_lod_bound") || q.boolean("shadow.vsm.raster_lod_bound");
    // shadow.vsm.min_caster_texels: the page views leave out the casters under this many of their texels in radius (V's
    // instance cull, RasterView::minInstanceTexels); the packing bounds leave them out too. The classification pages
    // (below) keep every caster: a face lit there must be lit on every mip.
    const float minCasterTexels = q.has("shadow.vsm.min_caster_texels") ? (float)q.number("shadow.vsm.min_caster_texels") : 0.0f;
    if (!(minCasterTexels >= 0)) fail("shadow.vsm.min_caster_texels = %g: 0 (every caster) or a positive radius in texels", minCasterTexels);
    // shadow.vsm.aggregate_small_casters (with min_caster_texels): a sun level draws the static casters under its
    // smallest caster as proxies (V's DepthRasterRequest::proxies) - their shadow as a density, not their clusters.
    const bool aggregate = minCasterTexels > 0 && q.has("shadow.vsm.aggregate_small_casters") && q.boolean("shadow.vsm.aggregate_small_casters");
    const float aggregateCoverage = q.has("shadow.vsm.aggregate_coverage") ? (float)q.number("shadow.vsm.aggregate_coverage") : 0.5f;
    if (aggregate && !(aggregateCoverage > 0 && aggregateCoverage <= 1)) fail("shadow.vsm.aggregate_coverage = %g: a share in (0, 1]", aggregateCoverage);
    const CasterBounds bounds = fc.services.rasterizeDepth && split
                                    ? casterBounds(fc.scene, lodBound, (float)q.number("visibility.lod_error_px"),
                                                   fc.scene.source() ? fc.scene.source()->windSpeed : 0.0f, minCasterTexels)
                                    : CasterBounds{};
    uint32_t sunRequests = 0, localRequests = 0;
    // The sun levels' requests of one instance set into one atlas ('base' names them: base, base1, ..; maskWords: the set's
    // first word of the tile mask and slots). shadow.vsm.static_separate: the casters that are not movable into the static
    // atlas where a page is drawn anew, the movable ones into the sampled atlas where a page is drawn anew or its movable
    // casters changed; without it every caster into the sampled atlas.
    // The HZB of the static copies of the page list's pages as the static atlas stands (VsmStaticHzb.hlsl: one group per
    // page; the kept pages have theirs from the frame their copy was drawn in).
    auto staticHzbPass = [&](const char* name) {
        ID3D12PipelineState* ph = sh.compute("Passes/Shadow/VsmStaticHzb");
        g.addPass(name, QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(pageList, Use::SrvCompute);
                      b.use(args, Use::IndirectArgs);
                      b.use(atlasStatic, Use::SrvCompute);
                      b.use(staticHzb, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[4] = { ctx.srv(pageList), ctx.srv(atlasStatic), ctx.uav(staticHzb), 0 };
                      ctx.cmd->SetPipelineState(ph);
                      ctx.computeConstants(k, 4);
                      ctx.cmd->ExecuteIndirect(signature, 1, ctx.resource(args), 0, nullptr, 0);
                  });
    };
    // occlusion: its views are culled against the static copies' HZB - 1: as it stands (the movable set's views,
    // shadow.vsm.static_hzb_cull); 2: in two phases (the static set's own views, static_occlusion_two_phase: a kept coarser
    // page's HZB as the guess, then the HZB of what the first phase drew).
    auto sunFamily = [&](const std::string& base, TextureRef target, uint32_t maskWords, uint32_t instanceSet, uint32_t occlusion) {
        const bool occluders = occlusion != 0;
        uint32_t made = 0;
        auto request = [&](const std::string& name) {
            DepthRasterRequest r;
            r.name = name;
            r.instanceMask = scene::InstanceCastShadow;
            r.depthTarget = target;
            r.atlasSlots = atlasSlots;
            if (occluders)
            {
                r.tileOccluders = staticHzb;
                r.tileOccludersSrv = staticHzbSrv;
                r.atlasSlotsSrv = atlasSlotsSrv;
            }
            if (occlusion == 2)
            {
                r.tileGuess = occluderGuess;
                r.tileGuessSrv = occluderGuessSrv;
                r.buildTileOccluders = [&]() { staticHzbPass("s.vsm.statichzb.p1"); };
            }
            r.atlasTilesPerRow = kAtlasPagesPerRow;
            r.cullMask = mask;
            r.cullTilePx = kPage;
            r.tileLocal = true;
            r.cull = D3D12_CULL_MODE_NONE;
            r.proxies = aggregate;
            r.proxyCoverage = aggregateCoverage;
            return r;
        };
        DepthRasterRequest r = request(base);
        uint64_t sum = 0;
        for (uint32_t k = 0; k < kLevels; ++k)
        {
            RasterView v;
            v.viewProj = levelViewProj(c, k);
            v.viewportX = v.viewportY = 0;
            v.viewportWidth = v.viewportHeight = kVirtual;
            v.lodPixelsPerMetre = 1.0f / std::ldexp(1.0f, (int)k - 10);
            v.minInstanceTexels = minCasterTexels;
            v.userData = k;
            v.instanceSet = instanceSet;
            v.tileOccluders = occluders;
            v.tileTwoPhase = occlusion == 2;
            v.cullMaskOffset = maskWords + k * (kTable * kTable / 32);
            const uint64_t bound = split ? levelBound(bounds, v.viewProj, v.lodPixelsPerMetre, instanceSet) : 0;
            if (bound > listCapacity)
            {
                // The level alone is over a run's lists: one request per instance batch of its casters - while a few
                // batches do it. The bound is every caster of the level's window at the level's cut, whatever pages are
                // asked for: a forest's 1.1 M trees stand at thousands of capacities in the middle levels (5,146
                // requests, batch 3), where a frame draws a few hundred pages. Past kMaxLevelBatches (or with no split)
                // the level is one request without the guarantee: V's run reports an overflow of its lists
                // (VSM_ERR_RASTER_OVERFLOW; the casters past the capacity are missing from that level's pages that frame).
                constexpr size_t kMaxLevelBatches = 4;
                std::vector<LevelBatch> batches = levelBatches(bounds, v.viewProj, v.lodPixelsPerMetre, listCapacity, instanceSet);
                if (batches.empty() || batches.size() > kMaxLevelBatches)
                {
                    if (fc.frame.frameIndex == 0)
                        logf("S VSM: sun level %u can reach %llu cluster entries (list capacity %llu): drawn as one request, an overflow is reported\n", k,
                             (unsigned long long)bound, (unsigned long long)listCapacity);
                    batches.assign(1, LevelBatch{ 0, 0 });
                }
                if (!r.views.empty())
                {
                    fc.services.rasterizeDepth(fc, r);
                    r = request(base + std::to_string(++made));
                    sum = 0;
                }
                for (const LevelBatch& batch : batches)
                {
                    RasterView bv = v;
                    bv.instanceFirst = batch.first;
                    bv.instanceEnd = batch.end;
                    r.views.push_back(bv);
                    fc.services.rasterizeDepth(fc, r);
                    r = request(base + std::to_string(++made));
                }
                continue;
            }
            if (!r.views.empty() && sum + bound > listCapacity)
            {
                fc.services.rasterizeDepth(fc, r);
                r = request(base + std::to_string(++made));
                sum = 0;
            }
            r.views.push_back(v);
            sum += bound;
        }
        if (!r.views.empty())
        {
            fc.services.rasterizeDepth(fc, r);
            ++made;
        }
        return made;
    };
    if (fc.services.rasterizeDepth)
    {
        if (separate)
        {
            sunRequests = sunFamily("s.vsm.static", atlasStatic, 0, 1, twoPhase ? 2u : 0u);
            // The HZB of the static copies drawn just now; then the movable casters' views are culled against it: a
            // caster under the static surface of every page it would be drawn into leaves no texel after the merge.
            if (hzbCull) staticHzbPass("s.vsm.statichzb");
            sunRequests += sunFamily("s.vsm.raster", atlas, kLevels * (kTable * kTable / 32), 2, hzbCull ? 1u : 0u);
        }
        else
            sunRequests = sunFamily("s.vsm.raster", atlas, 0, 0, 0);
    }
    if (fc.services.rasterizeDepth && activeLocal > 0)
    {
        // Local lights: the (light, face, mip) views (42 per light) packed into requests of at most
        // shadow.vsm.local_request_views views (whole lights; V's limit: kDepthRasterMaxViews) whose bounds (localBound per
        // view) sum to at most the list capacity; each view's viewport is its mip's resolution, the tile masks and slots
        // are packed per light (VsmLocalCullMask). 252 (6 lights) was the limit of V's 8-bit view field: a cull chain per
        // 6 lights.
        const uint32_t viewsPerLight = 6 * kLocalMips;
        const int64_t requestViewsWanted = q.has("shadow.vsm.local_request_views") ? q.integer("shadow.vsm.local_request_views") : 252;
        if (requestViewsWanted < (int64_t)viewsPerLight || requestViewsWanted > (int64_t)kDepthRasterMaxViews)
            fail("shadow.vsm.local_request_views = %lld: %u (one light) .. %u", (long long)requestViewsWanted, viewsPerLight, kDepthRasterMaxViews);
        const uint32_t requestViews = (uint32_t)requestViewsWanted / viewsPerLight * viewsPerLight;
        auto request = [&]() {
            DepthRasterRequest r;
            r.name = "s.vsm.localraster" + std::to_string(localRequests++);
            r.instanceMask = scene::InstanceCastShadow;
            r.depthTarget = atlas;
            r.atlasSlots = localSlots;
            r.atlasTilesPerRow = kAtlasPagesPerRow;
            r.cullMask = localMask;
            r.cullTilePx = kPage;
            r.tileLocal = true;
            r.cull = D3D12_CULL_MODE_NONE;
            return r;
        };
        DepthRasterRequest r = request();
        uint64_t sum = 0;
        {
            for (uint32_t a = 0; a < activeLocal; ++a)
            {
                const uint32_t slot = s.localActive[a];
                const VsmLocalLightCpu& l = s.localData[slot];
                uint64_t faceBounds[6][kLocalMips] = {};
                if (split) localBounds(bounds, l, faceBounds);
                for (uint32_t face = 0; face < 6; ++face)
                    for (uint32_t mip = 0; mip < kLocalMips; ++mip)
                        if (faceBounds[face][mip] > listCapacity)
                            fail("S VSM: local light %u face %u mip %u can reach %llu cluster entries, over the raster list capacity %llu (visibility.max_visible_clusters)",
                                 l.lightIndex, face, mip, (unsigned long long)faceBounds[face][mip], (unsigned long long)listCapacity);
                for (uint32_t face = 0; face < 6; ++face)
                    for (uint32_t mip = 0; mip < kLocalMips; ++mip)
                    {
                        const uint64_t bound = faceBounds[face][mip];
                        if (!r.views.empty() && (sum + bound > listCapacity || r.views.size() >= (split ? requestViews : 42u * 6u) || (!split && face == 0 && mip == 0 && a % 6 == 0)))
                        {
                            fc.services.rasterizeDepth(fc, r);
                            r = request();
                            sum = 0;
                        }
                        RasterView v;
                        v.viewProj = localViewProj(l, face);
                        v.viewportX = v.viewportY = 0;
                        v.viewportWidth = v.viewportHeight = kPage << mip;
                        v.lodPixelsPerMetre = 0.5f * (float)(kPage << mip);  // focal length in texels (90 degree face)
                        v.minInstanceTexels = minCasterTexels;
                        v.userData = slot | face << 7 | mip << 10;
                        v.cullMaskOffset = a * kLocalLightWords + face * kLocalFaceWords + kLocalViewWordOffset[mip];
                        r.views.push_back(v);
                        sum += bound;
                    }
            }
            fc.services.rasterizeDepth(fc, r);
        }
    }
    if (fc.services.rasterizeDepth && (s.rasterRequests[0] != sunRequests || s.rasterRequests[1] != localRequests))
    {
        logf("S VSM: %u sun and %u local raster requests (%u active local lights; views packed under the list capacity %llu)\n", sunRequests, localRequests,
             activeLocal, (unsigned long long)listCapacity);
        s.rasterRequests[0] = sunRequests;
        s.rasterRequests[1] = localRequests;
    }
    // L3 stage 1 (14.3-1/2): the classification pages of the active lights (conservative raster, nearest depth over the
    // texel: VsmClsPixel), their blocks (VsmClsBlocks) and the (tile, light) lit classification of the main view
    // (LocalTileClassify) - shadow.vsm.classification_pages. Every active light's six faces are drawn each frame (the
    // static / dynamic layers of 14.3-6 follow).
    fc.resources.vsmTileLit = {};
    s.clsBlocksRef = {};
    s.clsActiveCount = 0;
    if (fc.services.rasterizeDepth && activeLocal > 0 && main.depth.valid() && q.boolean("shadow.vsm.classification_pages"))
    {
        const uint32_t clsPages = activeLocal * 6, clsRows = (clsPages + 47) / 48, clsWidth = 48 * 128, clsHeight = clsRows * 128;
        const uint64_t clsWords = (uint64_t)clsWidth * clsHeight;
        if (clsWords * 4 > s.clsAtlasBytes)
            fail("S VSM: %u active local lights need %llu B of classification atlas, %llu B allocated", activeLocal, (unsigned long long)(clsWords * 4), (unsigned long long)s.clsAtlasBytes);
        const BufferRef clsAtlas = g.importBuffer(s.clsAtlas.Get(), BufferDesc{ "S VSM classification atlas", s.clsAtlasBytes, 0 });
        const BufferRef clsBlocks = g.createBuffer(BufferDesc{ "S VSM classification blocks", (uint64_t)clsPages * 256 * 4, 0 });
        const bool twin = q.boolean("shadow.vsm.classification_twin");
        const BufferRef clsTwin = twin ? g.importBuffer(s.clsTwin.Get(), BufferDesc{ "S VSM classification twin", s.clsAtlasBytes, 0 }) : BufferRef{};
        const BufferRef twinBlocks = twin ? g.createBuffer(BufferDesc{ "S VSM classification twin blocks", (uint64_t)clsPages * 256 * 4, 0 }) : BufferRef{};
        const uint32_t tilesX = groups(main.view.width, 8), tilesY = groups(main.view.height, 8);
        const BufferRef tileLit = g.createBuffer(BufferDesc{ "S VSM tile lit", (uint64_t)tilesX * tilesY * 80, 0 });
        auto asUint = [](float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; };
        ID3D12PipelineState* pClear = sh.compute("Passes/Shadow/VsmClsClear");
        for (int which = 0; which < (twin ? 2 : 1); ++which)
        {
            const BufferRef target = which == 0 ? clsAtlas : clsTwin;
            g.addPass(which == 0 ? "s.vsm.cls.clear" : "s.vsm.cls.cleartwin", QueueType::Compute, [&](PassBuilder& b) { b.use(target, Use::UavCompute); },
                      [=](PassContext& ctx) {
                          const uint32_t k[4] = { ctx.uav(target), (uint32_t)clsWords, 0, 0 };
                          ctx.cmd->SetPipelineState(pClear);
                          ctx.computeConstants(k, 4);
                          ctx.cmd->Dispatch(groups((uint32_t)(clsWords / 4), 256), 1, 1);
                      });
        }
        for (int which = 0; which < (twin ? 2 : 1); ++which)
        {
            // (V passes pixelConstants as given, so the atlas is a persistent buffer with a persistent raw UAV descriptor.)
            // which 0: the conservative classification pages (corner max); 1: the exact twin (standard raster, centre).
            DepthRasterRequest cr;
            cr.instanceMask = scene::InstanceCastShadow;
            cr.pixelKernel = which == 0 ? "Passes/Shadow/VsmClsPixel.MODE0" : "Passes/Shadow/VsmClsPixel.MODE1";
            cr.bufferUses = { { which == 0 ? clsAtlas : clsTwin, Use::UavGraphics } };
            cr.conservative = which == 0;
            cr.cull = D3D12_CULL_MODE_NONE;
            cr.pixelConstants[0] = which == 0 ? s.clsAtlasUav : s.clsTwinUav;
            cr.pixelConstants[1] = clsWidth;
            uint64_t sum = 0;
            uint32_t clsRequests = 0;
            auto flush = [&]() {
                if (cr.views.empty()) return;
                cr.name = (which == 0 ? "s.vsm.clsraster" : "s.vsm.clstwinraster") + std::to_string(clsRequests++);
                fc.services.rasterizeDepth(fc, cr);
                cr.views.clear();
                sum = 0;
            };
            for (uint32_t a = 0; a < activeLocal; ++a)
            {
                const uint32_t slot = s.localActive[a];
                const VsmLocalLightCpu& l = s.localData[slot];
                uint64_t faceBounds[6][kLocalMips] = {};
                if (split) localBounds(bounds, l, faceBounds);
                for (uint32_t face = 0; face < 6; ++face)
                {
                    const uint64_t bound = faceBounds[face][0];
                    if (!cr.views.empty() && (sum + bound > listCapacity || cr.views.size() >= 252u)) flush();
                    RasterView v;
                    v.viewProj = localViewProj(l, face);
                    const uint32_t page = a * 6 + face;
                    v.viewportX = (page % 48) * 128;
                    v.viewportY = (page / 48) * 128;
                    v.viewportWidth = v.viewportHeight = kPage;
                    v.lodPixelsPerMetre = 0.5f * (float)kPage;
                    v.userData = slot | face << 7;
                    v.cullMaskOffset = UINT32_MAX;
                    cr.views.push_back(v);
                    sum += bound;
                }
            }
            flush();
        }
        for (int which = 0; which < (twin ? 2 : 1); ++which)
        {
            ID3D12PipelineState* pBlocks = sh.compute(which == 0 ? "Passes/Shadow/VsmClsBlocks.MODE0" : "Passes/Shadow/VsmClsBlocks.MODE1");
            const BufferRef pagesIn = which == 0 ? clsAtlas : clsTwin, blocksOut = which == 0 ? clsBlocks : twinBlocks;
            g.addPass(which == 0 ? "s.vsm.cls.blocks" : "s.vsm.cls.twinblocks", QueueType::Compute,
                      [&](PassBuilder& b) { b.use(pagesIn, Use::SrvCompute); b.use(blocksOut, Use::UavCompute); },
                      [=](PassContext& ctx) {
                          const uint32_t k[4] = { ctx.srv(pagesIn), ctx.uav(blocksOut), clsWidth, 48 };
                          ctx.cmd->SetPipelineState(pBlocks);
                          ctx.computeConstants(k, 4);
                          ctx.cmd->Dispatch(clsPages, 1, 1);
                      });
        }
        ID3D12PipelineState* pClassify = sh.compute("Passes/Shadow/LocalTileClassify");
        const TextureRef depth = main.depth;
        const BufferRef clsStats = s.statsRef;
        const uint32_t toleranceBits = asUint((float)q.number("shadow.vsm.classification_tolerance"));
        g.addPass("s.vsm.cls.tiles", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(depth, Use::SrvCompute);
                      b.use(froxelLists, Use::SrvCompute);
                      b.use(clsBlocks, Use::SrvCompute);
                      if (twin) b.use(twinBlocks, Use::SrvCompute);
                      b.use(tileLit, Use::UavCompute);
                      b.use(clsStats, Use::UavCompute);  // words 64..67: tiles, pairs, lit, umbra (RendererGate)
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[12] = { ctx.srv(depth), ctx.srv(froxelLists), localLightsSrv, slotOfSrv, ctx.srv(clsBlocks), ctx.uav(tileLit), tilesX, toleranceBits,
                                               twin ? ctx.srv(twinBlocks) : 0xFFFFFFFFu, ctx.uav(clsStats), 0, 0 };
                      ctx.cmd->SetPipelineState(pClassify);
                      ctx.bindFrameConstants(mainConstants);
                      ctx.computeConstants(k, 12);
                      ctx.cmd->Dispatch(tilesX, tilesY, 1);
                  });
        fc.resources.vsmTileLit = tileLit;
        s.clsBlocksRef = clsBlocks;
        s.clsActiveCount = activeLocal;
    }

    if (separate)
    {
        // static_separate: the static copy of every page drawn this frame into its sampled page (VsmMergePages.ps: a quad
        // per page whose depth is the static atlas's texel, under the depth test that keeps the nearer): after it a page
        // holds what one raster of all its casters would. The pages drawn anew, then the kept ones whose movable casters
        // were drawn anew. (A local light's page has no static copy: its static page was cleared, the merge leaves it.)
        MeshPipelineDesc d;
        d.meshShader = "Passes/Shadow/VsmClearPages.ms";
        d.pixelShader = "Passes/Shadow/VsmMergePages.ps";
        d.depthFormat = DXGI_FORMAT_D32_FLOAT;
        d.depthWrite = true;
        d.depthFunc = D3D12_COMPARISON_FUNC_GREATER;
        d.cull = D3D12_CULL_MODE_NONE;
        ID3D12PipelineState* merge = sh.mesh("s.vsm.mergepages", d);
        const uint32_t atlasW = kAtlasPagesPerRow * kPage, atlasH = pagesNow / kAtlasPagesPerRow * kPage;
        g.addPass("s.vsm.merge", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(atlas, Use::DepthWrite);
                      b.use(atlasStatic, Use::SrvGraphics);
                      b.use(pageList, Use::SrvGraphics);
                      b.use(dynamicList, Use::SrvGraphics);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const D3D12_CPU_DESCRIPTOR_HANDLE dsv = ctx.dsv(atlas);
                      ctx.cmd->OMSetRenderTargets(0, nullptr, FALSE, &dsv);
                      D3D12_VIEWPORT vp{ 0, 0, (float)atlasW, (float)atlasH, 0, 1 };
                      D3D12_RECT sc{ 0, 0, (LONG)atlasW, (LONG)atlasH };
                      ctx.cmd->RSSetViewports(1, &vp);
                      ctx.cmd->RSSetScissorRects(1, &sc);
                      ctx.cmd->SetPipelineState(merge);
                      const uint32_t groupCount = (pagesNow + 31) / 32;  // VsmClearPages.ms: 32 pages per group, the list's count
                      for (BufferRef list : { pageList, dynamicList })
                      {
                          const uint32_t k[4] = { ctx.srv(list), atlasW, atlasH, ctx.srv(atlasStatic) };
                          ctx.graphicsConstants(k, 4);
                          ctx.cmd->DispatchMesh(groupCount, 1, 1);
                      }
                  });
    }
    {
        ID3D12PipelineState* pso = sh.compute("Passes/Shadow/VsmPageMax");
        g.addPass("s.vsm.pagemax", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(pageList, Use::SrvCompute);
                      b.use(args, Use::IndirectArgs);
                      b.use(atlas, Use::SrvCompute);
                      b.use(meta, Use::UavCompute);
                      b.use(blocks, Use::UavCompute);
                      if (separate) b.use(dynamicList, Use::SrvCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      uint32_t k[8] = { ctx.srv(pageList), ctx.srv(atlas), ctx.uav(meta), ring, localLightsSrv, ctx.uav(blocks), 0, 0 };
                      ctx.cmd->SetPipelineState(pso);
                      ctx.computeConstants(k, 8);
                      ctx.cmd->ExecuteIndirect(signature, 1, ctx.resource(args), 0, nullptr, 0);
                      if (separate)
                      {
                          // the kept pages whose movable casters were drawn anew (other pages than the list's: no barrier)
                          k[0] = ctx.srv(dynamicList);
                          ctx.computeConstants(k, 8);
                          ctx.cmd->ExecuteIndirect(signature, 1, ctx.resource(args), 12, nullptr, 0);
                      }
                  });
    }
    {
        // Search bound grid for the visibility pass (VsmSearchGrid): fill, then 3 x 3 dilation.
        ID3D12PipelineState* pf = sh.compute("Passes/Shadow/VsmSearchGrid.MODE0");
        ID3D12PipelineState* pd = sh.compute("Passes/Shadow/VsmSearchGrid.MODE1");
        const BufferRef fill = g.createBuffer(BufferDesc{ "S VSM search fill", (uint64_t)kSlots * 4, 0 });
        const BufferRef bound = g.createBuffer(BufferDesc{ "S VSM search bound", (uint64_t)kSlots * 4, 0 });
        s.boundRef = bound;
        fc.resources.vsmSearchBound = bound;
        g.addPass("s.vsm.searchgrid", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(table, Use::SrvCompute);
                      b.use(meta, Use::SrvCompute);
                      b.use(fill, Use::UavCompute);
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[8] = { ctx.srv(table), ctx.srv(meta), ctx.uav(fill), 0, ring, off, 0, 0 };
                      ctx.cmd->SetPipelineState(pf);
                      ctx.computeConstants(k, 8);
                      ctx.cmd->Dispatch(groups(kSlots, 256), 1, 1);
                  });
        g.addPass("s.vsm.searchdilate", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(fill, Use::SrvCompute);
                      b.use(bound, Use::UavCompute);
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[8] = { 0, 0, ctx.srv(fill), ctx.uav(bound), ring, off, 0, 0 };
                      ctx.cmd->SetPipelineState(pd);
                      ctx.computeConstants(k, 8);
                      ctx.cmd->Dispatch(groups(kSlots, 256), 1, 1);
                  });
    }
}

void recordVisibility(FramePassContext& fc, ViewResources& view)
{
    State& s = fc.state<State>(kStateKey);
    if (!s.pagesRecorded || s.recordedFrame != fc.frame.frameIndex) fail("S.shadowVisibility: shadowPages was not recorded this frame");
    if (!view.depth.valid() || !view.gbuffer.valid())
    {
        tracks::pending("S.shadowVisibility (view without depth or G-buffer)");
        return;
    }
    RenderGraph& g = fc.graph;
    // shadow.vsm.fold_small_passes: the list clear is the visibility pass's first dispatch, the overflow scan's two
    // levels are one pass, and a view without local shadow slots records no overflow passes (below).
    const bool foldSmall = !fc.quality.has("shadow.vsm.fold_small_passes") || fc.quality.boolean("shadow.vsm.fold_small_passes");
    PassChain chain(g, QueueType::Compute, foldSmall);
    const uint32_t w = view.view.width, h = view.view.height;
    const TextureRef out = g.createTexture(TextureDesc{ "S shadow visibility", w, h, 1, 1, DXGI_FORMAT_R32_UINT });
    view.shadowVisibility = out;
    const TextureRef depth = view.depth, gbuffer = view.gbuffer;
    const TextureRef mirrorMask = view.view.planarMask, mirrorTiles = view.view.planarTileMask;  // planar views: mirror pixels only
    const TextureRef atlas = s.atlasRef;
    // Local slots (1-3) and the overflow list from the view's froxel lists (INTERFACES 7.3, 7.4, v1.22): the main view's
    // (FrameRenderer sets them after froxels; tests without froxels: shadowPages' lists), a planar reflection view's own
    // (recorded here with its air volume from the mirror plane on).
    const bool mainView = view.view.kind == gpu::ViewKind::Main;
    if (view.view.kind == gpu::ViewKind::PlanarReflection && !view.froxelLights.valid()) recordPlanarFroxels(fc, view);
    // (A14's auxiliary views with the fog - atmosphere.fog.auxiliary_views: their own lists, air and fog volume, as a
    //  planar view's; without a fog record they stay as they were, without lists and air)
    else if (!mainView && !view.froxelLights.valid() && fogSecondaryFor(fc, view.frameConstants)) recordPlanarFroxels(fc, view);
    const BufferRef froxelLists = view.froxelLights.valid() ? view.froxelLights : (mainView ? fc.resources.froxelLights : BufferRef{});
    const bool localSlots = froxelLists.valid() && s.localLightsNow != UINT32_MAX && !megaLightsOwnLocalShadows(fc);
    const uint32_t localLightsSrv = s.localLightsNow, slotOfSrv = s.slotOfNow;
    const BufferRef table = s.tableRef, bound = s.boundRef, blocks = s.blocksRef, statsBuf = s.statsRef, layers = s.layersRef, useBuf = s.useRef;
    // Overflow list (INTERFACES 7.3, v1.20): the main view's shadow-casting lights past the third. Capacity = 1.5 x the
    // need of the last completed frame (the needs are summed past the capacity, so an overage frame reports its full
    // need), a power of two of words, at least 1 MB; shrinks only below a quarter (no plan churn around a boundary).
    // Allocation (RENDERER_REDESIGN_V2 14.3-3, L3 stage 3): a count pass writes each listed tile's need, a two-level
    // prefix sum in tile order (ShadowOverflowScan) gives the block starts, the fill pass evaluates: the block layout
    // and the set of tiles over the capacity depend only on the frame's content.
    const bool overflowList = true;  // every view (its own lists; empty without local slots)
    const uint32_t tilesX = groups(w, 8), tilesY = groups(h, 8), tiles = tilesX * tilesY;
    if (tiles > 2048u * 2048u) fail("shadow overflow: %u tiles exceed the two-level scan (2048 x 2048)", tiles);
    TextureRef heads;
    BufferRef overflow, overflowTiles, fallback, needs, blockSums;
    uint32_t capacity = 0;
    if (overflowList)
    {
        const uint64_t need = s.latest.overflowWords;
        uint64_t target = std::max<uint64_t>(kOverflowMinWords, need + need / 2);
        uint64_t pow2 = kOverflowMinWords;
        while (pow2 < target) pow2 <<= 1;
        target = std::min<uint64_t>(pow2, 1ull << 30);
        if (target > s.overflowCapacity || target * 4 <= s.overflowCapacity)
        {
            if (s.overflowCapacity && target > s.overflowCapacity)
                logf("S shadow overflow: frame %llu needed %u words (%u tiles over capacity): capacity grows to %llu words\n",
                     (unsigned long long)s.latest.frame, s.latest.overflowWords, s.latest.overflowOverTiles, (unsigned long long)target);
            s.overflowCapacity = (uint32_t)target;
        }
        capacity = s.overflowCapacity;
        const uint32_t forced = (uint32_t)fc.quality.integer("shadow.vsm.overflow_capacity_words");
        if (forced > 0) capacity = forced;  // tests (M's fallback against the list): every tile over it goes to the fallback
        s.latest.overflowCapacity = capacity;
        heads = g.createTexture(TextureDesc{ "S shadow overflow tiles", tilesX, tilesY, 1, 1, DXGI_FORMAT_R32_UINT });
        overflow = g.createBuffer(BufferDesc{ "S shadow overflow", (uint64_t)std::max(capacity, 64u) * 4, 0 });
        overflowTiles = g.createBuffer(BufferDesc{ "S shadow overflow tile list", 16 + (uint64_t)tiles * 4, 0 });
        fallback = g.createBuffer(BufferDesc{ "S shadow overflow fallback tiles", 16 + (uint64_t)tiles * 4, 0 });
        needs = g.createBuffer(BufferDesc{ "S shadow overflow need", (uint64_t)tiles * 4, 0 });
        blockSums = g.createBuffer(BufferDesc{ "S shadow overflow block sums", 2049 * 4, 0 });
        view.shadowOverflowTiles = heads;
        view.shadowOverflow = overflow;
        view.shadowOverflowFallbackTiles = fallback;
    }
    const uint32_t ring = s.ringCbv[s.constantsOffset / kRingStride];
    const uint32_t rays = (uint32_t)fc.quality.integer("shadow.vsm.search_taps"), steps = (uint32_t)fc.quality.integer("shadow.vsm.filter_taps");
    // The sun's screen-space contact ray (ShadowReceiver.hlsli shadowSunContact): steps | round(length x 2^20) << 8, 0 = off.
    uint32_t contact = 0;
    if (fc.quality.has("shadow.vsm.screen_ray_length") && fc.quality.has("shadow.vsm.screen_ray_steps"))
    {
        const double length = fc.quality.number("shadow.vsm.screen_ray_length");
        const int64_t contactSteps = fc.quality.integer("shadow.vsm.screen_ray_steps");
        if (length < 0 || length > 1 || contactSteps < 0 || contactSteps > 16) fail("shadow.vsm.screen_ray_*: a length in [0, 1] of the view depth and 0..16 steps");
        if (length > 0 && contactSteps > 0) contact = (uint32_t)contactSteps | ((uint32_t)std::llround(length * 1048576.0) << 8);
    }
    const D3D12_GPU_VIRTUAL_ADDRESS constants = view.frameConstants;
    const char* variant = s.debugPaths ? ".PATHS1" : ".PATHS0";
    ID3D12PipelineState* pc = fc.shaders.compute("Passes/Shadow/ShadowListClear");
    const TextureRef tlut = fc.resources.transmittanceLut;  // B5 cloud shadows (the atmosphere record names the cloud record)
    ID3D12PipelineState* p1 = fc.shaders.compute(std::string("Passes/Shadow/ShadowVisibility") + variant);
    ID3D12PipelineState* pa = fc.shaders.compute("Passes/Shadow/ShadowListArgs");
    ID3D12PipelineState* p2 = fc.shaders.compute(std::string("Passes/Shadow/ShadowPenumbra") + variant + ".STAGE0");
    ID3D12PipelineState* p3 = s.debugPaths ? nullptr : fc.shaders.compute("Passes/Shadow/ShadowPenumbra.PATHS0.STAGE1");
    ID3D12PipelineState* poCount = overflowList ? fc.shaders.compute("Passes/Shadow/ShadowOverflow.MODE0") : nullptr;
    ID3D12PipelineState* poFill = overflowList ? fc.shaders.compute("Passes/Shadow/ShadowOverflow.MODE1") : nullptr;
    ID3D12PipelineState* poScan0 = overflowList ? fc.shaders.compute("Passes/Shadow/ShadowOverflowScan.MODE0") : nullptr;
    ID3D12PipelineState* poScan1 = overflowList ? fc.shaders.compute("Passes/Shadow/ShadowOverflowScan.MODE1") : nullptr;
    ID3D12CommandSignature* signature = s.dispatchSignature.Get();
    // Pass 1 settles the pixels the page structures decide; the mixed ones go to a list for pass 2 (indirect).
    const BufferRef list = g.createBuffer(BufferDesc{ "S penumbra list", 4 + (uint64_t)w * h * 4, 0 });
    const BufferRef args = g.createBuffer(BufferDesc{ "S penumbra args", 16, 0 });
    // The penumbra's filtered pixels (STAGE=0 -> STAGE=1): count, then 16 B records from byte 16.
    const BufferRef filterList = g.createBuffer(BufferDesc{ "S penumbra filter list", 16 + (uint64_t)w * h * 16, 0 });
    const BufferRef filterArgs = g.createBuffer(BufferDesc{ "S penumbra filter args", 16, 0 });
    chain.add("s.shadow.listclear",
              [&](PassBuilder& b) {
                  b.use(list, Use::UavCompute);
                  if (overflowList)
                  {
                      b.use(overflowTiles, Use::UavCompute);
                      b.use(fallback, Use::UavCompute);
                  }
              },
              [=](PassContext& ctx) {
                  const uint32_t k[4] = { ctx.uav(list), overflowList ? ctx.uav(overflowTiles) : 0xFFFFFFFFu, overflowList ? ctx.uav(fallback) : 0xFFFFFFFFu,
                                          0xFFFFFFFFu };
                  ctx.cmd->SetPipelineState(pc);
                  ctx.computeConstants(k, 4);
                  ctx.cmd->Dispatch(1, 1, 1);
              });
    chain.add("s.shadow.visibility",
              [&](PassBuilder& b) {
                  b.use(depth, Use::SrvCompute);
                  b.use(gbuffer, Use::SrvCompute);
                  b.use(atlas, Use::SrvCompute);
                  b.use(table, Use::SrvCompute);
                  b.use(bound, Use::SrvCompute);
                  b.use(blocks, Use::SrvCompute);
                  b.use(statsBuf, Use::UavCompute);
                  b.use(list, Use::UavCompute);
                  b.use(out, Use::UavCompute);
                  if (localSlots) b.use(froxelLists, Use::SrvCompute);
                  if (overflowList)
                  {
                      b.use(overflowTiles, Use::UavCompute);
                      b.use(heads, Use::UavCompute);
                      b.use(needs, Use::UavCompute);
                  }
                  if (mirrorMask.valid()) b.use(mirrorMask, Use::SrvCompute);
                  if (mirrorTiles.valid()) b.use(mirrorTiles, Use::SrvCompute);
                  b.use(layers, Use::SrvCompute);
                  if (tlut.valid()) b.use(tlut, Use::SrvCompute);
                  if (useBuf.valid()) b.use(useBuf, Use::UavCompute);
              },
              [=](PassContext& ctx) {
                  const uint32_t k[24] = { ctx.srv(depth), ctx.srv(gbuffer), ctx.uav(out), ring, overflowList ? ctx.uav(overflowTiles) : 0xFFFFFFFFu,
                                           ctx.srv(table), ctx.srv(atlas), ctx.srv(bound), ctx.uav(list), ctx.srv(blocks), ctx.uav(statsBuf), 0,
                                           localSlots ? ctx.srv(froxelLists) : 0xFFFFFFFFu, localLightsSrv, slotOfSrv,
                                           overflowList ? ctx.uav(heads) : 0xFFFFFFFFu,
                                           mirrorMask.valid() ? ctx.srv(mirrorMask) : 0xFFFFFFFFu, mirrorTiles.valid() ? ctx.srv(mirrorTiles) : 0xFFFFFFFFu,
                                           ctx.srv(layers), tlut.valid() ? ctx.srv(tlut) : 0xFFFFFFFFu,
                                           overflowList ? ctx.uav(needs) : 0xFFFFFFFFu, tilesX, contact, 0 };
                  ctx.cmd->SetPipelineState(p1);
                  ctx.bindFrameConstants(constants);
                  ctx.computeConstants(k, 24);
                  ctx.cmd->Dispatch(groups(w, 8), groups(h, 8), 1);
              });
    chain.flush("s.shadow.visibility");
    g.addPass("s.shadow.listargs", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(list, Use::SrvCompute);
                  b.use(args, Use::UavCompute);
                  b.use(filterList, Use::UavCompute);
              },
              [=](PassContext& ctx) {
                  const uint32_t k[4] = { ctx.srv(list), ctx.uav(args), ctx.uav(filterList), 0 };
                  ctx.cmd->SetPipelineState(pa);
                  ctx.computeConstants(k, 4);
                  ctx.cmd->Dispatch(1, 1, 1);
              });
    g.addPass("s.shadow.penumbra", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(depth, Use::SrvCompute);
                  b.use(gbuffer, Use::SrvCompute);
                  b.use(atlas, Use::SrvCompute);
                  b.use(table, Use::SrvCompute);
                  b.use(bound, Use::SrvCompute);
                  b.use(blocks, Use::SrvCompute);
                  b.use(list, Use::SrvCompute);
                  b.use(args, Use::IndirectArgs);
                  b.use(filterList, Use::UavCompute);
                  b.use(statsBuf, Use::UavCompute);
                  b.use(out, Use::UavCompute);
                  b.use(layers, Use::SrvCompute);
                  if (tlut.valid()) b.use(tlut, Use::SrvCompute);
                  if (useBuf.valid()) b.use(useBuf, Use::UavCompute);
              },
              [=](PassContext& ctx) {
                  const uint32_t k[16] = { ctx.srv(depth), ctx.srv(gbuffer), ctx.uav(out), ring, contact, ctx.srv(table), ctx.srv(atlas), ctx.srv(bound),
                                           ctx.srv(list), ctx.srv(blocks), ctx.uav(statsBuf), ctx.uav(filterList), rays, steps, ctx.srv(layers),
                                           tlut.valid() ? ctx.srv(tlut) : 0xFFFFFFFFu };
                  ctx.cmd->SetPipelineState(p2);
                  ctx.bindFrameConstants(constants);
                  ctx.computeConstants(k, 16);
                  ctx.cmd->ExecuteIndirect(signature, 1, ctx.resource(args), 0, nullptr, 0);
              });
    // The penumbra filter over the pixels the search left (ShadowPenumbra STAGE=1, full waves).
    if (p3)
    {
        g.addPass("s.shadow.filterargs", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(filterList, Use::SrvCompute);
                      b.use(filterArgs, Use::UavCompute);
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[4] = { ctx.srv(filterList), ctx.uav(filterArgs), 0xFFFFFFFFu, 0 };
                      ctx.cmd->SetPipelineState(pa);
                      ctx.computeConstants(k, 4);
                      ctx.cmd->Dispatch(1, 1, 1);
                  });
        g.addPass("s.shadow.penumbra.filter", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(depth, Use::SrvCompute);
                      b.use(gbuffer, Use::SrvCompute);
                      b.use(atlas, Use::SrvCompute);
                      b.use(table, Use::SrvCompute);
                      b.use(bound, Use::SrvCompute);
                      b.use(blocks, Use::SrvCompute);
                      b.use(filterList, Use::SrvCompute);
                      b.use(filterArgs, Use::IndirectArgs);
                      b.use(out, Use::UavCompute);
                      b.use(layers, Use::SrvCompute);
                      if (tlut.valid()) b.use(tlut, Use::SrvCompute);
                      if (useBuf.valid()) b.use(useBuf, Use::UavCompute);
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[16] = { ctx.srv(depth), ctx.srv(gbuffer), ctx.uav(out), ring, contact, ctx.srv(table), ctx.srv(atlas), ctx.srv(bound),
                                               ctx.srv(filterList), ctx.srv(blocks), 0xFFFFFFFFu, 0xFFFFFFFFu, rays, steps, ctx.srv(layers),
                                               tlut.valid() ? ctx.srv(tlut) : 0xFFFFFFFFu };
                      ctx.cmd->SetPipelineState(p3);
                      ctx.bindFrameConstants(constants);
                      ctx.computeConstants(k, 16);
                      ctx.cmd->ExecuteIndirect(signature, 1, ctx.resource(filterArgs), 0, nullptr, 0);
                  });
    }
    // The hair's shadow on the view's surfaces (shading.hair_shadows; Passes/Hair/HairShadow.hlsl MODE 0): the sun slot
    // times what the frame's grooms let through towards the sun, from E's density volume, after the passes that write
    // the slot - and, where S fills the local slots (no shading.mega_lights), slots 1..3 times what they let through
    // towards each slot's light. A frame without a volume records nothing; the hair records are shaded with the hair in
    // front of them by M (the fragment visibility below stays the opaque casters': M multiplies the cluster fragments'
    // by HairShadow MODE 2's profile).
    {
        const FrameResources hr = fc.resources;
        const bool hairShadows = !fc.quality.has("shading.hair_shadows") || fc.quality.boolean("shading.hair_shadows");
        if (hairShadows && !s.debugPaths && hr.hairDensityParams.valid() && hr.hairDensity.valid() && hr.hairDensityCoarse.valid())
        {
            const uint32_t hairSteps = fc.quality.has("shading.hair_shadow_steps") ? (uint32_t)fc.quality.integer("shading.hair_shadow_steps") : 32u;
            if (hairSteps < 2 || hairSteps > 128) fail("shading.hair_shadow_steps must be in [2, 128]");
            const uint32_t hairJitter = !fc.quality.has("shading.hair_march_jitter") || fc.quality.boolean("shading.hair_march_jitter") ? 1u : 0u;
            const float3 originOffset = view.view.position - hr.hairOrigin;
            const BufferRef hairParams = hr.hairDensityParams;
            const TextureRef hairFine = hr.hairDensity, hairCoarse = hr.hairDensityCoarse;
            ID3D12PipelineState* ph = fc.shaders.compute("Passes/Hair/HairShadow.MODE0");
            g.addPass("s.shadow.hair", QueueType::Compute,
                      [&](PassBuilder& b) {
                          b.use(depth, Use::SrvCompute);
                          b.use(hairParams, Use::SrvCompute);
                          b.use(hairFine, Use::SrvCompute);
                          b.use(hairCoarse, Use::SrvCompute);
                          if (localSlots) b.use(froxelLists, Use::SrvCompute);
                          b.use(out, Use::UavCompute);
                      },
                      [=](PassContext& ctx) {
                          uint32_t k[12] = { ctx.srv(hairParams), ctx.srv(depth), ctx.uav(out), hairSteps, 0, 0, 0, hairJitter,
                                             localSlots ? ctx.srv(froxelLists) : 0xFFFFFFFFu, 0, 0, 0 };
                          const float o[3] = { originOffset.x, originOffset.y, originOffset.z };
                          std::memcpy(&k[4], o, 12);
                          ctx.cmd->SetPipelineState(ph);
                          ctx.bindFrameConstants(constants);
                          ctx.computeConstants(k, 12);
                          ctx.cmd->Dispatch(groups(w, 8), groups(h, 8), 1);
                      });
        }
    }
    // Fragment visibility of the coverage layer (S request 20260926_S_fragment_visibility, INTERFACES 7.3 v1.41): views
    // with V's coverage records. Pass 1 per listed tile (pixel depth ranges), pass 2 per block of records (pair pixels).
    if (view.coverageDepthRange.valid() && view.coverageTileList.valid() && view.coverageRecords.valid())
    {
        const BufferRef tileList = view.coverageTileList, records = view.coverageRecords;
        const TextureRef ranges = view.coverageDepthRange;
        const uint64_t recordCount = g.desc(records).size / 16;
        const BufferRef fragments = g.createBuffer(BufferDesc{ "S shadow fragment visibility", (uint64_t)w * h * 12, 12 });
        const BufferRef fragmentSun = g.createBuffer(BufferDesc{ "S shadow fragment sun", std::max<uint64_t>((recordCount + 3) / 4 * 4, 4), 0 });
        view.shadowFragmentVisibility = fragments;
        view.shadowFragmentSun = fragmentSun;
        ID3D12PipelineState* f0 = fc.shaders.compute("Passes/Shadow/ShadowFragments.MODE0");
        ID3D12PipelineState* f1 = fc.shaders.compute("Passes/Shadow/ShadowFragments.MODE1");
        auto words = [=](PassContext& ctx, bool second) {
            std::array<uint32_t, 20> k = { second ? 0u : ctx.srv(ranges), ctx.srv(tileList), ctx.srv(records), second ? ctx.srv(fragments) : ctx.uav(fragments),
                                           ctx.srv(table), ctx.srv(atlas), ctx.srv(bound), ctx.srv(blocks),
                                           ring, localLightsSrv, slotOfSrv, (localSlots && !second) ? ctx.srv(froxelLists) : 0xFFFFFFFFu,
                                           second ? ctx.uav(fragmentSun) : 0u, ctx.srv(layers), second ? 0u : ctx.srv(depth), second ? 0u : ctx.srv(gbuffer),
                                           ctx.uav(statsBuf), second ? ctx.srv(ranges) : 0u, tlut.valid() ? ctx.srv(tlut) : 0xFFFFFFFFu, 0 };
            return k;
        };
        g.addPass("s.shadow.fragments", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(ranges, Use::SrvCompute);
                      b.use(tileList, Use::SrvCompute);
                      b.use(tileList, Use::IndirectArgs);
                      b.use(records, Use::SrvCompute);
                      b.use(fragments, Use::UavCompute);
                      b.use(table, Use::SrvCompute);
                      b.use(atlas, Use::SrvCompute);
                      b.use(bound, Use::SrvCompute);
                      b.use(blocks, Use::SrvCompute);
                      b.use(layers, Use::SrvCompute);
                      b.use(depth, Use::SrvCompute);
                      b.use(gbuffer, Use::SrvCompute);
                      b.use(statsBuf, Use::UavCompute);
                      if (localSlots) b.use(froxelLists, Use::SrvCompute);
                      if (tlut.valid()) b.use(tlut, Use::SrvCompute);  // (the cloud layer's shadow on the fragments)
                  },
                  [=](PassContext& ctx) {
                      const auto k = words(ctx, false);
                      ctx.cmd->SetPipelineState(f0);
                      ctx.bindFrameConstants(constants);
                      ctx.computeConstants(k.data(), 20);
                      ctx.cmd->ExecuteIndirect(signature, 1, ctx.resource(tileList), 0, nullptr, 0);  // V's args over the listed tiles
                  });
        g.addPass("s.shadow.fragmentsun", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(tileList, Use::SrvCompute);
                      b.use(tileList, Use::IndirectArgs);
                      b.use(records, Use::SrvCompute);
                      b.use(fragments, Use::SrvCompute);
                      b.use(fragmentSun, Use::UavCompute);
                      b.use(ranges, Use::SrvCompute);
                      b.use(statsBuf, Use::UavCompute);
                      b.use(table, Use::SrvCompute);
                      b.use(atlas, Use::SrvCompute);
                      b.use(bound, Use::SrvCompute);
                      b.use(blocks, Use::SrvCompute);
                      b.use(layers, Use::SrvCompute);
                      if (tlut.valid()) b.use(tlut, Use::SrvCompute);
                  },
                  [=](PassContext& ctx) {
                      const auto k = words(ctx, true);
                      ctx.cmd->SetPipelineState(f1);
                      ctx.bindFrameConstants(constants);
                      ctx.computeConstants(k.data(), 20);
                      ctx.cmd->ExecuteIndirect(signature, 1, ctx.resource(tileList), 32, nullptr, 0);  // V's args over the record blocks
                  });
    }
    if (!overflowList) return;
    // Bounded dense filter queue. It changes scheduling only: excess items run
    // the same filter in the producer, so capacity never discards a light/tap.
    const bool splitOverflow = localSlots && (!fc.quality.has("shadow.vsm.overflow_filter_queue") || fc.quality.boolean("shadow.vsm.overflow_filter_queue"));
    BufferRef overflowFilter, overflowFilterArgs;
    if (splitOverflow)
    {
        const uint64_t pixels = (uint64_t)w * h;
        if (pixels * 29 >= (1ull << 32)) fail("overflow filter counter exceeds 32-bit bound"); // list max 32, first 3 in slots
        const uint32_t filterCapacity = (uint32_t)std::min<uint64_t>((uint64_t)capacity * 4, std::max<uint64_t>(1024, pixels / 8));
        overflowFilter = g.createBuffer({ "S local overflow filter queue", 16 + (uint64_t)filterCapacity * 48, 0 });
        overflowFilterArgs = g.createBuffer({ "S local overflow filter args", 16, 0 });
        ID3D12PipelineState* begin = fc.shaders.compute("Passes/Shadow/ShadowOverflowArgs.MODE0");
        g.addPass("s.shadow.overflow.begin", QueueType::Compute,
                  [&](PassBuilder& b) { b.use(overflowFilter, Use::UavCompute); },
                  [=](PassContext& ctx) {
                      const uint32_t k[4] = { ctx.uav(overflowFilter), 0, filterCapacity, 0 };
                      ctx.cmd->SetPipelineState(begin); ctx.computeConstants(k, 4); ctx.cmd->Dispatch(1, 1, 1);
                  });
    }
    // Overflow tiles (indirect, one group per listed tile; without local slots the list is empty and the dispatches have
    // no groups): count each tile's need, allocate in tile order (two-level prefix sum), then fill: the lights past the
    // third, one block per tile from the capacity.
    // shadow.vsm.fold_small_passes: a view without local shadow slots lists no tile - the visibility pass reads no froxel
    // list then, so no pixel has a light past the third, every tile's head is 0 from that pass and the fallback list is
    // empty from the clear - and the count, the scan and the fill, which would run over nothing, are not recorded.
    if (foldSmall && !localSlots) return;
    const uint32_t scanBlocks = (tiles + 2047) / 2048;
    g.addPass("s.shadow.overflow.count", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(depth, Use::SrvCompute);
                  b.use(overflowTiles, Use::SrvCompute);
                  b.use(overflowTiles, Use::IndirectArgs);
                  b.use(needs, Use::UavCompute);
                  b.use(statsBuf, Use::UavCompute);
                  if (localSlots) b.use(froxelLists, Use::SrvCompute);
                  if (mirrorMask.valid()) b.use(mirrorMask, Use::SrvCompute);
              },
              [=](PassContext& ctx) {
                  const uint32_t k[20] = { ctx.srv(depth), 0, 0, ring, ctx.srv(overflowTiles), 0, 0,
                                           0, 0, capacity, 0, ctx.uav(statsBuf),
                                           localSlots ? ctx.srv(froxelLists) : 0xFFFFFFFFu, localLightsSrv, slotOfSrv, ctx.uav(needs),
                                           mirrorMask.valid() ? ctx.srv(mirrorMask) : 0xFFFFFFFFu, 0xFFFFFFFFu, tilesX, 0 };
                  ctx.cmd->SetPipelineState(poCount);
                  ctx.bindFrameConstants(constants);
                  ctx.computeConstants(k, 20);
                  ctx.cmd->ExecuteIndirect(signature, 1, ctx.resource(overflowTiles), 4, nullptr, 0);
              });
    chain.add("s.shadow.overflow.scan.blocks",
              [&](PassBuilder& b) { b.use(needs, Use::UavCompute); b.use(blockSums, Use::UavCompute); },
              [=](PassContext& ctx) {
                  const uint32_t k[4] = { ctx.uav(needs), ctx.uav(blockSums), tiles, 0 };
                  ctx.cmd->SetPipelineState(poScan0);
                  ctx.computeConstants(k, 4);
                  ctx.cmd->Dispatch(scanBlocks, 1, 1);
              });
    chain.add("s.shadow.overflow.scan.top",
              [&](PassBuilder& b) { b.use(needs, Use::UavCompute); b.use(blockSums, Use::UavCompute); },
              [=](PassContext& ctx) {
                  const uint32_t k[4] = { ctx.uav(needs), ctx.uav(blockSums), scanBlocks, 0 };
                  ctx.cmd->SetPipelineState(poScan1);
                  ctx.computeConstants(k, 4);
                  ctx.cmd->Dispatch(1, 1, 1);
              });
    chain.flush("s.shadow.overflow.scan");
    g.addPass("s.shadow.overflow", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(depth, Use::SrvCompute);
                  b.use(gbuffer, Use::SrvCompute);
                  b.use(atlas, Use::SrvCompute);
                  b.use(table, Use::SrvCompute);
                  b.use(blocks, Use::SrvCompute);
                  b.use(overflowTiles, Use::SrvCompute);
                  b.use(overflowTiles, Use::IndirectArgs);
                  b.use(needs, Use::SrvCompute);
                  b.use(blockSums, Use::SrvCompute);
                  b.use(heads, Use::UavCompute);
                  b.use(overflow, Use::UavCompute);
                  b.use(fallback, Use::UavCompute);
                  b.use(statsBuf, Use::UavCompute);
                  if (splitOverflow) b.use(overflowFilter, Use::UavCompute);
                  if (localSlots) b.use(froxelLists, Use::SrvCompute);
                  if (mirrorMask.valid()) b.use(mirrorMask, Use::SrvCompute);
                  if (useBuf.valid()) b.use(useBuf, Use::UavCompute);
              },
              [=](PassContext& ctx) {
                  const uint32_t k[20] = { ctx.srv(depth), ctx.srv(gbuffer), ctx.uav(heads), ring, ctx.srv(overflowTiles), ctx.srv(table), ctx.srv(atlas),
                                           ctx.srv(blocks), ctx.uav(overflow), capacity, ctx.uav(fallback), ctx.uav(statsBuf),
                                           localSlots ? ctx.srv(froxelLists) : 0xFFFFFFFFu, localLightsSrv, slotOfSrv, ctx.srv(needs),
                                           mirrorMask.valid() ? ctx.srv(mirrorMask) : 0xFFFFFFFFu,
                                           splitOverflow ? ctx.uav(overflowFilter) : 0xFFFFFFFFu, tilesX, ctx.srv(blockSums) };
                  ctx.cmd->SetPipelineState(poFill);
                  ctx.bindFrameConstants(constants);
                  ctx.computeConstants(k, 20);
                  ctx.cmd->ExecuteIndirect(signature, 1, ctx.resource(overflowTiles), 4, nullptr, 0);
              });
    if (splitOverflow)
    {
        ID3D12PipelineState* makeArgs = fc.shaders.compute("Passes/Shadow/ShadowOverflowArgs.MODE1");
        ID3D12PipelineState* filter = fc.shaders.compute("Passes/Shadow/ShadowOverflowFilter");
        g.addPass("s.shadow.overflow.args", QueueType::Compute,
                  [&](PassBuilder& b) { b.use(overflowFilter, Use::UavCompute); b.use(overflowFilterArgs, Use::UavCompute); },
                  [=](PassContext& ctx) {
                      const uint32_t k[4] = { ctx.uav(overflowFilter), ctx.uav(overflowFilterArgs), 0, 0 };
                      ctx.cmd->SetPipelineState(makeArgs); ctx.computeConstants(k, 4); ctx.cmd->Dispatch(1, 1, 1);
                  });
        g.addPass("s.shadow.overflow.filter", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(overflowFilter, Use::SrvCompute); b.use(overflowFilterArgs, Use::IndirectArgs);
                      b.use(overflow, Use::UavCompute); b.use(depth, Use::SrvCompute); b.use(gbuffer, Use::SrvCompute);
                      b.use(table, Use::SrvCompute); b.use(atlas, Use::SrvCompute); b.use(blocks, Use::SrvCompute);
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[12] = { ctx.srv(overflowFilter), ctx.uav(overflow), ctx.srv(depth), ctx.srv(gbuffer),
                                               ctx.srv(table), ctx.srv(atlas), ctx.srv(blocks), ring, localLightsSrv, 0, 0, 0 };
                      ctx.cmd->SetPipelineState(filter); ctx.bindFrameConstants(constants); ctx.computeConstants(k, 12);
                      ctx.cmd->ExecuteIndirect(signature, 1, ctx.resource(overflowFilterArgs), 0, nullptr, 0);
                  });
    }
}
} // namespace unx::render::shadow
