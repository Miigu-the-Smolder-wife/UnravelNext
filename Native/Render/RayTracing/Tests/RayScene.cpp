// Correctness of the R track's ray scene (RayScene, RayPipeline, RayShaders.hlsli, Deform.hlsl): closest-hit t, hit
// identity (scene instance, mesh triangle), facing, surface normal and visibility rays against a CPU brute-force
// intersector, on a scene with rotated/scaled static instances, a dynamic rigid instance, a skinned (deformed) instance
// and a two-submesh mesh with an alpha-tested (untextured, hence opaque) submesh. Also refits the deformed BLASes and
// rebuilds the dynamic TLAS, then traces again.
//
//   unx_test_raytracing_rayscene [--rays N] [--validate]     --validate: debug layer + GPU-based validation
#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/render/GpuScene.h"
#include "unx/rt/RayPipeline.h"
#include "unx/rt/RayScene.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <random>

using namespace unx;
using namespace unx::render;

namespace
{
struct TestRay
{
    float3 origin;
    float tMax;
    float3 direction;
    float visibleTMax;
};
struct TestResult
{
    float t;
    uint32_t sceneInstance, meshTriangle, flags;
    float3 normal;
    float pad;
};

float3x4 transform(float3 axis, float angle, float scale, float3 t)
{
    axis = normalize(axis);
    const float c = std::cos(angle), s = std::sin(angle), k = 1 - c;
    float3x4 m;
    const float x = axis.x, y = axis.y, z = axis.z;
    const float r[3][3] = { { c + x * x * k, x * y * k - z * s, x * z * k + y * s }, { y * x * k + z * s, c + y * y * k, y * z * k - x * s }, { z * x * k - y * s, z * y * k + x * s, c + z * z * k } };
    for (int i = 0; i < 3; ++i)
    {
        for (int j = 0; j < 3; ++j) m.m[i][j] = r[i][j] * scale;
        m.m[i][3] = (&t.x)[i];
    }
    return m;
}

float3x4 compose(const float3x4& a, const float3x4& b)
{
    float3x4 r;
    for (int i = 0; i < 3; ++i)
        for (int j = 0; j < 4; ++j)
        {
            float s = j == 3 ? a.m[i][3] : 0.f;
            for (int k = 0; k < 3; ++k) s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    return r;
}

float3 applyVector(const float3x4& m, float3 v)
{
    return { m.m[0][0] * v.x + m.m[0][1] * v.y + m.m[0][2] * v.z, m.m[1][0] * v.x + m.m[1][1] * v.y + m.m[1][2] * v.z, m.m[2][0] * v.x + m.m[2][1] * v.y + m.m[2][2] * v.z };
}

void addBox(scene::Mesh& mesh, float3 lo, float3 hi)
{
    const float3 n[6] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    for (int f = 0; f < 6; ++f)
    {
        const float3 nn = n[f];
        const float3 u = std::fabs(nn.y) > 0.5f ? float3{ 1, 0, 0 } : float3{ 0, 1, 0 };
        const float3 v = cross(nn, u);
        const float3 c = (lo + hi) * 0.5f, h = (hi - lo) * 0.5f;
        auto corner = [&](float a, float b) {
            const float3 p = nn + u * a + v * b;
            return float3{ c.x + p.x * h.x, c.y + p.y * h.y, c.z + p.z * h.z };
        };
        const uint32_t base = (uint32_t)mesh.positions.size();
        for (auto [a, b] : { std::pair{ -1.f, -1.f }, { 1.f, -1.f }, { 1.f, 1.f }, { -1.f, 1.f } })
        {
            mesh.positions.push_back(corner(a, b));
            mesh.normals.push_back(nn);
            mesh.uv0.push_back({ a * 0.5f + 0.5f, b * 0.5f + 0.5f });
        }
        // CCW seen from outside: u x v = n.
        mesh.indices.insert(mesh.indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
    }
}

scene::Scene makeScene(uint32_t seed)
{
    scene::Scene s;
    s.name = "rt_test";
    s.materials.push_back({});
    scene::Material alpha;
    alpha.name = "alpha untextured";
    alpha.alphaCutoff = 0.5f;
    s.materials.push_back(alpha);

    scene::Mesh ground;
    ground.name = "ground";
    for (auto [x, z] : { std::pair{ -20.f, -20.f }, { 20.f, -20.f }, { 20.f, 20.f }, { -20.f, 20.f } })
    {
        ground.positions.push_back({ x, 0, z });
        ground.normals.push_back({ 0, 1, 0 });
        ground.uv0.push_back({ x, z });
    }
    ground.indices = { 0, 2, 1, 0, 3, 2 };  // CCW seen from +Y
    ground.submeshes.push_back({ 0, 6, 0 });
    s.meshes.push_back(ground);

    scene::Mesh box;
    box.name = "box";
    addBox(box, { -0.5f, -0.5f, -0.5f }, { 0.5f, 0.5f, 0.5f });
    box.submeshes.push_back({ 0, 36, 0 });
    s.meshes.push_back(box);

    scene::Mesh two;
    two.name = "two submeshes";
    addBox(two, { -0.5f, 0, -0.5f }, { 0.5f, 1, 0.5f });
    addBox(two, { -0.3f, 1.2f, -0.3f }, { 0.3f, 1.8f, 0.3f });
    two.submeshes.push_back({ 0, 36, 0 });
    two.submeshes.push_back({ 36, 36, 1 });
    s.meshes.push_back(two);

    // Skinned tube along +y (0..2), joints at y = 0 and y = 1.
    scene::Mesh tube;
    tube.name = "tube";
    const int rings = 21, sides = 12;
    for (int r = 0; r < rings; ++r)
    {
        const float y = 2.0f * r / (rings - 1);
        for (int k = 0; k < sides; ++k)
        {
            const float a = 6.2831853f * k / sides;
            tube.positions.push_back({ 0.25f * std::cos(a), y, 0.25f * std::sin(a) });
            tube.normals.push_back({ std::cos(a), 0, std::sin(a) });
            tube.uv0.push_back({ (float)k / sides, y });
            const float w1 = std::clamp((y - 0.7f) / 0.6f, 0.f, 1.f);
            tube.skin.joints.insert(tube.skin.joints.end(), { 0, 1, 0, 0 });
            tube.skin.weights.insert(tube.skin.weights.end(), { 1 - w1, w1, 0, 0 });
        }
    }
    for (int r = 0; r + 1 < rings; ++r)
        for (int k = 0; k < sides; ++k)
        {
            const uint32_t a = r * sides + k, b = r * sides + (k + 1) % sides, c = a + sides, d = b + sides;
            tube.indices.insert(tube.indices.end(), { a, c, b, b, c, d });  // CCW seen from outside
        }
    tube.submeshes.push_back({ 0, (uint32_t)tube.indices.size(), 0 });
    tube.skin.inverseBind = { float3x4{}, transform({ 0, 1, 0 }, 0, 1, { 0, -1, 0 }) };
    s.meshes.push_back(tube);
    scene::Skeleton skeleton;
    skeleton.name = "tube";
    skeleton.jointToModel = { float3x4{}, transform({ 0, 0, 1 }, 0.6f, 1, { 0, 1, 0 }) };
    s.skeletons.push_back(skeleton);

    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> U(0, 1);
    scene::Instance g;
    g.mesh = 0;
    s.instances.push_back(g);
    for (int i = 0; i < 40; ++i)
    {
        scene::Instance b;
        b.mesh = 1;
        b.transform = transform({ U(rng) - 0.5f, U(rng), U(rng) - 0.5f }, U(rng) * 6.28f, 0.5f + U(rng) * 1.5f, { (U(rng) - 0.5f) * 30, U(rng) * 4, (U(rng) - 0.5f) * 30 });
        s.instances.push_back(b);
    }
    scene::Instance dyn;
    dyn.mesh = 1;
    dyn.flags |= scene::InstanceDynamic;
    dyn.transform = transform({ 1, 1, 0 }, 0.7f, 2.0f, { 3, 2, 3 });
    s.instances.push_back(dyn);
    scene::Instance tw;
    tw.mesh = 2;
    tw.transform = transform({ 0, 1, 0 }, 0.3f, 1.5f, { -4, 0, -2 });
    s.instances.push_back(tw);
    scene::Instance tw2 = tw;  // override: both submeshes opaque material
    tw2.transform = transform({ 0, 1, 0 }, 1.1f, 1.0f, { -6, 0, 4 });
    tw2.materialOverrides = { 0, 0 };
    s.instances.push_back(tw2);
    scene::Instance sk;
    sk.mesh = 3;
    sk.flags |= scene::InstanceSkinned | scene::InstanceDynamic;
    sk.skeleton = 0;
    sk.transform = transform({ 0, 1, 0 }, 0.5f, 1.5f, { 1, 0, -5 });
    s.instances.push_back(sk);
    return s;
}

struct CpuTri
{
    float3 p0, p1, p2, n0, n1, n2;
    uint32_t instance, triangle;
};

// The GPU's skinning: unorm16 weights, jointToModel * inverseBind palette (GpuScene::upload, Deformation.hlsli).
std::vector<CpuTri> worldTriangles(const scene::Scene& s)
{
    std::vector<CpuTri> out;
    for (uint32_t i = 0; i < (uint32_t)s.instances.size(); ++i)
    {
        const scene::Instance& in = s.instances[i];
        const scene::Mesh& m = s.meshes[in.mesh];
        std::vector<float3> p = m.positions, n = m.normals;
        if (in.flags & scene::InstanceSkinned)
        {
            const scene::Skeleton& sk = s.skeletons[in.skeleton];
            for (size_t v = 0; v < p.size(); ++v)
            {
                float3 sp{}, sn{};
                for (int k = 0; k < 4; ++k)
                {
                    const float w = std::lround(std::clamp(m.skin.weights[4 * v + k], 0.f, 1.f) * 65535.f) / 65535.f;
                    const uint16_t j = m.skin.joints[4 * v + k];
                    const float3x4 pal = compose(sk.jointToModel[j], m.skin.inverseBind[j]);
                    sp = sp + pal.transformPoint(m.positions[v]) * w;
                    sn = sn + applyVector(pal, m.normals[v]) * w;
                }
                p[v] = sp;
                n[v] = normalize(sn);
            }
        }
        for (size_t v = 0; v < p.size(); ++v)
        {
            p[v] = in.transform.transformPoint(p[v]);
            n[v] = normalize(applyVector(in.transform, n[v]));
        }
        for (uint32_t t = 0; t < (uint32_t)m.indices.size() / 3; ++t)
        {
            const uint32_t a = m.indices[3 * t], b = m.indices[3 * t + 1], c = m.indices[3 * t + 2];
            out.push_back({ p[a], p[b], p[c], n[a], n[b], n[c], i, t });
        }
    }
    return out;
}

struct CpuHit
{
    float t = -1;
    uint32_t tri = 0;
    float u = 0, v = 0;
};

CpuHit intersect(const std::vector<CpuTri>& tris, float3 o, float3 d, float tMax)
{
    CpuHit best;
    best.t = tMax;
    bool any = false;
    for (uint32_t k = 0; k < (uint32_t)tris.size(); ++k)
    {
        const CpuTri& t = tris[k];
        const float3 e1 = t.p1 - t.p0, e2 = t.p2 - t.p0;
        const float3 pv = cross(d, e2);
        const float det = dot(e1, pv);
        if (std::fabs(det) < 1e-12f) continue;
        const float inv = 1.0f / det;
        const float3 tv = o - t.p0;
        const float u = dot(tv, pv) * inv;
        if (u < 0 || u > 1) continue;
        const float3 qv = cross(tv, e1);
        const float v = dot(d, qv) * inv;
        if (v < 0 || u + v > 1) continue;
        const float tt = dot(e2, qv) * inv;
        if (tt > 0 && tt < best.t)
        {
            best = { tt, k, u, v };
            any = true;
        }
    }
    if (!any) best.t = -1;
    return best;
}

struct GpuBuffer
{
    ComPtr<ID3D12Resource> resource;
    uint32_t view = 0;
};

GpuBuffer createBuffer(Device& device, uint64_t bytes, D3D12_HEAP_TYPE type, bool uav)
{
    GpuBuffer b;
    D3D12_HEAP_PROPERTIES heap{ type };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (uav) d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&b.resource)), "test buffer");
    return b;
}

