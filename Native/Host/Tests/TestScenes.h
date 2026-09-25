#pragma once
// Small scenes shared by the host tests (I track).
#include "unx/core/Math.h"
#include "unx/scene/SceneData.h"

#include <utility>

namespace unx::host::test
{
// One grey box (counter-clockwise outward faces) on nothing, camera 0 in front: the least content a HostRenderer commits.
inline scene::Scene oneBox()
{
    scene::Scene s;
    s.name = "one box";
    scene::Material m;
    m.name = "grey";
    s.materials.push_back(m);
    scene::Mesh mesh;
    mesh.name = "box";
    const float3 axes[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    for (const float3& a : axes)
    {
        const float3 u = a.x != 0 ? float3{ 0, 0, 1 } : float3{ 1, 0, 0 };
        const float3 w = cross(a, u);  // u x w = a
        const uint32_t base = (uint32_t)mesh.positions.size();
        const float su[4] = { -1, 1, 1, -1 }, sw[4] = { -1, -1, 1, 1 };
        for (int c = 0; c < 4; ++c)
        {
            mesh.positions.push_back((a + u * su[c] + w * sw[c]) * 0.5f);
            mesh.normals.push_back(a);
        }
        mesh.indices.insert(mesh.indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
    }
    mesh.submeshes.push_back({ 0, (uint32_t)mesh.indices.size(), 0 });
    s.meshes.push_back(std::move(mesh));
    scene::Instance inst;
    inst.mesh = 0;
    s.instances.push_back(inst);
    scene::Camera c;
    c.name = "front";
    c.position = { 0, 0.5f, 3 };
    s.cameras.push_back(c);
    return s;
}
} // namespace unx::host::test
