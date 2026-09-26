// A5 depth of field against the CPU thin-lens path tracer (render C, 2026-09-27; FEATURES_GAME 4.1 gate (ii) on the CPU
// reference while the GPU tracer runs short jobs only). The lens operator alone is judged: the renderer's DoF passes run on
// the reference's own pinhole image (box-filtered pixels) and pixel-centre depths, and the result is compared with the
// reference's thin-lens render of the same scene (lens sampled per path). The scene is emissive only (black base colour,
// textured emission, no sun, no scattering medium), so a path's radiance is the emission of the first surface it meets:
// no shading term differs between the two paths, only the lens integral. Scene (the aiming case's layers): a textured
// backdrop at 40 m, a textured target panel at 20 m, a near "weapon" plate at 0.6 m (lower right), camera 24 mm.
//   aim         focus 20 m, the weapon at rho = -15 px (the target in focus, the backdrop nearly);
//   background  focus 1.5 m, the backdrop at rho = +10 px, the target at +9.6 px, the weapon near focus.
// Per pixel class (the pinhole depth's rho; "edge" = within max |rho| + 2 px of a depth discontinuity, where hidden
// surfaces show through the lens and the renderer fills them from the neighbouring background - reported, not gated):
// relMSE of renderer vs reference over in focus (|rho| < 0.05), near focus (< 1), foreground (rho < -1) and background (rho > 1) interiors, with
// the reference's own noise (relMSE of its two halves / 2). Gate: each interior class relMSE < 1e-3 + 2 x noise.
//   unx_test_reference_dof [--width W]
#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/metrics/Metrics.h"
#include "unx/reference/PathTracer.h"
#include "unx/render/Device.h"
#include "unx/render/Frame.h"
#include "unx/render/GpuScene.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"
#include "unx/scene/SceneData.h"
#include "unx/shading/DepthOfField.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::render;