std::vector<TestResult> trace(Device& device, ShaderLibrary& shaders, rt::RayScene& rs, D3D12_GPU_VIRTUAL_ADDRESS frameConstants, const std::vector<TestRay>& rays)
{
    const uint64_t rayBytes = rays.size() * sizeof(TestRay), resultBytes = rays.size() * sizeof(TestResult);
    GpuBuffer upload = createBuffer(device, rayBytes, D3D12_HEAP_TYPE_UPLOAD, false);
    void* mapped = nullptr;
    D3D12_RANGE none{ 0, 0 };
    check(upload.resource->Map(0, &none, &mapped), "map rays");
    std::memcpy(mapped, rays.data(), rayBytes);
    upload.resource->Unmap(0, nullptr);
    GpuBuffer rayBuf = createBuffer(device, rayBytes, D3D12_HEAP_TYPE_DEFAULT, false);
    GpuBuffer resBuf = createBuffer(device, resultBytes, D3D12_HEAP_TYPE_DEFAULT, true);
    GpuBuffer readback = createBuffer(device, resultBytes, D3D12_HEAP_TYPE_READBACK, false);
    DescriptorHeaps& h = device.descriptors();
    rayBuf.view = h.allocateResource();
    resBuf.view = h.allocateResource();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Buffer.NumElements = (UINT)rays.size();
    sd.Buffer.StructureByteStride = sizeof(TestRay);
    device.d3d()->CreateShaderResourceView(rayBuf.resource.Get(), &sd, h.resourceCpu(rayBuf.view));
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
    ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
    ud.Buffer.NumElements = (UINT)rays.size();
    ud.Buffer.StructureByteStride = sizeof(TestResult);
    device.d3d()->CreateUnorderedAccessView(resBuf.resource.Get(), nullptr, &ud, h.resourceCpu(resBuf.view));

    rt::RayPipeline& pipeline = rt::RayPipeline::get(device, shaders, rt::standardRayPipeline("RayTracing/Tests/TraceTest", { "TraceTestGen" }));
    CommandList cl = device.acquireCommandList(QueueType::Graphics);
    cl.list->CopyBufferRegion(rayBuf.resource.Get(), 0, upload.resource.Get(), 0, rayBytes);
    D3D12_GLOBAL_BARRIER g{ D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_SYNC_ALL_SHADING, D3D12_BARRIER_ACCESS_COPY_DEST, D3D12_BARRIER_ACCESS_SHADER_RESOURCE };
    D3D12_BARRIER_GROUP group{ D3D12_BARRIER_TYPE_GLOBAL, 1 };
    group.pGlobalBarriers = &g;
    cl.list->Barrier(1, &group);
    uint32_t constants[32] = {};
    constants[0] = rayBuf.view;
    constants[1] = resBuf.view;
    constants[2] = (uint32_t)rays.size();
    rs.rootConstants(&constants[24]);
    cl.list->SetComputeRoot32BitConstants(0, 32, constants, 0);
    cl.list->SetComputeRootConstantBufferView(1, frameConstants);
    pipeline.dispatch(cl.list.Get(), 0, (uint32_t)rays.size(), 1, 1);
    D3D12_GLOBAL_BARRIER g2{ D3D12_BARRIER_SYNC_ALL_SHADING | D3D12_BARRIER_SYNC_RAYTRACING, D3D12_BARRIER_SYNC_COPY, D3D12_BARRIER_ACCESS_UNORDERED_ACCESS, D3D12_BARRIER_ACCESS_COPY_SOURCE };
    group.pGlobalBarriers = &g2;
    cl.list->Barrier(1, &group);
    cl.list->CopyBufferRegion(readback.resource.Get(), 0, resBuf.resource.Get(), 0, resultBytes);
    device.queue(QueueType::Graphics).waitCpu(device.submit(cl));
    std::vector<TestResult> results(rays.size());
    D3D12_RANGE all{ 0, resultBytes };
    check(readback.resource->Map(0, &all, &mapped), "map results");
    std::memcpy(results.data(), mapped, resultBytes);
    readback.resource->Unmap(0, &none);
    h.freeResource(rayBuf.view);
    h.freeResource(resBuf.view);
    return results;
}

