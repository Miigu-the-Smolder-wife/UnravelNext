#include "VsmSystem.h"

#include "FroxelSystem.h"
#include "SResources.h"

#include "unx/render/GpuScene.h"
#include "unx/render/Tracks.h"

#include <algorithm>
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
constexpr uint32_t kRingSlots = 16, kRingStride = 1024;  // per-frame constants; frames in flight must be < kRingSlots
constexpr uint32_t kStatsSlots = 4;
constexpr uint64_t kOverflowMinWords = 1u << 18;  // 1 MB overflow list at least (INTERFACES 7.3)
constexpr uint32_t kMetaBytes = 32;  // VsmPageMeta
constexpr uint32_t kBlockBytes = 341 * 32;  // VSM_BLOCK_ENTRIES x VsmBlock

struct State
{
    bool initialized = false;
    bool needsInit = false;  // pool (re)created: the next frame resets tables and free list
    uint32_t poolPagesX = 0, poolPagesY = 0;
    ComPtr<ID3D12Resource> pool, table, requests, meta, blocks, freeList, dirtyList, cullMask, args, stats, lastRevision, movedList, ring;
    ComPtr<ID3D12Resource> motion, jointCounts, jointStaging;  // dirty rule (a): per instance x level motion state; joints
    uint32_t jointsRevision = UINT32_MAX;
    bool jointsPending = false;
    ComPtr<ID3D12CommandSignature> dispatchSignature;
    uint32_t poolUav = UINT32_MAX, tableSrv = UINT32_MAX, ringSrv = UINT32_MAX, metaUav = UINT32_MAX;
    uint32_t ringCbv[kRingSlots] = {};  // constant buffer view of each ring slot (ConstantBuffer<VsmConstants>)
    uint32_t instanceCapacity = 0;
    uint8_t* ringMapped = nullptr;
    // Stats readback ring: slot i holds the counters of frame statsFrame[i], complete once the graphics queue passes
    // statsFence[i] (the fence of the frame after it was recorded is known at the next record).
    ComPtr<ID3D12Resource> statsReadback;
    uint64_t statsFrame[kStatsSlots] = {};
    uint64_t statsFence[kStatsSlots] = {};
    int lastStatsSlot = -1;
    VsmStats latest;
    // CPU view of the clipmap.
    float3 sun{};
    float3 windDirection{};
    float windSpeed = -1;
    uint32_t sceneRevision = UINT32_MAX;
    float hMin = 0, hMax = 0;
    uint64_t frames = 0;
    VsmConstantsCpu constants{};
    uint32_t constantsOffset = 0;
    // This frame's graph handles (valid between shadowPages and the end of the frame's recording).
    uint64_t recordedFrame = UINT64_MAX;
    BufferRef poolRef;
    BufferRef tableRef, metaRef, boundRef, blocksRef, statsRef;
    bool pagesRecorded = false;
    bool debugPaths = false;
    // Pool growth: a frame whose requests exhausted the pool sets the next size (never shrinks while running).
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
    ComPtr<ID3D12Resource> localRing, localMask;
    uint8_t* localMapped = nullptr;
    uint32_t localStride = 0, localCap = 0;
    uint32_t localLightsSrv[kRingSlots] = {}, localSlotOfSrv[kRingSlots] = {}, localActiveSrv[kRingSlots] = {};
    uint32_t localLightsNow = UINT32_MAX, slotOfNow = UINT32_MAX, activeNow = UINT32_MAX;
};

// Views of the page-count-dependent resources: new descriptor indices each time (frames in flight keep the old ones).
void createPoolViews(Device& device, State& s)
{
    DescriptorHeaps& h = device.descriptors();
    s.poolUav = h.allocateResource();
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.Format = DXGI_FORMAT_R32_TYPELESS;
    ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = s.poolPagesX * s.poolPagesY * kPage * kPage;
    ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
    device.d3d()->CreateUnorderedAccessView(s.pool.Get(), nullptr, &ud, h.resourceCpu(s.poolUav));
    s.metaUav = h.allocateResource();
    D3D12_UNORDERED_ACCESS_VIEW_DESC md{};
    md.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    md.Format = DXGI_FORMAT_UNKNOWN;
    md.Buffer.NumElements = s.poolPagesX * s.poolPagesY;
    md.Buffer.StructureByteStride = kMetaBytes;
    device.d3d()->CreateUnorderedAccessView(s.meta.Get(), nullptr, &md, h.resourceCpu(s.metaUav));
}

// Views of the resources created once (page table, constants ring).
void createFixedViews(Device& device, State& s)
{
    DescriptorHeaps& h = device.descriptors();
    s.tableSrv = h.allocateResource();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Format = DXGI_FORMAT_R32_TYPELESS;
    sd.Buffer.NumElements = (UINT)((uint64_t)kTotalSlots * 8 / 4);
    sd.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device.d3d()->CreateShaderResourceView(s.table.Get(), &sd, h.resourceCpu(s.tableSrv));
    for (uint32_t i = 0; i < kRingSlots; ++i)
    {
        s.ringCbv[i] = h.allocateResource();
        D3D12_CONSTANT_BUFFER_VIEW_DESC cd{ s.ring->GetGPUVirtualAddress() + (uint64_t)i * kRingStride, kRingStride };
        device.d3d()->CreateConstantBufferView(&cd, h.resourceCpu(s.ringCbv[i]));
    }
}

