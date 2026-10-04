// Actual DLL boundary: retain NativeVfx's executor across renderer destruction,
// including CPU checkpoint pointers consumed after the callback returns.
#include "unx/host/UnravelNextHost.h"
#include "unx/core/File.h"
#include "unx/render/Device.h"
#include "TestScenes.h"
#include "../../Render/Passes/FX/Tests/RppStream.h"
#include <cstdio>
#include <cstring>

using namespace unx;
int main()
{
    try
    {
        const auto bin = executableDirectory();
        HMODULE dll = LoadLibraryW((bin / "UnravelNext.dll").c_str());
        if (!dll) fail("LoadLibrary failed: %lu", GetLastError());
#define API(name) auto name = reinterpret_cast<decltype(&::name)>(GetProcAddress(dll, #name)); if (!name) fail("Missing " #name);
        API(UnxRendererCreate) API(UnxRendererDestroy) API(UnxSceneLoad)
        API(UnxSceneCommit) API(UnxVfxStreamExecutor) API(UnxLastError)
#undef API
        auto ok = [&](int32_t result, const char* operation = "call") { if (result != UNX_OK) fail("%s ABI %d: %s", operation, result, UnxLastError()); };
        const auto sceneFile = bin / "host_vfx_lifetime.unxscene";
        scene::save(host::test::oneBox(), sceneFile);
        UnxRendererDesc desc{};
        desc.size = sizeof desc; desc.version = 1;
        desc.flags = UNX_RENDERER_STANDALONE; desc.framesInFlight = 1;
        strcpy_s(desc.shaderDirectory, (bin / "shaders").string().c_str());
        strcpy_s(desc.qualityDirectory, (std::filesystem::path(UNX_SOURCE_DIR) / "Config/quality").string().c_str());
        auto create = [&] {
            UnxRenderer renderer = 0; ok(UnxRendererCreate(&desc, &renderer), "create");
            ok(UnxSceneLoad(renderer, sceneFile.string().c_str(), nullptr, nullptr, nullptr), "load");
            ok(UnxSceneCommit(renderer, nullptr), "commit");
            return renderer;
        };
        const auto renderer = create();
        NV_StreamExecutor executor{}; ok(UnxVfxStreamExecutor(renderer, &executor));
        fx::test::RppConfig config;
        config.emitters = 16; config.particles = 4096; config.bodies = 0;
        config.features = false; config.heightfield = false; config.sheet = false; config.rppOutputs = false;
        fx::test::RppStream stream(config);
        const auto packet = stream.next(nullptr);
        NV_StreamHeader header{}; std::memcpy(&header, packet.data(), sizeof header);
        if (executor.submit(executor.user, packet.data(), packet.size())) fail("Live submit failed");
        NV_StreamReadback readback{}; readback.size = sizeof readback; readback.version = 1;
        if (executor.readback(executor.user, header.stream, header.generation, header.tick, &readback) || readback.counters.status)
            fail("Live readback failed");
        const NV_StreamParticle* records = nullptr; uint64_t count = 0;
        if (executor.checkpoint(executor.user, header.stream, header.generation, header.tick, &records, &count) || !count)
            fail("Live checkpoint failed/empty");
        const NV_StreamParticle first = records[0];
        const NV_StreamParticleOrientation* orientations = nullptr; uint64_t orientationCount = 0;
        if (executor.checkpoint_orientations(executor.user, header.stream, header.generation, header.tick, &orientations, &orientationCount) || orientationCount != count)
            fail("Live orientations failed");
        const auto firstOrientation = orientations[0];
        ok(UnxRendererDestroy(renderer));
        ok(UnxRendererDestroy(renderer)); // managed Dispose after the device callback
        if (UnxRendererDestroy(0) == UNX_OK) fail("Unissued renderer identity accepted");
        if (std::memcmp(&records[0], &first, sizeof first) || std::memcmp(&orientations[0], &firstOrientation, sizeof firstOrientation))
            fail("Checkpoint arrays expired with their owner");
        auto closed = [&] {
            const NV_StreamParticle* stale = reinterpret_cast<const NV_StreamParticle*>(1); uint64_t staleCount = 123;
            if (executor.checkpoint(executor.user, header.stream, header.generation, header.tick, &stale, &staleCount) != 10 || stale || staleCount)
                fail("Destroyed checkpoint was not rejected/cleared");
            const NV_StreamParticleOrientation* staleOrientation = reinterpret_cast<const NV_StreamParticleOrientation*>(1); staleCount = 123;
            if (executor.checkpoint_orientations(executor.user, header.stream, header.generation, header.tick, &staleOrientation, &staleCount) != 10 || staleOrientation || staleCount)
                fail("Destroyed orientation was not rejected/cleared");
            readback.events = reinterpret_cast<const NV_StreamEvent*>(1); readback.event_count = 123;
            if (executor.readback(executor.user, header.stream, header.generation, header.tick, &readback) != 10 || readback.events || readback.event_count)
                fail("Destroyed readback was not rejected/cleared");
            // Submit must preserve NativeVfx's commit contract; its later readback reports closure.
            if (executor.submit(executor.user, packet.data(), packet.size())) fail("Destroyed submit violated commit contract");
        };
        closed();
        const auto replacement = create();
        if (replacement == renderer) fail("Renderer identity reused");
        NV_StreamExecutor replacementExecutor{}; ok(UnxVfxStreamExecutor(replacement, &replacementExecutor));
        if (replacementExecutor.submit(replacementExecutor.user, packet.data(), packet.size())) fail("Replacement submit failed");
        const NV_StreamParticle* replacementRecords = nullptr; uint64_t replacementCount = 0;
        if (replacementExecutor.checkpoint(replacementExecutor.user, header.stream, header.generation, header.tick, &replacementRecords, &replacementCount) || replacementCount != count)
            fail("Replacement checkpoint failed");
        if (replacementRecords == records || std::memcmp(&records[0], &first, sizeof first) || std::memcmp(&orientations[0], &firstOrientation, sizeof firstOrientation))
            fail("Another renderer invalidated the retained stream's arrays");
        closed(); // an allocator-reused HostRenderer address cannot retarget the old executor
        ok(UnxRendererDestroy(replacement));
        executor.detach(executor.user, header.stream);
        replacementExecutor.detach(replacementExecutor.user, header.stream);
        FreeLibrary(dll);
        std::puts("PASS: live VFX callbacks; stable CPU snapshots; all retained callbacks safe after owner destruction and replacement");
        return 0;
    }
    catch (const std::exception& error) { std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1; }
}
