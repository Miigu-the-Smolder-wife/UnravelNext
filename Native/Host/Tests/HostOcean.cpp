// B7 sea input through the host (HostRenderer::setOcean / UnxFrameSetOcean, FrameContext::ocean, INTERFACES v1.72):
//   1. the sea the queued frames take equals the input in world coordinates, and after an origin shift of (1024, -2048, 3072)
//      its level and lake centre are in the new coordinates (the spectrum unchanged);
//   2. frames with the sea render (no debug-layer errors), and null clears it;
//   3. invalid seas are refused on the calling thread (negative wind, no fetch, bounds, a lake without a radius, non-finite
//      level).
// Correctness run (standalone HostRenderer, hardware GPU; GpuLock -Kind correctness). --warp: 1 and 3 on the software
// adapter without frames.
#include "Renderer/HostRenderer.h"

#include "TestScenes.h"
#include "unx/core/File.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <dxgi1_6.h>
#include <limits>
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")

using namespace unx;
using namespace unx::host;
using namespace unx::render;

namespace
{
// (diagnosis) the faulting function and line of an access violation or integer division by zero
LONG WINAPI crashReport(EXCEPTION_POINTERS* e)
{
    const DWORD code = e->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_ACCESS_VIOLATION) return EXCEPTION_CONTINUE_SEARCH;
    HANDLE process = GetCurrentProcess();
    SymInitialize(process, nullptr, TRUE);
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256] = {};
    SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = 255;
    const DWORD64 address = (DWORD64)e->ExceptionRecord->ExceptionAddress;
    DWORD64 displacement = 0;
    IMAGEHLP_LINE64 line{ sizeof(IMAGEHLP_LINE64) };
    DWORD lineDisplacement = 0;
    const bool haveSymbol = SymFromAddr(process, address, &displacement, symbol) != 0;
    const bool haveLine = SymGetLineFromAddr64(process, address, &lineDisplacement, &line) != 0;
    std::fprintf(stderr, "exception 0x%08lx at %s (%s:%lu)\n", code, haveSymbol ? symbol->Name : "?", haveLine ? line.FileName : "?", haveLine ? line.LineNumber : 0);
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
} // namespace

int main(int argc, char** argv)
{
    AddVectoredExceptionHandler(1, crashReport);
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
                p.deltaTime = 1.0f / 60;
                p.width = 320;
                p.height = 180;
                p.camera = h.scene().cameras[0];
                h.renderStandalone(h.queueFrame(std::move(p)), nullptr, 0);
            }
        };

        // 1.
        HostRenderer::OceanInput sea;
        sea.windSpeed = 12, sea.windDirection = 0.7f, sea.fetch = 80000, sea.spread = 8, sea.seed = 5;
        sea.level = 2.5;
        sea.horizontalBound = 6, sea.verticalBound = 4;
        sea.lake = true;
        sea.lakeCentre[0] = 100, sea.lakeCentre[1] = -40;
        sea.lakeRadius = 300;
        expect("no sea before one is set", !h.queuedOcean());
        h.setOcean(&sea);
        std::optional<OceanFrame> f = h.queuedOcean();
        expect("the queued frames take the sea (world coordinates before any shift)",
               f && f->windSpeed == 12 && f->windDirection == 0.7f && f->fetch == 80000 && f->spread == 8 && f->seed == 5 && f->level == 2.5f &&
                   f->horizontalBound == 6 && f->verticalBound == 4 && f->lake == 1 && f->lakeCentre[0] == 100 && f->lakeCentre[1] == -40 && f->lakeRadius == 300);
        frames(2);
        h.setOriginShift(float3{ 1024, -2048, 3072 });
        f = h.queuedOcean();
        logf("  after the shift: level %g, lake centre (%g, %g)\n", f ? f->level : 0.0f, f ? f->lakeCentre[0] : 0.0f, f ? f->lakeCentre[1] : 0.0f);
        expect("after an origin shift: level and lake centre in the new coordinates, spectrum unchanged",
               f && f->level == 2050.5f && f->lakeCentre[0] == -924 && f->lakeCentre[1] == -3112 && f->windSpeed == 12 && f->seed == 5);

        // 2.
        frames(3);
        h.setOcean(nullptr);
        expect("null clears the sea", !h.queuedOcean());
        frames(1);

        // 3.
        HostRenderer::OceanInput bad = sea;
        bad.windSpeed = -1;
        expect("negative wind speed is refused", throws([&] { h.setOcean(&bad); }));
        bad = sea;
        bad.fetch = 0;
        expect("a fetch of 0 is refused", throws([&] { h.setOcean(&bad); }));
        bad = sea;
        bad.verticalBound = 0;
        expect("a displacement bound of 0 is refused", throws([&] { h.setOcean(&bad); }));
        bad = sea;
        bad.lakeRadius = 0;
        expect("a lake without a radius is refused", throws([&] { h.setOcean(&bad); }));
        bad = sea;
        bad.level = std::numeric_limits<double>::quiet_NaN();
        expect("a non-finite level is refused", throws([&] { h.setOcean(&bad); }));
        expect("a refused sea leaves none set", !h.queuedOcean());
        expect("D3D12 debug layer errors 0", h.debugErrors() == 0);
        logf(failures ? "HOST OCEAN TEST FAILED (%u)\n" : "HOST OCEAN TEST PASS\n", failures);
        return failures ? 1 : 0;
    }
    catch (const std::exception& e)
    {
        logf("HOST OCEAN TEST ERROR: %s\n", e.what());
        return 2;
    }
}
