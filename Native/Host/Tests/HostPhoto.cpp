// B11 photo mode through the host (FEATURES_GAME 17; HostRenderer::photoBegin / photoSave / photoEnd, E's GpuPathTracer
// on the renderer's device, M's post chain via FrameRenderer::recordImage):
//   1. the snapshot (photoScene) holds what the frames draw beyond the committed content: a C2b runtime instance with
//      its mesh, and a view model at the latest frame camera x its pose;
//   2. photo frames: the accumulation reaches its target (4 samples per frame: 2 per half), each frame shows the image
//      through the post chain - every channel of the displayed frame is the CPU film curve + sRGB OETF of the saved EXR
//      within the 10-bit triangular dither (<= 1 code; mean within 0.2), and the saved 16-bit PNG is the frame's code
//      values exactly (v x 65535 / 1023);
//   3. the photo is the tracer's image of the snapshot: a GpuPathTracer of its own (own device) rendering photoScene()
//      with the same camera and sample count gives the same image up to the passes' float sums (relMSE < 1e-6);
//   4. a new output size restarts the accumulation at that size;
//   5. photoEnd: the frames show the scene again (the photo camera looked at the sky: the box is back), status off;
//   6. no D3D12 debug-layer errors.
// Correctness run (standalone HostRenderer, hardware GPU; GpuLock -Kind correctness).
#include "Renderer/HostRenderer.h"

#include "TestScenes.h"
#include "../../Render/Passes/Shading/Tests/FilmCurve.h"
#include "unx/core/File.h"
#include "unx/metrics/Metrics.h"
#include "unx/reference/GpuPathTracer.h"
#include "unx/render/Device.h"

#include <wincodec.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::host;
using namespace unx::render;

namespace
{
constexpr uint32_t kWidth = 320, kHeight = 180;

float3x4 translation(float x, float y, float z)
{
    float3x4 t;
    t.m[0][3] = x, t.m[1][3] = y, t.m[2][3] = z;
    return t;
}

std::vector<uint16_t> readPng16(const std::filesystem::path& path, uint32_t& width, uint32_t& height)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ComPtr<IWICImagingFactory> factory;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "WIC factory");
    ComPtr<IWICBitmapDecoder> decoder;
    check(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder), "PNG decoder");
    ComPtr<IWICBitmapFrameDecode> frame;
    check(decoder->GetFrame(0, &frame), "PNG frame");
    WICPixelFormatGUID format{};
    check(frame->GetPixelFormat(&format), "PNG format");
    if (format != GUID_WICPixelFormat48bppRGB) fail("the PNG is not 16-bit RGB");
    check(frame->GetSize(&width, &height), "PNG size");
    std::vector<uint16_t> rgb((size_t)width * height * 3);
    check(frame->CopyPixels(nullptr, width * 6, (UINT)(rgb.size() * 2), (BYTE*)rgb.data()), "PNG pixels");
    return rgb;
}

float srgbOetf(float c) { return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.0f / 2.4f) - 0.055f; }
} // namespace

