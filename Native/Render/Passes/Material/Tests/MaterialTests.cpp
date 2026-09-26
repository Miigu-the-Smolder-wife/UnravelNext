// M material resolve correctness (no GPU lock):
//   1. mip chains on the CPU: LEAN slope moments (mean, internal variance, |mean|^2) of known normal maps, alpha
//      coverage kept at the cutoff on every level, box weights for odd sizes;
//   2. the resolve's surface reconstruction against a double-precision replica (pixel-centre ray, barycentrics, uv and
//      its screen derivatives, curvature variance, band-limited roughness), including a camera 2 km from the origin;
//   3. G-buffer normal / base colour / roughness, tile class lists and dispatch counts, R's reflection lobe tiles;
//   4. LEAN at the two footprint limits: magnified (no added variance) and a footprint covering whole tiles of the map
//      (exact total slope variance of the texture).
//   5. the reference's shading-normal rules: a mapped normal facing away from the viewer is bent to n.v = 1e-4.
//   unx_test_material_materialtests [--no-debug-layer]
#include "MTestFrame.h"
#include "../../../../../Reference/PathTracer/src/CutFace.h"

#include "unx/material/TextureSystem.h"
#include "unx/scene/MaterialModel.h"

#include <cstdio>
#include <dxgi1_6.h>
#include <map>
#include <random>
#include <set>

using namespace unx;
using namespace unx::render;
using namespace unx::mtest;

