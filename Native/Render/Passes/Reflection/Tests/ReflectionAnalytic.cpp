// Reflection correctness against analytic answers (R track, ARCHITECTURE 2.6):
//  1. White furnace with a mirror floor half (roughness 0.05: M path) and a glossy half (0.3: G at grazing, K elsewhere):
//     the GI cache converges to the uniform L = Le / (1 - rho) (its rays shade hits diffusely), and a reflection hit on
//     a wall (roughness 0.5, f0 0.04) leaves Le + rho L + S(NoV_hit) L toward the floor, S = the model's specular
//     albedo (HitShading.hlsli, M's shSpecularAlbedo). The expected value of a pixel is that radiance averaged over its
//     floor lobe (256 VNDF samples, masked ones excluded as on the GPU, the walls intersected exactly); the G path's
//     control variate cancels in the uniform cache, so any deviation is a transport, shading or plumbing error.
//  2. A mirror ground under a constant sky L: M pixels see the sky, value L.
//  4. Reflection exact set (ARCHITECTURE 2.6): a mirror floor reflects a skinned emissive panel whose RT proxy (a
//     hand-made LOD cut of its even triangles, budget 100) has a hole in half of every cell. Once the panel's reflection
//     hits put it in the exact set it is traced with its original mesh: every reflected panel pixel reads its emission.
//  3. Textures at hits (INTERFACES v1.11): a mirror floor reflects a wall whose emission is modulated by a published
//     16 x 16 checker texture (black sky, black diffuse): an M pixel's value is the wall's emission x the texture's
//     bilinear value at the reflected point, computed on the CPU from the mirror direction.
// Frame path: RayScene::record -> ray-traced primary visibility standing in for V/M -> GI -> reflections; the check reads
// reflectionRadiance (M's API) and the per-pixel mode.
//
//   unx_test_reflection_reflectionanalytic [--frames N] [--set key=value ...] [--validate]
#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/gi/GiSystem.h"
#include "unx/refl/ReflectionSystem.h"
#include "unx/render/GpuScene.h"
#include "unx/rt/RayPipeline.h"
#include "unx/rt/RayScene.h"
#include "unx/scene/MaterialModel.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

using namespace unx;
using namespace unx::render;

namespace
{
void addQuad(scene::Mesh& m, float3 a, float3 b, float3 c, float3 d, float3 n)
{
    const uint32_t base = (uint32_t)m.positions.size();
    for (float3 p : { a, b, c, d })
    {
        m.positions.push_back(p);
        m.normals.push_back(n);
        m.uv0.push_back({ p.x, p.z });
    }
    m.indices.insert(m.indices.end(), { base, base + 1, base + 2, base, base + 2, base + 3 });
}

scene::Scene furnaceWithMirrors(float le, float rho)
{
    scene::Scene s;
    s.name = "refl_furnace";
    for (float r : { 0.5f, 0.05f, 0.3f })
    {
        scene::Material m;
        m.name = "wall";
        m.baseColor = { rho, rho, rho };
        m.emissive = { le, le, le };
        m.roughness = r;
        s.materials.push_back(m);
    }
    scene::Mesh room;
    room.name = "room";
    const float x0 = -6, x1 = 6, y0 = 0, y1 = 5, z0 = -6, z1 = 6;
    // Walls and ceiling facing inward (CCW seen from inside).
    addQuad(room, { x0, y1, z0 }, { x1, y1, z0 }, { x1, y1, z1 }, { x0, y1, z1 }, { 0, -1, 0 });
    addQuad(room, { x0, y0, z0 }, { x0, y1, z0 }, { x0, y1, z1 }, { x0, y0, z1 }, { 1, 0, 0 });
    addQuad(room, { x1, y0, z1 }, { x1, y1, z1 }, { x1, y1, z0 }, { x1, y0, z0 }, { -1, 0, 0 });
    addQuad(room, { x1, y0, z0 }, { x1, y1, z0 }, { x0, y1, z0 }, { x0, y0, z0 }, { 0, 0, 1 });
    addQuad(room, { x0, y0, z1 }, { x0, y1, z1 }, { x1, y1, z1 }, { x1, y0, z1 }, { 0, 0, -1 });
    room.submeshes.push_back({ 0, (uint32_t)room.indices.size(), 0 });
    const uint32_t floorStart = (uint32_t)room.indices.size();
    addQuad(room, { x0, y0, z1 }, { 0, y0, z1 }, { 0, y0, z0 }, { x0, y0, z0 }, { 0, 1, 0 });  // mirror half (x < 0)
    room.submeshes.push_back({ floorStart, 6, 1 });
    addQuad(room, { 0, y0, z1 }, { x1, y0, z1 }, { x1, y0, z0 }, { 0, y0, z0 }, { 0, 1, 0 });  // glossy half
    room.submeshes.push_back({ floorStart + 6, 6, 2 });
    s.meshes.push_back(room);
    s.instances.push_back({});
    s.sun.illuminance = 0;
    scene::Camera cam;
    cam.name = "inside";
    cam.position = { 0, 1.2f, 5.5f };
    cam.forward = normalize(float3{ 0, -0.25f, -1 });
    cam.up = normalize(cross(cross(cam.forward, float3{ 0, 1, 0 }), cam.forward));
    s.cameras.push_back(cam);
    return s;
}

scene::Scene mirrorUnderSky()
{
    scene::Scene s;
    s.name = "refl_sky_mirror";
    scene::Material m;
    m.name = "mirror";
    m.baseColor = { 0.9f, 0.9f, 0.9f };
    m.metallic = 1;
    m.roughness = 0.05f;
    s.materials.push_back(m);
    scene::Mesh plane;
    plane.name = "ground";
    addQuad(plane, { -200, 0, 200 }, { 200, 0, 200 }, { 200, 0, -200 }, { -200, 0, -200 }, { 0, 1, 0 });
    plane.submeshes.push_back({ 0, 6, 0 });
    s.meshes.push_back(plane);
    s.instances.push_back({});
    s.sun.illuminance = 0;
    scene::Camera cam;
    cam.name = "above";
    cam.position = { 0, 3, 0 };
    cam.forward = normalize(float3{ 0, -0.6f, -1 });
    cam.up = normalize(cross(cross(cam.forward, float3{ 0, 1, 0 }), cam.forward));
    s.cameras.push_back(cam);
    return s;
}

struct Buffer
{
    ComPtr<ID3D12Resource> resource;
};

Buffer createBuffer(Device& device, uint64_t bytes, D3D12_HEAP_TYPE type, bool uav)
{
    Buffer b;
    D3D12_HEAP_PROPERTIES heap{ type };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (uav) d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&b.resource)), "buffer");
    return b;
}

