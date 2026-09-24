// Shared procedural materials: albedo in linear units stored as sRGB8, roughness/metal maps, normal maps from
// analytic height functions. Albedo values follow measured ranges (asphalt 0.05-0.12, concrete 0.3-0.45, brick
// 0.2-0.35, grass 0.1-0.2, bark 0.1-0.25).
#include "Common.h"

#include <cmath>

namespace unx::scenegen::detail
{
namespace
{
using scene::TextureFormat;

Texture albedoTexture(std::string name, uint32_t size, const std::function<float3(float, float)>& colour, bool withAlpha = false,
                      const std::function<float(float, float)>& alpha = {})
{
    Texture t = makeTexture(std::move(name), size, size, TextureFormat::Rgba8Srgb);
    for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x)
        {
            const float u = (x + 0.5f) / size, v = (y + 0.5f) / size;
            const float3 c = colour(u, v);
            uint8_t* p = &t.texels[4 * ((size_t)y * size + x)];
            p[0] = toSrgb8(c.x);
            p[1] = toSrgb8(c.y);
            p[2] = toSrgb8(c.z);
            p[3] = withAlpha ? toUnorm8(alpha(u, v)) : 255;
        }
    return t;
}

Texture roughMetalTexture(std::string name, uint32_t size, const std::function<float(float, float)>& roughness, float metallic)
{
    Texture t = makeTexture(std::move(name), size, size, TextureFormat::Rg8RoughMetal);
    for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x)
        {
            const float u = (x + 0.5f) / size, v = (y + 0.5f) / size;
            t.texels[2 * ((size_t)y * size + x)] = toUnorm8(roughness(u, v));
            t.texels[2 * ((size_t)y * size + x) + 1] = toUnorm8(metallic);
        }
    return t;
}

Material standard(std::string name, float3 base, float roughness, float metallic = 0)
{
    Material m;
    m.name = std::move(name);
    m.baseColor = base;
    m.roughness = roughness;
    m.metallic = metallic;
    return m;
}

// Brick pattern (running bond, 225 x 75 mm pitch, 10 mm mortar) over a 0.9 m square tile: 4 bricks x 12 courses.
float brickMortar(float u, float v)
{
    const float course = v * 12.0f;
    const int row = (int)std::floor(course);
    const float fy = course - row;
    const float shift = (row & 1) ? 0.5f : 0.0f;
    const float along = u * 4.0f + shift;
    const float fx = along - std::floor(along);
    const float mx = 0.010f / 0.225f, my = 0.010f / 0.075f;
    const float dx = std::min(fx, 1 - fx) / mx, dy = std::min(fy, 1 - fy) / my;
    return clampf(1.0f - std::min(dx, dy), 0, 1);  // 1 in mortar joints
}
int brickId(float u, float v)
{
    const int row = (int)std::floor(v * 12.0f);
    const float along = u * 4.0f + ((row & 1) ? 0.5f : 0.0f);
    return row * 16 + ((int)std::floor(along) & 3);
}
} // namespace

