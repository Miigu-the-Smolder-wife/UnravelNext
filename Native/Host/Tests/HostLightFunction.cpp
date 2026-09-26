// Light functions in M's shading (A8 join, E's Passes/Lights; ShadeOpaque P[6].w): a floor lit only by one spot light
// (the sun, GI irradiance and reflections left out through shading.experiment_disable, so the local light is the whole
// signal), seen from above:
//   1. a constant light function of 0.5 (one intensity key) gives the image of the same light at half intensity
//      (within 1 code: the function multiplies the light's illuminance);
//   2. a two-texel cookie (left texel black, right white, bilinear between their centres) on the spot: where it is 1 the
//      floor is the uncookied light's, where it is 0 the floor gets nothing from the light (within 1 code);
//   3. no D3D12 debug-layer errors.
// Correctness run (standalone HostRenderer, hardware GPU; GpuLock -Kind correctness).
#include "Renderer/HostRenderer.h"

#include "TestScenes.h"
#include "unx/core/File.h"
#include "unx/lights/LightFunctions.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::host;
using namespace unx::render;

namespace
{
constexpr uint32_t kWidth = 640, kHeight = 360;

scene::Scene spotScene(float intensity, bool withLight)
{
    scene::Scene s = test::oneBox();
    s.name = "spot floor";
    s.materials[0].baseColor = { 0.6f, 0.6f, 0.6f };
    s.instances[0].transform.m[1][3] = -5.0f;  // the box out of sight
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
    if (withLight)
    {
        scene::Light l;
        l.type = scene::LightType::Spot;
        l.position = { 0, 3, 0 };
        l.forward = { 0, -1, 0 };
        l.right = { 1, 0, 0 };
        l.intensity = intensity;
        l.range = 20;
        l.spotInner = 0.9f;
        l.spotOuter = 1.0f;
        s.lights.push_back(l);
    }
    scene::Camera& c = s.cameras[0];
    c.position = { 0, 8, 0 };
    c.forward = { 0, -1, 0 };
    c.up = { 0, 0, -1 };
    c.ev100 = 6;
    return s;
}

std::vector<uint32_t> renderSpot(float intensity, bool withLight, const lights::LightFunction* function, uint32_t& errors)
{
    HostRendererOptions o;
    o.standalone = true;
    o.framesInFlight = 2;
    o.shaderDirectory = executableDirectory() / "shaders";
    o.qualityDirectory = std::filesystem::path(UNX_SOURCE_DIR) / "Config/quality";
    o.qualityOverrides = { "gi.deterministic=true", "shading.experiment_disable=22" };  // no GI irradiance, reflections, sun
    HostRenderer h(o);
    h.scene() = spotScene(intensity, withLight);
    h.commit();
    if (function) lights::lightFunctions(h.trackStateForTest()).set(0, *function);
    std::vector<uint32_t> pixels((size_t)kWidth * kHeight);
    for (uint32_t f = 0; f < 4; ++f)
    {
        FramePacket p;
        p.frameIndex = f;
        p.deltaTime = 1.0f / 60;
        p.width = kWidth;
        p.height = kHeight;
        p.camera = h.scene().cameras[0];
        const bool last = f == 3;
        h.renderStandalone(h.queueFrame(std::move(p)), last ? pixels.data() : nullptr, last ? pixels.size() * 4 : 0);
    }
    errors += h.debugErrors();
    return pixels;
}

int channelDiff(uint32_t a, uint32_t b)
{
    int d = 0;
    for (int c = 0; c < 3; ++c) d = std::max(d, std::abs((int)((a >> (10 * c)) & 1023u) - (int)((b >> (10 * c)) & 1023u)));
    return d;
}
float luma(uint32_t p) { return 0.2126f * (p & 1023u) + 0.7152f * ((p >> 10) & 1023u) + 0.0722f * ((p >> 20) & 1023u); }
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
        // 1.
        lights::LightFunction half;
        half.intensityKeys = { float2{ 0, 0.5f } };
        const std::vector<uint32_t> withHalf = renderSpot(1000, true, &half, errors), halfLight = renderSpot(500, true, nullptr, errors),
                                    full = renderSpot(1000, true, nullptr, errors), dark = renderSpot(1000, false, nullptr, errors);
        int worst = 0;
        uint32_t lit = 0;
        for (size_t i = 0; i < withHalf.size(); ++i)
        {
            worst = std::max(worst, channelDiff(withHalf[i], halfLight[i]));
            lit += luma(full[i]) > luma(dark[i]) + 8;
        }
        logf("  constant 0.5 function vs half intensity: worst %d codes over %zu px (%u px lit by the light)\n", worst, withHalf.size(), lit);
        expect("a constant 0.5 light function = the light at half intensity (<= 1 code)", lit > 1000 && worst <= 1);

        // 2. cookie: 2 x 1 texels, left black, right white
        lights::LightFunction cookie;
        cookie.profile = lights::Profile::Cookie;
        cookie.image.width = 2;
        cookie.image.height = 1;
        cookie.image.rgb = { 0, 0, 0, 1, 1, 1 };
        cookie.tanX = cookie.tanY = 1.5f;
        const std::vector<uint32_t> cookied = renderSpot(1000, true, &cookie, errors);
        uint32_t litSide = 0, litSideOn = 0, darkSide = 0, darkSideSame = 0;
        for (uint32_t y = kHeight / 2 - 40; y < kHeight / 2 + 40; ++y)
            for (uint32_t x = 0; x < kWidth; ++x)
            {
                const size_t i = (size_t)y * kWidth + x;
                if (luma(full[i]) <= luma(dark[i]) + 8) continue;  // outside the spot's pool
                // the floor point (camera 8 m above, looking down, image right = +x): the cookie's u = 0.5 + x / (2 x 3 m x 1.5);
                // bilinear between the texel centres (u 0.25 and 0.75, x = -/+2.25 m): 0 left of -2.25 m, 1 right of 2.25 m
                // (1 px margin in world units)
                const double tanV = std::tan(spotScene(1, false).cameras[0].verticalFov * 0.5), tanH = tanV * kWidth / kHeight;
                const double fx = ((x + 0.5) / kWidth * 2 - 1) * tanH * 8, margin = 2 * tanH * 8 / kWidth;
                if (std::abs(fx) >= 4.5 - margin) continue;  // outside (or at the edge of) the cookie
                if (fx > 2.25 + margin) { ++litSide; litSideOn += channelDiff(cookied[i], full[i]) <= 1; }
                else if (fx < -2.25 - margin) { ++darkSide; darkSideSame += channelDiff(cookied[i], dark[i]) <= 1; }
            }
        logf("  cookie: white side %u px (%u lit), black side %u px (%u as without the light)\n", litSide, litSideOn, darkSide, darkSideSame);
        expect("cookie: where it is white (bilinear 1) the floor is the uncookied light's (<= 1 code)", litSide > 500 && litSideOn == litSide);
        expect("cookie: where it is black (bilinear 0) the floor gets nothing from the light (<= 1 code)", darkSide > 500 && darkSideSame == darkSide);
        expect("D3D12 debug layer errors 0", errors == 0);
        logf(failures ? "HOST LIGHT FUNCTION TEST FAILED (%u)\n" : "HOST LIGHT FUNCTION TEST PASS\n", failures);
        return failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        logf("HOST LIGHT FUNCTION TEST ERROR: %s\n", e.what());
        return 2;
    }
}
