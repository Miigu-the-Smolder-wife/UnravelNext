// The showcase scenes (SceneGen.h: showcase_bathhouse, showcase_atrium, showcase_shore): places built to be looked at,
// each holding the renderer features that had no scene - so that the A/B batch has something to measure them on. What a
// scene and each of its cameras exercises is listed in Docs/Status/UE6_WORKPLAN_KO.md (the batch's groups name them).
//   showcase_bathhouse  a tiled bath room in the evening sun: window panes in their frames (one mesh: an opaque frame and
//                       a Glass pane), a stained slit of Glass alone, a shutter that is only a shadow, bath water under
//                       rising steam, pendant lamps in glass shades, paper lanterns that light the room by their
//                       emission alone, a rect light with an image and barn doors, a cookie light through the steam, a
//                       figure (skin, eyes, strand hair, a cloth robe) lit by a light of its own channel, glazed and
//                       wet tiles with a height map, a bench with detail maps, a towel tinted by vertex colours.
//   showcase_atrium     a courtyard under a roof of tinted panes in a steel grid (one mesh, four glass materials): the
//                       high sun falls through it in coloured patches; trees in planters, lacquered benches, cloth
//                       banners and a carpet, a solid glass sphere and prism, a gallery behind a glass railing with
//                       small emissive lamps.
//   showcase_shore      a lake shore under a low sun: reeds and a wet jetty with a lantern, mist over the water, a
//                       forest that starts on the far shore, behind it a bare ridge whose crest stands in the cloud
//                       layer, cirrus above; a second camera for rain.
// The scene file holds no light functions, no decals and no rain: those are the scene's extras (SceneGen.h extras), which
// whoever renders the scene applies, as it makes hair bodies of the grooms.
#include "Common.h"

#include <algorithm>
#include <cmath>

namespace unx::scenegen
{
using namespace detail;

namespace
{
uint32_t colourTexture(Scene& s, const char* name, uint32_t size, const std::function<float4(float, float)>& colour, bool wrap = true)
{
    Texture t = makeTexture(name, size, size, scene::TextureFormat::Rgba8Srgb, wrap);
    for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x)
        {
            const float4 c = colour((x + 0.5f) / size, (y + 0.5f) / size);
            uint8_t* p = &t.texels[4 * ((size_t)y * size + x)];
            p[0] = toSrgb8(c.x), p[1] = toSrgb8(c.y), p[2] = toSrgb8(c.z), p[3] = toUnorm8(c.w);
        }
    return addTexture(s, std::move(t));
}
uint32_t greyTexture(Scene& s, const char* name, uint32_t size, const std::function<float(float, float)>& value)
{
    Texture t = makeTexture(name, size, size, scene::TextureFormat::R8Linear);
    for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x) t.texels[(size_t)y * size + x] = toUnorm8(value((x + 0.5f) / size, (y + 0.5f) / size));
    return addTexture(s, std::move(t));
}
// A box of one material as its own mesh and instance.
void solid(Scene& s, std::string name, uint32_t material, float3 lo, float3 hi, float uvScale = 1.0f, uint32_t flags = scene::InstanceCastShadow, bool tangents = false)
{
    MeshBuilder b(std::move(name));
    b.material(material);
    b.box(lo, hi, uvScale);
    const Material& m = s.materials[material];
    addInstance(s, addMesh(s, b.finish(tangents || m.normalTexture != scene::kNone || m.detailNormalTexture != scene::kNone)), float3x4{}, flags);
}
// 1 on a tile, 0 in the grout: n x n tiles to the uv unit.
float tilePattern(float u, float v, float n, float grout)
{
    const float fu = u * n - std::floor(u * n), fv = v * n - std::floor(v * n);
    const float d = std::max(std::fabs(fu - 0.5f), std::fabs(fv - 0.5f));
    return smoothstepf(0.5f, 0.5f - grout, d);
}
// A decal's box (unx/decal/Decals.h Decal::box): the unit cube's half-extent axes x, y, z (z: out of the surface the
// decal is laid on) and its centre.
float3x4 decalBox(float3 centre, float3 x, float3 y, float3 z)
{
    float3x4 m;
    const float3 columns[4] = { x, y, z, centre };
    for (int c = 0; c < 4; ++c) m.m[0][c] = columns[c].x, m.m[1][c] = columns[c].y, m.m[2][c] = columns[c].z;
    return m;
}
ExtraDecal makeDecal(float3x4 box, uint32_t material, uint32_t channels)
{
    ExtraDecal d;
    d.box = box;
    d.material = material;
    d.channels = channels;
    return d;
}
// A square image for a light (a cookie, a gobo): linear rgb, rows from the top.
void lightImage(ExtraLightFunction& f, uint32_t size, const std::function<float3(float, float)>& colour)
{
    f.imageWidth = f.imageHeight = size;
    f.imageRgb.resize((size_t)size * size * 3);
    for (uint32_t y = 0; y < size; ++y)
        for (uint32_t x = 0; x < size; ++x)
        {
            const float3 c = colour((x + 0.5f) / size, (y + 0.5f) / size);
            float* p = &f.imageRgb[3 * ((size_t)y * size + x)];
            p[0] = c.x, p[1] = c.y, p[2] = c.z;
        }
}
Material glassPane(const char* name, float3 tint)
{
    Material m;
    m.name = name;
    m.cls = scene::MaterialClass::Glass;
    m.baseColor = tint;
    m.roughness = 0.02f;
    m.ior = 1.5f;
    m.twoSided = true;  // a pane: one surface stands for both faces
    return m;
}
Material glassSolid(const char* name, float3 transmittance, float distance)
{
    Material m = glassPane(name, transmittance);
    m.twoSided = false;  // a body: front and back faces, the base colour its transmittance over 'distance'
    m.attenuationDistance = distance;
    return m;
}