Palette buildPalette(Scene& s, uint64_t seed, bool wetRoads)
{
    const uint32_t sd = (uint32_t)(seed * 2654435761u);
    Palette p;
    const uint32_t N = 512;

    // Asphalt: 4 m tile. Aggregate speckle albedo 0.06-0.11, 1.5 mm bumps.
    auto asphaltHeight = [sd](float u, float v) { return 0.0015f * fbm2(u * 64, v * 64, sd + 11, 4, 64); };
    const uint32_t asphaltAlbedo = addTexture(s, albedoTexture("asphalt_albedo", N, [sd](float u, float v) {
        const float n = fbm2(u * 128, v * 128, sd + 10, 3, 128);
        const float g = 0.06f + 0.05f * n;
        return f3(g, g, g * 1.02f);
    }));
    const uint32_t asphaltNormal = addTexture(s, normalMapFromHeight("asphalt_normal", N, 4.0f, asphaltHeight));
    {
        Material m = standard("asphalt", { 1, 1, 1 }, 0.75f);
        m.baseColorTexture = asphaltAlbedo;
        m.normalTexture = asphaltNormal;
        p.asphalt = addMaterial(s, m);
    }
    {
        // Wet asphalt (CityNight): standing water in low spots (roughness 0.15), damp elsewhere (0.35); water darkens
        // the albedo (Lekner & Dorf: ~0.5-0.7 of dry for rough dark surfaces).
        const uint32_t wetRm = addTexture(s, roughMetalTexture("asphalt_wet_rm", N, [sd](float u, float v) {
            const float puddle = smoothstepf(0.55f, 0.62f, fbm2(u * 6, v * 6, sd + 12, 4, 6));
            return lerpf(0.35f, 0.15f, puddle);
        }, 0.0f));
        Material m = standard("asphalt_wet", { 0.6f, 0.6f, 0.6f }, 1.0f);
        m.baseColorTexture = asphaltAlbedo;
        m.normalTexture = asphaltNormal;
        m.roughMetalTexture = wetRm;
        p.asphaltWet = addMaterial(s, m);
    }
    if (!wetRoads) p.asphaltWet = p.asphalt;

    // Concrete: 2 m tile.
    {
        const uint32_t a = addTexture(s, albedoTexture("concrete_albedo", N, [sd](float u, float v) {
            const float n = fbm2(u * 32, v * 32, sd + 20, 5, 32);
            const float g = 0.30f + 0.14f * n;
            return f3(g, g * 0.98f, g * 0.95f);
        }));
        const uint32_t nm = addTexture(s, normalMapFromHeight("concrete_normal", N, 2.0f, [sd](float u, float v) { return 0.0008f * fbm2(u * 64, v * 64, sd + 21, 3, 64); }));
        Material m = standard("concrete", { 1, 1, 1 }, 0.85f);
        m.baseColorTexture = a;
        m.normalTexture = nm;
        p.concrete = addMaterial(s, m);
    }
    // Brick: 0.9 m tile, mortar recessed 6 mm.
    {
        const uint32_t a = addTexture(s, albedoTexture("brick_albedo", 1024, [sd](float u, float v) {
            const float mortar = brickMortar(u, v);
            const uint32_t id = (uint32_t)brickId(u, v);
            const float var = ((id * 2654435761u + sd) >> 8 & 255) / 255.0f;
            const float grain = fbm2(u * 256, v * 256, sd + 30, 2, 256);
            const float3 brick = f3(0.30f + 0.10f * var, 0.10f + 0.04f * var, 0.07f + 0.02f * var) * (0.85f + 0.3f * grain);
            return brick * (1 - mortar) + f3(0.45f, 0.43f, 0.40f) * mortar;
        }));
        const uint32_t nm = addTexture(s, normalMapFromHeight("brick_normal", 1024, 0.9f, [](float u, float v) { return -0.006f * brickMortar(u, v); }));
        Material m = standard("brick", { 1, 1, 1 }, 0.85f);
        m.baseColorTexture = a;
        m.normalTexture = nm;
        p.brick = addMaterial(s, m);
    }
    // Plaster: 2 m tile.
    {
        const uint32_t a = addTexture(s, albedoTexture("plaster_albedo", N, [sd](float u, float v) {
            const float g = 0.55f + 0.08f * fbm2(u * 16, v * 16, sd + 40, 4, 16);
            return f3(g, g * 0.96f, g * 0.9f);
        }));
        Material m = standard("plaster", { 1, 1, 1 }, 0.9f);
        m.baseColorTexture = a;
        p.plaster = addMaterial(s, m);
    }
    // Window glass until the Glass class exists (INTERFACES 8.1: P4): dark smooth dielectric, f0 0.04.
    p.windowGlass = addMaterial(s, standard("window_glass", { 0.02f, 0.025f, 0.03f }, 0.05f));
    {
        Material m = standard("window_lit", { 0.02f, 0.02f, 0.02f }, 0.05f);
        m.emissive = f3(40.0f, 30.0f, 18.0f);  // nits, warm interior seen through glass
        p.windowLit = addMaterial(s, m);
    }
    p.metal = addMaterial(s, standard("galvanised_steel", { 0.56f, 0.57f, 0.58f }, 0.45f, 1.0f));
    p.roofing = addMaterial(s, standard("roofing", { 0.09f, 0.09f, 0.1f }, 0.9f));
    // Ground cover: 8 m tile grass/soil mix.
    {
        const uint32_t a = addTexture(s, albedoTexture("grass_albedo", N, [sd](float u, float v) {
            const float n = fbm2(u * 32, v * 32, sd + 50, 5, 32);
            const float mix = smoothstepf(0.35f, 0.65f, fbm2(u * 4, v * 4, sd + 51, 3, 4));
            const float3 green = f3(0.07f, 0.12f, 0.03f) * (0.8f + 0.4f * n), brown = f3(0.14f, 0.10f, 0.06f) * (0.8f + 0.4f * n);
            return green * mix + brown * (1 - mix);
        }));
        Material m = standard("ground_grass", { 1, 1, 1 }, 0.95f);
        m.baseColorTexture = a;
        p.grass = addMaterial(s, m);
    }
    p.soil = addMaterial(s, standard("soil", { 0.13f, 0.09f, 0.06f }, 0.95f));
    // Bark: 1 m tile, vertical fissures 4 mm deep.
    {
        auto barkHeight = [sd](float u, float v) {
            const float w = fbm2(u * 8, v * 2, sd + 60, 3, 8);
            return 0.004f * std::fabs(std::sin((u * 24 + 2.0f * w) * kPi));
        };
        const uint32_t a = addTexture(s, albedoTexture("bark_albedo", N, [sd](float u, float v) {
            const float n = fbm2(u * 24, v * 8, sd + 61, 4, 24);
            return f3(0.13f, 0.10f, 0.08f) * (0.7f + 0.6f * n);
        }));
        const uint32_t nm = addTexture(s, normalMapFromHeight("bark_normal", N, 1.0f, barkHeight));
        Material m = standard("bark", { 1, 1, 1 }, 0.9f);
        m.baseColorTexture = a;
        m.normalTexture = nm;
        p.bark = addMaterial(s, m);
    }
    // Rock: 2 m tile.
    {
        const uint32_t a = addTexture(s, albedoTexture("rock_albedo", N, [sd](float u, float v) {
            const float n = fbm2(u * 16, v * 16, sd + 70, 5, 16);
            const float g = 0.18f + 0.16f * n;
            return f3(g, g * 0.97f, g * 0.93f);
        }));
        const uint32_t nm = addTexture(s, normalMapFromHeight("rock_normal", N, 2.0f, [sd](float u, float v) { return 0.01f * fbm2(u * 16, v * 16, sd + 71, 5, 16); }));
        Material m = standard("rock", { 1, 1, 1 }, 0.7f);
        m.baseColorTexture = a;
        m.normalTexture = nm;
        p.rock = addMaterial(s, m);
        Material w = m;
        w.name = "rock_wet";
        w.baseColor = f3(0.6f, 0.6f, 0.6f);
        w.roughness = 0.2f;
        p.rockWet = addMaterial(s, w);
    }
    p.sand = addMaterial(s, standard("sand", { 0.42f, 0.37f, 0.28f }, 0.9f));
    return p;
}
} // namespace unx::scenegen::detail
