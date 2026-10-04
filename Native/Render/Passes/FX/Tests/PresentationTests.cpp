// Immutable presentation regression: snapshot every render input, overwrite simulation in the same graph, then read
// the snapshot again. Repeat with RESET/generation change and capacity growth; collect retired sources before execute.
// This deliberately delays the consumer past simulation, instead of proving only a copy made before an overwrite.
#include "RppStream.h"
#include "unx/core/Config.h"
#include "unx/core/File.h"
#include "unx/core/Log.h"
#include "unx/fx/Particles.h"
#include "unx/render/Shaders.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <string>

using namespace unx;
using namespace unx::render;

namespace
{
void require(bool value, const char* message) { if (!value) fail("FX presentation: %s", message); }

std::vector<BufferRef> buffers(const fx::ParticleRenderInputs& p)
{
    std::vector<BufferRef> result = { p.posAge[0], p.posAge[1], p.velocity[0], p.velocity[1], p.dynamic[0], p.dynamic[1],
        p.emitters, p.programs, p.curveKeys, p.renderRanges, p.renderBlocks };
    if (p.ribbonRanges.valid()) { result.push_back(p.ribbonRanges); result.push_back(p.ribbonRows); }
    if (p.orientation[0].valid()) { result.push_back(p.orientation[0]); result.push_back(p.orientation[1]); }
    return result;
}

struct Readback
{
    ComPtr<ID3D12Resource> resource;
    uint64_t bytes = 0;
    std::vector<uint8_t> read() const
    {
        void* data = nullptr;
        D3D12_RANGE range{ 0, (SIZE_T)bytes };
        check(resource->Map(0, &range, &data), "map FX presentation test");
        std::vector<uint8_t> result((const uint8_t*)data, (const uint8_t*)data + bytes);
        D3D12_RANGE none{ 0, 0 };
        resource->Unmap(0, &none);
        return result;
    }
};

Readback readback(Device& device, RenderGraph& graph, BufferRef input)
{
    Readback result;
    result.bytes = graph.desc(input).size;
    D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_READBACK };
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = std::max<uint64_t>(result.bytes, 256);
    desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    check(device.d3d()->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                               nullptr, IID_PPV_ARGS(&result.resource)), "FX presentation readback");
    graph.addPass("fx.test.presentation.readback", QueueType::Graphics,
                  [input](PassBuilder& b) { b.use(input, Use::CopySrc); b.keep(); },
                  [input, result](PassContext& c) {
                      c.cmd->CopyBufferRegion(result.resource.Get(), 0, c.resource(input), 0, result.bytes);
                  });
    return result;
}