struct Check
{
    uint32_t hitMismatch = 0, tMismatch = 0, identityMismatch = 0, coincident = 0, faceMismatch = 0, normalMismatch = 0, visibilityMismatch = 0, hits = 0;
    uint32_t proxyHits = 0;
    double maxTError = 0;
};

Check compare(const scene::Scene& s, const std::vector<CpuTri>& tris, const std::vector<TestRay>& rays, const std::vector<TestResult>& gpu, const std::vector<CpuHit>& cpu,
              const std::vector<uint8_t>& expectVisible)
{
    Check c;
    (void)s;
    for (size_t i = 0; i < rays.size(); ++i)
    {
        const CpuHit& h = cpu[i];
        const TestResult& r = gpu[i];
        if ((h.t >= 0) != (r.t >= 0))
        {
            ++c.hitMismatch;
            continue;
        }
        if (rays[i].visibleTMax > 0 && (((r.flags >> 1) & 1u) != expectVisible[i])) ++c.visibilityMismatch;
        if (h.t < 0) continue;
        ++c.hits;
        const double tErr = std::fabs((double)h.t - r.t);
        c.maxTError = std::max(c.maxTError, tErr);
        // t is ill-conditioned at grazing incidence: float error in the plane distance grows as 1 / |cos theta|.
        const CpuTri& ht = tris[h.tri];
        const float cosTheta = std::fabs(dot(rays[i].direction, normalize(cross(ht.p1 - ht.p0, ht.p2 - ht.p0))));
        const double tol = 1e-4 * std::max(1.0, (double)h.t) / std::max(cosTheta, 1e-3f);
        if (tErr > tol)
        {
            if (c.tMismatch++ < 4)
                logf("  t mismatch ray %zu: cpu t %.6f (instance %u tri %u, u %.5f v %.5f) gpu t %.6f (instance %u tri %u)\n", i, h.t, tris[h.tri].instance, tris[h.tri].triangle, h.u, h.v, r.t,
                     r.sceneInstance, r.meshTriangle, cosTheta);
            continue;
        }
        const CpuTri& t = tris[h.tri];
        const bool proxy = (r.flags & 4u) != 0;  // RT proxy geometry: identity by instance only (the cut's own triangle order)
        if (proxy) ++c.proxyHits;
        if (t.instance != r.sceneInstance || (!proxy && t.triangle != r.meshTriangle))
        {
            ++c.coincident;  // same t within tolerance: shared edge or coplanar faces
            continue;
        }
        const float3 geo = cross(t.p1 - t.p0, t.p2 - t.p0);
        const bool front = dot(rays[i].direction, geo) < 0;
        if (front != ((r.flags & 1u) != 0)) ++c.faceMismatch;
        float3 n = normalize(t.n0 * (1 - h.u - h.v) + t.n1 * h.u + t.n2 * h.v);
        if (!front) n = -n;
        if (dot(n, r.normal) < 0.999f) ++c.normalMismatch;
    }
    return c;
}
} // namespace

