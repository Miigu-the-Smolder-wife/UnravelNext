// Surface state layers in the material resolve (A7 join, M SurfaceLayers.hlsli): bricks of E's field reach the G-buffer's
// base colour through the layer rule. Floor (y = 0, grey 0.5) from 10 m above, read through debug.view = "albedo" (the
// stored sRGB8 base colour); the floor lies on the boundary between brick rows y = -1 and y = 0, so both rows carry the
// same values (the trilinear lookup then returns them exactly away from the region's edges):
//   1. scorch 1 over x, z in [0, 2): soot (0.03);
//   2. blood 0.5 over x in [-3, -1), z in [0, 2): lerp(grey, blood, 128/255) (unorm8 in the field);
//   3. snow +0.05 m over x in [0, 2), z in [-3, -1): lerp(grey, snow, 1 - exp(-d / 0.01 m)), d = 102/2048 m (int16
//      1/2048 m in the field); the floor faces up (full slope factor);
//   4. elsewhere grey; the frame before the delta grey everywhere;
//   5. no D3D12 debug-layer errors.
// Pixels whose floor point is within 0.3 m of a region's edge are left out (the trilinear ramp to absent bricks).
// Correctness run (standalone HostRenderer, hardware GPU; GpuLock -Kind correctness).
#include "Renderer/HostRenderer.h"

#include "TestScenes.h"
#include "unx/core/File.h"
#include "unx/decal/SurfaceState.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::host;
using namespace unx::render;

namespace
{
constexpr uint32_t kWidth = 960, kHeight = 540;

scene::Scene floorScene()
{
    scene::Scene s = test::oneBox();
    s.name = "floor";
    s.materials[0].baseColor = { 0.5f, 0.5f, 0.5f };
    s.instances[0].transform.m[1][3] = -5.0f;  // the box out of sight under the floor
    scene::Mesh floor;
    floor.name = "floor";
    const float h = 20;
    floor.positions = { { -h, 0, h }, { h, 0, h }, { h, 0, -h }, { -h, 0, -h } };
    floor.normals.assign(4, float3{ 0, 1, 0 });
    floor.indices = { 0, 1, 2, 0, 2, 3 };
    floor.submeshes.push_back({ 0, 6, 0 });
    s.meshes.push_back(std::move(floor));
    scene::Instance fi;
    fi.mesh = 1;
    s.instances.push_back(fi);
    scene::Camera& c = s.cameras[0];
    c.position = { 0, 10, 0 };
    c.forward = { 0, -1, 0 };
    c.up = { 0, 0, -1 };
    return s;
}

uint32_t srgb8(double linear)
{
    const double v = linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1 / 2.4) - 0.055;
    return (uint32_t)std::lround(std::clamp(v, 0.0, 1.0) * 255.0);
}
uint32_t srgb8(double r, double g, double b) { return srgb8(r) | srgb8(g) << 8 | srgb8(b) << 16; }

// Bricks x in [x0, x1), z in [z0, z1) of rows y = -1 and 0 with channel c = value.
void addRegion(std::vector<surface::BrickInput>& out, int x0, int x1, int z0, int z1, uint32_t channel, float value)
{
    for (int y = -1; y <= 0; ++y)
        for (int x = x0; x < x1; ++x)
            for (int z = z0; z < z1; ++z)
            {
                surface::BrickInput b{};
                b.key[0] = x;
                b.key[1] = y;
                b.key[2] = z;
                for (uint32_t v = 0; v < 64; ++v) b.value[v * surface::kChannels + channel] = value;
                out.push_back(b);
            }
}

std::vector<uint32_t> readAlbedo(HostRenderer& h, uint64_t& frame)
{
    std::vector<uint32_t> pixels((size_t)kWidth * kHeight);
    for (int f = 0; f < 3; ++f)
    {
        FramePacket p;
        p.frameIndex = frame++;
        p.deltaTime = 1.0f / 60;
        p.width = kWidth;
        p.height = kHeight;
        p.camera = h.scene().cameras[0];
        const bool last = f == 2;
        h.renderStandalone(h.queueFrame(std::move(p)), last ? pixels.data() : nullptr, last ? pixels.size() * 4 : 0);
    }
    for (uint32_t& px : pixels)
    {
        uint32_t rgb = 0;
        for (int c = 0; c < 3; ++c) rgb |= (uint32_t)std::lround(((px >> (10 * c)) & 1023u) * 255.0 / 1023.0) << (8 * c);
        px = rgb;
    }
    return pixels;
}
} // namespace