// HitShading.hlsli's indirect specular albedo: (f0 A + B)(1 + f0 (1 / (A + B) - 1)), bilinear on the 32 x 32 table.
double specularAlbedo(double f0, double NoV, double roughness)
{
    const std::vector<float>& t = scene::model::specularAlbedoTable();  // the frame constant's table (v1.25)
    const uint32_t n = scene::model::kAlbedoTableSize;
    const double x = std::clamp(NoV, 0.0, 1.0) * (n - 1), y = std::clamp(roughness, 0.0, 1.0) * (n - 1);
    const uint32_t x0 = (uint32_t)x, y0 = (uint32_t)y, x1 = std::min(x0 + 1, n - 1), y1 = std::min(y0 + 1, n - 1);
    const double fx = x - x0, fy = y - y0;
    double ab[2];
    for (int c = 0; c < 2; ++c)
    {
        const double a = t[2 * (y0 * n + x0) + c], b = t[2 * (y0 * n + x1) + c], cc = t[2 * (y1 * n + x0) + c], d = t[2 * (y1 * n + x1) + c];
        ab[c] = (a * (1 - fx) + b * fx) * (1 - fy) + (cc * (1 - fx) + d * fx) * fy;
    }
    return (f0 * ab[0] + ab[1]) * (1 + f0 * (1 / (ab[0] + ab[1]) - 1));
}

// ReflectionInternal.hlsli reflSampleGgx (Heitz 2018 visible normals), n = +Y here.
float3 sampleGgx(float3 v, float alpha, float u1, float u2)
{
    // Frame of n = (0, 1, 0) as the HLSL builds it (Duff et al. 2017 with n.z = 0 -> s = 1).
    const float3 n{ 0, 1, 0 };
    const float sgn = 1.0f, a = -1.0f / (sgn + n.z), c = n.x * n.y * a;
    const float3 t{ 1 + sgn * n.x * n.x * a, sgn * c, -sgn * n.x };
    const float3 b{ c, sgn + n.y * n.y * a, -n.y };
    const float3 ve{ dot(v, t), dot(v, b), dot(v, n) };
    const float3 vh = normalize(float3{ alpha * ve.x, alpha * ve.y, std::max(ve.z, 1e-4f) });
    const float lensq = vh.x * vh.x + vh.y * vh.y;
    const float3 t1 = lensq > 0 ? float3{ -vh.y, vh.x, 0 } / std::sqrt(lensq) : float3{ 1, 0, 0 };
    const float3 t2 = cross(vh, t1);
    const float r = std::sqrt(u1), phi = 6.28318530718f * u2;
    const float p1 = r * std::cos(phi);
    const float sv = 0.5f * (1 + vh.z);
    const float p2 = (1 - sv) * std::sqrt(std::max(0.0f, 1 - p1 * p1)) + sv * r * std::sin(phi);
    const float3 nh = t1 * p1 + t2 * p2 + vh * std::sqrt(std::max(0.0f, 1 - p1 * p1 - p2 * p2));
    const float3 he = normalize(float3{ alpha * nh.x, alpha * nh.y, std::max(0.0f, nh.z) });
    const float3 h = t * he.x + b * he.y + n * he.z;
    return h * (2 * dot(v, h)) - v;
}

// Expected reflection value of a pixel and the standard deviation of one lobe sample around it (a pixel's M value is one
// sample, a G value the mean of reflection.g_rays_per_sample: the per-pixel tolerance adds z sigma / sqrt(n), familyZ).
struct Expectation
{
    double mean = 0, sigma = 0;
    double hitNoV = 1;  // mean cosine at the reflection hits (diagnostics)
    float3 surface{};   // the pixel's surface point and roughness (diagnostics)
    double roughness = 0;
};

// Per-sample tolerance in sigmas for a check over 'samples' values that together may fail by chance with probability
// 0.1 % (Bonferroni, two-sided normal tail): z with samples x erfc(z / sqrt 2) = 1e-3 (129600 samples: z = 5.8). A fixed
// 4 sigma over ~39 k samples expects ~2.5 chance exceedances; bias is caught by the mean check (< 1 %) instead.
double familyZ(size_t samples)
{
    double lo = 0, hi = 10;
    for (int i = 0; i < 60; ++i)
    {
        const double z = 0.5 * (lo + hi);
        (samples * std::erfc(z / std::sqrt(2.0)) > 1e-3 ? lo : hi) = z;
    }
    return hi;
}

