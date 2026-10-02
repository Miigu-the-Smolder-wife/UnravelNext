// Automatic exposure (FEATURES_GAME 6.2, render A item A4; owner M). The shading kernels (ShadeOpaque, ShadeSky: every
// pixel of the main view once, before the edge and coverage composites refine some of them) add each pixel's luminance to
// a 64-bin log2 histogram, centre-weighted (ShadingCommon.hlsli shExposureHistogram). The histogram is copied into a
// readback ring; the frame recorded framesInFlight frames later (whose slot fence the host waited for) reads it, meters
// the scene luminance (the weighted mean of log2 L over the bins left after cutting the darkest and the brightest
// fractions), and moves its EV100 towards the target with exact exponential adaptation over the frame's delta time
// (dark -> bright and bright -> dark time constants). After a cut or a restore the EV holds until the first histogram of
// the new view, then snaps to its target. The EV is a frame constant:
// every pass of the frame sees one exposure (one frame of meter latency, deterministic).
// Snap frames (the renderer's first frames before any histogram came back, and the frames after a cut or restore until a
// histogram of the new view came back): their EV was not metered on what they show, so each of them meters its own
// histogram on the GPU after its shading (exposureMeter, ExposureMeter.hlsl) and its output applies the correction
// (PostFinal; the gate's captures): the first frame after a cut is exposed for itself, not 2-3 frames later.
#include "unx/shading/Exposure.h"