// Alpha round (INTERFACES v1.11): a 4 x 4 m quad at z = 0 facing +z with uv 0..2 (wrap addressing exercised) and an
// alpha-tested material whose baseColor texture is a 16 x 16 random alpha pattern published through
// GpuScene::setMaterialTextures, in front of an opaque backstop at z = -1.
scene::Scene makeAlphaScene()
{
    scene::Scene s;
    s.name = "rt_alpha";
    s.materials.push_back({});
    scene::Material leaf;
    leaf.name = "alpha textured";
    leaf.alphaCutoff = 0.5f;
    s.materials.push_back(leaf);
    auto quad = [&](float z, uint32_t material) {
        scene::Mesh m;
        m.name = "quad";
        for (auto [x, y] : { std::pair{ -2.f, -2.f }, { 2.f, -2.f }, { 2.f, 2.f }, { -2.f, 2.f } })
        {
            m.positions.push_back({ x, y, z });
            m.normals.push_back({ 0, 0, 1 });
            m.uv0.push_back({ (x + 2) * 0.5f, (y + 2) * 0.5f });
        }
        m.indices = { 0, 1, 2, 0, 2, 3 };  // CCW seen from +z
        m.submeshes.push_back({ 0, 6, material });
        s.meshes.push_back(m);
        scene::Instance in;
        in.mesh = (uint32_t)s.meshes.size() - 1;
        s.instances.push_back(in);
    };
    quad(0, 1);
    quad(-1, 0);
    return s;
}