namespace
{
#define CHECK(cond) \
    do { if (!(cond)) fail("%s:%d: CHECK failed: %s", __FILE__, __LINE__, #cond); } while (0)

scene::Texture checker(uint32_t n, uint32_t seed)
{
    scene::Texture t;
    t.width = t.height = n;
    t.format = scene::TextureFormat::Rgba8Linear;
    for (uint32_t y = 0; y < n; ++y)
        for (uint32_t x = 0; x < n; ++x)
        {
            const bool c = ((x / 4) ^ (y / 4) ^ seed) & 1;
            const uint32_t hsh = (x * 73856093u) ^ (y * 19349663u) ^ (seed * 83492791u);
            const uint8_t d = (uint8_t)(hsh >> 24) / 4;
            t.texels.insert(t.texels.end(), { (uint8_t)(c ? 230 - d : 30 + d), (uint8_t)(c ? 200 : 60 + d), (uint8_t)(c ? 90 + d : 180), 255 });
        }
    return t;
}

// A camera-facing quad (normal -z) over [x0, x1] x [y0, y1] at depth z, uv repeating 'tiles' times.
scene::Mesh quad(float x0, float x1, float y0, float y1, float z, float tiles, uint32_t material)
{
    scene::Mesh m;
    m.name = "quad";
    m.positions = { { x0, y0, z }, { x0, y1, z }, { x1, y1, z }, { x1, y0, z } };
    m.normals = { { 0, 0, -1 }, { 0, 0, -1 }, { 0, 0, -1 }, { 0, 0, -1 } };
    m.uv0 = { { 0, tiles }, { 0, 0 }, { tiles, 0 }, { tiles, tiles } };
    m.indices = { 0, 1, 2, 0, 2, 3 };
    m.submeshes.push_back({ 0, 6, material });
    return m;
}

struct Frame
{
    GpuScene scene;
    RenderGraph graph;
    TrackState state;
    FrameContext frame;
    FrameResources resources;
    FrameServices services;
    FramePassContext fc;
    Frame(Device& device, ShaderLibrary& shaders, const QualityConfig& quality)
        : scene(device), graph(device),
          fc{ device, graph, shaders, quality, scene, frame, resources, services, [](const ViewDesc&) -> D3D12_GPU_VIRTUAL_ADDRESS { return 0; }, &state }
    {
    }
};

ComPtr<ID3D12Resource> buffer(Device& device, uint64_t bytes, D3D12_HEAP_TYPE heap)
{
    D3D12_HEAP_PROPERTIES hp{ heap };
    D3D12_RESOURCE_DESC1 rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = std::max<uint64_t>(bytes, 256);
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    check(device.d3d()->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)), "buffer");
    return r;
}

uint32_t pitchOf(uint32_t bytesPerRow) { return (bytesPerRow + 255) / 256 * 256; }

// The renderer's depth of field over an RGBA32F image and R32F device depth; returns RGBA32F.
std::vector<float> renderDof(Device& device, ShaderLibrary& shaders, const ViewDesc& view, float aperture, float focus, const std::vector<float>& rgba,
                             const std::vector<float>& deviceDepth)
{
    const uint32_t w = view.width, h = view.height;
    const QualityConfig q = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
    Frame x(device, shaders, q);
    x.frame.lensAperture = aperture;
    x.frame.lensFocus = focus;
    const uint32_t cPitch = pitchOf(w * 16), dPitch = pitchOf(w * 4);
    ComPtr<ID3D12Resource> upC = buffer(device, (uint64_t)cPitch * h, D3D12_HEAP_TYPE_UPLOAD), upD = buffer(device, (uint64_t)dPitch * h, D3D12_HEAP_TYPE_UPLOAD),
                           rb = buffer(device, (uint64_t)cPitch * h, D3D12_HEAP_TYPE_READBACK);
    {
        uint8_t* p = nullptr;
        check(upC->Map(0, nullptr, reinterpret_cast<void**>(&p)), "map");
        for (uint32_t y = 0; y < h; ++y) std::memcpy(p + (size_t)y * cPitch, &rgba[(size_t)y * w * 4], (size_t)w * 16);
        upC->Unmap(0, nullptr);
        check(upD->Map(0, nullptr, reinterpret_cast<void**>(&p)), "map");
        for (uint32_t y = 0; y < h; ++y) std::memcpy(p + (size_t)y * dPitch, &deviceDepth[(size_t)y * w], (size_t)w * 4);
        upD->Unmap(0, nullptr);
    }
    ViewResources v;
    v.view = view;
    const TextureRef image = x.graph.createTexture(TextureDesc{ "dofgate.image", w, h, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT });
    v.depth = x.graph.createTexture(TextureDesc{ "dofgate.depth", w, h, 1, 1, DXGI_FORMAT_R32_FLOAT });
    const TextureRef out = x.graph.createTexture(TextureDesc{ "dofgate.out", w, h, 1, 1, DXGI_FORMAT_R32G32B32A32_FLOAT });
    const TextureRef depth = v.depth;
    ID3D12Resource *uc = upC.Get(), *ud = upD.Get(), *r = rb.Get();
    auto copyIn = [=](PassContext& c, TextureRef t, ID3D12Resource* src, DXGI_FORMAT f, uint32_t pitch) {
        D3D12_TEXTURE_COPY_LOCATION to{}, from{};
        to.pResource = c.resource(t);
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        from.pResource = src;
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint.Footprint = { f, w, h, 1, pitch };
        c.cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    };
    x.graph.addPass("dofgate.upload", QueueType::Graphics,
                    [&](PassBuilder& b) {
                        b.use(image, Use::CopyDst);
                        b.use(depth, Use::CopyDst);
                    },
                    [=](PassContext& c) {
                        copyIn(c, image, uc, DXGI_FORMAT_R32G32B32A32_FLOAT, cPitch);
                        copyIn(c, depth, ud, DXGI_FORMAT_R32_FLOAT, dPitch);
                    });
    if (!shading::depthOfFieldActive(x.fc, v)) fail("depth of field inactive");
    shading::depthOfField(x.fc, v, image, out);
    x.graph.addPass("dofgate.readback", QueueType::Graphics,
                    [&](PassBuilder& b) {
                        b.use(out, Use::CopySrc);
                        b.keep();
                    },
                    [=](PassContext& c) {
                        D3D12_TEXTURE_COPY_LOCATION to{}, from{};
                        to.pResource = r;
                        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                        to.PlacedFootprint.Footprint = { DXGI_FORMAT_R32G32B32A32_FLOAT, w, h, 1, cPitch };
                        from.pResource = c.resource(out);
                        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                        c.cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
                    });
    x.graph.execute(nullptr);
    device.waitIdle();
    if (device.drainDebugMessages() != 0) fail("D3D12 debug layer errors");
    std::vector<float> result((size_t)w * h * 4);
    uint8_t* p = nullptr;
    check(rb->Map(0, nullptr, reinterpret_cast<void**>(&p)), "map");
    for (uint32_t y = 0; y < h; ++y) std::memcpy(&result[(size_t)y * w * 4], p + (size_t)y * cPitch, (size_t)w * 16);
    rb->Unmap(0, nullptr);
    return result;
}
} // namespace

