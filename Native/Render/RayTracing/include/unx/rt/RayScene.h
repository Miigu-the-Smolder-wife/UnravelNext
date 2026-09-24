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
    uint64_t deformedTriangles = 0;        // triangles refit per frame
    uint64_t deformedVertices = 0;         // vertices deformed per frame
    uint64_t meshBlasBytes = 0, meshBlasBytesBeforeCompaction = 0;
    uint64_t deformedBlasBytes = 0, tlasStaticBytes = 0, tlasDynamicBytes = 0;
    double loadMs = 0;                     // CPU wall time of the load-time build (includes GPU waits)
    uint32_t deformedAboveProxyBudget = 0; // deformed meshes with more triangles than raytracing.character_proxy_triangles
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
    void recordRefit(ID3D12GraphicsCommandList7* cmd, bool refit) const;
    void recordDynamicTlas(ID3D12GraphicsCommandList7* cmd);

    struct Frame  // graph references of the current frame
    {
        BufferRef tlasStatic, tlasDynamic, deformedBlas, deformedVertices;
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

    struct Deformed
    {
        uint32_t sceneInstance = 0;
        uint32_t vertexBase = 0, vertexCount = 0;
        uint64_t blasOffset = 0, scratchOffset = 0;
        uint32_t geometryBase = 0;
        std::vector<D3D12_RAYTRACING_GEOMETRY_DESC> geometries;
    };
    std::vector<Deformed> m_deformed;
    Buffer m_deformedPool, m_deformedBlasPool, m_deformedScratch, m_deformJobs, m_deformGroups;
    uint32_t m_deformGroupCount = 0;
    uint32_t m_deformedPoolUav = gpu::kNone;

    std::vector<RtInstance> m_instances;
    std::vector<RtGeometry> m_geometries;
    std::vector<D3D12_RAYTRACING_INSTANCE_DESC> m_staticDescs, m_dynamicDescs;
    std::vector<uint32_t> m_dynamicRecord;  // RtInstance index of each dynamic TLAS instance
    Buffer m_instanceBuffer, m_geometryBuffer, m_indexPool, m_vertexMap;
    Buffer m_tlasStatic, m_tlasDynamic, m_tlasScratch, m_staticDescBuffer, m_dynamicDescBuffer;
    uint32_t m_tlasStaticSrv = gpu::kNone, m_tlasDynamicSrv = gpu::kNone;
    RaySceneStats m_stats;
};
} // namespace unx::render::rt
