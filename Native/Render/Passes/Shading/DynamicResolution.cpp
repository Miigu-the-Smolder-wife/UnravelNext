// Dynamic resolution of M's temporal upscale (output.dynamic_resolution_target_ms; unx/shading/DynamicResolution.h): the
// main view's internal height follows the GPU's frame time, so a frame stays inside a time budget where the scene gets
// heavier and takes the pixels back where it gets lighter. The upscale's output and its history keep the output's size:
// only the input changes (the reference's dynamic resolution with TSR; its TSR takes a different input rectangle every
// frame, as m.upscale and its histories do here - Upscale.cpp keeps each history slot at the size of the frame that
// wrote it and reprojects by UV).
// The rules are the reference's heuristic (its DynamicResolutionState / r.DynamicRes.*). That code is in the reference's
// Engine module, which is not in the reference checkout: the rules and the defaults below are written from the
// heuristic's documented behaviour and its console variables as remembered, not from the source.
//   measure    every completed frame's GPU time (GpuProfiler, FrameContext::timing) with the resolution fraction that
//              frame was rendered at (fraction = internal height / output height; the times arrive a few frames late,
//              so each frame's decision is kept until its time comes back);
//   suggest    per measured frame the fraction that would have met the target: fraction x sqrt(target / time) - the
//              time taken as proportional to the pixels; with dynamic_resolution_fixed_ms the part of a frame that does
//              not scale is taken off both first (this renderer's measured model is fixed + per-pixel);
//   weigh      the suggestions of the last history_frames frames, each older one x frame_weight;
//   panic      over_budget_frames newest frames in a row above the budget: their suggestion is taken at once and the
//              history starts again;
//   otherwise  a change only every change_period_frames frames and only by more than change_threshold_percent;
//              an increase goes increase_blend of the way (the proportional model overshoots upwards);
//   limits     min_percent .. max_percent of the output's height, at most the static settings' height, in steps of
//              step_lines, rounded down (the size does not creep by single lines: a step re-keys the frame's graph plan
//              and restarts the screen-space histories of the systems that key them on the view's size - the list is
//              in Docs/Status/UE6_PORT_STATUS_KO.md 2.3.1).
// A view the static settings render natively (no upscale) is not scaled: the native and the upscaled paths do not share
// their histories.
#include "unx/shading/DynamicResolution.h"

#include "unx/core/Config.h"
#include "unx/render/GpuProfiler.h"

#include <algorithm>
#include <cmath>

