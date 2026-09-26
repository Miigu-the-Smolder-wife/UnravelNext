#pragma once
// GPU representation of a scene::Scene (INTERFACES_KO.md 6.3). Owner: core. Tracks read it through FrameConstants
// (Scene.hlsli accessors) and the C++ accessors below; V's cluster builder output is installed with setClusters().
#include "unx/render/Device.h"
#include "unx/render/GpuSceneLayout.h"
#include "unx/scene/SceneData.h"

#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace unx::render
{
// Edge half-plane masks of the coverage mask LUT (Coverage.hlsli coverageTriangleMaskLut, INTERFACES 5.5.1; design
// revision 1, 11 b): kCoverageLutAngles x kCoverageLutDistances entries of two uint32 masks (sure inside, sure outside),
// row-major by angle bin. The inward normal n (upper half plane) is binned by its pseudo-angle pa = n.x / (|n.x| + n.y)
// in [-1, 1]: bin k holds pa in [1 - (k + 1) / 32, 1 - k / 32]; the pixel centre's signed distance inside the edge h
// (unit n) by j: h in [-R + j D, -R + (j + 1) D], D = 2R / distances, R = sqrt(2) / 2. Bit i of 'inside' = subsample i
// (coverageSample) is inside for every (n, h) of the bin by more than kCoverageLutMargin px, of 'outside' = outside for
// every one by more than it; the rest the shader tests exactly. Published as FrameConstants::coverageMaskLut.
constexpr uint32_t kCoverageLutAngles = 64, kCoverageLutDistances = 64;
constexpr float kCoverageLutMargin = 1.0f / 128;  // px: covers float error of the edge functions up to ~30,000 px
const std::vector<uint32_t>& coverageMaskTable();  // 2 words per entry

struct ClusterData  // V's builder output for the whole scene (per-mesh ranges go into gpu::Mesh)
{
    std::vector<gpu::Cluster> clusters;
    std::vector<gpu::LodLevel> lodLevels;
    std::vector<uint32_t> lodLevelClusters;       // cluster indices of every LodLevel cut (LodLevel::clusterOffset/Count)
    std::vector<uint32_t> clusterVertexIndices;
    std::vector<uint32_t> clusterTriangles;
    struct MeshRange
    {
        uint32_t clusterOffset = 0, clusterCount = 0, lodLevelOffset = 0, lodLevelCount = 0;
    };
    std::vector<MeshRange> meshes;  // one per scene mesh
    // V-internal buffers (hierarchy nodes, ...): uploaded as StructuredBuffers, found with GpuScene::srv(name).
    struct Named
    {
        std::string name;
        uint32_t stride = 0;
        std::vector<uint8_t> bytes;
    };
    std::vector<Named> named;
};

class ShaderLibrary;

struct InstanceTransformUpdate
{
    uint32_t instance = 0;
    float3x4 objectToWorld;
    uint32_t flags = 0;  // kTransformTeleport: the instance jumped (prevObjectToWorld = objectToWorld, zero motion)
};
constexpr uint32_t kTransformTeleport = 1;

// C2b (render C): room for meshes and instances added at run time (CARVE destruction fragments, generated geometry):
// every scene buffer keeps a tail of this many elements past the uploaded content.
struct RuntimeCapacity
{
    uint32_t meshes = 0, submeshes = 0, vertices = 0, indices = 0;
    uint32_t clusters = 0, clusterVertexIndices = 0, clusterTriangles = 0, nodes = 0;
    uint32_t instances = 0;
    // GPU-written instances (A3 mesh particles, fx.mesh_instances_max): records the GPU fills every frame after the
    // CPU-known ones; see gpuInstanceRange.
    uint32_t gpuInstances = 0;
};
constexpr uint32_t kRuntimeMaxDepth = 6;  // hierarchy depth limit of a runtime mesh (V runs at least this many node passes)

class GpuScene
{
public:
    explicit GpuScene(Device& device);
    ~GpuScene();

    // Packs and uploads the whole scene (blocking; load time only). Keeps a pointer to 'scene' for CPU readers.
    void upload(const scene::Scene& scene);
    // Installs V's cluster hierarchy (replaces the placeholder buffers). A CPU copy is kept for readers (R: BLAS inputs).
    void setClusters(ClusterData clusters);
    const ClusterData& clusters() const { return m_clusterData; }
    // Scene indices and counts of FrameConstants.
    void fill(gpu::FrameConstants& constants) const;

    // Per-frame updates (INTERFACES_KO.md 6.3, v1.8). Called between frames for the frame about to be recorded
    // ('frameIndex'); FrameRenderer::record applies them first (flushUpdates). "Previous" always means the previous
    // rendered frame:
    //   updateTransforms: the listed instances get objectToWorld; prevObjectToWorld = their objectToWorld of the previous
    //                     rendered frame; transformRevision += 1 (once per frame). An instance that moved in the previous
    //                     frame and not in this one gets prev = current, so its motion is zero once it stops.
    //   updateSkeleton:   joint-to-model transforms of a skeleton; every skinned instance using it gets a new bone
    //                     palette (jointToModel x inverseBind), the previous palette = the previous rendered frame's,
    //                     deformRevision += 1; the same settling rule as transforms.
    //   setInstanceVisible: hidden instances carry gpu::kInstanceHidden; every reader skips them (V culling, R's TLAS).
    // The CPU mirror (instances()) is updated immediately.
    void updateTransforms(uint64_t frameIndex, std::span<const InstanceTransformUpdate> updates);
    // History discontinuity (FrameContext::discontinuity, kDiscontinuityRestore; FrameRenderer calls it before
    // flushUpdates): this frame's previous transforms and palettes = the current ones, so no instance has motion.
    void resetMotion();
    // The current frame's bone palette of a skinned instance (CPU copy): paletteJoints(instance) joints of 3 float4 rows
    // (row-major 3 x 4, jointToModel x inverseBind); empty for other instances. Valid until the next updateSkeleton.
    std::span<const float4> palette(uint32_t instance) const;
    uint32_t paletteJoints(uint32_t instance) const;
    void updateSkeleton(uint64_t frameIndex, uint32_t skeleton, std::span<const float3x4> jointToModel);
    void setInstanceVisible(uint32_t instance, bool visible);
    // v1.53 (A12): marks a first-person view model (gpu::kInstanceViewModel; E's Passes/ViewModel sets it every frame, so
    // a host re-upload of the instance loses nothing).
    void setInstanceViewModel(uint32_t instance, bool viewModel);
    uint32_t viewModelInstances() const { return m_viewModelInstances; }  // instances marked now (A5: the rotation blur's exclusion)
    // Scene motion of the frame being recorded (after flushUpdates; A5 motion blur): an instance moved or re-posed in this
    // frame, or wind-animated instances (they move every frame). The camera's own motion is the view's (prevViewProj).
    bool hasMotion() const { return !m_movedNow.empty() || !m_posedNow.empty() || m_windInstances > 0; }
    // Instances whose transform changed in the frame being recorded (after flushUpdates; C3: V refreshes its chunk spheres).
    const std::vector<uint32_t>& movedInstances() const { return m_movedNow; }
    // Changes of the view-model flag (setInstanceViewModel; C3: V rebuilds its instance hierarchy with view models flat).
    uint64_t viewModelRevision() const { return m_viewModelRevision; }
    // C2b runtime pool (call reserveRuntime before upload): meshes and instances added between frames without a scene
    // revision (no track rebuilds). addRuntimeMesh takes the mesh and its own cluster hierarchy (clusterbuilder::build of
    // a one-mesh scene, depth <= kRuntimeMaxDepth) and returns its mesh index, kNone when the pool is full. A runtime
    // instance takes a slot after the uploaded instances (staticInstanceCount()), reusing freed slots; removal hides it
    // and frees the slot framesInFlight + 1 flushes later (frames in flight may still read it); a removed mesh's ranges
    // are reused the same way (the caller removes its instances first). Ray tracing does not see runtime geometry yet
    // (R's event BLAS, FEATURES_GAME 2.1); raster, shadows and materials do.
    void reserveRuntime(const RuntimeCapacity& capacity);
    uint32_t addRuntimeMesh(const scene::Mesh& mesh, const ClusterData& clusters);
    void removeRuntimeMesh(uint32_t mesh);
    uint32_t addRuntimeInstance(const scene::Instance& instance);
    void removeRuntimeInstance(uint32_t instance);
    uint32_t staticInstanceCount() const { return m_staticInstances; }
    // C5 terrain deformation (scene/TerrainPatch.h): the replaced blocks of a terrain tile instance (the host adds their
    // patch meshes as runtime geometry in the same frame). V forces the instance's source clusters inside the rectangle
    // of the replaced blocks and drops its source triangles whose centroid is in a replaced block. An empty block list
    // clears it (the slot is reused framesInFlight + 1 flushes later). Needs the runtime pool; kPatchSlots instances.
    struct PatchRegion
    {
        float originX = 0, originZ = 0;  // object xz of the block grid's corner (the tile's vertex 0)
        float blockX = 0, blockZ = 0;    // object size of a block along x and z (signed like the grid)
        uint32_t blocksPerSide = 0;
        std::vector<uint32_t> blocks;    // replaced blocks, bj * blocksPerSide + bi
    };
    void setPatchRegion(uint32_t instance, const PatchRegion& region);
    // GPU-written instance range (A3 mesh particles, INTERFACES v1.58): instance ids [first, first + capacity) at the tail
    // of the instance buffer, after the uploaded and the C2b runtime ones. A pass before V's culling writes gpu::Instance
    // records there through instanceUav (raw, the whole instance buffer) and the live count (uint) at countByteOffset
    // through countUav (raw); the count is zeroed by every frame's scene update, so a frame without the writer draws none.
    // Records: prevObjectToWorld set, flags without SKINNED / WIND, bonePalette, morph, patch = kNone. V culls them like
    // the other dynamic instances; readers of instances see them through the usual loadInstance. The range moves when
    // scene edits change the uploaded count: query it every frame. capacity 0: none.
    struct GpuInstanceRange
    {
        uint32_t first = 0, capacity = 0;
        uint32_t instanceUav = gpu::kNone, countUav = gpu::kNone, countByteOffset = 0;
        // For the writer's barriers: both are read as shader resources during the frame (after the scene update).
        ID3D12Resource* instanceBuffer = nullptr;
        ID3D12Resource* countBuffer = nullptr;
    };
    GpuInstanceRange gpuInstanceRange() const;
    // A3 FX particle lights (v1.79, S_STATUS 10): a tail of 'capacity' gpu::Light records after the scene's lights that the
    // FX module writes every frame on the GPU (point lights, renderer world, castShadow 0 / shadowIndex 0xFFFF) with its
    // count F in the count buffer's element 0 (0 after a capacity change). Readers loop to lightCount + min(F, capacity)
    // (FrameConstants::fxLightCount / fxLightCapacity). setFxLightCapacity rebuilds the light buffer when the capacity
    // changes (the FX stream's light rows changed) and refuses a total above kMaxSceneLights (S's 16-bit light list
    // entries carry the shadow flag in bit 15, so light indices end at 0x7FFF).
    struct FxLightRange
    {
        uint32_t first = 0, capacity = 0;
        uint32_t lightUav = gpu::kNone, countUav = gpu::kNone, countSrv = gpu::kNone;  // raw UAVs; count SRV (StructuredBuffer<uint>)
        // For the writer's barriers: both are read as shader resources during the frame.
        ID3D12Resource* lightBuffer = nullptr;
        ID3D12Resource* countBuffer = nullptr;
    };
    static constexpr uint32_t kMaxSceneLights = 32768;
    bool setFxLightCapacity(uint32_t capacity);
    FxLightRange fxLightRange() const;
    uint32_t staticMeshCount() const { return m_staticMeshes; }
    // C2b readers for ray tracing (R's runtime BLAS, B's request): the submesh records of a live runtime mesh (mesh-relative
    // index ranges, as the GPU table holds them; empty when the mesh is not a live runtime mesh), and the slot's generation:
    // it increases each time a runtime mesh is added into that slot, so a reused slot is told apart. Runtime instances are
    // instances()[i] for i >= staticInstanceCount(); a removed one carries gpu::kInstanceHidden until its slot is reused.
    std::vector<gpu::Submesh> runtimeSubmeshes(uint32_t mesh) const;
    uint64_t runtimeMeshGeneration(uint32_t mesh) const;
    const RuntimeCapacity& runtimeCapacity() const { return m_runtimeCap; }
    //   setMorph (C4):    blend shape weights (one per shape of the instance's mesh) and vertex animation time of a morph
    //                     instance; the previous weights / time = the previous rendered frame's, deformRevision += 1, the
    //                     culling radius covers both; the same settling rule as palettes.
    void setMorph(uint64_t frameIndex, uint32_t instance, std::span<const float> weights, float vertexAnimationTime);
    // Origin rebase (C9): every instance's current and previous transform, its break centre and every light move by
    // -shift (no motion: both move), exactly (shift is a multiple of 1024 m, float translations stay exact where they
    // are representable after the move). The instance table is shifted on the GPU by the next flushUpdates (one pass
    // over the instances, no re-upload); lights are re-uploaded. Call before the frame's transform updates.
    void rebase(float3 shift);
    float3 originOffset() const { return m_originOffset; }  // sum of the shifts applied since upload
    // Material textures published by M's texture system (INTERFACES_KO.md 6.3, v1.10): one entry per scene material.
    // Rewrites the material buffer (new SRV; the old one is released when the GPU is done) and bumps the revision of the
    // materials whose textures changed and the scene revision. Call before any frame constants of the frame are
    // allocated (tracks::prepareScene), since they carry the material buffer's SRV.
    void setMaterialTextures(const std::vector<gpu::MaterialTextures>& perMaterial);
    // Scene edits after upload (v1.44, INTERFACES 6.3; I request 20260926_I_game_features 1-2, D0). The caller first
    // changes its scene (the source), then names what changed:
    //   setInstances: the instances at 'indices' are packed from source instances[i]; i == instances().size() appends (in
    //                 order). Transform with no motion (previous = current), mesh (already uploaded: a new mesh needs
    //                 upload), flags (the instance is visible again), material overrides, wind. Skinned instances need
    //                 upload. Removal is setInstanceVisible(false); the caller keeps the free slots and reuses them here.
    //   setMaterials: the materials at 'indices' are packed from source materials[i] (i == materials().size() appends);
    //                 their published textures stay until M's texture system republishes them. Textures added to the
    //                 source are picked up with the next setMaterials (the revision).
    // Both bump the scene revision, so every track rebuilds what it derives from the scene (V, R's ray scene, S's pages,
    // the GI epoch; M re-uploads textures only when their content or references changed). Blocking (edit time).
    void setInstances(std::span<const uint32_t> indices);
    void setMaterials(std::span<const uint32_t> indices);
    const std::vector<gpu::Material>& materials() const { return m_materials; }
    // Uploads the changes for 'frameIndex': a scatter kernel on the graphics queue, submitted before the frame's graph;
    // the other queues wait for it. Upload slot frameIndex % framesInFlight (the caller waited for that slot's frame).
    void flushUpdates(uint64_t frameIndex, uint32_t framesInFlight, ShaderLibrary& shaders);

    const scene::Scene* source() const { return m_source; }
    const std::vector<gpu::Instance>& instances() const { return m_instances; }
    const std::vector<gpu::Light>& lights() const { return m_lights; }  // CPU mirror of the light records (revisions)
    // A9: a material of the scene is anisotropic (the anisotropy table is in coatTable; M's resolve writes the frame word)
    bool anyAnisotropic() const { return m_anisotropic; }
    const std::vector<gpu::Mesh>& meshes() const { return m_meshes; }
    uint32_t revision() const { return m_revision; }
    ID3D12Resource* buffer(const char* name) const;  // "vertices", "indices", "instances", "bonePalette", "prevBonePalette", ...
    uint32_t srv(const char* name) const;             // bindless SRV of cluster buffers and of ClusterData::named

private:
    struct Buffer
    {
        ComPtr<ID3D12Resource> resource;
        uint32_t srv = gpu::kNone;
        uint32_t count = 0;
    };
    Buffer createStructured(const void* data, size_t stride, size_t count, const wchar_t* name, bool uav = false, size_t capacity = 0);
    void release(Buffer& b);
    void markRecord(uint32_t instance);
    void writePalette(uint32_t instance, std::vector<float4>& palette);  // jointToModel x inverseBind of its skeleton
    gpu::Instance packInstance(const scene::Instance& in, std::vector<float4>* palette);  // overrides appended to m_remap
    gpu::Material packMaterial(const scene::Material& m) const;
    void rawUav(uint32_t& index, const Buffer& b, uint64_t bytes, bool fresh);  // fresh: a new descriptor, the old freed later
    void createLightBuffer(const std::vector<gpu::Light>& lights);  // lights + the FX tail (m_fxLightCapacity)
    void createFxLightCount();
    struct Upload
    {
        ComPtr<ID3D12Resource> buffer;
        uint8_t* mapped = nullptr;
        uint64_t bytes = 0;
        uint32_t srv = gpu::kNone;
    };

    Device& m_device;
    const scene::Scene* m_source = nullptr;
    std::vector<gpu::Instance> m_instances;
    std::vector<gpu::Light> m_lights;
    std::vector<gpu::Mesh> m_meshes;
    std::vector<gpu::Material> m_materials;
    std::vector<uint32_t> m_remap;  // material override table (gpu::Instance::materialRemap)
    Buffer m_instanceBuffer, m_meshBuffer, m_submeshBuffer, m_vertexBuffer, m_indexBuffer, m_materialBuffer, m_materialRemapBuffer,
        m_lightBuffer, m_skinBuffer, m_bonePalette, m_prevBonePalette, m_albedoTable, m_specularTable, m_coverageTable;
    uint32_t m_fxLightCapacity = 0, m_lightUav = gpu::kNone, m_fxCountUav = gpu::kNone;  // A3 FX light tail
    Buffer m_fxLightCount;
    Buffer m_clusterBuffer, m_lodLevelBuffer, m_lodLevelClusterBuffer, m_clusterVertexIndexBuffer, m_clusterTriangleBuffer;
    // C4 morphs: records (float4 rows, updatable), mesh data (raw words), CPU mirrors.
    Buffer m_morphRecords, m_morphData;
    // v1.74 Terrain-class material layers (gpu::TerrainLayer; each terrain material's terrainLayers word points here).
    Buffer m_terrainLayerBuffer;
    void packTerrainLayers(std::vector<gpu::Material>& materials);
    // v1.76 A9 layer records (gpu::MaterialLayers; a layered material's classFlags bits 16..31) and the coat tables.
    Buffer m_materialLayerBuffer, m_coatTable;
    void packMaterialLayers(std::vector<gpu::Material>& materials);
    void buildLayerTables(bool anisotropic);
    bool m_anisotropic = false;
    std::vector<float4> m_morphRows;
    std::vector<uint32_t> m_morphMeshBlock;  // per mesh: word offset of its block in m_morphData, kNone = no morph
    std::vector<uint64_t> m_morphFrame;      // per instance: frame of its latest setMorph
    std::vector<uint32_t> m_morphedNow, m_morphedBefore;
    uint32_t m_morphUav = gpu::kNone;
    void writeMorphRows(uint32_t instance, bool settle);  // previous = current (settle) and the rows' upload marks
    std::vector<uint32_t> m_morphRowsDirty;
    float morphRadiusOf(uint32_t instance) const;
    std::vector<std::pair<std::string, Buffer>> m_named;
    ClusterData m_clusterData;
    uint32_t m_revision = 0;

    // Per-frame updates: CPU mirrors of the palettes, the frame of each instance's latest change, the instances changed
    // in this frame and in the previous flushed one (they settle: prev = current), records to upload.
    std::vector<float4> m_palette, m_prevPalette;
    std::vector<scene::Skeleton> m_poses;  // current joint-to-model per skeleton
    std::vector<uint64_t> m_transformFrame, m_paletteFrame;
    std::vector<uint32_t> m_movedNow, m_movedBefore, m_posedNow, m_posedBefore;
    uint32_t m_windInstances = 0;  // instances with the wind flag (hasMotion)
    uint32_t m_viewModelInstances = 0;
    uint64_t m_viewModelRevision = 0;  // instances with gpu::kInstanceViewModel
    // Instances flagged gpu::kInstanceMotionBreak in this frame and in the previous flushed one (the flag lasts one frame).
    std::vector<uint32_t> m_brokenNow, m_brokenBefore;
    std::vector<uint8_t> m_brokenMarked;
    void markBreak(uint32_t instance, const float4 (&before)[3]);
    std::vector<uint32_t> m_records;
    // C2b runtime pool: capacity, first element of each runtime region, free ranges, CPU mirrors of the written ranges.
    struct RangeAllocator
    {
        std::vector<std::pair<uint32_t, uint32_t>> free;  // (first, count), sorted
        uint32_t allocate(uint32_t count);                // kNone when no range fits
        void release(uint32_t first, uint32_t count);
    };
    enum RuntimeTarget : uint32_t { RtMeshes = 4, RtSubmeshes, RtVertices, RtIndices, RtClusters, RtClusterVertexIndices, RtClusterTriangles, RtNodes, RtRoots, RtSpheres, RtSheets, RtPatch, RtTargetEnd };
    Buffer m_patchData;                      // C5 terrain patch slots (target RtPatch)
    std::vector<uint32_t> m_patchSlotOf;     // per instance, kNone
    std::vector<uint32_t> m_patchFreeSlots;
    RuntimeCapacity m_runtimeCap;
    uint32_t m_staticInstances = 0, m_staticMeshes = 0;
    RangeAllocator m_rtAlloc[RtTargetEnd];  // per target (meshes: mesh slots; roots follow the mesh slots)
    std::vector<uint8_t> m_rtBytes[RtTargetEnd];      // mirror of each target's whole runtime region
    uint32_t m_rtFirst[RtTargetEnd] = {};             // first element of the runtime region (target element size)
    uint32_t m_rtStride[RtTargetEnd] = {};
    uint32_t m_rtUav[RtTargetEnd];
    std::vector<std::pair<uint32_t, uint32_t>> m_rtDirty;  // (target, 16-byte element within the whole buffer)
    struct RuntimeMesh
    {
        bool live = false;
        uint64_t generation = 0;  // adds into this slot so far
        uint32_t vertices = 0, vertexCount = 0, indices = 0, indexCount = 0, submeshes = 0, submeshCount = 0;
        uint32_t clusters = 0, clusterCount = 0, cvi = 0, cviCount = 0, ct = 0, ctCount = 0, nodes = 0, nodeCount = 0;
    };
    std::vector<RuntimeMesh> m_runtimeMeshes;  // by mesh index - staticMeshCount
    std::vector<uint8_t> m_runtimeInstanceLive;
    std::vector<std::pair<uint64_t, std::function<void()>>> m_rtReleases;  // (flush count at which it is safe, release)
    uint64_t m_flushes = 0;
    uint32_t m_framesInFlightSeen = 2;
    void writeRuntime(uint32_t target, uint32_t element, const void* data, uint32_t count);  // element in target units
    Buffer* runtimeBuffer(uint32_t target);
    float3 m_pendingShift{}, m_originOffset{};
    std::vector<uint8_t> m_recordMarked;
    std::vector<Upload> m_uploads;
    uint32_t m_instanceUav = gpu::kNone, m_paletteUav = gpu::kNone, m_prevPaletteUav = gpu::kNone;
};
} // namespace unx::render
