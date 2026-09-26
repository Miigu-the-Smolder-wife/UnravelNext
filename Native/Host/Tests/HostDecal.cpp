// Decals in the material resolve (render A joins E's A7; FEATURES_GAME 5.2): E's decalApply runs in M's resolve after the
// normal map and before the band limit, with the pixel's camera-relative position, its screen derivatives, the
// geometric normal of the side it shades and its instance. Floor (y = 0) and a unit box (top at y = 1) seen from above
// through the renderer's G-buffer (debug.view = "albedo": the stored sRGB8 base colour, exact):
//   1. a world-space decal, half opacity, hard edges, over the floor: inside its box the floor's base colour is
//      lerp(grey, red, 0.5), outside unchanged;
//   2. a decal attached to the box instance whose volume also contains the floor around the box: the box's top turns
//      blue, the floor inside that volume stays grey (instance rule), the box's sides (normal at 90 degrees to the
//      decal's +Z) stay grey (angle fade);
//   3. the frame without decals is grey everywhere (no decal pass effect when none is live);
//   4. no D3D12 debug-layer errors.
// Pixels within 1.5 px of a silhouette or a decal box edge are left out (their centre may fall on either side).
// Correctness run (standalone HostRenderer, hardware GPU; GpuLock -Kind correctness).
#include "Renderer/HostRenderer.h"

#include "TestScenes.h"
#include "unx/core/File.h"
#include "unx/decal/Decals.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::host;
using namespace unx::render;