void run(Device& device)
{
    const QualityConfig quality = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
    ShaderLibrary shaders(device, executableDirectory() / "shaders");
    RenderGraph graph(device);
    fx::ParticleSystem ps(device, quality);
    fx::test::RppConfig config;
    config.emitters = 16; config.particles = 4096;
    config.features = false; config.bodies = 0; config.heightfield = false; config.sheet = false;
    config.ribbons = 2; config.ribbonPoints = 256; config.volumes = 2; config.volumeParticles = 256;
    config.lightPrograms = 1;
    fx::test::RppStream stream(config);
    uint64_t frame = 0;
    NV_StreamHeader latest{};
    auto submit = [&](const std::vector<uint8_t>& packet) {
        latest = *reinterpret_cast<const NV_StreamHeader*>(packet.data());
        if (frame & 1u)
        {
            auto owned = packet;
            ps.submitOwned(std::move(owned));
        }
        else ps.submit(packet.data(), packet.size());
        ps.record(graph, shaders, frame, QueueType::Graphics);
    };
    auto finish = [&] {
        graph.execute(nullptr);
        device.waitIdle();
        const auto counters = ps.readback(latest.stream, latest.generation, latest.tick).counters;
        require((counters.status & ~1u) == 0, "simulation status");
        require(counters.alive == latest.alive_after, "simulation alive count");
        ++frame;
    };
    bool rejected = false;
    try { ps.submitOwned(std::vector<uint8_t>(4)); }
    catch (const std::exception&) { rejected = true; }
    require(rejected && ps.pendingTicks() == 0, "owned submission bypassed stream validation");
    require(!ps.renderInputs(graph, frame).valid && ps.presentationSources(graph, frame).empty(), "empty stream snapshot");
    for (uint32_t i = 0; i < 2; ++i)
    {
        auto packet = stream.next(nullptr);
        reinterpret_cast<NV_StreamHeader*>(packet.data())->slot_capacity = 8192;
        submit(packet);
        finish();
    }

    // Both parity allocations are large enough, so the first mutation overwrites existing resources. The second
    // mutation explicitly reallocates the pair and replaces tables from a different generation and stream.
    for (uint32_t experiment = 0; experiment < 2; ++experiment)
    {
        const auto rawPrevious = ps.readState("posAgePrev"), rawCurrent = ps.readState("posAge");
        const auto snapshot = ps.renderInputs(graph, frame);
        require(snapshot.valid && snapshot.tick == latest.tick && snapshot.generation == latest.generation &&
                snapshot.stream == latest.stream, "snapshot producer identity");
        require(snapshot.lights && !snapshot.lights->chunks.empty(), "snapshot light ranges missing");
        const auto refs = buffers(snapshot);
        const auto sources = ps.presentationSources(graph, frame);
        require(refs.size() == 15 && sources.size() == refs.size(), "ribbon/orientation/table sources missing");
        require(ps.presentationSources(graph, frame + 1).empty(), "source lease escaped its frame");
        const uint32_t passes = graph.passCount();
        const auto again = ps.renderInputs(graph, frame);
        require(again.posAge[0].id == snapshot.posAge[0].id && graph.passCount() == passes, "duplicate snapshot copy");
        std::vector<Readback> before, after;
        for (BufferRef ref : refs) before.push_back(readback(device, graph, ref));

        auto packet = stream.next(nullptr);
        reinterpret_cast<NV_StreamHeader*>(packet.data())->slot_capacity = ps.capacity();
        if (experiment == 1)
        {
            fx::test::RppStream resetStream(config);
            packet = resetStream.next(nullptr);
            auto& header = *reinterpret_cast<NV_StreamHeader*>(packet.data());
            header.generation = latest.generation + 1;
            header.stream = latest.stream + 1;
            header.slot_capacity = std::max(header.slot_capacity, ps.capacity() * 2);
        }
        submit(packet);
        const auto pinned = ps.renderInputs(graph, frame);
        require(pinned.tickSerial == snapshot.tickSerial && pinned.generation == snapshot.generation &&
                pinned.stream == snapshot.stream && pinned.reset == snapshot.reset && pinned.tickTime == snapshot.tickTime &&
                pinned.capacity == snapshot.capacity && pinned.lights == snapshot.lights &&
                std::memcmp(pinned.anchor, snapshot.anchor, sizeof snapshot.anchor) == 0, "snapshot metadata changed with simulation");
        const auto pinnedRefs = buffers(pinned);
        for (size_t i = 0; i < refs.size(); ++i)
        {
            require(pinnedRefs[i].id == refs[i].id, "snapshot GPU handles changed with simulation");
            after.push_back(readback(device, graph, refs[i]));
        }
        // Old source buffers were retired by growth, but the presentation pass has not been submitted yet.
        device.collectGarbage();
        finish();
        for (size_t i = 0; i < refs.size(); ++i)
            require(before[i].read() == after[i].read(), "simulation changed a delayed render consumer's snapshot");
        require(before[0].read() == rawPrevious && before[1].read() == rawCurrent, "snapshot differs from authoritative interpolation pair");
    }

    // A new frame sees the RESET's actual identity, and its source fence precedes snapshot-only consumers.
    const auto reset = ps.renderInputs(graph, frame);
    require(reset.valid && reset.reset && reset.stream == latest.stream && reset.generation == latest.generation &&
            reset.tick == latest.tick, "next frame did not observe reset generation");
    std::vector<Readback> resetReadbacks;
    for (BufferRef ref : buffers(reset)) resetReadbacks.push_back(readback(device, graph, ref));
    uint64_t sourceFence = 0;
    require(graph.fenceAfterImportedReads(ps.presentationSources(graph, frame),
                                        [&](Queue&, uint64_t fence) { sourceFence = fence; }), "source fence registration");
    graph.execute(nullptr);
    device.waitIdle();
    require(sourceFence && sourceFence < graph.lastFence(QueueType::Graphics), "source fence waited for presentation consumers");
    require(device.drainDebugMessages() == 0, "D3D12 debug errors");
    logf("FX presentation: all 15 inputs immutable across overwrite and reset/growth; pair bytes and identity exact; source released before consumers.\n");
}

