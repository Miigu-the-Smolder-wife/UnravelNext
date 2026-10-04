#include "../../Native/Render/include/unx/render/SortedPrefix.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <numeric>
#include <random>
#include <stdexcept>

using unx::render::detail::retainSortedPrefix;

namespace
{
struct Candidate
{
    uint32_t plane, x, y, w, h;
    bool eligible, current;
    bool operator==(const Candidate&) const = default;
};

void verify(uint32_t n, size_t capacity, uint32_t seed)
{
    std::mt19937 random(seed);
    std::vector<uint32_t> counts(n), wanted(n);
    std::iota(wanted.begin(), wanted.end(), 0u);
    for (auto& c : counts) c = seed % 2 ? random() % 8 : random();
    std::shuffle(wanted.begin(), wanted.end(), random);
    auto expected = wanted;
    auto compare = [&](uint32_t a, uint32_t b) { return counts[a] != counts[b] ? counts[a] > counts[b] : a < b; };
    std::sort(expected.begin(), expected.end(), compare);
    expected.resize(std::min(capacity, expected.size()));
    retainSortedPrefix(wanted, capacity, compare);
    if (wanted != expected) throw std::runtime_error("Exact-set owners/order differ");

    std::vector<Candidate> planes;
    for (uint32_t i = 0; i < n; ++i)
        planes.push_back({i, random() % 4096, random() % 4096, 64 * (1 + random() % 8), 64 * (1 + random() % 8),
                          random() % 2 != 0, random() % 2 != 0});
    std::shuffle(planes.begin(), planes.end(), random);
    auto original = planes;
    auto screenSize = [&](const Candidate& c) { return c.current ? uint64_t(counts[c.plane]) : uint64_t(c.w) * c.h; };
    auto rank = [&](const Candidate& a, const Candidate& b) {
        const uint64_t sa = screenSize(a), sb = screenSize(b);
        return sa != sb ? sa > sb : a.plane < b.plane;
    };
    std::sort(original.begin(), original.end(), rank);
    original.resize(std::min(capacity, original.size()));
    retainSortedPrefix(planes, capacity, rank);
    if (original != planes) throw std::runtime_error("Planar candidates/order differ");
    // The second, stable ordering inherits the first sort's order on ties.
    auto cameraOrder = [&](const Candidate& a, const Candidate& b) {
        if (a.eligible != b.eligible) return a.eligible;
        if (a.eligible && counts[a.plane] != counts[b.plane]) return counts[a.plane] > counts[b.plane];
        return uint64_t(a.w) * a.h > uint64_t(b.w) * b.h;
    };
    std::stable_sort(original.begin(), original.end(), cameraOrder);
    std::stable_sort(planes.begin(), planes.end(), cameraOrder);
    if (original != planes) throw std::runtime_error("Planar camera priority changed");
}

void benchmark(uint32_t n, size_t capacity, uint32_t mode)
{
    std::mt19937 random(481);
    std::vector<uint32_t> counts(n), input(n);
    std::iota(input.begin(), input.end(), 0u);
    for (auto& c : counts) c = mode == 3 ? 1 : random();
    auto compare = [&](uint32_t a, uint32_t b) { return counts[a] != counts[b] ? counts[a] > counts[b] : a < b; };
    if (mode == 1 || mode == 2) std::sort(input.begin(), input.end(), compare);
    if (mode == 2) std::reverse(input.begin(), input.end());
    uint64_t oldSum = 0, newSum = 0;
    std::vector<double> before, after;
    auto time = [&](bool optimized) {
        const auto start = std::chrono::steady_clock::now();
        for (uint32_t repeat = 0; repeat < 24; ++repeat)
        {
            auto values = input;
            if (optimized) retainSortedPrefix(values, capacity, compare);
            else { std::sort(values.begin(), values.end(), compare); values.resize(std::min(capacity, values.size())); }
            uint64_t& sum = optimized ? newSum : oldSum;
            for (auto v : values) sum += v;
        }
        return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / 24;
    };
    for (uint32_t round = 0; round < 5; ++round)
        if (round % 2 == 0) { before.push_back(time(false)); after.push_back(time(true)); }
        else { after.push_back(time(true)); before.push_back(time(false)); }
    if (oldSum != newSum) throw std::runtime_error("Benchmark output differs");
    std::sort(before.begin(), before.end()); std::sort(after.begin(), after.end());
    std::printf("candidates=%u keep=%zu mode=%u old_us=%.2f new_us=%.2f\n", n, capacity, mode, before[2], after[2]);
}
}

int main()
{
    try
    {
        uint32_t checks = 0;
        for (uint32_t n : {0u, 1u, 7u, 8u, 9u, 32u, 33u, 63u, 64u, 65u, 256u, 257u, 1024u})
            for (size_t keep : {size_t(0), size_t(1), size_t(8), size_t(64), size_t(n), size_t(n) + 1})
            { verify(n, keep, 52); ++checks; }
        for (uint32_t seed = 0; seed < 1000; ++seed)
        { verify(seed % 513, seed % 3 ? 8 : 64, seed); ++checks; }
        std::printf("PASS: %u inputs; identical exact owners, planar candidates and stable camera priority, including ties and capacity boundaries.\n", checks);
        std::printf("CPU selection only; GPU/frame time excluded. modes: random, sorted, reversed, tied.\n");
        for (uint32_t n : {64u, 512u, 4096u})
            for (size_t keep : {size_t(8), size_t(64)})
                for (uint32_t mode = 0; mode < 4; ++mode) benchmark(n, keep, mode);
        return 0;
    }
    catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
}
