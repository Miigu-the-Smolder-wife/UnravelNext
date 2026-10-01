// W2 closed basins through the host (HostRenderer::setPools / addPoolSources, UnxFrameSetPools / UnxFrameAddPoolSources,
// FrameContext::pools, INTERFACES v1.78):
//   1. the basins the queued frames take equal the input in world coordinates; after an origin shift of
//      (1024, -2048, 3072) their centres and the pending sources are in the new coordinates;
//   2. sources go to exactly one frame (the next queued), and a basin left out of a later set drops its pending sources;
//   3. frames with basins and sources render (no debug-layer errors) - W's pool stream in a full frame;
//   4. invalid basins and sources are refused on the calling thread.
// Correctness run (standalone HostRenderer, hardware GPU; GpuLock -Kind correctness). --warp: 1, 2 and 4 on the software
// adapter without frames.
#include "Renderer/HostRenderer.h"

#include "TestScenes.h"
#include "unx/core/File.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <dxgi1_6.h>
#include <limits>

using namespace unx;
using namespace unx::host;
using namespace unx::render;

int main(int argc, char** argv)
{
    try
    {
        const bool warp = argc > 1 && std::strcmp(argv[1], "--warp") == 0;
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
        o.debugLayer = !warp;
        ComPtr<ID3D12Device> warpDevice;
        if (warp)
        {
            ComPtr<IDXGIFactory6> factory;
            check(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory)), "DXGI factory");
            ComPtr<IDXGIAdapter> adapter;
            check(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)), "WARP adapter");
            if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_2, IID_PPV_ARGS(&warpDevice))))
                check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_1, IID_PPV_ARGS(&warpDevice)), "WARP device");
            o.standaloneDevice = warpDevice.Get();
        }
        o.shaderDirectory = executableDirectory() / "shaders";
        o.qualityDirectory = std::filesystem::path(UNX_SOURCE_DIR) / "Config/quality";
        HostRenderer h(o);
        h.scene() = host::test::oneBox();
        h.commit();
        uint64_t index = 0;
        auto frames = [&](uint32_t n) {
            if (warp) return;  // (the standalone frame path on the software adapter faults inside the system's display mux query)
            for (uint32_t f = 0; f < n; ++f)
            {
                FramePacket p;
                p.frameIndex = index++;
                p.time = double(index) / 60;
                p.deltaTime = 1.0f / 60;
                p.width = 320;
                p.height = 180;
                p.camera = h.scene().cameras[0];
                h.renderStandalone(h.queueFrame(std::move(p)), nullptr, 0);
            }
        };

        // 1. A 4 x 3 m bath in front of the test camera and a 25 x 12.5 m pool elsewhere.
        std::vector<HostRenderer::PoolInput> set(2);
        set[0].id = 7, set[0].material = 0, set[0].sizeX = 4, set[0].sizeZ = 3, set[0].depth = 0.6f, set[0].surfaceFilm = 1;
        set[0].centre[0] = h.scene().cameras[0].position.x, set[0].centre[1] = h.scene().cameras[0].position.y - 1.5, set[0].centre[2] = h.scene().cameras[0].position.z - 4;
        set[0].yaw = 0.3f;
        set[1].id = 9, set[1].sizeX = 25, set[1].sizeZ = 12.5f, set[1].depth = 2;
        set[1].centre[0] = 100, set[1].centre[1] = 0, set[1].centre[2] = -40;
        h.setPools(set);
        auto [pools, sources] = h.queuedPools();
        expect("the queued frames take the basins (world coordinates before any shift)",
               pools.size() == 2 && pools[0].id == 7 && pools[0].sizeX == 4 && pools[0].depth == 0.6f && pools[0].surfaceFilm == 1 && pools[0].yaw == 0.3f &&
                   pools[1].id == 9 && pools[1].centre[0] == 100 && pools[1].centre[2] == -40 && sources.empty());
        std::vector<FramePacket::PoolSource> s(2);
        s[0].pool = 9, s[0].source = PoolSourceFrame{ 101.5, -38.0, 0.2f, 3.0f, 0.01f };
        s[1].pool = 7, s[1].source = PoolSourceFrame{ set[0].centre[0] + 0.5, set[0].centre[2], 0.1f, 1.0f, 0.0f };
        h.addPoolSources(s);
        h.setOriginShift(float3{ 1024, -2048, 3072 });
        std::tie(pools, sources) = h.queuedPools();
        logf("  after the shift: pool 9 centre (%g, %g, %g), its source (%g, %g)\n", pools[1].centre[0], pools[1].centre[1], pools[1].centre[2], sources.size() ? sources[0].source.x : 0.0,
             sources.size() ? sources[0].source.z : 0.0);
        expect("after an origin shift: basin centres and pending sources in the new coordinates",
               pools.size() == 2 && pools[1].centre[0] == -924 && pools[1].centre[1] == 2048 && pools[1].centre[2] == -3112 && sources.size() == 2 &&
                   sources[0].source.x == 101.5 - 1024 && sources[0].source.z == -38.0 - 3072 && sources[0].source.volume == 0.01f);

        // 2.
        {
            FramePacket p;
            p.frameIndex = index;
            p.time = double(index) / 60;
            p.deltaTime = 1.0f / 60;
            p.width = 320;
            p.height = 180;
            p.camera = h.scene().cameras[0];
            const uint64_t ticket = h.queueFrame(std::move(p));
            ++index;
            expect("the next queued frame takes the sources: none left pending", h.queuedPools().second.empty());
            if (!warp) h.renderStandalone(ticket, nullptr, 0);
        }
        h.addPoolSources(std::vector<FramePacket::PoolSource>{ s[0] });
        h.setPools(std::vector<HostRenderer::PoolInput>{ set[0] });
        expect("a basin left out of the set drops its pending sources", h.queuedPools().second.empty() && h.queuedPools().first.size() == 1);
        h.setPools(set);

        // 3. frames with basins (the bath in view) and sources every frame
        for (int f = 0; f < 12; ++f)
        {
            FramePacket::PoolSource hit;  // world coordinates: the host subtracts its origin offset when the frame is queued
            hit.pool = 7;
            hit.source = PoolSourceFrame{ set[0].centre[0] + 0.3 * std::sin(0.5 * f), set[0].centre[2], 0.08f, 0.5f, 2e-4f };
            h.addPoolSources(std::vector<FramePacket::PoolSource>{ hit });
            frames(1);
        }
        if (!warp)
        {
            // 3b. the basin's surface statistics (UnxPoolStatsLatest): valid after framesInFlight + 1 records, finite, the
            // splashes seen (RMS > 0); a basin not in the set has none.
            water::PoolStats st;
            expect("pool statistics of the bath are valid after 12 frames", h.poolStats(7, st) && st.valid);
            expect("pool statistics are finite and the splashes left a non-zero RMS and deviation",
                   std::isfinite(st.mean) && std::isfinite(st.rms) && std::isfinite(st.maxDeviation) && st.rms > 0 && st.maxDeviation >= st.rms);
            expect("pool statistics name a frame of the run", st.frame < 12);
            expect("a basin not in the set has no statistics", !h.poolStats(8, st));
        }
        h.setPools({});
        expect("an empty set clears the basins", h.queuedPools().first.empty());
        frames(2);
        if (!warp)
        {
            water::PoolStats st;
            expect("a basin removed from the set loses its statistics", !h.poolStats(7, st));
        }

        // 4.
        auto refused = [&](const char* what, auto change) {
            std::vector<HostRenderer::PoolInput> bad = set;
            change(bad);
            expect(what, throws([&] { h.setPools(bad); }));
        };
        refused("a basin with id 0 is refused", [](auto& b) { b[0].id = 0; });
        refused("two basins with the same id are refused", [](auto& b) { b[1].id = b[0].id; });
        refused("a basin of size 0 is refused", [](auto& b) { b[0].sizeZ = 0; });
        refused("a surface film other than 0 or 1 is refused", [](auto& b) { b[0].surfaceFilm = 0.5f; });
        refused("a non-finite centre is refused", [](auto& b) { b[1].centre[1] = std::numeric_limits<double>::quiet_NaN(); });
        expect("a refused set leaves the previous one (none)", h.queuedPools().first.empty());
        h.setPools(set);
        auto badSource = [&](const char* what, FramePacket::PoolSource src) { expect(what, throws([&] { h.addPoolSources(std::vector<FramePacket::PoolSource>{ src }); })); };
        FramePacket::PoolSource src = s[0];
        src.pool = 8;
        badSource("a source for an unknown basin is refused", src);
        src = s[0];
        src.source.x = 130;
        badSource("a source outside its basin is refused", src);
        src = s[0];
        src.source.radius = 0;
        badSource("a source of radius 0 is refused", src);
        src = s[0];
        src.source.impulse = std::numeric_limits<float>::infinity();
        badSource("a non-finite source is refused", src);
        expect("refused sources leave none pending", h.queuedPools().second.empty());
        expect("D3D12 debug layer errors 0", h.debugErrors() == 0);
        logf(failures ? "HOST POOLS TEST FAILED (%u)\n" : "HOST POOLS TEST PASS\n", failures);
        return failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        logf("HOST POOLS TEST ERROR: %s\n", e.what());
        return 2;
    }
}
