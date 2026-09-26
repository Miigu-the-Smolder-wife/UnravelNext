// Strand hair (track E, B10). See include/unx/hair/Hair.h and HairSimulate.hlsl.
#include "unx/hair/Hair.h"

#include "unx/core/Log.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"
#include "unx/render/Tracks.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace unx::hair
{
using namespace unx::render;

namespace
{
constexpr uint32_t kConstantSlot = 256;

struct Constants  // HairSimulate.hlsl HairConstants
{
    uint32_t guides, nodes, follows, capsules;
    uint32_t rest, state, tickPrev, tickCur;
    uint32_t guideJoint, inputs, jointsPrev, jointsCur;
    uint32_t capsuleOffset, frameNodes, frameRotations, followBuffer;
    uint32_t segments, localIterations, substeps, lodKeep;
    uint32_t segmentBase, pad0, pad1, pad2;
    float gravity[3], damping;
    float wind[3], dt;
    float globalStiffness, globalRange, localStiffness, dftlDamping;
    float collisionMargin, windDrag, frameFraction, radiusScale;
    float cameraPosition[3], rootRadius;
    float tipRadius, pad3, pad4, pad5;
};
static_assert(sizeof(Constants) == 192);

uint32_t pcg(uint32_t v)
{
    const uint32_t state = v * 747796405u + 2891336453u;
    const uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

ComPtr<ID3D12Resource> makeBuffer(Device& device, uint64_t bytes, D3D12_HEAP_TYPE type, const wchar_t* name)
{
    D3D12_HEAP_PROPERTIES heap{ type };
    D3D12_RESOURCE_DESC1 d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = std::max<uint64_t>(bytes, 16);
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (type == D3D12_HEAP_TYPE_DEFAULT) d.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> r;
    check(device.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&r)),
          "hair buffer");
    r->SetName(name);
    return r;
}
float3 rowsTransform(const float4* rows, float3 p)
{
    return { rows[0].x * p.x + rows[0].y * p.y + rows[0].z * p.z + rows[0].w, rows[1].x * p.x + rows[1].y * p.y + rows[1].z * p.z + rows[1].w,
             rows[2].x * p.x + rows[2].y * p.y + rows[2].z * p.z + rows[2].w };
}
} // namespace

struct HairSystem::Body
{
    BodyDesc desc;
    uint32_t guides = 0, nodes = 0, follows = 0;
    std::vector<float4> rest, follow, initial;
    std::vector<uint32_t> guideJoint;
    float extent = 0;  // max distance of a rest node from its root (LOD bound)
    struct Tick
    {
        std::vector<float4> jointsPrev, jointsCur, capsules;
        float3 wind;
        float dt;
    };
    std::vector<Tick> pending;
    std::vector<float4> jointsPrev, jointsCur;  // the latest tick's
    bool started = false, uploaded = false;
    // GPU (created by the pass)
    ComPtr<ID3D12Resource> restGpu, stateGpu, tickPrevGpu, tickCurGpu, jointGpu, followGpu, frameNodesGpu, frameRotationsGpu;
    BufferRef frameTickCur;
    uint64_t frameIndex = UINT64_MAX;
};

HairSystem::HairSystem() = default;
HairSystem::~HairSystem() = default;