// ---------------------------------------------------------------------------------------------------------------------
// showcase_bathhouse. The room: x in [-5, 5], z in [-4, 4], 4 m high; the +Z wall holds two windows and a slit between
// them, and the evening sun (24 degrees up) comes in through them towards -Z, across the floor and the sunken bath.
Scene bathhouse(const Request& rq, std::vector<Groom>* grooms, SceneExtras* extras)
{
    Scene s;
    s.name = "showcase_bathhouse";
    s.sun.direction = sunDirection(24.0f, 78.0f);
    s.windDirection = normalize(f3(0.8f, 0, 0.6f));
    s.windSpeed = 0.0f;
    const Palette p = buildPalette(s, rq.seed, false);
    const float W = 5.0f, D = 4.0f, H = 4.0f, T = 0.25f;
    const uint32_t cast = scene::InstanceCastShadow;

    // ---- materials
    // tiles: 4 x 4 to the uv unit (25 cm at the meshes' uv of metres), a height map for the grout (parallax), a glaze
    const uint32_t tileColour = colourTexture(s, "bath_tile_colour", 256, [](float u, float v) {
        const float t = tilePattern(u, v, 4.0f, 0.035f);
        const float shade = 0.92f + 0.08f * fbm2(u * 16.0f, v * 16.0f, 301u, 3, 16);
        const float3 c = f3(0.33f, 0.32f, 0.30f) * (1 - t) + f3(0.78f, 0.86f, 0.82f) * (t * shade);
        return float4{ c.x, c.y, c.z, 1.0f };
    });
    const uint32_t tileHeight = greyTexture(s, "bath_tile_height", 256, [](float u, float v) { return tilePattern(u, v, 4.0f, 0.035f); });
    Material tile;
    tile.name = "bath_tile";
    tile.baseColor = f3(1, 1, 1);
    tile.baseColorTexture = tileColour;
    tile.heightTexture = tileHeight;
    tile.heightScale = 0.004f;
    tile.roughness = 0.3f;
    tile.clearcoat = 1.0f;  // the glaze
    tile.clearcoatRoughness = 0.06f;
    const uint32_t tileMat = addMaterial(s, tile);
    Material floorTile = tile;  // the same tiles laid diagonally (the uv transform), under a film of water (the 1.33 coat)
    floorTile.name = "bath_floor_tile_wet";
    floorTile.baseColor = f3(0.86f, 0.80f, 0.72f);
    floorTile.uvScale = { 1.4142f, 1.4142f };
    floorTile.uvRotation = 45.0f * kPi / 180.0f;
    floorTile.clearcoatRoughness = 0.03f;
    floorTile.clearcoatIor = 1.33f;
    const uint32_t floorMat = addMaterial(s, floorTile);
    Material basinTile = tile;
    basinTile.name = "bath_basin_tile";
    basinTile.baseColor = f3(0.45f, 0.72f, 0.85f);
    const uint32_t basinMat = addMaterial(s, basinTile);
    // wood with detail maps (bench, window frames, shutter)
    Material wood;
    wood.name = "bath_wood";
    wood.baseColor = f3(0.27f, 0.15f, 0.08f);
    wood.roughness = 0.5f;
    wood.detailColorTexture = colourTexture(s, "bath_wood_grain", 128, [](float u, float v) {
        const float g = 0.2176f * (0.55f + 0.9f * fbm2(u * 2.0f, v * 24.0f, 311u, 4, 24));  // around the neutral value (sRGB 0.5)
        return float4{ g, g, g, 1.0f };
    });
    wood.detailNormalTexture =
        addTexture(s, normalMapFromHeight("bath_wood_grain_normal", 128, 0.3f, [](float u, float v) { return 0.0008f * fbm2(u * 2.0f, v * 24.0f, 312u, 3, 24); }));
    wood.detailScale = { 3, 3 };
    const uint32_t woodMat = addMaterial(s, wood);
    const uint32_t paneClear = addMaterial(s, glassPane("bath_pane_clear", f3(0.86f, 0.95f, 0.90f)));
    const uint32_t paneAmber = addMaterial(s, glassPane("bath_pane_amber", f3(0.95f, 0.62f, 0.18f)));
    const uint32_t stainedBlue = addMaterial(s, glassPane("bath_stained_blue", f3(0.15f, 0.35f, 0.90f)));
    const uint32_t stainedRed = addMaterial(s, glassPane("bath_stained_red", f3(0.90f, 0.12f, 0.10f)));
    const uint32_t shadeMat = addMaterial(s, glassPane("bath_lamp_shade", f3(0.98f, 0.70f, 0.35f)));
    Material bulb;  // the pendant lamps' bulbs: seen, but the lamp's light is its analytic light (not twice)
    bulb.name = "bath_bulb";
    bulb.baseColor = f3(0.9f, 0.9f, 0.9f);
    bulb.emissive = f3(40000.0f, 30000.0f, 16000.0f);
    bulb.emissiveVisibleOnly = true;
    const uint32_t bulbMat = addMaterial(s, bulb);
    Material lantern;  // paper lanterns with no analytic light: their emission lights the room through the surface cache
    lantern.name = "bath_lantern_paper";
    lantern.baseColor = f3(0.9f, 0.85f, 0.7f);
    lantern.roughness = 0.9f;
    lantern.emissive = f3(3000.0f, 2300.0f, 1300.0f);
    const uint32_t lanternMat = addMaterial(s, lantern);
    Material water;
    water.name = "bath_water";
    water.cls = scene::MaterialClass::Water;
    water.baseColor = f3(0.55f, 0.82f, 0.80f);
    water.roughness = 0.03f;
    water.specular = 0.25f;  // f0 0.02 (n = 1.33)
    water.ior = 1.33f;
    const uint32_t waterMat = addMaterial(s, water);
    Material skin;
    skin.name = "figure_skin";
    skin.cls = scene::MaterialClass::Subsurface;
    skin.baseColor = f3(0.80f, 0.56f, 0.45f);
    skin.roughness = 0.45f;
    skin.specular = 0.35f;
    skin.transmission = 0.3f;
    const uint32_t skinMat = addMaterial(s, skin);
    Material robe;  // cloth: the fuzz replaces the base's highlight
    robe.name = "figure_robe";
    robe.baseColor = f3(0.10f, 0.14f, 0.32f);
    robe.roughness = 0.85f;
    robe.sheenColor = f3(0.6f, 0.65f, 0.9f);
    robe.sheenRoughness = 0.45f;
    robe.cloth = 1.0f;
    const uint32_t robeMat = addMaterial(s, robe);
    Material towel = robe;  // a striped towel: the stripes are vertex colours
    towel.name = "bath_towel";
    towel.baseColor = f3(0.85f, 0.83f, 0.78f);
    towel.sheenColor = f3(0.9f, 0.9f, 0.9f);
    towel.vertexColorTint = true;
    const uint32_t towelMat = addMaterial(s, towel);
    Material hair;
    hair.name = "figure_hair";
    hair.cls = scene::MaterialClass::Hair;
    hair.baseColor = f3(0.10f, 0.06f, 0.04f);
    hair.roughness = 0.3f;
    hair.ior = 1.55f;
    hair.hairEumelanin = 1.3f;
    hair.hairBetaN = 0.3f;
    const uint32_t hairMat = addMaterial(s, hair);
    Material pot;
    pot.name = "bath_pot_glazed";
    pot.baseColor = f3(0.55f, 0.20f, 0.12f);
    pot.roughness = 0.5f;
    pot.clearcoat = 1.0f;
    pot.clearcoatRoughness = 0.05f;
    const uint32_t potMat = addMaterial(s, pot);
    Material fern;
    fern.name = "bath_fern";
    fern.cls = scene::MaterialClass::Foliage;
    fern.baseColor = f3(0.12f, 0.30f, 0.08f);
    fern.roughness = 0.5f;
    fern.transmission = 0.35f;
    fern.twoSided = true;
    const uint32_t fernMat = addMaterial(s, fern);

    // ---- the room
    {
        // the ground outside, around the building's footprint (not under it: the basin lies below the ground's level)
        MeshBuilder b("bath_outside_ground");
        b.material(p.grass);
        b.box({ -60.0f, -1.2f, -60.0f }, { -W - T, -0.2f, 60.0f }, 1.0f / 8.0f);
        b.box({ W + T, -1.2f, -60.0f }, { 60.0f, -0.2f, 60.0f }, 1.0f / 8.0f);
        b.box({ -W - T, -1.2f, D + T }, { W + T, -0.2f, 60.0f }, 1.0f / 8.0f);
        b.box({ -W - T, -1.2f, -60.0f }, { W + T, -0.2f, -D - T }, 1.0f / 8.0f);
        addInstance(s, addMesh(s, b.finish(false)), float3x4{});
    }
    // the floor around the basin (x in [-2.2, 2.2], z in [-3.2, -0.4])
    solid(s, "bath_floor_west", floorMat, { -W, -0.2f, -D }, { -2.2f, 0.0f, D });
    solid(s, "bath_floor_east", floorMat, { 2.2f, -0.2f, -D }, { W, 0.0f, D });
    solid(s, "bath_floor_north", floorMat, { -2.2f, -0.2f, -0.4f }, { 2.2f, 0.0f, D });
    solid(s, "bath_floor_south", floorMat, { -2.2f, -0.2f, -D }, { 2.2f, 0.0f, -3.2f });
    solid(s, "bath_basin_bottom", basinMat, { -2.4f, -1.1f, -3.4f }, { 2.4f, -0.9f, -0.2f });
    solid(s, "bath_basin_west", basinMat, { -2.4f, -0.9f, -3.4f }, { -2.2f, -0.2f, -0.2f });
    solid(s, "bath_basin_east", basinMat, { 2.2f, -0.9f, -3.4f }, { 2.4f, -0.2f, -0.2f });
    solid(s, "bath_basin_north", basinMat, { -2.2f, -0.9f, -0.4f }, { 2.2f, -0.2f, -0.2f });
    solid(s, "bath_basin_south", basinMat, { -2.2f, -0.9f, -3.4f }, { 2.2f, -0.2f, -3.2f });
    {
        MeshBuilder b("bath_water");
        b.material(waterMat);
        b.quadXZ(-2.2f, -3.2f, 2.2f, -0.4f, -0.12f, 0.25f);
        addInstance(s, addMesh(s, b.finish(false)), float3x4{}, 0);
    }
    // walls (their feet 30 cm into the ground: no seam at the floor's level for the sun to find) and ceiling; the +Z wall
    // around its openings: windows x in [-3.6, -1.2] and [1.2, 3.6], the slit x in [-0.35, 0.35], all y in [0.9, 3.1]
    solid(s, "bath_wall_nz", tileMat, { -W - T, -0.5f, -D - T }, { W + T, H, -D });
    solid(s, "bath_wall_nx", tileMat, { -W - T, -0.5f, -D }, { -W, H, D });
    solid(s, "bath_wall_px", tileMat, { W, -0.5f, -D }, { W + T, H, D });
    solid(s, "bath_wall_pz_below", tileMat, { -W - T, -0.5f, D }, { W + T, 0.9f, D + T });
    solid(s, "bath_wall_pz_above", tileMat, { -W - T, 3.1f, D }, { W + T, H, D + T });
    const float columns[4][2] = { { -W - T, -3.6f }, { -1.2f, -0.35f }, { 0.35f, 1.2f }, { 3.6f, W + T } };
    for (const auto& c : columns) solid(s, "bath_wall_pz_column", tileMat, { c[0], 0.9f, D }, { c[1], 3.1f, D + T });
    solid(s, "bath_ceiling", p.plaster, { -W - T, H, -D - T }, { W + T, H + T, D + T });

    // ---- the windows: one mesh, a wooden frame (opaque) and its pane (Glass) - the pane passes GI and shadow rays through
    // the any-hit shader, not through the instance's mask. The left window's pane is amber by a material override.
    {
        MeshBuilder b("bath_window");
        b.material(woodMat);
        b.box({ -1.2f, 1.02f, -0.06f }, { 1.2f, 1.1f, 0.06f });
        b.box({ -1.2f, -1.1f, -0.06f }, { 1.2f, -1.02f, 0.06f });
        b.box({ -1.2f, -1.02f, -0.06f }, { -1.12f, 1.02f, 0.06f });
        b.box({ 1.12f, -1.02f, -0.06f }, { 1.2f, 1.02f, 0.06f });
        b.box({ -0.03f, -1.02f, -0.04f }, { 0.03f, 1.02f, 0.04f });
        b.box({ -1.12f, -0.03f, -0.04f }, { 1.12f, 0.03f, 0.04f });
        b.material(paneClear);
        b.quad4({ -1.12f, -1.02f, 0 }, { 1.12f, -1.02f, 0 }, { 1.12f, 1.02f, 0 }, { -1.12f, 1.02f, 0 }, { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 });
        const uint32_t window = addMesh(s, b.finish(true));
        addInstance(s, window, placement({ 2.4f, 2.0f, D + 0.5f * T }, 0.0f));
        Instance& left = addInstance(s, window, placement({ -2.4f, 2.0f, D + 0.5f * T }, 0.0f));
        left.materialOverrides = { woodMat, paneAmber };
    }
    // the slit's stained glass: two panes, each an instance of Glass alone (out of the GI and shadow masks)
    for (int half = 0; half < 2; ++half)
    {
        MeshBuilder b(half ? "bath_stained_red" : "bath_stained_blue");
        b.material(half ? stainedRed : stainedBlue);
        const float y0 = half ? 2.0f : 0.9f, y1 = half ? 3.1f : 2.0f, z = D + 0.5f * T;
        b.quad4({ -0.35f, y0, z }, { 0.35f, y0, z }, { 0.35f, y1, z }, { -0.35f, y1, z }, { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 });
        addInstance(s, addMesh(s, b.finish(false)), float3x4{});
    }
    // a shutter outside the right window that is only its shadow: slats in no view, no reflection, no GI ray
    {
        MeshBuilder b("bath_shutter_shadow_only");
        b.material(woodMat);
        for (int k = 0; k < 11; ++k) b.box({ 1.2f, 0.95f + 0.2f * k, D + T + 0.4f }, { 3.6f, 0.99f + 0.2f * k, D + T + 0.46f });
        addInstance(s, addMesh(s, b.finish(true)), float3x4{}, cast | scene::InstanceShadowOnly);
    }

    // ---- furniture
    solid(s, "bath_bench_seat", woodMat, { -4.9f, 0.42f, -3.2f }, { -3.9f, 0.48f, -1.2f }, 1.0f, cast, true);
    for (float lz : { -3.15f, -1.3f })
        for (float lx : { -4.85f, -4.0f }) solid(s, "bath_bench_leg", woodMat, { lx, 0.0f, lz }, { lx + 0.06f, 0.42f, lz + 0.06f }, 1.0f, cast, true);
    {
        MeshBuilder b("bath_towel");
        b.material(towelMat);
        std::vector<uint32_t> colours;  // ten bands across the towel, white and blue by turns: each band's vertices carry its colour
        for (int k = 0; k < 10; ++k)
        {
            b.box({ -4.6f, 0.48f, -2.6f + 0.05f * k }, { -4.1f, 0.56f, -2.55f + 0.05f * k });
            colours.resize(b.mesh.positions.size(), (k & 1) ? 0xFFB06030u : 0xFFFFFFFFu);
        }
        Mesh m = b.finish(false);
        m.colors = std::move(colours);
        addInstance(s, addMesh(s, std::move(m)), float3x4{});
    }
    // a fern in a glazed pot in the sun patch: its fronds take no shadow of themselves (the sun's slot leaves out the
    // casters inside the instance's own bounds)
    {
        MeshBuilder b("bath_pot");
        b.material(potMat);
        b.cylinder({ -4.2f, 0.0f, 3.0f }, { 0, 0.5f, 0 }, 0.22f, 0.3f, 24, 2, true);
        addInstance(s, addMesh(s, b.finish(false)), float3x4{});
        MeshBuilder f("bath_fern");
        f.material(fernMat);
        const float3 c = f3(-4.2f, 0.5f, 3.0f);
        for (int k = 0; k < 18; ++k)
        {
            const float yaw = 2 * kPi * k / 18 + 0.3f * (k & 1), rise = 0.35f + 0.25f * (k % 3);
            const float3 out = f3(std::cos(yaw), 0, std::sin(yaw)), side = f3(-std::sin(yaw), 0, std::cos(yaw));
            const float3 tip = c + out * 0.75f + f3(0, rise, 0);
            f.quad4(c - side * 0.03f, c + side * 0.03f, tip + side * 0.09f, tip - side * 0.09f, { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 });
        }
        addInstance(s, addMesh(s, f.finish(false)), float3x4{}, cast | scene::InstanceNoSelfShadow);
    }
    // paper lanterns on the floor around the bath (9 cm cubes: under the mesh cards' size rule but for the emissive
    // light sources' fifth of it)
    {
        MeshBuilder b("bath_lantern");
        b.material(lanternMat);
        b.box({ -0.045f, 0.0f, -0.045f }, { 0.045f, 0.09f, 0.045f });
        const uint32_t mesh = addMesh(s, b.finish(false));
        const float at[6][2] = { { -2.6f, -3.5f }, { 0.0f, -3.6f }, { 2.6f, -3.5f }, { -2.6f, -0.1f }, { 2.6f, -0.1f }, { -4.6f, -0.6f } };
        for (const auto& q : at) addInstance(s, mesh, placement({ q[0], 0.0f, q[1] }, 0.4f), 0);
    }

    // ---- lights
    const float3 warm = luminanceNormalised(f3(1.0f, 0.78f, 0.55f));
    // pendant lamps: a bulb under an open cone of amber glass (an instance of Glass alone that casts shadows). Below the
    // cone's rim the lamp's light is direct; what leaves upwards and sideways crosses the glass - clear to the view's
    // sampled shadow rays, amber to the hits' and the cards' tinted rays. The middle lamp carries a gobo (the extra).
    const float3 pendants[3] = { { -3.0f, 3.0f, 1.2f }, { 0.0f, 3.0f, 2.2f }, { 3.0f, 3.0f, 1.2f } };
    const uint32_t pendantLight = (uint32_t)s.lights.size();
    {
        MeshBuilder shade("bath_lamp_shade");
        shade.material(shadeMat);
        shade.cylinder({ 0.0f, -0.06f, 0.0f }, { 0.0f, 0.2f, 0.0f }, 0.2f, 0.04f, 32, 4, false);
        const uint32_t shadeMesh = addMesh(s, shade.finish(false));
        MeshBuilder bulbMesh("bath_bulb");
        bulbMesh.material(bulbMat);
        bulbMesh.sphere({ 0, 0, 0 }, 0.035f, 16, 8);
        const uint32_t bulbIndex = addMesh(s, bulbMesh.finish(false));
        for (const float3& c : pendants)
        {
            solid(s, "bath_lamp_cord", p.metal, c + f3(-0.006f, 0.14f, -0.006f), f3(c.x + 0.006f, H, c.z + 0.006f));
            addInstance(s, shadeMesh, float3x4::translation(c));
            addInstance(s, bulbIndex, float3x4::translation(c), 0);
            scene::Light l;
            l.type = scene::LightType::Point;
            l.position = c;
            l.color = warm;
            l.intensity = 800.0f;  // 89 lux on the floor under it
            l.range = 9.0f;
            l.castShadow = true;
            l.rayEndBias = 0.05f;  // (past the bulb)
            s.lights.push_back(l);
        }
    }
    // a spot above the bath, down through the steam; its cookie is the scene's extra (a slowly turning leaf lattice)
    const uint32_t cookieLight = (uint32_t)s.lights.size();
    {
        scene::Light l;
        l.type = scene::LightType::Spot;
        l.position = f3(0.0f, 3.9f, -1.8f);
        l.forward = f3(0, -1, 0);
        l.right = f3(1, 0, 0);
        l.color = luminanceNormalised(f3(0.85f, 0.92f, 1.0f));
        l.intensity = 6000.0f;  // 370 lux on the water
        l.range = 8.0f;
        l.spotInner = 0.30f;
        l.spotOuter = 0.55f;
        l.castShadow = true;
        s.lights.push_back(l);
    }
    // a wall sconce with a photometric profile (the extra: a batwing distribution)
    const uint32_t iesLight = (uint32_t)s.lights.size();
    {
        solid(s, "bath_sconce", p.metal, { -W, 2.64f, 0.38f }, { -W + 0.2f, 2.7f, 0.62f });
        scene::Light l;
        l.type = scene::LightType::Spot;
        l.position = f3(-4.85f, 2.6f, 0.5f);
        l.forward = normalize(f3(0.35f, -1.0f, 0.0f));
        l.right = f3(0, 0, 1);
        l.color = warm;
        l.intensity = 2500.0f;
        l.range = 7.0f;
        l.spotInner = 1.0f;
        l.spotOuter = 1.35f;
        l.castShadow = true;
        s.lights.push_back(l);
    }
    // a rect light over the bench that shows an image (a paper screen's lattice) between barn doors
    {
        const uint32_t image = colourTexture(s, "bath_light_screen", 64, [](float u, float v) {
            const float bar = std::min(std::fabs(std::fmod(u * 4.0f, 1.0f) - 0.5f), std::fabs(std::fmod(v * 2.0f, 1.0f) - 0.5f)) < 0.06f ? 0.08f : 1.0f;
            return float4{ 1.0f * bar, 0.86f * bar, 0.62f * bar, 1.0f };
        }, false);
        scene::Light l;
        l.type = scene::LightType::Rect;
        l.position = f3(-4.4f, 3.9f, -2.2f);
        l.forward = f3(0, -1, 0);
        l.right = f3(0, 0, 1);
        l.size = { 1.6f, 0.7f };
        l.intensity = 6000.0f;  // nits: about 580 lux on the bench
        l.range = 7.0f;
        l.castShadow = true;
        l.sourceTexture = image;
        l.barnDoorLength = 0.25f;
        l.barnDoorAngle = 0.5f;
        s.lights.push_back(l);
    }

    // ---- the figure at the bath's east edge, turned to its camera: skin, eyes, strand hair (the groom), a cloth robe.
    // It is in lighting channels 0 and 1; a rim light behind it is in channel 1 alone and lights nothing else. Decals
    // do not paint it.
    const float3 feet = f3(2.7f, 0.0f, 0.9f), head = feet + f3(0, 1.62f, 0), figureCamera = f3(2.35f, 1.58f, 2.05f);
    const uint32_t figureFlags = scene::withLightingChannels(cast | scene::InstanceNoDecals, 3);
    {
        auto part = [&](const char* name, uint32_t material, const std::function<void(MeshBuilder&)>& build, uint32_t flags) {
            MeshBuilder b(name);
            b.material(material);
            build(b);
            addInstance(s, addMesh(s, b.finish(false)), float3x4{}, flags);
        };
        part("figure_robe", robeMat, [&](MeshBuilder& b) {
            b.cylinder(feet + f3(0, 0.02f, 0), { 0, 1.36f, 0 }, 0.27f, 0.16f, 32, 6, true);
            for (float side : { -1.0f, 1.0f }) b.cylinder(feet + f3(side * 0.19f, 1.34f, 0), { side * 0.10f, -0.52f, 0.05f }, 0.065f, 0.055f, 16, 3, true);
        }, figureFlags);
        part("figure_skin", skinMat, [&](MeshBuilder& b) {
            b.cylinder(feet + f3(0, 1.36f, 0), { 0, 0.17f, 0 }, 0.055f, 0.05f, 16, 2, false);
            b.sphere(head, 0.105f, 64, 32);
            for (float side : { -1.0f, 1.0f }) b.sphere(feet + f3(side * 0.29f, 0.80f, 0.05f), 0.045f, 24, 12);
        }, figureFlags);
        const float irisRadius = 0.245f, eyeRadius = 0.0125f;
        Material eyeMaterial;
        eyeMaterial.name = "figure_eye";
        eyeMaterial.cls = scene::MaterialClass::Subsurface;
        eyeMaterial.baseColor = f3(1, 1, 1);
        eyeMaterial.baseColorTexture = addTexture(s, eyeTexture(256, irisRadius));
        eyeMaterial.roughness = 0.12f;
        eyeMaterial.specular = 0.31f;
        eyeMaterial.eyeIrisRadius = irisRadius;
        MeshBuilder eye("figure_eye");
        eye.material(addMaterial(s, eyeMaterial));
        eyeball(eye, 48, 24);
        const uint32_t eyeMesh = addMesh(s, eye.finish(false));
        // (the face is the side of the head towards the figure's camera: the eyes look at it)
        const float3 z = normalize(figureCamera - head), x = normalize(cross(f3(0, 1, 0), z)), y = cross(z, x);
        for (float side : { -1.0f, 1.0f })
        {
            const float3 centre = head + z * 0.094f + x * (side * 0.034f) + f3(0, 0.018f, 0);
            float3x4 m;
            const float3 axes[3] = { x, y, z };
            for (int c = 0; c < 3; ++c) m.m[0][c] = axes[c].x * eyeRadius, m.m[1][c] = axes[c].y * eyeRadius, m.m[2][c] = axes[c].z * eyeRadius;
            m.m[0][3] = centre.x, m.m[1][3] = centre.y, m.m[2][3] = centre.z;
            addInstance(s, eyeMesh, m, scene::withLightingChannels(scene::InstanceNoDecals, 3));  // (no shadow: as shading_ball's eyes)
        }
        if (grooms)
        {
            // the hair gate's body on this head: 2,500 guides x 12 nodes of 2.5 cm, 16 follow strands each; roots on the
            // upper scalp behind the hairline (none on the quarter of the head that is the face)
            Groom g;
            g.head = head;
            g.headRadius = 0.105f;
            g.material = hairMat;
            g.nodesPerStrand = 12;
            Rng r(11, 420);
            const float3 face = normalize(f3(z.x, 0, z.z));
            for (uint32_t k = 0; k < 2500;)
            {
                const float phi = 2 * kPi * r.uniform(), c = 0.1f + 0.88f * r.uniform(), sn = std::sqrt(1 - c * c);
                const float3 n{ sn * std::cos(phi), c, sn * std::sin(phi) };
                if (dot(n, face) > 0.25f) continue;  // (the generator's sequence decides: the same roots every time)
                ++k;
                float3 dir = n + f3(0, -1.5f, 0);
                dir = normalize(dir - n * std::fmin(dot(dir, n), 0.0f));
                for (uint32_t node = 0; node < g.nodesPerStrand; ++node) g.restPositions.push_back(n * 0.108f + dir * (0.025f * node));
                for (uint32_t f = 0; f < 16; ++f) g.follows.push_back({ k - 1, f3(0, 0.003f * (r.uniform() - 0.5f), 0.003f * (r.uniform() - 0.5f)), 1.2f });
            }
            grooms->push_back(std::move(g));
        }
        scene::Light rim;
        rim.type = scene::LightType::Spot;
        rim.position = f3(4.6f, 2.6f, -1.6f);
        rim.forward = normalize(head - rim.position);
        rim.right = f3(0, 0, 1);
        rim.color = luminanceNormalised(f3(0.75f, 0.85f, 1.0f));
        rim.intensity = 4000.0f;  // about 420 lux on the head
        rim.range = 8.0f;
        rim.spotInner = 0.25f;
        rim.spotOuter = 0.40f;
        rim.castShadow = true;
        rim.lightingChannels = 2;  // channel 1: the figure alone
        s.lights.push_back(rim);
    }

    // ---- steam over the water: a box of rising, turbulent medium (the lamps' and the cookie light's cones show in it)
    {
        scene::FogVolume steam;
        steam.centre = { 0.0f, 0.9f, -1.8f };
        steam.halfSize = { 2.4f, 1.0f, 1.6f };
        steam.shape = 1;
        steam.density = 0.18f;
        steam.heightFalloff = 1.5f;
        steam.edge = 0.4f;
        steam.sourcePlane = 0.05f;
        steam.riseSpeed = 0.3f;
        steam.turbulence = 0.6f;
        steam.turbulenceScale = 0.4f;
        s.fogVolumes.push_back(steam);
    }

    // ---- extras: the light functions and the decals (their materials are scene materials no mesh uses)
    Material stain;
    stain.name = "bath_decal_stain";
    stain.baseColor = f3(0.30f, 0.24f, 0.15f);
    stain.roughness = 0.8f;
    stain.baseColorTexture = colourTexture(s, "bath_decal_stain", 128, [](float u, float v) {
        const float d = std::sqrt((u - 0.5f) * (u - 0.5f) * 1.6f + (v - 0.4f) * (v - 0.4f));
        const float a = smoothstepf(0.42f, 0.15f, d + 0.12f * fbm2(u * 6.0f, v * 6.0f, 331u, 3)) * (0.6f + 0.4f * smoothstepf(0.2f, 0.9f, v));
        return float4{ 1, 1, 1, a };
    }, false);
    const uint32_t stainMat = addMaterial(s, stain);
    Material mark;
    mark.name = "bath_decal_mark";
    mark.baseColor = f3(0.85f, 0.15f, 0.10f);
    mark.roughness = 0.4f;
    mark.baseColorTexture = colourTexture(s, "bath_decal_mark", 128, [](float u, float v) {
        const float d = std::sqrt((u - 0.5f) * (u - 0.5f) + (v - 0.5f) * (v - 0.5f));
        const float ring = smoothstepf(0.02f, 0.0f, std::fabs(d - 0.38f) - 0.05f), bar = std::fabs(v - 0.5f) < 0.06f && std::fabs(u - 0.5f) < 0.3f ? 1.0f : 0.0f;
        return float4{ 1, 1, 1, std::max(ring, bar) };
    }, false);
    const uint32_t markMat = addMaterial(s, mark);
    Material puddle;
    puddle.name = "bath_decal_puddle";
    puddle.baseColor = f3(1, 1, 1);
    puddle.roughness = 0.03f;
    puddle.baseColorTexture = colourTexture(s, "bath_decal_puddle", 128, [](float u, float v) {
        const float d = std::sqrt((u - 0.5f) * (u - 0.5f) + (v - 0.5f) * (v - 0.5f)) + 0.1f * fbm2(u * 4.0f, v * 4.0f, 337u, 3);
        return float4{ 1, 1, 1, smoothstepf(0.45f, 0.30f, d) };
    }, false);
    const uint32_t puddleMat = addMaterial(s, puddle);
    if (extras)
    {
        ExtraLightFunction cookie;
        cookie.light = cookieLight;
        cookie.profile = 2;
        cookie.tanX = cookie.tanY = std::tan(0.55f);
        cookie.rotationSpeed = 0.15f;
        lightImage(cookie, 64, [](float u, float v) {  // leaves: bright gaps between dark blades
            const float a = std::atan2(v - 0.5f, u - 0.5f), r = std::sqrt((u - 0.5f) * (u - 0.5f) + (v - 0.5f) * (v - 0.5f));
            const float blade = 0.5f + 0.5f * std::sin(9.0f * a + 14.0f * r);
            const float open = smoothstepf(0.35f, 0.65f, blade) * smoothstepf(0.5f, 0.42f, r);
            return f3(open, open, open);
        });
        extras->lightFunctions.push_back(std::move(cookie));
        ExtraLightFunction ies;
        ies.light = iesLight;
        ies.profile = 1;
        for (int deg = 0; deg <= 90; deg += 5)
        {
            // a batwing: the peak 35 degrees off the axis, half of it on the axis, nothing past 80
            const float x = (float)deg, peak = std::exp(-(x - 35.0f) * (x - 35.0f) / (2.0f * 14.0f * 14.0f));
            ies.iesVertical.push_back(x);
            ies.iesValues.push_back(deg > 80 ? 0.0f : std::max(peak, 0.5f * std::exp(-x * x / (2.0f * 20.0f * 20.0f))));
        }
        extras->lightFunctions.push_back(std::move(ies));
        // the middle pendant lamp's gobo: a pierced screen all around the bulb - spots of light on the walls and in the
        // steam, more light downwards than upwards
        ExtraLightFunction gobo;
        gobo.light = pendantLight + 1;
        gobo.profile = 3;
        lightImage(gobo, 128, [](float u, float v) {
            const float du = u * 24.0f - std::floor(u * 24.0f) - 0.5f, dv = v * 12.0f - std::floor(v * 12.0f) - 0.5f;
            const float hole = smoothstepf(0.38f, 0.26f, std::sqrt(du * du + dv * dv));
            const float level = (0.15f + 0.85f * hole) * lerpf(1.0f, 0.4f, smoothstepf(0.4f, 0.6f, v));  // (v from the forward axis: down)
            return f3(level, level, level);
        });
        extras->lightFunctions.push_back(std::move(gobo));
        // a stain on the +X wall (base colour and roughness, fading on surfaces turned from it), a painted mark on the
        // floor at the bath's edge (base colour only), a puddle under the figure (roughness only: the floor there turns
        // glossy, the figure - InstanceNoDecals - does not)
        ExtraDecal wall = makeDecal(decalBox({ W - 0.03f, 2.2f, -2.0f }, { 0, 0, 0.6f }, { 0, 0.8f, 0 }, { -0.15f, 0, 0 }), stainMat, 1 | 4);
        wall.fadeStartDegrees = 50.0f;
        wall.fadeEndDegrees = 75.0f;
        extras->decals.push_back(wall);
        ExtraDecal floorMark = makeDecal(decalBox({ 2.9f, 0.0f, -1.8f }, { 0.35f, 0, 0 }, { 0, 0, -0.35f }, { 0, 0.1f, 0 }), markMat, 1);
        floorMark.opacity = 0.9f;
        floorMark.priority = 1;
        extras->decals.push_back(floorMark);
        ExtraDecal wet = makeDecal(decalBox({ feet.x, 0.0f, feet.z }, { 0.7f, 0, 0 }, { 0, 0, -0.7f }, { 0, 0.3f, 0 }), puddleMat, 4);
        wet.edge = 0.5f;
        extras->decals.push_back(wet);
    }

    // the exposure of a room with sun patches: the patches (some 5,000 nits on the floor) stand four stops over the key,
    // the lamps' light (50 to 100 nits) two to three under it
    const float ev = 11.5f;
    s.cameras.push_back(camera("room", { -4.4f, 1.7f, 3.3f }, { 1.5f, 1.0f, -2.0f }, ev, 65.0f));
    // from the bath's far end towards the windows: the panes' colours on the floor and the water, the stained slit, the
    // shutter's stripes
    s.cameras.push_back(camera("panes", { 0.0f, 1.5f, -3.6f }, { 0.0f, 1.8f, 4.0f }, ev, 65.0f));
    s.cameras.push_back(camera("bath", { -3.4f, 1.9f, -0.2f }, { 0.5f, -0.1f, -1.9f }, ev));
    // head and shoulders: skin, the eyes (turned to this camera), the hair, the robe's cloth; the rim light of its channel
    s.cameras.push_back(camera("figure", figureCamera, head - f3(0, 0.05f, 0), ev - 0.5f, 35.0f));
    // the middle pendant lamp from below its rim: the glass cone, the bulb, the gobo's spots on the wall behind
    s.cameras.push_back(camera("lamp", { 0.9f, 2.4f, 3.3f }, pendants[1], ev - 0.5f, 30.0f));
    // along the +X wall, low: the tiles' grout under parallax, the glaze, the wet floor
    s.cameras.push_back(camera("tiles", { 4.2f, 0.5f, -2.2f }, { 4.95f, 0.6f, -3.2f }, ev - 1.0f, 40.0f));
    // the bench under the rect light: the screen's lattice and the barn doors' cut on the wood (detail maps) and the wall,
    // the striped towel (vertex colours), the emitter's image in the wet floor
    s.cameras.push_back(camera("bench", { -2.7f, 1.5f, -0.9f }, { -4.4f, 0.5f, -2.2f }, ev - 1.0f, 50.0f));
    for (const auto& c : s.cameras) s.paths.push_back(staticPath(c));
    return s;
}

