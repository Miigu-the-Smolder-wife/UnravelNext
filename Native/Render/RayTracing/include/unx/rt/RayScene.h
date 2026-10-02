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
#include "unx/rt/ProxyPoseBound.h"

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
struct RtDeformedVertex  // 24 B
{
    float3 position;
    uint32_t normalOct;
    uint32_t motion[2];  // world - prevWorld, fp16 x 3 (RayScene.hlsli)
};
struct DeformJob  // 16 B (Deform.hlsl)
{
    uint32_t sceneInstance, deformedBase, vertexMap, vertexCount;
};
constexpr uint32_t kRtInstanceDeformed = 1u;
constexpr uint32_t kRtGeometryProxyIndices = 1u;
constexpr uint32_t kRtMaskGi = 1u, kRtMaskReflection = 2u, kRtMaskEmitter = 4u, kRtMaskAll = 0xFFu;
// Instances that cast shadows (scene::InstanceCastShadow) carry this bit; shadow rays of lights (shading.mega_lights) use it
// alone, so a mesh that casts no shadow in S's shadow maps blocks no light here either. No other ray's mask has it.
constexpr uint32_t kRtMaskShadow = 16u;
// The mask of a scene instance from its gpu::Instance flags (hidden: none; scene::InstanceShadowOnly, bit 7: the shadow
// rays alone when it casts, no ray otherwise).
constexpr uint32_t rtInstanceMask(uint32_t flags)
{
    if (flags & 0x80000000u) return 0u;
    if (flags & 0x80u) return (flags & 1u) ? kRtMaskShadow : 0u;
    return (flags & 1u) ? kRtMaskAll : (kRtMaskAll & ~kRtMaskShadow);
}
constexpr uint32_t kRtMaskFluid = 8u;  // W's triangle streams (refraction rays only: no scene records to shade them)
constexpr uint32_t kRtInstanceEmitter = 0xFFFFFEu;  // RT_INSTANCE_EMITTER (RayScene.hlsli)
constexpr uint32_t kRtInstanceStreamBase = 0xFFFF00u;  // + stream slot (< 64): RT_INSTANCE_STREAM (RayScene.hlsli)

struct DynamicTlasCensus  // the dynamic TLAS's instance descriptors (sampled every 64 frames)
{
    uint32_t instances = 0, distinctBlas = 0, nonFinite = 0, maskZero = 0, nullBlas = 0;
    float maxScale = 0;          // largest row sum of |linear part|
    float extent[3] = {};        // span of the translations (m)
};

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
    uint32_t staticTlasInherited = 0;      // B3: this build kept the previous object's static TLAS (an instance edit)
    uint32_t deformedAboveProxyBudget = 0; // deformed meshes with more triangles than raytracing.character_proxy_triangles
    uint32_t exactSlots = 0;               // reflection exact set capacity (original BLAS)
    uint32_t exactOccupied = 0, exactBuilds = 0;  // last frame: slots in use, slots (re)built
    uint64_t exactVertices = 0;            // vertices deformed per occupied slot's owner (last frame)
    uint32_t proxySwitches = 0;            // last frame: deformed instances whose proxy cut changed (BLAS rebuilt)
    float posedErrorOverBindMax = 0;       // last frame: largest finest-cut error bound in its pose over V's bind-pose error
    DynamicTlasCensus dynamicCensus;
    // Since load (measurement summaries): frames recorded, and over them exact slots (re)built, exact slots occupied, exact
    // vertices deformed and proxy cut switches.
    uint64_t framesRecorded = 0, exactBuildsTotal = 0, exactOccupiedTotal = 0, exactVerticesTotal = 0, proxySwitchesTotal = 0;
    // Since load, over the exact set selections: instances with enough hits whose finest proxy is within its error bound
    // (left out), and instances wanted (before the slot limit).
    uint64_t exactWithinBoundTotal = 0, exactWantedTotal = 0;
};

class RayScene
{
public:
    // The frame's ray scene, kept in the FrameRenderer's track state ("R.rayScene"); built on first use (load time,
    // outside the frame graph) and rebuilt when the GPU scene is re-uploaded (streaming boundary).
    static RayScene& get(FramePassContext& fc);
    // The track state's ray scene if one was built (measurement tools; nullptr otherwise).
    static RayScene* find(TrackState& state);
    // Device-keyed registry for tests that trace without a frame context; releaseDevice drops a device's scenes.
    static RayScene& get(Device& device, ShaderLibrary& shaders, GpuScene& scene, const QualityConfig& quality);
    static void releaseDevice(Device& device);
    // Drops the registry's scene built for 'scene' (call before a GpuScene it was built from is destroyed: the registry
    // is keyed by address, and a later GpuScene at the same address must not receive it).
    static void release(Device& device, GpuScene& scene);

