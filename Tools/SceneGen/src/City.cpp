// City layout shared by CityBlock (day) and CityNight: a square grid of blocks with streets, raised sidewalks,
// buildings with window panes, parapets and rooftop units, street lights, street trees (alpha cards) and overhead
// wires (6 mm radius: sub-pixel beyond ~20 m at 4K, the coverage-layer case in a city view).
#include "Common.h"

#include <algorithm>
#include <cmath>

namespace unx::scenegen::detail
{
namespace
{
constexpr float kBlock = 60.0f, kSidewalk = 3.0f, kRoad = 10.0f, kPitch = kBlock + 2 * kSidewalk + kRoad, kCurb = 0.15f;

struct Facade
{
    float3 origin;  // bottom-left corner seen from outside
    float3 along;   // unit, left to right seen from outside
    float3 normal;  // outward
    float width, height;
};

void windowGrid(MeshBuilder& glass, MeshBuilder& lit, const Facade& f, Rng& r, bool night, CityLayout& layout)
{
    const float floorH = 3.5f, colW = 3.0f, winW = 1.6f, winH = 1.8f, proud = 0.03f;
    const int floors = (int)std::floor((f.height - 1.5f) / floorH), cols = (int)std::floor((f.width - 1.0f) / colW);
    if (floors <= 0 || cols <= 0) return;
    const float x0 = 0.5f * (f.width - cols * colW);
    const float3 up = f3(0, 1, 0);
    for (int fl = 0; fl < floors; ++fl)
        for (int c = 0; c < cols; ++c)
        {
            const float cx = x0 + (c + 0.5f) * colW, cy = 1.2f + fl * floorH + 0.5f * winH + 0.4f;
            const float3 centre = f.origin + f.along * cx + up * cy + f.normal * proud;
            const float3 hx = f.along * (0.5f * winW), hy = up * (0.5f * winH);
            const bool isLit = night && r.uniform() < 0.35f;
            MeshBuilder& b = isLit ? lit : glass;
            b.quad4(centre - hx - hy, centre + hx - hy, centre + hx + hy, centre - hx + hy, { 0, 1 }, { 1, 1 }, { 1, 0 }, { 0, 0 });
            if (fl == 0)
            {
                layout.windowCentres.push_back(centre);
                layout.windowNormals.push_back(f.normal);
            }
        }
}
} // namespace

CityLayout buildCity(Scene& s, const Palette& p, uint64_t seed, float scale, bool night)
{
    CityLayout layout;
    const int blocks = std::max(1, (int)std::lround(5.0f * std::sqrt(std::max(scale, 0.04f))));
    const float half = blocks * kPitch * 0.5f;  // street centre lines at -half + k * pitch
    layout.extent = half + kRoad * 0.5f;
    Rng r(seed, 10);

    // Terrain: 2 x 2 km, 2 m grid, with the city square cut out (the street plane covers it).
    {
        MeshBuilder b("terrain");
        b.material(p.grass);
        const int n = 1000;
        const float x0 = -1000, z0 = -1000, d = 2000.0f / n;
        const uint32_t first = (uint32_t)b.mesh.positions.size();
        for (int j = 0; j <= n; ++j)
            for (int i = 0; i <= n; ++i)
            {
                const float x = x0 + i * d, z = z0 + j * d, e = 0.5f;
                const float hx = (cityTerrain(x + e, z) - cityTerrain(x - e, z)) / (2 * e), hz = (cityTerrain(x, z + e) - cityTerrain(x, z - e)) / (2 * e);
                b.vertex({ x, cityTerrain(x, z), z }, { -hx, 1, -hz }, { x / 8, z / 8 });
            }
        const float cut = layout.extent;
        for (int j = 0; j < n; ++j)
            for (int i = 0; i < n; ++i)
            {
                const float qx0 = x0 + i * d, qz0 = z0 + j * d;
                if (qx0 >= -cut && qx0 + d <= cut && qz0 >= -cut && qz0 + d <= cut) continue;
                const uint32_t a = first + j * (n + 1) + i, bb = a + 1, dd = a + n + 1, c = dd + 1;
                b.quad(a, dd, c, bb);
            }
        addMesh(s, b.finish(false));
        addInstance(s, (uint32_t)s.meshes.size() - 1, float3x4{});
    }
    // Street plane (asphalt) over the city square, in 32 m tiles so UVs stay small.
    {
        MeshBuilder b("streets");
        b.material(night ? p.asphaltWet : p.asphalt);
        const int tiles = (int)std::ceil(2 * layout.extent / 32.0f);
        const float t = 2 * layout.extent / tiles;
        for (int j = 0; j < tiles; ++j)
            for (int i = 0; i < tiles; ++i)
            {
                const float xa = -layout.extent + i * t, za = -layout.extent + j * t;
                b.quadXZ(xa, za, xa + t, za + t, 0.0f, 0.25f);
            }
        addMesh(s, b.finish(true));
        addInstance(s, (uint32_t)s.meshes.size() - 1, float3x4{});
    }

    FoliageAssets trees = buildFoliage(s, p, seed ^ 0xC17Eull, FoliageStyle{ false });

    // Street lamp (instanced): pole 8 m, arm 1.6 m towards +X, head, lens facing down.
    uint32_t lampMesh;
    {
        Material lens;
        lens.name = "lamp_lens";
        lens.baseColor = f3(0.8f, 0.8f, 0.8f);
        lens.roughness = 0.3f;
        if (night) lens.emissive = f3(500, 400, 300);  // visual only; the spot light carries the illumination
        const uint32_t lensMat = addMaterial(s, lens);
        MeshBuilder b("street_lamp");
        b.material(p.metal);
        b.cylinder({ 0, 0, 0 }, { 0, 8, 0 }, 0.09f, 0.06f, 12, 4, true, 1.0f);
        b.cylinder({ 0, 7.8f, 0 }, { 1.6f, 0.25f, 0 }, 0.04f, 0.035f, 8, 2, false, 1.0f);
        b.box({ 1.35f, 7.95f, -0.18f }, { 1.95f, 8.12f, 0.18f }, 1.0f);
        b.material(lensMat);
        b.quad4({ 1.4f, 7.94f, -0.14f }, { 1.9f, 7.94f, -0.14f }, { 1.9f, 7.94f, 0.14f }, { 1.4f, 7.94f, 0.14f }, { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 });
        lampMesh = addMesh(s, b.finish(true));
    }

    MeshBuilder wires("overhead_wires");
    wires.material(p.metal);

    for (int bz = 0; bz < blocks; ++bz)
        for (int bx = 0; bx < blocks; ++bx)
        {
            const float cx = -half + (bx + 0.5f) * kPitch, cz = -half + (bz + 0.5f) * kPitch;
            const float lotHalf = 0.5f * kBlock, walkHalf = lotHalf + kSidewalk;
            // Sidewalk slab and curb.
            {
                MeshBuilder b("sidewalk");
                b.material(p.concrete);
                b.box({ cx - walkHalf, 0, cz - walkHalf }, { cx + walkHalf, kCurb, cz + walkHalf }, 0.5f);
                addInstance(s, addMesh(s, b.finish(true)), float3x4{});
            }
            // Lots: split the block into 1-4 buildings.
            const bool splitX = r.uniform() < 0.7f, splitZ = r.uniform() < 0.7f;
            const float sx = splitX ? r.range(0.35f, 0.65f) : 1.0f, sz = splitZ ? r.range(0.35f, 0.65f) : 1.0f;
            const float xs[3] = { cx - lotHalf, cx - lotHalf + sx * kBlock, cx + lotHalf };
            const float zs[3] = { cz - lotHalf, cz - lotHalf + sz * kBlock, cz + lotHalf };
            const float centreDist = std::sqrt(cx * cx + cz * cz) / half;
            for (int iz = 0; iz < (splitZ ? 2 : 1); ++iz)
                for (int ix = 0; ix < (splitX ? 2 : 1); ++ix)
                {
                    const float x0 = xs[ix] + 1.5f, x1 = (splitX ? xs[ix + 1] : xs[2]) - 1.5f;
                    const float z0 = zs[iz] + 1.5f, z1 = (splitZ ? zs[iz + 1] : zs[2]) - 1.5f;
                    const float height = std::round(lerpf(90.0f, 14.0f, clampf(centreDist, 0, 1)) * r.range(0.45f, 1.0f) / 3.5f) * 3.5f + 1.5f;
                    const uint32_t facadeMats[3] = { p.brick, p.concrete, p.plaster };
                    const uint32_t facade = facadeMats[r.below(3)];
                    MeshBuilder b("building");
                    b.material(facade);
                    b.box({ x0, kCurb, z0 }, { x1, height, z1 }, 1.0f / 0.9f);
                    b.material(p.roofing);
                    const float pt = 0.25f, ph = 1.0f;
                    b.box({ x0, height, z0 }, { x1, height + ph, z0 + pt });
                    b.box({ x0, height, z1 - pt }, { x1, height + ph, z1 });
                    b.box({ x0, height, z0 + pt }, { x0 + pt, height + ph, z1 - pt });
                    b.box({ x1 - pt, height, z0 + pt }, { x1, height + ph, z1 - pt });
                    const int units = 1 + (int)r.below(4);
                    for (int u = 0; u < units; ++u)
                    {
                        const float ux = r.range(x0 + 2, x1 - 4), uz = r.range(z0 + 2, z1 - 4);
                        b.box({ ux, height, uz }, { ux + r.range(1.5f, 3.0f), height + r.range(1.0f, 2.2f), uz + r.range(1.5f, 3.0f) });
                    }
                    MeshBuilder glass("windows"), lit("windows_lit");
                    glass.material(p.windowGlass);
                    lit.material(p.windowLit);
                    const float h = height - kCurb;
                    const Facade faces[4] = {
                        { { x0, kCurb, z1 }, { 1, 0, 0 }, { 0, 0, 1 }, x1 - x0, h },
                        { { x1, kCurb, z0 }, { -1, 0, 0 }, { 0, 0, -1 }, x1 - x0, h },
                        { { x1, kCurb, z1 }, { 0, 0, -1 }, { 1, 0, 0 }, z1 - z0, h },
                        { { x0, kCurb, z0 }, { 0, 0, 1 }, { -1, 0, 0 }, z1 - z0, h },
                    };
                    for (const Facade& f : faces) windowGrid(glass, lit, f, r, night, layout);
                    // Buildings are single meshes (unique); windows are separate submeshes of the same mesh.
                    Mesh bm = b.finish(true);
                    for (MeshBuilder* w : { &glass, &lit })
                    {
                        Mesh wm = w->finish(false);
                        if (wm.indices.empty()) continue;
                        const uint32_t base = (uint32_t)bm.positions.size(), ioff = (uint32_t)bm.indices.size();
                        bm.positions.insert(bm.positions.end(), wm.positions.begin(), wm.positions.end());
                        bm.normals.insert(bm.normals.end(), wm.normals.begin(), wm.normals.end());
                        bm.uv0.insert(bm.uv0.end(), wm.uv0.begin(), wm.uv0.end());
                        for (size_t k = 0; k < wm.positions.size(); ++k)
                        {
                            const float3 t = normalize(cross(f3(0, 1, 0), wm.normals[k]));
                            bm.tangents.push_back({ t.x, t.y, t.z, 1 });
                        }
                        for (uint32_t idx : wm.indices) bm.indices.push_back(idx + base);
                        for (scene::Submesh sm : wm.submeshes)
                        {
                            sm.indexOffset += ioff;
                            bm.submeshes.push_back(sm);
                        }
                    }
                    addInstance(s, addMesh(s, std::move(bm)), float3x4{});
                }
        }

    // Street lamps along both sidewalks of every street segment, every 30 m, staggered; trees half-way between.
    std::vector<std::pair<float3, float>> lampCandidates;  // base, yaw (arm points to the road centre)
    for (int k = 0; k <= blocks; ++k)
    {
        const float c = -half + k * kPitch;
        for (int side = -1; side <= 1; side += 2)
        {
            const float off = c + side * (kRoad * 0.5f + 0.6f);
            for (float t = -half + 12.0f; t <= half - 12.0f; t += 30.0f)
            {
                const float along = t + (side > 0 ? 15.0f : 0.0f);
                if (along > half - 10.0f) continue;
                // Skip intersections (within 8 m of a crossing street centre line).
                const float m = std::fmod(along + half + 1000 * kPitch, kPitch);
                if (m < kRoad * 0.5f + 3.0f || m > kPitch - kRoad * 0.5f - 3.0f) continue;
                // Streets along Z (x = off) and along X (z = off).
                lampCandidates.push_back({ f3(off, kCurb, along), side > 0 ? kPi : 0.0f });
                lampCandidates.push_back({ f3(along, kCurb, off), side > 0 ? kPi * 0.5f : -kPi * 0.5f });
            }
        }
    }
    // Exactly 128 lamps at gate scale (CityNight: 128 shadowed spots); a deterministic even subset of the candidates.
    const size_t lampCount = std::min(lampCandidates.size(), (size_t)std::lround(128 * std::max(scale, 0.04f)));
    std::vector<size_t> chosen;
    for (size_t i = 0; i < lampCount; ++i) chosen.push_back(i * lampCandidates.size() / lampCount);
    std::vector<float3> heads;
    for (size_t idx : chosen)
    {
        const auto& [base, yaw] = lampCandidates[idx];
        // Arm along local +X; yaw 0 means the arm points +X. Road centre is at -side direction.
        const float3x4 t = placement(base, yaw);
        addInstance(s, lampMesh, t);
        const float3 head = t.transformPoint(f3(1.65f, 7.92f, 0));
        layout.lampBases.push_back(base);
        layout.lampHeads.push_back(head);
        heads.push_back(head);
    }
    // Trees on sidewalks, offset from lamps.
    for (size_t i = 0; i < lampCandidates.size(); i += 2)
    {
        const float3 base = lampCandidates[i].first;
        const float3 pos = base + f3(0, 0, 7.5f);
        const float m = std::fmod(pos.z + half + 1000 * kPitch, kPitch);
        if (m < kRoad * 0.5f + 6.0f || m > kPitch - kRoad * 0.5f - 6.0f) continue;
        scene::Instance& in = addInstance(s, trees.treeMeshes[i / 2 % trees.treeMeshes.size()], placement(pos, r.range(0, 2 * kPi), r.range(0.6f, 0.8f)),
                                          scene::InstanceCastShadow | scene::InstanceWind);
        in.wind = { 20.0f, r.range(0, 2 * kPi), 1.0f };
    }
    // Overhead wires: two per street segment between consecutive lamps on the same street line, with sag.
    for (size_t i = 0; i + 1 < heads.size(); ++i)
    {
        const float3 a = layout.lampBases[i], b = layout.lampBases[i + 1];
        if (std::fabs(a.x - b.x) > 0.01f && std::fabs(a.z - b.z) > 0.01f) continue;  // not on the same line
        if (length(b - a) > 70.0f) continue;
        for (float hgt : { 7.3f, 7.6f })
        {
            const int segs = 8;
            for (int k = 0; k < segs; ++k)
            {
                auto point = [&](float t) {
                    const float3 q = a + (b - a) * t;
                    return f3(q.x, hgt - 0.6f * 4 * t * (1 - t), q.z);
                };
                const float3 p0 = point((float)k / segs), p1 = point((float)(k + 1) / segs);
                wires.cylinder(p0, p1 - p0, 0.006f, 0.006f, 6, 1, false, 1.0f);
            }
        }
    }
    if (!wires.mesh.indices.empty()) addInstance(s, addMesh(s, wires.finish(false)), float3x4{});

    // Facade anchors for night lighting: street-level window centres already collected (first floor).
    return layout;
}
} // namespace unx::scenegen::detail
