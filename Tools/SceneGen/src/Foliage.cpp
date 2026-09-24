// Trees and grass for the forest scenes (ARCHITECTURE 1.2 census scene, 2.1 bands, 2.7).
//   thin: 40k geometric leaves per tree (6 cm long, 3 cm wide diamonds, opaque, two-sided) and grass clumps of 100
//         blades 4 mm wide at the base (opaque). This is thin *geometry*: the coverage layer's exact-area case.
//   card: 1.5k alpha-tested leaf cards per tree (35 cm, INTERFACES 10.1) and clumps of 8 alpha-tested grass cards
//         30 cm wide.
// Crown shape, trunk and grass clump layout follow the microbench census scene (Tools/Microbench buildRtScene).
#include "Common.h"

#include <cmath>

namespace unx::scenegen::detail
{
namespace
{
float leafClusterAlpha(float u, float v, uint32_t seed)
{
    // Five elliptic leaves with serrated edges around the card centre, plus a 6 mm twig along the diagonal.
    float a = 0;
    for (uint32_t k = 0; k < 5; ++k)
    {
        Rng r(seed, 900 + k);
        const float cx = 0.5f + r.range(-0.22f, 0.22f), cy = 0.5f + r.range(-0.22f, 0.22f), rot = r.range(0, 2 * kPi);
        const float du = u - cx, dv = v - cy, ca = std::cos(rot), sa = std::sin(rot);
        const float x = (du * ca + dv * sa) / 0.21f, y = (-du * sa + dv * ca) / 0.12f;
        const float rr = std::sqrt(x * x + y * y), th = std::atan2(y, x);
        if (rr < 1.0f + 0.06f * std::sin(14.0f * th)) a = 1;
    }
    const float d = std::fabs((u - v)) / 1.41421356f;
    if (d < 0.009f && u > 0.2f && u < 0.8f) a = 1;
    return a;
}

float grassCardAlpha(float u, float v, uint32_t seed)
{
    // 12 tapering blades rising from the bottom edge (v = 1) to heights 60-100% of the card.
    for (uint32_t k = 0; k < 12; ++k)
    {
        Rng r(seed, 1000 + k);
        const float base = r.range(0.06f, 0.94f), top = r.range(0.0f, 0.4f), lean = r.range(-0.12f, 0.12f), w = r.range(0.012f, 0.022f);
        const float h = 1.0f - v;  // 0 at the bottom
        const float height = 1.0f - top;
        if (h > height) continue;
        const float t = h / height;
        const float centre = base + lean * t * t;
        if (std::fabs(u - centre) < w * (1 - t)) return 1;
    }
    return 0;
}
} // namespace

FoliageAssets buildFoliage(Scene& s, const Palette& p, uint64_t seed, FoliageStyle style)
{
    FoliageAssets out;
    const uint32_t sd = (uint32_t)(seed * 0x9E3779B1u);
    uint32_t leafMaterials[4], grassMaterial;
    if (style.thin)
    {
        for (int v = 0; v < 4; ++v)
        {
            Material m;
            m.name = "leaf_thin_" + std::to_string(v);
            m.cls = scene::MaterialClass::Foliage;
            m.baseColor = f3(0.055f + 0.01f * v, 0.11f + 0.015f * v, 0.028f);
            m.roughness = 0.5f;
            m.transmission = 0.3f;
            m.twoSided = true;
            leafMaterials[v] = addMaterial(s, m);
        }
        Material g;
        g.name = "grass_blade";
        g.cls = scene::MaterialClass::Foliage;
        g.baseColor = f3(0.07f, 0.13f, 0.035f);
        g.roughness = 0.55f;
        g.transmission = 0.35f;
        g.twoSided = true;
        grassMaterial = addMaterial(s, g);
    }
    else
    {
        for (int v = 0; v < 4; ++v)
        {
            Texture t = makeTexture("leaf_card_" + std::to_string(v), 512, 512, scene::TextureFormat::Rgba8Srgb);
            for (uint32_t y = 0; y < 512; ++y)
                for (uint32_t x = 0; x < 512; ++x)
                {
                    const float u = (x + 0.5f) / 512, vv = (y + 0.5f) / 512;
                    const float n = fbm2(u * 16, vv * 16, sd + 300 + v, 3, 16);
                    uint8_t* px = &t.texels[4 * ((size_t)y * 512 + x)];
                    px[0] = toSrgb8((0.05f + 0.01f * v) * (0.8f + 0.4f * n));
                    px[1] = toSrgb8((0.11f + 0.015f * v) * (0.8f + 0.4f * n));
                    px[2] = toSrgb8(0.028f * (0.8f + 0.4f * n));
                    px[3] = leafClusterAlpha(u, vv, sd + 17u * v) > 0.5f ? 255 : 0;
                }
            Material m;
            m.name = "leaf_card_" + std::to_string(v);
            m.cls = scene::MaterialClass::Foliage;
            m.baseColorTexture = addTexture(s, std::move(t));
            m.baseColor = f3(1, 1, 1);
            m.roughness = 0.5f;
            m.transmission = 0.3f;
            m.twoSided = true;
            m.alphaCutoff = 0.5f;
            leafMaterials[v] = addMaterial(s, m);
        }
        Texture t = makeTexture("grass_card", 512, 512, scene::TextureFormat::Rgba8Srgb);
        for (uint32_t y = 0; y < 512; ++y)
            for (uint32_t x = 0; x < 512; ++x)
            {
                const float u = (x + 0.5f) / 512, v = (y + 0.5f) / 512;
                const float shade = 0.7f + 0.3f * (1 - v);
                uint8_t* px = &t.texels[4 * ((size_t)y * 512 + x)];
                px[0] = toSrgb8(0.07f * shade);
                px[1] = toSrgb8(0.13f * shade);
                px[2] = toSrgb8(0.035f * shade);
                px[3] = grassCardAlpha(u, v, sd + 77) > 0.5f ? 255 : 0;
            }
        Material g;
        g.name = "grass_card";
        g.cls = scene::MaterialClass::Foliage;
        g.baseColorTexture = addTexture(s, std::move(t));
        g.baseColor = f3(1, 1, 1);
        g.roughness = 0.55f;
        g.transmission = 0.35f;
        g.twoSided = true;
        g.alphaCutoff = 0.5f;
        grassMaterial = addMaterial(s, g);
    }

    // Trees: 4 variants. Trunk 8 m (radius 0.35 -> 0.14), six branches, crown ellipsoid centred at 7 m with radii
    // 3.5 / 3 / 3.5 m (microbench crown).
    const int leaves = style.thin ? 40000 : 1500;
    out.leavesPerTree = (uint32_t)leaves;
    for (int v = 0; v < 4; ++v)
    {
        Rng r(seed, 100 + v);
        MeshBuilder b(std::string(style.thin ? "tree_thin_" : "tree_card_") + std::to_string(v));
        b.material(p.bark);
        b.cylinder({ 0, -0.3f, 0 }, { 0, 8.3f, 0 }, 0.35f, 0.14f, 16, 8, false, 1.0f);
        for (int k = 0; k < 6; ++k)
        {
            const float h = 3.5f + 0.6f * k + r.range(-0.2f, 0.2f), yaw = 2 * kPi * k / 6 + r.range(-0.4f, 0.4f);
            const float3 dir = normalize(f3(std::cos(yaw), 0.55f + r.range(0, 0.3f), std::sin(yaw)));
            b.cylinder({ 0, h, 0 }, dir * r.range(1.8f, 2.6f), 0.06f, 0.02f, 8, 2, false, 1.0f);
        }
        b.material(leafMaterials[v]);
        for (int q = 0; q < leaves; ++q)
        {
            float cx, cy, cz;
            do
            {
                cx = r.uniform() * 2 - 1;
                cy = r.uniform() * 2 - 1;
                cz = r.uniform() * 2 - 1;
            } while (cx * cx + cy * cy + cz * cz > 1);
            const float3 c = f3(cx * 3.5f, 7.0f + cy * 3.0f, cz * 3.5f);
            const float yaw = r.uniform() * 2 * kPi, pitch = r.uniform() * kPi;
            const float3 axis = f3(std::cos(yaw) * std::cos(pitch), std::sin(pitch), std::sin(yaw) * std::cos(pitch));
            const float3 side = f3(-std::sin(yaw), 0, std::cos(yaw));
            if (style.thin)
            {
                // Diamond leaf: 6 cm along 'axis', 3 cm across, folded 10 degrees along the midrib.
                const float3 nrm = normalize(cross(axis, side));
                const float fold = 0.03f * 0.5f * std::sin(10.0f * kPi / 180.0f);
                const float3 tip = c + axis * 0.03f, base = c - axis * 0.03f;
                const float3 left = c - side * 0.015f + nrm * fold, right = c + side * 0.015f + nrm * fold;
                const float3 nl = normalize(cross(tip - base, left - base)), nr = normalize(cross(right - base, tip - base));
                const uint32_t i0 = b.vertex(base, nl, { 0.5f, 1 }), i1 = b.vertex(left, nl, { 0, 0.5f }), i2 = b.vertex(tip, nl, { 0.5f, 0 });
                const uint32_t j0 = b.vertex(base, nr, { 0.5f, 1 }), j1 = b.vertex(tip, nr, { 0.5f, 0 }), j2 = b.vertex(right, nr, { 1, 0.5f });
                b.triangle(i0, i2, i1);
                b.triangle(j0, j2, j1);
            }
            else
            {
                const float e = 0.175f;
                const float3 p0 = c - (axis + side) * e, p1 = c + (axis - side) * e, p2 = c + (axis + side) * e, p3 = c - (axis - side) * e;
                b.quad4(p0, p1, p2, p3, { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 });
            }
        }
        out.treeMeshes.push_back(addMesh(s, b.finish(true)));
    }

    // Grass clumps: 8 variants over 1 m^2.
    const int blades = style.thin ? 100 : 8;
    out.bladesPerClump = (uint32_t)blades;
    for (int v = 0; v < 8; ++v)
    {
        Rng r(seed, 200 + v);
        MeshBuilder b(std::string(style.thin ? "grass_thin_" : "grass_card_") + std::to_string(v));
        b.material(grassMaterial);
        const float halfWidth = style.thin ? 0.002f : 0.15f;
        for (int q = 0; q < blades; ++q)
        {
            const float cx = r.uniform() - 0.5f, cz = r.uniform() - 0.5f, yaw = r.uniform() * 2 * kPi;
            const float h = 0.5f * (0.6f + 0.8f * r.uniform());
            const float bx = std::cos(yaw) * halfWidth, bz = std::sin(yaw) * halfWidth;
            const float lean = style.thin ? 0.1f * (r.uniform() - 0.5f) : 0.0f;
            const float topScale = style.thin ? 0.3f : 1.0f;
            const float3 p0 = f3(cx - bx, 0, cz - bz), p1 = f3(cx + bx, 0, cz + bz);
            const float3 p2 = f3(cx + bx * topScale + lean, h, cz + bz * topScale), p3 = f3(cx - bx * topScale + lean, h, cz - bz * topScale);
            b.quad4(p0, p1, p2, p3, { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 });
        }
        out.grassMeshes.push_back(addMesh(s, b.finish(false)));
    }
    return out;
}
} // namespace unx::scenegen::detail
