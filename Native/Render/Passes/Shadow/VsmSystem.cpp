#include "VsmSystem.h"

#include "FroxelSystem.h"
#include "SResources.h"

#include "unx/render/GpuScene.h"
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
constexpr uint32_t kStatsSlots = 4, kStatsBytes = 256;  // VSM stats words (VsmBegin clears them)
constexpr uint64_t kOverflowMinWords = 1u << 18;  // 1 MB overflow list at least (INTERFACES 7.3)
constexpr uint32_t kMetaBytes = 48;  // VsmPageMeta
constexpr uint32_t kBlockBytes = 341 * 32;  // VSM_BLOCK_ENTRIES x VsmBlock

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
    uint32_t ringCbv[kRingSlots] = {};  // constant buffer view of each ring slot (ConstantBuffer<VsmConstants>)
    uint8_t* ringMapped = nullptr;
    // Stats readback ring: slot i holds the counters of frame statsFrame[i], complete once the graphics queue passes
    // statsFence[i] (the fence of the frame after it was recorded is known at the next record).
    ComPtr<ID3D12Resource> statsReadback;
    uint64_t statsFrame[kStatsSlots] = {};
    uint64_t statsFence[kStatsSlots] = {};
    int lastStatsSlot = -1;
    uint32_t errorBitsSeen = 0;  // OR of the error words of every harvested frame (INTERFACES 3.6)
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
    uint8_t* localMapped = nullptr;
    uint32_t localStride = 0, localCap = 0;
    uint32_t localLightsSrv[kRingSlots] = {}, localSlotOfSrv[kRingSlots] = {}, localActiveSrv[kRingSlots] = {};
    uint32_t localLightsNow = UINT32_MAX, slotOfNow = UINT32_MAX, activeNow = UINT32_MAX;
};