uint32_t HairSystem::addBody(const BodyDesc& d)
{
    const uint32_t N = d.nodesPerStrand;
    if (N < 2 || N > 32) fail("hair: nodesPerStrand must be in [2, 32]");
    if (d.restPositions.empty() || d.restPositions.size() % N != 0) fail("hair: restPositions = guides x nodesPerStrand");
    const uint32_t guides = (uint32_t)(d.restPositions.size() / N);
    if (d.guideJoint.size() != guides) fail("hair: guideJoint needs one joint per guide");
    for (uint32_t j : d.guideJoint)
        if (j >= d.joints) fail("hair: guide joint %u of %u joints", j, d.joints);
    for (const BodyDesc::Follow& f : d.follows)
        if (f.guide >= guides) fail("hair: follow strand of guide %u (%u guides)", f.guide, guides);
    if (d.params.substeps == 0 || d.params.localIterations > 16) fail("hair: substeps >= 1, localIterations <= 16");
    auto b = std::make_unique<Body>();
    b->desc = d;
    b->guides = guides;
    b->nodes = N;
    b->follows = (uint32_t)d.follows.size();
    for (uint32_t g = 0; g < guides; ++g)
        for (uint32_t i = 0; i < N; ++i)
        {
            const float3 p = d.restPositions[g * N + i];
            const float3 next = i + 1 < N ? d.restPositions[g * N + i + 1] : p;
            const float len = length(next - p);
            if (i + 1 < N && !(len > 0)) fail("hair: guide %u has a zero-length segment at node %u", g, i);
            b->rest.push_back({ p.x, p.y, p.z, len });
            b->extent = std::max(b->extent, length(p - d.restPositions[g * N]));
        }
    b->guideJoint = d.guideJoint;
    for (uint32_t f = 0; f < b->follows; ++f)
    {
        const BodyDesc::Follow& x = d.follows[f];
        uint32_t guideBits;
        std::memcpy(&guideBits, &x.guide, 4);
        float g;
        std::memcpy(&g, &guideBits, 4);
        const uint32_t h = pcg(f * 2654435761u + 12345u);
        float hf;
        std::memcpy(&hf, &h, 4);
        b->follow.push_back({ x.offset.x, x.offset.y, x.offset.z, g });
        b->follow.push_back({ x.tipSpread, hf, 0, 0 });
    }
    uint32_t id;
    if (!m_free.empty())
    {
        id = m_free.back();
        m_free.pop_back();
        m_bodies[id] = std::move(b);
    }
    else
    {
        id = (uint32_t)m_bodies.size();
        m_bodies.push_back(std::move(b));
    }
    return id;
}

void HairSystem::removeBody(uint32_t body)
{
    if (body >= m_bodies.size() || !m_bodies[body]) fail("hair: no body %u", body);
    m_bodies[body].reset();
    m_free.push_back(body);
}

void HairSystem::tick(uint32_t body, const float3x4* joints, uint32_t jointCount, const Capsule* capsules, uint32_t capsuleCount, float3 wind, float dt)
{
    if (body >= m_bodies.size() || !m_bodies[body]) fail("hair: no body %u", body);
    Body& b = *m_bodies[body];
    if (jointCount != b.desc.joints) fail("hair: %u joints for a body of %u", jointCount, b.desc.joints);
    if (!(dt > 0)) fail("hair: tick dt must be positive");
    std::vector<float4> rows(3 * (size_t)jointCount);
    for (uint32_t j = 0; j < jointCount; ++j)
        for (int r = 0; r < 3; ++r) rows[3 * j + r] = { joints[j].m[r][0], joints[j].m[r][1], joints[j].m[r][2], joints[j].m[r][3] };
    if (!b.started)
    {
        // the first tick: the strands start at rest on their joints, not moving
        b.initial.resize((size_t)b.guides * b.nodes);
        for (uint32_t g = 0; g < b.guides; ++g)
            for (uint32_t i = 0; i < b.nodes; ++i)
            {
                const float4& r = b.rest[(size_t)g * b.nodes + i];
                const float3 p = rowsTransform(&rows[3 * b.guideJoint[g]], { r.x, r.y, r.z });
                b.initial[(size_t)g * b.nodes + i] = { p.x, p.y, p.z, 0 };
            }
        b.jointsPrev = rows;
        b.jointsCur = rows;
        b.started = true;
        return;
    }
    Body::Tick t;
    t.jointsPrev = b.jointsCur;
    t.jointsCur = rows;
    for (uint32_t k = 0; k < capsuleCount; ++k)
    {
        t.capsules.push_back({ capsules[k].a.x, capsules[k].a.y, capsules[k].a.z, capsules[k].radius });
        t.capsules.push_back({ capsules[k].b.x, capsules[k].b.y, capsules[k].b.z, 0 });
    }
    t.wind = wind;
    t.dt = dt;
    b.pending.push_back(std::move(t));
    b.jointsPrev = b.jointsCur;
    b.jointsCur = rows;
}

