#pragma once
// Ray-traced scene of the R track (ARCHITECTURE 2.7, 2.8, 2.12). Ported from the previous engine's ray scene partition
// and deformation logic (TitanNative RayScene.cpp / RayPartition.inl), not its structure:
//  - one BLAS per mesh for rigid instances (original triangles, one geometry per submesh, OPAQUE unless the submesh
//    material is alpha-tested, PREFER_FAST_TRACE, compacted), shared by every instance of the mesh;
//  - one BLAS per deformed instance (skinned characters: world-space vertices written by RayTracing/Deform through
//    deformVertex(), ALLOW_UPDATE, refit each frame);
//  - a static TLAS (rigid, non-dynamic instances; rebuilt only when that set changes) and a dynamic TLAS (Dynamic and
//    deformed instances; rebuilt every frame, <= raytracing.dynamic_tlas_instances_max).
// Rays query the dynamic TLAS, then the static one with TMax clipped (RayShaders.hlsli).
#include "unx/render/Frame.h"
#include "unx/render/GpuScene.h"

#include <cstdint>
#include <vector>

namespace unx::render::rt
{
// GPU records (RayScene.hlsli).
struct RtInstance  // 16 B
{
    uint32_t sceneInstance;
    uint32_t geometryBase;
    uint32_t vertexBase;
    uint32_t flags;
};
struct RtGeometry  // 16 B
{
    uint32_t indexOffset;
    uint32_t submesh;
    uint32_t flags;
    uint32_t vertexMap;
};
struct RtDeformedVertex  // 16 B
{
    float3 position;
    uint32_t normalOct;
};
struct DeformJob  // 16 B (Deform.hlsl)
{
    uint32_t sceneInstance, deformedBase, vertexMap, vertexCount;
};
constexpr uint32_t kRtInstanceDeformed = 1u;
constexpr uint32_t kRtGeometryProxyIndices = 1u;
constexpr uint32_t kRtMaskGi = 1u, kRtMaskReflection = 2u, kRtMaskAll = 0xFFu;

struct RaySceneStats
{
    uint32_t staticInstances = 0, dynamicInstances = 0, deformedInstances = 0;
    uint32_t meshBlas = 0;
    uint64_t meshBlasTriangles = 0;        // unique triangles in mesh BLASes
    uint64_t deformedTriangles = 0;        // triangles refit per frame (the proxies' current cuts)
    uint64_t deformedVertices = 0;         // vertices deformed per frame
    uint64_t meshBlasBytes = 0, meshBlasBytesBeforeCompaction = 0;
    uint64_t deformedBlasBytes = 0, tlasStaticBytes = 0, tlasDynamicBytes = 0;
    double loadMs = 0;                     // CPU wall time of the load-time build (includes GPU waits)
    uint32_t deformedAboveProxyBudget = 0; // deformed meshes with more triangles than raytracing.character_proxy_triangles
    uint32_t exactSlots = 0;               // reflection exact set capacity (original BLAS)
    uint32_t exactOccupied = 0, exactBuilds = 0;  // last frame: slots in use, slots (re)built
    uint64_t exactVertices = 0;            // vertices deformed per occupied slot's owner (last frame)
    uint32_t proxySwitches = 0;            // last frame: deformed instances whose proxy cut changed (BLAS rebuilt)
};

class RayScene
{
public:
    // The frame's ray scene, kept in the FrameRenderer's track state ("R.rayScene"); built on first use (load time,
    // outside the frame graph) and rebuilt when the GPU scene is re-uploaded (streaming boundary).
    static RayScene& get(FramePassContext& fc);
    // Device-keyed registry for tests that trace without a frame context; releaseDevice drops a device's scenes.
    static RayScene& get(Device& device, ShaderLibrary& shaders, GpuScene& scene, const QualityConfig& quality);
    static void releaseDevice(Device& device);
    // Drops the registry's scene built for 'scene' (call before a GpuScene it was built from is destroyed: the registry
    // is keyed by address, and a later GpuScene at the same address must not receive it).
    static void release(Device& device, GpuScene& scene);