// ---------------------------------------------------------------------------------------------------------------------
// showcase_atrium. A courtyard 16 x 16 m, 9 m to its roof of 2 m panes in a steel grid; a gallery 4 m up along the -Z
// wall behind a glass railing. The sun stands 62 degrees high.
Scene atrium(const Request& rq, SceneExtras* extras)
{
    Scene s;
    s.name = "showcase_atrium";
    s.sun.direction = sunDirection(62.0f, 55.0f);
    s.windDirection = normalize(f3(0.8f, 0, 0.6f));
    s.windSpeed = 0.6f;
    const Palette p = buildPalette(s, rq.seed, false);
    const float W = 8.0f, H = 9.0f, T = 0.4f;
    const uint32_t cast = scene::InstanceCastShadow;

    // the floor: stone slabs with detail maps
    Material stone;
    stone.name = "atrium_stone";
    stone.baseColor = f3(1, 1, 1);
    stone.roughness = 0.55f;
    stone.baseColorTexture = colourTexture(s, "atrium_stone", 256, [](float u, float v) {
        const float t = tilePattern(u, v, 2.0f, 0.012f);
        const float3 c = f3(0.20f, 0.19f, 0.18f) * (1 - t) + f3(0.56f, 0.53f, 0.49f) * (t * (0.9f + 0.1f * fbm2(u * 8.0f, v * 8.0f, 401u, 3, 8)));
        return float4{ c.x, c.y, c.z, 1.0f };
    });
    stone.detailColorTexture = colourTexture(s, "atrium_stone_detail", 128, [](float u, float v) {
        const float g = 0.2176f * (0.6f + 0.8f * fbm2(u * 8.0f, v * 8.0f, 402u, 4, 8));
        return float4{ g, g, g, 1.0f };
    });
    stone.detailNormalTexture =
        addTexture(s, normalMapFromHeight("atrium_stone_detail_normal", 128, 0.25f, [](float u, float v) { return 0.0012f * fbm2(u * 16.0f, v * 16.0f, 403u, 3, 16); }));
    stone.detailScale = { 4, 4 };
    const uint32_t stoneMat = addMaterial(s, stone);
    solid(s, "atrium_floor", stoneMat, { -W - T, -0.3f, -W - T }, { W + T, 0.0f, W + T }, 0.5f, cast, true);
    solid(s, "atrium_wall_nz", p.plaster, { -W - T, 0.0f, -W - T }, { W + T, H, -W });
    solid(s, "atrium_wall_pz", p.brick, { -W - T, 0.0f, W }, { W + T, H, W + T });
    solid(s, "atrium_wall_nx", p.plaster, { -W - T, 0.0f, -W }, { -W, H, W });
    solid(s, "atrium_wall_px", p.brick, { W, 0.0f, -W }, { W + T, H, W });

    // ---- the roof: ONE mesh - the steel grid (opaque) and the panes in four glass materials. The sun reaches the floor
    // through the panes' colours: the view's tinted sun shadow, the cards' tinted direct light, the hits' shadow rays.
    {
        const uint32_t tints[4] = { addMaterial(s, glassPane("atrium_pane_clear", f3(0.90f, 0.95f, 0.93f))), addMaterial(s, glassPane("atrium_pane_amber", f3(0.95f, 0.66f, 0.20f))),
                                    addMaterial(s, glassPane("atrium_pane_blue", f3(0.22f, 0.45f, 0.92f))), addMaterial(s, glassPane("atrium_pane_green", f3(0.25f, 0.80f, 0.40f))) };
        MeshBuilder b("atrium_roof");
        b.material(p.metal);
        for (int k = 0; k <= 8; ++k)
        {
            const float c = -W + 2.0f * k;
            b.box({ c - 0.06f, H - 0.1f, -W - T }, { c + 0.06f, H + 0.1f, W + T });
            b.box({ -W - T, H - 0.1f, c - 0.06f }, { W + T, H + 0.1f, c + 0.06f });
        }
        for (int t = 0; t < 4; ++t)
        {
            b.material(tints[t]);
            for (int j = 0; j < 8; ++j)
                for (int i = 0; i < 8; ++i)
                {
                    // mostly clear, the colours on a diagonal pattern
                    const int pick = ((i + 2 * j) % 5 == 0) ? 1 : ((i * 3 + j) % 7 == 0) ? 2 : ((i + j * 5) % 11 == 0) ? 3 : 0;
                    if (pick != t) continue;
                    const float x0 = -W + 2.0f * i + 0.06f, z0 = -W + 2.0f * j + 0.06f, x1 = x0 + 1.88f, z1 = z0 + 1.88f;
                    b.quadXZ(x0, z0, x1, z1, H + 0.05f, 0.5f);
                }
        }
        addInstance(s, addMesh(s, b.finish(true)), float3x4{});
    }

    // ---- the gallery along the -Z wall: a slab on posts, a railing of glass panes (instances of Glass alone), small
    // emissive lamps under it (6 cm: emissive light sources by the size rule), two downlights
    solid(s, "atrium_gallery", p.concrete, { -W, 3.8f, -W }, { W, 4.0f, -5.5f });
    for (float x : { -6.0f, -2.0f, 2.0f, 6.0f }) solid(s, "atrium_gallery_post", p.concrete, { x - 0.15f, 0.0f, -5.8f }, { x + 0.15f, 3.8f, -5.5f });
    {
        MeshBuilder b("atrium_railing_pane");
        b.material(addMaterial(s, glassPane("atrium_railing_glass", f3(0.88f, 0.94f, 0.92f))));
        b.quad4({ -0.95f, 0.0f, 0 }, { 0.95f, 0.0f, 0 }, { 0.95f, 1.1f, 0 }, { -0.95f, 1.1f, 0 }, { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 });
        const uint32_t pane = addMesh(s, b.finish(false));
        for (int k = 0; k < 8; ++k) addInstance(s, pane, placement({ -7.0f + 2.0f * k, 4.0f, -5.52f }, 0.0f));
        solid(s, "atrium_railing_rail", p.metal, { -W, 5.1f, -5.55f }, { W, 5.14f, -5.49f });
    }
    {
        Material lamp;
        lamp.name = "atrium_marker_lamp";
        lamp.baseColor = f3(0.9f, 0.9f, 0.9f);
        lamp.emissive = f3(9000.0f, 6500.0f, 3500.0f);
        MeshBuilder b("atrium_marker_lamp");
        b.material(addMaterial(s, lamp));
        b.box({ -0.03f, -0.03f, -0.03f }, { 0.03f, 0.03f, 0.03f });
        const uint32_t mesh = addMesh(s, b.finish(false));
        for (int k = 0; k < 6; ++k) addInstance(s, mesh, placement({ -6.5f + 2.6f * k, 3.74f, -6.6f }, 0.0f), 0);
        for (float x : { -4.0f, 4.0f })
        {
            scene::Light l;
            l.type = scene::LightType::Spot;
            l.position = f3(x, 3.75f, -6.8f);
            l.forward = f3(0, -1, 0);
            l.color = luminanceNormalised(f3(1.0f, 0.82f, 0.62f));
            l.intensity = 5000.0f;  // 360 lux on the floor under the gallery
            l.range = 7.0f;
            l.spotInner = 0.5f;
            l.spotOuter = 0.9f;
            l.castShadow = true;
            s.lights.push_back(l);
        }
    }

    // ---- planters with trees (leaf cards: Foliage with transmission), lacquered benches, cloth banners and a carpet
    {
        const FoliageAssets f = buildFoliage(s, p, rq.seed ^ 0xA7A7ull, FoliageStyle{ false });
        const float at[4][2] = { { -4.5f, 3.5f }, { 4.5f, 3.5f }, { -4.5f, -2.0f }, { 4.5f, -2.0f } };
        for (int k = 0; k < 4; ++k)
        {
            const float x = at[k][0], z = at[k][1];
            solid(s, "atrium_planter", p.concrete, { x - 0.9f, 0.0f, z - 0.9f }, { x + 0.9f, 0.6f, z + 0.9f });
            solid(s, "atrium_planter_soil", p.soil, { x - 0.8f, 0.6f, z - 0.8f }, { x + 0.8f, 0.62f, z + 0.8f });
            Instance& tree = addInstance(s, f.treeMeshes[k & 3], placement({ x, 0.55f, z }, 0.7f * k, 0.42f), cast | scene::InstanceWind);
            tree.wind = { 20.0f, 1.3f * k, 1.0f };
        }
    }
    {
        Material lacquer;
        lacquer.name = "atrium_lacquer";
        lacquer.baseColor = f3(0.45f, 0.04f, 0.03f);
        lacquer.roughness = 0.5f;
        lacquer.clearcoat = 1.0f;
        lacquer.clearcoatRoughness = 0.04f;
        const uint32_t lacquerMat = addMaterial(s, lacquer);
        for (float z : { 6.3f, -4.4f })
        {
            solid(s, "atrium_bench_seat", lacquerMat, { -1.4f, 0.42f, z - 0.25f }, { 1.4f, 0.5f, z + 0.25f });
            for (float x : { -1.3f, 1.2f }) solid(s, "atrium_bench_leg", lacquerMat, { x, 0.0f, z - 0.2f }, { x + 0.1f, 0.42f, z + 0.2f });
        }
        Material cloth;
        cloth.name = "atrium_banner_cloth";
        cloth.baseColor = f3(1, 1, 1);
        cloth.roughness = 0.8f;
        cloth.sheenColor = f3(0.9f, 0.85f, 0.8f);
        cloth.sheenRoughness = 0.5f;
        cloth.cloth = 1.0f;
        cloth.twoSided = true;
        cloth.baseColorTexture = colourTexture(s, "atrium_banner", 64, [](float u, float v) {
            const float band = std::fabs(u - 0.5f) < 0.12f || std::fmod(v * 6.0f, 1.0f) < 0.12f ? 1.0f : 0.0f;
            const float3 c = f3(0.55f, 0.07f, 0.06f) * (1 - band) + f3(0.80f, 0.62f, 0.20f) * band;
            return float4{ c.x, c.y, c.z, 1.0f };
        });
        const uint32_t clothMat = addMaterial(s, cloth);
        for (float x : { -3.0f, 0.0f, 3.0f })
        {
            MeshBuilder b("atrium_banner");
            b.material(clothMat);
            b.quad4({ x - 0.6f, 4.3f, -5.3f }, { x + 0.6f, 4.3f, -5.3f }, { x + 0.6f, 8.3f, -5.3f }, { x - 0.6f, 8.3f, -5.3f }, { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 });
            addInstance(s, addMesh(s, b.finish(false)), float3x4{});
        }
        Material carpet = cloth;
        carpet.name = "atrium_carpet";
        carpet.twoSided = false;
        carpet.baseColorTexture = scene::kNone;
        carpet.baseColor = f3(0.12f, 0.20f, 0.30f);
        solid(s, "atrium_carpet", addMaterial(s, carpet), { -1.0f, 0.0f, -4.0f }, { 1.0f, 0.015f, 5.5f });
    }
    // ---- solid glass on a plinth in the middle: a sphere and a prism (one-sided bodies: Fresnel at both faces and
    // absorption over the path inside, on the tinted shadow rays)
    solid(s, "atrium_plinth", p.concrete, { -0.6f, 0.015f, -0.6f }, { 0.6f, 0.9f, 0.6f });
    {
        MeshBuilder b("atrium_glass_sphere");
        b.material(addMaterial(s, glassSolid("atrium_glass_sea", f3(0.55f, 0.85f, 0.75f), 0.6f)));
        b.sphere({ 0.0f, 1.5f, 0.0f }, 0.6f, 64, 32);
        addInstance(s, addMesh(s, b.finish(false)), float3x4{});
        MeshBuilder c("atrium_glass_prism");
        c.material(addMaterial(s, glassSolid("atrium_glass_ruby", f3(0.85f, 0.12f, 0.15f), 0.25f)));
        c.box({ 1.6f, 0.0f, 1.2f }, { 1.9f, 1.4f, 1.5f });
        addInstance(s, addMesh(s, c.finish(false)), float3x4{});
    }

    // ---- extras: decals
    Material rose;
    rose.name = "atrium_decal_rose";
    rose.baseColor = f3(0.10f, 0.12f, 0.16f);
    rose.roughness = 0.35f;
    rose.baseColorTexture = colourTexture(s, "atrium_decal_rose", 256, [](float u, float v) {
        const float a = std::atan2(v - 0.5f, u - 0.5f), r = std::sqrt((u - 0.5f) * (u - 0.5f) + (v - 0.5f) * (v - 0.5f));
        const float star = smoothstepf(0.02f, 0.0f, r - 0.46f * (0.35f + 0.65f * std::pow(std::fabs(std::cos(4.0f * a)), 6.0f)));
        const float ring = smoothstepf(0.012f, 0.0f, std::fabs(r - 0.47f) - 0.012f);
        return float4{ 1, 1, 1, std::max(star, ring) };
    }, false);
    const uint32_t roseMat = addMaterial(s, rose);
    Material poster;
    poster.name = "atrium_decal_poster";
    poster.baseColor = f3(1, 1, 1);
    poster.roughness = 0.6f;
    poster.baseColorTexture = colourTexture(s, "atrium_decal_poster", 128, [](float u, float v) {
        const float3 c = f3(0.9f, 0.85f, 0.7f) * (1 - v) + f3(0.2f, 0.45f, 0.6f) * v;
        const float sun = smoothstepf(0.2f, 0.17f, std::sqrt((u - 0.6f) * (u - 0.6f) + (v - 0.35f) * (v - 0.35f)));
        const float3 d = c * (1 - sun) + f3(0.95f, 0.5f, 0.1f) * sun;
        return float4{ d.x, d.y, d.z, 0.92f };
    }, false);
    const uint32_t posterMat = addMaterial(s, poster);
    Material cracks;  // a normal-only decal: hairline cracks in the floor
    cracks.name = "atrium_decal_cracks";
    cracks.baseColor = f3(1, 1, 1);
    cracks.roughness = 0.6f;
    cracks.baseColorTexture = colourTexture(s, "atrium_decal_cracks_mask", 64, [](float u, float v) {
        const float d = std::sqrt((u - 0.5f) * (u - 0.5f) + (v - 0.5f) * (v - 0.5f));
        return float4{ 1, 1, 1, smoothstepf(0.5f, 0.3f, d) };
    }, false);
    cracks.normalTexture = addTexture(s, normalMapFromHeight("atrium_decal_cracks", 128, 1.0f, [](float u, float v) {
        const float line = std::fabs(std::sin(9.0f * u + 2.5f * std::sin(5.0f * v)) * std::sin(7.0f * v + 2.0f * std::sin(6.0f * u)));
        return -0.004f * smoothstepf(0.06f, 0.0f, line);
    }));
    const uint32_t cracksMat = addMaterial(s, cracks);
    if (extras)
    {
        // a compass rose on the floor in front of the plinth (base colour; fading out when it is small on screen)
        ExtraDecal floorRose = makeDecal(decalBox({ 0.0f, 0.0f, 3.0f }, { 1.5f, 0, 0 }, { 0, 0, -1.5f }, { 0, 0.15f, 0 }), roseMat, 1);
        floorRose.fadeScreenSize = 0.02f;
        floorRose.priority = 1;  // (over the carpet's edge)
        extras->decals.push_back(floorRose);
        // a poster on the -X wall (the sunlit one), tinted, fading in over the first seconds
        ExtraDecal wallPoster = makeDecal(decalBox({ -W + 0.02f, 2.4f, 2.0f }, { 0, 0, -0.9f }, { 0, 1.2f, 0 }, { 0.12f, 0, 0 }), posterMat, 1 | 4);
        wallPoster.color = f3(1.0f, 0.95f, 0.9f);
        wallPoster.fadeInStart = 1.0f;
        wallPoster.fadeInDuration = 2.0f;
        extras->decals.push_back(wallPoster);
        // cracks in the floor near a planter: the normal alone
        extras->decals.push_back(makeDecal(decalBox({ -3.0f, 0.0f, 0.8f }, { 1.2f, 0, 0 }, { 0, 0, -1.2f }, { 0, 0.1f, 0 }), cracksMat, 2));
    }

    const float ev = 13.5f;  // sunlit through the roof: the clear panes' patches two stops over the key
    // across the floor from a corner: the panes' colours on the stone, the carpet and the plinth; the compass rose
    s.cameras.push_back(camera("floor", { -6.5f, 1.7f, 6.5f }, { 2.0f, 0.5f, -2.0f }, ev, 65.0f));
    // up into the roof: the panes against the sky, the steel grid between them
    s.cameras.push_back(camera("roof", { 0.0f, 1.6f, 5.0f }, { 0.0f, 9.0f, -1.0f }, ev + 1.0f, 70.0f));
    // the cloth banners over the gallery in the coloured sun, the railing's panes under them
    s.cameras.push_back(camera("banners", { 2.5f, 1.7f, 1.5f }, { 0.0f, 6.0f, -5.3f }, ev, 50.0f));
    // the glass sphere and the prism: their tinted shadows on the plinth and the carpet
    s.cameras.push_back(camera("sculpture", { 2.6f, 1.6f, 3.0f }, { 0.3f, 1.3f, 0.2f }, ev, 45.0f));
    // under the gallery, out of the sun: the small lamps' and the downlights' light
    s.cameras.push_back(camera("gallery", { -6.0f, 1.6f, -3.0f }, { 3.0f, 2.8f, -6.8f }, ev - 2.0f));
    // a tree in its planter against the sunlit wall: the leaves' transmission, the lacquered bench behind
    s.cameras.push_back(camera("planter", { -6.6f, 1.5f, 5.6f }, { -4.5f, 2.6f, 3.5f }, ev, 50.0f));
    for (const auto& c : s.cameras) s.paths.push_back(staticPath(c));
    return s;
}

