#pragma once
// Embree representation of a scene::Scene and surface evaluation (INTERFACES_KO.md 6, 8.1).
//   Shared meshes: one Embree scene per mesh, placed by instance arrays grouped by (mesh, shadow flag).
//   Deformed instances (skinned, or wind with nonzero displacement at the render time): their own world-space
//   geometry, deformed with the same functions as Passes/Common/Deformation.hlsli (skin: linear blend, 4 joints;
//   wind v1: windOffset). Their triangle count is bounded (kMaxDeformedTriangles); beyond it the reference refuses the
//   scene instead of rendering it undeformed.
//   Ray masks: bit 0 every instance, bit 1 shadow casters (InstanceCastShadow); shadow rays use bit 1.
#include "Atmosphere.h"

#include "unx/scene/MaterialModel.h"
#include "unx/scene/SceneData.h"

#include <embree4/rtcore.h>

#include <cstdint>
#include <memory>
#include <vector>

namespace unx::reference
{
constexpr uint32_t kMaskAll = 1u, kMaskShadow = 2u;

struct Hit
{
    uint32_t instance = ~0u;  // scene instance index
    uint32_t triangle = 0;    // mesh triangle index
    float u = 0, v = 0, t = 0;
};

struct Surface
{
    float3 p;        // world position
    float3 ng;       // geometric normal, flipped towards the viewer for two-sided materials
    float3 ns;       // shading normal (normal map applied), same side as ng
    bool frontFacing = true;   // the viewer is on the ng side (always true for two-sided)
    scene::model::Surface bsdf;
    Rgb emission;
    uint32_t material = 0;
};

struct Texel
{
    float r, g, b, a;
};

class Texture
{
public:
    explicit Texture(const scene::Texture& t);
    Texel sample(float2 uv) const;  // bilinear on mip 0, wrap or clamp as the texture says; sRGB decoded
private:
    uint32_t m_w, m_h;
    bool m_wrap;
    std::vector<Texel> m_texels;
};

struct DeformationReport
{
    uint32_t skinnedInstances = 0, windInstances = 0;
    uint64_t deformedTriangles = 0;
};

class RtScene
{
public:
    static constexpr uint64_t kMaxDeformedTriangles = 150'000'000;

    RtScene(const scene::Scene& scene, float time, uint32_t threads);
    ~RtScene();
    RtScene(const RtScene&) = delete;
    RtScene& operator=(const RtScene&) = delete;

    bool intersect(float3 o, float3 d, float tnear, float tfar, uint32_t mask, Hit& hit) const;
    bool occluded(float3 o, float3 d, float tnear, float tfar) const;  // shadow casters only
    Surface surface(const Hit& hit, float3 rayDir) const;

    const scene::Scene& scene() const { return m_scene; }
    const DeformationReport& deformation() const { return m_deform; }
    bool alphaOpaque(uint32_t instance, uint32_t triangle, float u, float v) const;
    uint32_t instanceOf(uint32_t topGeom, uint32_t instPrim) const;  // instance-array hit -> scene instance
    bool deformed(uint32_t instance) const { return m_instanceDeformed[instance] >= 0; }  // own world-space geometry

    struct MeshData;
    struct Group;

private:
    const MeshData& meshOf(uint32_t instance) const;
    uint32_t materialOf(uint32_t instance, const MeshData& md, uint32_t triangle) const;

    const scene::Scene& m_scene;
    RTCDevice m_device = nullptr;
    RTCScene m_top = nullptr;
    std::vector<std::unique_ptr<MeshData>> m_meshes;     // shared, per scene mesh
    std::vector<std::unique_ptr<MeshData>> m_deformed;   // per deformed instance
    std::vector<int32_t> m_instanceDeformed;             // instance -> index into m_deformed or -1
    std::vector<std::unique_ptr<Group>> m_groups;        // instance arrays
    std::vector<int32_t> m_geomToGroup;                  // top-level geomID -> group or -1
    std::vector<int32_t> m_geomToDeformed;               // top-level geomID -> deformed index or -1
    std::vector<Texture> m_textures;
    DeformationReport m_deform;
};

// Robust ray origin offset (Waechter & Binder, Ray Tracing Gems ch. 6).
float3 offsetRayOrigin(float3 p, float3 n);
} // namespace unx::reference