    RayScene(Device& device, ShaderLibrary& shaders, GpuScene& scene, const QualityConfig& quality);
    ~RayScene();
    RayScene(const RayScene&) = delete;
    RayScene& operator=(const RayScene&) = delete;

    // Frame path (ARCHITECTURE 4.1 C2): imports the acceleration structures, declares r.as.deform -> r.as.refit (deformed
    // BLASes) -> r.as.tlas.dynamic, and sets FrameResources::tlasStatic/tlasDynamic.
    void record(FramePassContext& fc);
    // Declares what a tracing pass of this frame reads (both TLASes, deformed BLASes and vertices).
    void declareTraversal(PassBuilder& b) const;
    // Bindless indices for ray libraries: root constants P[6], P[7] (RtSceneSrvs).
    void rootConstants(uint32_t out[8]) const;
    const RaySceneStats& stats() const { return m_stats; }
    uint32_t sceneRevision() const { return m_sceneRevision; }

    // Reflection exact set (ARCHITECTURE 2.6): per skinned instance, the M/G reflection rays that hit it this frame. The
    // reflection trace adds to it (ReflectionHit.hlsli, RtInstance flags >> 8 = deformed index) and calls
    // recordExactReadback after its trace; the characters hit at least raytracing.exact_set_min_hits times (most first, at
    // most raytracing.exact_set_max) are traced with their original mesh from framesInFlight frames later.
    BufferRef exactHitCounts() const { return m_frame.exactCounts; }
    void recordExactReadback(FramePassContext& fc);

    // Sun visibility at ray hits from S's VSM (v1.18, shadowSunVisibilityAt): the frame's VSM buffers, captured by the
    // ray pass's recording (after S's shadowPages; invalid when S is not in the build). declareVsm adds their reads to the
    // pass; vsmSrvs writes ShadowSrvs for 'user' (0 = GI, 1 = reflections) into R's upload ring from the pass's execution
    // and returns the raw SRV the kernel loads it from (UNX_NONE without a VSM).
    struct VsmRefs
    {
        BufferRef pageTable, pool, blocks, searchBound;
        uint32_t constants = 0xFFFFFFFFu;
        bool valid() const { return pageTable.valid() && pool.valid() && blocks.valid() && searchBound.valid() && constants != 0xFFFFFFFFu; }
    };
    static VsmRefs vsmRefs(const FrameResources& r) { return { r.vsmPageTable, r.vsmPool, r.vsmBlocks, r.vsmSearchBound, r.vsmConstants }; }
    static void declareVsm(PassBuilder& b, const VsmRefs& v);
    uint32_t vsmSrvs(PassContext& c, const VsmRefs& v, uint64_t frame, uint32_t user);

    // Re-deforms the deformed instances, refits their BLASes and rebuilds the dynamic TLAS on 'cmd', with its own
    // barriers: the load-time path and tests that run outside a frame graph (the frame path is record()).
    void updateDynamic(ID3D12GraphicsCommandList7* cmd, bool refit);

private:
    struct Buffer
    {
        ComPtr<ID3D12Resource> resource;
        uint32_t srv = gpu::kNone;
        uint64_t bytes = 0;
        D3D12_GPU_VIRTUAL_ADDRESS address() const { return resource ? resource->GetGPUVirtualAddress() : 0; }
    };
    Buffer createBuffer(uint64_t bytes, bool uav, bool accelerationStructure, const wchar_t* name);
    Buffer createStructured(const void* data, uint32_t stride, uint32_t count, const wchar_t* name);
    void upload(Buffer& target, const void* data, uint64_t bytes);
    void release(Buffer& b);
    uint32_t tlasSrv(D3D12_GPU_VIRTUAL_ADDRESS address);