// Expected reflection value of a pixel in furnaceWithMirrors (see the file comment).
std::function<Expectation(uint32_t, uint32_t)> furnaceExpectation(const ViewDesc& view, uint32_t width, uint32_t height, double le, double rho)
{
    return [=](uint32_t px, uint32_t py) {
        const float4x4& m = view.invViewProj;
        auto unproject = [&](float z) {
            const float x = (px + 0.5f) / width * 2 - 1, y = 1 - (py + 0.5f) / height * 2;
            const float w = m.m[3][0] * x + m.m[3][1] * y + m.m[3][2] * z + m.m[3][3];
            return float3{ (m.m[0][0] * x + m.m[0][1] * y + m.m[0][2] * z + m.m[0][3]) / w, (m.m[1][0] * x + m.m[1][1] * y + m.m[1][2] * z + m.m[1][3]) / w,
                           (m.m[2][0] * x + m.m[2][1] * y + m.m[2][2] * z + m.m[2][3]) / w };
        };
        const float3 a = unproject(1.0f), b = unproject(0.5f);
        // The room's six faces, normals inward; the floor's halves are 0.05 (x < 0) and 0.3, everything else 0.5.
        struct Hit
        {
            float3 p, n;
            float roughness;
        };
        auto trace = [](float3 o, float3 d) {
            double tMin = 1e30;
            float3 n{};
            auto plane = [&](float dir, float origin, float bound, float3 normal) {
                if (dir == 0) return;
                const double t = (bound - origin) / dir;
                if (t > 1e-4 && t < tMin) tMin = t, n = normal;
            };
            plane(d.x, o.x, d.x > 0 ? 6.0f : -6.0f, float3{ d.x > 0 ? -1.0f : 1.0f, 0, 0 });
            plane(d.z, o.z, d.z > 0 ? 6.0f : -6.0f, float3{ 0, 0, d.z > 0 ? -1.0f : 1.0f });
            plane(d.y, o.y, d.y > 0 ? 5.0f : 0.0f, float3{ 0, d.y > 0 ? -1.0f : 1.0f, 0 });
            const float3 p = o + d * (float)tMin;
            return Hit{ p, n, n.y > 0.5f ? (p.x < 0 ? 0.05f : 0.3f) : 0.5f };
        };
        const Hit primary = trace(a, normalize(b - a));
        const float3 v = normalize(a - primary.p);
        // The sampler's frame is built for n = +Y; rotate the problem so the surface normal is +Y.
        const float3 n = primary.n;
        const float3 tAxis = std::fabs(n.y) > 0.5f ? float3{ 1, 0, 0 } : float3{ 0, 1, 0 };
        const float3 bx = normalize(cross(tAxis, n)), bz = cross(bx, n);  // world = bx * x + n * y + bz * z
        auto toLocal = [&](float3 w) { return float3{ dot(w, bx), dot(w, n), dot(w, bz) }; };
        auto toWorld = [&](float3 l) { return bx * l.x + n * l.y + bz * l.z; };
        const float alpha = std::max(primary.roughness * primary.roughness, 1e-4f);
        const double L = le / (1 - rho);
        double sum = 0, sumSq = 0, sumNoV = 0;
        uint32_t valid = 0;
        const uint32_t k = 16;
        for (uint32_t i = 0; i < k; ++i)
            for (uint32_t j = 0; j < k; ++j)
            {
                const float3 r = toWorld(sampleGgx(toLocal(v), alpha, (i + 0.5f) / k, (j + 0.5f) / k));
                if (dot(r, n) <= 0) continue;
                const Hit h = trace(primary.p, r);
                const double NoV = std::max(-dot(h.n, r), 1e-4f);
                const double x = le + rho * L + specularAlbedo(0.04, NoV, h.roughness) * L;
                sum += x;
                sumSq += x * x;
                sumNoV += NoV;
                ++valid;
            }
        if (!valid) return Expectation{ L, 0 };
        const double mean = sum / valid;
        return Expectation{ mean, std::sqrt(std::max(sumSq / valid - mean * mean, 0.0)), sumNoV / valid, primary.p, primary.roughness };
    };
}

// Scene 3: a mirror floor and an emissive back wall (z = -6, facing +z, uv 0..1 over x in [-6, 6], y in [0, 6]) whose
// emission (10) is multiplied by a checker texture; everything else black.
scene::Scene texturedWallInMirror()
{
    scene::Scene s;
    s.name = "refl_textured_wall";
    scene::Material floorMaterial;
    floorMaterial.name = "mirror";
    floorMaterial.baseColor = { 1, 1, 1 };
    floorMaterial.metallic = 1;
    floorMaterial.roughness = 0;
    s.materials.push_back(floorMaterial);
    scene::Material wall;
    wall.name = "textured emitter";
    wall.baseColor = { 0, 0, 0 };
    wall.emissive = { 10, 10, 10 };
    s.materials.push_back(wall);
    scene::Mesh floor;
    floor.name = "floor";
    addQuad(floor, { -40, 0, 20 }, { 40, 0, 20 }, { 40, 0, -6 }, { -40, 0, -6 }, { 0, 1, 0 });
    floor.submeshes.push_back({ 0, 6, 0 });
    s.meshes.push_back(floor);
    scene::Mesh back;
    back.name = "wall";
    for (auto [x, y] : { std::pair{ -6.f, 0.f }, { 6.f, 0.f }, { 6.f, 6.f }, { -6.f, 6.f } })
    {
        back.positions.push_back({ x, y, -6 });
        back.normals.push_back({ 0, 0, 1 });
        back.uv0.push_back({ (x + 6) / 12, y / 6 });
    }
    back.indices = { 0, 1, 2, 0, 2, 3 };
    back.submeshes.push_back({ 0, 6, 1 });
    s.meshes.push_back(back);
    s.instances.push_back({});
    scene::Instance w;
    w.mesh = 1;
    s.instances.push_back(w);
    s.sun.illuminance = 0;
    scene::Camera cam;
    cam.name = "low";
    cam.position = { 0.4f, 1.0f, 8.0f };
    cam.forward = normalize(float3{ 0, -0.12f, -1 });
    cam.up = normalize(cross(cross(cam.forward, float3{ 0, 1, 0 }), cam.forward));
    s.cameras.push_back(cam);
    return s;
}