    RayScene(Device& device, ShaderLibrary& shaders, GpuScene& scene, const QualityConfig& quality, RayScene* previous = nullptr);
    ~RayScene();
    RayScene(const RayScene&) = delete;
    RayScene& operator=(const RayScene&) = delete;

    // Frame path (ARCHITECTURE 4.1 C2): imports the acceleration structures, declares r.as.deform -> r.as.refit (deformed
    // BLASes) -> r.as.tlas.dynamic, and sets FrameResources::tlasStatic/tlasDynamic.
    void record(FramePassContext& fc);
    // Declares what a tracing pass of this frame reads (both TLASes, deformed BLASes and vertices).
    void declareTraversal(PassBuilder& b) const;
    // W's triangle streams traced this frame (FrameResources::triangleStreams slot, vertex buffer): refraction rays see
    // them (mask kRtMaskFluid, instance id kRtInstanceStreamBase + slot; R-W2).
    const std::vector<std::pair<uint32_t, BufferRef>>& streams() const { return m_streamsNow; }
    // Bindless indices for ray libraries: root constants P[6], P[7] (RtSceneSrvs; word 7 = this frame's local-light grid,
    // HitLocalLights.hlsli).
    void rootConstants(uint32_t out[8]) const;
    // Decals at ray hits (RayTracing/HitDecals.hlsli, FEATURES_GAME 5.2): once per frame, before the first pass that shades
    // hits reads rootConstants (GI, reflections), builds the decal query TLAS (one BLAS of procedural AABBs from
    // main.decalFrames, rebuilt each frame the main view has decals) and writes words 16..19 of this frame's light-grid
    // header. declareDecals adds its reads to a pass that shades hits.
    void recordDecals(FramePassContext& fc, const ViewResources& main);
    void declareDecals(PassBuilder& b) const;
    // Hair at ray hits (RayTracing/HitHair.hlsli; raytracing.hair): the rays' proxy of E's strands is E's density volume
    // (Passes/Hair/HairDensity.hlsli; FrameResources::hairDensityParams, recorded by tracks::hair after this scene's
    // record) - a ray's first fibre in it is a hit the ray pass shades. Once per frame, before the first pass whose rays
    // read it takes rootConstants (GI's gather, the radiance cache, the translucency volume, the reflections): word 22 of
    // this frame's light-grid header = the parameters' SRV (0xFFFFFFFF: no volume this frame, or the switch off), word 23
    // = the previous frame's translucency volume (FrameResources::translucencyGiPrevParams: the hits' indirect light;
    // 0xFFFFFFFF: none). declareHair adds the volumes' reads to such a pass; hairParams is the buffer for a compute pass
    // that follows the same rays (the screen traces).
    void recordHair(FramePassContext& fc);
    void declareHair(PassBuilder& b) const;
    BufferRef hairParams() const { return m_frame.hairParams; }
    const RaySceneStats& stats() const { return m_stats; }
    // B3: whether this object was rebuilt from the previous one for an instance edit (meshes and materials kept), and the
    // world AABBs of the geometry that changed (instances added, removed, re-meshed, hidden or moved while static) at
    // this build or in this frame's record. GI invalidates the cache entries whose texel rays cross them instead of a new
    // lighting epoch.
    bool incrementalRebuild() const { return m_incremental && !m_materialsChanged && !m_lightsChanged; }
    const std::vector<std::pair<float3, float3>>& changes() const { return m_changes; }
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
        BufferRef pageTable;
        TextureRef atlas;  // v1.43: the page atlas (the lookups take its SRV from the VSM constants)
        BufferRef blocks, searchBound, layers;  // layers: S's transmittance layer (v1.26; invalid = none)
        uint32_t constants = 0xFFFFFFFFu;
        bool valid() const { return pageTable.valid() && atlas.valid() && blocks.valid() && searchBound.valid() && constants != 0xFFFFFFFFu; }
    };
    static VsmRefs vsmRefs(const FrameResources& r) { return { r.vsmPageTable, r.vsmAtlas, r.vsmBlocks, r.vsmSearchBound, r.vsmLayers, r.vsmConstants }; }
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
    // Analytic area lights as ray geometry (design 12.4 structure 2): one procedural AABB BLAS over the scene's lights
    // (inactive boxes for point and spot lights, so PrimitiveIndex = light index), one dynamic TLAS instance with
    // InstanceID RT_INSTANCE_EMITTER, mask kRtMaskEmitter and hit group 1 (RayShaders.hlsli: RtEmitterIntersect).
    void buildEmitters();
    Buffer m_emitterAabbs, m_emitterBlas;
    uint32_t m_emitterLights = 0;
    // Decal query structure (recordDecals): AABBs written by RayTracing/DecalBoxes, the BLAS over them, a one-instance TLAS
    // (descriptor in m_decalDesc), sized for m_decalCapacity decals.
    Buffer m_decalAabbs, m_decalBlas, m_decalBlasScratch, m_decalTlas, m_decalTlasScratch, m_decalDesc;
    uint32_t m_decalCapacity = 0, m_decalTlasSrv = 0xFFFFFFFFu;
    uint64_t m_decalFrame = ~0ull;
    BufferRef m_decalFrames, m_decalTlasRef;
    // Runtime geometry (C2b pool: CARVE fragments, generated meshes; GpuScene::addRuntimeMesh / addRuntimeInstance): one
    // object-space BLAS per runtime mesh generation, built the frame the mesh is first used (the pool m_runtimePool, a
    // first-fit allocator over it), and each live runtime instance a dynamic TLAS instance with its transform - rigid
    // motion is the transform, so nothing is refit and a sleeping fragment costs nothing. Its RtInstance / RtGeometry
    // records live after the load-time ones (m_runtimeRecordBase, m_runtimeGeometryBase) and are copied every frame
    // from an upload ring. Hit shading then follows the static mesh path (GpuScene's vertex, index, submesh records).
    struct RuntimeBlas
    {
        uint64_t generation = 0;           // GpuScene::runtimeMeshGeneration; 0 = none
        uint64_t offset = 0, bytes = 0;    // in m_runtimePool
        uint32_t geometryBase = 0, geometryCount = 0;
        bool anyAlpha = false;
    };
    struct RuntimeFree
    {
        uint64_t frame;                    // reusable once this frame has completed
        uint64_t offset, bytes;
        uint32_t geometryBase, geometryCount;
    };
    std::vector<RuntimeBlas> m_runtimeBlas;  // by mesh index - staticMeshCount()
    std::vector<RuntimeFree> m_runtimeFrees;
    std::vector<std::pair<uint64_t, uint64_t>> m_runtimePoolFree;      // (offset, bytes), sorted
    std::vector<std::pair<uint32_t, uint32_t>> m_runtimeGeometryFree;  // (first, count), sorted
    Buffer m_runtimePool, m_runtimeScratch;
    ComPtr<ID3D12Resource> m_runtimeRing;    // kDescSlots x (instances + geometries) x 16 B of records
    uint8_t* m_runtimeRingMapped = nullptr;
    uint32_t m_runtimeInstanceCap = 0, m_runtimeGeometryCap = 0, m_runtimeRecordBase = 0, m_runtimeGeometryBase = 0;
    std::vector<RtInstance> m_runtimeRecords;
    std::vector<RtGeometry> m_runtimeGeometries;
    std::vector<uint8_t> m_runtimeSeen;      // per GpuScene instance: drawn last frame (appear / disappear -> GI changes)
    uint32_t m_dynamicCountNow = 0;          // dynamic TLAS instances this frame (load-time ones + live runtime ones)
    void setupRuntime();
    // Writes the runtime instances' descriptors after the load-time dynamic ones (slot) and records the frame's BLAS
    // builds and record copy.
    void recordRuntime(FramePassContext& fc, D3D12_RAYTRACING_INSTANCE_DESC* slot);
    // W's triangle streams (R-W2): one BLAS per active stream, rebuilt every frame at the stream's capacity (unused
    // vertices are degenerate: no hit) into m_streamPool, and a dynamic TLAS instance each after the runtime ones.
    void recordStreams(FramePassContext& fc, D3D12_RAYTRACING_INSTANCE_DESC* slot);
    Buffer m_streamPool, m_streamScratch;
    uint64_t m_streamOffset[64] = {}, m_streamBytes[64] = {}, m_streamScratchBytes = 0;
    uint32_t m_streamTriangles[64] = {};
    std::vector<std::pair<uint32_t, BufferRef>> m_streamsNow;
    // The frame's light-grid slot for the header words written after record() (decals); publishes an empty grid when
    // record() had nothing to publish.
    uint8_t* lightSlot(FramePassContext& fc);
    // E's light functions (A8) for the hits' local lights: word 20 of the frame's light-grid header.
    void recordLightFunctions(FramePassContext& fc);
    void recordFxLights(FramePassContext& fc);  // A3 FX particle lights at the hits (word 21)
    void publishLightSlot(FramePassContext& fc);
    bool m_emittersEnabled = false;  // raytracing.emitters
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
        BufferRef tlasStatic, tlasDynamic, deformedBlas, deformedVertices, exactCounts, instances, jobs, lightFunctions, runtimePool, geometries, streamPool;
        BufferRef fxCdf, fxLights;  // A3: the FX lights' groups (FxLightGroups.hlsl) and the scene light buffer they index
        BufferRef hairParams;       // E's hair density volume (recordHair): parameters and the two textures
        TextureRef hairFine, hairCoarse;
        TextureRef hairAmbient, hairDirectional;  // the translucency volume the hair hits read (invalid: none)
    };
    Frame m_frame;
    uint64_t m_hairFrame = ~0ull;    // the frame recordHair ran for
    bool m_hairEnabled = true;       // raytracing.hair

    Device& m_device;
    ShaderLibrary& m_shaders;
    GpuScene& m_scene;
    uint32_t m_sceneRevision = 0;

    struct MeshBlas
    {
        uint64_t offset = 0;  // in the compacted pool of this object's build
        D3D12_GPU_VIRTUAL_ADDRESS address = 0;  // the BLAS (this build's pool, or an inherited one)
        uint32_t geometryBase = gpu::kNone;
        uint32_t alphaMask = 0;  // per submesh (<= 32): alpha-tested when built (an alpha change rebuilds)
        bool anyAlpha = false;
        bool built = false;      // has a BLAS (built here or inherited)
    };
    std::vector<MeshBlas> m_meshBlas;  // per scene mesh (geometryBase kNone = unused)
    Buffer m_meshBlasPool;
    // Scene edits after upload (B3: destruction events, INTERFACES 6.3 v1.44): a revision that keeps the meshes (records
    // and alpha layout) rebuilds this object from the previous one, inheriting its mesh BLASes (their pools stay alive
    // here) and, when the static set is the same, its static TLAS; only new meshes are built.
    std::vector<Buffer> m_inheritedPools;
    std::vector<gpu::Mesh> m_meshRecords;          // the meshes this object's BLASes were built from
    std::vector<gpu::Material> m_materialRecords;  // the materials (a change: GI's lighting epoch)
    std::vector<gpu::Light> m_lightRecords;        // the lights (a change: GI's lighting epoch)
    std::vector<gpu::Instance> m_instanceRecords;  // the instances at build (change bounds of the next rebuild)
    bool m_incremental = false, m_materialsChanged = false, m_lightsChanged = false;
    std::vector<std::pair<float3, float3>> m_changes;       // world AABBs whose geometry changed at this build or this frame
    std::vector<std::pair<float3, float3>> m_buildChanges;  // this build's, handed to the first record
    uint32_t alphaMaskOf(uint32_t mesh) const;

    // RT proxies of a skinned mesh (ARCHITECTURE 2.8): V's uniform-error LOD cuts (ClusterData::lodLevels) with at most
    // raytracing.character_proxy_triangles triangles, finest first (the coarsest cut when none fits; the full mesh when
    // the scene has no cluster data). Each: per-submesh ranges of compact indices in R's index pool, a vertex map (compact
    // -> mesh vertex) so only its vertices are deformed, RtGeometry records, and the cut's object-space error.
    struct ProxyMesh
    {
        bool reduced = false;
        float error = 0;                // V's cut error (bind pose, as claimed)
        ProxyPoseCoefficients pose;     // the cut's error in any pose (ProxyPoseBound.h), measured at load
        uint32_t vertexMap = 0, vertexCount = 0, geometryBase = 0, triangles = 0;
        std::vector<uint32_t> indexOffset, indexCount;  // per geometry (non-empty submesh), in R's index pool
        std::vector<uint32_t> submesh;
    };
    const std::vector<ProxyMesh>& proxyLevels(uint32_t mesh);
    ProxyMesh buildProxy(uint32_t mesh, uint32_t lodLevel, std::vector<std::vector<uint32_t>>* listsOut = nullptr);  // lodLevel = kNone: the full mesh
    ProxyMesh buildProxyFromLists(uint32_t mesh, const std::vector<std::vector<uint32_t>>& perSubmesh, bool reduced, const ProxyPoseCoefficients* pose = nullptr);
    // The CPU work of a mesh's proxy levels (skin-aware or LOD cuts, pose coefficients): cached per mesh and carried into
    // an incremental rebuild for meshes whose record is unchanged (B3: a destruction event rebuilt every character's
    // cuts, ~55 ms [measured]); the pools and geometry records are rebuilt from it, which is cheap.
    struct ProxyRecipe
    {
        std::vector<std::vector<uint32_t>> lists;  // per submesh, mesh vertex indices
        bool reduced = false;
        float error = 0;
        ProxyPoseCoefficients pose;
    };
    struct ProxyCache
    {
        bool valid = false;
        ProxyPoseSkeleton skeleton;
        std::vector<ProxyRecipe> levels;
    };
    std::vector<ProxyCache> m_proxyCache;  // per scene mesh
    std::vector<std::vector<ProxyMesh>> m_proxyLevels;      // per scene mesh (built on demand)
    std::vector<ProxyPoseSkeleton> m_proxySkeletons;        // per scene mesh: joint centres, reference joint
    ProxyPoseTerms m_poseTerms;                             // scratch: this frame's palette terms of one instance
    std::vector<uint8_t> m_proxyLevelsBuilt;
    std::vector<uint32_t> m_indexPoolData, m_vertexMapData;
    uint32_t m_proxyBudget = 0;
    float m_proxyErrorPx = 1;  // raytracing.proxy_error_px
    float m_proxySkinWeight = 1;  // raytracing.proxy_skin_weight (skinAwareCuts)
    bool m_skinAwareCuts = true;  // raytracing.skinned_proxy_cuts
    float m_proxyPosedFactor = 1.5f;  // raytracing.proxy_posed_factor
    bool m_proxyErrorBound = false;   // raytracing.proxy_error_model = "bound"
    uint32_t m_experiment = 0; // raytracing.experiment_disable (cost attribution only): 1 = build the deformed BLASes every frame
    // Per frame: each deformed instance's cut, the coarsest whose error in the instance's current pose (ProxyPoseBound.h:
    // bind-pose error plus the joints' relative motion times the cut's weight mismatch; raytracing.experiment_disable 16:
    // V's bind-pose error alone, the previous rule) x the instance's scale is at most
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
        float exactNeed = 0;  // the finest cut's error over its bound at the last view (> 1: the exact set may help)
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
    // Local lights for next-event estimation at ray hits (HitLocalLights.hlsli, the reference estimator's shared
    // Lights.hlsli): the scene lights normalised like the reference's LightSet and a uniform cell grid of the lights whose
    // range reaches each cell, rebuilt on the CPU when the lights change; one upload-ring slot per frame in flight.
    void updateLightGrid(FramePassContext& fc);
    // Emissive triangles of rigid instances (GI next-event estimation with MIS, HitLocalLights.hlsli): per triangle of an
    // instance with an emissive submesh (instance, mesh triangle, submesh, running weight = emission luminance x world
    // area at build), the total, and per scene instance its first entry (0xFFFFFFFF: none). Rebuilt on a scene revision;
    // its SRV is word 15 of the local-light data's header.
    void updateEmissive(FramePassContext& fc);
    ComPtr<ID3D12Resource> m_emissive;
    uint32_t m_emissiveSrv = 0xFFFFFFFFu, m_emissiveRevision = 0xFFFFFFFFu;
    std::vector<uint8_t> m_lightImage;       // header 80 B, lights (96 B each), cell starts (cells + 1), cell lights
    uint64_t m_lightHash = 0, m_lightVersion = 0;
    uint32_t m_lightCellLongestLogged = 0xFFFFFFFFu;  // the light grid's longest cell list at the last log line
    uint64_t m_lightSlotVersion[4] = {};     // kDescSlots
    ComPtr<ID3D12Resource> m_lightRing;
    uint8_t* m_lightRingMapped = nullptr;
    uint64_t m_lightSlotBytes = 0;
    uint32_t m_lightRingSrv[4] = {};         // kDescSlots
    uint32_t m_lightSrvNow = 0xFFFFFFFFu;
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