    void buildMeshBlas();
    void buildDeformed();
    void buildStaticTlas();
    void recordDeform(ID3D12GraphicsCommandList7* cmd) const;
    // Refit (refit = true) or build every deformed BLAS; 'rebuild' (per deformed instance, optional) builds those whose proxy
    // cut changed this frame.
    void recordRefit(ID3D12GraphicsCommandList7* cmd, bool refit, const std::vector<uint8_t>* rebuild = nullptr) const;
    void recordExactBuilds(ID3D12GraphicsCommandList7* cmd, const std::vector<uint8_t>& rebuild) const;
    void selectExactSet(FramePassContext& fc);
    uint32_t maxMeshVertices() const;
    void recordDynamicTlas(ID3D12GraphicsCommandList7* cmd, D3D12_GPU_VIRTUAL_ADDRESS descs);
    void recordStaticTlas(ID3D12GraphicsCommandList7* cmd, D3D12_GPU_VIRTUAL_ADDRESS descs);
    // INTERFACES 6.3 (v1.8): the instance's current transform and visibility (hidden = mask 0: no ray can hit it).
    void refreshDesc(D3D12_RAYTRACING_INSTANCE_DESC& d, const gpu::Instance& in, bool worldSpace) const;
    static uint64_t staticKey(const gpu::Instance& in) { return ((uint64_t)(in.flags & gpu::kInstanceHidden) << 32) | in.transformRevision; }

    struct Frame  // graph references of the current frame
    {
        BufferRef tlasStatic, tlasDynamic, deformedBlas, deformedVertices, exactCounts, instances, jobs;
    };
    Frame m_frame;

    Device& m_device;
    ShaderLibrary& m_shaders;
    GpuScene& m_scene;
    uint32_t m_sceneRevision = 0;

    struct MeshBlas
    {
        uint64_t offset = 0;  // in the compacted pool
        uint32_t geometryBase = gpu::kNone;
        bool anyAlpha = false;
    };
    std::vector<MeshBlas> m_meshBlas;  // per scene mesh (geometryBase kNone = unused)
    Buffer m_meshBlasPool;

    // RT proxies of a skinned mesh (ARCHITECTURE 2.8): V's uniform-error LOD cuts (ClusterData::lodLevels) with at most
    // raytracing.character_proxy_triangles triangles, finest first (the coarsest cut when none fits; the full mesh when
    // the scene has no cluster data). Each: per-submesh ranges of compact indices in R's index pool, a vertex map (compact
    // -> mesh vertex) so only its vertices are deformed, RtGeometry records, and the cut's object-space error.
    struct ProxyMesh
    {
        bool reduced = false;
        float error = 0;
        uint32_t vertexMap = 0, vertexCount = 0, geometryBase = 0, triangles = 0;
        std::vector<uint32_t> indexOffset, indexCount;  // per geometry (non-empty submesh), in R's index pool
        std::vector<uint32_t> submesh;
    };
    const std::vector<ProxyMesh>& proxyLevels(uint32_t mesh);
    ProxyMesh buildProxy(uint32_t mesh, uint32_t lodLevel);  // lodLevel = kNone: the full mesh
    std::vector<std::vector<ProxyMesh>> m_proxyLevels;      // per scene mesh (built on demand)
    std::vector<uint8_t> m_proxyLevelsBuilt;
    std::vector<uint32_t> m_indexPoolData, m_vertexMapData;
    uint32_t m_proxyBudget = 0;
    float m_proxyErrorPx = 1;  // raytracing.proxy_error_px
    // Per frame: each deformed instance's cut, the coarsest whose error (x the instance's scale) is at most
    // proxy_error_px x the main view's pixel angle x the instance's distance from the eye (its bounding sphere's nearest
    // point). A reflection or GI ray reaching the instance has a footprint of at least that width (the eye's pixel cone
    // over a path no shorter than the straight distance), so the cut's error stays below every ray's footprint. Finer at
    // once when the bound is exceeded, coarser only below 0.8 of it (hysteresis). A change rebuilds that BLAS.
    void selectProxyLevels(FramePassContext& fc);
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> proxyGeometry(uint32_t sceneInstance, uint32_t mesh, const ProxyMesh& p, uint32_t vertexBase) const;
    std::vector<uint8_t> m_deformedRebuild;  // per deformed instance: build instead of refit this frame