// An n x n RGBA8 (UNORM, linear) texture on the GPU; returns its bindless SRV.
uint32_t createTexture(Device& device, const std::vector<uint8_t>& rgba, uint32_t n, ComPtr<ID3D12Resource>& texture)
{
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT }, up{ D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = n;
    d.Height = n;
    d.DepthOrArraySize = d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_COPY_DEST, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&texture)), "texture");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT64 bytes = 0;
    device.d3d()->GetCopyableFootprints1(&d, 0, 1, 0, &fp, nullptr, nullptr, &bytes);
    Buffer staging = createBuffer(device, bytes, D3D12_HEAP_TYPE_UPLOAD, false);
    uint8_t* mapped = nullptr;
    D3D12_RANGE none{ 0, 0 };
    check(staging.resource->Map(0, &none, reinterpret_cast<void**>(&mapped)), "map texture staging");
    for (uint32_t y = 0; y < n; ++y) std::memcpy(mapped + fp.Offset + y * fp.Footprint.RowPitch, rgba.data() + y * n * 4, n * 4);
    staging.resource->Unmap(0, nullptr);
    CommandList cl = device.acquireCommandList(QueueType::Graphics);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = texture.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.pResource = staging.resource.Get();
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

// Scene 4: mirror floor and a skinned emissive panel (8 x 8 cells, x in [-2, 2], y in [0.5, 4.5], z = -4, facing +z).
scene::Scene skinnedPanelInMirror()
{
    scene::Scene s;
    s.name = "refl_exact_set";
    scene::Material floorMaterial;
    floorMaterial.name = "mirror";
    floorMaterial.baseColor = { 1, 1, 1 };
    floorMaterial.metallic = 1;
    floorMaterial.roughness = 0;
    s.materials.push_back(floorMaterial);
    scene::Material panelMaterial;
    panelMaterial.name = "emitter";
    panelMaterial.baseColor = { 0, 0, 0 };
    panelMaterial.emissive = { 5, 5, 5 };
    s.materials.push_back(panelMaterial);
    scene::Mesh floor;
    floor.name = "floor";
    addQuad(floor, { -40, 0, 20 }, { 40, 0, 20 }, { 40, 0, -8 }, { -40, 0, -8 }, { 0, 1, 0 });
    floor.submeshes.push_back({ 0, 6, 0 });
    s.meshes.push_back(floor);
    scene::Mesh panel;
    panel.name = "panel";
    const int cells = 8;
    for (int j = 0; j <= cells; ++j)
        for (int i = 0; i <= cells; ++i)
        {
            panel.positions.push_back({ -2 + 4.0f * i / cells, 0.5f + 4.0f * j / cells, -4 });
            panel.normals.push_back({ 0, 0, 1 });
            panel.uv0.push_back({ (float)i / cells, (float)j / cells });
            panel.skin.joints.insert(panel.skin.joints.end(), { 0, 0, 0, 0 });
            panel.skin.weights.insert(panel.skin.weights.end(), { 1, 0, 0, 0 });
        }
    for (int j = 0; j < cells; ++j)
        for (int i = 0; i < cells; ++i)
        {
            const uint32_t a = j * (cells + 1) + i, b = a + 1, c = a + cells + 1, d = c + 1;
            panel.indices.insert(panel.indices.end(), { a, b, d, a, d, c });  // CCW seen from +z
        }
    panel.submeshes.push_back({ 0, (uint32_t)panel.indices.size(), 1 });
    panel.skin.inverseBind = { float3x4{} };
    s.meshes.push_back(panel);
    scene::Skeleton skeleton;
    skeleton.name = "panel";
    skeleton.jointToModel = { float3x4{} };
    s.skeletons.push_back(skeleton);
    s.instances.push_back({});
    scene::Instance p;
    p.mesh = 1;
    p.flags |= scene::InstanceSkinned | scene::InstanceDynamic;
    p.skeleton = 0;
    s.instances.push_back(p);
    s.sun.illuminance = 0;
    scene::Camera cam;
    cam.name = "mirror";
    cam.position = { 0.3f, 1.2f, 7.0f };
    cam.forward = normalize(float3{ 0, -0.2f, -1 });
    cam.up = normalize(cross(cross(cam.forward, float3{ 0, 1, 0 }), cam.forward));
    s.cameras.push_back(cam);
    return s;
}

// The panel's cuts (V's ClusterData format): the full mesh (error 0) and its even triangles (error 0.1).
ClusterData panelCuts(const scene::Scene& s)
{
    ClusterData cd;
    cd.meshes.resize(s.meshes.size());
    const scene::Mesh& m = s.meshes[1];
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
                cd.clusterTriangles.push_back(localOf(m.indices[3 * t]) | (localOf(m.indices[3 * t + 1]) << 8) | (localOf(m.indices[3 * t + 2]) << 16));
            }
            cd.clusterVertexIndices.insert(cd.clusterVertexIndices.end(), local.begin(), local.end());
            c.counts = (uint32_t)local.size() | ((uint32_t)(last - first) << 8);
            c.material = 1;
            cd.lodLevelClusters.push_back((uint32_t)cd.clusters.size());
            cd.clusters.push_back(c);
            level.triangleCount += (uint32_t)(last - first);
        }
        level.clusterCount = (uint32_t)cd.lodLevelClusters.size() - level.clusterOffset;
        cd.lodLevels.push_back(level);
    };
    cd.meshes[1].lodLevelOffset = 0;
    addCut(0.0f, 1);
    addCut(0.1f, 2);
    cd.meshes[1].lodLevelCount = 2;
    return cd;
}

// --scan-cache: after a run, every live GI cache entry against the furnace's uniform L = 2 (Passes/GI/Tests/GiCacheScan).
bool g_scanCache = false;