int main()
{
    try
    {
        uint32_t failures = 0;
        auto expect = [&](const char* what, bool ok) {
            logf("  %-86s %s\n", what, ok ? "ok" : "FAILED");
            if (!ok) ++failures;
        };
        HostRendererOptions o;
        o.standalone = true;
        o.framesInFlight = 2;
        o.shaderDirectory = executableDirectory() / "shaders";
        o.qualityDirectory = std::filesystem::path(UNX_SOURCE_DIR) / "Config/quality";
        o.qualityOverrides = { "gi.deterministic=true", "debug.view=\"albedo\"" };
        HostRenderer h(o);
        h.scene() = floorScene();
        h.commit();
        uint64_t frame = 0;
        const std::vector<uint32_t> before = readAlbedo(h, frame);

        std::vector<surface::BrickInput> bricks;
        addRegion(bricks, 0, 2, 0, 2, 1, 1.0f);    // scorch
        addRegion(bricks, -3, -1, 0, 2, 4, 0.5f);  // blood
        addRegion(bricks, 0, 2, -3, -1, 5, 0.05f); // snow (m)
        h.surfaceDelta(bricks.data(), bricks.size(), nullptr, 0);
        h.setSurfaceHalfLives({ 0, 0, 0, 0, 0, 0 });
        h.setSurfaceTime(1.0);
        const std::vector<uint32_t> after = readAlbedo(h, frame);

        const double tanV = std::tan(floorScene().cameras[0].verticalFov * 0.5), tanH = tanV * kWidth / kHeight;
        const double blood = 128.0 / 255.0, snowDepth = 102.0 / 2048.0, snowA = 1 - std::exp(-snowDepth / 0.01);
        auto mix = [](double a, double b, double t) { return a + (b - a) * t; };
        const uint32_t grey = srgb8(0.5, 0.5, 0.5), soot = srgb8(0.03, 0.03, 0.03);
        const uint32_t red = srgb8(mix(0.5, 0.30, blood), mix(0.5, 0.012, blood), mix(0.5, 0.010, blood));
        const uint32_t white = srgb8(mix(0.5, 0.90, snowA), mix(0.5, 0.91, snowA), mix(0.5, 0.93, snowA));
        auto close = [](uint32_t a, uint32_t b) {
            for (int c = 0; c < 3; ++c)
                if (std::abs((int)((a >> (8 * c)) & 255u) - (int)((b >> (8 * c)) & 255u)) > 1) return false;
            return true;
        };
        struct Region { const char* name; double x0, x1, z0, z1; uint32_t want; uint32_t count = 0, wrong = 0; };
        Region regions[3] = { { "scorch", 0, 2, 0, 2, soot }, { "blood", -3, -1, 0, 2, red }, { "snow", 0, 2, -3, -1, white } };
        uint32_t greyCount = 0, greyWrong = 0, beforeWrong = 0;
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const size_t i = (size_t)y * kWidth + x;
                // floor point of the pixel (camera straight down: right = +x, image up = -z)
                const double fx = ((x + 0.5) / kWidth * 2 - 1) * tanH * 10, fz = -(1 - (y + 0.5) / kHeight * 2) * tanV * 10;
                beforeWrong += before[i] != grey;
                int inside = -1;
                bool edge = false;
                for (int r = 0; r < 3; ++r)
                {
                    const Region& g = regions[r];
                    const bool in = fx > g.x0 + 0.3 && fx < g.x1 - 0.3 && fz > g.z0 + 0.3 && fz < g.z1 - 0.3;
                    const bool around = fx > g.x0 - 0.3 && fx < g.x1 + 0.3 && fz > g.z0 - 0.3 && fz < g.z1 + 0.3;
                    if (in) inside = r;
                    else if (around) edge = true;
                }
                if (inside >= 0)
                {
                    Region& g = regions[inside];
                    ++g.count;
                    if (!close(after[i], g.want) && ++g.wrong <= 3) logf("    %s pixel (%u, %u): %06x, expected %06x\n", g.name, x, y, after[i], g.want);
                }
                else if (!edge)
                {
                    ++greyCount;
                    if (!close(after[i], grey) && ++greyWrong <= 3) logf("    grey pixel (%u, %u): %06x\n", x, y, after[i]);
                }
            }
        for (const Region& g : regions) logf("  %-6s: %u px, %u off by more than 1 code (expected %06x)\n", g.name, g.count, g.wrong, g.want);
        logf("  grey elsewhere: %u px, %u off; before the delta %u px not grey\n", greyCount, greyWrong, beforeWrong);
        expect("before the delta the floor is grey", beforeWrong == 0);
        expect("scorch 1: soot base colour", regions[0].count > 1000 && regions[0].wrong == 0);
        expect("blood 0.5 (unorm8): lerp(grey, blood, 128/255)", regions[1].count > 1000 && regions[1].wrong == 0);
        expect("snow +0.05 m (1/2048 m): lerp(grey, snow, 1 - exp(-d / 1 cm)) on the up-facing floor", regions[2].count > 1000 && regions[2].wrong == 0);
        expect("grey away from the bricks", greyCount > 100000 && greyWrong == 0);
        expect("D3D12 debug layer errors 0", h.debugErrors() == 0);
        logf(failures ? "HOST SURFACE LAYERS TEST FAILED (%u)\n" : "HOST SURFACE LAYERS TEST PASS\n", failures);
        return failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        logf("HOST SURFACE LAYERS TEST ERROR: %s\n", e.what());
        return 2;
    }
}
