// Automatic exposure through the host (render A item A4; FEATURES_GAME 6.2, M Exposure.cpp): a camera without an EV100
// (NaN) gets the renderer's metered exposure.
//   1. Metering math (meterEv100) on synthetic histograms: one bin -> log2 L of its centre minus log2(1.2 x grey); the
//      dark and bright cuts drop exactly their fractions; an empty histogram is not a meter.
//   2. Day (the default sun) and night (the sun 30 degrees below the horizon) on the one-box scene: the first metered
//      frame snaps to the target, later frames hold it; the night EV is several stops below the day's.
//   3. Adaptation: after the scene turns from day to night (no cut), the EV moves towards the night target as
//      target + (ev0 - target) exp(-t / tau_darker) at the frames' own delta time, within 0.02 EV at t = 1 s.
// Correctness run (standalone HostRenderer, hardware GPU; GpuLock -Kind correctness).
#include "Renderer/HostRenderer.h"

#include "TestScenes.h"
#include "unx/core/File.h"
#include "unx/shading/Exposure.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>

using namespace unx;
using namespace unx::host;
using namespace unx::render;

int main()
{
    try
    {
        uint32_t failures = 0;
        auto expect = [&](const char* what, bool ok) {
            logf("  %-80s %s\n", what, ok ? "ok" : "FAILED");
            if (!ok) ++failures;
        };

        // 1. Metering.
        {
            uint32_t bins[64] = {};
            bins[40] = 1000;  // log2 L = -8 + 40.5 x 0.5 = 12.25
            bool valid = false;
            const float ev = shading::meterEv100(bins, 0.18f, 0.0f, 0.0f, valid);
            expect("one bin: EV = log2 L - log2(1.2 x 0.18)", valid && std::abs(ev - (12.25f - std::log2(1.2f * 0.18f))) < 1e-4f);
            bins[0] = 100;   // 10 % very dark
            bins[63] = 100;  // 10 % very bright (of 1200)
            const float cut = shading::meterEv100(bins, 0.18f, 100.0f / 1200.0f, 100.0f / 1200.0f, valid);
            expect("cuts drop exactly the darkest and brightest fractions", valid && std::abs(cut - ev) < 1e-4f);
            uint32_t empty[64] = {};
            shading::meterEv100(empty, 0.18f, 0.05f, 0.02f, valid);
            expect("an empty histogram is not a meter", !valid);
        }

        HostRendererOptions o;
        o.standalone = true;
        o.framesInFlight = 2;
        o.shaderDirectory = executableDirectory() / "shaders";
        o.qualityDirectory = std::filesystem::path(UNX_SOURCE_DIR) / "Config/quality";
        const QualityConfig quality = QualityConfig::loadDirectory(o.qualityDirectory);
        const float tauDarker = (float)quality.number("shading.exposure_adapt_darker_seconds");
        HostRenderer h(o);
        h.scene() = test::oneBox();
        const scene::Sun day = h.scene().sun;
        scene::Sun night = day;
        night.direction = normalize(float3{ 0.3f, -0.5f, 0.2f });
        h.commit();
        scene::Camera camera = h.scene().cameras[0];
        camera.ev100 = std::numeric_limits<float>::quiet_NaN();  // automatic exposure
        uint64_t index = 0;
        auto frame = [&](float dt) {
            FramePacket p;
            p.frameIndex = index++;
            p.deltaTime = dt;
            p.width = 640;
            p.height = 360;
            p.camera = camera;
            h.renderStandalone(h.queueFrame(std::move(p)), nullptr, 0);
            return h.lastEv100ForTest();
        };

        // 2. Day: the first metered frame (framesInFlight after the first) snaps; then it holds.
        std::vector<float> dayEv;
        for (int i = 0; i < 12; ++i) dayEv.push_back(frame(1.0f / 60));
        const float evDay = dayEv.back();
        logf("  day EV100: first frames %.3f %.3f %.3f, settled %.3f\n", dayEv[0], dayEv[1], dayEv[2], evDay);
        expect("day: the metered exposure holds (last 6 frames within 0.02 EV)", std::abs(dayEv[6] - evDay) < 0.02f);
        expect("day: metered, not the starting value", std::abs(evDay - 14.0f) > 0.05f);

        // 3. Night without a cut: adaptation at tau_darker over the frames' delta time.
        h.setSun(night);
        const float dt = 1.0f / 30;
        std::vector<float> nightEv;
        for (int i = 0; i < 240; ++i) nightEv.push_back(frame(dt));
        const float evNight = nightEv.back();
        logf("  night EV100 after 8 s: %.3f (day %.3f)\n", evNight, evDay);
        expect("night is several stops below day", evDay - evNight > 4.0f);
        // The night target is metered from the second night frame on (framesInFlight lag): from there the EV follows
        // ev(t) = target + (ev0 - target) exp(-t / tau) exactly (frame by frame with the same dt).
        const int start = 2 + 1;
        const float ev0 = nightEv[start - 1], target = evNight;  // after 8 s = 4 tau, within 2 % of the target
        const int steps = (int)std::lround(1.0 / dt);
        const float predicted = target + (ev0 - target) * std::exp(-steps * dt / tauDarker);
        const float measured = nightEv[start - 1 + steps];
        logf("  adaptation: ev0 %.3f, after 1 s %.3f, predicted %.3f (tau %.2f s)\n", ev0, measured, predicted, tauDarker);
        expect("adaptation follows the exponential over delta time (1 s, within 0.03 EV of the closed form)", std::abs(measured - predicted) < 0.03f + 0.02f * std::abs(ev0 - target));

        // A cut: the EV holds while the meter still shows the view before it (framesInFlight frames), then snaps the
        // whole way to the new view's target in one frame (adaptation would move ~5 % per frame). The target itself is
        // the cut frame's meter (its GI has not reconverged after the sun jump), so it matches the settled day within
        // 0.15 EV only.
        h.setSun(day);
        h.setDiscontinuity(kDiscontinuityCut);
        const float before = nightEv.back();
        const float c0 = frame(dt), c1 = frame(dt), c2 = frame(dt);
        logf("  cut back to day: %.3f %.3f %.3f (night %.3f, settled day %.3f)\n", c0, c1, c2, before, evDay);
        expect("a cut holds the EV while the meter shows the old view", c0 == before && c1 == before);
        expect("then snaps the whole way to the new view's target", c2 - before > 0.95f * (evDay - before) && std::abs(c2 - evDay) < 0.15f);
        expect("D3D12 debug layer errors 0", h.debugErrors() == 0);
        logf(failures ? "HOST EXPOSURE TEST FAILED (%u)\n" : "HOST EXPOSURE TEST PASSED\n", failures);
        return failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL %s\n", e.what());
        return 1;
    }
}