int main(int argc, char** argv)
{
    try
    {
        uint32_t W = 480;
        for (int i = 1; i + 1 < argc; ++i)
            if (std::string(argv[i]) == "--width") W = (uint32_t)std::atoi(argv[i + 1]);
        const uint32_t H = W * 9 / 16;
        scene::Scene s;
        s.name = "dof gate";
        s.sun.illuminance = 0;
        s.atmosphere.rayleighScattering = { 0, 0, 0 };
        s.atmosphere.mieScattering = { 0, 0, 0 };
        s.atmosphere.mieAbsorption = { 0, 0, 0 };
        s.atmosphere.ozoneAbsorption = { 0, 0, 0 };
        s.textures = { checker(64, 1), checker(64, 2), checker(64, 3) };
        s.materials.resize(3);
        const float emission[3] = { 1.0f, 2.0f, 0.6f };
        for (uint32_t k = 0; k < 3; ++k)
        {
            s.materials[k].baseColor = { 0, 0, 0 };
            s.materials[k].specular = 0;
            s.materials[k].emissive = { emission[k], emission[k], emission[k] };
            s.materials[k].emissiveTexture = k;
        }
        // Texels of at least 3 px on every layer at 480 px (the operator's input is the pixel's value: detail finer than a pixel
        // is not in the pinhole image, whatever the kernel): backdrop 3 tiles, target 0.4, weapon 0.7.
        s.meshes = { quad(-40, 40, -25, 25, 40, 3, 0), quad(-4, 1, -2.5f, 2.5f, 20, 0.4f, 1), quad(0.08f, 0.35f, -0.26f, -0.06f, 0.6f, 0.7f, 2) };
        s.instances.resize(3);
        for (uint32_t k = 0; k < 3; ++k) s.instances[k].mesh = k;
        scene::Camera cam;
        cam.name = "aim";
        cam.forward = { 0, 0, 1 };
        cam.verticalFov = 2 * std::atan(10.125f / 24.0f);  // 24 mm on the 36 x 20.25 mm sensor
        cam.nearPlane = 0.05f;
        cam.ev100 = std::log2(1.0f / 1.2f);  // exposure 1: pixel values are the emission (0.1 .. 2), well above the metric's 1e-4
        s.cameras.push_back(cam);
        scene::validate(s);

        DeviceOptions o;
        o.debugLayer = true;
        Device device(o);
        ShaderLibrary shaders(device, executableDirectory() / "shaders");
        ViewDesc view = ViewDesc::fromCamera(cam, W, H, {});
        const double fpx = 0.5 * H * view.proj.m[1][1];

        reference::PathTracer pt(s);
        const reference::ResolvedCamera rc = reference::resolveCamera(s, { "aim", "", 0 });
        const std::vector<float> depth = pt.primaryDepths(rc, W, H);
        reference::RenderSettings pinholeSettings;
        pinholeSettings.width = W, pinholeSettings.height = H, pinholeSettings.samplesPerPixel = 256, pinholeSettings.russianRouletteStart = 2;
        const reference::RenderOutput pinhole = pt.render(rc, pinholeSettings);
        std::vector<float> rgba((size_t)W * H * 4), deviceDepth((size_t)W * H);
        for (size_t i = 0; i < (size_t)W * H; ++i)
        {
            for (int c = 0; c < 3; ++c) rgba[4 * i + c] = pinhole.image.rgb[3 * i + c];
            rgba[4 * i + 3] = 1;
            deviceDepth[i] = std::isfinite(depth[i]) ? (float)(view.nearPlane / depth[i]) : 0.0f;
        }

        struct Case
        {
            const char* name;
            double rho, z, focus;  // the aperture giving |rho| px at depth z
        };
        int failures = 0;
        for (const Case& k : { Case{ "aim", 15, 0.6, 20 }, Case{ "background", 10, 40, 1.5 } })
        {
            const float aperture = (float)(2 * k.rho / (fpx * std::abs(1 / k.focus - 1 / k.z)));
            auto rhoOf = [&](double z) { return 0.5 * fpx * aperture * (1 / k.focus - 1 / z); };
            reference::ResolvedCamera lc = rc;
            lc.lensAperture = aperture;
            lc.lensFocus = (float)k.focus;
            reference::RenderSettings ls = pinholeSettings;
            ls.samplesPerPixel = 1024;
            const reference::RenderOutput lens = pt.render(lc, ls);
            const std::vector<float> dof = renderDof(device, shaders, view, aperture, (float)k.focus, rgba, deviceDepth);
            {
                // The captures (linear, exposure 1): the renderer's depth of field and the thin-lens reference.
                metrics::Image a = lens.image, pin = pinhole.image;
                for (size_t i = 0; i < (size_t)W * H; ++i)
                    for (int c = 0; c < 3; ++c) a.rgb[3 * i + c] = dof[4 * i + c];
                const std::string base = std::string(UNX_SOURCE_DIR) + "/Results/C/Dof/gate_" + k.name;
                metrics::writeExr(base + "_renderer.exr", a);
                metrics::writeExr(base + "_reference.exr", lens.image);
                if (std::string(k.name) == "aim") metrics::writeExr(std::string(UNX_SOURCE_DIR) + "/Results/C/Dof/gate_pinhole.exr", pin);
            }

            // Classes by the pinhole depth's rho; edges near depth discontinuities.
            double maxRho = 0;
            for (float z : depth)
                if (std::isfinite(z)) maxRho = std::max(maxRho, std::abs(rhoOf(z)));
            const int reachPx = (int)std::ceil(maxRho) + 2;
            std::vector<uint8_t> disc((size_t)W * H, 0), edge((size_t)W * H, 0);
            for (uint32_t y = 0; y < H; ++y)
                for (uint32_t x = 0; x + 1 < W; ++x)
                {
                    const float a = depth[(size_t)y * W + x], b = depth[(size_t)y * W + x + 1];
                    if (std::abs(a - b) > 0.05f * std::min(a, b)) disc[(size_t)y * W + x] = disc[(size_t)y * W + x + 1] = 1;
                    if (y + 1 < H)
                    {
                        const float c = depth[(size_t)(y + 1) * W + x];
                        if (std::abs(a - c) > 0.05f * std::min(a, c)) disc[(size_t)y * W + x] = disc[(size_t)(y + 1) * W + x] = 1;
                    }
                }
            for (uint32_t y = 0; y < H; ++y)
                for (uint32_t x = 0; x < W; ++x)
                    if (disc[(size_t)y * W + x])
                        for (int dy = -reachPx; dy <= reachPx; ++dy)
                            for (int dx = -reachPx; dx <= reachPx; ++dx)
                            {
                                const int xx = (int)x + dx, yy = (int)y + dy;
                                if (xx >= 0 && yy >= 0 && xx < (int)W && yy < (int)H) edge[(size_t)yy * W + xx] = 1;
                            }
            // Image borders: the lens sees past the frame (not in the pinhole image): also left out.
            for (uint32_t y = 0; y < H; ++y)
                for (uint32_t x = 0; x < W; ++x)
                    if ((int)x < reachPx || (int)y < reachPx || (int)(W - 1 - x) < reachPx || (int)(H - 1 - y) < reachPx) edge[(size_t)y * W + x] = 1;
            const char* names[5] = { "focus", "foreground", "background", "edge", "in focus" };
            double err[5] = {}, noise[5] = {};
            uint64_t count[5] = {};
            for (size_t i = 0; i < (size_t)W * H; ++i)
            {
                const double r = rhoOf(depth[i]);
                const int cls = edge[i] ? 3 : (std::abs(r) < 0.05 ? 4 : (std::abs(r) < 1 ? 0 : (r < 0 ? 1 : 2)));
                for (int c = 0; c < 3; ++c)
                {
                    const double ref = lens.image.rgb[3 * i + c], got = dof[4 * i + c];
                    const double a = lens.halfA.rgb[3 * i + c], b = lens.halfB.rgb[3 * i + c];
                    err[cls] += (got - ref) * (got - ref) / (ref * ref + 1e-4);
                    noise[cls] += 0.5 * (a - b) * (a - b) / (ref * ref + 1e-4) / 2;  // the mean's variance: half of a half's
                }
                ++count[cls];
            }
            logf("[%s] aperture %.4f m, focus %.2f m, max |rho| %.1f px (%ux%u, reference 1024 spp):\n", k.name, aperture, k.focus, maxRho, W, H);
            for (int c : { 4, 0, 1, 2, 3 })
            {
                if (count[c] == 0) continue;
                const double e = err[c] / (3.0 * count[c]), nz = noise[c] / (3.0 * count[c]);
                const bool gated = c != 3;
                const bool ok = !gated || e < 1e-3 + 2 * nz;
                logf("  %-10s %7llu px  relMSE %.3e  reference noise %.3e  %s\n", names[c], (unsigned long long)count[c], e, nz, gated ? (ok ? "ok" : "FAILED") : "(reported)");
                if (!ok) ++failures;
            }
        }
        logf(failures ? "FAILED: %d class(es)\n" : "PASS depth_of_field_matches_the_thin_lens\n", failures);
        return failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        logf("FAILED: %s\n", e.what());
        return 1;
    }
}