namespace
{
constexpr double kPi = 3.14159265358979323846;

struct D3
{
    double x = 0, y = 0, z = 0;
};
D3 d3(float3 v) { return { v.x, v.y, v.z }; }
D3 operator+(D3 a, D3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
D3 operator-(D3 a, D3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
D3 operator*(D3 a, double s) { return { a.x * s, a.y * s, a.z * s }; }
double dot(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
D3 cross(D3 a, D3 b) { return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x }; }
double len(D3 a) { return std::sqrt(dot(a, a)); }
D3 norm(D3 a) { return a * (1.0 / len(a)); }

// GPU vertex normal/tangent storage: octahedral snorm16 (GpuScene.cpp encode, Scene.hlsli decode).
uint32_t octEncode(float3 n)
{
    float s = std::fabs(n.x) + std::fabs(n.y) + std::fabs(n.z);
    float ex = n.x / s, ey = n.y / s;
    if (n.z < 0)
    {
        const float ox = (1 - std::fabs(ey)) * (ex >= 0 ? 1.f : -1.f), oy = (1 - std::fabs(ex)) * (ey >= 0 ? 1.f : -1.f);
        ex = ox;
        ey = oy;
    }
    auto q = [](float v) { return (uint32_t)(int32_t)std::lround(std::clamp(v, -1.f, 1.f) * 32767.f) & 0xFFFFu; };
    return q(ex) | (q(ey) << 16);
}
D3 octDecode(uint32_t p)
{
    const double ex = (int16_t)(p & 0xFFFF) / 32767.0, ey = (int16_t)(p >> 16) / 32767.0;
    D3 n{ ex, ey, 1 - std::fabs(ex) - std::fabs(ey) };
    if (n.z < 0)
    {
        const double ox = (1 - std::fabs(n.y)) * (n.x >= 0 ? 1 : -1), oy = (1 - std::fabs(n.x)) * (n.y >= 0 ? 1 : -1);
        n.x = ox;
        n.y = oy;
    }
    return norm(n);
}
D3 quantised(float3 n) { return octDecode(octEncode(n)); }

double srgbToLinear(double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); }

// ---------------------------------------------------------------- scene building
uint32_t addGridPlane(scene::Scene& s, float size, uint32_t n, float uvPerMetre, uint32_t material, float3 origin = {})
{
    scene::Mesh m;
    m.name = "plane";
    for (uint32_t j = 0; j <= n; ++j)
        for (uint32_t i = 0; i <= n; ++i)
        {
            const float x = origin.x + (i / (float)n - 0.5f) * size, z = origin.z + (j / (float)n - 0.5f) * size;
            m.positions.push_back({ x, origin.y, z });
            m.normals.push_back({ 0, 1, 0 });
            m.tangents.push_back({ 1, 0, 0, 1 });
            m.uv0.push_back({ x * uvPerMetre, z * uvPerMetre });
        }
    for (uint32_t j = 0; j < n; ++j)
        for (uint32_t i = 0; i < n; ++i)
        {
            const uint32_t a = j * (n + 1) + i, b = a + 1, c = a + n + 1, d = c + 1;
            // +Y facing, counter-clockwise seen from above: (a, c, b), (b, c, d).
            for (uint32_t v : { a, c, b, b, c, d }) m.indices.push_back(v);
        }
    m.submeshes.push_back({ 0, (uint32_t)m.indices.size(), material });
    s.meshes.push_back(std::move(m));
    return (uint32_t)s.meshes.size() - 1;
}

uint32_t addSphere(scene::Scene& s, float radius, uint32_t rings, uint32_t sectors, uint32_t material)
{
    scene::Mesh m;
    m.name = "sphere";
    for (uint32_t r = 0; r <= rings; ++r)
        for (uint32_t k = 0; k <= sectors; ++k)
        {
            const double th = kPi * r / rings, ph = 2 * kPi * k / sectors;
            const float3 n{ (float)(std::sin(th) * std::cos(ph)), (float)std::cos(th), (float)(std::sin(th) * std::sin(ph)) };
            m.positions.push_back(n * radius);
            m.normals.push_back(n);
            const float3 t = normalize(float3{ (float)-std::sin(ph), 0.0f, (float)std::cos(ph) } + float3{ 1e-6f, 0, 0 });
            m.tangents.push_back({ t.x, t.y, t.z, 1 });
            m.uv0.push_back({ (float)k / sectors, (float)r / rings });
        }
    for (uint32_t r = 0; r < rings; ++r)
        for (uint32_t k = 0; k < sectors; ++k)
        {
            const uint32_t a = r * (sectors + 1) + k, b = a + 1, c = a + sectors + 1, d = c + 1;
            // Outward counter-clockwise.
            for (uint32_t v : { a, b, c, b, d, c }) m.indices.push_back(v);
        }
    m.submeshes.push_back({ 0, (uint32_t)m.indices.size(), material });
    s.meshes.push_back(std::move(m));
    return (uint32_t)s.meshes.size() - 1;
}

scene::Camera lookAt(float3 eye, float3 target, const char* name)
{
    scene::Camera c;
    c.name = name;
    c.position = eye;
    c.forward = normalize(target - eye);
    c.up = { 0, 1, 0 };
    return c;
}

// ---------------------------------------------------------------- CPU replica of MaterialSurface.hlsli
struct Replica
{
    D3 offset;
    double uv[2], dx[2], dy[2];
    double variance;
    bool front;
    D3 normal;  // normalised interpolated normal (no normal map), not flipped
    D3 geometric;  // unit normal of the triangle's plane on its counter-clockwise side
};

void pixelRay(const ViewDesc& v, double px, double py, D3& D, D3& Dx, D3& Dy)
{
    const auto& P = v.proj.m;
    const double ndcx = px / v.width * 2 - 1, ndcy = 1 - py / v.height * 2;
    const double vx = (ndcx + P[0][2] - P[0][3]) / P[0][0], vy = (ndcy + P[1][2] - P[1][3]) / P[1][1];
    const D3 ax{ v.view.m[0][0], v.view.m[0][1], v.view.m[0][2] }, ay{ v.view.m[1][0], v.view.m[1][1], v.view.m[1][2] }, az{ v.view.m[2][0], v.view.m[2][1], v.view.m[2][2] };
    D = ax * vx + ay * vy - az;
    Dx = ax * (2.0 / (v.width * P[0][0]));
    Dy = ay * (-2.0 / (v.height * P[1][1]));
}

Replica replicate(const TestFrame& tf, const ViewDesc& view, uint32_t visId, uint32_t px, uint32_t py)
{
    const gpu::VisibleCluster vc = tf.vis.visible.at((visId - 1) >> 7);  // INTERFACES 7.1 v1.5: stored + 1
    const gpu::Cluster& c = tf.vis.clusters.clusters.at(vc.cluster);
    const uint32_t packed = tf.vis.clusters.clusterTriangles.at(c.triangleOffset + ((visId - 1) & 127));
    const scene::Instance& inst = tf.sceneData.instances.at(vc.instance);
    const scene::Mesh& mesh = tf.sceneData.meshes.at(inst.mesh);
    D3 P[3], N[3];
    double uv[3][2];
    for (int k = 0; k < 3; ++k)
    {
        const uint32_t mv = tf.vis.clusters.clusterVertexIndices.at(c.vertexOffset + ((packed >> (8 * k)) & 0xFF));
        P[k] = d3(inst.transform.transformPoint(mesh.positions[mv]));
        const D3 n = quantised(mesh.normals[mv]);
        N[k] = norm(d3(inst.transform.transformVector(float3{ (float)n.x, (float)n.y, (float)n.z })));
        uv[k][0] = mesh.uv0[mv].x;
        uv[k][1] = mesh.uv0[mv].y;
    }
    D3 D, Dx, Dy;
    pixelRay(view, px + 0.5, py + 0.5, D, Dx, Dy);
    const D3 C = d3(view.position);
    const D3 r0 = P[0] - C, e1 = P[1] - P[0], e2 = P[2] - P[0], n = cross(e1, e2);
    const double nD = dot(n, D), t = dot(n, r0) / nD, inv = 1 / dot(n, n);
    const D3 r = D * t - r0;
    const D3 rx = (Dx - D * (dot(n, Dx) / nD)) * t, ry = (Dy - D * (dot(n, Dy) / nD)) * t;
    const double b1 = dot(n, cross(r, e2)) * inv, b2 = dot(n, cross(e1, r)) * inv;
    const double b1x = dot(n, cross(rx, e2)) * inv, b2x = dot(n, cross(e1, rx)) * inv, b1y = dot(n, cross(ry, e2)) * inv, b2y = dot(n, cross(e1, ry)) * inv;
    const double b[3] = { 1 - b1 - b2, b1, b2 }, bx[3] = { -b1x - b2x, b1x, b2x }, by[3] = { -b1y - b2y, b1y, b2y };
    Replica out{};
    out.offset = D * t;
    out.front = nD < 0;
    D3 nn{}, nx{}, ny{};
    for (int k = 0; k < 3; ++k)
    {
        for (int a = 0; a < 2; ++a)
        {
            out.uv[a] += b[k] * uv[k][a];
            out.dx[a] += bx[k] * uv[k][a];
            out.dy[a] += by[k] * uv[k][a];
        }
        nn = nn + N[k] * b[k];
        nx = nx + N[k] * bx[k];
        ny = ny + N[k] * by[k];
    }
    const double l = len(nn);
    const D3 nh = nn * (1 / l);
    const D3 gx = (nx - nh * dot(nh, nx)) * (1 / l), gy = (ny - nh * dot(nh, ny)) * (1 / l);
    out.variance = (dot(gx, gx) + dot(gy, gy)) / 12.0;
    out.normal = nh;
    out.geometric = norm(n);
    return out;
}

struct Report
{
    int failures = 0;
    void operator()(bool ok, const char* what, double value, double limit)
    {
        logf("%-66s %.3e (limit %.1e) %s\n", what, value, limit, ok ? "ok" : "FAIL");
        if (!ok) ++failures;
    }
};

// ---------------------------------------------------------------- 1. CPU mip chains
void testMipChains(Report& report)
{
    // Checkerboard of slopes +-a along x on 64 x 64: level 1 (2 x 2 texel blocks) holds mean 0 and internal trace a^2.
    scene::Scene s;
    scene::Texture nt;
    nt.name = "checker normals";
    nt.width = nt.height = 64;
    nt.format = scene::TextureFormat::Rg8Normal;
    const uint8_t hiX = 160, loX = 96;  // x = +-0.2549 (a = x / z)
    for (uint32_t y = 0; y < 64; ++y)
        for (uint32_t x = 0; x < 64; ++x)
        {
            nt.texels.push_back(((x + y) & 1) ? hiX : loX);
            nt.texels.push_back(128);
        }
    s.textures.push_back(nt);
    scene::Material m;
    m.normalTexture = 0;
    s.materials.push_back(m);
    const material::MipChain chain = material::buildMipChain(s, 0);
    // Expected level >= 1 statistics from the two texel kinds (8-bit codes: 128 is 0.0039, not 0).
    auto slopes = [](uint8_t cx, uint8_t cy, double& sx, double& sy) {
        const double x = cx / 255.0 * 2 - 1, y = cy / 255.0 * 2 - 1, z = std::sqrt(1 - x * x - y * y);
        sx = x / z;
        sy = y / z;
    };
    double hx, hy, lx, ly;
    slopes(hiX, 128, hx, hy);
    slopes(loX, 128, lx, ly);
    const double mx = 0.5 * (hx + lx), my = 0.5 * (hy + ly);
    const double e2 = 0.5 * (hx * hx + hy * hy + lx * lx + ly * ly), q0 = mx * mx + my * my, inner0 = e2 - q0;
    const double S = chain.slopeRange;
    auto channel = [&](uint32_t level, uint32_t i, uint32_t k) {
        uint16_t v;
        std::memcpy(&v, chain.levels[level].data() + (size_t)i * 8 + 2 * k, 2);
        return v / 65535.0;
    };
    double worstMean = 0, worstInner = 0, worstQ = 0;
    for (uint32_t l = 1; l < (uint32_t)chain.levels.size(); ++l)
    {
        const uint32_t texels = (uint32_t)chain.levels[l].size() / 8;
        for (uint32_t i = 0; i < texels; ++i)
        {
            const double gx = (channel(l, i, 0) * 2 - 1) * S, gy = (channel(l, i, 1) * 2 - 1) * S;
            const double inner = channel(l, i, 2) * 2 * S * S, q = channel(l, i, 3) * 2 * S * S;
            worstMean = std::max({ worstMean, std::abs(gx - mx), std::abs(gy - my) });
            worstInner = std::max(worstInner, std::abs(inner - inner0));
            worstQ = std::max(worstQ, std::abs(q - q0));
        }
    }
    report(worstMean < 2 * S / 65535, "mips: checker slopes, mean slope of every level (abs)", worstMean, 2 * S / 65535);
    report(worstInner < 2 * S * S / 65535, "mips: checker slopes, internal variance trace = a^2 (abs)", worstInner, 2 * S * S / 65535);
    report(worstQ < 2 * S * S / 65535, "mips: checker slopes, |mean|^2 channel (abs)", worstQ, 2 * S * S / 65535);

    // Alpha coverage kept at the cutoff on every level (random leaf-like mask, 96 x 80: odd sizes below).
    scene::Scene s2;
    scene::Texture at;
    at.name = "alpha";
    at.width = 96;
    at.height = 80;
    at.format = scene::TextureFormat::Rgba8Srgb;
    std::mt19937 rng(7);
    for (uint32_t y = 0; y < at.height; ++y)
        for (uint32_t x = 0; x < at.width; ++x)
        {
            const double d = std::hypot(x - 48.0, y - 40.0) / 40.0 + 0.25 * std::uniform_real_distribution<double>(-1, 1)(rng);
            at.texels.push_back(60);
            at.texels.push_back(140);
            at.texels.push_back(40);
            at.texels.push_back((uint8_t)std::clamp(255.0 * (1.2 - d), 0.0, 255.0));
        }
    s2.textures.push_back(at);
    scene::Material leaf;
    leaf.baseColorTexture = 0;
    leaf.alphaCutoff = 0.5f;
    s2.materials.push_back(leaf);
    const material::MipChain ac = material::buildMipChain(s2, 0);
    auto cov = [&](uint32_t l) {
        const auto& b = ac.levels[l];
        size_t pass = 0, n = b.size() / 4;
        for (size_t i = 0; i < n; ++i) pass += b[i * 4 + 3] / 255.0 >= 0.5 ? 1 : 0;
        return (double)pass / n;
    };
    const double c0 = cov(0);
    double worstCov = 0;
    uint32_t w = at.width, h = at.height;
    for (uint32_t l = 1; l < (uint32_t)ac.levels.size(); ++l)
    {
        w = std::max(1u, w / 2);
        h = std::max(1u, h / 2);
        if (w * h < 16) break;  // below 16 texels the fraction is quantised coarser than any tolerance
        worstCov = std::max(worstCov, std::abs(cov(l) - c0) - 1.0 / (w * h));
    }
    report(worstCov <= 0, "mips: alpha coverage at the cutoff, excess over one texel (levels >= 16 texels)", worstCov, 0.0);
    report(ac.levels.size() == 7 && ac.levels[1].size() == 48 * 40 * 4, "mips: odd-size chain dimensions (96x80 -> 7 levels)", (double)ac.levels.size(), 7);
}

// ---------------------------------------------------------------- 2-3. resolve against the replica
void testResolve(TestFrame& tf, Report& report)
{
    scene::Scene s;
    s.name = "material resolve test";
    scene::Material ground;
    ground.name = "ground";
    ground.baseColor = { 0.5f, 0.3f, 0.2f };
    ground.roughness = 0.3f;
    s.materials.push_back(ground);
    scene::Material ball;
    ball.name = "ball";
    ball.cls = scene::MaterialClass::Subsurface;
    ball.baseColor = { 0.8f, 0.6f, 0.5f };
    ball.roughness = 0.05f;
    s.materials.push_back(ball);
    scene::Material leaf;
    leaf.name = "two-sided sheet";
    leaf.cls = scene::MaterialClass::Foliage;
    leaf.baseColor = { 0.2f, 0.5f, 0.1f };
    leaf.roughness = 0.6f;
    leaf.twoSided = true;
    leaf.transmission = 0.3f;
    s.materials.push_back(leaf);
    const uint32_t plane = addGridPlane(s, 40, 32, 0.25f, 0);
    const uint32_t sphere = addSphere(s, 1.0f, 24, 48, 1);
    const uint32_t farPlane = addGridPlane(s, 6, 4, 1.0f, 0, { 2000, 0, 1995 });
    const uint32_t sheet = addGridPlane(s, 2, 2, 1.0f, 2);
    scene::Instance iPlane;
    iPlane.mesh = plane;
    s.instances.push_back(iPlane);
    scene::Instance iSphere;
    iSphere.mesh = sphere;
    iSphere.transform = float3x4::translation({ 2, 1, 0 });
    s.instances.push_back(iSphere);
    scene::Instance iFar;
    iFar.mesh = farPlane;
    s.instances.push_back(iFar);
    scene::Instance iSheet;  // the sheet flipped upside down at 0.5 m: seen from above, its back side (two-sided flip)
    iSheet.mesh = sheet;
    iSheet.transform.m[1][1] = -1;
    iSheet.transform.m[2][2] = -1;
    iSheet.transform.m[0][3] = -2;
    iSheet.transform.m[1][3] = 0.5f;
    s.instances.push_back(iSheet);
    s.cameras.push_back(lookAt({ 0, 3, 9 }, { 0, 0, 0 }, "near"));
    s.cameras.push_back(lookAt({ 2000, 1.7f, 2000 }, { 2000, 0, 1994 }, "far from origin"));
    tf.setScene(s);

    const uint32_t W = 960, H = 540;
    for (uint32_t cam = 0; cam < 2; ++cam)
    {
        std::shared_ptr<std::vector<uint8_t>> vis, gb, words, tiles, args, lobes, dbg;
        ViewDesc viewDesc;
        uint32_t tilesX = 0, tilesY = 0;
        tf.run([&](FramePassContext& fc) {
            ViewResources v = tf.mainView(fc, W, H, cam);
            viewDesc = v.view;
            tf.vis.record(fc, v);
            BufferRef debug = fc.graph.createBuffer({ "m.test.debug", (uint64_t)W * H * 48, 16 });
            fc.state<material::ResolveDebug>("M.resolveDebug").buffer = debug;
            tracks::materialResolve(fc, v);
            fc.state<material::ResolveDebug>("M.resolveDebug").buffer = {};
            const material::ResolveOutputs& o = material::resolveOutputs(fc, v);
            tilesX = o.tilesX;
            tilesY = o.tilesY;
            vis = tf.readback(fc, v.visId);
            gb = tf.readback(fc, v.gbuffer);
            words = tf.readback(fc, o.materialWord);
            lobes = tf.readback(fc, v.reflectionLobeTiles);
            tiles = tf.readbackBuffer(fc, o.tiles, (uint64_t)material::kShadeClassCount * tilesX * tilesY * 4);
            args = tf.readbackBuffer(fc, o.tileArgs, material::kShadeClassCount * 12);
            dbg = tf.readbackBuffer(fc, debug, (uint64_t)W * H * 48);
        });
        const char* tag = cam == 0 ? "near" : "2 km from origin";

        // Surface reconstruction at every pixel.
        double eUv = 0, eD = 0, eVar = 0, ePos = 0, eRough = 0, eNormal = 0, eBase = 0;
        uint32_t surfaces = 0, frontMismatch = 0, backSheet = 0;
        for (uint32_t y = 0; y < H; ++y)
            for (uint32_t x = 0; x < W; ++x)
            {
                const uint32_t id = texelOf<uint32_t>(*vis, W, x, y);
                if (id == 0) continue;  // VIS_NONE
                ++surfaces;
                const Replica r = replicate(tf, viewDesc, id, x, y);
                float4 g[3];
                std::memcpy(g, dbg->data() + ((size_t)y * W + x) * 48, 48);
                // uv error in units of the pixel's uv step (the texture footprint), derivatives relative.
                const double dScale = std::max({ std::abs(r.dx[0]), std::abs(r.dx[1]), std::abs(r.dy[0]), std::abs(r.dy[1]), 1e-12 });
                eUv = std::max(eUv, std::max(std::abs(g[0].x - r.uv[0]), std::abs(g[0].y - r.uv[1])) / dScale);
                eD = std::max(eD, std::max({ std::abs(g[0].z - r.dx[0]), std::abs(g[0].w - r.dx[1]), std::abs(g[1].x - r.dy[0]), std::abs(g[1].y - r.dy[1]) }) / dScale);
                eVar = std::max(eVar, std::abs(g[1].z - r.variance) / (r.variance + 1e-6));
                const D3 go{ g[2].x, g[2].y, g[2].z };
                const double pe = len(go - r.offset) / len(r.offset);
                if (pe > ePos && pe > 1e-5)
                    logf("  pos (%u,%u) id %08x gpu (%.6f %.6f %.6f) cpu (%.6f %.6f %.6f) uv gpu (%.6f %.6f) cpu (%.6f %.6f)\n", x, y, id, go.x, go.y, go.z, r.offset.x, r.offset.y,
                         r.offset.z, g[0].x, g[0].y, r.uv[0], r.uv[1]);
                ePos = std::max(ePos, pe);
                if ((g[2].w > 0.5f) != r.front) ++frontMismatch;

                const gpu::VisibleCluster vc = tf.vis.visible[(id - 1) >> 7];
                const uint32_t matIndex = tf.vis.clusters.clusters[vc.cluster].material;
                const scene::Material& mat = s.materials[matIndex];
                const double alpha = std::max((double)mat.roughness * mat.roughness, 1e-4);
                const double rough = std::min(std::sqrt(std::sqrt(alpha * alpha + r.variance)), 1.0);
                const uint2 packed = texelOf<uint2>(*gb, W, x, y);
                eRough = std::max(eRough, std::abs((packed.y >> 24) / 255.0 - rough));
                D3 n = r.normal, ng = r.geometric;
                if (!r.front && mat.twoSided)
                {
                    n = n * -1;
                    ng = ng * -1;
                    ++backSheet;
                }
                // the reference's rules (MaterialInternal.hlsli): on the geometric side, bent to n.v = 1e-4 if needed
                if (dot(n, ng) < 0) n = n - ng * (2 * dot(n, ng));
                const D3 view = norm(r.offset) * -1.0;
                if ((r.front || mat.twoSided) && dot(n, view) < 1e-4) n = norm(n + view * (1e-4 - dot(n, view)));
                eNormal = std::max(eNormal, len(octDecode(packed.x) - n));
                for (int k = 0; k < 3; ++k)
                {
                    const double c = (&mat.baseColor.x)[k], enc = c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1 / 2.4) - 0.055;
                    eBase = std::max(eBase, std::abs((double)((packed.y >> (8 * k)) & 0xFF) - std::round(enc * 255)));
                }
                const uint32_t word = texelOf<uint32_t>(*words, W, x, y);
                M_CHECK((word & 0xFFFF) == matIndex, "material word %u != material %u at (%u, %u)", word & 0xFFFF, matIndex, x, y);
            }
        logf("[%s] %u surface pixels, %u two-sided back-side pixels\n", tag, surfaces, backSheet);
        report(eUv < 5e-2, format("resolve [%s]: uv vs double replica (pixel uv steps)", tag).c_str(), eUv, 5e-2);
        report(eD < 2e-3, format("resolve [%s]: duv/dx, duv/dy vs double replica (rel.)", tag).c_str(), eD, 2e-3);
        report(eVar < 2e-2, format("resolve [%s]: curvature variance (rel., +1e-6)", tag).c_str(), eVar, 2e-2);
        report(ePos < 1e-5, format("resolve [%s]: hit position (rel. to distance)", tag).c_str(), ePos, 1e-5);
        report(frontMismatch == 0, format("resolve [%s]: front-facing mismatches", tag).c_str(), frontMismatch, 0);
        report(eRough <= 0.5 / 255 + 1e-6, format("resolve [%s]: G-buffer roughness vs replica", tag).c_str(), eRough, 0.5 / 255);
        report(eNormal < 1e-4, format("resolve [%s]: G-buffer normal vs replica (oct16)", tag).c_str(), eNormal, 1e-4);
        report(eBase <= 1, format("resolve [%s]: G-buffer base colour sRGB8 code vs encoded material", tag).c_str(), eBase, 1);
        if (cam == 0) report(backSheet > 1000, "resolve [near]: two-sided sheet seen from its back side (pixels)", backSheet, 1000);

        // Tile classes and dispatch counts, reflection lobe tiles.
        uint32_t counts[material::kShadeClassCount] = {};
        std::set<uint32_t> listed[material::kShadeClassCount];
        uint32_t argBad = 0, listBad = 0, lobeBad = 0;
        for (uint32_t c = 0; c < material::kShadeClassCount; ++c)
        {
            uint32_t a3[3];
            std::memcpy(a3, args->data() + 12 * c, 12);
            counts[c] = a3[0];
            if (a3[1] != 1 || a3[2] != 1) ++argBad;
            for (uint32_t i = 0; i < a3[0]; ++i)
            {
                uint32_t t;
                std::memcpy(&t, tiles->data() + 4 * ((size_t)c * tilesX * tilesY + i), 4);
                if (!listed[c].insert(t).second) ++listBad;
            }
        }
        for (uint32_t ty = 0; ty < tilesY; ++ty)
            for (uint32_t tx = 0; tx < tilesX; ++tx)
            {
                uint32_t mask = 0;
                double minLobe = 1.0;
                for (uint32_t y = ty * 8; y < std::min(H, ty * 8 + 8); ++y)
                    for (uint32_t x = tx * 8; x < std::min(W, tx * 8 + 8); ++x)
                    {
                        const uint32_t id = texelOf<uint32_t>(*vis, W, x, y);
                        if (id == 0)
                        {
                            mask |= 1u << (uint32_t)material::ShadeClass::Sky;
                            continue;
                        }
                        const scene::Material& mat = s.materials[tf.vis.clusters.clusters[tf.vis.visible[(id - 1) >> 7].cluster].material];
                        mask |= 1u << (uint32_t)(mat.cls == scene::MaterialClass::Subsurface ? material::ShadeClass::Subsurface : material::ShadeClass::Opaque);
                        const uint2 packed = texelOf<uint2>(*gb, W, x, y);
                        D3 D, Dx, Dy;
                        pixelRay(viewDesc, x + 0.5, y + 0.5, D, Dx, Dy);
                        const double r = (packed.y >> 24) / 255.0, al = std::max(r * r, 1e-4);
                        const double NoV = std::clamp(dot(octDecode(packed.x), norm(D) * -1), 0.0, 1.0);
                        minLobe = std::min(minLobe, 2 * std::atan(std::sqrt(3.0) * al) * NoV / kPi);
                    }
                const uint32_t key = tx | (ty << 16);
                for (uint32_t c = 0; c < material::kShadeClassCount; ++c)
                    if (((mask >> c) & 1) != (uint32_t)listed[c].count(key)) ++listBad;
                const int expected = (int)std::floor(std::clamp(minLobe, 0.0, 1.0) * 255.0);
                const int got = (*lobes)[(size_t)ty * TestFrame::rowPitch(tilesX, 1) + tx];
                if (std::abs(got - expected) > 1) ++lobeBad;
            }
        report(argBad == 0, format("tiles [%s]: dispatch args (y = z = 1)", tag).c_str(), argBad, 0);
        report(listBad == 0, format("tiles [%s]: class lists = CPU classification (mismatches)", tag).c_str(), listBad, 0);
        report(lobeBad == 0, format("tiles [%s]: reflection lobe tiles within 1/255 of CPU min", tag).c_str(), lobeBad, 0);
        logf("[%s] tiles: sky %u, opaque %u, subsurface %u, water %u of %u\n", tag, counts[0], counts[1], counts[2], counts[3], tilesX * tilesY);
    }
}

// ---------------------------------------------------------------- 4. LEAN footprint limits
void testLean(TestFrame& tf, Report& report)
{
    scene::Scene s;
    s.name = "lean test";
    scene::Texture nt;
    nt.name = "bumps";
    nt.width = nt.height = 64;
    nt.format = scene::TextureFormat::Rg8Normal;
    std::mt19937 rng(11);
    std::uniform_int_distribution<int> u(128 - 40, 128 + 40);
    for (uint32_t i = 0; i < 64 * 64; ++i)
    {
        nt.texels.push_back((uint8_t)u(rng));
        nt.texels.push_back((uint8_t)u(rng));
    }
    s.textures.push_back(nt);
    scene::Material m;
    m.name = "bumpy";
    m.baseColor = { 0.5f, 0.5f, 0.5f };
    m.roughness = 0.2f;
    m.normalTexture = 0;
    s.materials.push_back(m);
    // Tile 0.1 m: 640 texels per metre.
    addGridPlane(s, 4000, 8, 10.0f, 0);
    scene::Instance a;
    a.mesh = 0;
    s.instances.push_back(a);
    s.cameras.push_back(lookAt({ 0.3f, 0.12f, 0.2f }, { 0.3f, 0, 0.2f + 1e-3f }, "magnified"));
    s.cameras.push_back(lookAt({ 0.3f, 400, 0.2f }, { 0.3f, 0, 0.2f + 1e-3f }, "whole tiles"));
    tf.setScene(s);

    // Exact slope statistics of the map (the moments' source): total variance trace and mean.
    double sx = 0, sy = 0, s2 = 0;
    for (uint32_t i = 0; i < 64 * 64; ++i)
    {
        const double x = nt.texels[2 * i] / 255.0 * 2 - 1, y = nt.texels[2 * i + 1] / 255.0 * 2 - 1, z = std::sqrt(std::max(1 - x * x - y * y, 0.0));
        const double l = std::sqrt(x * x + y * y + z * z), nz = std::max(z / l, 1.0 / 64);
        sx += x / l / nz;
        sy += y / l / nz;
        s2 += (x * x + y * y) / (l * l) / (nz * nz);
    }
    sx /= 4096;
    sy /= 4096;
    s2 /= 4096;
    const double trace = s2 - sx * sx - sy * sy;
    const double alpha = 0.2 * 0.2;
    const double roughWhole = std::sqrt(std::sqrt(alpha * alpha + trace));
    logf("lean: map slope trace %.5f, mean (%.4f, %.4f); expected roughness at whole-tile footprints %.4f (base 0.2)\n", trace, sx, sy, roughWhole);

    // Mip 0 of the moments exactly as uploaded (unorm16), for the magnified replica.
    const material::MipChain chain = material::buildMipChain(s, 0);
    const double S = chain.slopeRange;
    auto mean0 = [&](int ix, int iy, int k) {
        ix = ((ix % 64) + 64) % 64;
        iy = ((iy % 64) + 64) % 64;
        uint16_t v;
        std::memcpy(&v, chain.levels[0].data() + ((size_t)iy * 64 + ix) * 8 + 2 * k, 2);
        return ((double)(float)(v / 65535.0) * 2 - 1) * S;
    };

    const uint32_t W = 960, H = 540;
    for (uint32_t cam = 0; cam < 2; ++cam)
    {
        std::shared_ptr<std::vector<uint8_t>> gb, dbg;
        tf.run([&](FramePassContext& fc) {
            ViewResources v = tf.mainView(fc, W, H, cam);
            tf.vis.record(fc, v);
            BufferRef debug = fc.graph.createBuffer({ "m.test.debug", (uint64_t)W * H * 48, 16 });
            fc.state<material::ResolveDebug>("M.resolveDebug").buffer = debug;
            tracks::materialResolve(fc, v);
            fc.state<material::ResolveDebug>("M.resolveDebug").buffer = {};
            gb = tf.readback(fc, v.gbuffer);
            dbg = tf.readbackBuffer(fc, debug, (uint64_t)W * H * 48);
        });
        double sum = 0, worst = 0, nDev = 0, worstVar = 0, maxRho = 0;
        uint32_t n = 0;
        for (uint32_t y = H / 4; y < 3 * H / 4; ++y)
            for (uint32_t x = W / 4; x < 3 * W / 4; ++x)
            {
                const uint2 p = texelOf<uint2>(*gb, W, x, y);
                const double r = (p.y >> 24) / 255.0;
                double expected = roughWhole;
                if (cam == 0)
                {
                    // Bilinear interpolant of mip 0 over the pixel box: (|J tx|^2 + |J ty|^2) / 12.
                    float4 g[2];
                    std::memcpy(g, dbg->data() + ((size_t)y * W + x) * 48, 32);
                    const double tx[2] = { g[0].z * 64.0, g[0].w * 64.0 }, ty[2] = { g[1].x * 64.0, g[1].y * 64.0 };
                    maxRho = std::max(maxRho, std::sqrt(std::max(tx[0] * tx[0] + tx[1] * tx[1], ty[0] * ty[0] + ty[1] * ty[1])));
                    const double pu = g[0].x * 64.0 - 0.5, pv = g[0].y * 64.0 - 0.5;
                    const int iu = (int)std::floor(pu), iv = (int)std::floor(pv);
                    const double fu = pu - iu, fv = pv - iv;
                    double jx[2], jy[2];
                    for (int k = 0; k < 2; ++k)
                    {
                        const double m00 = mean0(iu, iv, k), m10 = mean0(iu + 1, iv, k), m01 = mean0(iu, iv + 1, k), m11 = mean0(iu + 1, iv + 1, k);
                        const double du = (m10 - m00) * (1 - fv) + (m11 - m01) * fv, dv = (m01 - m00) * (1 - fu) + (m11 - m10) * fu;
                        jx[k] = du * tx[0] + dv * tx[1];
                        jy[k] = du * ty[0] + dv * ty[1];
                    }
                    const double var = (jx[0] * jx[0] + jx[1] * jx[1] + jy[0] * jy[0] + jy[1] * jy[1]) / 12.0;
                    worstVar = std::max(worstVar, std::abs(g[1].z - var) / (var + 1e-6));
                    expected = std::sqrt(std::sqrt(alpha * alpha + var));
                }
                sum += r;
                worst = std::max(worst, std::abs(r - expected));
                if (cam == 1)
                {
                    // Mean normal of whole tiles: normalize(T sx + B sy + N) with T = +x, B = cross(N, T) = -z (N = +y).
                    const D3 e = norm(D3{ sx, 1, -sy });
                    nDev = std::max(nDev, len(octDecode(p.x) - e));
                }
                ++n;
            }
        if (cam == 0)
        {
            logf("lean [magnified]: footprint major axis up to %.3f texels\n", maxRho);
            report(worstVar < 1e-2, "lean [magnified]: variance = bilinear field over the pixel box (rel., +1e-6)", worstVar, 1e-2);
            report(worst <= 0.5 / 255 + 1e-3, "lean [magnified]: roughness vs replica", worst, 0.5 / 255 + 1e-3);
        }
        else
        {
            report(worst <= 2.5 / 255, "lean [whole-tile footprint]: roughness = sqrt(sqrt(a^2 + map trace))", worst, 2.5 / 255);
            report(nDev < 5e-3, "lean [whole-tile footprint]: normal = the map's mean slope", nDev, 5e-3);
        }
        logf("lean [%s]: mean roughness %.4f over %u pixels\n", cam == 0 ? "magnified" : "whole tiles", sum / n, n);
    }
}

// 5. The reference's shading-normal rules (Reference/PathTracer/src/RtScene.cpp, INTERFACES 8.1 v1.65): a constant
// normal map tilting the normal 52 degrees towards +x, seen from above looking along +x at 30 degrees down, so the mapped
// normal faces away from the viewer below 52 degrees of elevation. The G-buffer normal must be the mapped normal bent
// towards the pixel's view direction until n.v = 1e-4 (normalize(n + v (1e-4 - n.v))), and the mapped normal itself
// where it already faces the viewer (U2: without the bend those texels reflected nothing).
void testFacingAway(TestFrame& tf, Report& report)
{
    scene::Scene s;
    s.name = "facing away";
    scene::Texture nt;
    nt.name = "tilt";
    nt.width = nt.height = 4;
    nt.format = scene::TextureFormat::Rg8Normal;
    for (uint32_t i = 0; i < 16; ++i)
    {
        nt.texels.push_back(228);
        nt.texels.push_back(128);
    }
    s.textures.push_back(nt);
    scene::Material m;
    m.name = "tilted";
    m.baseColor = { 0.5f, 0.5f, 0.5f };
    m.roughness = 0.5f;
    m.normalTexture = 0;
    s.materials.push_back(m);
    addGridPlane(s, 40, 8, 1.0f, 0);
    scene::Instance a;
    a.mesh = 0;
    s.instances.push_back(a);
    s.cameras.push_back(lookAt({ 0, 1, 0.01f }, { 1.7320508f, 0, 0.01f }, "grazing"));
    tf.setScene(s);
    // The map's mean slope as uploaded (mip 0, unorm16 x slope range; constant texture)
    const material::MipChain chain = material::buildMipChain(s, 0);
    double mean[2];
    for (int k = 0; k < 2; ++k)
    {
        uint16_t v;
        std::memcpy(&v, chain.levels[0].data() + 2 * k, 2);
        mean[k] = ((double)(float)(v / 65535.0) * 2 - 1) * chain.slopeRange;
    }
    const D3 mapped = norm(D3{ mean[0], 1, -mean[1] });  // T = +x, B = cross(N, T) = -z, N = +y

    const uint32_t W = 640, H = 360;
    std::shared_ptr<std::vector<uint8_t>> gb;
    ViewDesc view;
    tf.run([&](FramePassContext& fc) {
        ViewResources v = tf.mainView(fc, W, H, 0);
        view = v.view;
        tf.vis.record(fc, v);
        tracks::materialResolve(fc, v);
        gb = tf.readback(fc, v.gbuffer);
    });
    double worstAway = 0, worstFacing = 0, minNv = 1;
    uint32_t away = 0, facing = 0;
    for (uint32_t y = H / 2 + 8; y < H; ++y)
        for (uint32_t x = 8; x < W - 8; ++x)
        {
            D3 D, Dx, Dy;
            pixelRay(view, x + 0.5, y + 0.5, D, Dx, Dy);
            if (D.y >= -1e-3) continue;  // (the plane only)
            const D3 v = norm(D) * -1.0;
            const double nv = dot(mapped, v);
            const D3 expected = nv < 1e-4 ? norm(mapped + v * (1e-4 - nv)) : mapped;
            const D3 got = octDecode(texelOf<uint2>(*gb, W, x, y).x);
            if (nv < 1e-4)
            {
                ++away;
                worstAway = std::max(worstAway, len(got - expected));
                minNv = std::min(minNv, dot(got, v));
            }
            else
            {
                ++facing;
                worstFacing = std::max(worstFacing, len(got - expected));
            }
        }
    logf("facing away: %u pixels with the mapped normal facing away (n.v < 1e-4), %u facing; G-buffer n.v >= %.2e\n", away, facing, minNv);
    report(away > 20000 && worstAway < 2e-4, "facing away: G-buffer normal = the mapped normal bent to n.v = 1e-4 (octahedral 2 x 16 bits)", worstAway, 2e-4);
    report(facing > 1000 && worstFacing < 2e-4, "facing away: where the mapped normal faces the viewer it is kept", worstFacing, 2e-4);
}

// 6. A11 cut faces (MaterialCut.hlsli over render C's CutFace.hlsli) against the Cut definition of the CPU reference
// (Reference/PathTracer/src/CutFace.h): an open 2 m grid of Cut material whose object-space normal is oblique (all three
// projections weigh in), in a rigidly rotated instance, with a base colour texture, a normal map and an edge damage band
// along the grid's border (the cluster triangle words' boundary bits, as the cluster builder sets them). Magnified
// (a texel spans many pixels: the footprint filter is bilinear mip 0, the reference's). Per pixel, from the pixel's own
// barycentrics: base colour (the three projections' taps x weights, damage applied; pixels within two footprints of the
// damage threshold left out: the resolve filters the band's edge, the reference steps) and the shading normal (whiteout
// of the taps' mean slopes in object space, to world through the instance). Also: interior pixels far from the border
// have no damage, and the band covers pixels next to the border.
void testCutFace(TestFrame& tf, Report& report)
{
    namespace cf = unx::reference::cutface;
    scene::Scene s;
    s.name = "cut face test";
    const uint32_t T = 32;
    scene::Texture base;  // smooth colour field (sRGB)
    base.name = "cut base";
    base.width = base.height = T;
    for (uint32_t y = 0; y < T; ++y)
        for (uint32_t x = 0; x < T; ++x)
        {
            base.texels.push_back((uint8_t)(128 + 100 * std::sin(0.4 * x)));
            base.texels.push_back((uint8_t)(128 + 90 * std::cos(0.3 * y)));
            base.texels.push_back((uint8_t)(128 + 80 * std::sin(0.25 * (x + y))));
            base.texels.push_back(255);
        }
    s.textures.push_back(base);
    scene::Texture bumps;  // gentle smooth slopes
    bumps.name = "cut bumps";
    bumps.width = bumps.height = T;
    bumps.format = scene::TextureFormat::Rg8Normal;
    for (uint32_t y = 0; y < T; ++y)
        for (uint32_t x = 0; x < T; ++x)
        {
            bumps.texels.push_back((uint8_t)(128 + 30 * std::sin(0.5 * x + 0.2 * y)));
            bumps.texels.push_back((uint8_t)(128 + 30 * std::cos(0.35 * y)));
        }
    s.textures.push_back(bumps);
    scene::Material cut;
    cut.name = "cut";
    cut.cls = scene::MaterialClass::Cut;
    cut.baseColor = { 0.9f, 0.8f, 0.7f };
    cut.roughness = 0.6f;
    cut.baseColorTexture = 0;
    cut.normalTexture = 1;
    cut.cutScale = 0.5f;
    cut.cutDamageWidth = 0.12f;
    s.materials.push_back(cut);
    const uint32_t mesh = addGridPlane(s, 2.0f, 8, 1.0f, 0);
    // object space: the grid turned so its normal is (1, 2, 3) / |.| (every projection has a weight)
    const float3 nObj = normalize(float3{ 1, 2, 3 });
    {
        const float3 t = normalize(cross(float3{ 0, 0, 1 }, nObj)), b = cross(t, nObj);  // (t, n, b) = the new (x, y, z): a rotation
        scene::Mesh& m = s.meshes[mesh];
        for (float3& p : m.positions) p = t * p.x + nObj * p.y + b * p.z;
        for (float3& n : m.normals) n = nObj;
        for (float4& tg : m.tangents) tg = { t.x, t.y, t.z, 1 };
    }
    scene::Instance inst;
    inst.mesh = mesh;
    {
        // rigid: a rotation taking nObj towards the camera (+z), then 4 m in front of it
        const float3 a = normalize(cross(nObj, float3{ 0, 0, 1 }));
        const float angle = std::acos(dot(nObj, float3{ 0, 0, 1 }));
        const float c = std::cos(angle), sn = std::sin(angle), k = 1 - c;
        float3x4& m = inst.transform;
        m.m[0][0] = c + a.x * a.x * k, m.m[0][1] = a.x * a.y * k - a.z * sn, m.m[0][2] = a.x * a.z * k + a.y * sn;
        m.m[1][0] = a.y * a.x * k + a.z * sn, m.m[1][1] = c + a.y * a.y * k, m.m[1][2] = a.y * a.z * k - a.x * sn;
        m.m[2][0] = a.z * a.x * k - a.y * sn, m.m[2][1] = a.z * a.y * k + a.x * sn, m.m[2][2] = c + a.z * a.z * k;
        m.m[0][3] = 0.1f, m.m[1][3] = 0.05f, m.m[2][3] = -2.0f;
    }
    s.instances.push_back(inst);
    s.cameras.push_back(lookAt({ 0.2f, 0.1f, 1.0f }, { 0.1f, 0.05f, -2.0f }, "cut"));
    // boundary edges (the builder's rule at LOD 0: an edge the submesh uses once), in each cluster triangle's corner order
    const scene::Mesh& gm = s.meshes[mesh];
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> uses;
    for (size_t i = 0; i < gm.indices.size(); i += 3)
        for (int e = 0; e < 3; ++e)
        {
            const uint32_t a = gm.indices[i + (e + 1) % 3], b = gm.indices[i + (e + 2) % 3];
            ++uses[{ std::min(a, b), std::max(a, b) }];
        }
    tf.setScene(s, {}, [&](ClusterData& d) {
        for (const gpu::Cluster& c : d.clusters)
        {
            const uint32_t triangles = (c.counts >> 8) & 0xFF;
            for (uint32_t t = 0; t < triangles; ++t)
            {
                uint32_t& word = d.clusterTriangles[c.triangleOffset + t];
                uint32_t mv[3];
                for (int k = 0; k < 3; ++k) mv[k] = d.clusterVertexIndices[c.vertexOffset + ((word >> (8 * k)) & 0xFF)];
                for (uint32_t i = 0; i < 3; ++i)
                {
                    const uint32_t a = mv[(i + 1) % 3], b = mv[(i + 2) % 3];
                    if (uses[{ std::min(a, b), std::max(a, b) }] == 1) word |= 1u << (24 + i);
                }
            }
        }
    });

    const uint32_t W = 960, H = 540;
    std::shared_ptr<std::vector<uint8_t>> vis, gb;
    ViewDesc viewDesc;
    tf.run([&](FramePassContext& fc) {
        ViewResources v = tf.mainView(fc, W, H, 0);
        viewDesc = v.view;
        tf.vis.record(fc, v);
        tracks::materialResolve(fc, v);
        vis = tf.readback(fc, v.visId);
        gb = tf.readback(fc, v.gbuffer);
    });

    // mip 0 of the moments exactly as uploaded (the normal map's LEAN means), and the base colour texels in linear
    const material::MipChain chain = material::buildMipChain(s, 1);
    const double S = chain.slopeRange;
    auto wrap = [&](int i) { return ((i % (int)T) + (int)T) % (int)T; };
    auto mean0 = [&](int ix, int iy, int k) {
        uint16_t v;
        std::memcpy(&v, chain.levels[0].data() + ((size_t)wrap(iy) * T + wrap(ix)) * 8 + 2 * k, 2);
        return ((double)(float)(v / 65535.0) * 2 - 1) * S;
    };
    auto srgbToLinear = [](double c) { return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4); };
    auto oetf = [](double c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1 / 2.4) - 0.055; };
    auto bilinear = [&](double u, double v, auto&& texel) {  // texel(ix, iy) -> D3, wrap addressing, texel centres at +0.5
        const double pu = u * T - 0.5, pv = v * T - 0.5;
        const int iu = (int)std::floor(pu), iv = (int)std::floor(pv);
        const double fu = pu - iu, fv = pv - iv;
        return texel(iu, iv) * ((1 - fu) * (1 - fv)) + texel(iu + 1, iv) * (fu * (1 - fv)) + texel(iu, iv + 1) * ((1 - fu) * fv) + texel(iu + 1, iv + 1) * (fu * fv);
    };
    auto baseTexel = [&](int ix, int iy) {
        const uint8_t* t = &base.texels[((size_t)wrap(iy) * T + wrap(ix)) * 4];
        return D3{ srgbToLinear(t[0] / 255.0), srgbToLinear(t[1] / 255.0), srgbToLinear(t[2] / 255.0) };
    };
    auto slopeTexel = [&](int ix, int iy) { return D3{ mean0(ix, iy, 0), mean0(ix, iy, 1), 0 }; };

    {
        uint32_t covered = 0;
        for (uint32_t i = 0; i < W * H; ++i) covered += texelOf<uint32_t>(*vis, W, i % W, i / W) != 0;
        logf("cut face: %u pixels rasterised, %zu clusters, first word %08x, camera (%g %g %g)\n", covered, tf.vis.clusters.clusters.size(),
             tf.vis.clusters.clusterTriangles.empty() ? 0u : tf.vis.clusters.clusterTriangles[0], viewDesc.position.x, viewDesc.position.y, viewDesc.position.z);
    }
    double worstBase = 0, worstNormal = 0;
    uint32_t judged = 0, damaged = 0, bandSkipped = 0, interiorDamaged = 0, interior = 0;
    for (uint32_t y = 0; y < H; ++y)
        for (uint32_t x = 0; x < W; ++x)
        {
            const uint32_t id = texelOf<uint32_t>(*vis, W, x, y);
            if (id == 0) continue;
            const gpu::VisibleCluster vc = tf.vis.visible.at((id - 1) >> 7);
            const gpu::Cluster& c = tf.vis.clusters.clusters.at(vc.cluster);
            const uint32_t word = tf.vis.clusters.clusterTriangles.at(c.triangleOffset + ((id - 1) & 127));
            uint32_t mv[3];
            D3 Pw[3];
            float3 Po[3], No[3];
            for (int k = 0; k < 3; ++k)
            {
                mv[k] = tf.vis.clusters.clusterVertexIndices.at(c.vertexOffset + ((word >> (8 * k)) & 0xFF));
                Po[k] = gm.positions[mv[k]];
                const D3 q = quantised(gm.normals[mv[k]]);
                No[k] = float3{ (float)q.x, (float)q.y, (float)q.z };
                Pw[k] = d3(inst.transform.transformPoint(Po[k]));
            }
            D3 D, Dx, Dy;
            pixelRay(viewDesc, x + 0.5, y + 0.5, D, Dx, Dy);
            const D3 C = d3(viewDesc.position);
            const D3 r0 = Pw[0] - C, e1 = Pw[1] - Pw[0], e2 = Pw[2] - Pw[0], n = cross(e1, e2);
            const double nD = dot(n, D), t = dot(n, r0) / nD, inv = 1 / dot(n, n);
            const D3 r = D * t - r0, rx = (Dx - D * (dot(n, Dx) / nD)) * t, ry = (Dy - D * (dot(n, Dy) / nD)) * t;
            const double b1 = dot(n, cross(r, e2)) * inv, b2 = dot(n, cross(e1, r)) * inv;
            const double b1x = dot(n, cross(rx, e2)) * inv, b2x = dot(n, cross(e1, rx)) * inv, b1y = dot(n, cross(ry, e2)) * inv, b2y = dot(n, cross(e1, ry)) * inv;
            const double b[3] = { 1 - b1 - b2, b1, b2 }, bx[3] = { -b1x - b2x, b1x, b2x }, by[3] = { -b1y - b2y, b1y, b2y };
            float3 p{}, dpdx{}, dpdy{}, ni{};
            for (int k = 0; k < 3; ++k)
            {
                p = p + Po[k] * (float)b[k];
                dpdx = dpdx + Po[k] * (float)bx[k];
                dpdy = dpdy + Po[k] * (float)by[k];
                ni = ni + No[k] * (float)b[k];
            }
            ni = normalize(ni);
            const float3 ng = normalize(cross(Po[1] - Po[0], Po[2] - Po[0]));
            D3 baseSum{}, normalSum{};
            for (uint32_t k = 0; k < 3; ++k)
            {
                const cf::Projection pr = cf::projection(k, p, ng, cut.cutScale);
                if (!(pr.weight > 0)) continue;
                baseSum = baseSum + bilinear(pr.uv.x, pr.uv.y, baseTexel) * pr.weight;
                const D3 slope = bilinear(pr.uv.x, pr.uv.y, slopeTexel);
                const float3 tn = normalize(float3{ (float)slope.x, (float)slope.y, 1.0f });
                normalSum = normalSum + d3(cf::whiteout(tn, pr, ni)) * pr.weight;
            }
            // damage: the reference's threshold and the pixel's object footprint
            const uint32_t edges = (word >> 24) & 7;
            const float distance = cf::edgeDistance(float3{ (float)b[0], (float)b[1], (float)b[2] }, Po[0], Po[1], Po[2], edges);
            const float3 q = p * (1.0f / cut.cutDamageWidth);
            const float noise = 0.5f * cf::noise(q) + 0.3f * cf::noise(q * 2.03f + float3{ 17.1f, 17.1f, 17.1f }) + 0.2f * cf::noise(q * 4.01f + float3{ 41.7f, 41.7f, 41.7f });
            const double threshold = cut.cutDamageWidth * (0.35 + 0.65 * noise), footprint = std::max(length(dpdx), length(dpdy));
            const bool bandEdge = std::isfinite(distance) && std::abs(threshold - distance) < 2 * footprint;
            const float dmg = cf::damage(p, distance, cut.cutDamageWidth);
            if (distance > 0.3f)
            {
                ++interior;
                interiorDamaged += dmg > 0;
            }
            if (bandEdge)
            {
                ++bandSkipped;
                continue;
            }
            damaged += dmg > 0;
            const D3 expectedBase = D3{ baseSum.x * cut.baseColor.x, baseSum.y * cut.baseColor.y, baseSum.z * cut.baseColor.z } * (1 - 0.45 * dmg);
            const uint2 packed = texelOf<uint2>(*gb, W, x, y);
            const double got[3] = { srgbToLinear((packed.y & 0xFF) / 255.0), srgbToLinear(((packed.y >> 8) & 0xFF) / 255.0),
                                    srgbToLinear(((packed.y >> 16) & 0xFF) / 255.0) };
            const double want[3] = { expectedBase.x, expectedBase.y, expectedBase.z };
            for (int k = 0; k < 3; ++k)  // in sRGB codes: the G-buffer's 8-bit quantisation
                worstBase = std::max(worstBase, std::abs(oetf(got[k]) - oetf(std::min(want[k], 1.0))) * 255);
            const D3 nw = norm(d3(inst.transform.transformVector(normalize(float3{ (float)normalSum.x, (float)normalSum.y, (float)normalSum.z }))));
            worstNormal = std::max(worstNormal, len(octDecode(packed.x) - nw));
            ++judged;
        }
    logf("cut face: %u pixels judged (%u in the damage band), %u at the band's edge left out; interior (> 0.3 m from the border) %u, damaged %u\n", judged,
         damaged, bandSkipped, interior, interiorDamaged);
    // (the G-buffer's rounding, half a code, and the texture unit's fixed-point bilinear weights, 8 subtexel bits)
    report(judged > 50000 && worstBase <= 2.0, "cut face: base colour = three projections x weights, damage applied (sRGB codes)", worstBase, 2.0);
    report(worstNormal < 5e-3, "cut face: shading normal = whiteout of the taps, object -> world (octahedral 16 bits)", worstNormal, 5e-3);
    report(damaged > 1000 && interiorDamaged == 0, "cut face: the damage band lies along the border only", interiorDamaged, 0);
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        bool debugLayer = true, warp = false, cutOnly = false;
        for (int i = 1; i < argc; ++i)
        {
            if (std::string(argv[i]) == "--no-debug-layer") debugLayer = false;
            if (std::string(argv[i]) == "--warp") warp = true;
            if (std::string(argv[i]) == "--cut") cutOnly = true;
        }
        ComPtr<ID3D12Device> warpDevice;
        if (warp)  // --warp: the software adapter (no debug layer)
        {
            ComPtr<IDXGIFactory6> factory;
            check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "DXGI factory");
            ComPtr<IDXGIAdapter> adapter;
            check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP adapter");
            if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_2, IID_PPV_ARGS(&warpDevice))))
                check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&warpDevice)), "WARP device");
        }
        Report report;
        TestFrame tf(debugLayer, false, warpDevice.Get());
        if (cutOnly)  // --cut: test 6 alone
        {
            testCutFace(tf, report);
            logf("%s: %d failure(s)\n", report.failures ? "FAILED" : "passed", report.failures);
            return report.failures ? 1 : 0;
        }
        testMipChains(report);
        testResolve(tf, report);
        testLean(tf, report);
        testFacingAway(tf, report);
        testCutFace(tf, report);
        logf("%s: %d failure(s)\n", report.failures ? "FAILED" : "passed", report.failures);
        return report.failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        logf("error: %s\n", e.what());
        return 2;
    }
}
