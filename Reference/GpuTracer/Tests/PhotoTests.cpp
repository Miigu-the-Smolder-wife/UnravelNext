// Photo mode on the GPU reference tracer (FEATURES_GAME 17; B11), against closed forms and against itself:
//   defocus   thin lens (aperture A, focus F) over an emitting half plane at depth D: every pixel is the box-filtered
//             fraction of a uniform disk of radius rho = (A / 2) |1 / D - 1 / F| f_px (pixels) on the bright side of
//             the edge (the lens maps onto the plane affinely, so the kernel is exactly that disk); in focus (F = D) the
//             edge stays a step;
//   lenslight the sun-caustic light tracer's connection through the lens (Caustic.hlsl) against camera paths alone
//             (both through the lens, the same expectation): column profiles across the caustic's edge, defocused;
//   progress  start / pass / current gives render()'s image for the same samples (up to float summation order), and
//             the halves' relMSE falls as 1 / samples;
//   exr       writeExr / readExr round trip, bit exact.
//   unx_test_reference_photo [case]
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/metrics/Metrics.h"
#include "unx/reference/GpuPathTracer.h"
#include "unx/reference/PathTracer.h"
#include "unx/render/Device.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <vector>

using namespace unx;

namespace
{
bool g_warp = false;  // --warp
constexpr double kPi = 3.14159265358979323846;
#define CHECK(c, ...) do { if (!(c)) fail(__VA_ARGS__); } while (0)

scene::Scene blankScene(const char* name)
{
    scene::Scene s;
    s.name = name;
    s.atmosphere.rayleighScattering = { 0, 0, 0 };
    s.atmosphere.mieScattering = { 0, 0, 0 };
    s.atmosphere.mieAbsorption = { 0, 0, 0 };
    s.atmosphere.ozoneAbsorption = { 0, 0, 0 };
    s.atmosphere.groundAlbedo = { 0, 0, 0 };
    s.sun.illuminance = 0;
    return s;
}
void addQuad(scene::Scene& s, float x0, float x1, float z0, float z1, float y, uint32_t material)
{
    scene::Mesh m;
    m.name = "quad" + std::to_string(s.meshes.size());
    m.positions = { { x0, y, z0 }, { x0, y, z1 }, { x1, y, z1 }, { x1, y, z0 } };
    m.normals = { { 0, 1, 0 }, { 0, 1, 0 }, { 0, 1, 0 }, { 0, 1, 0 } };
    m.uv0 = { { 0, 0 }, { 0, 1 }, { 1, 1 }, { 1, 0 } };
    m.indices = { 0, 1, 2, 0, 2, 3 };
    m.submeshes = { { 0, 6, material } };
    s.meshes.push_back(m);
    scene::Instance in;
    in.mesh = (uint32_t)s.meshes.size() - 1;
    s.instances.push_back(in);
}

// Emitting half plane x < 0 (1 nit), black half x >= 0, both at y = 0; camera 10 m above looking down (right = +x).
scene::Scene edgeScene()
{
    scene::Scene s = blankScene("photo_edge");
    scene::Material bright, dark;
    bright.name = "emitter";
    bright.baseColor = { 0, 0, 0 };
    bright.specular = 0;
    bright.emissive = { 1, 1, 1 };
    dark.name = "black";
    dark.baseColor = { 0, 0, 0 };
    dark.specular = 0;
    s.materials = { bright, dark };
    addQuad(s, -100, 0, -100, 100, 0, 0);
    addQuad(s, 0, 100, -100, 100, 0, 1);
    return s;
}
reference::ResolvedCamera downCamera(float height, float tanHalf)
{
    reference::ResolvedCamera c;
    c.position = { 0, height, 0 };
    c.forward = { 0, -1, 0 };
    c.up = { 0, 0, -1 };
    c.verticalFov = 2 * std::atan(tanHalf);
    c.ev100 = 0;  // exposure 1 / 1.2
    return c;
}
reference::RenderSettings settings(uint32_t w, uint32_t h, uint32_t spp)
{
    reference::RenderSettings rs;
    rs.width = w;
    rs.height = h;
    rs.samplesPerPixel = spp;
    rs.russianRouletteStart = 4;
    rs.seed = 0x6E5EED;
    return rs;
}
// Bright fraction of a uniform disk of radius rho centred t pixels on the bright side of a straight edge.
double diskFraction(double t, double rho)
{
    if (rho <= 0) return t > 0 ? 1 : (t < 0 ? 0 : 0.5);
    if (t >= rho) return 1;
    if (t <= -rho) return 0;
    return 0.5 + (t * std::sqrt(rho * rho - t * t) + rho * rho * std::asin(t / rho)) / (kPi * rho * rho);
}

void testDefocus()
{
    const scene::Scene s = edgeScene();
    const uint32_t W = 128, H = 8, spp = 4096;
    const float tanHalf = 0.02f, D = 10;
    const double fpx = (H / 2.0) / tanHalf;  // 200 px per unit tangent; the edge is at image x = 64
    const double exposure = 1.0 / 1.2;
    reference::GpuPathTracer gpt(s, UNX_SOURCE_DIR, "unx_test_reference_photo defocus");
    gpt.setWarp(g_warp);
    struct Case
    {
        float aperture, focus;
    };
    for (const Case c : { Case{ 0.5f, 5.0f }, Case{ 0.3f, 20.0f }, Case{ 0.5f, 10.0f } })
    {
        reference::ResolvedCamera cam = downCamera(D, tanHalf);
        cam.lensAperture = c.aperture;
        cam.lensFocus = c.focus;
        const reference::RenderOutput out = gpt.render(cam, settings(W, H, spp));
        CHECK(out.stats.nanSamples == 0 && out.stats.truncatedPaths == 0, "defocus: %llu NaN, %llu truncated", (unsigned long long)out.stats.nanSamples,
              (unsigned long long)out.stats.truncatedPaths);
        const double rho = 0.5 * c.aperture * std::abs(1.0 / D - 1.0 / c.focus) * fpx;
        double worstZ = 0, worstErr = 0;
        for (uint32_t x = 0; x < W; ++x)
        {
            double sum = 0;
            for (uint32_t y = 0; y < H; ++y) sum += out.image.pixel(x, y)[1];
            const double measured = sum / H / exposure;
            // box filter over the pixel: 4000-point midpoint rule of the disk fraction (smooth; error < 1e-7)
            double expected = 0;
            const int n = 4000;
            for (int k = 0; k < n; ++k) expected += diskFraction(64.0 - (x + (k + 0.5) / n), rho) / n;
            const double sigma = std::sqrt(std::max(expected * (1 - expected), 0.0) / (H * (double)spp)) + 1e-6;
            const double z = std::abs(measured - expected) / sigma;
            worstZ = std::max(worstZ, z);
            worstErr = std::max(worstErr, std::abs(measured - expected));
            CHECK(z <= 5, "defocus A %.2f F %.1f: column %u measured %.5f, expected %.5f (%.1f sigma)", c.aperture, c.focus, x, measured, expected, z);
        }
        logf("  defocus  A %.2f m, F %.1f m (disk radius %.2f px): 128 columns within %.2f sigma of the closed form (worst |err| %.4f)\n", c.aperture, c.focus, rho,
             worstZ, worstErr);
    }
}

// The sun-caustic scene of ReferenceTests (a 2 x 2 m mirror casting the sun onto an albedo-0.1 floor), seen wide enough
// to include the caustic's edge, through a defocused lens.
void testLensLightTracer()
{
    scene::Scene s = blankScene("photo_caustic");
    scene::Material floor, mirror;
    floor.name = "lambert";
    floor.baseColor = { 0.1f, 0.1f, 0.1f };
    floor.roughness = 1;
    floor.specular = 0;
    mirror.name = "mirror";
    mirror.baseColor = { 1, 1, 1 };
    mirror.metallic = 1;
    mirror.roughness = 0.03f;
    s.materials = { floor, mirror };
    addQuad(s, -2000, 2000, -2000, 2000, 0, 0);
    const double elev = 60.0 * kPi / 180;
    const float3 ws{ (float)std::cos(elev), (float)std::sin(elev), 0 };
    s.sun.direction = ws;
    s.sun.illuminance = 100000;
    const float3 M{ -3, 3, 0 }, r = normalize(float3{ 0, 0, 0 } - M), n = normalize(r + ws);
    const float3 t1{ 0, 0, 1 }, t2 = normalize(cross(n, t1));
    scene::Mesh mesh;
    mesh.name = "mirror";
    mesh.positions = { M - t1 - t2, M + t1 - t2, M + t1 + t2, M - t1 + t2 };
    mesh.normals = { n, n, n, n };
    mesh.uv0 = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    mesh.indices = { 0, 1, 2, 0, 2, 3 };
    if (dot(cross(mesh.positions[1] - mesh.positions[0], mesh.positions[2] - mesh.positions[0]), n) < 0) mesh.indices = { 0, 2, 1, 0, 3, 2 };
    mesh.submeshes = { { 0, 6, 1 } };
    s.meshes.push_back(mesh);
    scene::Instance in;
    in.mesh = 1;
    s.instances.push_back(in);
    CHECK(reference::hasSunCausticSurfaces(s), "lenslight: the mirror is not a sun-caustic surface");

    const uint32_t W = 48, H = 8;
    reference::GpuPathTracer gpt(s, UNX_SOURCE_DIR, "unx_test_reference_photo lenslight");
    gpt.setWarp(g_warp);
    // column means and their standard errors from the halves (8 pixels per column: 8 degrees of freedom)
    auto column = [&](const reference::RenderOutput& o, uint32_t x, double& mean, double& se) {
        double m = 0, v = 0;
        for (uint32_t y = 0; y < H; ++y)
        {
            const double ha = o.halfA.pixel(x, y)[1], hb = o.halfB.pixel(x, y)[1];
            m += 0.5 * (ha + hb);
            v += 0.25 * (ha - hb) * (ha - hb);  // variance estimate of the pixel mean (a + b) / 2
        }
        mean = m / H;
        se = std::sqrt(v) / H;
    };
    std::string failures;
    // the pinhole first (the estimators' agreement without a lens), then the lens (its light-tracer connection)
    for (const float aperture : { 0.0f, 0.4f })
    {
        reference::ResolvedCamera cam = downCamera(5, 0.1f);  // 6 m wide: the caustic's edges are inside
        cam.lensAperture = aperture;
        cam.lensFocus = aperture > 0 ? 2.5f : 0.0f;
        reference::RenderSettings lt = settings(W, H, 1024), cp = settings(W, H, 262144);
        cp.sunCaustics = false;
        cp.seed = 0x5A5A;
        const reference::RenderOutput a = gpt.render(cam, lt);
        const reference::RenderOutput b = gpt.render(cam, cp);
        double worst = 0, lo = 1e30, hi = 0;
        uint32_t beyond3 = 0;
        for (uint32_t x = 0; x < W; ++x)
        {
            double ma, sa, mb, sb;
            column(a, x, ma, sa);
            column(b, x, mb, sb);
            lo = std::min(lo, mb);
            hi = std::max(hi, mb);
            // floor: float accumulation (1e-5 relative) where the direct sun makes both nearly noise-free
            // allowance 3e-4 relative: light tracer and camera paths already differ by up to 1.3e-4 just outside the
            // caustic's edges with the pinhole (README 6, open item); the lens must not add to it
            const double z = std::abs(ma - mb) / std::sqrt(sa * sa + sb * sb + 1e-10 * mb * mb);
            const bool outside = std::abs(ma - mb) > 5 * std::sqrt(sa * sa + sb * sb) + 3e-4 * mb;
            worst = std::max(worst, z);
            beyond3 += z > 3 ? 1 : 0;
            if (z > 3)
                logf("    A %.1f column %2u: light tracer %.6f +- %.2g, camera paths %.6f +- %.2g (diff %+.3e rel, %.1f sigma)\n", aperture, x, ma, sa, mb, sb, (ma - mb) / mb, z);
            if (outside) failures += format(" A %.1f column %u (%.1f sigma, %.2e rel);", aperture, x, z, (ma - mb) / mb);
        }
        if (!(hi > 1.5 * lo)) failures += format(" A %.1f: the view does not cross the caustic's edge;", aperture);
        logf("  lenslight  A %.1f m: light tracer (1024 spp) vs camera paths (262144 spp), %u columns across the caustic edge (%.4g .. %.4g), worst %.2f sigma, "
             "%u beyond 3\n",
             aperture, W, lo, hi, worst, beyond3);
    }
    CHECK(failures.empty(), "lenslight:%s", failures.c_str());
}

void testProgressive()
{
    const scene::Scene s = edgeScene();
    reference::ResolvedCamera cam = downCamera(10, 0.02f);
    cam.lensAperture = 0.5f;
    cam.lensFocus = 5;
    reference::GpuPathTracer gpt(s, UNX_SOURCE_DIR, "unx_test_reference_photo progress");
    gpt.setWarp(g_warp);
    // WARP (about 15 ms per path in software): 16 x 8 pixels to 32 samples - the kernels, the sample chunks and the GPU
    // reduction; the noise ratio (a statistic of the whole image) is checked on the GPU size only
    const uint32_t W = g_warp ? 16 : 128, total = g_warp ? 32 : 256, early = total / 4;
    const reference::RenderOutput full = gpt.render(cam, settings(W, 8, total));
    gpt.start(cam, settings(W, 8, 4 * total));
    double relAt64 = 0;
    while (gpt.samplesDone() < total)
    {
        const uint32_t done = gpt.pass(8);
        if (done == early) relAt64 = gpt.current().halvesRelMse;
    }
    const reference::RenderOutput part = gpt.current();
    CHECK(gpt.samplesDone() == total, "progress: %u samples", gpt.samplesDone());
    // the GPU-reduced halves' relMSE (currentHalvesRelMse, updated with the image) = the read-back one up to the order
    // of the double additions
    gpt.currentImageResource();
    const double gpuRel = gpt.currentHalvesRelMse();
    CHECK(std::abs(gpuRel - part.halvesRelMse) <= 1e-12 * part.halvesRelMse, "progress: GPU halves' relMSE %.17g, read back %.17g", gpuRel,
          part.halvesRelMse);
    double worst = 0;
    for (size_t i = 0; i < full.image.rgb.size(); ++i) worst = std::max(worst, (double)std::abs(full.image.rgb[i] - part.image.rgb[i]));
    CHECK(worst <= 1e-5, "progress: %u samples in passes of 8 differ from render(%u) by %.3g", total, total, worst);
    // the halves' relMSE is the noise of a half: 1 / samples
    const double ratio = relAt64 / part.halvesRelMse;
    if (!g_warp) CHECK(ratio > 2.5 && ratio < 6.5, "progress: halves' relMSE 64 -> 256 samples fell by %.2f (expected ~4)", ratio);
    logf("  progress  passes of 8 samples reach render()'s image (worst %.1e); halves' relMSE %.3g at %u -> %.3g at %u samples (x%.2f); "
         "GPU-reduced %.3g (relative difference %.1e)\n",
         worst, relAt64, early, part.halvesRelMse, total, ratio, gpuRel, std::abs(gpuRel - part.halvesRelMse) / part.halvesRelMse);
}

// Shutter time integral: a 0.4 m emitting square (1 nit) moves 1 m along +x during the shutter over a black floor, seen
// from 10 m above (5 cm pixels). A pixel column's value is the time average of the square's overlap with the column's
// footprint (a trapezoid profile: box (8 px) convolved with the motion (20 px)); rows inside the square in z are used.
void testMotionBlur()
{
    scene::Scene s = blankScene("photo_motion");
    scene::Material emitter, black;
    emitter.name = "emitter";
    emitter.baseColor = { 0, 0, 0 };
    emitter.specular = 0;
    emitter.emissive = { 1, 1, 1 };
    black.name = "black";
    black.baseColor = { 0, 0, 0 };
    black.specular = 0;
    s.materials = { emitter, black };
    addQuad(s, -100, 100, -100, 100, 0, 1);    // floor (instance 0)
    addQuad(s, -0.2f, 0.2f, -0.2f, 0.2f, 0.01f, 0);  // the square (instance 1), centred at the origin
    float3x4 open, close;
    open.m[0][3] = -0.5f;
    close.m[0][3] = 0.5f;
    s.instances[1].transform = open;
    s.instances[1].flags |= scene::InstanceDynamic;
    const uint32_t W = 128, H = 16;
    const float tanHalf = 0.04f;  // 5 cm per pixel at 10 m (2 tanHalf / H x 10)
    reference::ResolvedCamera cam = downCamera(10, tanHalf);
    reference::ShutterMotion motion;
    motion.open = 0;
    motion.close = 1.0f / 330;
    motion.cameraPosition = cam.position;
    motion.cameraForward = cam.forward;
    motion.cameraUp = cam.up;
    motion.instances = { s.instances[0].transform, close };
    reference::GpuPathTracer gpt(s, UNX_SOURCE_DIR, "unx_test_reference_photo motion");
    gpt.setWarp(g_warp);
    gpt.setMotion(motion);
    reference::RenderSettings rs = settings(W, H, 4096);
    const reference::RenderOutput out = gpt.render(cam, rs);
    CHECK(out.stats.nanSamples == 0 && out.stats.truncatedPaths == 0, "motion: NaN or truncated paths");
    const double exposure = 1.0 / 1.2, pixel = 2.0 * tanHalf * 10 / H;  // 0.05 m
    double worstZ = 0, worstErr = 0, peak = 0;
    uint32_t bad = 0;
    for (uint32_t x = 0; x < W; ++x)
    {
        const double xa = (x - 64.0) * pixel, xb = xa + pixel;
        double expected = 0;
        const int n = 20000;
        for (int k = 0; k < n; ++k)
        {
            const double c = -0.5 + (k + 0.5) / n;
            expected += std::max(0.0, std::min(xb, c + 0.2) - std::max(xa, c - 0.2)) / pixel / n;
        }
        double m = 0, v = 0;
        for (uint32_t y = 5; y <= 10; ++y)
        {
            const double a = out.halfA.pixel(x, y)[1] / exposure, b = out.halfB.pixel(x, y)[1] / exposure;
            m += 0.5 * (a + b);
            v += 0.25 * (a - b) * (a - b);
        }
        m /= 6;
        const double se = std::sqrt(v) / 6 + 1e-4;
        const double z = std::abs(m - expected) / se;
        worstZ = std::max(worstZ, z);
        worstErr = std::max(worstErr, std::abs(m - expected));
        peak = std::max(peak, expected);
        if (std::abs(m - expected) > 5 * se + 3e-3)
        {
            ++bad;
            logf("    motion column %3u: measured %.5f expected %.5f (+- %.2g)\n", x, m, expected, se);
        }
    }
    CHECK(bad == 0, "motion: %u columns off the trapezoid", bad);
    CHECK(peak > 0.35 && peak < 0.45, "motion: the closed form's plateau %.3f (0.4 = 8 px square over a 20 px path)", peak);
    logf("  motion  square moving 20 px in the shutter: 128 columns match the trapezoid (worst |err| %.4f, %.2f sigma; plateau %.3f)\n", worstErr, worstZ, peak);
}

// Photo mode inside a game: the tracer on the caller's render::Device (render A request) gives the image of its own
// device (same estimator; float summation order aside), and returns every descriptor when destroyed.
void testSharedDevice()
{
    const scene::Scene s = edgeScene();
    reference::ResolvedCamera cam = downCamera(10, 0.02f);
    cam.lensAperture = 0.5f;
    cam.lensFocus = 5;
    reference::RenderOutput own;
    {
        reference::GpuPathTracer gpt(s, UNX_SOURCE_DIR, "unx_test_reference_photo shared (own device)");
        gpt.setWarp(g_warp);
        own = gpt.render(cam, settings(128, 8, 256));
    }
    render::DeviceOptions o;
    render::Device device(o);
    const uint32_t before = device.descriptors().resourcesInUse();
    reference::RenderOutput shared;
    uint32_t peak = 0, imageBits = 0;
    {
        reference::GpuPathTracer gpt(s, device, unx::executableDirectory() / "shaders" / "Reference", "unx_test_reference_photo shared");
        gpt.start(cam, settings(128, 8, 256));
        while (gpt.samplesDone() < 256) gpt.pass();
        shared = gpt.current();
        // the progressive image on the GPU (render A's display path) = the read-back image, bit for bit
        ID3D12Resource* image = gpt.currentImageResource();
        CHECK(image && gpt.currentImageRevision() == 256, "shared: no GPU image (revision %u)", gpt.currentImageRevision());
        const D3D12_RESOURCE_DESC d = image->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT rows = 0;
        UINT64 rowBytes = 0, total = 0;
        device.d3d()->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &rowBytes, &total);
        D3D12_HEAP_PROPERTIES hp{ D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        render::ComPtr<ID3D12Resource> rb;
        render::check(device.d3d()->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)), "readback");
        render::CommandList cl = device.acquireCommandList(render::QueueType::Compute);
        D3D12_TEXTURE_COPY_LOCATION dst{ rb.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }, src{ image, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        dst.PlacedFootprint = fp;
        cl.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        device.queue(render::QueueType::Compute).waitCpu(device.submit(cl));
        float* p = nullptr;
        render::check(rb->Map(0, nullptr, reinterpret_cast<void**>(&p)), "map readback");
        uint32_t differ = 0;
        for (uint32_t y = 0; y < 8; ++y)
            for (uint32_t x = 0; x < 128; ++x)
            {
                const float* g = reinterpret_cast<const float*>(reinterpret_cast<const uint8_t*>(p) + (size_t)y * fp.Footprint.RowPitch) + 4 * x;
                differ += std::memcmp(g, shared.image.pixel(x, y), 12) != 0 ? 1 : 0;
                imageBits += 1;
            }
        rb->Unmap(0, nullptr);
        CHECK(differ == 0, "shared: %u of %u GPU image pixels differ from current()", differ, imageBits);
        peak = device.descriptors().resourcesInUse();
        CHECK(peak > before, "shared: the tracer allocated no descriptors on the shared device");
    }
    const uint32_t after = device.descriptors().resourcesInUse();
    double worst = 0;
    for (size_t i = 0; i < own.image.rgb.size(); ++i) worst = std::max(worst, (double)std::abs(own.image.rgb[i] - shared.image.rgb[i]));
    CHECK(worst <= 1e-5, "shared: the image differs from the own-device render by %.3g", worst);
    CHECK(after == before, "shared: %u descriptors left on the device (%u before)", after, before);
    logf("  shared  the renderer's device: same image as the tracer's own device (worst %.1e), GPU image = read-back image (%u px bit exact), all %u descriptors returned\n", worst, imageBits, peak - before);
}

