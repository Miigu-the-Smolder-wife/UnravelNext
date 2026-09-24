#pragma once
// GPU representation of a scene::Scene (INTERFACES_KO.md 6.3). Owner: core. Tracks read it through FrameConstants
// (Scene.hlsli accessors) and the C++ accessors below; V's cluster builder output is installed with setClusters().
#include "unx/render/Device.h"
#include "unx/render/GpuSceneLayout.h"
#include "unx/scene/SceneData.h"

#include <vector>

namespace unx::render
{
struct ClusterData  // V's builder output for the whole scene (per-mesh ranges go into gpu::Mesh)
{
    std::vector<gpu::Cluster> clusters;
    std::vector<gpu::LodLevel> lodLevels;
    std::vector<uint32_t> clusterVertexIndices;
    std::vector<uint32_t> clusterTriangles;
    struct MeshRange
    {
        uint32_t clusterOffset = 0, clusterCount = 0, lodLevelOffset = 0;
    };
    std::vector<MeshRange> meshes;  // one per scene mesh
};

class GpuScene
{
public:
    explicit GpuScene(Device& device);
    ~GpuScene();

    // Packs and uploads the whole scene (blocking; load time only). Keeps a pointer to 'scene' for CPU readers.
    void upload(const scene::Scene& scene);
    // Installs V's cluster hierarchy (replaces the placeholder buffers).
    void setClusters(const ClusterData& clusters);
    // Scene indices and counts of FrameConstants.
    void fill(gpu::FrameConstants& constants) const;

    const scene::Scene* source() const { return m_source; }
    const std::vector<gpu::Instance>& instances() const { return m_instances; }
    const std::vector<gpu::Mesh>& meshes() const { return m_meshes; }
    uint32_t revision() const { return m_revision; }
    ID3D12Resource* buffer(const char* name) const;  // "vertices", "indices", ... (R builds BLAS from them)

private:
    struct Buffer
    {
        ComPtr<ID3D12Resource> resource;
        uint32_t srv = gpu::kNone;
        uint32_t count = 0;
    };
    Buffer createStructured(const void* data, size_t stride, size_t count, const wchar_t* name);
    void release(Buffer& b);

    Device& m_device;
    const scene::Scene* m_source = nullptr;
    std::vector<gpu::Instance> m_instances;
    std::vector<gpu::Mesh> m_meshes;
    Buffer m_instanceBuffer, m_meshBuffer, m_submeshBuffer, m_vertexBuffer, m_indexBuffer, m_materialBuffer, m_materialRemapBuffer,
        m_lightBuffer, m_skinBuffer, m_bonePalette, m_prevBonePalette, m_albedoTable;
    Buffer m_clusterBuffer, m_lodLevelBuffer, m_clusterVertexIndexBuffer, m_clusterTriangleBuffer;
    uint32_t m_revision = 0;
};
} // namespace unx::render