// Page-count-dependent resources (pool, metadata, blocks, free and dirty lists); the first call also creates the fixed
// ones. A regrown pool starts empty: the next frame resets the page table and free list (VsmInit) and re-renders.
void createState(FramePassContext& fc, State& s, uint32_t pages)
{
    const QualityConfig& q = fc.quality;
    if (q.integer("shadow.vsm.virtual_resolution") != kVirtual || q.integer("shadow.vsm.page_texels") != kPage || q.integer("shadow.vsm.clipmap_levels") != kLevels)
        fail("shadow.vsm: virtual_resolution / page_texels / clipmap_levels are compiled into the S kernels (16384 / 128 / 20)");
    const uint32_t perRow = 64;  // 8192 texels
    if (pages == 0 || pages % perRow) fail("VSM pool: %u pages is not a positive multiple of %u", pages, perRow);
    Device& d = fc.device;
    for (ComPtr<ID3D12Resource>* r : { std::addressof(s.pool), std::addressof(s.meta), std::addressof(s.blocks), std::addressof(s.freeList), std::addressof(s.dirtyList) })
        if (*r) d.deferRelease(*r);
    s.poolPagesX = perRow;
    s.poolPagesY = pages / perRow;
    s.pool = createBuffer(d, L"S VSM pool", (uint64_t)pages * kPage * kPage * 4);
    s.meta = createBuffer(d, L"S VSM page metadata", (uint64_t)pages * kMetaBytes);
    s.blocks = createBuffer(d, L"S VSM page blocks", (uint64_t)pages * kBlockBytes);
    s.freeList = createBuffer(d, L"S VSM free list", 4 + (uint64_t)pages * 4);
    s.dirtyList = createBuffer(d, L"S VSM dirty list", 8 + (uint64_t)pages * 8);
    createPoolViews(d, s);
    if (!s.table)
    {
        s.table = createBuffer(d, L"S VSM page table", (uint64_t)kTotalSlots * 8);
        s.requests = createBuffer(d, L"S VSM requests", (uint64_t)kTotalSlots * 4);
        s.localMask = createBuffer(d, L"S VSM local cull mask", (uint64_t)kLocalLights * kLocalViewsPerLight * 512 * 4);
        s.cullMask = createBuffer(d, L"S VSM cull mask", (uint64_t)kSlots / 8);
        s.args = createBuffer(d, L"S VSM indirect args", 48);  // dirty pages at 0, moved x levels at 16, moved x local lights at 32
        s.stats = createBuffer(d, L"S VSM stats", 80);
        s.ring = createBuffer(d, L"S VSM constants ring", (uint64_t)kRingSlots * kRingStride, D3D12_HEAP_TYPE_UPLOAD);
        s.statsReadback = createBuffer(d, L"S VSM stats readback", (uint64_t)kStatsSlots * 80, D3D12_HEAP_TYPE_READBACK);
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
    const float t = c.level[k].texel, V = (float)kVirtual, range = c.hMax - c.hMin;
    const float ox = (float)c.level[k].origin[0] * kPage, oy = (float)c.level[k].origin[1] * kPage;
    float4x4 m;
    m.m[0][0] = 2 * c.lightX.x / (t * V); m.m[0][1] = 2 * c.lightX.y / (t * V); m.m[0][2] = 2 * c.lightX.z / (t * V); m.m[0][3] = -2 * ox / V - 1;
    m.m[1][0] = -2 * c.lightY.x / (t * V); m.m[1][1] = -2 * c.lightY.y / (t * V); m.m[1][2] = -2 * c.lightY.z / (t * V); m.m[1][3] = 1 + 2 * oy / V;
    m.m[2][0] = -c.lightZ.x / range; m.m[2][1] = -c.lightZ.y / range; m.m[2][2] = -c.lightZ.z / range; m.m[2][3] = c.hMax / range;
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

// View-projection of a local face at a mip: the face's tangent square [-1, 1]^2 onto the top-left res x res pixels of
// the 16384^2 viewport (s = res / 16384), depth d = f (z - n) / ((f - n) z) (VsmLocalPixel.hlsl inverts it).
float4x4 localViewProj(const VsmLocalLightCpu& l, uint32_t face, uint32_t mip)
{
    float3 right, up, axis;
    cubeBasis(face, right, up, axis);
    const float sc = (float)(kPage << mip) / kVirtual;
    const float n = l.nearM, f = l.farM, A = f / (f - n), B = -f * n / (f - n);
    const float3 r0 = right * sc + axis * (sc - 1), r1 = up * sc + axis * (1 - sc), r2 = axis * A, r3 = axis;
    float4x4 m;
    const float3 rows[4] = { r0, r1, r2, r3 };
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
    out.pool = s.poolRef;
    out.table = s.tableRef;
    out.blocks = s.blocksRef;
    out.bound = s.boundRef;
    out.constantsCbv = s.ringCbv[s.constantsOffset / kRingStride];
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
        D3D12_RANGE r{ i * 80, i * 80 + 80 };
        check(s.statsReadback->Map(0, &r, reinterpret_cast<void**>(&p)), "map VSM stats");
        const uint32_t* w = reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(p) + i * 80);
        s.latest = { s.statsFrame[i], w[0], w[1], w[2], w[3], w[4], w[5], w[8], w[9], w[10], w[11], w[12], w[13], w[14] };
        s.latest.overflowWords = w[16];
        s.latest.overflowOverTiles = w[17];
        s.latest.overflowOverPixels = w[18];
        s.latest.overflowLights = w[19];
        D3D12_RANGE none{ 0, 0 };
        s.statsReadback->Unmap(0, &none);
    }

    // Pool size: shadow.vsm.pool_pages_per_mpixel of the main view (at least pool_pages), grown to 1.25 x the requests
    // of a completed frame that exhausted it, so requests are not dropped in steady state (a dropped page is a missing
    // shadow). The frame that exhausts renders without those pages; the frames after the growth have them.
    if (s.latest.exhausted > 0 && s.latest.frame > s.grownAtFrame)
    {
        const uint32_t current = s.poolPagesX * s.poolPagesY;
        s.poolTarget = std::max(current + 64, (uint32_t)((uint64_t)s.latest.requested * 5 / 4));
        s.grownAtFrame = s.frames + 1;  // stats of frames recorded with the old pool do not count
        logf("S VSM: %u page requests exhausted the %u-page pool (frame %llu): pool grows to %u pages\n", s.latest.exhausted, current,
             (unsigned long long)s.latest.frame, (s.poolTarget + 63) / 64 * 64);
    }
    // Local lights first: their count sizes the pool's initial budget with the view's pixels.
    updateLocalLights(fc, s, main);
    const double mpixels = (double)main.view.width * main.view.height / 1e6;
    const double budget = mpixels * q.number("shadow.vsm.pool_pages_per_mpixel") + s.latest.localAssigned * q.number("shadow.vsm.pool_pages_per_local_light");
    uint32_t pages = std::max((uint32_t)q.integer("shadow.vsm.pool_pages"), (uint32_t)budget);
    pages = (std::max(pages, s.poolTarget) + 63) / 64 * 64;
    if (!s.pool || s.poolPagesX * s.poolPagesY < pages) createState(fc, s, pages);

    // Light basis, caster range, scene-wide invalidation.
    const scene::Scene* src = fc.scene.source();
    const float3 sunDir = normalize(src ? src->sun.direction : scene::Sun{}.direction);
    bool invalidate = !s.initialized;
    if (std::memcmp(&sunDir, &s.sun, sizeof sunDir) != 0 || fc.scene.revision() != s.sceneRevision)
    {
        invalidate = true;
        s.sun = sunDir;
        s.sceneRevision = fc.scene.revision();
        float lo, hi;
        casterHeightRange(fc.scene, sunDir, lo, hi);
        const float margin = (float)q.number("shadow.vsm.height_margin_m");
        s.hMin = lo - margin;
        s.hMax = hi + margin;
    }
    const float3 windDir = src ? src->windDirection : float3{};
    const float windSpeed = src ? src->windSpeed : 0.0f;
    const bool windChanged = std::memcmp(&windDir, &s.windDirection, sizeof windDir) != 0 || windSpeed != s.windSpeed;
    s.windDirection = windDir;
    s.windSpeed = windSpeed;
    VsmConstantsCpu& c = s.constants;
    c = {};
    c.windChanged = windChanged ? 1u : 0u;
    c.lightZ = sunDir;
    lightBasis(sunDir, c.lightX, c.lightY);
    c.hMin = s.hMin;
    c.hMax = s.hMax;
    c.tanSunRadius = std::tan(src ? src->sun.angularRadius : scene::Sun{}.angularRadius);
    c.poolPagesX = s.poolPagesX;
    c.poolPagesY = s.poolPagesY;
    c.frame = (uint32_t)++s.frames;
    c.sceneInvalidate = invalidate ? 1u : 0u;
    c.time = (float)fc.frame.time;
    c.lodBias = (float)q.number("shadow.vsm.lod_bias");
    c.receiverBiasTexels = (float)q.number("shadow.vsm.receiver_bias_texels");
    c.maxReceiverSlope = (float)q.number("shadow.vsm.max_receiver_slope");
    c.cacheFrames = (uint32_t)q.integer("shadow.vsm.cache_frames");
    c.instanceCount = (uint32_t)fc.scene.instances().size();
    c.windTexels = (uint32_t)q.integer("shadow.vsm.wind_texels");
    c.searchTaps = (uint32_t)q.integer("shadow.vsm.search_taps");
    c.filterTaps = (uint32_t)q.integer("shadow.vsm.filter_taps");
    const float3 cam = main.view.position;
    const float cu = dot(cam, c.lightX), cv = dot(cam, c.lightY);
    c.cameraUV[0] = cu;
    c.cameraUV[1] = cv;
    for (uint32_t k = 0; k < kLevels; ++k)
    {
        const float texel = std::ldexp(1.0f, (int)k - 10);
        const float pageSize = texel * kPage;
        c.level[k].texel = texel;
        c.level[k].origin[0] = (int32_t)std::floor(cu / pageSize) - (int32_t)kTable / 2;
        c.level[k].origin[1] = (int32_t)std::floor(cv / pageSize) - (int32_t)kTable / 2;
    }
    s.constantsOffset = (uint32_t)(fc.frame.frameIndex % kRingSlots) * kRingStride;
    std::memcpy(s.ringMapped + s.constantsOffset, &c, sizeof c);
    s.initialized = true;

    // Instances' last seen revisions (dirty rule a).
    if (c.instanceCount > s.instanceCapacity)
    {
        if (s.lastRevision) fc.device.deferRelease(s.lastRevision);
        s.instanceCapacity = std::max(c.instanceCount, 1u);
        s.lastRevision = createBuffer(fc.device, L"S VSM instance revisions", (uint64_t)s.instanceCapacity * 8);
        if (s.movedList) fc.device.deferRelease(s.movedList);
        s.movedList = createBuffer(fc.device, L"S VSM moved instances", 4 + (uint64_t)s.instanceCapacity * 8);
        if (s.motion) fc.device.deferRelease(s.motion);
        s.motion = createBuffer(fc.device, L"S VSM instance motion", (uint64_t)s.instanceCapacity * kLevels * 16);
        if (s.jointCounts) fc.device.deferRelease(s.jointCounts);
        s.jointCounts = createBuffer(fc.device, L"S VSM joint counts", (uint64_t)s.instanceCapacity * 4);
        s.jointsRevision = UINT32_MAX;
    }
    // Joint counts of the skinned instances (displacement bound of their palettes), when the scene changes.
    if (s.jointsRevision != fc.scene.revision())
    {
        std::vector<uint32_t> counts(s.instanceCapacity, 0);
        if (const scene::Scene* scn = fc.scene.source())
            for (uint32_t i = 0; i < c.instanceCount && i < scn->instances.size(); ++i)
            {
                const scene::Instance& in = scn->instances[i];
                if ((in.flags & scene::InstanceSkinned) && in.skeleton < scn->skeletons.size()) counts[i] = (uint32_t)scn->skeletons[in.skeleton].jointToModel.size();
            }
        if (s.jointStaging) fc.device.deferRelease(s.jointStaging);
        s.jointStaging = createBuffer(fc.device, L"S VSM joint counts staging", (uint64_t)s.instanceCapacity * 4, D3D12_HEAP_TYPE_UPLOAD);
        void* p = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(s.jointStaging->Map(0, &none, &p), "map VSM joint counts");
        std::memcpy(p, counts.data(), counts.size() * 4);
        s.jointStaging->Unmap(0, nullptr);
        s.jointsRevision = fc.scene.revision();
        s.jointsPending = true;
    }

    RenderGraph& g = fc.graph;
    const BufferRef pool = g.importBuffer(s.pool.Get(), BufferDesc{ "S VSM pool", (uint64_t)pages * kPage * kPage * 4, 0 });
    const BufferRef table = g.importBuffer(s.table.Get(), BufferDesc{ "S VSM page table", (uint64_t)kTotalSlots * 8, 0 });
    const BufferRef requests = g.importBuffer(s.requests.Get(), BufferDesc{ "S VSM requests", (uint64_t)kTotalSlots * 4, 0 });
    const BufferRef localMask = g.importBuffer(s.localMask.Get(), BufferDesc{ "S VSM local cull mask", (uint64_t)kLocalLights * kLocalViewsPerLight * 512 * 4, 0 });
    // Slots the per-slot passes cover: the sun's and those of the assigned local lights.
    const uint32_t slotsUsed = kSlots + s.localUsed * kLocalLightSlots;
    const uint32_t localLightsSrv = s.localLightsNow, slotOfSrv = s.slotOfNow, activeSrv = s.activeNow;
    const uint32_t activeLocal = (uint32_t)s.localActive.size();
    // Froxel light lists (with each entry's shadow-slot bit): the local page marks and the visibility slots read them.
    recordFroxelLists(fc, main, slotOfSrv);
    const BufferRef froxelLists = fc.resources.froxelLights;
    const BufferRef meta = g.importBuffer(s.meta.Get(), BufferDesc{ "S VSM page metadata", (uint64_t)pages * kMetaBytes, kMetaBytes });
    const BufferRef blocks = g.importBuffer(s.blocks.Get(), BufferDesc{ "S VSM page blocks", (uint64_t)pages * kBlockBytes, 0 });
    s.blocksRef = blocks;
    const BufferRef freeList = g.importBuffer(s.freeList.Get(), BufferDesc{ "S VSM free list", 4 + (uint64_t)pages * 4, 0 });
    const BufferRef dirty = g.importBuffer(s.dirtyList.Get(), BufferDesc{ "S VSM dirty list", 8 + (uint64_t)pages * 8, 0 });
    const BufferRef mask = g.importBuffer(s.cullMask.Get(), BufferDesc{ "S VSM cull mask", (uint64_t)kSlots / 8, 0 });
    const BufferRef args = g.importBuffer(s.args.Get(), BufferDesc{ "S VSM indirect args", 48, 0 });
    const BufferRef moved = g.importBuffer(s.movedList.Get(), BufferDesc{ "S VSM moved instances", 4 + (uint64_t)s.instanceCapacity * 8, 0 });
    const BufferRef motion = g.importBuffer(s.motion.Get(), BufferDesc{ "S VSM instance motion", (uint64_t)s.instanceCapacity * kLevels * 16, 16 });
    const BufferRef joints = g.importBuffer(s.jointCounts.Get(), BufferDesc{ "S VSM joint counts", (uint64_t)s.instanceCapacity * 4, 4 });
    if (s.jointsPending)
    {
        ID3D12Resource* staging = s.jointStaging.Get();
        const uint64_t bytes = (uint64_t)s.instanceCapacity * 4;
        g.addPass("s.vsm.joints", QueueType::Graphics,
                  [&](PassBuilder& b) {
                      b.use(joints, Use::CopyDst);
                      b.keep();
                  },
                  [=](PassContext& ctx) { ctx.cmd->CopyBufferRegion(ctx.resource(joints), 0, staging, 0, bytes); });
        s.jointsPending = false;
    }
    const BufferRef statsBuf = g.importBuffer(s.stats.Get(), BufferDesc{ "S VSM stats", 80, 0 });
    s.statsRef = statsBuf;
    const BufferRef revisions = g.importBuffer(s.lastRevision.Get(), BufferDesc{ "S VSM instance revisions", (uint64_t)s.instanceCapacity * 8, 8 });
    s.recordedFrame = fc.frame.frameIndex;
    s.poolRef = pool;
    s.tableRef = table;
    s.metaRef = meta;
    s.pagesRecorded = true;
    // FrameResources (v1.18): other tracks read the VSM through ShadowVisibility.hlsli (ShadowSrvs).
    fc.resources.vsmPool = pool;
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
                      b.use(freeList, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[8] = { ctx.uav(table), ctx.uav(requests), ctx.uav(meta), ctx.uav(freeList), kTotalSlots, pages, 0, 0 };
                      ctx.cmd->SetPipelineState(pso);
                      ctx.computeConstants(k, 8);
                      ctx.cmd->Dispatch(groups(std::max(kTotalSlots, pages), 256), 1, 1);
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
                  [=](PassContext& ctx) { ctx.cmd->CopyBufferRegion(rb, slot * 80ull, ctx.resource(statsBuf), 0, 80); });
        s.statsFrame[slot] = c.frame - 1;
        s.statsFence[slot] = 0;
        s.lastStatsSlot = (int)slot;
    }
    {
        ID3D12PipelineState* pso = sh.compute("Passes/Shadow/VsmBegin");
        g.addPass("s.vsm.begin", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(dirty, Use::UavCompute);
                      b.use(statsBuf, Use::UavCompute);
                      b.use(moved, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[4] = { ctx.uav(dirty), ctx.uav(statsBuf), ctx.uav(moved), 0 };
                      ctx.cmd->SetPipelineState(pso);
                      ctx.computeConstants(k, 4);
                      ctx.cmd->Dispatch(1, 1, 1);
                  });
    }
    if (main.depth.valid())
    {
        ID3D12PipelineState* pso = sh.compute("Passes/Shadow/VsmMark");
        const TextureRef depth = main.depth;
        const uint32_t w = main.view.width, h = main.view.height;
        g.addPass("s.vsm.mark", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(depth, Use::SrvCompute);
                      b.use(requests, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[4] = { ctx.srv(depth), ctx.uav(requests), ring, off };
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
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[8] = { ctx.uav(requests), ring, grid.gridX | grid.gridY << 16, grid.slices | grid.tilePx << 16, nearBits, farBits, texelBits, 0 };
                      ctx.cmd->SetPipelineState(pso);
                      ctx.bindFrameConstants(mainConstants);
                      ctx.computeConstants(k, 8);
                      ctx.cmd->Dispatch(grid.gridX, grid.gridY, 1);
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
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[8] = { ctx.uav(requests), ctx.srv(froxelLists), localLightsSrv, slotOfSrv, texelBits, 0, 0, 0 };
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
    if (c.instanceCount > 0)
    {
        // Dirty rule (a): instances whose revisions changed -> list -> one group per (instance, level).
        ID3D12PipelineState* pm = sh.compute("Passes/Shadow/VsmMoved");
        ID3D12PipelineState* pa = sh.compute("Passes/Shadow/VsmMovedArgs");
        ID3D12PipelineState* pi = sh.compute("Passes/Shadow/VsmInvalidate");
        const uint32_t n = c.instanceCount;
        g.addPass("s.vsm.moved", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(revisions, Use::UavCompute);
                      b.use(moved, Use::UavCompute);
                      b.use(motion, Use::UavCompute);
                      b.use(joints, Use::SrvCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[8] = { ctx.uav(revisions), ctx.uav(moved), n, ctx.uav(motion), ctx.srv(joints), ring, 0, 0 };
                      ctx.cmd->SetPipelineState(pm);
                      ctx.bindFrameConstants(mainConstants);
                      ctx.computeConstants(k, 8);
                      ctx.cmd->Dispatch(groups(n, 64), 1, 1);
                  });
        g.addPass("s.vsm.movedargs", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(moved, Use::SrvCompute);
                      b.use(args, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[4] = { ctx.srv(moved), ctx.uav(args), activeLocal, 0 };
                      ctx.cmd->SetPipelineState(pa);
                      ctx.computeConstants(k, 4);
                      ctx.cmd->Dispatch(1, 1, 1);
                  });
        g.addPass("s.vsm.invalidate", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(table, Use::UavCompute);
                      b.use(moved, Use::SrvCompute);
                      b.use(motion, Use::UavCompute);
                      b.use(args, Use::IndirectArgs);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[4] = { ctx.uav(table), ctx.srv(moved), ring, ctx.uav(motion) };
                      ctx.cmd->SetPipelineState(pi);
                      ctx.bindFrameConstants(mainConstants);
                      ctx.computeConstants(k, 4);
                      ctx.cmd->ExecuteIndirect(signature, 1, ctx.resource(args), 16, nullptr, 0);
                  });
    }
        if (activeLocal > 0)
        {
            ID3D12PipelineState* pl = sh.compute("Passes/Shadow/VsmLocalInvalidate");
            g.addPass("s.vsm.localinvalidate", QueueType::Compute,
                      [&](PassBuilder& b) {
                          b.use(table, Use::UavCompute);
                          b.use(moved, Use::SrvCompute);
                          b.use(args, Use::IndirectArgs);
                          b.keep();
                      },
                      [=](PassContext& ctx) {
                          const uint32_t k[8] = { ctx.uav(table), ctx.srv(moved), localLightsSrv, activeSrv, activeLocal, 0, 0, 0 };
                          ctx.cmd->SetPipelineState(pl);
                          ctx.bindFrameConstants(mainConstants);
                          ctx.computeConstants(k, 8);
                          ctx.cmd->ExecuteIndirect(signature, 1, ctx.resource(args), 32, nullptr, 0);
                      });
        }
    {
        ID3D12PipelineState* pso = sh.compute("Passes/Shadow/VsmRelease");
        g.addPass("s.vsm.release", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(table, Use::UavCompute);
                      b.use(requests, Use::SrvCompute);
                      b.use(meta, Use::UavCompute);
                      b.use(freeList, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[8] = { ctx.uav(table), ctx.srv(requests), ctx.uav(meta), ctx.uav(freeList), ring, localLightsSrv, 0, 0 };
                      ctx.cmd->SetPipelineState(pso);
                      ctx.computeConstants(k, 8);
                      ctx.cmd->Dispatch(groups(slotsUsed, 256), 1, 1);
                  });
    }
    {
        ID3D12PipelineState* pso = sh.compute("Passes/Shadow/VsmAllocate");
        g.addPass("s.vsm.allocate", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(table, Use::UavCompute);
                      b.use(requests, Use::UavCompute);
                      b.use(meta, Use::UavCompute);
                      b.use(freeList, Use::UavCompute);
                      b.use(dirty, Use::UavCompute);
                      b.use(statsBuf, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[8] = { ctx.uav(table), ctx.uav(requests), ctx.uav(meta), ctx.uav(freeList), ctx.uav(dirty), ctx.uav(statsBuf), ring, localLightsSrv };
                      ctx.cmd->SetPipelineState(pso);
                      ctx.computeConstants(k, 8);
                      ctx.cmd->Dispatch(groups(slotsUsed, 256), 1, 1);
                  });
    }
    {
        ID3D12PipelineState* pm = sh.compute("Passes/Shadow/VsmCullMask");
        ID3D12PipelineState* pf = sh.compute("Passes/Shadow/VsmFinalize");
        g.addPass("s.vsm.cullmask", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(table, Use::SrvCompute);
                      b.use(mask, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[4] = { ctx.srv(table), ctx.uav(mask), ring, off };
                      ctx.cmd->SetPipelineState(pm);
                      ctx.computeConstants(k, 4);
                      ctx.cmd->Dispatch(groups(kSlots / 32, 64), 1, 1);
                  });
        g.addPass("s.vsm.finalize", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(dirty, Use::SrvCompute);
                      b.use(args, Use::UavCompute);
                      b.use(freeList, Use::SrvCompute);
                      b.use(statsBuf, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[4] = { ctx.srv(dirty), ctx.uav(args), ctx.srv(freeList), ctx.uav(statsBuf) };
                      ctx.cmd->SetPipelineState(pf);
                      ctx.computeConstants(k, 4);
                      ctx.cmd->Dispatch(1, 1, 1);
                  });
    }
    {
        ID3D12PipelineState* pso = sh.compute("Passes/Shadow/VsmClear");
        g.addPass("s.vsm.clear", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(dirty, Use::SrvCompute);
                      b.use(args, Use::IndirectArgs);
                      b.use(pool, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[4] = { ctx.srv(dirty), ctx.uav(pool), ring, off };
                      ctx.cmd->SetPipelineState(pso);
                      ctx.computeConstants(k, 4);
                      ctx.cmd->ExecuteIndirect(signature, 1, ctx.resource(args), 0, nullptr, 0);
                  });
    }

    // Dirty pages through V's cluster pipeline: one orthographic view per level over its whole window; the tile mask
    // limits the raster to dirty pages, the pixel kernel writes only into dirty pages.
    if (fc.services.rasterizeDepth)
    {
        DepthRasterRequest r;
        r.name = "s.vsm.raster";
        r.instanceMask = scene::InstanceCastShadow;
        r.pixelKernel = "Passes/Shadow/VsmPagePixel";
        r.bufferUses.push_back({ pool, Use::UavGraphics });
        r.bufferUses.push_back({ table, Use::SrvGraphics });
        r.bufferUses.push_back({ meta, Use::UavGraphics });
        r.pixelConstants[0] = s.poolUav;
        r.pixelConstants[1] = s.tableSrv;
        std::memcpy(&r.pixelConstants[2], &c.hMin, 4);
        std::memcpy(&r.pixelConstants[3], &c.hMax, 4);
        r.pixelConstants[4] = s.poolPagesX;
        r.pixelConstants[5] = s.metaUav;
        r.cullMask = mask;
        r.cullTilePx = kPage;
        r.tileLocal = true;  // fragments only inside dirty pages (INTERFACES 5.3 v1.7)
        r.cull = D3D12_CULL_MODE_NONE;
        for (uint32_t k = 0; k < kLevels; ++k)
        {
            RasterView v;
            v.viewProj = levelViewProj(c, k);
            v.viewportX = v.viewportY = 0;
            v.viewportWidth = v.viewportHeight = kVirtual;
            v.lodPixelsPerMetre = 1.0f / c.level[k].texel;
            v.userData = k | ((uint32_t)(c.level[k].origin[0] & 127) << 5) | ((uint32_t)(c.level[k].origin[1] & 127) << 12);
            v.cullMaskOffset = k * (kTable * kTable / 32);
            r.views.push_back(v);
        }
        fc.services.rasterizeDepth(fc, r);
    }
    if (fc.services.rasterizeDepth && !s.localActive.empty())
    {
        // Local lights: tile masks of the (light, face, mip) views, then the raster of their dirty pages; one request per
        // 6 lights (42 views each, V's 255-view limit), all views in one 16384^2 viewport (VsmLocalPixel.hlsl).
        const uint32_t views = (uint32_t)s.localActive.size() * kLocalViewsPerLight;
        ID3D12PipelineState* pm = sh.compute("Passes/Shadow/VsmLocalCullMask");
        g.addPass("s.vsm.localcullmask", QueueType::Compute,
                  [&](PassBuilder& b) {
                      b.use(table, Use::SrvCompute);
                      b.use(localMask, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[4] = { ctx.srv(table), ctx.uav(localMask), views, activeSrv };
                      ctx.cmd->SetPipelineState(pm);
                      ctx.computeConstants(k, 4);
                      ctx.cmd->Dispatch(groups(views * 512, 64), 1, 1);
                  });
        for (uint32_t first = 0; first < s.localActive.size(); first += 6)
        {
            DepthRasterRequest r;
            r.name = "s.vsm.localraster" + std::to_string(first / 6);
            r.instanceMask = scene::InstanceCastShadow;
            r.pixelKernel = "Passes/Shadow/VsmLocalPixel";
            r.bufferUses.push_back({ pool, Use::UavGraphics });
            r.bufferUses.push_back({ table, Use::SrvGraphics });
            r.bufferUses.push_back({ meta, Use::UavGraphics });
            r.pixelConstants[0] = s.poolUav;
            r.pixelConstants[1] = s.tableSrv;
            r.pixelConstants[2] = localLightsSrv;
            r.pixelConstants[5] = s.metaUav;
            r.cullMask = localMask;
            r.cullTilePx = kPage;
            r.tileLocal = true;
            r.cull = D3D12_CULL_MODE_NONE;
            for (uint32_t a = first; a < std::min<uint32_t>(first + 6, (uint32_t)s.localActive.size()); ++a)
            {
                const uint32_t slot = s.localActive[a];
                const VsmLocalLightCpu& l = s.localData[slot];
                for (uint32_t face = 0; face < 6; ++face)
                    for (uint32_t mip = 0; mip < kLocalMips; ++mip)
                    {
                        RasterView v;
                        v.viewProj = localViewProj(l, face, mip);
                        v.viewportX = v.viewportY = 0;
                        v.viewportWidth = v.viewportHeight = kVirtual;
                        v.lodPixelsPerMetre = 0.5f * (float)(kPage << mip);  // focal length in texels (90 degree face)
                        v.userData = slot | face << 7 | mip << 10;
                        v.cullMaskOffset = (a * kLocalViewsPerLight + face * kLocalMips + mip) * 512;
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
                      b.use(dirty, Use::SrvCompute);
                      b.use(args, Use::IndirectArgs);
                      b.use(pool, Use::SrvCompute);
                      b.use(meta, Use::UavCompute);
                      b.use(blocks, Use::UavCompute);
                      b.keep();
                  },
                  [=](PassContext& ctx) {
                      const uint32_t k[8] = { ctx.srv(dirty), ctx.srv(pool), ctx.uav(meta), ring, off, ctx.uav(blocks), 0, 0 };
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
    const BufferRef pool = s.poolRef;
    // Local slots (1-3) and the overflow list from the view's froxel lists (INTERFACES 7.3, 7.4, v1.22): the main view's
    // (FrameRenderer sets them after froxels; tests without froxels: shadowPages' lists), a planar reflection view's own
    // (recorded here with its air volume from the mirror plane on).
    const bool mainView = view.view.kind == gpu::ViewKind::Main;
    if (view.view.kind == gpu::ViewKind::PlanarReflection && !view.froxelLights.valid()) recordPlanarFroxels(fc, view);
    const BufferRef froxelLists = view.froxelLights.valid() ? view.froxelLights : (mainView ? fc.resources.froxelLights : BufferRef{});
    const bool localSlots = froxelLists.valid() && s.localLightsNow != UINT32_MAX;
    const uint32_t localLightsSrv = s.localLightsNow, slotOfSrv = s.slotOfNow;
    const BufferRef table = s.tableRef, bound = s.boundRef, blocks = s.blocksRef, statsBuf = s.statsRef;
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
        s.latest.overflowCapacity = capacity;
        heads = g.createTexture(TextureDesc{ "S shadow overflow tiles", tilesX, tilesY, 1, 1, DXGI_FORMAT_R32_UINT });
        overflow = g.createBuffer(BufferDesc{ "S shadow overflow", (uint64_t)capacity * 4, 0 });
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
                  b.use(pool, Use::SrvCompute);
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
              },
              [=](PassContext& ctx) {
                  const uint32_t k[16] = { ctx.srv(depth), ctx.srv(gbuffer), ctx.uav(out), ring, overflowList ? ctx.uav(overflowTiles) : 0xFFFFFFFFu,
                                           ctx.srv(table), ctx.srv(pool), ctx.srv(bound), ctx.uav(list), ctx.srv(blocks), ctx.uav(statsBuf), 0,
                                           localSlots ? ctx.srv(froxelLists) : 0xFFFFFFFFu, localLightsSrv, slotOfSrv,
                                           overflowList ? ctx.uav(heads) : 0xFFFFFFFFu };
                  ctx.cmd->SetPipelineState(p1);
                  ctx.bindFrameConstants(constants);
                  ctx.computeConstants(k, 16);
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
                  b.use(pool, Use::SrvCompute);
                  b.use(table, Use::SrvCompute);
                  b.use(bound, Use::SrvCompute);
                  b.use(blocks, Use::SrvCompute);
                  b.use(list, Use::SrvCompute);
                  b.use(args, Use::IndirectArgs);
                  b.use(statsBuf, Use::UavCompute);
                  b.use(out, Use::UavCompute);
              },
              [=](PassContext& ctx) {
                  const uint32_t k[16] = { ctx.srv(depth), ctx.srv(gbuffer), ctx.uav(out), ring, off, ctx.srv(table), ctx.srv(pool), ctx.srv(bound),
                                           ctx.srv(list), ctx.srv(blocks), ctx.uav(statsBuf), 0, rays, steps, 0, 0 };
                  ctx.cmd->SetPipelineState(p2);
                  ctx.bindFrameConstants(constants);
                  ctx.computeConstants(k, 16);
                  ctx.cmd->ExecuteIndirect(signature, 1, ctx.resource(args), 0, nullptr, 0);
              });
    if (!overflowList) return;
    // Overflow tiles: recount, one block per tile from the capacity, the lights past the third (indirect, one group per
    // listed tile; without local slots the list is empty and the dispatch has no groups).
    g.addPass("s.shadow.overflow", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(depth, Use::SrvCompute);
                  b.use(gbuffer, Use::SrvCompute);
                  b.use(pool, Use::SrvCompute);
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
              },
              [=](PassContext& ctx) {
                  const uint32_t k[16] = { ctx.srv(depth), ctx.srv(gbuffer), ctx.uav(heads), ring, ctx.srv(overflowTiles), ctx.srv(table), ctx.srv(pool),
                                           ctx.srv(blocks), ctx.uav(overflow), capacity, ctx.uav(fallback), ctx.uav(statsBuf),
                                           localSlots ? ctx.srv(froxelLists) : 0xFFFFFFFFu, localLightsSrv, slotOfSrv, ctx.uav(counter) };
                  ctx.cmd->SetPipelineState(po);
                  ctx.bindFrameConstants(constants);
                  ctx.computeConstants(k, 16);
                  ctx.cmd->ExecuteIndirect(signature, 1, ctx.resource(overflowTiles), 4, nullptr, 0);
              });
}
} // namespace unx::render::shadow