namespace unx::render::shading
{
namespace
{
constexpr uint32_t kHistoryMax = 64, kDecisions = 64;

struct ControllerState
{
    struct Entry
    {
        float fraction = 0, gpuMs = 0;
    };
    Entry history[kHistoryMax];  // a ring; the newest at (head - 1)
    uint32_t count = 0, head = 0;
    struct Decision
    {
        uint64_t frame = ~0ull;
        float fraction = 0;
    } decisions[kDecisions];     // per frame index: the fraction that frame was rendered at
    float fraction = 0;          // the current one (0: none yet - the first frame takes the maximum)
    uint32_t sinceChange = 0;    // frames since the fraction last changed
    uint64_t consumed = ~0ull;   // the newest timing taken in
    tracks::DynamicResolutionStatus status;
};

double numberOr(const QualityConfig& q, const char* key, double fallback) { return q.has(key) ? q.number(key) : fallback; }
int64_t integerOr(const QualityConfig& q, const char* key, int64_t fallback) { return q.has(key) ? q.integer(key) : fallback; }
} // namespace

uint32_t dynamicResolutionHeight(TrackState& state, const QualityConfig& q, const FrameContext& frame, uint32_t outputHeight, uint32_t maxHeight)
{
    ControllerState& s = state.get<ControllerState>("M.dynamicResolution");
    const double budget = numberOr(q, "output.dynamic_resolution_target_ms", 0.0);
    s.status = tracks::DynamicResolutionStatus{};
    s.status.height = s.status.maxHeight = s.status.minHeight = maxHeight;
    s.status.scale = outputHeight ? (float)maxHeight / (float)outputHeight : 1.0f;
    if (!(budget > 0) || outputHeight == 0 || maxHeight >= outputHeight)
    {
        // off (or a natively rendered view): the next time it is switched on starts from the maximum
        s.count = 0;
        s.fraction = 0;
        s.sinceChange = 0;
        return maxHeight;
    }
    const double headroom = numberOr(q, "output.dynamic_resolution_headroom_percent", 10.0);
    const double minPercent = numberOr(q, "output.dynamic_resolution_min_percent", 50.0);
    const double maxPercent = numberOr(q, "output.dynamic_resolution_max_percent", 100.0);
    const int64_t historyFrames = integerOr(q, "output.dynamic_resolution_history_frames", 16);
    const double frameWeight = numberOr(q, "output.dynamic_resolution_frame_weight", 0.9);
    const int64_t changePeriod = integerOr(q, "output.dynamic_resolution_change_period_frames", 8);
    const double changeThreshold = numberOr(q, "output.dynamic_resolution_change_threshold_percent", 2.0);
    const double increaseBlend = numberOr(q, "output.dynamic_resolution_increase_blend", 0.9);
    const int64_t overBudgetFrames = integerOr(q, "output.dynamic_resolution_over_budget_frames", 2);
    const int64_t stepLines = integerOr(q, "output.dynamic_resolution_step_lines", 72);
    const double fixedMs = numberOr(q, "output.dynamic_resolution_fixed_ms", 0.0);
    if (!(headroom >= 0 && headroom < 100) || !(minPercent > 0 && minPercent <= 100) || !(maxPercent >= minPercent && maxPercent <= 100) || historyFrames < 1 ||
        historyFrames > (int64_t)kHistoryMax || !(frameWeight > 0 && frameWeight <= 1) || changePeriod < 0 || changePeriod > 600 ||
        !(changeThreshold >= 0 && changeThreshold < 100) || !(increaseBlend > 0 && increaseBlend <= 1) || overBudgetFrames < 1 || overBudgetFrames > 16 ||
        stepLines < 1 || stepLines > 256 || !(fixedMs >= 0))
        fail("output.dynamic_resolution_*: headroom in [0, 100), 0 < min <= max <= 100 percent, history frames in [1, 64], frame weight and increase blend in "
             "(0, 1], change period in [0, 600], change threshold in [0, 100), over-budget frames in [1, 16], step lines in [1, 256], fixed ms >= 0");

    // the limits as heights (whole steps), then as fractions of the output's height
    const uint32_t step = (uint32_t)stepLines;
    const uint32_t highest = std::min(maxHeight, std::max(8u, (uint32_t)std::lround(maxPercent * 0.01 * outputHeight)));
    uint32_t lowest = std::max(8u, (uint32_t)std::lround(minPercent * 0.01 * outputHeight));
    lowest = std::min(std::max((lowest + step - 1) / step * step, step), highest);
    const float maxFraction = (float)highest / (float)outputHeight, minFraction = (float)lowest / (float)outputHeight;
    if (!(s.fraction > 0)) s.fraction = maxFraction;
    s.fraction = std::clamp(s.fraction, minFraction, maxFraction);

    // the newest completed frame's time, with the fraction that frame was rendered at
    if (frame.timing && frame.timing->frame != s.consumed)
    {
        s.consumed = frame.timing->frame;
        const ControllerState::Decision& d = s.decisions[frame.timing->frame % kDecisions];
        if (d.frame == frame.timing->frame && d.fraction > 0 && frame.timing->gpuFrameMs > 0)
        {
            s.history[s.head] = { d.fraction, (float)frame.timing->gpuFrameMs };
            s.head = (s.head + 1) % kHistoryMax;
            s.count = std::min(s.count + 1, kHistoryMax);
        }
    }

    // the suggestion over the history, newest first
    const double target = budget * (1.0 - headroom * 0.01);
    double suggestion = 0, weights = 0, weight = 1, averageMs = 0;
    uint32_t browsed = 0, overBudget = 0;
    bool panic = false;
    const uint32_t frames = std::min(s.count, (uint32_t)historyFrames);
    for (uint32_t i = 0; i < frames; ++i)
    {
        const ControllerState::Entry& e = s.history[(s.head + kHistoryMax - 1 - i) % kHistoryMax];
        if (browsed == overBudget && e.gpuMs > budget) ++overBudget;
        // (the time as fixed + per pixel: the per-pixel part against what the target leaves for it)
        const double scalable = std::max((double)e.gpuMs - fixedMs, 1e-3), allowed = std::max(target - fixedMs, 1e-3);
        suggestion += (double)e.fraction * std::sqrt(allowed / scalable) * weight;
        averageMs += (double)e.gpuMs * weight;
        weights += weight;
        weight *= frameWeight;
        ++browsed;
        if (overBudget >= (uint32_t)overBudgetFrames && overBudget == browsed)
        {
            panic = true;  // (only these frames speak)
            break;
        }
    }
    float next = s.fraction;
    if (weights > 0)
    {
        suggestion /= weights;
        averageMs /= weights;
        if (panic)
        {
            next = (float)suggestion;
            s.count = 0;  // (this frame is the baseline of what follows)
        }
        else
        {
            const bool worth = std::abs(suggestion - (double)s.fraction) > changeThreshold * 0.01;
            const bool due = s.sinceChange >= (uint32_t)changePeriod;
            if (worth && due)
                next = suggestion > (double)s.fraction ? (float)((double)s.fraction + (suggestion - (double)s.fraction) * increaseBlend) : (float)suggestion;
        }
    }
    // whole steps inside the limits, rounded down (a height the suggestion does not reach would stay over the budget; an
    // increase waits until a whole step fits); no change keeps the height as it is
    const uint32_t current = std::clamp((uint32_t)std::lround((double)s.fraction * outputHeight), lowest, highest);
    uint32_t height = current;
    if (next != s.fraction)
        height = std::clamp((uint32_t)std::floor((double)std::clamp(next, minFraction, maxFraction) * outputHeight / step + 1e-4) * step, lowest, highest);
    if (next > s.fraction && height < current) height = current;
    if (height != current)
    {
        s.fraction = (float)height / (float)outputHeight;
        s.sinceChange = 0;
    }
    else
    {
        height = current;
        ++s.sinceChange;
    }
    s.decisions[frame.frameIndex % kDecisions] = { frame.frameIndex, (float)height / (float)outputHeight };

    s.status.active = true;
    s.status.targetMs = (float)budget;
    s.status.averageMs = (float)averageMs;
    s.status.scale = (float)height / (float)outputHeight;
    s.status.height = height;
    s.status.maxHeight = highest;
    s.status.minHeight = lowest;
    return height;
}

tracks::DynamicResolutionStatus dynamicResolutionStatus(TrackState& state) { return state.get<ControllerState>("M.dynamicResolution").status; }
} // namespace unx::render::shading