// The CPU replica of materialBaseColorLevel(m, uv, 0).a: bilinear on texel centres, wrap addressing, unorm8.
float alphaAt(const std::vector<uint8_t>& alpha, uint32_t n, float u, float v)
{
    const float x = u * n - 0.5f, y = v * n - 0.5f;
    const int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    const float fx = x - x0, fy = y - y0;
    const int ni = (int)n;
    auto at = [&](int i, int j) { return alpha[((j % ni + ni) % ni) * n + ((i % ni + ni) % ni)] / 255.0f; };
    return (at(x0, y0) * (1 - fx) + at(x0 + 1, y0) * fx) * (1 - fy) + (at(x0, y0 + 1) * (1 - fx) + at(x0 + 1, y0 + 1) * fx) * fy;
}

// An n x n RGBA8 texture on the GPU with the given alpha (rgb white); returns its bindless SRV.
uint32_t createAlphaTexture(Device& device, const std::vector<uint8_t>& alpha, uint32_t n, ComPtr<ID3D12Resource>& texture)
{
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT }, up{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = n;
    d.Height = n;
    d.DepthOrArraySize = d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_COPY_DEST, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&texture)), "alpha texture");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 bytes = 0;
    device.d3d()->GetCopyableFootprints1(&d, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
    D3D12_RESOURCE_DESC1 bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bytes;
    bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> staging;
    check(device.d3d()->CreateCommittedResource3(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&staging)), "alpha staging");
    uint8_t* mapped = nullptr;
    D3D12_RANGE none{ 0, 0 };
    check(staging->Map(0, &none, reinterpret_cast<void**>(&mapped)), "map alpha staging");
    for (uint32_t y = 0; y < n; ++y)
        for (uint32_t x = 0; x < n; ++x)
        {
            uint8_t* px = mapped + fp.Offset + y * fp.Footprint.RowPitch + x * 4;
            px[0] = px[1] = px[2] = 255;
            px[3] = alpha[y * n + x];
        }
    staging->Unmap(0, nullptr);
    CommandList cl = device.acquireCommandList(QueueType::Graphics);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst.SubresourceIndex = 0;
    src.pResource = staging.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint = fp;
    cl.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_TEXTURE_BARRIER tb{};
    tb.SyncBefore = D3D12_BARRIER_SYNC_COPY;
    tb.SyncAfter = D3D12_BARRIER_SYNC_NONE;
    tb.AccessBefore = D3D12_BARRIER_ACCESS_COPY_DEST;
    tb.AccessAfter = D3D12_BARRIER_ACCESS_NO_ACCESS;
    tb.LayoutBefore = D3D12_BARRIER_LAYOUT_COPY_DEST;
    tb.LayoutAfter = D3D12_BARRIER_LAYOUT_SHADER_RESOURCE;
    tb.pResource = texture.Get();
    tb.Subresources.IndexOrFirstMipLevel = 0xFFFFFFFFu;
    D3D12_BARRIER_GROUP group{ D3D12_BARRIER_TYPE_TEXTURE, 1 };
    group.pTextureBarriers = &tb;
    cl.list->Barrier(1, &group);
    device.queue(QueueType::Graphics).waitCpu(device.submit(cl));
    const uint32_t srv = device.descriptors().allocateResource();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.Texture2D.MipLevels = 1;
    device.d3d()->CreateShaderResourceView(texture.Get(), &sd, device.descriptors().resourceCpu(srv));
    return srv;
}

// Hand-made V cluster data for the proxy round: the skinned tube (mesh 3) gets two uniform-error cuts, the full mesh
// (error 0) and a coarser cut of its even triangles (error 0.1), in clusters of at most 64 triangles; every other mesh
// has none. With raytracing.character_proxy_triangles = 300, R must choose the 240-triangle cut.
ClusterData tubeCuts(const scene::Scene& s, uint32_t tubeMesh)
{
    ClusterData cd;
    cd.meshes.resize(s.meshes.size());
    const scene::Mesh& m = s.meshes[tubeMesh];
    auto addCut = [&](float error, uint32_t stride) {
        gpu::LodLevel level{};
        level.clusterOffset = (uint32_t)cd.lodLevelClusters.size();
        level.error = error;
        std::vector<uint32_t> tris;
        for (uint32_t t = 0; t < (uint32_t)m.indices.size() / 3; t += stride) tris.push_back(t);
        for (size_t first = 0; first < tris.size(); first += 64)
        {
            gpu::Cluster c{};
            c.vertexOffset = (uint32_t)cd.clusterVertexIndices.size();
            c.triangleOffset = (uint32_t)cd.clusterTriangles.size();
            std::vector<uint32_t> local;
            auto localOf = [&](uint32_t v) {
                for (uint32_t k = 0; k < (uint32_t)local.size(); ++k)
                    if (local[k] == v) return k;
                local.push_back(v);
                return (uint32_t)local.size() - 1;
            };
            const size_t last = std::min(first + 64, tris.size());
            for (size_t k = first; k < last; ++k)
            {
                const uint32_t t = tris[k];
                const uint32_t a = localOf(m.indices[3 * t]), b = localOf(m.indices[3 * t + 1]), cc = localOf(m.indices[3 * t + 2]);
                cd.clusterTriangles.push_back(a | (b << 8) | (cc << 16));
            }
            cd.clusterVertexIndices.insert(cd.clusterVertexIndices.end(), local.begin(), local.end());
            c.counts = (uint32_t)local.size() | ((uint32_t)(last - first) << 8) | (0u << 16);
            c.material = m.submeshes[0].material;
            cd.lodLevelClusters.push_back((uint32_t)cd.clusters.size());
            cd.clusters.push_back(c);
            level.triangleCount += (uint32_t)(last - first);
        }
        level.clusterCount = (uint32_t)cd.lodLevelClusters.size() - level.clusterOffset;
        cd.lodLevels.push_back(level);
    };
    cd.meshes[tubeMesh].lodLevelOffset = (uint32_t)cd.lodLevels.size();
    addCut(0.0f, 1);
    addCut(0.1f, 2);
    cd.meshes[tubeMesh].lodLevelCount = 2;
    return cd;
}

