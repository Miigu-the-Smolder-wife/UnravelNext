#include "World.h"

#include "Environment.h"
#include "Rng.h"

#include "unx/core/Log.h"
#include "unx/core/Sha256.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <sstream>

namespace unx::rpp
{
const char* sectionId(Section s)
{
    constexpr const char* ids[] = { "city", "forest", "waterside", "interior" };
    return ids[s];
}

namespace
{
using scene::Instance;
using scene::Material;
using scene::Mesh;
using scene::Scene;

// ---- content-addressed merge of textures, materials and meshes (identical palette/foliage content of C's scenes is
// stored once) ----
struct Hasher
{
    Sha256 h;
    template <class T>
    void pod(const T& v) { h.update(&v, sizeof v); }
    void bytes(const void* p, size_t n) { h.update(p, n); }
    void str(const std::string& s) { uint64_t n = s.size(); pod(n); h.update(s.data(), s.size()); }
    template <class T>
    void vec(const std::vector<T>& v) { uint64_t n = v.size(); pod(n); if (n) h.update(v.data(), n * sizeof(T)); }
    std::string hex()
    {
        const auto d = h.finish();
        static const char* x = "0123456789abcdef";
        std::string s;
        for (uint8_t b : d) { s += x[b >> 4]; s += x[b & 15]; }
        return s;
    }
};

struct Merger
{
    Scene& w;
    std::map<std::string, uint32_t> textures, materials, meshes;
    explicit Merger(Scene& world) : w(world) {}

    uint32_t texture(const scene::Texture& t)
    {
        Hasher h;
        h.str(t.name); h.pod(t.width); h.pod(t.height); h.pod(t.format); h.pod(t.wrap); h.vec(t.texels);
        const std::string k = h.hex();
        auto it = textures.find(k);
        if (it != textures.end()) return it->second;
        w.textures.push_back(t);
        return textures[k] = (uint32_t)w.textures.size() - 1;
    }
    uint32_t material(Material m, const std::vector<uint32_t>& texRemap)
    {
        for (uint32_t* t : { &m.baseColorTexture, &m.normalTexture, &m.roughMetalTexture, &m.emissiveTexture, &m.occlusionTexture })
            if (*t != scene::kNone) *t = texRemap.at(*t);
        Hasher h;
        h.str(m.name); h.pod(m.cls); h.pod(m.baseColor); h.pod(m.roughness); h.pod(m.metallic); h.pod(m.specular); h.pod(m.emissive);
        h.pod(m.alphaCutoff); h.pod(m.transmission); h.pod(m.ior); h.pod(m.twoSided);
        h.pod(m.baseColorTexture); h.pod(m.normalTexture); h.pod(m.roughMetalTexture); h.pod(m.emissiveTexture); h.pod(m.occlusionTexture);
        const std::string k = h.hex();
        auto it = materials.find(k);
        if (it != materials.end()) return it->second;
        w.materials.push_back(m);
        return materials[k] = (uint32_t)w.materials.size() - 1;
    }
    uint32_t mesh(Mesh m, const std::vector<uint32_t>& matRemap)
    {
        for (scene::Submesh& s : m.submeshes) s.material = matRemap.at(s.material);
        Hasher h;
        h.str(m.name); h.vec(m.positions); h.vec(m.normals); h.vec(m.tangents); h.vec(m.uv0); h.vec(m.indices); h.vec(m.submeshes);
        h.vec(m.skin.joints); h.vec(m.skin.weights); h.vec(m.skin.inverseBind);
        const std::string k = h.hex();
        auto it = meshes.find(k);
        if (it != meshes.end()) return it->second;
        w.meshes.push_back(std::move(m));
        return meshes[k] = (uint32_t)w.meshes.size() - 1;
    }
};

// Per source scene: texture, material and mesh index maps into the world (meshes merged lazily: dropped meshes such as
// the 1M-vertex terrains are never hashed or copied).
struct Source
{
    const Scene& s;
    Merger& m;
    std::vector<uint32_t> tex, mat;
    std::vector<uint32_t> mesh;
    Source(const Scene& scene, Merger& merger) : s(scene), m(merger), mesh(scene.meshes.size(), scene::kNone)
    {
        for (const auto& t : s.textures) tex.push_back(m.texture(t));
        for (const auto& mt : s.materials) mat.push_back(m.material(mt, tex));
    }
    uint32_t meshIndex(uint32_t local)
    {
        if (mesh[local] == scene::kNone) mesh[local] = m.mesh(s.meshes[local], mat);
        return mesh[local];
    }
};

float3x4 compose(const float3x4& a, const float3x4& b)  // a * b (3x4 affine)
{
    float3x4 r;
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            float v = j == 3 ? a.m[i][3] : 0.0f;
            for (int k = 0; k < 3; ++k) v += a.m[i][k] * b.m[k][j];
            r.m[i][j] = v;
        }
    }
    return r;
}
float3 origin(const float3x4& t) { return { t.m[0][3], t.m[1][3], t.m[2][3] }; }
float4 quatMul(float4 a, float4 b)
{
    return { a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
             a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z };
}
bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }

// A box with flat normals, tangents (u axis) and planar UVs (0.5 per metre, as C's solid()).
Mesh boxMesh(const std::string& name, float3 lo, float3 hi, uint32_t material)
{
    Mesh m;
    m.name = name;
    const float3 c = (lo + hi) * 0.5f, e = (hi - lo) * 0.5f;
    const float3 axes[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    for (const float3& a : axes)
    {
        const float3 u = a.x != 0 ? float3{ 0, 0, a.x } : float3{ 1, 0, 0 };
        const float3 wv = cross(a, u);
        const uint32_t base = (uint32_t)m.positions.size();
        for (int k = 0; k < 4; ++k)
        {
            const float su = (k == 1 || k == 2) ? 1.0f : -1.0f, sw = k >= 2 ? 1.0f : -1.0f;
            const float3 p = c + float3{ (a.x + u.x * su + wv.x * sw) * e.x, (a.y + u.y * su + wv.y * sw) * e.y, (a.z + u.z * su + wv.z * sw) * e.z };
            m.positions.push_back(p);
            m.normals.push_back(a);
            m.tangents.push_back({ u.x, u.y, u.z, 1.0f });
            m.uv0.push_back({ dot(p, u) * 0.5f, dot(p, wv) * 0.5f });
        }
        m.indices.insert(m.indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
    }
    m.submeshes.push_back({ 0, (uint32_t)m.indices.size(), material });
    return m;
}

scene::Light pointLight(scene::LightType type, float3 p, float3 colour, float intensity, float range, bool shadow)
{
    scene::Light l;
    l.type = type;
    l.position = p;
    l.color = colour / (0.2126f * colour.x + 0.7152f * colour.y + 0.0722f * colour.z);
    l.intensity = intensity;
    l.range = range;
    l.castShadow = shadow;
    return l;
}
} // namespace

World buildWorld(const Layout& layoutIn, uint64_t seed)
{
    World world;
    Scene& w = world.scene;
    w.name = "rpp1_world";
    w.seed = seed;
    Layout layout = layoutIn;

    // ---- C's gate scenes (seed 1, scale 1) and their dynamic content ----
    const uint64_t cSeed = 1;
    scenegen::DynamicContent dc[SectionCount];
    const scenegen::SceneId ids[SectionCount] = { scenegen::SceneId::CityNight, scenegen::SceneId::ForestCombat, scenegen::SceneId::Waterside, scenegen::SceneId::Interior };
    Scene src[SectionCount];
    for (uint32_t s = 0; s < SectionCount; ++s)
    {
        src[s] = scenegen::generateWithContent({ ids[s], cSeed, 1.0f }, dc[s]);
        world.sourceHashes[s] = scene::contentHash(src[s]);
        logf("source %-13s %zu instances, %zu meshes, %zu lights, hash %.16s\n", src[s].name.c_str(), src[s].instances.size(), src[s].meshes.size(), src[s].lights.size(),
             world.sourceHashes[s].c_str());
    }
    checkAgainstSceneGen(src[SectionForest], src[SectionCity], src[SectionWaterside]);

    // ---- the lodge anchor: lake-local (110, 157) (r 192, 30 m above the waterline), floor 0.2 m above the lake ground,
    // window (+Z) towards the sun at 105 s ----
    {
        const float lx = 110.0f, lz = 157.0f;
        const float3 floorLocal{ lx, sg::lakeFloor(lx, lz) + 0.2f, lz };
        layout.lodge.translation = layout.lake.toWorld(floorLocal);
        const float3 sun = sunAt(105.0f).direction;
        layout.lodge.yaw = std::atan2(sun.x, sun.z);  // R_y(yaw) (0,0,1) = (sin yaw, 0, cos yaw) = horizontal sun direction
    }
    // The forest road: city east exit (city-local (195, 119.5)) to 10 m short of the combat stand (x 180 on the stand's axis).
    PathAnchors& pa = world.anchorsForPath;
    pa.road = { layout.city.toWorld({ 195.0f, 0, 119.5f }), { -40.0f, 0, -250.0f }, { 100.0f, 0, -205.0f }, { 180.0f, 0, -200.0f } };
    const Terrain terrain(layout);
    const Anchor anchors[SectionCount] = { layout.city, Anchor{}, layout.lake, layout.lodge };
    const Terrain::Source ground[SectionCount] = { Terrain::Source::City, Terrain::Source::Forest, Terrain::Source::Lake, Terrain::Source::Lodge };

    // The combat stand keeps C's coordinates and terrain: no district weight within standKeepOut.
    for (int k = 0; k < 64; ++k)
    {
        const float a = 2 * kPi * k / 64, r = layout.standKeepOut;
        const float x = layout.standCentre.x + r * std::cos(a), z = layout.standCentre.y + r * std::sin(a);
        if (terrain.cityWeight(x, z) > 0 || terrain.lakeWeight(x, z) > 0 || terrain.padWeight(x, z) > 0)
            fail("rppbuild: a district blends into the combat stand's keep-out circle at (%.0f, %.0f)", x, z);
    }

    Merger merger(w);
    Source sources[SectionCount] = { Source(src[0], merger), Source(src[1], merger), Source(src[2], merger), Source(src[3], merger) };

    // ---- terrain (the forest palette's ground material) ----
    {
        uint32_t groundMat = scene::kNone;
        for (const Mesh& m : src[SectionForest].meshes)
            if (m.name == "terrain") groundMat = sources[SectionForest].mat[m.submeshes.at(0).material];
        w.meshes.push_back(terrain.buildMesh(groundMat));
        Instance in;
        in.mesh = (uint32_t)w.meshes.size() - 1;
        in.flags = scene::InstanceCastShadow;
        w.instances.push_back(in);
    }

    // ---- classify the sources' instances ----
    struct Candidate { uint32_t section, instance; float3 pos; };
    std::vector<Candidate> standTrees, cityTrees, lakeTrees, baseTrees, reeds, baseGrass;
    std::vector<std::pair<uint32_t, uint32_t>> statics;  // (section, instance)
    std::vector<uint32_t> bodyInstances[SectionCount];
    std::vector<uint32_t> lodgeWalls;
    for (uint32_t s = 0; s < SectionCount; ++s)
    {
        const Scene& sc = src[s];
        uint32_t forestTreeOrdinal = 0;
        for (uint32_t i = 0; i < sc.instances.size(); ++i)
        {
            const Instance& in = sc.instances[i];
            const std::string& name = sc.meshes[in.mesh].name;
            if (name == "terrain" || name == "ground") continue;
            if (in.flags & scene::InstanceDynamic) { bodyInstances[s].push_back(i); continue; }
            const float3 p = anchors[s].toWorld(origin(in.transform));
            if (startsWith(name, "tree_"))
            {
                if (s == SectionForest) (forestTreeOrdinal++ < 100000 ? baseTrees : standTrees).push_back({ s, i, p });
                else if (s == SectionCity) cityTrees.push_back({ s, i, p });
                else if (s == SectionWaterside) { if (terrain.lakeWeight(p.x, p.z) > 0) lakeTrees.push_back({ s, i, p }); }
                else fail("rppbuild: unexpected tree in %s", sc.name.c_str());
                continue;
            }
            if (startsWith(name, "grass_")) { if (s == SectionForest) baseGrass.push_back({ s, i, p }); else fail("rppbuild: unexpected grass in %s", sc.name.c_str()); continue; }
            if (name == "reeds") { reeds.push_back({ s, i, p }); continue; }
            if (s == SectionInterior && (name == "wall_px" || name == "wall_nx")) { lodgeWalls.push_back(i); continue; }  // one gets the door
            statics.push_back({ s, i });
        }
    }
    if (baseTrees.size() != 100000 || baseGrass.size() != 1000000)
        fail("rppbuild: forest_combat base has %zu trees / %zu grass (expected 100000 / 1000000: C changed the scene layout)", baseTrees.size(), baseGrass.size());

    auto place = [&](uint32_t s, const Instance& in, bool reseat) {
        Instance o = in;
        o.mesh = sources[s].meshIndex(in.mesh);
        for (uint32_t& m : o.materialOverrides) m = sources[s].mat.at(m);
        o.transform = compose(anchors[s].matrix(), in.transform);
        if (reseat)
        {
            const float3 p = origin(o.transform);
            o.transform.m[1][3] += terrain.height(p.x, p.z) - terrain.sourceHeight(ground[s], p.x, p.z);
        }
        w.instances.push_back(o);
        return (uint32_t)w.instances.size() - 1;
    };

    for (const auto& [s, i] : statics) place(s, src[s].instances[i], true);
    {
        const int doorWall = layout.lodge.toLocal(layout.lake.translation).x >= 0 ? +1 : -1;
        for (uint32_t i : lodgeWalls)
            if (src[SectionInterior].meshes[src[SectionInterior].instances[i].mesh].name != (doorWall > 0 ? "wall_px" : "wall_nx")) place(SectionInterior, src[SectionInterior].instances[i], true);
    }
    world.stats.staticInstances = (uint32_t)statics.size() + 1;

    // Lodge wall with a door (C's room has none), in the side wall that faces the lake: wall_px = x [4, 4.2] (door z
    // [-0.5, 0.5]) or wall_nx = x [-4.2, -4] (door z [1.9, 2.9], clear of the sofa and the lamp pole); door height 2.2 m.
    {
        const float3 toLake = layout.lodge.toLocal(layout.lake.translation) ;
        pa.lodgeDoorWall = toLake.x >= 0 ? +1 : -1;
        const char* replaced = pa.lodgeDoorWall > 0 ? "wall_px" : "wall_nx";
        uint32_t plaster = scene::kNone;
        for (const Instance& in : src[SectionInterior].instances)
            if (src[SectionInterior].meshes[in.mesh].name == replaced) plaster = sources[SectionInterior].mat[src[SectionInterior].meshes[in.mesh].submeshes.at(0).material];
        if (plaster == scene::kNone) fail("rppbuild: interior has no %s", replaced);
        const float x0 = pa.lodgeDoorWall > 0 ? 4.0f : -4.2f, x1 = x0 + 0.2f;
        const float z0 = pa.lodgeDoorWall > 0 ? -0.5f : 1.9f, z1 = z0 + 1.0f;
        const float3 parts[3][2] = { { { x0, -0.2f, -3.0f }, { x1, 3.5f, z0 } }, { { x0, -0.2f, z1 }, { x1, 3.5f, 3.0f } }, { { x0, 2.2f, z0 }, { x1, 3.5f, z1 } } };
        for (int k = 0; k < 3; ++k)
        {
            w.meshes.push_back(boxMesh("rpp1_lodge_wall_door_" + std::to_string(k), parts[k][0], parts[k][1], plaster));
            Instance in;
            in.mesh = (uint32_t)w.meshes.size() - 1;
            in.transform = layout.lodge.matrix();
            w.instances.push_back(in);
        }
        const float zc = 0.5f * (z0 + z1), s = (float)pa.lodgeDoorWall;
        pa.lodgeDoorOutside = layout.lodge.toWorld({ s * 6.0f, 0, zc });
        pa.lodgeDoorInside = layout.lodge.toWorld({ s * 3.0f, 0, zc });
    }

    // ---- vegetation: exact totals ----
    auto cityCore = [&](float3 p) { const float3 l = layout.city.toLocal(p); return std::max(std::fabs(l.x), std::fabs(l.z)) <= 200.0f; };
    auto lakeCoreTree = [&](float3 p) { return terrain.lakeWeight(p.x, p.z) > 0.5f; };
    auto lakeWetGrass = [&](float3 p) {
        if (terrain.lakeWeight(p.x, p.z) <= 0.5f) return false;
        const float3 l = layout.lake.toLocal(p);
        return sg::lakeFloor(l.x, l.z) < 0.3f;
    };
    auto pad = [&](float3 p) { return terrain.padWeight(p.x, p.z) > 0; };
    auto roadDistance = [&](float3 p) {
        float best = 1e30f;
        for (size_t k = 0; k + 1 < pa.road.size(); ++k)
        {
            const float ax = pa.road[k].x, az = pa.road[k].z, bx = pa.road[k + 1].x - ax, bz = pa.road[k + 1].z - az;
            const float t = std::clamp(((p.x - ax) * bx + (p.z - az) * bz) / (bx * bx + bz * bz), 0.0f, 1.0f);
            const float dx = p.x - ax - t * bx, dz = p.z - az - t * bz;
            best = std::min(best, std::sqrt(dx * dx + dz * dz));
        }
        return best;
    };
    auto roadTree = [&](float3 p) { return roadDistance(p) < 6.0f; };
    auto roadGrass = [&](float3 p) { return roadDistance(p) < 2.5f; };
    auto inZone = [](float3 p) { return std::fabs(p.x) < 1000.0f && std::fabs(p.z) < 1000.0f; };
    WorldStats& st = world.stats;
    auto keep = [&](const Candidate& c) { place(c.section, src[c.section].instances[c.instance], true); };
    for (const Candidate& c : standTrees) keep(c);
    for (const Candidate& c : cityTrees) keep(c);
    for (const Candidate& c : lakeTrees) keep(c);
    st.treesStand = (uint32_t)standTrees.size();
    st.treesCity = (uint32_t)cityTrees.size();
    st.treesLake = (uint32_t)lakeTrees.size();
    uint32_t trees = st.treesStand + st.treesCity + st.treesLake;
    for (const Candidate& c : baseTrees)
    {
        if (trees >= 100000) break;
        if (cityCore(c.pos) || lakeCoreTree(c.pos) || pad(c.pos) || roadTree(c.pos)) continue;
        keep(c);
        ++trees;
        ++st.treesBase;
    }
    {
        Rng r(seed, "vegetation.trees");
        const Instance& proto = src[SectionForest].instances[baseTrees[0].instance];
        uint32_t guard = 0;
        while (trees < 100000)
        {
            if (++guard > 10000000) fail("rppbuild: tree fill did not converge");
            const float x = r.range(-1000, 1000), z = r.range(-1000, 1000);
            const float3 p{ x, terrain.height(x, z) - 0.2f, z };
            const float yaw = r.range(0, 2 * kPi), sc = r.range(0.8f, 1.3f), phase = r.range(0, 2 * kPi);
            if (cityCore(p) || lakeCoreTree(p) || pad(p) || roadTree(p)) continue;
            Instance in = proto;
            in.mesh = sources[SectionForest].meshIndex(src[SectionForest].instances[baseTrees[trees & 3].instance].mesh);
            const float c = std::cos(yaw) * sc, s = std::sin(yaw) * sc;
            in.transform = float3x4{};
            in.transform.m[0][0] = c; in.transform.m[0][2] = s; in.transform.m[1][1] = sc; in.transform.m[2][0] = -s; in.transform.m[2][2] = c;
            in.transform.m[0][3] = p.x; in.transform.m[1][3] = p.y; in.transform.m[2][3] = p.z;
            in.wind.phase = phase;
            w.instances.push_back(in);
            ++trees;
            ++st.treesFill;
        }
    }
    st.trees = trees;

    uint32_t grass = 0;
    for (const Candidate& c : reeds)
        if (inZone(c.pos)) { keep(c); ++grass; ++st.grassReeds; }
    for (const Candidate& c : baseGrass)
    {
        if (grass >= 1000000) break;
        if (cityCore(c.pos) || lakeWetGrass(c.pos) || pad(c.pos) || roadGrass(c.pos)) continue;
        keep(c);
        ++grass;
        ++st.grassBase;
    }
    {
        Rng r(seed, "vegetation.grass");
        uint32_t guard = 0;
        while (grass < 1000000)
        {
            if (++guard > 50000000) fail("rppbuild: grass fill did not converge");
            const float x = r.range(-1000, 1000), z = r.range(-1000, 1000);
            const float3 p{ x, terrain.height(x, z), z };
            const float yaw = r.range(0, 2 * kPi), sc = r.range(0.7f, 1.2f), phase = r.range(0, 2 * kPi);
            if (cityCore(p) || lakeWetGrass(p) || pad(p) || roadGrass(p)) continue;
            Instance in = src[SectionForest].instances[baseGrass[grass & 7].instance];
            in.mesh = sources[SectionForest].meshIndex(in.mesh);
            const float c = std::cos(yaw) * sc, s = std::sin(yaw) * sc;
            in.transform = float3x4{};
            in.transform.m[0][0] = c; in.transform.m[0][2] = s; in.transform.m[1][1] = sc; in.transform.m[2][0] = -s; in.transform.m[2][2] = c;
            in.transform.m[0][3] = p.x; in.transform.m[1][3] = p.y; in.transform.m[2][3] = p.z;
            in.wind.phase = phase;
            w.instances.push_back(in);
            ++grass;
            ++st.grassFill;
        }
    }
    st.grass = grass;

    // ---- rigid bodies: every section's 1,024 as dynamic instances; body positions re-seated with the content ----
    for (uint32_t s = 0; s < SectionCount; ++s)
    {
        SectionContent& sec = world.sections[s];
        sec.source = ids[s];
        sec.anchor = anchors[s];
        sec.content = dc[s];
        if (sec.content.bodies.size() != 1024 || bodyInstances[s].size() != 1024) fail("rppbuild: %s has %zu bodies", src[s].name.c_str(), sec.content.bodies.size());
        const float4 qa = anchors[s].quaternion();
        for (scenegen::DynamicBody& b : sec.content.bodies)
        {
            const Instance& in = src[s].instances.at(b.instance);
            const float3 p = anchors[s].toWorld(b.position);
            const float dy = terrain.height(p.x, p.z) - terrain.sourceHeight(ground[s], p.x, p.z);
            Instance o = in;
            o.mesh = sources[s].meshIndex(in.mesh);
            o.transform = compose(anchors[s].matrix(), in.transform);
            o.transform.m[1][3] += dy;
            // The instance and the file must agree exactly (BodiesFile check: 1e-3 m): the file takes the instance's values.
            b.position = origin(o.transform);
            b.rotation = quatMul(qa, b.rotation);
            b.velocity = anchors[s].rotate(b.velocity);
            b.angularVelocity = anchors[s].rotate(b.angularVelocity);
            b.restHeight += anchors[s].translation.y + dy;
            w.instances.push_back(o);
            b.instance = (uint32_t)w.instances.size() - 1;
        }
        world.stats.dynamicBodies += 1024;
        // Character slots: world coordinates on the world terrain; interior slots on water are re-drawn around the lodge.
        Rng r(seed, std::string("characters.") + sectionId((Section)s));
        for (scenegen::CharacterSlot& c : sec.content.characters)
        {
            float3 p = anchors[s].toWorld(c.position);
            c.yaw += anchors[s].yaw;
            if (s == SectionInterior)
            {
                for (int t = 0; t < 1000; ++t)
                {
                    const float3 l = layout.lake.toLocal(p);
                    if (sg::lakeFloor(l.x, l.z) > 0.3f && !(terrain.padWeight(p.x, p.z) > 0.9f)) break;
                    const float a = r.range(0, 2 * kPi), d = r.range(8.0f, 60.0f);
                    p = layout.lodge.toWorld({ d * std::cos(a), 0, d * std::sin(a) });
                }
            }
            p.y = terrain.height(p.x, p.z);
            c.position = p;
        }
    }

    // ---- lights (manifest slots lights.*) ----
    auto addLight = [&](scene::Light l, Section s) {
        w.lights.push_back(l);
        ++st.lightsBySection[s];
        if (l.castShadow) ++st.shadowedBySection[s];
    };
    {
        // City: the first 104 street spots, 208 rects, 52 tubes, 52 spheres of city_night's lists (C order: spots, rects, tubes, spheres).
        uint32_t want[6] = {}, taken[6] = {};
        want[(int)scene::LightType::Spot] = 104; want[(int)scene::LightType::Rect] = 208; want[(int)scene::LightType::Tube] = 52; want[(int)scene::LightType::Sphere] = 52;
        for (const scene::Light& l0 : src[SectionCity].lights)
        {
            const int t = (int)l0.type;
            if (taken[t] >= want[t]) continue;
            ++taken[t];
            scene::Light l = l0;
            if (l.type == scene::LightType::Tube && pa.neonCount == 0) pa.neonFirst = (uint32_t)w.lights.size();
            if (l.type == scene::LightType::Tube) ++pa.neonCount;
            l.position = layout.city.toWorld(l.position);
            l.forward = layout.city.rotate(l.forward);
            l.right = layout.city.rotate(l.right);
            addLight(l, SectionCity);
        }
        // Lodge: C interior's 6 (5 shadowed).
        for (const scene::Light& l0 : src[SectionInterior].lights)
        {
            scene::Light l = l0;
            l.position = layout.lodge.toWorld(l.position);
            l.forward = layout.lodge.rotate(l.forward);
            l.right = layout.lodge.rotate(l.right);
            addLight(l, SectionInterior);
        }
    }
    const float3 warm{ 1.0f, 0.78f, 0.55f }, flame{ 1.0f, 0.62f, 0.3f };
    {
        // Forest: 8 shadowed lanterns on a 12 m ring around the combat point; 16 muzzle-flash lights (unshadowed, intensity
        // keyed by the event script) at 1.4 m above the first 16 forest character slots.
        const float cx = layout.standCentre.x, cz = layout.standCentre.y;
        for (int k = 0; k < 8; ++k)
        {
            const float a = 2 * kPi * (k + 0.5f) / 8, x = cx + 12.0f * std::cos(a), z = cz + 12.0f * std::sin(a);
            addLight(pointLight(scene::LightType::Point, { x, terrain.height(x, z) + 1.2f, z }, warm, 400.0f, 20.0f, true), SectionForest);
        }
        pa.muzzleFirst = (uint32_t)w.lights.size();
        pa.muzzleCount = 16;
        for (int k = 0; k < 16; ++k)
        {
            const float3 p = world.sections[SectionForest].content.characters.at(k).position + float3{ 0, 1.4f, 0 };
            addLight(pointLight(scene::LightType::Point, p, flame, 0.0f, 8.0f, false), SectionForest);  // off until the path fires it
        }
    }
    {
        // Lake: 8 shadowed dock spots (4 m posts along the lodge shore), 40 bollard spheres on the shore path (lake-local r 205).
        for (int k = 0; k < 8; ++k)
        {
            const float3 lp = layout.lodge.toWorld({ -6.0f - 2.5f * k, 0, 8.0f - 2.0f * k });
            scene::Light l = pointLight(scene::LightType::Spot, { lp.x, terrain.height(lp.x, lp.z) + 4.0f, lp.z }, warm, 3000.0f, 25.0f, true);
            l.forward = { 0, -1, 0 };
            l.spotInner = 0.7f;
            l.spotOuter = 1.2f;
            addLight(l, SectionWaterside);
        }
        for (int k = 0; k < 40; ++k)
        {
            const float a = 2 * kPi * k / 40;
            const float3 p = layout.lake.toWorld({ 205.0f * std::cos(a), 0, 205.0f * std::sin(a) });
            scene::Light l = pointLight(scene::LightType::Sphere, { p.x, terrain.height(p.x, p.z) + 0.9f, p.z }, warm, 30000.0f, 12.0f, false);
            l.size = { 0.05f, 0 };
            addLight(l, SectionWaterside);
        }
    }
    {
        // Lodge extras: 3 shadowed porch spots outside the door (+X wall), 6 candles, a fireplace glow panel, 8 porch bulbs.
        for (int k = 0; k < 3; ++k)
        {
            scene::Light l = pointLight(scene::LightType::Spot, layout.lodge.toWorld({ 4.9f, 2.7f, -1.2f + 1.2f * k }), warm, 1500.0f, 12.0f, true);
            l.forward = { 0, -1, 0 };
            l.spotInner = 0.6f;
            l.spotOuter = 1.1f;
            addLight(l, SectionInterior);
        }
        const float3 candles[6] = { { -0.8f, 0.86f, -0.3f }, { -0.5f, 0.86f, 0.2f }, { 0.3f, 0.86f, -0.2f }, { 3.6f, 1.2f, 1.0f }, { 3.6f, 1.7f, 1.5f }, { -3.5f, 1.0f, -2.6f } };
        for (const float3& c : candles)
        {
            scene::Light l = pointLight(scene::LightType::Sphere, layout.lodge.toWorld(c), flame, 20000.0f, 4.0f, false);
            l.size = { 0.008f, 0 };
            addLight(l, SectionInterior);
        }
        {
            scene::Light l = pointLight(scene::LightType::Rect, layout.lodge.toWorld({ -3.99f, 0.5f, -1.0f }), flame, 800.0f, 6.0f, false);
            l.forward = layout.lodge.rotate({ 1, 0, 0 });
            l.right = layout.lodge.rotate({ 0, 0, 1 });
            l.size = { 0.8f, 0.5f };
            addLight(l, SectionInterior);
        }
        for (int k = 0; k < 8; ++k)
        {
            const float3 p = k < 4 ? float3{ -4.6f + 3.0f * k, 2.6f, 3.6f } : float3{ -4.6f + 3.0f * (k - 4), 2.6f, -3.6f };
            scene::Light l = pointLight(scene::LightType::Sphere, layout.lodge.toWorld(p), warm, 25000.0f, 8.0f, false);
            l.size = { 0.03f, 0 };
            addLight(l, SectionInterior);
        }
    }
    for (uint32_t s = 0; s < SectionCount; ++s)
    {
        st.lights += st.lightsBySection[s];
        st.lightsShadowed += st.shadowedBySection[s];
    }
    if (st.lights != 512 || st.lightsShadowed != 128) fail("rppbuild: %u lights (%u shadowed), expected 512 (128)", st.lights, st.lightsShadowed);

    // ---- environment at t = 0, cameras (section gate cameras at their anchors, names "<section>.<camera>") ----
    w.sun.direction = sunAt(0).direction;
    w.atmosphere = atmosphereAt(0);
    const WindState wind = windAt(0);
    w.windDirection = wind.direction;
    w.windSpeed = wind.speed;
    for (uint32_t s = 0; s < SectionCount; ++s)
        for (const scene::Camera& c0 : src[s].cameras)
        {
            scene::Camera c = c0;
            c.name = std::string(sectionId((Section)s)) + "." + c0.name;
            c.position = anchors[s].toWorld(c0.position);
            c.position.y += terrain.height(c.position.x, c.position.z) - terrain.sourceHeight(ground[s], c.position.x, c.position.z);
            c.forward = anchors[s].rotate(c0.forward);
            c.up = anchors[s].rotate(c0.up);
            w.cameras.push_back(c);
            scene::CameraPath p;
            p.name = c.name;
            p.keys.push_back({ 0, c.position, c.forward, c.up });
            w.paths.push_back(p);
        }

    world.layout = layout;
    for (const Instance& in : w.instances)
        for (const scene::Submesh& sm : w.meshes[in.mesh].submeshes) st.triangles += sm.indexCount / 3;
    return world;
}

std::string sectionBodiesJson(const World& world, Section s)
{
    auto v3 = [](float3 v) { return format("[%.9g, %.9g, %.9g]", v.x, v.y, v.z); };
    const scenegen::DynamicContent& c = world.sections[s].content;
    size_t resting = 0, falling = 0, rolling = 0;
    for (const scenegen::DynamicBody& b : c.bodies) (b.motion[0] == 'r' ? (b.motion[1] == 'e' ? resting : rolling) : falling)++;
    std::ostringstream o;
    o << "{\n  \"format\": 1,\n  \"scene\": \"" << world.scene.name << "\",\n  \"source\": \"" << scenegen::sceneName(world.sections[s].source)
      << "\",\n  \"seed\": 1,\n  \"scale\": 1,\n  \"section\": \"" << c.section << "\",\n  \"rpp1Section\": \"" << sectionId(s)
      << "\",\n  \"gravity\": [0, -9.81, 0],\n  \"t0\": 0,\n"
      << format("  \"mix\": { \"resting\": %zu, \"falling\": %zu, \"rolling\": %zu },\n", resting, falling, rolling) << "  \"bodies\": [\n";
    for (size_t i = 0; i < c.bodies.size(); ++i)
    {
        const scenegen::DynamicBody& b = c.bodies[i];
        o << "    { \"instance\": " << b.instance << ", \"kind\": \"" << b.kind << "\", \"shape\": \"" << b.shape << "\", \"halfExtents\": " << v3(b.halfExtents)
          << ", \"position\": " << v3(b.position) << format(", \"rotation\": [%.9g, %.9g, %.9g, %.9g]", b.rotation.x, b.rotation.y, b.rotation.z, b.rotation.w)
          << ", \"motion\": \"" << b.motion << "\", \"velocity\": " << v3(b.velocity) << ", \"angularVelocity\": " << v3(b.angularVelocity)
          << format(", \"restHeight\": %.9g, \"period\": %.9g }", b.restHeight, b.period) << (i + 1 < c.bodies.size() ? ",\n" : "\n");
    }
    o << "  ],\n  \"characters\": [\n";
    for (size_t i = 0; i < c.characters.size(); ++i)
    {
        const scenegen::CharacterSlot& ch = c.characters[i];
        o << "    { \"position\": " << v3(ch.position) << format(", \"yaw\": %.9g, \"hair\": %s }", ch.yaw, ch.hero ? "true" : "false") << (i + 1 < c.characters.size() ? ",\n" : "\n");
    }
    o << "  ]\n}\n";
    return o.str();
}
} // namespace unx::rpp