std::vector<HairSystem::Body*> HairSystem::bodies()
{
    std::vector<Body*> out;
    for (auto& b : m_bodies)
        if (b) out.push_back(b.get());
    return out;
}

BufferRef HairSystem::tickState(uint32_t body) const
{
    if (body >= m_bodies.size() || !m_bodies[body]) return {};
    return m_bodies[body]->frameTickCur;
}

HairSystem& hairSystem(TrackState& state) { return state.get<HairSystem>("hair.system"); }

namespace
{
class HairPass
{
public:
    explicit HairPass(Device& device) : m_device(device) {}
    ~HairPass()
    {
        m_device.waitIdle();
        for (Slot& s : m_slots)
            if (s.upload && s.mapped) s.upload->Unmap(0, nullptr);
    }

    void record(FramePassContext& fc, ViewResources& view, HairSystem& system)
    {
        std::vector<HairSystem::Body*> bodies;
        for (HairSystem::Body* b : system.bodies())
            if (b->started) bodies.push_back(b);
        if (bodies.empty()) return;
        if (m_slots.size() != std::max(fc.framesInFlight, 1u))
        {
            m_device.waitIdle();
            for (Slot& s : m_slots)
                if (s.upload && s.mapped) s.upload->Unmap(0, nullptr);
            m_slots.assign(std::max(fc.framesInFlight, 1u), Slot{});
        }
        RenderGraph& g = fc.graph;
        const ViewDesc& v = view.view;

        // staging layout: constants slots, then float4 inputs, then first-time uploads
        struct Dispatch
        {
            HairSystem::Body* body;
            uint32_t step, slot, groups;
        };
        std::vector<Dispatch> dispatches;
        std::vector<Constants> constants;
        std::vector<float4> inputs;
        struct Init
        {
            ID3D12Resource* target;
            uint64_t offset, bytes;
        };
        std::vector<Init> inits;
        std::vector<uint8_t> initBytes;
        auto stage = [&](ID3D12Resource* target, const void* data, uint64_t bytes) {
            inits.push_back({ target, (uint64_t)initBytes.size(), bytes });
            const uint8_t* p = static_cast<const uint8_t*>(data);
            initBytes.insert(initBytes.end(), p, p + bytes);
            while (initBytes.size() % 16) initBytes.push_back(0);
        };
        uint32_t segmentsTotal = 0;
        std::vector<uint32_t> header(1 + kBodyHeaderWords * bodies.size(), 0);
        header[0] = (uint32_t)bodies.size();
        for (size_t bi = 0; bi < bodies.size(); ++bi)
        {
            HairSystem::Body& b = *bodies[bi];
            const uint64_t nodes = (uint64_t)b.guides * b.nodes;
            if (!b.restGpu)
            {
                b.restGpu = makeBuffer(m_device, nodes * 16, D3D12_HEAP_TYPE_DEFAULT, L"hair rest");
                b.stateGpu = makeBuffer(m_device, nodes * 32, D3D12_HEAP_TYPE_DEFAULT, L"hair state");
                b.tickPrevGpu = makeBuffer(m_device, nodes * 16, D3D12_HEAP_TYPE_DEFAULT, L"hair tick prev");
                b.tickCurGpu = makeBuffer(m_device, nodes * 16, D3D12_HEAP_TYPE_DEFAULT, L"hair tick cur");
                b.jointGpu = makeBuffer(m_device, (uint64_t)b.guides * 4, D3D12_HEAP_TYPE_DEFAULT, L"hair guide joints");
                b.followGpu = makeBuffer(m_device, (uint64_t)b.follows * 32, D3D12_HEAP_TYPE_DEFAULT, L"hair follows");
                b.frameNodesGpu = makeBuffer(m_device, nodes * 16, D3D12_HEAP_TYPE_DEFAULT, L"hair frame nodes");
                b.frameRotationsGpu = makeBuffer(m_device, nodes * 16, D3D12_HEAP_TYPE_DEFAULT, L"hair frame rotations");
                b.uploaded = false;
            }
            if (!b.uploaded)
            {
                std::vector<float4> state(2 * nodes);
                for (uint64_t n = 0; n < nodes; ++n) state[2 * n] = state[2 * n + 1] = b.initial[n];
                stage(b.restGpu.Get(), b.rest.data(), nodes * 16);
                stage(b.stateGpu.Get(), state.data(), nodes * 32);
                stage(b.tickPrevGpu.Get(), b.initial.data(), nodes * 16);
                stage(b.tickCurGpu.Get(), b.initial.data(), nodes * 16);
                stage(b.jointGpu.Get(), b.guideJoint.data(), (uint64_t)b.guides * 4);
                if (b.follows) stage(b.followGpu.Get(), b.follow.data(), (uint64_t)b.follows * 32);
                b.uploaded = true;
            }
            const SimulationParams& sp = b.desc.params;
            auto base = [&](Constants& c) {
                c = {};
                c.guides = b.guides;
                c.nodes = b.nodes;
                c.follows = b.follows;
                c.gravity[0] = sp.gravity.x;
                c.gravity[1] = sp.gravity.y;
                c.gravity[2] = sp.gravity.z;
                c.damping = sp.damping;
                c.globalStiffness = sp.globalStiffness;
                c.globalRange = sp.globalRange;
                c.localStiffness = sp.localStiffness;
                c.dftlDamping = sp.dftlDamping;
                c.localIterations = sp.localIterations;
                c.substeps = sp.substeps;
                c.collisionMargin = sp.collisionMargin;
                c.windDrag = sp.windDrag;
                c.rootRadius = b.desc.rootRadius;
                c.tipRadius = b.desc.tipRadius;
                c.cameraPosition[0] = v.position.x;
                c.cameraPosition[1] = v.position.y;
                c.cameraPosition[2] = v.position.z;
            };
            for (const HairSystem::Body::Tick& t : b.pending)
            {
                Constants c;
                base(c);
                c.jointsPrev = (uint32_t)inputs.size();
                inputs.insert(inputs.end(), t.jointsPrev.begin(), t.jointsPrev.end());
                c.jointsCur = (uint32_t)inputs.size();
                inputs.insert(inputs.end(), t.jointsCur.begin(), t.jointsCur.end());
                c.capsuleOffset = (uint32_t)inputs.size();
                c.capsules = (uint32_t)(t.capsules.size() / 2);
                inputs.insert(inputs.end(), t.capsules.begin(), t.capsules.end());
                c.wind[0] = t.wind.x;
                c.wind[1] = t.wind.y;
                c.wind[2] = t.wind.z;
                c.dt = t.dt;
                dispatches.push_back({ &b, 0, (uint32_t)constants.size(), (b.guides + 63) / 64 });
                constants.push_back(c);
            }
            b.pending.clear();
            // LOD: strands crossing a pixel of the hair's cross-section ~ total projected strand length / projected area;
            // above 2, a deterministic subset with widths scaled by 1 / fraction (projected coverage kept)
            const float4* root = &b.jointsCur[0];
            const float3 centre{ root[0].w, root[1].w, root[2].w };
            const float distance = std::max(length(centre - v.position), v.nearPlane);
            const float pxPerMetre = (float)v.height / (2 * std::tan(v.verticalFov * 0.5f) * distance);
            const float widthPx = std::max(2 * b.extent * pxPerMetre, 1.0f);
            const float strandLengthPx = b.extent * pxPerMetre;
            const float density = b.follows * strandLengthPx / (widthPx * widthPx);
            const float fraction = density > 2 ? std::max(2 / density, 1.0f / (float)std::max(b.follows, 1u)) : 1.0f;
            Constants cf;
            base(cf);
            cf.jointsPrev = (uint32_t)inputs.size();
            inputs.insert(inputs.end(), b.jointsPrev.begin(), b.jointsPrev.end());
            cf.jointsCur = (uint32_t)inputs.size();
            inputs.insert(inputs.end(), b.jointsCur.begin(), b.jointsCur.end());
            cf.frameFraction = system.fraction();
            cf.lodKeep = fraction >= 1 ? 0xFFFFFFFFu : (uint32_t)std::min(4294967295.0, (double)fraction * 4294967296.0);
            cf.radiusScale = 1 / fraction;
            cf.segmentBase = segmentsTotal;
            dispatches.push_back({ &b, 1, (uint32_t)constants.size(), (b.guides + 63) / 64 });
            constants.push_back(cf);
            if (b.follows)
            {
                dispatches.push_back({ &b, 2, (uint32_t)constants.size(), (b.follows + 63) / 64 });
                constants.push_back(cf);
            }
            uint32_t* h = &header[1 + kBodyHeaderWords * bi];
            h[0] = segmentsTotal;
            h[1] = b.follows * (b.nodes - 1);
            h[2] = b.nodes - 1;
            h[3] = b.desc.material;
            h[4] = b.desc.instance;
            std::memcpy(&h[5], &fraction, 4);
            std::memcpy(&h[6], &cf.radiusScale, 4);
            segmentsTotal += b.follows * (b.nodes - 1);
        }

        const uint64_t constantsBytes = constants.size() * kConstantSlot, inputsBytes = std::max<size_t>(inputs.size(), 1) * 16,
                       headerBytes = (header.size() * 4 + 15) / 16 * 16;
        const uint64_t bytes = constantsBytes + inputsBytes + headerBytes + initBytes.size();
        Slot& slot = m_slots[fc.frame.frameIndex % m_slots.size()];
        if (!slot.upload || slot.bytes < bytes)
        {
            if (slot.upload)
            {
                slot.upload->Unmap(0, nullptr);
                m_device.deferRelease(slot.upload);
            }
            slot.bytes = std::max<uint64_t>(bytes, 64 * 1024);
            slot.upload = makeBuffer(m_device, slot.bytes, D3D12_HEAP_TYPE_UPLOAD, L"hair upload");
            D3D12_RANGE none{ 0, 0 };
            check(slot.upload->Map(0, &none, reinterpret_cast<void**>(&slot.mapped)), "map hair upload");
        }
        std::memset(slot.mapped, 0, (size_t)(constantsBytes + inputsBytes + headerBytes));

        // graph resources
        const BufferRef constantBuffer = g.createBuffer(BufferDesc{ "hair.constants", constantsBytes, 0 });
        const BufferRef inputBuffer = g.createBuffer(BufferDesc{ "hair.inputs", inputsBytes, 16 });
        const BufferRef headerBuffer = g.createBuffer(BufferDesc{ "hair.bodies", headerBytes, 0 });
        const BufferRef segments = g.createBuffer(BufferDesc{ "hair.segments", std::max<uint64_t>((uint64_t)segmentsTotal * 32, 32), 16 });
        struct Imported
        {
            BufferRef rest, state, tickPrev, tickCur, joint, follow, frameNodes, frameRotations;
        };
        std::vector<Imported> imported(bodies.size());
        for (size_t bi = 0; bi < bodies.size(); ++bi)
        {
            HairSystem::Body& b = *bodies[bi];
            const uint64_t nodes = (uint64_t)b.guides * b.nodes;
            Imported& im = imported[bi];
            im.rest = g.importBuffer(b.restGpu.Get(), BufferDesc{ "hair.rest", nodes * 16, 16 });
            im.state = g.importBuffer(b.stateGpu.Get(), BufferDesc{ "hair.state", nodes * 32, 16 });
            im.tickPrev = g.importBuffer(b.tickPrevGpu.Get(), BufferDesc{ "hair.tickPrev", nodes * 16, 16 });
            im.tickCur = g.importBuffer(b.tickCurGpu.Get(), BufferDesc{ "hair.tickCur", nodes * 16, 16 });
            im.joint = g.importBuffer(b.jointGpu.Get(), BufferDesc{ "hair.guideJoint", std::max<uint64_t>((uint64_t)b.guides * 4, 16), 4 });
            im.follow = g.importBuffer(b.followGpu.Get(), BufferDesc{ "hair.follows", std::max<uint64_t>((uint64_t)b.follows * 32, 16), 16 });
            im.frameNodes = g.importBuffer(b.frameNodesGpu.Get(), BufferDesc{ "hair.frameNodes", nodes * 16, 16 });
            im.frameRotations = g.importBuffer(b.frameRotationsGpu.Get(), BufferDesc{ "hair.frameRotations", nodes * 16, 16 });
            b.frameTickCur = im.tickCur;
            b.frameIndex = fc.frame.frameIndex;
        }

        // copy data in: constants get their descriptors at execute time (they name graph views), so the upload pass
        // writes the mapped slot then copies
        uint8_t* mapped = slot.mapped;
        ID3D12Resource* upload = slot.upload.Get();
        std::memcpy(mapped + constantsBytes, inputs.data(), inputs.size() * 16);
        std::memcpy(mapped + constantsBytes + inputsBytes, header.data(), header.size() * 4);
        std::memcpy(mapped + constantsBytes + inputsBytes + headerBytes, initBytes.data(), initBytes.size());
        const uint64_t initBase = constantsBytes + inputsBytes + headerBytes;
        std::vector<Dispatch> ds = dispatches;
        std::vector<Constants> cs = constants;
        std::vector<Imported> ims = imported;
        std::vector<HairSystem::Body*> bs = bodies;
        g.addPass("hair.upload", QueueType::Graphics,
                  [=](PassBuilder& b) {
                      b.use(constantBuffer, Use::CopyDst);
                      b.use(inputBuffer, Use::CopyDst);
                      b.use(headerBuffer, Use::CopyDst);
                      for (const Imported& im : ims)
                          for (BufferRef x : { im.rest, im.state, im.tickPrev, im.tickCur, im.joint, im.follow }) b.use(x, Use::CopyDst);
                  },
                  [=](PassContext& c) mutable {
                      for (const Dispatch& d : ds)
                      {
                          size_t bi = 0;
                          while (bs[bi] != d.body) ++bi;
                          const Imported& im = ims[bi];
                          Constants k = cs[d.slot];
                          k.rest = c.srv(im.rest);
                          k.state = c.uav(im.state);
                          k.tickPrev = d.step == 0 ? c.uav(im.tickPrev) : c.srv(im.tickPrev);
                          k.tickCur = d.step == 0 ? c.uav(im.tickCur) : c.srv(im.tickCur);
                          k.guideJoint = c.srv(im.joint);
                          k.inputs = c.srv(inputBuffer);
                          k.frameNodes = d.step == 1 ? c.uav(im.frameNodes) : c.srv(im.frameNodes);
                          k.frameRotations = d.step == 1 ? c.uav(im.frameRotations) : c.srv(im.frameRotations);
                          k.followBuffer = c.srv(im.follow);
                          k.segments = c.uav(segments);
                          std::memcpy(mapped + (size_t)d.slot * kConstantSlot, &k, sizeof k);
                      }
                      c.cmd->CopyBufferRegion(c.resource(constantBuffer), 0, upload, 0, constantsBytes);
                      c.cmd->CopyBufferRegion(c.resource(inputBuffer), 0, upload, constantsBytes, inputsBytes);
                      c.cmd->CopyBufferRegion(c.resource(headerBuffer), 0, upload, constantsBytes + inputsBytes, headerBytes);
                      (void)initBase;
                  });
        // first-time uploads of persistent buffers (their own pass: the targets are imported buffers)
        if (!inits.empty())
        {
            std::vector<Init> in = inits;
            g.addPass("hair.init", QueueType::Graphics,
                      [=](PassBuilder& b) {
                          for (const Imported& im : ims)
                              for (BufferRef x : { im.rest, im.state, im.tickPrev, im.tickCur, im.joint, im.follow }) b.use(x, Use::CopyDst);
                      },
                      [=](PassContext& c) {
                          for (const Init& x : in) c.cmd->CopyBufferRegion(x.target, 0, upload, initBase + x.offset, x.bytes);
                      });
        }
        ID3D12PipelineState* pso[3] = { fc.shaders.compute("Passes/Hair/HairSimulate.STEP0"), fc.shaders.compute("Passes/Hair/HairSimulate.STEP1"),
                                        fc.shaders.compute("Passes/Hair/HairSimulate.STEP2") };
        for (const Dispatch& d : dispatches)
        {
            size_t bi = 0;
            while (bodies[bi] != d.body) ++bi;
            const Imported im = imported[bi];
            const char* names[3] = { "hair.simulate", "hair.frames", "hair.segments" };
            ID3D12PipelineState* p = pso[d.step];
            const uint32_t step = d.step, slotIndex = d.slot, groups = d.groups;
            g.addPass(names[step], QueueType::Graphics,
                      [=](PassBuilder& b) {
                          b.use(constantBuffer, Use::SrvCompute);
                          b.use(inputBuffer, Use::SrvCompute);
                          b.use(im.rest, Use::SrvCompute);
                          b.use(im.joint, Use::SrvCompute);
                          if (step == 0)
                          {
                              b.use(im.state, Use::UavCompute);
                              b.use(im.tickPrev, Use::UavCompute);
                              b.use(im.tickCur, Use::UavCompute);
                          }
                          else if (step == 1)
                          {
                              b.use(im.tickPrev, Use::SrvCompute);
                              b.use(im.tickCur, Use::SrvCompute);
                              b.use(im.frameNodes, Use::UavCompute);
                              b.use(im.frameRotations, Use::UavCompute);
                          }
                          else
                          {
                              b.use(im.follow, Use::SrvCompute);
                              b.use(im.frameNodes, Use::SrvCompute);
                              b.use(im.frameRotations, Use::SrvCompute);
                              b.use(segments, Use::UavCompute);
                          }
                      },
                      [=](PassContext& c) {
                          const uint32_t k[2] = { c.srv(constantBuffer), slotIndex * kConstantSlot };
                          c.cmd->SetPipelineState(p);
                          c.computeConstants(k, 2);
                          c.cmd->Dispatch(groups, 1, 1);
                      });
        }
        fc.resources.hairSegments = segments;
        fc.resources.hairBodies = headerBuffer;
    }

private:
    struct Slot
    {
        ComPtr<ID3D12Resource> upload;
        uint8_t* mapped = nullptr;
        uint64_t bytes = 0;
    };
    Device& m_device;
    std::vector<Slot> m_slots;
};
} // namespace
} // namespace unx::hair

namespace unx::render::tracks
{
void hair(FramePassContext& fc, ViewResources& view)
{
    if (!fc.trackState) return;
    auto& p = fc.trackState->get<std::unique_ptr<hair::HairPass>>("hair.pass");
    if (!p) p = std::make_unique<hair::HairPass>(fc.device);
    p->record(fc, view, hair::hairSystem(*fc.trackState));
}
} // namespace unx::render::tracks
