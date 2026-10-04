#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace unx::render::detail
{
// Half-open ranges of 16-byte elements in the whole target buffer. Keep writes
// as ranges until flush; payload still comes from the final CPU mirror, so
// overlapping writes retain the existing last-write-wins behavior.
struct RuntimeDirtyRange
{
    uint32_t target;
    uint64_t first, end;
};

inline uint64_t normalizeRuntimeDirtyRanges(std::vector<RuntimeDirtyRange>& ranges)
{
    std::sort(ranges.begin(), ranges.end(), [](const auto& a, const auto& b) {
        if (a.target != b.target) return a.target < b.target;
        if (a.first != b.first) return a.first < b.first;
        return a.end < b.end;
    });
    size_t count = 0;
    for (const RuntimeDirtyRange range : ranges)
    {
        if (range.first >= range.end) continue;
        if (count != 0 && ranges[count - 1].target == range.target && range.first <= ranges[count - 1].end)
            ranges[count - 1].end = std::max(ranges[count - 1].end, range.end);
        else ranges[count++] = range;
    }
    ranges.resize(count);
    uint64_t elements = 0;
    for (const auto& range : ranges) elements += range.end - range.first;
    return elements;
}
} // namespace unx::render::detail
