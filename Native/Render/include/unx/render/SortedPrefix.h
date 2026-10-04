#pragma once

#include <algorithm>
#include <cstddef>
#include <vector>

namespace unx::render::detail
{
// For a comparator with a unique tie-breaker, preserve exactly the prefix of a
// full sort. Partition first when most candidates are discarded, then sort only
// the retained prefix. Keep full sorting when most candidates survive.
template<class T, class Compare>
void retainSortedPrefix(std::vector<T>& values, size_t capacity, Compare compare)
{
    if (capacity == 0) { values.clear(); return; }
    const size_t keep = std::min(capacity, values.size());
    if (keep < values.size() / 4)
    {
        const auto middle = values.begin() + static_cast<std::ptrdiff_t>(keep);
        std::nth_element(values.begin(), middle, values.end(), compare);
        std::sort(values.begin(), middle, compare);
    }
    else std::sort(values.begin(), values.end(), compare);
    values.resize(keep);
}
} // namespace unx::render::detail