    struct Deformed
    {
        uint32_t sceneInstance = 0, mesh = 0;
        uint32_t originalGeometryBase = 0;  // the mesh's own geometry records (scene indices): exact set
        uint32_t vertexBase = 0, vertexCount = 0;
        uint64_t blasOffset = 0, scratchOffset = 0;
        uint32_t geometryBase = 0;
        uint32_t level = 0, record = 0;  // current proxy cut (index into proxyLevels), RtInstance record
        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries;
    };
    std::vector<Deformed> m_deformed;
    std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> originalGeometry(const Deformed& d, uint32_t vertexBase) const;
    // Exact set slots: a vertex region (largest skinned mesh), BLAS and scratch each; owner = deformed index or kNone.
    struct ExactSlot
    {
        uint32_t owner = 0xFFFFFFFFu, record = 0, job = 0, vertexBase = 0;
        uint64_t blasOffset = 0, scratchOffset = 0;
        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries;  // the owner's full mesh
    };
    std::vector<ExactSlot> m_exact;
    uint32_t m_exactMinHits = 0;
    Buffer m_exactCounts, m_exactZero;     // uint per deformed instance (UAV), and zeros to clear it
    uint32_t m_exactCountsUav = 0xFFFFFFFFu;
    ComPtr<ID3D12Resource> m_exactReadback, m_patchRing;
    const uint32_t* m_exactReadbackMapped = nullptr;
    uint8_t* m_patchRingMapped = nullptr;
    std::vector<uint64_t> m_exactSlotFrame;  // frame whose counts each read-back slot holds
    std::vector<uint8_t> m_exactRebuild;     // per slot: build (new owner) instead of refit this frame
    uint64_t m_poolVertices = 1;             // deformed pool size (proxies + exact slots)
    static constexpr uint64_t kPatchSlotBytes = 16384;       // per ring slot: exact set patches, then proxy cut patches
    static constexpr uint64_t kProxyPatchOffset = 1024;
    ComPtr<ID3D12Resource> m_vsmRing;        // kDescSlots x 2 users x 32 B of ShadowSrvs
    uint8_t* m_vsmRingMapped = nullptr;
    uint32_t m_vsmRingSrv[8] = {};           // kDescSlots x 2 users (static_assert in RayScene.cpp)
    Buffer m_deformedPool, m_deformedBlasPool, m_deformedScratch, m_deformJobs, m_deformGroups;
    uint32_t m_deformGroupCount = 0;
    uint32_t m_deformedPoolUav = gpu::kNone;

    std::vector<RtInstance> m_instances;
    std::vector<RtGeometry> m_geometries;
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> m_staticDescs, m_dynamicDescs;
    std::vector<uint32_t> m_dynamicRecord;  // RtInstance index of each dynamic TLAS instance
    Buffer m_instanceBuffer, m_geometryBuffer, m_indexPool, m_vertexMap;
    Buffer m_tlasStatic, m_tlasDynamic, m_tlasScratch, m_staticDescBuffer, m_dynamicDescBuffer, m_staticScratch;
    // Per-frame instance descriptors (upload ring, kDescSlots slots >= frames in flight): dynamic ones every frame, static
    // ones when a static instance's visibility or transform changed (rare; the static TLAS is then rebuilt in the frame).
    static constexpr uint32_t kDescSlots = 4;
    ComPtr<ID3D12Resource> m_descRing;
    uint8_t* m_descRingMapped = nullptr;
    uint64_t m_descSlotBytes = 0;
    std::vector<uint32_t> m_staticScene;   // scene instance of each static descriptor
    std::vector<uint64_t> m_staticKeys;    // visibility + transform revision the static TLAS was built with
    uint32_t m_tlasStaticSrv = gpu::kNone, m_tlasDynamicSrv = gpu::kNone;
    RaySceneStats m_stats;
};
} // namespace unx::render::rt