void scanCache(Device& device, ShaderLibrary& shaders, gi::GiSystem& gi, float L)
{
    constexpr uint64_t kBytes = 32 + 64 * 64;
    Buffer result = createBuffer(device, kBytes, D3D12_HEAP_TYPE_DEFAULT, true);
    Buffer readback = createBuffer(device, kBytes, D3D12_HEAP_TYPE_READBACK, false);
    RenderGraph g(device);
    const BufferRef cache = g.importBuffer(gi.cache(), { "scan cache", gi.cacheBytes(), 0 });
    const BufferRef out = g.importBuffer(result.resource.Get(), { "scan result", kBytes, 0 });
    g.addPass("test.scan.clear", QueueType::Graphics, [&](PassBuilder& b) { b.use(out, Use::CopyDst); },
              [out](PassContext& c) {
                  D3D12_WRITEBUFFERIMMEDIATE_PARAMETER p[5];
                  for (int i = 0; i < 5; ++i) p[i] = { c.resource(out)->GetGPUVirtualAddress() + 4 * i, 0 };
                  c.cmd->WriteBufferImmediate(5, p, nullptr);
              });
    const uint32_t capacity = gi.settings().capacity;
    g.addPass("test.scan", QueueType::Compute,
              [&](PassBuilder& b) {
                  b.use(cache, Use::SrvCompute);
                  b.use(out, Use::UavCompute);
                  b.keep();
              },
              [&shaders, cache, out, L, capacity](PassContext& c) {
                  const float tolerance = 0.02f;
                  uint32_t k[4] = { c.srv(cache), c.uav(out), 0, 0 };
                  std::memcpy(&k[2], &L, 4);
                  std::memcpy(&k[3], &tolerance, 4);
                  c.cmd->SetPipelineState(shaders.compute("Passes/GI/Tests/GiCacheScan"));
                  c.computeConstants(k, 4);
                  c.cmd->Dispatch((capacity + 63) / 64, 1, 1);
              });
    g.execute(nullptr);
    device.waitIdle();
    CommandList cl = device.acquireCommandList(QueueType::Graphics);
    cl.list->CopyBufferRegion(readback.resource.Get(), 0, result.resource.Get(), 0, kBytes);
    device.queue(QueueType::Graphics).waitCpu(device.submit(cl));
    std::vector<uint32_t> w(kBytes / 4);
    void* mapped = nullptr;
    D3D12_RANGE all{ 0, (SIZE_T)kBytes }, none{ 0, 0 };
    check(readback.resource->Map(0, &all, &mapped), "map scan");
    std::memcpy(w.data(), mapped, kBytes);
    readback.resource->Unmap(0, &none);
    logf("cache scan: %u live updated entries; off by > 2 %%: SH irradiance %u, texel mean %u, texel minimum %u (%u recorded)\n", w[0], w[1], w[2], w[3], w[4]);
    auto f = [&](uint32_t i) { float v; std::memcpy(&v, &w[i], 4); return v; };
    for (uint32_t r = 0; r < std::min(w[4], 64u); ++r)
    {
        const uint32_t a = 8 + r * 16;
        logf("  entry %u level %u class %u at (%.3f, %.3f, %.3f) n (%.2f, %.2f, %.2f): E %.4f, texel mean %.4f min %.4f, updates %u history %u, used %u updated %u\n",
             w[a + 11], w[a + 3] & 255, w[a + 3] >> 8, f(a), f(a + 1), f(a + 2), f(a + 12), f(a + 13), f(a + 14), f(a + 4), f(a + 5), f(a + 6), w[a + 7], w[a + 8], w[a + 9],
             w[a + 10]);
    }
}

struct Outcome
{
    uint32_t surface = 0, k = 0, mirror = 0, glossy = 0;
    double worstM = 0, worstG = 0, meanM = 0, meanG = 0;
    double excessM = -1, excessG = -1;  // largest deviation beyond the pixel's allowance (<= 0: all within)
    uint32_t exactOccupied = 0, exactSlots = 0;  // RayScene's reflection exact set after the last frame
    uint32_t outliersM = 0;
};