// ---------------------------------------------------------------------------------------------------------------------
// showcase_shore. The lake (water at y = 0, about 118 m to its bank) at the origin; the cameras stand on the south
// shore (+Z) and look north across it to the forest on the far shore - densest at its edge, thinning out over a
// kilometre of rising ground - and past it to a bare ridge 2.6 km away whose crest (1.1 to 1.4 km) stands in the cloud
// layer (0.9 to 2.4 km). A jetty with a lantern runs out from the south shore.
float shoreGround(float x, float z)
{
    const float r = std::sqrt(x * x + z * z);
    const float bank = lerpf(-5.0f, 0.0f, smoothstepf(30.0f, 118.0f, r)) + 2.0f * smoothstepf(118.0f, 200.0f, r);
    const float away = smoothstepf(150.0f, 400.0f, r);  // (the lake and its banks keep their shape)
    const float rolling = 2.0f * std::sin(x / 170.0f) * std::cos(z / 210.0f) + 1.2f * std::sin(x / 63.0f + z / 71.0f);
    const float dz = (z + 2600.0f) / 900.0f;
    const float crest = 0.85f + 0.15f * std::sin(x / 310.0f) + 0.05f * std::sin(x / 97.0f + 1.3f);
    return bank + away * (rolling + 0.03f * std::max(-z - 300.0f, 0.0f) + 1250.0f * crest * std::exp(-dz * dz));
}