int main()
{
    try
    {
        uint32_t failures = 0;
        auto expect = [&](const char* what, bool ok) {
            logf("  %-96s %s\n", what, ok ? "ok" : "FAILED");
            if (!ok) ++failures;
        };
        HostRendererOptions o;
        o.standalone = true;
        o.framesInFlight = 2;
        o.debugLayer = true;
        o.shaderDirectory = executableDirectory() / "shaders";
        o.qualityDirectory = std::filesystem::path(UNX_SOURCE_DIR) / "Config/quality";
        // Item 2 compares the displayed frame with the curve and the OETF alone: the chain's terms that are on by default
        // (bloom, vignette, local exposure; PostTests' subject) are off here.
        o.qualityOverrides = { "gi.deterministic=true", "shading.post_bloom_strength=0.0", "shading.post_vignette=0.0", "shading.post_local_exposure=false" };
        HostRenderer h(o);
        h.scene() = host::test::oneBox();
        scene::Instance second = h.scene().instances[0];  // instance 1: the view model
        second.transform = translation(0, -50, 0);
        h.scene().instances.push_back(second);
        scene::Camera camera = h.scene().cameras[0];
        camera.ev100 = 13.0f;
        RuntimeCapacity capacity;
        capacity.meshes = 4, capacity.submeshes = 8, capacity.vertices = 1024, capacity.indices = 4096;
        capacity.clusters = 64, capacity.clusterVertexIndices = 4096, capacity.clusterTriangles = 4096, capacity.nodes = 64;
        capacity.instances = 16;
        h.reserveRuntime(capacity);
        h.commit();

        uint64_t index = 0;
        std::vector<uint32_t> image;
        auto frame = [&](uint32_t w, uint32_t hgt, bool read) {
            FramePacket p;
            p.frameIndex = index;
            p.time = index / 60.0;
            ++index;
            p.deltaTime = 1.0f / 60;
            p.width = w;
            p.height = hgt;
            p.camera = camera;
            image.assign((size_t)w * hgt, 0);
            h.renderStandalone(h.queueFrame(std::move(p)), read ? image.data() : nullptr, read ? image.size() * 4 : 0);
        };

        // 1. snapshot
        scene::Mesh half = host::test::oneBox().meshes[0];
        half.name = "runtime box";
        for (float3& p : half.positions) p = p * 0.5f;
        h.addRuntimeInstance(h.addRuntimeMesh(half), translation(1.2f, 0, 0), scene::InstanceCastShadow);
        h.viewModelAdd(1, translation(0.3f, -0.2f, -1.5f));
        for (int f = 0; f < 3; ++f) frame(kWidth, kHeight, false);
        const scene::Scene shot = h.photoScene();
        const scene::Instance& vm = shot.instances[1];
        const float3 vmAt{ vm.transform.m[0][3], vm.transform.m[1][3], vm.transform.m[2][3] };
        const float3 vmWant = camera.position + float3{ 0.3f, -0.2f, -1.5f };  // the camera looks down -z, up +y
        logf("  snapshot: %zu instances, %zu meshes; view model at (%.4f %.4f %.4f), runtime instance mesh %u at x %.3f\n", shot.instances.size(), shot.meshes.size(),
             vmAt.x, vmAt.y, vmAt.z, shot.instances.back().mesh, shot.instances.back().transform.m[0][3]);
        expect("snapshot: the runtime instance and its mesh are in it", shot.instances.size() == 3 && shot.meshes.size() == 2 && shot.instances[2].mesh == 1 &&
                                                                           shot.instances[2].transform.m[0][3] == 1.2f);
        expect("snapshot: the view model at the frame camera x its pose (1e-5 m)",
               std::abs(vmAt.x - vmWant.x) < 1e-5f && std::abs(vmAt.y - vmWant.y) < 1e-5f && std::abs(vmAt.z - vmWant.z) < 1e-5f);

        // 2. photo frames and saves
        PhotoSettings settings;
        settings.samplesPerPixel = 16;
        settings.halfSamplesPerFrame = 2;
        const double photoTime = (index - 1) / 60.0;  // the latest queued frame's time: the snapshot's (wind)
        h.photoBegin(camera, settings);
        // the tracer's passes add at most 2 samples per half each (fewer while its dispatch size ramps up)
        std::vector<double> frameMs;
        std::vector<uint32_t> samples;
        for (int f = 0; f < 40 && (samples.empty() || samples.back() < 16); ++f)
        {
            const auto t0 = std::chrono::steady_clock::now();
            frame(kWidth, kHeight, false);
            frameMs.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
            samples.push_back(h.photoStatus().samples);
        }
        frame(kWidth, kHeight, false);  // one more: the accumulation stays at its target
        PhotoStatus st = h.photoStatus();
        bool rising = true;
        for (size_t i = 0; i < samples.size(); ++i) rising = rising && samples[i] > (i ? samples[i - 1] : 0) && samples[i] <= (i ? samples[i - 1] : 0) + 4;
        std::string trace;
        for (size_t i = 0; i < samples.size(); ++i) trace += std::to_string(samples[i]) + " (" + std::to_string((int)frameMs[i]) + " ms) ";
        logf("  photo: start %.2f s; samples after each frame (frame time): %s-> %u of %u, error '%s'\n", st.startSeconds, trace.c_str(), st.samples, st.target, st.error.c_str());
        expect("photo: every frame adds 1..4 samples (2 per half at most), the accumulation stops at its target", st.active && rising && st.samples == 16 && st.error.empty());
        const std::filesystem::path dir = executableDirectory() / "host_photo";
        std::filesystem::create_directories(dir);
        h.photoSave(dir / "photo.exr", dir / "photo.png");
        frame(kWidth, kHeight, true);
        const std::vector<uint32_t> shown = image;
        st = h.photoStatus();
        expect("photo save: both files written, relMSE estimate reported", st.saves == 1 && st.relMse >= 0 && st.error.empty());
        const metrics::Image exr = metrics::readExr(dir / "photo.exr");
        int worst = 0;
        double sum = 0;
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                float c[3] = { exr.pixel(x, y)[0], exr.pixel(x, y)[1], exr.pixel(x, y)[2] };
                unx::test::filmCurve(c, 0);
                for (int k = 0; k < 3; ++k)
                {
                    const float want = std::clamp(srgbOetf(std::clamp(c[k], 0.0f, 1.0f)), 0.0f, 1.0f) * 1023.0f;
                    const float got = (float)((shown[(size_t)y * kWidth + x] >> (10 * k)) & 1023u);
                    worst = std::max(worst, (int)std::ceil(std::abs(got - want) - 0.5f));
                    sum += got - want;
                }
            }
        const double mean = sum / (3.0 * kWidth * kHeight);
        logf("  displayed vs film curve + OETF of the EXR: worst %d codes (beyond rounding), mean %+.3f codes (relMSE estimate %.3g)\n", worst, mean, st.relMse);
        expect("the displayed frame is the post chain of the saved image (<= 1 code dither, |mean| < 0.2)", worst <= 1 && std::abs(mean) < 0.2);
        uint32_t pw = 0, ph = 0;
        const std::vector<uint16_t> png = readPng16(dir / "photo.png", pw, ph);
        uint32_t pngOff = 0;
        for (size_t i = 0; pw == kWidth && ph == kHeight && i < shown.size(); ++i)
            for (int k = 0; k < 3; ++k) pngOff += png[i * 3 + k] != (uint16_t)((((shown[i] >> (10 * k)) & 1023u) * 65535u + 511u) / 1023u);
        expect("the 16-bit PNG holds the displayed frame's codes exactly", pw == kWidth && ph == kHeight && pngOff == 0);

        // 3. the same image from a tracer of its own
        {
            reference::GpuPathTracer own(shot, std::filesystem::path(), "photo check");
            reference::ResolvedCamera c;
            c.position = camera.position, c.forward = camera.forward, c.up = camera.up;
            c.verticalFov = camera.verticalFov, c.nearPlane = camera.nearPlane, c.ev100 = camera.ev100;
            c.time = (float)photoTime;
            reference::RenderSettings rs;
            rs.width = kWidth, rs.height = kHeight, rs.samplesPerPixel = 16;
            rs.russianRouletteStart = (uint32_t)h.quality().number("reference.russian_roulette_start_bounce");
            const reference::RenderOutput ref = own.render(c, rs);
            const double e = metrics::relMse(ref.image, exr);
            logf("  photo vs own tracer (same snapshot, camera, 16 samples): relMSE %.3g (their halves' noise %.3g)\n", e, ref.halvesRelMse);
            expect("the photo is the tracer's image of the snapshot (relMSE < 1e-6)", e < 1e-6);
        }

        // 4. a new size restarts
        frame(kWidth / 2, kHeight / 2, false);
        st = h.photoStatus();
        logf("  after resize: %ux%u, %u samples\n", st.width, st.height, st.samples);
        expect("a new output size restarts the accumulation at that size", st.active && st.width == kWidth / 2 && st.height == kHeight / 2 && st.samples == 4);

        // 5. photoEnd (a photo of the sky first: the box is back afterwards)
        scene::Camera sky = camera;
        sky.forward = { 0, 1, 0 };
        sky.up = { 0, 0, -1 };
        h.photoBegin(sky, settings);
        frame(kWidth, kHeight, true);
        const std::vector<uint32_t> skyPhoto = image;
        h.photoEnd();
        for (int f = 0; f < 3; ++f) frame(kWidth, kHeight, f == 2);
        uint32_t changed = 0;
        for (size_t i = 0; i < image.size(); ++i) changed += image[i] != skyPhoto[i];
        st = h.photoStatus();
        logf("  after photoEnd: %u of %zu pixels differ from the sky photo\n", changed, image.size());
        expect("photoEnd: the scene again (the box in view), status off", !st.active && changed > image.size() / 10);

        const uint32_t errors = h.debugErrors();
        expect("D3D12 debug layer errors 0", errors == 0);
        logf(failures ? "HOST PHOTO TEST FAILED (%u)\n" : "HOST PHOTO TEST PASS\n", failures);
        return failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        logf("HOST PHOTO TEST ERROR: %s\n", e.what());
        return 2;
    }
}