int main(int argc, char** argv)
{
    try
    {
        uint32_t rayCount = 200000;
        bool validate = false;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            if (a == "--rays" && i + 1 < argc) rayCount = (uint32_t)std::stoul(argv[++i]);
            else if (a == "--validate") validate = true;
            else fail("unknown argument %s", a.c_str());
        }
        QualityConfig quality = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
        DeviceOptions options;
        options.debugLayer = validate;
        options.gpuValidation = validate;
        Device device(options);
        ShaderLibrary shaders(device, executableDirectory() / "shaders");
        const scene::Scene s = makeScene(7);
        GpuScene gpuScene(device);
        gpuScene.upload(s);
        rt::RayScene& rs = rt::RayScene::get(device, shaders, gpuScene, quality);

        gpu::FrameConstants fc{};
        gpuScene.fill(fc);
        GpuBuffer constants = createBuffer(device, 1024, D3D12_HEAP_TYPE_UPLOAD, false);
        void* mapped = nullptr;
        D3D12_RANGE none{ 0, 0 };
        check(constants.resource->Map(0, &none, &mapped), "map constants");
        std::memcpy(mapped, &fc, sizeof fc);
        constants.resource->Unmap(0, nullptr);

        const std::vector<CpuTri> tris = worldTriangles(s);
        std::mt19937 rng(11);
        std::uniform_real_distribution<float> U(0, 1);
        std::vector<TestRay> rays(rayCount);
        std::vector<CpuHit> cpu(rayCount);
        std::vector<uint8_t> expectVisible(rayCount, 0);
        for (uint32_t i = 0; i < rayCount; ++i)
        {
            TestRay& r = rays[i];
            r.origin = { (U(rng) - 0.5f) * 30, 0.05f + U(rng) * 6, (U(rng) - 0.5f) * 30 };
            float3 d;
            do d = { U(rng) * 2 - 1, U(rng) * 2 - 1, U(rng) * 2 - 1 };
            while (dot(d, d) > 1 || dot(d, d) < 1e-4f);
            r.direction = normalize(d);
            r.tMax = 100;
            cpu[i] = intersect(tris, r.origin, r.direction, r.tMax);
            // Visibility: short of the hit (visible), past it (blocked), or the full ray on a miss (visible).
            if (cpu[i].t >= 0)
            {
                const bool before = (i & 1) != 0;
                if (cpu[i].t > 0.02f)
                {
                    r.visibleTMax = before ? cpu[i].t * 0.995f : cpu[i].t * 1.005f + 1e-3f;
                    expectVisible[i] = before ? 1 : 0;
                }
            }
            else
            {
                r.visibleTMax = r.tMax;
                expectVisible[i] = 1;
            }
        }

        bool pass = true;
        for (int round = 0; round < 2; ++round)
        {
            if (round == 1)
            {
                // Refit the deformed BLASes (same pose) and rebuild the dynamic TLAS; results must not change.
                CommandList cl = device.acquireCommandList(QueueType::Graphics);
                cl.list->SetComputeRootConstantBufferView(1, constants.resource->GetGPUVirtualAddress());
                rs.updateDynamic(cl.list.Get(), true);
                device.queue(QueueType::Graphics).waitCpu(device.submit(cl));
            }
            const std::vector<TestResult> gpu = trace(device, shaders, rs, constants.resource->GetGPUVirtualAddress(), rays);
            const Check c = compare(s, tris, rays, gpu, cpu, expectVisible);
            // Ties: the same t (within the conditioned tolerance) on a different triangle. They occur where a ray passes within
            // ~1e-4 of the diagonal edge shared by a box face's two triangles or of a contact between instances, and the
            // watertight hardware test and the CPU test pick different sides; allow 0.05 % of hits (seen: 12 of 94981).
            const bool ok = c.hitMismatch == 0 && c.tMismatch == 0 && c.faceMismatch == 0 && c.normalMismatch == 0 && c.visibilityMismatch == 0 &&
                            c.coincident <= std::max<uint32_t>(2, c.hits / 2000);
            logf("%s: %u rays, %u hits; hit/miss mismatches %u, t mismatches %u (max |dt| %.2e), identity ties %u, facing %u, normal %u, visibility %u -> %s\n",
                 round == 0 ? "build" : "refit", rayCount, c.hits, c.hitMismatch, c.tMismatch, c.maxTError, c.coincident, c.faceMismatch, c.normalMismatch, c.visibilityMismatch,
                 ok ? "PASS" : "FAIL");
            pass = pass && ok;
        }
        {
            // Round 3 (INTERFACES 6.3, v1.8): hide one static, one dynamic rigid and one skinned instance and record a frame
            // through RayScene::record (per-frame descriptors, in-frame static TLAS rebuild): the rays must match the CPU
            // reference without them, and no ray may hit a hidden instance.
            std::vector<uint32_t> hidden;
            auto pick = [&](auto match) {
                for (uint32_t i = 1; i < (uint32_t)s.instances.size(); ++i)
                    if (match(s.instances[i].flags))
                    {
                        hidden.push_back(i);
                        return;
                    }
            };
            pick([](uint32_t f) { return (f & (scene::InstanceDynamic | scene::InstanceSkinned)) == 0; });
            pick([](uint32_t f) { return (f & scene::InstanceDynamic) != 0 && (f & scene::InstanceSkinned) == 0; });
            pick([](uint32_t f) { return (f & scene::InstanceSkinned) != 0; });
            for (uint32_t i : hidden) gpuScene.setInstanceVisible(i, false);
            RenderGraph graph(device);
            FrameContext frame;
            frame.frameIndex = 1;
            FrameResources resources;
            FrameServices services;
            TrackState state;
            FramePassContext fctx{ device, graph, shaders, quality, gpuScene, frame, resources, services,
                                   [&](const ViewDesc&) { return constants.resource->GetGPUVirtualAddress(); }, &state };
            rs.record(fctx);
            graph.execute(nullptr);
            device.queue(QueueType::Graphics).waitCpu(graph.lastFence(QueueType::Graphics));
            std::vector<CpuTri> shown;
            for (const CpuTri& t : tris)
                if (std::find(hidden.begin(), hidden.end(), t.instance) == hidden.end()) shown.push_back(t);
            std::vector<CpuHit> cpuShown(rayCount);
            std::vector<uint8_t> visibleShown(rayCount, 0);
            for (uint32_t i = 0; i < rayCount; ++i)
            {
                cpuShown[i] = intersect(shown, rays[i].origin, rays[i].direction, rays[i].tMax);
                if (rays[i].visibleTMax > 0) visibleShown[i] = intersect(shown, rays[i].origin, rays[i].direction, rays[i].visibleTMax).t < 0 ? 1 : 0;
            }
            const std::vector<TestResult> gpu = trace(device, shaders, rs, constants.resource->GetGPUVirtualAddress(), rays);
            uint32_t hiddenHits = 0;
            for (const TestResult& r : gpu)
                if (r.t >= 0 && std::find(hidden.begin(), hidden.end(), r.sceneInstance) != hidden.end()) ++hiddenHits;
            const Check c = compare(s, shown, rays, gpu, cpuShown, visibleShown);
            const bool ok = hidden.size() == 3 && hiddenHits == 0 && c.hitMismatch == 0 && c.tMismatch == 0 && c.faceMismatch == 0 && c.normalMismatch == 0 &&
                            c.visibilityMismatch == 0 && c.coincident <= std::max<uint32_t>(2, c.hits / 2000);
            logf("hidden (instances %u %u %u): %u hits; hits on hidden instances %u, hit/miss mismatches %u, t mismatches %u, identity ties %u, facing %u, normal %u, "
                 "visibility %u -> %s\n",
                 hidden.size() > 0 ? hidden[0] : 0, hidden.size() > 1 ? hidden[1] : 0, hidden.size() > 2 ? hidden[2] : 0, c.hits, hiddenHits, c.hitMismatch, c.tMismatch,
                 c.coincident, c.faceMismatch, c.normalMismatch, c.visibilityMismatch, ok ? "PASS" : "FAIL");
            pass = pass && ok;
        }
        {
            // Round 5 (ARCHITECTURE 2.8): the skinned tube traced through a reduced RT proxy taken from V's LOD cuts. The
            // CPU reference holds exactly the cut's triangles (the even ones of the tube); proxy hits are identified by
            // instance (the cut has its own triangle order), everything else as in round 1.
            QualityConfig proxyQuality = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
            proxyQuality.applyOverride("raytracing.character_proxy_triangles=300");
            proxyQuality.applyOverride("raytracing.skinned_proxy_cuts=\"cluster_lod\"");  // this round tests V's cut as the proxy
            const uint32_t tubeMesh = 3;
            GpuScene proxyScene(device);
            proxyScene.upload(s);
            proxyScene.setClusters(tubeCuts(s, tubeMesh));
            rt::RayScene& prs = rt::RayScene::get(device, shaders, proxyScene, proxyQuality);
            gpu::FrameConstants pfc{};
            proxyScene.fill(pfc);
            GpuBuffer pconstants = createBuffer(device, 1024, D3D12_HEAP_TYPE_UPLOAD, false);
            check(pconstants.resource->Map(0, &none, &mapped), "map proxy constants");
            std::memcpy(mapped, &pfc, sizeof pfc);
            pconstants.resource->Unmap(0, nullptr);
            std::vector<CpuTri> cut;
            for (const CpuTri& t : tris)
                if (s.instances[t.instance].mesh != tubeMesh || (t.triangle % 2) == 0) cut.push_back(t);
            std::vector<CpuHit> cpuCut(rayCount);
            std::vector<uint8_t> visibleCut(rayCount, 0);
            for (uint32_t i = 0; i < rayCount; ++i)
            {
                cpuCut[i] = intersect(cut, rays[i].origin, rays[i].direction, rays[i].tMax);
                if (rays[i].visibleTMax > 0) visibleCut[i] = intersect(cut, rays[i].origin, rays[i].direction, rays[i].visibleTMax).t < 0 ? 1 : 0;
            }
            const std::vector<TestResult> got = trace(device, shaders, prs, pconstants.resource->GetGPUVirtualAddress(), rays);
            const Check c = compare(s, cut, rays, got, cpuCut, visibleCut);
            const rt::RaySceneStats& st = prs.stats();
            const bool ok = st.deformedTriangles == 240 && c.proxyHits > 0 && c.hitMismatch == 0 && c.tMismatch == 0 && c.faceMismatch == 0 &&
                            c.normalMismatch == 0 && c.visibilityMismatch == 0 && c.coincident <= std::max<uint32_t>(2, c.hits / 2000);
            logf("proxy cut: %u deformed triangles (cut of 240 expected), %u hits (%u on the proxy); hit/miss mismatches %u, t mismatches %u, identity ties %u, "
                 "facing %u, normal %u, visibility %u -> %s\n",
                 st.deformedTriangles, c.hits, c.proxyHits, c.hitMismatch, c.tMismatch, c.coincident, c.faceMismatch, c.normalMismatch, c.visibilityMismatch,
                 ok ? "PASS" : "FAIL");
            pass = pass && ok;
            rt::RayScene::release(device, proxyScene);
        }
        {
            // Round 4: the any-hit alpha test reads M's published texture like V's raster (INTERFACES v1.11).
            const uint32_t n = 16;
            std::vector<uint8_t> alpha(n * n);
            std::mt19937 arng(5);
            for (uint8_t& a : alpha) a = (arng() & 1) ? 255 : 0;
            const scene::Scene as = makeAlphaScene();
            GpuScene alphaScene(device);
            alphaScene.upload(as);
            ComPtr<ID3D12Resource> texture;
            std::vector<gpu::MaterialTextures> published(as.materials.size());
            published[1].baseColor = createAlphaTexture(device, alpha, n, texture);
            alphaScene.setMaterialTextures(published);
            rt::RayScene& ars = rt::RayScene::get(device, shaders, alphaScene, quality);
            gpu::FrameConstants afc{};
            alphaScene.fill(afc);
            GpuBuffer aconstants = createBuffer(device, 1024, D3D12_HEAP_TYPE_UPLOAD, false);
            check(aconstants.resource->Map(0, &none, &mapped), "map alpha constants");
            std::memcpy(mapped, &afc, sizeof afc);
            aconstants.resource->Unmap(0, nullptr);
            const uint32_t grid = 512;
            std::vector<TestRay> arays(grid * grid);
            std::vector<int> expect(grid * grid);  // 1 = quad, 0 = backstop, -1 = within 2/255 of the cutoff (not judged)
            for (uint32_t j = 0; j < grid; ++j)
                for (uint32_t i = 0; i < grid; ++i)
                {
                    TestRay& r = arays[j * grid + i];
                    const float x = -1.99f + 3.98f * (i + 0.5f) / grid, y = -1.99f + 3.98f * (j + 0.5f) / grid;
                    r.origin = { x, y, 5 };
                    r.direction = { 0, 0, -1 };
                    r.tMax = 100;
                    r.visibleTMax = 0;
                    const float a = alphaAt(alpha, n, (x + 2) * 0.5f, (y + 2) * 0.5f);
                    expect[j * grid + i] = std::fabs(a - 0.5f) < 2.0f / 255 ? -1 : (a >= 0.5f ? 1 : 0);
                }
            const std::vector<TestResult> got = trace(device, shaders, ars, aconstants.resource->GetGPUVirtualAddress(), arays);
            uint32_t judged = 0, wrong = 0, cut = 0;
            for (size_t k = 0; k < arays.size(); ++k)
            {
                if (expect[k] < 0) continue;
                ++judged;
                const bool quadHit = got[k].t >= 0 && std::fabs(got[k].t - 5) < 1e-3f;
                const bool backstop = got[k].t >= 0 && std::fabs(got[k].t - 6) < 1e-3f;
                if (expect[k] == 1 ? !quadHit : !backstop) ++wrong;
                if (expect[k] == 0) ++cut;
            }
            const bool ok = wrong == 0 && cut > judged / 4 && cut < judged * 3 / 4;
            logf("alpha texture: %u rays judged (%u cut through the quad), %u disagree with the CPU alpha test -> %s\n", judged, cut, wrong, ok ? "PASS" : "FAIL");
            pass = pass && ok;
            rt::RayScene::release(device, alphaScene);
            device.descriptors().freeResource(published[1].baseColor);
        }
        rt::RayPipeline::releaseDevice(device);
        rt::RayScene::releaseDevice(device);
        device.waitIdle();
        const uint32_t errors = device.drainDebugMessages();
        if (validate) logf("debug layer + GPU-based validation errors: %u\n", errors);
        pass = pass && errors == 0;
        logf("RESULT %s\n", pass ? "PASS" : "FAIL");
        return pass ? 0 : 1;
    }
    catch (const std::exception& e)
    {
        logf("error: %s\n", e.what());
        return 2;
    }
}