Outcome run(Device& device, ShaderLibrary& shaders, const QualityConfig& quality, const scene::Scene& s, float3 sky,
            const std::function<Expectation(uint32_t, uint32_t)>& expectedAt, uint32_t frames, uint32_t width, uint32_t height,
            const std::vector<gpu::MaterialTextures>* textures = nullptr, const ClusterData* clusters = nullptr)
{
    GpuScene gpuScene(device);
    gpuScene.upload(s);
    if (textures) gpuScene.setMaterialTextures(*textures);
    if (clusters) gpuScene.setClusters(*clusters);
    Outcome out;
    TrackState state;
    RenderGraph graph(device);
    const ViewDesc view = ViewDesc::fromCamera(s.cameras[0], width, height, float4x4{});
    Buffer constants = createBuffer(device, 1024, D3D12_HEAP_TYPE_UPLOAD, false);
    uint8_t* mapped = nullptr;
    D3D12_RANGE none{ 0, 0 };
    check(constants.resource->Map(0, &none, reinterpret_cast<void**>(&mapped)), "map constants");
    const uint32_t stride = 4, countX = width / stride, countY = height / stride;
    const uint64_t resultBytes = (uint64_t)countX * countY * 16;
    Buffer result = createBuffer(device, resultBytes, D3D12_HEAP_TYPE_DEFAULT, true);
    Buffer readback = createBuffer(device, resultBytes, D3D12_HEAP_TYPE_READBACK, false);
    for (uint32_t f = 0; f < frames; ++f)
    {
        FrameContext frame;
        frame.frameIndex = f;
        frame.time = f / 60.0;
        frame.deltaTime = 1 / 60.0f;
        frame.mainView = view;
        FrameResources resources;
        FrameServices services;
        auto frameConstantsFor = [&](const ViewDesc& v) {
            gpu::FrameConstants c{};
            c.viewProj = v.viewProj;
            c.prevViewProj = v.prevViewProj;
            c.invViewProj = v.invViewProj;
            c.view = v.view;
            c.proj = v.proj;
            c.cameraPosition = v.position;
            c.nearPlane = v.nearPlane;
            c.viewWidth = v.width;
            c.viewHeight = v.height;
            c.frameIndex = f;
            c.time = (float)frame.time;
            c.deltaTime = frame.deltaTime;
            c.exposure = 1.0f;
            c.tanHalfFovY = std::tan(v.verticalFov * 0.5f);
            c.sunDirection = s.sun.direction;
            c.sunIlluminance = s.sun.illuminance;
            c.sunColor = s.sun.color;
            c.sunAngularRadius = s.sun.angularRadius;
            gpuScene.fill(c);
            std::memcpy(mapped, &c, sizeof c);
            return constants.resource->GetGPUVirtualAddress();
        };
        FramePassContext fc{ device, graph, shaders, quality, gpuScene, frame, resources, services, frameConstantsFor, &state };
        ViewResources main;
        main.view = view;
        main.frameConstants = frameConstantsFor(view);
        main.depth = graph.createTexture({ "test depth", width, height, 1, 1, DXGI_FORMAT_R32_FLOAT });
        main.gbuffer = graph.createTexture({ "test gbuffer", width, height, 1, 1, DXGI_FORMAT_R32G32_UINT });
        rt::RayScene& rays = rt::RayScene::get(fc);
        out.exactSlots = rays.stats().exactSlots;
        out.exactOccupied = rays.stats().exactOccupied;
        rays.record(fc);
        uint32_t scene[8];
        rays.rootConstants(scene);
        rt::RayPipeline& primary = rt::RayPipeline::get(device, shaders, rt::standardRayPipeline("Passes/GI/Tests/GiTestPrimary", { "GiTestPrimaryGen" }));
        const TextureRef depth = main.depth, gbuffer = main.gbuffer;
        const D3D12_GPU_VIRTUAL_ADDRESS fcAddress = main.frameConstants;
        graph.addPass("test.primary", QueueType::Compute,
                      [&](PassBuilder& b) {
                          b.use(depth, Use::UavGraphics);
                          b.use(gbuffer, Use::UavGraphics);
                          rays.declareTraversal(b);
                      },
                      [&, depth, gbuffer, scene, fcAddress](PassContext& c) {
                          uint32_t k[32] = {};
                          k[0] = c.uav(depth);
                          k[1] = c.uav(gbuffer);
                          std::memcpy(&k[24], scene, sizeof scene);
                          c.computeConstants(k, 32);
                          c.bindFrameConstants(fcAddress);
                          primary.dispatch(c.cmd, 0, width, height, 1);
                      });
        gi::GiSystem& gi = gi::GiSystem::get(fc);
        gi.setConstantSky(sky, { 0, 0, 0 });
        gi.record(fc, main, rays);
        refl::ReflectionSystem& reflections = refl::ReflectionSystem::get(fc);
        reflections.setConstantSky(sky, { 0, 0, 0 });
        reflections.record(fc, main, rays);
        const TextureRef reflection = main.reflection, modes = reflections.modes();
        const BufferRef resultRef = graph.importBuffer(result.resource.Get(), { "test result", resultBytes, 16 });
        graph.addPass("test.eval", QueueType::Compute,
                      [&](PassBuilder& b) {
                          b.use(reflection, Use::SrvCompute);
                          b.use(depth, Use::SrvCompute);
                          b.use(modes, Use::SrvCompute);
                          b.use(resultRef, Use::UavCompute);
                          b.keep();
                      },
                      [&, reflection, depth, modes, resultRef](PassContext& c) {
                          const uint32_t k[8] = { c.srv(reflection), c.srv(depth), c.srv(modes), c.uav(resultRef), width, height, stride, 0 };
                          c.cmd->SetPipelineState(shaders.compute("Passes/Reflection/Tests/ReflTestEval"));
                          c.computeConstants(k, 8);
                          c.cmd->Dispatch((countX + 7) / 8, (countY + 7) / 8, 1);
                      });
        graph.execute(nullptr);
        device.queue(QueueType::Graphics).waitCpu(graph.lastFence(QueueType::Graphics));
    }
    if (g_scanCache)
    {
        g_scanCache = false;  // the furnace (first scene) only
        if (gi::GiSystem* gi = gi::GiSystem::find(state)) scanCache(device, shaders, *gi, 2.0f);
    }
    CommandList cl = device.acquireCommandList(QueueType::Graphics);
    cl.list->CopyBufferRegion(readback.resource.Get(), 0, result.resource.Get(), 0, resultBytes);
    device.queue(QueueType::Graphics).waitCpu(device.submit(cl));
    std::vector<float> values((size_t)countX * countY * 4);
    void* rb = nullptr;
    D3D12_RANGE all{ 0, (SIZE_T)resultBytes };
    check(readback.resource->Map(0, &all, &rb), "map result");
    std::memcpy(values.data(), rb, resultBytes);
    readback.resource->Unmap(0, &none);
    const uint32_t raysPerSample = (uint32_t)quality.integer("reflection.g_rays_per_sample");
    double sumM = 0, sumG = 0;
    const double z = familyZ(values.size() / 4);
    for (size_t i = 0; i < values.size() / 4; ++i)
    {
        const float w = values[4 * i + 3];
        if (w < 0) continue;
        ++out.surface;
        const double v = (values[4 * i] + values[4 * i + 1] + values[4 * i + 2]) / 3.0;
        if (w == 0)
        {
            ++out.k;
            continue;
        }
        const uint32_t px = (uint32_t)(i % countX) * stride, py = (uint32_t)(i / countX) * stride;
        const Expectation e = expectedAt(px, py);
        const double expected = e.mean;
        if (expected <= 1e-6) continue;  // no defined expectation here (scene 3: pixels outside the checked reflection)
        // Allowed relative deviation: 3 % plus z sigma of the pixel's sample mean (1 sample for M, g_rays_per_sample for G).
        const double allowM = 0.03 + z * e.sigma / expected, allowG = 0.03 + z * e.sigma / std::sqrt((double)raysPerSample) / expected;
        if (w == 1)
        {
            ++out.mirror;
            sumM += v / expected;
            out.worstM = std::max(out.worstM, std::fabs(v / expected - 1));
            out.excessM = std::max(out.excessM, std::fabs(v / expected - 1) - allowM);
            if (std::fabs(v / expected - 1) > allowM && out.outliersM++ < 6) logf("  M outlier at pixel (%u, %u): %.4f (expected %.4f, sigma %.4f, hit NoV %.3f; surface (%.3f, %.3f, %.3f) roughness %.2f)\n", px, py, v,
                                                                           expected, e.sigma, e.hitNoV, e.surface.x, e.surface.y, e.surface.z, e.roughness);
        }
        else
        {
            ++out.glossy;
            sumG += v / expected;
            out.excessG = std::max(out.excessG, std::fabs(v / expected - 1) - allowG);
            out.worstG = std::max(out.worstG, std::fabs(v / expected - 1));
        }
    }
    out.meanM = out.mirror ? sumM / out.mirror : 0;
    out.meanG = out.glossy ? sumG / out.glossy : 0;
    device.waitIdle();
    return out;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        uint32_t frames = 128;  // cold cache: cells only reflections reach converge in ~100 frames (48: 4 of 38962 M pixels still 4 % low)
        bool validate = false;
        std::vector<std::string> overrides;
        for (int i = 1; i < argc; ++i)
        {
            const std::string a = argv[i];
            if (a == "--frames" && i + 1 < argc) frames = (uint32_t)std::stoul(argv[++i]);
            else if (a == "--set" && i + 1 < argc) overrides.push_back(argv[++i]);
            else if (a == "--validate") validate = true;
            else if (a == "--scan-cache") g_scanCache = true;  // diagnostics: GI cache entries vs the furnace (first scene only)
            else fail("unknown argument %s", a.c_str());
        }
        QualityConfig quality = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
        for (const std::string& o : overrides) quality.applyOverride(o);
        DeviceOptions options;
        options.debugLayer = validate;
        options.gpuValidation = validate;
        Device device(options);
        ShaderLibrary shaders(device, executableDirectory() / "shaders");
        bool pass = true;

        const scene::Scene furnace = furnaceWithMirrors(1, 0.5f);
        const Outcome a = run(device, shaders, quality, furnace, { 0, 0, 0 },
                              furnaceExpectation(ViewDesc::fromCamera(furnace.cameras[0], 1920, 1080, float4x4{}), 1920, 1080, 1.0, 0.5), frames, 1920, 1080);
        const bool okA = a.mirror > 0 && a.glossy > 0 && std::fabs(a.meanM - 1) < 0.01 && std::fabs(a.meanG - 1) < 0.01 && a.excessM <= 0 && a.excessG <= 0;
        logf("furnace (value / expected; cache L = 2): %u surface samples: K %u, M %u (mean %.4f, worst %.2f %%, %u beyond 3 %% + z sigma), G %u (mean %.4f, worst %.2f %%) -> %s\n", a.surface, a.k,
             a.mirror, a.meanM, 100 * a.worstM, a.outliersM, a.glossy, a.meanG, 100 * a.worstG, okA ? "PASS" : "FAIL");
        pass = pass && okA;

        const Outcome b = run(device, shaders, quality, mirrorUnderSky(), { 1, 1, 1 }, [](uint32_t, uint32_t) { return Expectation{ 1.0, 0.0 }; }, frames, 1920, 1080);
        const bool okB = b.mirror > 0 && std::fabs(b.meanM - 1) < 0.01 && b.worstM < 0.03;
        logf("sky mirror (L = 1): %u surface samples: K %u, M %u (mean %.4f, worst %.2f %%), G %u -> %s\n", b.surface, b.k, b.mirror, b.meanM, 100 * b.worstM, b.glossy,
             okB ? "PASS" : "FAIL");
        pass = pass && okB;

        {
            const uint32_t n = 16;
            std::vector<uint8_t> rgba(n * n * 4);
            for (uint32_t y = 0; y < n; ++y)
                for (uint32_t x = 0; x < n; ++x)
                {
                    const uint8_t v = ((x / 2 + y / 2) & 1) ? 255 : 51;
                    uint8_t* px = &rgba[(y * n + x) * 4];
                    px[0] = v;
                    px[1] = (uint8_t)(v / 2);
                    px[2] = (uint8_t)(255 - v);
                    px[3] = 255;
                }
            ComPtr<ID3D12Resource> texture;
            const scene::Scene t = texturedWallInMirror();
            std::vector<gpu::MaterialTextures> published(t.materials.size());
            published[1].emissive = createTexture(device, rgba, n, texture);
            published[1].clamp = gpu::MaterialTextureEmissive;
            auto texel = [&](int x, int y, int c) { return rgba[(std::clamp(y, 0, (int)n - 1) * n + std::clamp(x, 0, (int)n - 1)) * 4 + c] / 255.0; };
            const ViewDesc tv = ViewDesc::fromCamera(t.cameras[0], 1920, 1080, float4x4{});
            // Expected: 10 x texture (clamp addressing, bilinear at level 0; the single-level texture has no other) at the
            // wall point the mirror direction reaches; channel average like the check; floor pixels whose reflection
            // misses the wall see the black sky.
            auto expected = [&, tv](uint32_t px, uint32_t py) {
                const float4x4& m = tv.invViewProj;
                auto unproject = [&](float z) {
                    const float x = (px + 0.5f) / 1920 * 2 - 1, y = 1 - (py + 0.5f) / 1080 * 2;
                    const float w = m.m[3][0] * x + m.m[3][1] * y + m.m[3][2] * z + m.m[3][3];
                    return float3{ (m.m[0][0] * x + m.m[0][1] * y + m.m[0][2] * z + m.m[0][3]) / w, (m.m[1][0] * x + m.m[1][1] * y + m.m[1][2] * z + m.m[1][3]) / w,
                                   (m.m[2][0] * x + m.m[2][1] * y + m.m[2][2] * z + m.m[2][3]) / w };
                };
                const float3 a = unproject(1.0f), b = unproject(0.5f), d = normalize(b - a);
                if (d.y >= 0) return Expectation{ 0, 0 };
                if (d.z < 0)  // the wall itself in front of the floor: not a mirror pixel of this check
                {
                    const float3 w = a + d * ((-6 - a.z) / d.z);
                    if (w.y >= 0 && std::fabs(w.x) <= 6) return Expectation{ 0, 0 };
                }
                const float3 p = a + d * (-a.y / d.y);
                const float3 r{ d.x, -d.y, d.z };
                if (r.z >= 0) return Expectation{ 0, 0 };
                const float tw = (-6 - p.z) / r.z;
                const float3 q = p + r * tw;
                // An M pixel is one GGX (VNDF) sample; GGX tails are heavy (P(tan theta_h > T) = a^2 / (a^2 + T^2)), so near
                // the wall's silhouette a correct sample can cross the edge. Reflections closer to the boundary than the
                // tail radius at probability 1e-5, 2 t a sqrt(1 / 1e-5 - 1) (a = 1e-4: 0.88 m at 14 m), are not judged.
                const float margin = 2 * tw * 1e-4f * std::sqrt(1 / 1e-5f - 1);
                if (q.x < -6 + margin || q.x > 6 - margin || q.y < margin || q.y > 6 - margin) return Expectation{ 0, 0 };
                const double fx = (q.x + 6) / 12 * n - 0.5, fy = q.y / 6 * n - 0.5;
                const int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
                const double wx = fx - x0, wy = fy - y0;
                double sum = 0;
                for (int c = 0; c < 3; ++c)
                    sum += (texel(x0, y0, c) * (1 - wx) + texel(x0 + 1, y0, c) * wx) * (1 - wy) + (texel(x0, y0 + 1, c) * (1 - wx) + texel(x0 + 1, y0 + 1, c) * wx) * wy;
                // Near a checker edge the texture varies within the ray's sub-pixel spread: allow the local variation.
                const double dxv = std::fabs(texel(x0 + 1, y0, 0) - texel(x0, y0, 0)) + std::fabs(texel(x0, y0 + 1, 0) - texel(x0, y0, 0));
                return Expectation{ 10 * sum / 3, 10 * dxv * 0.02 };
            };
            const Outcome c = run(device, shaders, quality, t, { 0, 0, 0 }, expected, 32, 1920, 1080, &published);
            const bool okC = c.mirror > 1000 && std::fabs(c.meanM - 1) < 0.01 && c.excessM <= 0;
            logf("textured wall in a mirror: %u M pixels on the wall's reflection (mean value / expected %.4f, worst %.2f %%, %u beyond 3 %% + z sigma) -> %s\n",
                 c.mirror, c.meanM, 100 * c.worstM, c.outliersM, okC ? "PASS" : "FAIL");
            pass = pass && okC;
            device.waitIdle();
            device.descriptors().freeResource(published[1].emissive);
        }
        {
            QualityConfig proxyQuality = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
            proxyQuality.applyOverride("raytracing.character_proxy_triangles=100");
            const scene::Scene t = skinnedPanelInMirror();
            const ClusterData cuts = panelCuts(t);
            const ViewDesc tv = ViewDesc::fromCamera(t.cameras[0], 1920, 1080, float4x4{});
            // Expected: the panel's emission (5) wherever the mirror direction meets the panel's rectangle, away from its
            // silhouette by the GGX tail radius (as scene 3); other pixels are not judged.
            auto expected = [tv](uint32_t px, uint32_t py) {
                const float4x4& m = tv.invViewProj;
                auto unproject = [&](float z) {
                    const float x = (px + 0.5f) / 1920 * 2 - 1, y = 1 - (py + 0.5f) / 1080 * 2;
                    const float w = m.m[3][0] * x + m.m[3][1] * y + m.m[3][2] * z + m.m[3][3];
                    return float3{ (m.m[0][0] * x + m.m[0][1] * y + m.m[0][2] * z + m.m[0][3]) / w, (m.m[1][0] * x + m.m[1][1] * y + m.m[1][2] * z + m.m[1][3]) / w,
                                   (m.m[2][0] * x + m.m[2][1] * y + m.m[2][2] * z + m.m[2][3]) / w };
                };
                const float3 a = unproject(1.0f), b = unproject(0.5f), d = normalize(b - a);
                if (d.y >= 0) return Expectation{ 0, 0 };
                if (d.z < 0)  // the panel itself in front of the floor
                {
                    const float3 w = a + d * ((-4 - a.z) / d.z);
                    if (w.y >= 0.5f && w.y <= 4.5f && std::fabs(w.x) <= 2) return Expectation{ 0, 0 };
                }
                const float3 p = a + d * (-a.y / d.y);
                const float3 r{ d.x, -d.y, d.z };
                if (r.z >= 0 || p.z < -4) return Expectation{ 0, 0 };
                const float tw = (-4 - p.z) / r.z;
                const float3 q = p + r * tw;
                const float margin = 2 * tw * 1e-4f * std::sqrt(1 / 1e-5f - 1);
                if (q.x < -2 + margin || q.x > 2 - margin || q.y < 0.5f + margin || q.y > 4.5f - margin) return Expectation{ 0, 0 };
                return Expectation{ 5, 0 };
            };
            const Outcome c = run(device, shaders, proxyQuality, t, { 0, 0, 0 }, expected, 16, 1920, 1080, nullptr, &cuts);
            // Control: without the exact set the proxy's holes must show (the check above can fail).
            QualityConfig noExact = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
            noExact.applyOverride("raytracing.character_proxy_triangles=100");
            noExact.applyOverride("raytracing.exact_set_max=0");
            const Outcome control = run(device, shaders, noExact, t, { 0, 0, 0 }, expected, 16, 1920, 1080, nullptr, &cuts);
            logf("exact set control (no exact set, proxy with holes): mean value / expected %.4f\n", control.meanM);
            const bool okD = c.exactSlots == 1 && c.exactOccupied == 1 && c.mirror > 1000 && std::fabs(c.meanM - 1) < 0.02 && c.excessM <= 0 && control.meanM < 0.8;
            logf("exact set: %u of %u slot(s) occupied; %u M pixels on the panel's reflection (mean value / expected %.4f, worst %.2f %%, %u beyond 3 %% + z sigma) -> %s\n",
                 c.exactOccupied, c.exactSlots, c.mirror, c.meanM, 100 * c.worstM, c.outliersM, okD ? "PASS" : "FAIL");
            pass = pass && okD;
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
