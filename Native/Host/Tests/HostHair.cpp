// B10 strand hair through the host (HostRenderer::hairAddBody / hairTick / hairSetFrameFraction / hairRemoveBody, the
// UnxHair* path; E's hair::HairSystem on the render thread):
//   1. a body added on the host's thread is on the renderer's hair system with the host's id after the next frame, and
//      its guide state exists once ticks were rendered;
//   2. invalid input fails on the calling thread: a material that is not Hair-class, a tick with the wrong joint count,
//      a fraction outside [0, 1], a removed body;
//   3. removal leaves the system without the body; a new body takes the freed id (the host's mirror and the system agree);
//   4. no D3D12 debug-layer errors.
// Correctness run (standalone HostRenderer, hardware GPU; GpuLock -Kind correctness).
#include "Renderer/HostRenderer.h"

#include "TestScenes.h"
#include "unx/core/File.h"

#include <cmath>
#include <string>
#include <vector>

using namespace unx;
using namespace unx::host;

namespace
{
hair::BodyDesc bodyDesc(uint32_t material)
{
    hair::BodyDesc d;
    d.nodesPerStrand = 8;
    d.joints = 1;
    for (uint32_t g = 0; g < 2; ++g)
        for (uint32_t i = 0; i < d.nodesPerStrand; ++i) d.restPositions.push_back({ 0.02f * g, -0.03f * i, 0 });
    d.guideJoint = { 0, 0 };
    for (uint32_t f = 0; f < 4; ++f) d.follows.push_back({ f % 2, { 0, 0.002f * f, 0.001f }, 1.0f });
    d.material = material;
    return d;
}
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
        auto throws = [](auto&& f) {
            try { f(); } catch (const std::exception&) { return true; }
            return false;
        };
        HostRendererOptions o;
        o.standalone = true;
        o.framesInFlight = 2;
        o.debugLayer = true;
        o.shaderDirectory = executableDirectory() / "shaders";
        o.qualityDirectory = std::filesystem::path(UNX_SOURCE_DIR) / "Config/quality";
        HostRenderer h(o);
        h.scene() = host::test::oneBox();
        scene::Material hairMaterial = h.scene().materials[0];
        hairMaterial.cls = scene::MaterialClass::Hair;
        hairMaterial.ior = 1.55f;
        hairMaterial.roughness = 0.3f;
        hairMaterial.hairEumelanin = 1.3f;
        h.scene().materials.push_back(hairMaterial);
        h.commit();
        uint64_t index = 0;
        auto frame = [&] {
            FramePacket p;
            p.frameIndex = index++;
            p.deltaTime = 1.0f / 60;
            p.width = 320;
            p.height = 180;
            p.camera = h.scene().cameras[0];
            h.renderStandalone(h.queueFrame(std::move(p)), nullptr, 0);
        };
        auto tick = [&](uint32_t body, float y) {
            float3x4 joint;
            joint.m[1][3] = y;
            const float3x4 joints[1] = { joint };
            const hair::Capsule caps[1] = { { { 0, 0, 0 }, 0.1f, { 0, -0.2f, 0 }, 0 } };
            h.hairTick(body, joints, caps, { 1, 0, 0 }, 1.0f / 60);
        };
        hair::HairSystem& system = hair::hairSystem(h.trackStateForTest());

        // 1.
        const uint32_t body = h.hairAddBody(bodyDesc(1));
        for (int t = 0; t < 3; ++t)
        {
            tick(body, 1.0f + 0.05f * t);
            h.hairSetFrameFraction(0.5f);
            frame();
        }
        expect("the body is on the renderer's hair system with the host's id", body == 0 && system.bodies().size() == 1);
        expect("its guide state exists after rendered ticks", system.tickState(body).valid());
        expect("the frame fraction reached the system", system.fraction() == 0.5f);

        // 2.
        expect("a material that is not Hair-class is refused on the calling thread", throws([&] { h.hairAddBody(bodyDesc(0)); }));
        expect("a tick with the wrong joint count is refused", throws([&] {
            const float3x4 two[2];
            h.hairTick(body, two, {}, { 0, 0, 0 }, 1.0f / 60);
        }));
        expect("a fraction outside [0, 1] is refused", throws([&] { h.hairSetFrameFraction(1.5f); }));

        // 3.
        h.hairRemoveBody(body);
        frame();
        expect("removal leaves the system without the body", system.bodies().empty());
        expect("a removed body is refused", throws([&] { tick(body, 0); }));
        const uint32_t again = h.hairAddBody(bodyDesc(1));
        tick(again, 1.0f);
        frame();
        expect("a new body takes the freed id on both sides", again == body && system.bodies().size() == 1);

        expect("D3D12 debug layer errors 0", h.debugErrors() == 0);
        logf(failures ? "HOST HAIR TEST FAILED (%u)\n" : "HOST HAIR TEST PASS\n", failures);
        return failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        logf("HOST HAIR TEST ERROR: %s\n", e.what());
        return 2;
    }
}