Scene shore(const Request& rq, SceneExtras* extras)
{
    Scene s;
    s.name = "showcase_shore";
    s.sun.direction = sunDirection(10.0f, 140.0f);  // low, behind and left of the cameras: the far shore and the ridge are lit
    s.windDirection = normalize(f3(0.8f, 0, 0.6f));
    s.windSpeed = 2.5f;
    const Palette p = buildPalette(s, rq.seed, false);
    const uint32_t cast = scene::InstanceCastShadow;
    {
        MeshBuilder b("shore_terrain");
        b.material(p.grass);
        b.heightfield(-2000, -3600, 2000, 400, 800, shoreGround, 1.0f / 8.0f);  // 5 m grid
        addInstance(s, addMesh(s, b.finish(false)), float3x4{});
    }
    {
        Material water;
        water.name = "shore_water";
        water.cls = scene::MaterialClass::Water;
        water.baseColor = f3(0.45f, 0.62f, 0.60f);
        water.roughness = 0.03f;
        water.specular = 0.25f;
        water.ior = 1.33f;
        MeshBuilder b("shore_water");
        b.material(addMaterial(s, water));
        for (int j = 0; j < 9; ++j)
            for (int i = 0; i < 9; ++i) b.quadXZ(-135.0f + 30.0f * i, -135.0f + 30.0f * j, -105.0f + 30.0f * i, -105.0f + 30.0f * j, 0.0f, 1.0f / 30.0f);
        addInstance(s, addMesh(s, b.finish(false)), float3x4{}, 0);
    }
    const float scale = std::max(rq.scale, 0.02f);
    // ---- the forest: card trees from the far shore up the slope (a tree to some 20 m2 at the edge, thinning out to
    // nothing 1.1 km behind it), grass clumps in a ring around the lake - small instances by the tens of thousands,
    // most of them far from any camera (the ray scene's far field)
    {
        const FoliageAssets f = buildFoliage(s, p, rq.seed ^ 0x5403ull, FoliageStyle{ false });
        Rng rt(rq.seed, 70), rg(rq.seed, 71);
        const int trees = (int)std::lround(40000 * scale), clumps = (int)std::lround(150000 * scale);
        for (int i = 0, placed = 0; placed < trees && i < trees * 8; ++i)
        {
            const float depth = rt.uniform();
            const float x = rt.range(-1000, 1000), z = -122.0f - 1100.0f * depth * depth;
            if (x * x + z * z < 126.0f * 126.0f) continue;  // the lake
            Instance& in = addInstance(s, f.treeMeshes[placed & 3], placement({ x, shoreGround(x, z) - 0.2f, z }, rt.range(0, 2 * kPi), rt.range(0.8f, 1.3f)),
                                       cast | scene::InstanceWind);
            in.wind = { 20.0f, rt.range(0, 2 * kPi), 1.0f };
            ++placed;
        }
        for (int i = 0; i < clumps; ++i)
        {
            const float a = rg.range(0, 2 * kPi), r = rg.range(121.0f, 600.0f);
            const float x = r * std::cos(a), z = r * std::sin(a);
            if (std::fabs(x) < 2.0f && z > 100.0f) continue;  // the jetty's foot
            Instance& in = addInstance(s, f.grassMeshes[i & 7], placement({ x, shoreGround(x, z), z }, rg.range(0, 2 * kPi), rg.range(0.7f, 1.2f)), cast | scene::InstanceWind);
            in.wind = { 5.0f, rg.range(0, 2 * kPi), 0.0f };
        }
    }
    // ---- reeds along the waterline (3 mm stalks)
    {
        Material reed;
        reed.name = "shore_reed";
        reed.cls = scene::MaterialClass::Foliage;
        reed.baseColor = f3(0.12f, 0.13f, 0.05f);
        reed.roughness = 0.5f;
        reed.transmission = 0.2f;
        reed.twoSided = true;
        MeshBuilder b("shore_reeds");
        b.material(addMaterial(s, reed));
        Rng r(rq.seed, 72);
        for (int k = 0; k < 30; ++k)
        {
            const float x = r.range(-0.6f, 0.6f), z = r.range(-0.6f, 0.6f), h = r.range(1.0f, 1.8f);
            b.cylinder({ x, 0, z }, { r.range(-0.1f, 0.1f), h, r.range(-0.1f, 0.1f) }, 0.003f, 0.0015f, 5, 3, false, 1.0f);
        }
        const uint32_t mesh = addMesh(s, b.finish(false));
        Rng rr(rq.seed, 73);
        for (int i = 0; i < (int)std::lround(600 * scale); ++i)
        {
            const float a = rr.range(0, 2 * kPi), rad = rr.range(110.0f, 119.0f);
            const float x = rad * std::cos(a), z = rad * std::sin(a);
            if (std::fabs(x) < 4.0f && z > 0) continue;  // the jetty
            Instance& in = addInstance(s, mesh, placement({ x, -0.3f, z }, rr.range(0, 2 * kPi)), cast | scene::InstanceWind);
            in.wind = { 3.0f, rr.range(0, 2 * kPi), 0.3f };
        }
    }
    // ---- the jetty: wet planks (the water film: a 1.33 coat), posts, a lantern at its end - a metal cage and four
    // glass panes in one mesh around a small emissive mantle, and a shadow-casting light with a photometric profile
    // (the extra) in the lake's mist
    {
        Material planks;
        planks.name = "shore_planks_wet";
        planks.baseColor = f3(0.16f, 0.11f, 0.07f);
        planks.roughness = 0.6f;
        planks.clearcoat = 0.8f;
        planks.clearcoatRoughness = 0.08f;
        planks.clearcoatIor = 1.33f;
        const uint32_t plankMat = addMaterial(s, planks);
        {
            MeshBuilder deck("shore_jetty_planks");
            deck.material(plankMat);
            for (int k = 0; k < 140; ++k) deck.box({ -1.1f, 0.30f, 92.0f + 0.2f * k }, { 1.1f, 0.38f, 92.18f + 0.2f * k });
            addInstance(s, addMesh(s, deck.finish(false)), float3x4{});
            MeshBuilder posts("shore_jetty_posts");
            posts.material(p.bark);
            for (float z : { 93.0f, 101.0f, 109.0f, 117.0f })
                for (float x : { -1.05f, 0.95f }) posts.box({ x, -3.0f, z }, { x + 0.1f, 0.3f, z + 0.1f });
            addInstance(s, addMesh(s, posts.finish(true)), float3x4{});
        }
        const float3 lamp = f3(0.9f, 2.6f, 93.0f);
        solid(s, "shore_lamp_pole", p.metal, { lamp.x - 0.03f, 0.38f, lamp.z - 0.03f }, { lamp.x + 0.03f, lamp.y - 0.16f, lamp.z + 0.03f });
        MeshBuilder b("shore_lantern");
        b.material(p.metal);
        b.box({ -0.14f, -0.16f, -0.14f }, { 0.14f, -0.14f, 0.14f });
        b.box({ -0.16f, 0.14f, -0.16f }, { 0.16f, 0.18f, 0.16f });
        for (float x : { -0.14f, 0.12f })
            for (float z : { -0.14f, 0.12f }) b.box({ x, -0.14f, z }, { x + 0.02f, 0.14f, z + 0.02f });
        b.material(addMaterial(s, glassPane("shore_lantern_glass", f3(0.95f, 0.9f, 0.78f))));
        b.quad4({ -0.12f, -0.14f, 0.13f }, { 0.12f, -0.14f, 0.13f }, { 0.12f, 0.14f, 0.13f }, { -0.12f, 0.14f, 0.13f }, { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 });
        b.quad4({ 0.12f, -0.14f, -0.13f }, { -0.12f, -0.14f, -0.13f }, { -0.12f, 0.14f, -0.13f }, { 0.12f, 0.14f, -0.13f }, { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 });
        b.quad4({ 0.13f, -0.14f, 0.12f }, { 0.13f, -0.14f, -0.12f }, { 0.13f, 0.14f, -0.12f }, { 0.13f, 0.14f, 0.12f }, { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 });
        b.quad4({ -0.13f, -0.14f, -0.12f }, { -0.13f, -0.14f, 0.12f }, { -0.13f, 0.14f, 0.12f }, { -0.13f, 0.14f, -0.12f }, { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 });
        addInstance(s, addMesh(s, b.finish(true)), float3x4::translation(lamp));
        Material mantle;
        mantle.name = "shore_lantern_mantle";
        mantle.baseColor = f3(0.9f, 0.9f, 0.9f);
        mantle.emissive = f3(30000.0f, 21000.0f, 9000.0f);
        mantle.emissiveVisibleOnly = true;  // (its light is the analytic light below)
        MeshBuilder m("shore_lantern_mantle");
        m.material(addMaterial(s, mantle));
        m.sphere({ 0, 0, 0 }, 0.03f, 16, 8);
        addInstance(s, addMesh(s, m.finish(false)), float3x4::translation(lamp), 0);
        scene::Light l;
        l.type = scene::LightType::Point;
        l.position = lamp;
        l.forward = f3(0, -1, 0);
        l.right = f3(1, 0, 0);
        l.color = luminanceNormalised(f3(1.0f, 0.74f, 0.42f));
        l.intensity = 3000.0f;  // 600 lux on the planks under it: a warm pool under the low sun's light
        l.range = 40.0f;
        l.castShadow = true;
        l.rayEndBias = 0.05f;
        if (extras)
        {
            ExtraLightFunction ies;
            ies.light = (uint32_t)s.lights.size();
            ies.profile = 1;
            for (int deg = 0; deg <= 180; deg += 10)
            {
                // a lantern: most of its light sideways and down, little straight down (the base), little up (the cap)
                const float x = (float)deg;
                ies.iesVertical.push_back(x);
                ies.iesValues.push_back(std::max(0.08f, std::exp(-(x - 70.0f) * (x - 70.0f) / (2.0f * 30.0f * 30.0f))));
            }
            ies.flickerDepth = 0.12f;
            ies.flickerFrequency = 6.0f;
            extras->lightFunctions.push_back(std::move(ies));
        }
        s.lights.push_back(l);
    }
    // ---- shore rocks, wet near the water
    {
        MeshBuilder b("shore_rock");
        b.material(p.rock);
        b.sphere({ 0, 0, 0 }, 1.0f, 24, 16, 0.5f);
        Mesh m = b.finish(false);
        for (size_t k = 0; k < m.positions.size(); ++k)
        {
            const float3 q = m.positions[k];
            const float d = 0.75f + 0.5f * fbm2(q.x * 1.3f + 3.1f, q.z * 1.3f + q.y * 0.7f, (uint32_t)rq.seed + 95, 4);
            m.positions[k] = f3(q.x * d * 1.3f, q.y * d * 0.8f, q.z * d);
        }
        std::vector<float3> acc(m.positions.size());
        for (size_t t = 0; t < m.indices.size(); t += 3)
        {
            const uint32_t a = m.indices[t], bb = m.indices[t + 1], c = m.indices[t + 2];
            const float3 n = cross(m.positions[bb] - m.positions[a], m.positions[c] - m.positions[a]);
            acc[a] = acc[a] + n;
            acc[bb] = acc[bb] + n;
            acc[c] = acc[c] + n;
        }
        for (size_t k = 0; k < acc.size(); ++k) m.normals[k] = length(acc[k]) > 0 ? normalize(acc[k]) : f3(0, 1, 0);
        computeTangents(m);
        const uint32_t rock = addMesh(s, std::move(m));
        Rng r(rq.seed, 74);
        for (int i = 0; i < 40; ++i)
        {
            const float a = r.range(0, 2 * kPi), rad = r.range(114.0f, 126.0f);
            const float x = rad * std::cos(a), z = rad * std::sin(a);
            if (std::fabs(x) < 5.0f && z > 0) continue;
            const float y = shoreGround(x, z);
            Instance& in = addInstance(s, rock, placement({ x, y - 0.2f, z }, r.range(0, 2 * kPi), r.range(0.4f, 1.4f)));
            if (y < 0.3f) in.materialOverrides = { p.rockWet };
        }
    }

    // ---- weather: the cloud layer the ridge reaches into, cirrus above, mist over the lake and a bank of it in the reeds
    s.clouds.coverage = 0.5f;
    s.clouds.baseAltitude = 900.0f;
    s.clouds.topAltitude = 2400.0f;
    s.clouds.windX = 6.0f;
    s.clouds.cirrusCoverage = 0.5f;
    s.fog.enabled = true;
    s.fog.density = 0.0015f;  // 70 % of the far shore's light reaches the near one (250 m at the water's level)
    s.fog.heightFalloff = 0.08f;
    s.fog.height = 0.0f;
    s.fog.noiseScale = 30.0f;
    {
        scene::FogVolume mist;
        mist.centre = { 60.0f, 1.5f, 100.0f };
        mist.halfSize = { 40.0f, 2.5f, 20.0f };
        mist.density = 0.05f;
        mist.heightFalloff = 1.5f;
        s.fogVolumes.push_back(mist);
    }
    if (extras)
    {
        // the "rain" camera's weather: steady rain on a soaked shore
        ExtraWeather rain;
        rain.camera = "rain";
        rain.rainRate = 8.0f;
        rain.wetness = 0.9f;
        extras->weather.push_back(rain);
    }

    const float ev = 12.5f;
    const float eye = shoreGround(-12.0f, 131.0f) + 1.7f;
    // from the south shore: the jetty and the lake, the forest's edge beyond it, the ridge into the cloud, the cirrus
    s.cameras.push_back(camera("shore", { -12.0f, eye, 131.0f }, { 30.0f, 60.0f, -900.0f }, ev));
    // on the jetty, towards the lantern at its end: wet planks, the lantern's panes and its light in the mist (the
    // camera the rain weather belongs to)
    s.cameras.push_back(camera("rain", { -0.6f, 1.98f, 104.0f }, { 0.7f, 1.9f, 93.0f }, ev - 1.0f, 50.0f));
    // over the water near the far shore, along the tree line as it recedes: where instances hand over to the far field
    s.cameras.push_back(camera("forest_edge", { -40.0f, 6.0f, -100.0f }, { 400.0f, 30.0f, -700.0f }, ev));
    // the ridge's crest in the cloud layer (a long lens from over the lake)
    s.cameras.push_back(camera("ridge", { 0.0f, 30.0f, 100.0f }, { 0.0f, 1100.0f, -2600.0f }, ev + 0.5f, 25.0f));
    // low in the reeds on the south shore, towards the bank of mist
    s.cameras.push_back(camera("reeds", { 20.0f, 0.9f, 126.0f }, { 60.0f, 0.8f, 110.0f }, ev, 45.0f));
    for (const auto& c : s.cameras) s.paths.push_back(staticPath(c));
    return s;
}
} // namespace

namespace detail
{
Scene showcase(const Request& rq, std::vector<Groom>* grooms, SceneExtras* extras)
{
    switch (rq.id)
    {
    case SceneId::ShowcaseBathhouse: return bathhouse(rq, grooms, extras);
    case SceneId::ShowcaseAtrium: return atrium(rq, extras);
    case SceneId::ShowcaseShore: return shore(rq, extras);
    default: return Scene{};
    }
}
float showcaseShoreGround(float x, float z) { return shoreGround(x, z); }
} // namespace detail

SceneExtras extras(const Request& rq)
{
    SceneExtras e;
    if (rq.id == SceneId::ShowcaseBathhouse || rq.id == SceneId::ShowcaseAtrium || rq.id == SceneId::ShowcaseShore) (void)showcase(rq, nullptr, &e);
    return e;
}

std::vector<SceneId> showcaseScenes() { return { SceneId::ShowcaseBathhouse, SceneId::ShowcaseAtrium, SceneId::ShowcaseShore }; }
} // namespace unx::scenegen