void logicalImportPlans(Device& device)
{
    const QualityConfig quality = QualityConfig::loadDirectory(std::string(UNX_SOURCE_DIR) + "/Config/quality");
    ShaderLibrary shaders(device, executableDirectory() / "shaders");
    RenderGraph graph(device);
    fx::ParticleSystem ps(device, quality);
    fx::test::RppConfig config;
    config.emitters = 1; config.particles = 240; config.fields = 0;
    config.features = config.mesh = config.heightfield = config.sheet = config.turbulence = false;
    config.bodies = config.ribbons = config.volumes = 0;
    fx::test::RppStream stream(config);
    const uint32_t ticks = 3 * (uint32_t)quality.integer("fx.particles.readback_slots");
    constexpr uint64_t importIndex = 41; // deliberately repeated after execute clears all graph handles
    for (uint32_t frame = 0; frame < ticks; ++frame)
    {
        auto packet = stream.next(nullptr);
        auto& header = *reinterpret_cast<NV_StreamHeader*>(packet.data());
        header.slot_capacity = 4096; // hold allocation topology fixed while parity and the event ring rotate
        ps.submit(packet.data(), packet.size());
        ps.record(graph, shaders, importIndex, QueueType::Graphics);
        if (frame == 0)
        {
            graph.execute(nullptr);
            device.waitIdle();
            (void)ps.readback(header.stream, header.generation, header.tick);
            continue; // the first tick has not allocated the previous dynamic table yet
        }
        const auto snapshot = ps.renderInputs(graph, importIndex);
        require(snapshot.valid && snapshot.stream == header.stream && snapshot.generation == header.generation &&
                snapshot.tick == header.tick, "logical imports changed publication identity");
        const auto previous = readback(device, graph, snapshot.posAge[0]);
        const auto current = readback(device, graph, snapshot.posAge[1]);
        graph.execute(nullptr);
        device.waitIdle();
        require(ps.presentationSources(graph, importIndex).empty(), "completed recording retained a presentation source lease");
        // The first two ticks allocate both dynamic parities. Tick three establishes the steady topology;
        // every subsequent tick must reuse it, including previously unseen and wrapped event-ring slots.
        if (frame >= 3) require(graph.stats().planReused, "physical parity or event slot changed the logical render plan");
        const auto report = ps.readback(header.stream, header.generation, header.tick);
        require((report.counters.status & ~1u) == 0 && report.counters.alive == header.alive_after,
                "logical imports changed simulation results");
        require(previous.read() == ps.readState("posAgePrev") && current.read() == ps.readState("posAge"),
                "logical imports swapped the authoritative interpolation pair");
    }
    // An abandoned recording never executes, so only a globally unique recording identity distinguishes it from
    // a new graph reconstructed at precisely the same address with the same application import index.
    std::optional<RenderGraph> reusedAddress;
    reusedAddress.emplace(device);
    require(ps.renderInputs(*reusedAddress, importIndex).valid, "abandoned recording snapshot");
    const auto* address = &*reusedAddress;
    reusedAddress.reset();
    reusedAddress.emplace(device);
    require(&*reusedAddress == address, "graph-address reuse fixture");
    require(ps.presentationSources(*reusedAddress, importIndex).empty(), "new graph inherited an abandoned source lease");
    const auto renewed = ps.renderInputs(*reusedAddress, importIndex);
    require(renewed.valid && reusedAddress->passCount() == 1, "new graph reused abandoned presentation handles");
    const auto renewedCurrent = readback(device, *reusedAddress, renewed.posAge[1]);
    reusedAddress->execute(nullptr);
    device.waitIdle();
    require(renewedCurrent.read() == ps.readState("posAge"), "reconstructed graph imported stale physical handles");
    require(device.drainDebugMessages() == 0, "logical import D3D12 debug errors");
    logf("FX presentation: logical graph plan reused across physical parity and event-ring rotation; pair bytes and tick identity exact.\n");
}
}

int main(int argc, char** argv)
{
    try
    {
        DeviceOptions options;
        for (int i = 1; i < argc; ++i)
        {
            if (std::string(argv[i]) == "--no-debug-layer") options.debugLayer = false;
            else fail("unknown option %s", argv[i]);
        }
        Device device(options);
        run(device);
        logicalImportPlans(device);
        return 0;
    }
    catch (const std::exception& e) { logf("FAIL: %s\n", e.what()); return 1; }
}