// WARP termination check (first run of kernel changes, before the GPU): 2 x 2 pixels, 2 samples, the full path depth
// and the sun-caustic light paths - every dispatch completes (a looping kernel never would) with error bits 0.
void testTiny()
{
    const scene::Scene s = edgeScene();
    reference::ResolvedCamera cam = downCamera(10, 0.02f);
    cam.lensAperture = 0.5f;
    cam.lensFocus = 5;
    reference::GpuPathTracer gpt(s, UNX_SOURCE_DIR, "unx_test_reference_photo tiny");
    gpt.setWarp(g_warp);
    const reference::RenderOutput out = gpt.render(cam, settings(2, 2, 2));
    const reference::GpuRenderInfo& info = gpt.info();
    CHECK(info.errors == 0, "tiny: kernel error bits 0x%x", info.errors);
    CHECK(out.stats.nanSamples == 0, "tiny: %llu NaN samples", (unsigned long long)out.stats.nanSamples);
    gpt.currentImageResource();
    const double gpuRel = gpt.currentHalvesRelMse(), rel = gpt.current().halvesRelMse;
    CHECK(std::abs(gpuRel - rel) <= 1e-12 * std::max(rel, 1e-300), "tiny: GPU halves' relMSE %.17g, read back %.17g", gpuRel, rel);
    logf("  tiny  2 x 2 px, 2 spp on %s: %u dispatches all completed (longest %.1f ms, %.1f s in all), error bits 0, %llu paths, %llu rays, "
         "%llu truncated\n",
         g_warp ? "WARP" : "the GPU", info.dispatches, info.longestDispatchMs, info.gpuSeconds, (unsigned long long)out.stats.paths,
         (unsigned long long)out.stats.rays, (unsigned long long)out.stats.truncatedPaths);
}