constexpr uint32_t kAtlasPagesPerRow = 128;                                        // VsmCommon.hlsli VSM_ATLAS_PAGES_PER_ROW
constexpr uint32_t kLocalFaceWords = 173, kLocalLightWords = 6 * kLocalFaceWords;  // VsmLocal.hlsli
constexpr uint32_t kLocalViewWordOffset[kLocalMips] = { 0, 1, 2, 3, 5, 13, 45 };
constexpr uint32_t kScanGroupSlots = 1024;  // VsmScan.hlsl
constexpr uint32_t kScanGroupsMax = (kTotalSlots + kScanGroupSlots - 1) / kScanGroupSlots;

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
void createState(FramePassContext& fc, State& s, uint32_t pages)
{
    const QualityConfig& q = fc.quality;
    if (q.integer("shadow.vsm.virtual_resolution") != kVirtual || q.integer("shadow.vsm.page_texels") != kPage || q.integer("shadow.vsm.clipmap_levels") != kLevels)
        fail("shadow.vsm: virtual_resolution / page_texels / clipmap_levels are compiled into the S kernels (16384 / 128 / 20)");
    if (pages == 0 || pages % kAtlasPagesPerRow || pages / kAtlasPagesPerRow * kPage > 16384)
        fail("VSM atlas: %u pages is not a positive multiple of %u within one 16384^2 atlas", pages, kAtlasPagesPerRow);
    Device& d = fc.device;
    for (ComPtr<ID3D12Resource>* r : { std::addressof(s.atlas), std::addressof(s.meta), std::addressof(s.blocks), std::addressof(s.pageList), std::addressof(s.layers) })
        if (*r) d.deferRelease(*r);
    s.atlasPages = pages;
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
    // Transmittance layer: the per-page words (0 = no layer) until V's coverage-mode raster fills layer pages (v1.26).
    s.layersBytes = ((uint64_t)pages * 4 + 255) & ~255ull;
    s.layers = createBuffer(d, L"S VSM transmittance layer", s.layersBytes);
    if (!s.table)
    {
        s.table = createBuffer(d, L"S VSM page table", (uint64_t)kTotalSlots * 8);
        s.requests = createBuffer(d, L"S VSM requests", (uint64_t)kTotalSlots * 4);
        s.localMask = createBuffer(d, L"S VSM local cull mask", (uint64_t)kLocalLights * kLocalLightWords * 4);
        s.localSlots = createBuffer(d, L"S VSM local atlas slots", (uint64_t)kLocalLights * kLocalLightWords * 32 * 4);
        s.cullMask = createBuffer(d, L"S VSM cull mask", (uint64_t)kSlots / 8);
        s.atlasSlots = createBuffer(d, L"S VSM atlas slots", (uint64_t)kSlots * 4);
        s.groups = createBuffer(d, L"S VSM scan groups", (uint64_t)kScanGroupsMax * 4);
        s.args = createBuffer(d, L"S VSM indirect args", 16);
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
    return true;
}

namespace
{
// Local lights of this frame: shadow slots (persistent while a light keeps casting; a slot's generation changes when its
// light changes or moves, which releases its pages), the lights meeting the main view (raster views), and the uploads
// (local lights, scene light -> slot, active slots) in this frame's ring slice.
void updateLocalLights(FramePassContext& fc, State& s, const ViewResources& main)
{
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
        if (li - 1 < n && lights[li - 1].castShadow) s.slotOfLight[li - 1] = i;
        else
        {
            s.localLight[i] = 0;
            ++s.localGen[i];
        }
    }
    s.localWithoutSlot = 0;
    uint32_t freeSlot = 0;
    for (uint32_t li = 0; li < n; ++li)
    {
        if (!lights[li].castShadow || s.slotOfLight[li] != 0xFFFFu) continue;
        while (freeSlot < kLocalLights && s.localLight[freeSlot] != 0) ++freeSlot;
        if (freeSlot == kLocalLights)
        {
            ++s.localWithoutSlot;
            continue;
        }
        s.localLight[freeSlot] = li + 1;
        ++s.localGen[freeSlot];
        s.slotOfLight[li] = freeSlot;
    }
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
        s.localUsed = i + 1;
        const float3 eye = main.view.position;
        const float3 toLight = { d.position.x - eye.x, d.position.y - eye.y, d.position.z - eye.z };
        if (dot(toLight, toLight) <= d.farM * d.farM || sphereInView(main.view.viewProj, d.position, d.farM)) s.localActive.push_back(i);
    }
    s.latest.localAssigned = 0;
    for (uint32_t i = 0; i < kLocalLights; ++i) s.latest.localAssigned += s.localLight[i] != 0 ? 1u : 0u;
    s.latest.localActive = (uint32_t)s.localActive.size();
    s.latest.localWithoutSlot = s.localWithoutSlot;

    // Upload ring: [local lights 128 x 48 B][active slots 128 x 4 B][scene light -> slot, cap x 4 B].
    const uint32_t cap = std::max(n, 1u);
    if (!s.localRing || cap > s.localCap)
    {
        if (s.localRing) fc.device.deferRelease(s.localRing);
        s.localCap = std::max(cap, 256u);
        s.localStride = (kLocalLights * 48 + kLocalLights * 4 + s.localCap * 4 + 255) & ~255u;
        s.localRing = createBuffer(fc.device, L"S VSM local lights ring", (uint64_t)kRingSlots * s.localStride, D3D12_HEAP_TYPE_UPLOAD);
        D3D12_RANGE nothing{ 0, 0 };
        check(s.localRing->Map(0, &nothing, reinterpret_cast<void**>(&s.localMapped)), "map VSM local ring");
        DescriptorHeaps& h = fc.device.descriptors();
        for (uint32_t r = 0; r < kRingSlots; ++r)
        {
            auto view = [&](uint64_t offset, uint32_t elements, uint32_t stride) {
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
        if (s.statsFence[i] == 0 || s.statsFence[i] > completed || s.statsFrame[i] <= s.latest.frame) continue;
        uint32_t* p = nullptr;
        D3D12_RANGE r{ i * kStatsBytes, i * kStatsBytes + kStatsBytes };
        check(s.statsReadback->Map(0, &r, reinterpret_cast<void**>(&p)), "map VSM stats");
        const uint32_t* w = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(p) + i * kStatsBytes);
        const VsmStats keep = s.latest;
        s.latest = { s.statsFrame[i], w[0], w[1], w[2], w[3], 0, w[5], w[8], w[9], w[10], w[11], w[12], w[13], w[14] };
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
    // Local lights first: their count sizes the atlas's initial budget with the view's pixels.
    updateLocalLights(fc, s, main);
    const double mpixels = (double)main.view.width * main.view.height / 1e6;
    const double budget = mpixels * q.number("shadow.vsm.pool_pages_per_mpixel") + s.latest.localAssigned * q.number("shadow.vsm.pool_pages_per_local_light");
    uint32_t pages = std::max((uint32_t)q.integer("shadow.vsm.pool_pages"), (uint32_t)budget);
    pages = (std::max(pages, s.poolTarget) + kAtlasPagesPerRow - 1) / kAtlasPagesPerRow * kAtlasPagesPerRow;
    pages = std::min(pages, 16384u / kPage * kAtlasPagesPerRow);  // one 16384^2 atlas (16,384 pages)
    if (!s.atlas || s.atlasPages < pages) createState(fc, s, pages);

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
    {
        float lo, hi;
        casterHeightRange(fc.scene, sunDir, lo, hi);
        c.hMin = lo - margin;
        c.hMax = hi + margin;
    }
    c.tanSunRadius = tanSun;
    c.poolPagesX = kAtlasPagesPerRow;
    c.poolPagesY = s.atlasPages / kAtlasPagesPerRow;
    c.frame = (uint32_t)++s.frames;
    c.time = (float)fc.frame.time;
    c.lodBias = (float)q.number("shadow.vsm.lod_bias");
    c.receiverBiasTexels = (float)q.number("shadow.vsm.receiver_bias_texels");
    c.maxReceiverSlope = (float)q.number("shadow.vsm.max_receiver_slope");
    c.instanceCount = (uint32_t)fc.scene.instances().size();
    c.searchTaps = (uint32_t)q.integer("shadow.vsm.search_taps");
    c.filterTaps = (uint32_t)q.integer("shadow.vsm.filter_taps");
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
    recordFroxelLists(fc, main, slotOfSrv);
    const BufferRef froxelLists = fc.resources.froxelLights;
    const uint32_t pagesNow = s.atlasPages;
    const BufferRef meta = g.importBuffer(s.meta.Get(), BufferDesc{ "S VSM page metadata", (uint64_t)pagesNow * kMetaBytes, kMetaBytes });
    const BufferRef blocks = g.importBuffer(s.blocks.Get(), BufferDesc{ "S VSM page blocks", (uint64_t)pagesNow * kBlockBytes, 0 });
    s.blocksRef = blocks;
    const BufferRef pageList = g.importBuffer(s.pageList.Get(), BufferDesc{ "S VSM page list", 8 + (uint64_t)pagesNow * 8, 0 });
    const BufferRef layers = g.importBuffer(s.layers.Get(), BufferDesc{ "S VSM transmittance layer", s.layersBytes, 0 });
    s.layersRef = layers;
    fc.resources.vsmLayers = layers;  // v1.26: ShadowSrvs.layers (the pad1 word)
    const BufferRef mask = g.importBuffer(s.cullMask.Get(), BufferDesc{ "S VSM cull mask", (uint64_t)kSlots / 8, 0 });
    const BufferRef atlasSlots = g.importBuffer(s.atlasSlots.Get(), BufferDesc{ "S VSM atlas slots", (uint64_t)kSlots * 4, 0 });
    const BufferRef scanGroupsBuf = g.importBuffer(s.groups.Get(), BufferDesc{ "S VSM scan groups", (uint64_t)kScanGroupsMax * 4, 0 });
    const BufferRef args = g.importBuffer(s.args.Get(), BufferDesc{ "S VSM indirect args", 16, 0 });
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
        g.addPass("s.vsm.begin", QueueType::Compute,
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
    if (main.depth.valid())
    {
        ID3D12PipelineState* pso = sh.compute("Passes/Shadow/VsmMark");
        const bool subtileStats = q.integer("shadow.vsm.subtile_stats") != 0;  // measurement only
        const TextureRef depth = main.depth;
        const uint32_t w = main.view.width, h = main.view.height;
        g.addPass("s.vsm.mark", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(depth, Use::SrvCompute);
                      b.use(requests, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[4] = { ctx.srv(depth), ctx.uav(requests), ring, subtileStats ? 1u : 0u };
                      ctx.cmd->SetPipelineState(pso);
                      ctx.bindFrameConstants(mainConstants);
                      ctx.computeConstants(k, 4);
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
        g.addPass("s.vsm.markair", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(requests, Use::UavCompute);
                      b.use(statsBuf, Use::UavCompute);  // error word (INTERFACES 3.6)
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[8] = { ctx.uav(requests), ring, grid.gridX | grid.gridY << 16, grid.slices | grid.tilePx << 16, nearBits, farBits, texelBits,
                                              ctx.uav(statsBuf) };
                      ctx.cmd->SetPipelineState(pso);
                      ctx.bindFrameConstants(mainConstants);
                      ctx.computeConstants(k, 8);
                      ctx.cmd->Dispatch(grid.gridX, grid.gridY, 1);
                  });
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
    {
        ID3D12PipelineState* pso = sh.compute("Passes/Shadow/VsmPropagate");
        g.addPass("s.vsm.propagate", QueueType::Compute,
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
    ID3D12CommandSignature* signature = s.dispatchSignature.Get();
    {
        // Deterministic page assignment (VsmScan): count per group, prefix, assign.
        ID3D12PipelineState* p0 = sh.compute("Passes/Shadow/VsmScan.MODE0");
        ID3D12PipelineState* p1 = sh.compute("Passes/Shadow/VsmScan.MODE1");
        ID3D12PipelineState* p2 = sh.compute("Passes/Shadow/VsmScan.MODE2");
        auto words = [=](PassContext& ctx, uint32_t k[12]) {
            const uint32_t w[12] = { ctx.uav(requests), ctx.uav(table), ctx.uav(scanGroupsBuf), ctx.uav(statsBuf), scanSlots, pagesNow, ring, scanGroups,
                                     ctx.uav(pageList), ctx.uav(meta), localLightsSrv, ctx.uav(args) };
            std::memcpy(k, w, sizeof w);
        };
        g.addPass("s.vsm.scan.count", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(requests, Use::UavCompute);
                      b.use(scanGroupsBuf, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      uint32_t k[12];
                      const uint32_t w[12] = { ctx.uav(requests), 0, ctx.uav(scanGroupsBuf), 0, scanSlots, pagesNow, ring, scanGroups, 0, 0, 0, 0 };
                      std::memcpy(k, w, sizeof w);
                      ctx.cmd->SetPipelineState(p0);
                      ctx.computeConstants(k, 12);
                      ctx.cmd->Dispatch(scanGroups, 1, 1);
                  });
        g.addPass("s.vsm.scan.prefix", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(scanGroupsBuf, Use::UavCompute);
                      b.use(statsBuf, Use::UavCompute);
                      b.use(pageList, Use::UavCompute);
                      b.use(args, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[12] = { 0, 0, ctx.uav(scanGroupsBuf), ctx.uav(statsBuf), scanSlots, pagesNow, ring, scanGroups, ctx.uav(pageList), 0, 0,
                                               ctx.uav(args) };
                      ctx.cmd->SetPipelineState(p1);
                      ctx.computeConstants(k, 12);
                      ctx.cmd->Dispatch(1, 1, 1);
                  });
        g.addPass("s.vsm.scan.assign", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(requests, Use::UavCompute);
                      b.use(table, Use::UavCompute);
                      b.use(scanGroupsBuf, Use::UavCompute);
                      b.use(statsBuf, Use::UavCompute);
                      b.use(pageList, Use::UavCompute);
                      b.use(meta, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      uint32_t k[12];
                      words(ctx, k);
                      ctx.cmd->SetPipelineState(p2);
                      ctx.computeConstants(k, 12);
                      ctx.cmd->Dispatch(scanGroups, 1, 1);
                  });
    }
    {
        ID3D12PipelineState* pm = sh.compute("Passes/Shadow/VsmCullMask");
        g.addPass("s.vsm.cullmask", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(table, Use::SrvCompute);
                      b.use(mask, Use::UavCompute);
                      b.use(atlasSlots, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[4] = { ctx.srv(table), ctx.uav(mask), ring, ctx.uav(atlasSlots) };
                      ctx.cmd->SetPipelineState(pm);
                      ctx.computeConstants(k, 4);
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
    g.addPass("s.vsm.clear", QueueType::Graphics, [&](PassBuilder& b) { b.use(atlas, Use::DepthWrite); },
              [=](PassContext& ctx) { ctx.cmd->ClearDepthStencilView(ctx.dsv(atlas), D3D12_CLEAR_FLAG_DEPTH, 0.0f, 0, 0, nullptr); });

    // Every requested page through V's cluster pipeline into its atlas slot (tile atlas, hardware depth, no pixel
    // kernel): one orthographic view per level over its whole window; the tile mask limits the raster to the pages
    // with a slot this frame.
    if (fc.services.rasterizeDepth)
    {
        DepthRasterRequest r;
        r.name = "s.vsm.raster";
        r.instanceMask = scene::InstanceCastShadow;
        r.depthTarget = atlas;
        r.atlasSlots = atlasSlots;
        r.atlasTilesPerRow = kAtlasPagesPerRow;
        r.cullMask = mask;
        r.cullTilePx = kPage;
        r.tileLocal = true;
        r.cull = D3D12_CULL_MODE_NONE;
        for (uint32_t k = 0; k < kLevels; ++k)
        {
            RasterView v;
            v.viewProj = levelViewProj(c, k);
            v.viewportX = v.viewportY = 0;
            v.viewportWidth = v.viewportHeight = kVirtual;
            v.lodPixelsPerMetre = 1.0f / std::ldexp(1.0f, (int)k - 10);
            v.userData = k;
            v.cullMaskOffset = k * (kTable * kTable / 32);
            r.views.push_back(v);
        }
        fc.services.rasterizeDepth(fc, r);
    }
    if (fc.services.rasterizeDepth && activeLocal > 0)
    {
        // Local lights: the (light, face, mip) views of 6 lights per request (42 views each, V's 255-view limit); each
        // view's viewport is its mip's resolution, the tile masks and slots are packed per light (VsmLocalCullMask).
        for (uint32_t first = 0; first < activeLocal; first += 6)
        {
            DepthRasterRequest r;
            r.name = "s.vsm.localraster" + std::to_string(first / 6);
            r.instanceMask = scene::InstanceCastShadow;
            r.depthTarget = atlas;
            r.atlasSlots = localSlots;
            r.atlasTilesPerRow = kAtlasPagesPerRow;
            r.cullMask = localMask;
            r.cullTilePx = kPage;
            r.tileLocal = true;
            r.cull = D3D12_CULL_MODE_NONE;
            for (uint32_t a = first; a < std::min<uint32_t>(first + 6, activeLocal); ++a)
            {
                const uint32_t slot = s.localActive[a];
                const VsmLocalLightCpu& l = s.localData[slot];
                for (uint32_t face = 0; face < 6; ++face)
                    for (uint32_t mip = 0; mip < kLocalMips; ++mip)
                    {
                        RasterView v;
                        v.viewProj = localViewProj(l, face);
                        v.viewportX = v.viewportY = 0;
                        v.viewportWidth = v.viewportHeight = kPage << mip;
                        v.lodPixelsPerMetre = 0.5f * (float)(kPage << mip);  // focal length in texels (90 degree face)
                        v.userData = slot | face << 7 | mip << 10;
                        v.cullMaskOffset = a * kLocalLightWords + face * kLocalFaceWords + kLocalViewWordOffset[mip];
                        r.views.push_back(v);
                    }
            }
            fc.services.rasterizeDepth(fc, r);
        }
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
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[8] = { ctx.srv(pageList), ctx.srv(atlas), ctx.uav(meta), ring, localLightsSrv, ctx.uav(blocks), 0, 0 };
                      ctx.cmd->SetPipelineState(pso);
                      ctx.computeConstants(k, 8);
                      ctx.cmd->ExecuteIndirect(signature, 1, ctx.resource(args), 0, nullptr, 0);
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
    const BufferRef froxelLists = view.froxelLights.valid() ? view.froxelLights : (mainView ? fc.resources.froxelLights : BufferRef{});
    const bool localSlots = froxelLists.valid() && s.localLightsNow != UINT32_MAX;
    const uint32_t localLightsSrv = s.localLightsNow, slotOfSrv = s.slotOfNow;
    const BufferRef table = s.tableRef, bound = s.boundRef, blocks = s.blocksRef, statsBuf = s.statsRef, layers = s.layersRef, useBuf = s.useRef;
    // Overflow list (INTERFACES 7.3, v1.20): the main view's shadow-casting lights past the third. Capacity = 1.5 x the
    // need of the last completed frame (the counter keeps counting past the capacity, so an overage frame reports its
    // full need), a power of two of words, at least 1 MB; shrinks only below a quarter (no plan churn around a boundary).
    const bool overflowList = true;  // every view (its own lists; empty without local slots)
    const uint32_t tilesX = groups(w, 8), tilesY = groups(h, 8), tiles = tilesX * tilesY;
    TextureRef heads;
    BufferRef overflow, overflowTiles, fallback, counter;
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
        counter = g.createBuffer(BufferDesc{ "S shadow overflow counter", 16, 0 });
        view.shadowOverflowTiles = heads;
        view.shadowOverflow = overflow;
        view.shadowOverflowFallbackTiles = fallback;
    }
    const uint32_t ring = s.ringCbv[s.constantsOffset / kRingStride], off = 0;
    const uint32_t rays = (uint32_t)fc.quality.integer("shadow.vsm.search_taps"), steps = (uint32_t)fc.quality.integer("shadow.vsm.filter_taps");
    const D3D12_GPU_VIRTUAL_ADDRESS constants = view.frameConstants;
    const char* variant = s.debugPaths ? ".PATHS1" : ".PATHS0";
    ID3D12PipelineState* pc = fc.shaders.compute("Passes/Shadow/ShadowListClear");
    const TextureRef tlut = fc.resources.transmittanceLut;  // B5 cloud shadows (the atmosphere record names the cloud record)
    ID3D12PipelineState* p1 = fc.shaders.compute(std::string("Passes/Shadow/ShadowVisibility") + variant);
    ID3D12PipelineState* pa = fc.shaders.compute("Passes/Shadow/ShadowListArgs");
    ID3D12PipelineState* p2 = fc.shaders.compute(std::string("Passes/Shadow/ShadowPenumbra") + variant);
    ID3D12PipelineState* po = overflowList ? fc.shaders.compute("Passes/Shadow/ShadowOverflow") : nullptr;
    ID3D12CommandSignature* signature = s.dispatchSignature.Get();
    // Pass 1 settles the pixels the page structures decide; the mixed ones go to a list for pass 2 (indirect).
    const BufferRef list = g.createBuffer(BufferDesc{ "S penumbra list", 4 + (uint64_t)w * h * 4, 0 });
    const BufferRef args = g.createBuffer(BufferDesc{ "S penumbra args", 16, 0 });
    g.addPass("s.shadow.listclear", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(list, Use::UavCompute);
                  if (overflowList)
                  {
                      b.use(overflowTiles, Use::UavCompute);
                      b.use(fallback, Use::UavCompute);
                      b.use(counter, Use::UavCompute);
                  }
              },
              [=](PassContext& ctx) {
                  const uint32_t k[4] = { ctx.uav(list), overflowList ? ctx.uav(overflowTiles) : 0xFFFFFFFFu, overflowList ? ctx.uav(fallback) : 0xFFFFFFFFu,
                                          overflowList ? ctx.uav(counter) : 0xFFFFFFFFu };
                  ctx.cmd->SetPipelineState(pc);
                  ctx.computeConstants(k, 4);
                  ctx.cmd->Dispatch(1, 1, 1);
              });
    g.addPass("s.shadow.visibility", QueueType::Compute,
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
                  }
                  if (mirrorMask.valid()) b.use(mirrorMask, Use::SrvCompute);
                  if (mirrorTiles.valid()) b.use(mirrorTiles, Use::SrvCompute);
                  b.use(layers, Use::SrvCompute);
                  if (tlut.valid()) b.use(tlut, Use::SrvCompute);
                  if (useBuf.valid()) b.use(useBuf, Use::UavCompute);
              },
              [=](PassContext& ctx) {
                  const uint32_t k[20] = { ctx.srv(depth), ctx.srv(gbuffer), ctx.uav(out), ring, overflowList ? ctx.uav(overflowTiles) : 0xFFFFFFFFu,
                                           ctx.srv(table), ctx.srv(atlas), ctx.srv(bound), ctx.uav(list), ctx.srv(blocks), ctx.uav(statsBuf), 0,
                                           localSlots ? ctx.srv(froxelLists) : 0xFFFFFFFFu, localLightsSrv, slotOfSrv,
                                           overflowList ? ctx.uav(heads) : 0xFFFFFFFFu,
                                           mirrorMask.valid() ? ctx.srv(mirrorMask) : 0xFFFFFFFFu, mirrorTiles.valid() ? ctx.srv(mirrorTiles) : 0xFFFFFFFFu,
                                           ctx.srv(layers), tlut.valid() ? ctx.srv(tlut) : 0xFFFFFFFFu };
                  ctx.cmd->SetPipelineState(p1);
                  ctx.bindFrameConstants(constants);
                  ctx.computeConstants(k, 20);
                  ctx.cmd->Dispatch(groups(w, 8), groups(h, 8), 1);
              });
    g.addPass("s.shadow.listargs", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(list, Use::SrvCompute);
                  b.use(args, Use::UavCompute);
              },
              [=](PassContext& ctx) {
                  const uint32_t k[4] = { ctx.srv(list), ctx.uav(args), 0, 0 };
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
                  b.use(statsBuf, Use::UavCompute);
                  b.use(out, Use::UavCompute);
                  b.use(layers, Use::SrvCompute);
                  if (tlut.valid()) b.use(tlut, Use::SrvCompute);
                  if (useBuf.valid()) b.use(useBuf, Use::UavCompute);
              },
              [=](PassContext& ctx) {
                  const uint32_t k[16] = { ctx.srv(depth), ctx.srv(gbuffer), ctx.uav(out), ring, off, ctx.srv(table), ctx.srv(atlas), ctx.srv(bound),
                                           ctx.srv(list), ctx.srv(blocks), ctx.uav(statsBuf), 0, rays, steps, ctx.srv(layers),
                                           tlut.valid() ? ctx.srv(tlut) : 0xFFFFFFFFu };
                  ctx.cmd->SetPipelineState(p2);
                  ctx.bindFrameConstants(constants);
                  ctx.computeConstants(k, 16);
                  ctx.cmd->ExecuteIndirect(signature, 1, ctx.resource(args), 0, nullptr, 0);
              });
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
                                           ctx.uav(statsBuf), second ? ctx.srv(ranges) : 0u, 0, 0 };
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
    // Overflow tiles: recount, one block per tile from the capacity, the lights past the third (indirect, one group per
    // listed tile; without local slots the list is empty and the dispatch has no groups).
    g.addPass("s.shadow.overflow", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(depth, Use::SrvCompute);
                  b.use(gbuffer, Use::SrvCompute);
                  b.use(atlas, Use::SrvCompute);
                  b.use(table, Use::SrvCompute);
                  b.use(blocks, Use::SrvCompute);
                  b.use(overflowTiles, Use::SrvCompute);
                  b.use(overflowTiles, Use::IndirectArgs);
                  b.use(heads, Use::UavCompute);
                  b.use(overflow, Use::UavCompute);
                  b.use(fallback, Use::UavCompute);
                  b.use(counter, Use::UavCompute);
                  b.use(statsBuf, Use::UavCompute);
                  if (localSlots) b.use(froxelLists, Use::SrvCompute);
                  if (mirrorMask.valid()) b.use(mirrorMask, Use::SrvCompute);
                  if (useBuf.valid()) b.use(useBuf, Use::UavCompute);
              },
              [=](PassContext& ctx) {
                  const uint32_t k[20] = { ctx.srv(depth), ctx.srv(gbuffer), ctx.uav(heads), ring, ctx.srv(overflowTiles), ctx.srv(table), ctx.srv(atlas),
                                           ctx.srv(blocks), ctx.uav(overflow), capacity, ctx.uav(fallback), ctx.uav(statsBuf),
                                           localSlots ? ctx.srv(froxelLists) : 0xFFFFFFFFu, localLightsSrv, slotOfSrv, ctx.uav(counter),
                                           mirrorMask.valid() ? ctx.srv(mirrorMask) : 0xFFFFFFFFu, 0, 0, 0 };
                  ctx.cmd->SetPipelineState(po);
                  ctx.bindFrameConstants(constants);
                  ctx.computeConstants(k, 20);
                  ctx.cmd->ExecuteIndirect(signature, 1, ctx.resource(overflowTiles), 4, nullptr, 0);
              });
}
} // namespace unx::render::shadow