namespace
{
constexpr uint32_t kWidth = 960, kHeight = 540;
constexpr float kBoxX = 2.5f;  // box centre (x, 0.5, 0)

scene::Scene floorAndBox()
{
    scene::Scene s = test::oneBox();
    s.name = "floor and box";
    s.materials[0].baseColor = { 0.5f, 0.5f, 0.5f };
    scene::Material red, blue;
    red.name = "decal red";
    red.baseColor = { 0.9f, 0.1f, 0.1f };
    blue.name = "decal blue";
    blue.baseColor = { 0.1f, 0.2f, 0.9f };
    s.materials.push_back(red);
    s.materials.push_back(blue);
    s.instances[0].transform.m[0][3] = kBoxX;
    s.instances[0].transform.m[1][3] = 0.5f;
    scene::Mesh floor;
    floor.name = "floor";
    const float h = 20;  // beyond the view (half width 10.3 m at the floor)
    floor.positions = { { -h, 0, h }, { h, 0, h }, { h, 0, -h }, { -h, 0, -h } };  // counter-clockwise from above
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

decal::Decal box(float3 centre, float3 x, float3 y, float3 z, uint32_t material, uint32_t instance, float opacity)
{
    decal::Decal d;
    const float3 cols[4] = { x, y, z, centre };
    for (int c = 0; c < 4; ++c)
    {
        d.box.m[0][c] = cols[c].x;
        d.box.m[1][c] = cols[c].y;
        d.box.m[2][c] = cols[c].z;
    }
    d.material = material;
    d.instance = instance;
    d.opacity = opacity;
    d.edge = 0;
    return d;
}

uint32_t srgb8(double linear)
{
    const double v = linear <= 0.0031308 ? 12.92 * linear : 1.055 * std::pow(linear, 1 / 2.4) - 0.055;
    return (uint32_t)std::lround(std::clamp(v, 0.0, 1.0) * 255.0);
}

// Frame 'frames' of the scene with or without the decals; the G-buffer's sRGB8 base colour per pixel (from the albedo
// view's 10-bit output: code = round(byte / 255 x 1023)).
std::vector<uint32_t> renderFrames(bool withDecals, uint32_t& errors)
{
    HostRendererOptions o;
    o.standalone = true;
    o.framesInFlight = 2;
    o.shaderDirectory = executableDirectory() / "shaders";
    o.qualityDirectory = std::filesystem::path(UNX_SOURCE_DIR) / "Config/quality";
    o.qualityOverrides = { "gi.deterministic=true", "debug.view=\"albedo\"" };
    HostRenderer h(o);
    h.scene() = floorAndBox();
    h.commit();
    if (withDecals)
    {
        decal::DecalSet& set = decal::decals(h.trackStateForTest());
        set.add(box({ 0, 0, 0 }, { 1.5f, 0, 0 }, { 0, 0, 1.5f }, { 0, 0.5f, 0 }, 1, decal::kNone, 0.5f));
        // object space of the box (instance 0): centre at its middle, 1.5 m half extents across, 1.2 m up and down
        set.add(box({ 0, 0, 0 }, { 1.5f, 0, 0 }, { 0, 0, 1.5f }, { 0, 1.2f, 0 }, 2, 0, 1.0f));
    }
    std::vector<uint32_t> pixels((size_t)kWidth * kHeight);
    for (uint32_t f = 0; f < 3; ++f)
    {
        FramePacket p;
        p.frameIndex = f;
        p.deltaTime = 1.0f / 60;
        p.width = kWidth;
        p.height = kHeight;
        p.camera = h.scene().cameras[0];
        const bool last = f == 2;
        h.renderStandalone(h.queueFrame(std::move(p)), last ? pixels.data() : nullptr, last ? pixels.size() * 4 : 0);
    }
    errors += h.debugErrors();
    for (uint32_t& px : pixels)
    {
        uint32_t rgb = 0;
        for (int c = 0; c < 3; ++c) rgb |= (uint32_t)std::lround(((px >> (10 * c)) & 1023u) * 255.0 / 1023.0) << (8 * c);
        px = rgb;
    }
    return pixels;
}

enum Region { Grey, Red, Blue, Edge };

// What pixel (x, y) sees from the camera straight down (right = +x, image up = -z), within 1.5 px of no boundary.
Region classify(uint32_t x, uint32_t y, double tanV)
{
    const double tanH = tanV * kWidth / kHeight, pxPerRad = (kHeight * 0.5) / tanV;
    const double nx = ((x + 0.5) / kWidth * 2 - 1) * tanH, ny = (1 - (y + 0.5) / kHeight * 2) * tanV;
    // ray (nx, -1, -ny) from (0, 10, 0): at height yh the point is (10 - yh) (nx, -ny)
    const double margin = 1.5 / pxPerRad;  // 1.5 px in tangent units
    auto onLine = [&](double a, double b) { return std::abs(a - b) < margin; };
    // box top (y = 1, |x - 2.5| < 0.5, |z| < 0.5) seen at tangents (x / 9, -z / 9)
    const double tx = nx, tz = -ny;
    const double topX = tx * 9, topZ = tz * 9;
    const bool onTop = std::abs(topX - kBoxX) < 0.5 && std::abs(topZ) < 0.5;
    if (onLine(tx, (kBoxX - 0.5) / 9) || onLine(tx, (kBoxX + 0.5) / 9) || onLine(tz, 0.5 / 9) || onLine(tz, -0.5 / 9)) return Edge;
    if (onTop) return Blue;
    // box sides: between the top's and the base's silhouettes
    const bool inBase = std::abs(tx * 10 - kBoxX) < 0.5 && std::abs(tz * 10) < 0.5;
    if (inBase) return Grey;  // a side face (never visible from x < 2: then the base silhouette is inside the top's)
    if (onLine(tx, (kBoxX - 0.5) / 10) || onLine(tx, (kBoxX + 0.5) / 10) || onLine(tz, 0.5 / 10) || onLine(tz, -0.5 / 10)) return Edge;
    // floor (y = 0) at (10 tx, 10 tz): the world decal's box |x| < 1.5, |z| < 1.5
    const double fx = tx * 10, fz = tz * 10;
    if (onLine(tx, 1.5 / 10) || onLine(tx, -1.5 / 10) || onLine(tz, 1.5 / 10) || onLine(tz, -1.5 / 10)) return Edge;
    return std::abs(fx) < 1.5 && std::abs(fz) < 1.5 ? Red : Grey;
}
} // namespace

int main()
{
    try
    {
        uint32_t failures = 0, errors = 0;
        auto expect = [&](const char* what, bool ok) {
            logf("  %-86s %s\n", what, ok ? "ok" : "FAILED");
            if (!ok) ++failures;
        };
        const std::vector<uint32_t> plain = renderFrames(false, errors), painted = renderFrames(true, errors);
        const double tanV = std::tan(test::oneBox().cameras[0].verticalFov * 0.5);
        const uint32_t grey = srgb8(0.5) * 0x010101u;
        const uint32_t red = srgb8(0.7) | srgb8(0.3) << 8 | srgb8(0.3) << 16;  // lerp((0.5, 0.5, 0.5), (0.9, 0.1, 0.1), 0.5)
        const uint32_t blue = srgb8(0.1) | srgb8(0.2) << 8 | srgb8(0.9) << 16;
        uint32_t count[4] = {}, wrong[4] = {}, plainWrong = 0, boxFloorPixels = 0, boxFloorWrong = 0;
        for (uint32_t y = 0; y < kHeight; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const size_t i = (size_t)y * kWidth + x;
                const Region r = classify(x, y, tanV);
                if (r == Edge) continue;
                plainWrong += plain[i] != grey;
                ++count[r];
                const uint32_t want = r == Red ? red : r == Blue ? blue : grey;
                auto close = [](uint32_t a, uint32_t b) {
                    for (int c = 0; c < 3; ++c)
                        if (std::abs((int)((a >> (8 * c)) & 255u) - (int)((b >> (8 * c)) & 255u)) > 1) return false;
                    return true;
                };
                const bool bad = !close(painted[i], want);
                wrong[r] += bad;
                if (bad && wrong[r] <= 3)
                    logf("    pixel (%u, %u) region %d: %06x, expected %06x\n", x, y, (int)r, painted[i], want);
                // floor pixels inside the box decal's volume (|x - 2.5| < 1.5, |z| < 1.5) but outside the box's base
                const double tanH = tanV * kWidth / kHeight;
                const double fx = ((x + 0.5) / kWidth * 2 - 1) * tanH * 10, fz = -(1 - (y + 0.5) / kHeight * 2) * tanV * 10;
                if (r == Grey && std::abs(fx - kBoxX) < 1.5 && std::abs(fz) < 1.5 && !(std::abs(fx - kBoxX) < 0.6 && std::abs(fz) < 0.6))
                {
                    ++boxFloorPixels;
                    boxFloorWrong += bad;
                }
            }
        logf("  pixels: grey %u (wrong %u), red %u (wrong %u), blue %u (wrong %u); floor inside the instance decal's volume %u (painted %u); without decals %u not grey\n",
             count[Grey], wrong[Grey], count[Red], wrong[Red], count[Blue], wrong[Blue], boxFloorPixels, boxFloorWrong, plainWrong);
        expect("without decals: the G-buffer is the floor's grey everywhere", plainWrong == 0 && count[Grey] > 0);
        expect("world decal, half opacity: floor inside its box = lerp(grey, red, 0.5) (+-1 code)", count[Red] > 10000 && wrong[Red] == 0);
        expect("instance decal: the box's top is blue", count[Blue] > 1000 && wrong[Blue] == 0);
        expect("instance decal: the floor inside its volume stays grey (other instance)", boxFloorPixels > 1000 && boxFloorWrong == 0);
        expect("elsewhere (floor, box sides at 90 degrees) grey", wrong[Grey] == 0);
        expect("D3D12 debug layer errors 0", errors == 0);
        logf(failures ? "HOST DECAL TEST FAILED (%u)\n" : "HOST DECAL TEST PASS\n", failures);
        return failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        logf("HOST DECAL TEST ERROR: %s\n", e.what());
        return 2;
    }
}