// The atmosphere's tau_top table built on the GPU (the photo-mode start) against the CPU model's entries: on WARP 16
// entries at the ground, middle and top rows, on the GPU the whole table (1024 x 2048).
void testAtmosphereTable()
{
    const scene::Scene s = blankScene("photo_atmosphere");
    reference::GpuPathTracer gpt(s, UNX_SOURCE_DIR, "unx_test_reference_photo atmtable");
    gpt.setWarp(g_warp);
    const uint32_t mu = 2048, rows = 1024;
    reference::GpuPathTracer::AtmosphereTableCheck all;
    for (uint32_t row : g_warp ? std::vector<uint32_t>{ 0, rows / 2, rows - 1 } : std::vector<uint32_t>{ 0 })
    {
        const reference::GpuPathTracer::AtmosphereTableCheck c = gpt.checkAtmosphereTable(row * mu + (g_warp ? 1000 : 0), g_warp ? 16 : rows * mu);
        all.entries += c.entries;
        all.differing += c.differing;
        all.maxRelative = std::max(all.maxRelative, c.maxRelative);
        all.seconds += c.seconds;
    }
    CHECK(all.maxRelative <= 1e-6, "atmtable: GPU entries differ from the CPU's by up to %.3g (relative)", all.maxRelative);
    logf("  atmtable  %u entries built on the %s in %.3f s: %u of %u channels differ from the CPU's bitwise, largest relative difference %.2e\n", all.entries,
         g_warp ? "WARP" : "GPU", all.seconds, all.differing, 3 * all.entries, all.maxRelative);
}

