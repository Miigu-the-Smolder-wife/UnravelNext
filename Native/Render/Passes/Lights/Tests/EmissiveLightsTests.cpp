// Emissive quadtree cook (unx/lights/EmissiveLights.h, RENDERER_REDESIGN_V2 14.1b): CPU checks, no GPU.
//   1. one 4 x 2 m panel (two triangles, constant emission): one plane, the root square holds it, flux in = flux out,
//      every leaf <= leaf size, the children of every inner node are consecutive and inside their parent;
//   2. a two-sided panel gives two planes (opposite normals); a textured-emissive material and a skinned instance stay
//      out (counted); the converted-material bitset marks the converted materials alone;
//   3. a tilted panel (rotated instance) keeps its flux and its plane basis is orthonormal with the triangle normal;
//   4. the GPU image's offsets and sizes are consistent.
#include "unx/lights/EmissiveLights.h"

#include "unx/core/Log.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace unx;
using namespace unx::lights;

namespace
{
int g_failures = 0;
void report(bool ok, const char* what, double value, double limit)
{
    logf("%-78s %-12.4g (limit %.4g) %s\n", what, value, limit, ok ? "ok" : "FAIL");
    if (!ok) ++g_failures;
}

scene::Mesh quad(const char* name, float w, float h)
{
    scene::Mesh m;
    m.name = name;
    m.positions = { { -w / 2, 0, -h / 2 }, { w / 2, 0, -h / 2 }, { w / 2, 0, h / 2 }, { -w / 2, 0, h / 2 } };
    m.normals = { { 0, -1, 0 }, { 0, -1, 0 }, { 0, -1, 0 }, { 0, -1, 0 } };
    m.uv0 = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    m.indices = { 0, 1, 2, 0, 2, 3 };  // counter-clockwise seen from -y (cross(p1 - p0, p2 - p0) = -y: a ceiling panel facing down)
    scene::Submesh s;
    s.indexOffset = 0;
    s.indexCount = 6;
    s.material = 0;
    m.submeshes.push_back(s);
    return m;
}

scene::Instance instanceAt(uint32_t mesh, float3 at, uint32_t flags = scene::InstanceCastShadow)
{
    scene::Instance i;
    i.mesh = mesh;
    i.transform.m[0][3] = at.x;
    i.transform.m[1][3] = at.y;
    i.transform.m[2][3] = at.z;
    i.flags = flags;
    return i;
}

// Checks every node: leaves within the leaf size, children consecutive and inside the parent, flux accounted.
void checkTree(const EmissiveCook& cook, const EmissiveCookConfig& cfg, const char* label)
{
    uint32_t badLeaves = 0, badChildren = 0, leaves = 0;
    for (const EmissiveNode& n : cook.nodes)
    {
        if (!(n.luminance > 0)) continue;  // a hole left by the slot move
        if (n.firstChild == 0xFFFFFFFFu)
        {
            ++leaves;
            if (2 * n.half > cfg.leafSize * (1 + 1e-5)) ++badLeaves;
            continue;
        }
        for (uint32_t q = 0; q < 4; ++q)
        {
            const EmissiveNode& c = cook.nodes[n.firstChild + q];
            if (!(c.luminance > 0)) continue;
            const float dx = std::abs(c.centre.x - n.centre.x), dy = std::abs(c.centre.y - n.centre.y);
            if (std::abs(c.half - 0.5f * n.half) > 1e-6f || std::abs(dx - 0.5f * n.half) > 1e-5f || std::abs(dy - 0.5f * n.half) > 1e-5f) ++badChildren;
        }
    }
    logf("%s: %zu planes, %zu nodes, %u leaves; flux in %.6g out %.6g\n", label, cook.planes.size(), cook.nodes.size(), leaves, cook.fluxIn, cook.fluxOut);
    report(badLeaves == 0, (std::string(label) + ": leaves within the leaf size").c_str(), badLeaves, 0);
    report(badChildren == 0, (std::string(label) + ": children are their parent's quadrants").c_str(), badChildren, 0);
    report(std::abs(cook.fluxOut - cook.fluxIn) <= 1e-6 * std::max(cook.fluxIn, 1e-12), (std::string(label) + ": leaf flux equals the triangles' flux (rel.)").c_str(),
           std::abs(cook.fluxOut - cook.fluxIn) / std::max(cook.fluxIn, 1e-12), 1e-6);
}
} // namespace

