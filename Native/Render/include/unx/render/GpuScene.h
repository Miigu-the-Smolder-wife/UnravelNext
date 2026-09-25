#pragma once
// GPU representation of a scene::Scene (INTERFACES_KO.md 6.3). Owner: core. Tracks read it through FrameConstants
// (Scene.hlsli accessors) and the C++ accessors below; V's cluster builder output is installed with setClusters().
#include "unx/render/Device.h"
#include "unx/render/GpuSceneLayout.h"
#include "unx/scene/SceneData.h"

#include <span>
#include <string>
#include <utility>
#include <vector>

namespace unx::render
{
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
};

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
    void updateSkeleton(uint64_t frameIndex, uint32_t skeleton, std::span<const float3x4> jointToModel);
    void setInstanceVisible(uint32_t instance, bool visible);
    // Material textures published by M's texture system (INTERFACES_KO.md 6.3, v1.10): one entry per scene material.
    // Rewrites the material buffer (new SRV; the old one is released when the GPU is done) and bumps the revision of the
    // materials whose textures changed and the scene revision. Call before any frame constants of the frame are
    // allocated (tracks::prepareScene), since they carry the material buffer's SRV.
    void setMaterialTextures(const std::vector<gpu::MaterialTextures>& perMaterial);
    const std::vector<gpu::Material>& materials() const { return m_materials; }
    // Uploads the changes for 'frameIndex': a scatter kernel on the graphics queue, submitted before the frame's graph;
    // the other queues wait for it. Upload slot frameIndex % framesInFlight (the caller waited for that slot's frame).
    void flushUpdates(uint64_t frameIndex, uint32_t framesInFlight, ShaderLibrary& shaders);

    const scene::Scene* source() const { return m_source; }
    const std::vector<gpu::Instance>& instances() const { return m_instances; }
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
    Buffer createStructured(const void* data, size_t stride, size_t count, const wchar_t* name, bool uav = false);
    void release(Buffer& b);
    void markRecord(uint32_t instance);
    void writePalette(uint32_t instance, std::vector<float4>& palette);  // jointToModel x inverseBind of its skeleton
    uint32_t paletteJoints(uint32_t instance) const;
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
    std::vector<gpu::Mesh> m_meshes;
    std::vector<gpu::Material> m_materials;
    Buffer m_instanceBuffer, m_meshBuffer, m_submeshBuffer, m_vertexBuffer, m_indexBuffer, m_materialBuffer, m_materialRemapBuffer,
        m_lightBuffer, m_skinBuffer, m_bonePalette, m_prevBonePalette, m_albedoTable;
    Buffer m_clusterBuffer, m_lodLevelBuffer, m_lodLevelClusterBuffer, m_clusterVertexIndexBuffer, m_clusterTriangleBuffer;
    std::vector<std::pair<std::string, Buffer>> m_named;
    ClusterData m_clusterData;
    uint32_t m_revision = 0;

    // Per-frame updates: CPU mirrors of the palettes, the frame of each instance's latest change, the instances changed
    // in this frame and in the previous flushed one (they settle: prev = current), records to upload.
    std::vector<float4> m_palette, m_prevPalette;
    std::vector<scene::Skeleton> m_poses;  // current joint-to-model per skeleton
    std::vector<uint64_t> m_transformFrame, m_paletteFrame;
    std::vector<uint32_t> m_movedNow, m_movedBefore, m_posedNow, m_posedBefore;
    std::vector<uint32_t> m_records;
    std::vector<uint8_t> m_recordMarked;
    std::vector<Upload> m_uploads;
    uint32_t m_instanceUav = gpu::kNone, m_paletteUav = gpu::kNone, m_prevPaletteUav = gpu::kNone;
};
} // namespace unx::render
