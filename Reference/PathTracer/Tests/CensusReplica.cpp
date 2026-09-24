// Census cross-check: the microbench's census scene (Tools/Microbench buildRtScene, --only-edges) rebuilt as a
// scene::Scene with the same std::mt19937 stream and the same expressions (argument evaluation order included, same
// compiler), traced by the CPU reference with the same camera, and compared with the GPU census recorded in
// Tools/Microbench/Results/FLOORS.md. Thin foliage values were identical across GPU runs, so they are the strict check.
//   unx_test_census_replica [thin|card] [grass|nograss]
#include "unx/core/Log.h"
#include "unx/metrics/Census.h"
#include "unx/reference/PathTracer.h"

#include <cmath>
#include <cstring>
#include <random>
#include <vector>

using namespace unx;

namespace
{
float terrainHeight(float x, float z) { return 6.f * sinf(x / 70.f) * cosf(z / 90.f) + 2.f * sinf(x / 13.f + z / 17.f) + 0.7f * sinf(x / 3.1f - z / 2.7f); }

struct Inst
{
    float3x4 t;
    uint32_t mesh;
};
// Same parameter list as the microbench's instance() so the call expressions evaluate their arguments identically.
static Inst instance(float x, float y, float z, float yaw, float scale, uint32_t mesh, uint32_t flags)
{
    (void)flags;
    Inst d;
    const float c = cosf(yaw) * scale, s = sinf(yaw) * scale;
    d.t.m[0][0] = c; d.t.m[0][1] = 0; d.t.m[0][2] = s; d.t.m[0][3] = x;
    d.t.m[1][0] = 0; d.t.m[1][1] = scale; d.t.m[1][2] = 0; d.t.m[1][3] = y;
    d.t.m[2][0] = -s; d.t.m[2][1] = 0; d.t.m[2][2] = c; d.t.m[2][3] = z;
    d.mesh = mesh;
    return d;
}

scene::Mesh soup(const char* name, const std::vector<float>& pos, uint32_t material, bool canonicalUv)
{
    scene::Mesh m;
    m.name = name;
    for (size_t i = 0; i + 8 < pos.size() + 0; i += 9)
    {
        const float3 a{ pos[i], pos[i + 1], pos[i + 2] }, b{ pos[i + 3], pos[i + 4], pos[i + 5] }, c{ pos[i + 6], pos[i + 7], pos[i + 8] };
        float3 n = cross(b - a, c - a);
        n = length(n) > 0 ? normalize(n) : float3{ 0, 1, 0 };
        for (const float3& p : { a, b, c })
        {
            m.positions.push_back(p);
            m.normals.push_back(n);
        }
        if (canonicalUv)
        {
            // quad tri 0: (0,0)(1,0)(1,1), tri 1: (0,0)(1,1)(0,1)
            const bool first = ((i / 9) & 1) == 0;
            if (first) m.uv0.insert(m.uv0.end(), { { 0, 0 }, { 1, 0 }, { 1, 1 } });
            else m.uv0.insert(m.uv0.end(), { { 0, 0 }, { 1, 1 }, { 0, 1 } });
        }
    }
    for (uint32_t k = 0; k < m.positions.size(); ++k) m.indices.push_back(k);
    m.submeshes = { { 0, (uint32_t)m.indices.size(), material } };
    return m;
}

scene::Scene replica(bool thin, bool withGrass)
{
    scene::Scene S;
    S.name = thin ? "microbench_census_thin" : "microbench_census_card";
    scene::Material opaque;
    opaque.name = "opaque";
    S.materials.push_back(opaque);  // 0
    std::mt19937 rng(7);
    std::uniform_real_distribution<float> U(0.f, 1.f);
    const float sceneMin = -1000.f, sceneSize = 2000.f;
    // terrain 1024x1024 quads
    {
        const uint32_t n = 1024;
        scene::Mesh m;
        m.name = "terrain";
        for (uint32_t z = 0; z <= n; ++z)
            for (uint32_t x = 0; x <= n; ++x)
            {
                const float fx = sceneMin + x * (sceneSize / n), fz = sceneMin + z * (sceneSize / n);
                m.positions.push_back({ fx, terrainHeight(fx, fz), fz });
                m.normals.push_back({ 0, 1, 0 });
            }
        for (uint32_t z = 0; z < n; ++z)
            for (uint32_t x = 0; x < n; ++x)
            {
                const uint32_t a = z * (n + 1) + x;
                m.indices.insert(m.indices.end(), { a, a + 1, a + n + 1, a + 1, a + n + 2, a + n + 1 });
            }
        m.submeshes = { { 0, (uint32_t)m.indices.size(), 0 } };
        S.meshes.push_back(m);  // mesh 0
    }
    // city: 3000 boxes
    {
        std::vector<float> pos;
        auto box = [&](float x, float y, float z, float sx, float sy, float sz) {
            float v[8][3] = { { x, y, z }, { x + sx, y, z }, { x + sx, y + sy, z }, { x, y + sy, z }, { x, y, z + sz }, { x + sx, y, z + sz }, { x + sx, y + sy, z + sz }, { x, y + sy, z + sz } };
            int f[12][3] = { { 0, 1, 2 }, { 0, 2, 3 }, { 4, 6, 5 }, { 4, 7, 6 }, { 0, 4, 5 }, { 0, 5, 1 }, { 1, 5, 6 }, { 1, 6, 2 }, { 2, 6, 7 }, { 2, 7, 3 }, { 3, 7, 4 }, { 3, 4, 0 } };
            for (auto& t : f)
                for (int k : t) pos.insert(pos.end(), v[k], v[k] + 3);
        };
        for (int i = 0; i < 3000; ++i)
        {
            float x = -300 + U(rng) * 600, z = -300 + U(rng) * 600;
            box(x, terrainHeight(x, z) - 1, z, 8 + U(rng) * 20, 8 + U(rng) * 40, 8 + U(rng) * 20);
        }
        S.meshes.push_back(soup("city", pos, 0, false));  // mesh 1
    }
    // leaf alpha texture 256^2 (mip 0; the census samples level 0 with a wrapping bilinear sampler)
    {
        const uint32_t ts = 256;
        auto leafAlpha = [](float u, float v) {
            float du = u - 0.5f, dv = v - 0.5f;
            float ca = cosf(0.5f), sa = sinf(0.5f);
            float a = (du * ca + dv * sa) / 0.42f, b = (-du * sa + dv * ca) / 0.28f;
            float r = sqrtf(a * a + b * b), th = atan2f(b, a);
            return r < 1.f + 0.12f * sinf(9.f * th) ? 1.f : 0.f;
        };
        scene::Texture t;
        t.name = "leaf_alpha";
        t.width = t.height = ts;
        t.format = scene::TextureFormat::Rgba8Linear;
        t.wrap = true;
        t.texels.resize((size_t)ts * ts * 4, 255);
        for (uint32_t y = 0; y < ts; ++y)
            for (uint32_t x = 0; x < ts; ++x) t.texels[4 * ((size_t)y * ts + x) + 3] = (uint8_t)(255 * leafAlpha((x + 0.5f) / ts, (y + 0.5f) / ts));
        S.textures.push_back(t);
        scene::Material leaf;
        leaf.name = "leaf_alpha";
        leaf.baseColorTexture = 0;
        leaf.alphaCutoff = 0.5f + 1e-6f;  // microbench keeps alpha > 0.5; the scene rule is alpha >= cutoff
        leaf.twoSided = true;
        S.materials.push_back(leaf);  // 1
    }
    const int leafQuads = thin ? 40000 : 1500;
    const float leafHalf = thin ? 0.03f : 0.35f;
    // trees: 4 variants, trunk (indexed) + leaves (soup); mesh 2..5
    for (int t = 0; t < 4; ++t)
    {
        std::vector<float> pos;
        std::vector<uint32_t> idx;
        const int seg = 16, rings = 8;
        float H = 8.f;
        for (int r = 0; r <= rings; ++r)
            for (int s = 0; s < seg; ++s)
            {
                float a = s * 6.2831853f / seg, rad = 0.35f * (1.f - 0.6f * r / rings);
                pos.insert(pos.end(), { rad * cosf(a), H * r / rings, rad * sinf(a) });
            }
        for (int r = 0; r < rings; ++r)
            for (int s = 0; s < seg; ++s)
            {
                uint32_t a = r * seg + s, b = r * seg + (s + 1) % seg, c = a + seg, d = b + seg;
                idx.insert(idx.end(), { a, c, b, b, c, d });
            }
        const size_t trunkVerts = pos.size() / 3;
        std::vector<float> leaves;
        for (int q = 0; q < leafQuads; ++q)
        {
            float cx, cy, cz;
            do
            {
                cx = U(rng) * 2 - 1;
                cy = U(rng) * 2 - 1;
                cz = U(rng) * 2 - 1;
            } while (cx * cx + cy * cy + cz * cz > 1);
            cx *= 3.5f;
            cy = 7.f + cy * 3.0f;
            cz *= 3.5f;
            float yaw = U(rng) * 6.2831853f, pitch = U(rng) * 3.1415926f;
            float e = leafHalf;
            float ax = cosf(yaw) * cosf(pitch), ay = sinf(pitch), az = sinf(yaw) * cosf(pitch);
            float bx = -sinf(yaw), by = 0, bz = cosf(yaw);
            float p[4][3] = { { cx - (ax + bx) * e, cy - (ay + by) * e, cz - (az + bz) * e }, { cx + (ax - bx) * e, cy + (ay - by) * e, cz + (az - bz) * e },
                              { cx + (ax + bx) * e, cy + (ay + by) * e, cz + (az + bz) * e }, { cx - (ax - bx) * e, cy - (ay - by) * e, cz - (az - bz) * e } };
            leaves.insert(leaves.end(), p[0], p[0] + 3);
            leaves.insert(leaves.end(), p[1], p[1] + 3);
            leaves.insert(leaves.end(), p[2], p[2] + 3);
            leaves.insert(leaves.end(), p[0], p[0] + 3);
            leaves.insert(leaves.end(), p[2], p[2] + 3);
            leaves.insert(leaves.end(), p[3], p[3] + 3);
        }
        scene::Mesh m = soup("tree", leaves, 1, true);
        // Prepend the trunk (indexed) so mesh triangles 0..255 are the trunk, then the leaves.
        scene::Mesh tree;
        tree.name = "tree_" + std::to_string(t);
        for (size_t k = 0; k < trunkVerts; ++k)
        {
            tree.positions.push_back({ pos[3 * k], pos[3 * k + 1], pos[3 * k + 2] });
            const float3 r{ pos[3 * k], 0, pos[3 * k + 2] };
            tree.normals.push_back(length(r) > 0 ? normalize(r) : float3{ 1, 0, 0 });
            tree.uv0.push_back({ 0, 0 });
        }
        tree.indices = idx;
        const uint32_t base = (uint32_t)tree.positions.size();
        tree.positions.insert(tree.positions.end(), m.positions.begin(), m.positions.end());
        tree.normals.insert(tree.normals.end(), m.normals.begin(), m.normals.end());
        tree.uv0.insert(tree.uv0.end(), m.uv0.begin(), m.uv0.end());
        for (uint32_t i : m.indices) tree.indices.push_back(base + i);
        tree.submeshes = { { 0, (uint32_t)idx.size(), 0 }, { (uint32_t)idx.size(), (uint32_t)m.indices.size(), 1 } };
        S.meshes.push_back(tree);
    }
    // grass clump: thin = 100 opaque 4 mm blades; card = 8 cards 30 cm (alpha-tested with the leaf texture: the
    // census shader looks every non-opaque candidate up in the leaf uv buffer and alpha texture); mesh 6
    {
        std::vector<float> pos;
        const int blades = thin ? 100 : 8;
        const float bw = thin ? 0.002f : 0.15f, bh = 0.5f;
        for (int q = 0; q < blades; ++q)
        {
            float cx = U(rng) - 0.5f, cz = U(rng) - 0.5f, yaw = U(rng) * 6.28f, w = bw, h = bh * (0.6f + 0.8f * U(rng));
            float bx = cosf(yaw) * w, bz = sinf(yaw) * w;
            float lean = thin ? 0.1f * (U(rng) - 0.5f) : 0.f;
            float p[4][3] = { { cx - bx, 0, cz - bz }, { cx + bx, 0, cz + bz }, { cx + bx * 0.3f + lean, h, cz + bz * 0.3f }, { cx - bx * 0.3f + lean, h, cz - bz * 0.3f } };
            pos.insert(pos.end(), p[0], p[0] + 3);
            pos.insert(pos.end(), p[1], p[1] + 3);
            pos.insert(pos.end(), p[2], p[2] + 3);
            pos.insert(pos.end(), p[0], p[0] + 3);
            pos.insert(pos.end(), p[2], p[2] + 3);
            pos.insert(pos.end(), p[3], p[3] + 3);
        }
        S.meshes.push_back(soup("grass", pos, thin ? 0 : 1, true));
    }
    // instances: terrain, city, 100k trees, 1M grass (the microbench's order of RNG use)
    auto add = [&](const Inst& d) {
        scene::Instance in;
        in.mesh = d.mesh;
        in.transform = d.t;
        S.instances.push_back(in);
    };
    add(instance(0, 0, 0, 0, 1, 0, 0));
    add(instance(0, 0, 0, 0, 1, 1, 0));
    std::vector<Inst> trees, grass;
    for (int i = 0; i < 100000; ++i)
    {
        float x = sceneMin + U(rng) * sceneSize, z = sceneMin + U(rng) * sceneSize;
        trees.push_back(instance(x, terrainHeight(x, z) - 0.2f, z, U(rng) * 6.28f, 0.8f + U(rng) * 0.5f, (uint32_t)(2 + (i & 3)), 0));
    }
    for (int i = 0; i < 1000000; ++i)
    {
        float x = sceneMin + U(rng) * sceneSize, z = sceneMin + U(rng) * sceneSize;
        grass.push_back(instance(x, terrainHeight(x, z), z, U(rng) * 6.28f, 0.7f + U(rng) * 0.5f, 6u, 0));
    }
    for (const Inst& d : trees) add(d);
    if (withGrass)
        for (const Inst& d : grass) add(d);
    scene::Camera c;
    c.name = "census";
    c.position = { 600.f, terrainHeight(600.f, 600.f) + 1.7f, 600.f };
    c.forward = { 0.9848f, -0.1736f, 0.f };
    c.up = { 0.1736f, 0.9848f, 0.f };
    c.forward = normalize(c.forward);
    c.up = normalize(c.up);
    c.verticalFov = 2 * std::atan(tanf(30.f * 3.14159f / 180.f));
    c.nearPlane = 1e-6f;  // the census rays start at the camera (tMin 0)
    S.cameras.push_back(c);
    return S;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        const bool thin = !(argc > 1 && std::strcmp(argv[1], "card") == 0);
        const bool withGrass = !(argc > 2 && std::strcmp(argv[2], "nograss") == 0);
        const scene::Scene s = replica(thin, withGrass);
        reference::PathTracer pt(s);
        const std::vector<uint64_t> ids = pt.primaryIdentities(reference::resolveCamera(s, { "census", "", 0 }), 3840, 2160);
        const metrics::CensusResult r = metrics::selfCensus(ids, 3840, 2160);
        logf("replica %s %s: >=2 %.4f  >=3 %.4f  >=5 %.4f  >=9 %.4f | missed >=1 %.4f  >=4 %.4f  >=8 %.4f\n", thin ? "thin" : "card", withGrass ? "+grass" : "no grass",
             r.distinct2, r.distinct3, r.distinct5, r.distinct9, r.missed1, r.missed4, r.missed8);
        return 0;
    }
    catch (const std::exception& e)
    {
        logf("FAILED: %s\n", e.what());
        return 1;
    }
}