#include "unx/core/Config.h"
#include "unx/render/Device.h"
#include "unx/render/Frame.h"
#include "unx/render/RenderGraph.h"
#include "unx/render/Shaders.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace unx::render::shading
{
namespace
{
constexpr uint32_t kBins = 64, kSlots = 4;

struct ExposureState
{
    Device* device = nullptr;
    ComPtr<ID3D12Resource> histogram, readback;
    uint32_t* mapped = nullptr;
    uint64_t slotFrame[kSlots] = { UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX };
    bool cleared = false, initialized = false;
    // A cut or restore: the next histogram measured at or after snapFrom sets the EV at once (older ones show the view
    // before it); until then the EV holds.
    bool snapPending = false;
    uint64_t snapFrom = 0;
    float ev = 14.0f;
    // This frame: its EV is not metered on its view (snap frames, above); the EV and compensation it renders with.
    bool snapping = false;
    float evUsed = 14.0f, compensation = 0;
    uint64_t importFrame = UINT64_MAX;  // the frame whose graph imported the histogram (one import per graph)
    BufferRef imported;
    ~ExposureState()
    {
        if (!device) return;
        if (readback) readback->Unmap(0, nullptr);
        device->deferRelease(histogram);
        device->deferRelease(readback);
    }
    void ensure(Device& d)
    {
        if (histogram) return;
        device = &d;
        D3D12_HEAP_PROPERTIES heap{ D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC1 bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = kBins * 4;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        check(d.d3d()->CreateCommittedResource3(&heap, D3D12_HEAP_FLAG_NONE, &bd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&histogram)),
              "M exposure histogram");
        histogram->SetName(L"M exposure histogram");
        D3D12_HEAP_PROPERTIES rb{ D3D12_HEAP_TYPE_READBACK };
        bd.Width = (uint64_t)kSlots * kBins * 4;
        bd.Flags = D3D12_RESOURCE_FLAG_NONE;
        check(d.d3d()->CreateCommittedResource3(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_BARRIER_LAYOUT_UNDEFINED, nullptr, nullptr, 0, nullptr, IID_PPV_ARGS(&readback)),
              "M exposure readback");
        readback->SetName(L"M exposure readback");
        check(readback->Map(0, nullptr, reinterpret_cast<void**>(&mapped)), "map M exposure readback");
    }
};

struct Params
{
    float minEv, maxEv, targetGrey, tauUp, tauDown, cutLow, cutHigh, centreSigma;
    float linearDistance = 0;  // shading.exposure_linear_distance_ev (0: exponential all the way)
};

Params params(const QualityConfig& q)
{
    Params p;
    p.minEv = (float)q.number("shading.exposure_min_ev");
    p.maxEv = (float)q.number("shading.exposure_max_ev");
    p.targetGrey = (float)q.number("shading.exposure_target_grey");
    p.tauUp = (float)q.number("shading.exposure_adapt_brighter_seconds");
    p.tauDown = (float)q.number("shading.exposure_adapt_darker_seconds");
    p.cutLow = (float)q.number("shading.exposure_cut_dark");
    p.cutHigh = (float)q.number("shading.exposure_cut_bright");
    p.centreSigma = (float)q.number("shading.exposure_centre_sigma");
    p.linearDistance = q.has("shading.exposure_linear_distance_ev") ? (float)q.number("shading.exposure_linear_distance_ev") : 0.0f;
    if (!(p.linearDistance >= 0)) fail("shading.exposure_linear_distance_ev must be >= 0");
    if (!(p.minEv < p.maxEv) || !(p.targetGrey > 0) || !(p.tauUp > 0) || !(p.tauDown > 0) || p.cutLow < 0 || p.cutHigh < 0 || p.cutLow + p.cutHigh >= 1 ||
        !(p.centreSigma > 0))
        fail("shading.exposure_*: invalid (min < max ev, grey > 0, time constants > 0, 0 <= cuts with cut sum < 1, sigma > 0)");
    return p;
}
} // namespace

float meterEv100(const uint32_t* bins, float targetGrey, float cutLow, float cutHigh, bool& valid)
{
    double total = 0;
    for (uint32_t b = 0; b < kBins; ++b) total += bins[b];
    valid = total > 0;
    if (!valid) return 0;
    // The weighted mean of log2 L over the bins between the cuts (a bin straddling a cut counts in part).
    const double lo = total * cutLow, hi = total * (1.0 - cutHigh);
    double below = 0, sum = 0, weight = 0;
    for (uint32_t b = 0; b < kBins; ++b)
    {
        const double c = bins[b], a = std::max(below, lo), z = std::min(below + c, hi);
        if (z > a)
        {
            const double centre = kExposureLog2Min + (b + 0.5) * kExposureLog2Step;
            sum += (z - a) * centre;
            weight += z - a;
        }
        below += c;
    }
    if (!(weight > 0)) return valid = false, 0.0f;
    const double log2L = sum / weight;
    // Exposure 1 / (1.2 x 2^EV100) maps the metered luminance L onto the target grey: 2^EV100 = L / (1.2 grey).
    return (float)(log2L - std::log2(1.2 * targetGrey));
}

float autoExposureEv100(TrackState& state, Device& device, const QualityConfig& quality, const FrameContext& frame, uint32_t framesInFlight)
{
    ExposureState& s = state.get<ExposureState>("M.exposure");
    s.ensure(device);
    const Params p = params(quality);
    // The newest histogram the host has waited for: the frame framesInFlight before this one (its slot was reused).
    bool metered = false;
    float target = s.ev;
    if (frame.discontinuity != 0)
    {
        s.snapPending = true;
        s.snapFrom = frame.frameIndex;
    }
    uint64_t f = 0;
    if (frame.frameIndex >= framesInFlight)
    {
        f = frame.frameIndex - framesInFlight;
        const uint32_t slot = (uint32_t)(f % kSlots);
        if (s.slotFrame[slot] == f)
        {
            target = meterEv100(s.mapped + (size_t)slot * kBins, p.targetGrey, p.cutLow, p.cutHigh, metered);
            target -= frame.exposureCompensation;  // +1 = one stop brighter
        }
    }
    if (metered)
    {
        target = std::clamp(target, p.minEv, p.maxEv);
        if (!s.initialized || (s.snapPending && f >= s.snapFrom))
        {
            s.ev = target;  // first metering, or the first view after a cut or restore: no adaptation
            s.snapPending = false;
        }
        else if (!s.snapPending)
        {
            float dt = std::max(frame.deltaTime, 0.0f);
            const float tau = target > s.ev ? p.tauUp : p.tauDown;
            // Farther than the linear distance from the target the EV moves at a constant rate - the rate the exponential
            // has at that distance - and exponentially from there on (the reference's linear / exponential adaptation and
            // its r.EyeAdaptation.ExponentialTransitionDistance): a large change does not jump most of the way at once.
            float remaining = std::abs(target - s.ev);
            if (p.linearDistance > 0 && remaining > p.linearDistance)
            {
                const float rate = p.linearDistance / tau, linearTime = std::min(dt, (remaining - p.linearDistance) / rate);
                remaining -= rate * linearTime;
                dt -= linearTime;
                s.ev = target + (s.ev > target ? remaining : -remaining);
            }
            s.ev += (target - s.ev) * (1.0f - std::exp(-dt / tau));
        }
        s.initialized = true;
    }
    else if (!s.initialized)
        s.ev = std::clamp(frame.mainView.ev100, p.minEv, p.maxEv);  // before the first histogram: the host's starting value
    s.snapping = !s.initialized || s.snapPending;
    s.evUsed = s.ev;
    s.compensation = frame.exposureCompensation;
    return s.ev;
}

bool exposureSnapping(FramePassContext& fc)
{
    if (!fc.frame.autoExposure) return false;
    return fc.state<ExposureState>("M.exposure").snapping;
}

BufferRef exposureMeter(FramePassContext& fc, const ExposureHistogram& h)
{
    if (!exposureSnapping(fc) || !h.buffer.valid()) return {};
    ExposureState& s = fc.state<ExposureState>("M.exposure");
    const Params p = params(fc.quality);
    const BufferRef correction = fc.graph.createBuffer({ "M exposure correction", 16, 0 });
    const BufferRef histogram = h.buffer;
    ID3D12PipelineState* meter = fc.shaders.compute("Passes/Shading/ExposureMeter");
    const float evUsed = s.evUsed, compensation = s.compensation;
    fc.graph.addPass("m.exposure.meter", QueueType::Graphics,
                     [&](PassBuilder& b) {
                         b.use(histogram, Use::SrvCompute);
                         b.use(correction, Use::UavCompute);
                     },
                     [=](PassContext& c) {
                         auto u = [](float f) { uint32_t v; std::memcpy(&v, &f, 4); return v; };
                         const uint32_t k[12] = { c.srv(histogram), c.uav(correction), u(p.targetGrey), u(p.cutLow), u(p.cutHigh), u(evUsed), u(compensation), 0,
                                                  u(p.minEv), u(p.maxEv), 0, 0 };
                         c.cmd->SetPipelineState(meter);
                         c.computeConstants(k, 12);
                         c.cmd->Dispatch(1, 1, 1);
                     });
    return correction;
}

ExposureHistogram exposureHistogram(FramePassContext& fc)
{
    ExposureState& s = fc.state<ExposureState>("M.exposure");
    s.ensure(fc.device);
    ExposureHistogram h;
    if (s.importFrame != fc.frame.frameIndex)
    {
        s.imported = fc.graph.importBuffer(s.histogram.Get(), BufferDesc{ "M exposure histogram", kBins * 4, 0 });
        s.importFrame = fc.frame.frameIndex;
    }
    h.buffer = s.imported;
    const Params p = params(fc.quality);
    h.centreSigma = p.centreSigma;
    if (!s.cleared)
    {
        // First use: the buffer's content is undefined; later frames clear it after their readback copy.
        s.cleared = true;
        ID3D12PipelineState* clear = fc.shaders.compute("Passes/Shading/ExposureClear");
        const BufferRef buffer = h.buffer;
        fc.graph.addPass("m.exposure.first clear", QueueType::Graphics,
                         [&](PassBuilder& b) {
                             b.use(buffer, Use::UavCompute);
                             b.keep();
                         },
                         [=](PassContext& c) {
                             const uint32_t k[4] = { c.uav(buffer), kBins, 0, 0 };
                             c.cmd->SetPipelineState(clear);
                             c.computeConstants(k, 4);
                             c.cmd->Dispatch(1, 1, 1);
                         });
    }
    return h;
}

void exposureReadback(FramePassContext& fc, const ExposureHistogram& h)
{
    ExposureState& s = fc.state<ExposureState>("M.exposure");
    const uint32_t slot = (uint32_t)(fc.frame.frameIndex % kSlots);
    ID3D12Resource* rb = s.readback.Get();
    ID3D12PipelineState* clear = fc.shaders.compute("Passes/Shading/ExposureClear");
    const BufferRef buffer = h.buffer;
    fc.graph.addPass("m.exposure.readback", QueueType::Graphics,
                     [&](PassBuilder& b) {
                         b.use(buffer, Use::CopySrc);
                         b.keep();
                     },
                     [=](PassContext& c) { c.cmd->CopyBufferRegion(rb, (uint64_t)slot * kBins * 4, c.resource(buffer), 0, kBins * 4); });
    fc.graph.addPass("m.exposure.clear", QueueType::Graphics,
                     [&](PassBuilder& b) {
                         b.use(buffer, Use::UavCompute);
                         b.keep();
                     },
                     [=](PassContext& c) {
                         const uint32_t k[4] = { c.uav(buffer), kBins, 0, 0 };
                         c.cmd->SetPipelineState(clear);
                         c.computeConstants(k, 4);
                         c.cmd->Dispatch(1, 1, 1);
                     });
    s.slotFrame[slot] = fc.frame.frameIndex;
}

} // namespace unx::render::shading