void testExr()
{
    metrics::Image im;
    im.width = 7;
    im.height = 3;
    for (uint32_t i = 0; i < 7 * 3 * 3; ++i) im.rgb.push_back(i == 5 ? 6.5e4f : (float)std::sin(1.3 * i) * 1e3f);
    const std::filesystem::path p = std::filesystem::temp_directory_path() / "unx_photo_test.exr";
    metrics::writeExr(p, im);
    const metrics::Image back = metrics::readExr(p);
    CHECK(back.width == 7 && back.height == 3 && std::memcmp(back.rgb.data(), im.rgb.data(), im.rgb.size() * 4) == 0, "exr: round trip differs");
    std::filesystem::remove(p);
    logf("  exr  7 x 3 float image written and read back bit exact\n");
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        const char* only = nullptr;
        for (int i = 1; i < argc; ++i)
            if (std::strcmp(argv[i], "--warp") == 0) g_warp = true;  // the tracer's kernels on WARP (no GPU): first runs
            else only = argv[i];
        auto run = [&](const char* name, void (*fn)()) {
            if (only && std::strcmp(only, name) != 0) return;
            logf("[%s]\n", name);
            fn();
        };
        run("exr", testExr);
        run("tiny", testTiny);
        run("atmtable", testAtmosphereTable);
        run("defocus", testDefocus);
        run("progress", testProgressive);
        run("lenslight", testLensLightTracer);
        run("motion", testMotionBlur);
        if (!g_warp) run("shared", testSharedDevice);  // the renderer's device is the GPU
        logf("photo mode tests passed\n");
        return 0;
    }
    catch (const std::exception& e)
    {
        logf("FAILED: %s\n", e.what());
        return 1;
    }
}