int main()
{
    EmissiveCookConfig cfg;
    cfg.leafSize = 0.25f;
    // 1. one panel
    {
        scene::Scene sc;
        sc.name = "emissive panel";
        scene::Material m;
        m.name = "panel";
        m.emissive = { 4000, 3800, 3500 };
        sc.materials.push_back(m);
        sc.meshes.push_back(quad("panel", 4, 2));
        sc.instances.push_back(instanceAt(0, { 1, 3, 2 }));
        const EmissiveCook cook = cookEmissiveLights(sc, cfg);
        report(cook.planes.size() == 1, "one panel: one plane", (double)cook.planes.size(), 1);
        report(cook.trianglesConverted == 2, "one panel: two triangles converted", cook.trianglesConverted, 2);
        if (!cook.planes.empty())
        {
            const EmissivePlane& p = cook.planes[0];
            report(std::abs(p.normal.y + 1) < 1e-6f, "one panel: plane normal faces down (-y)", p.normal.y, -1);
            report(std::abs(p.origin.y - 3) < 1e-5f, "one panel: plane through the panel (origin y)", p.origin.y, 3);
            const EmissiveNode& root = cook.nodes[p.rootNode];
            report(root.half >= 2 && root.half <= 2.01f, "one panel: root square half size = 2 m (+margin)", root.half, 2);
            // the root's mean radiance: the panel's 8 m^2 over the 16 m^2 square = half
            report(std::abs(root.radiance.x - 2000) < 1, "one panel: root mean radiance = emission x area fraction", root.radiance.x, 2000);
            const double fluxExpected = 8.0 * (0.2126 * 4000 + 0.7152 * 3800 + 0.0722 * 3500);
            report(std::abs(cook.fluxIn - fluxExpected) < 1e-3 * fluxExpected, "one panel: flux in = luminance x 8 m^2", cook.fluxIn, fluxExpected);
        }
        checkTree(cook, cfg, "one panel");
        const std::vector<uint32_t> image = emissiveLightsImage(cook);
        report(image.size() * 4 == 32 + cook.planes.size() * 64 + cook.nodes.size() * 48 + cook.convertedMaterials.size() * 4, "image size = header + planes + nodes + bitset",
               (double)image.size() * 4, (double)(32 + cook.planes.size() * 64 + cook.nodes.size() * 48 + cook.convertedMaterials.size() * 4));
        report(image[0] == cook.planes.size() && image[1] == cook.nodes.size() && image[2] == 32 && image[3] == 32 + 64 * cook.planes.size(), "image header words", image[3],
               (double)(32 + 64 * cook.planes.size()));
        report((image[image[4] / 4] & 1u) == 1u, "image bitset: material 0 converted", image[image[4] / 4] & 1u, 1);
    }
    // 2. two-sided, textured, skinned, bitset
    {
        scene::Scene sc;
        sc.name = "mixed emitters";
        scene::Material twoSided, textured, dark;
        twoSided.name = "two-sided";
        twoSided.emissive = { 1000, 1000, 1000 };
        twoSided.twoSided = true;
        textured.name = "textured";
        textured.emissive = { 500, 500, 500 };
        textured.emissiveTexture = 7;  // (any texture index: the material stays on the cache path)
        dark.name = "dark";
        sc.materials = { dark, twoSided, textured };
        sc.meshes.push_back(quad("panel", 2, 2));
        scene::Instance a = instanceAt(0, { 0, 2, 0 });
        a.materialOverrides = { 1 };
        scene::Instance b = instanceAt(0, { 5, 2, 0 });
        b.materialOverrides = { 2 };
        scene::Instance c = instanceAt(0, { 10, 2, 0 }, scene::InstanceCastShadow | scene::InstanceSkinned);
        c.materialOverrides = { 1 };
        sc.instances = { a, b, c };
        const EmissiveCook cook = cookEmissiveLights(sc, cfg);
        report(cook.planes.size() == 2, "two-sided panel: two planes (both sides)", (double)cook.planes.size(), 2);
        if (cook.planes.size() == 2) report(dot(cook.planes[0].normal, cook.planes[1].normal) < -0.999f, "two-sided panel: opposite plane normals", dot(cook.planes[0].normal, cook.planes[1].normal), -1);
        report(cook.trianglesTextured == 2, "textured emissive stays on the cache path (triangles)", cook.trianglesTextured, 2);
        report(cook.trianglesSkinned == 2, "skinned emitter stays on the cache path (triangles)", cook.trianglesSkinned, 2);
        report(cook.convertedMaterials.size() == 1 && cook.convertedMaterials[0] == 2u, "bitset marks the two-sided material alone", cook.convertedMaterials.empty() ? -1 : cook.convertedMaterials[0], 2);
        checkTree(cook, cfg, "mixed emitters");
    }
    // 3. tilted panel
    {
        scene::Scene sc;
        sc.name = "tilted";
        scene::Material m;
        m.emissive = { 100, 100, 100 };
        sc.materials.push_back(m);
        sc.meshes.push_back(quad("panel", 3, 1));
        scene::Instance i = instanceAt(0, { 0, 2, 0 });
        const float ca = std::cos(0.7f), sa = std::sin(0.7f);  // rotation about z
        i.transform.m[0][0] = ca; i.transform.m[0][1] = -sa; i.transform.m[1][0] = sa; i.transform.m[1][1] = ca;
        sc.instances.push_back(i);
        const EmissiveCook cook = cookEmissiveLights(sc, cfg);
        report(cook.planes.size() == 1, "tilted panel: one plane", (double)cook.planes.size(), 1);
        if (!cook.planes.empty())
        {
            const EmissivePlane& p = cook.planes[0];
            const float3 n = p.normal;
            const float ortho = std::abs(dot(p.right, p.up)) + std::abs(dot(p.right, n)) + std::abs(dot(p.up, n)) + std::abs(length(p.right) - 1) + std::abs(length(p.up) - 1);
            report(ortho < 1e-5f, "tilted panel: orthonormal plane basis", ortho, 1e-5);
            // the rotated panel's normal: R (0,-1,0) = (sin a, -cos a, 0)
            report(std::abs(n.x - sa) < 1e-5f && std::abs(n.y + ca) < 1e-5f, "tilted panel: plane normal is the rotated face normal", n.x, sa);
        }
        checkTree(cook, cfg, "tilted panel");
    }
    if (g_failures == 0) logf("PASS: 0 failure(s)\n");
    else logf("FAIL: %d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
