#include "../../Native/Render/include/unx/render/RuntimeDirtyRanges.h"
#include "../../Native/Render/Frame/SceneUpdateWriter.h"

#include <array>
#include <chrono>
#include <cstdio>
#include <random>
#include <stdexcept>

using namespace unx::render::detail;

namespace
{
using Element = std::pair<uint32_t, uint32_t>;
std::vector<Element> original(const std::vector<RuntimeDirtyRange>& writes)
{
    std::vector<Element> result;
    for (const auto& w : writes)
        for (uint64_t i = w.first; i < w.end; ++i) result.push_back({w.target, static_cast<uint32_t>(i)});
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

void verifyRanges(const std::vector<RuntimeDirtyRange>& writes)
{
    auto ranges = writes;
    const auto expected = original(writes);
    const uint64_t count = normalizeRuntimeDirtyRanges(ranges);
    std::vector<Element> actual;
    for (const auto& r : ranges)
        for (uint64_t i = r.first; i < r.end; ++i) actual.push_back({r.target, static_cast<uint32_t>(i)});
    if (expected != actual || count != expected.size()) throw std::runtime_error("Dirty elements/order differ");
    // Already-normalized input is unchanged; mirrors are consumed only once.
    if (normalizeRuntimeDirtyRanges(ranges) != count) throw std::runtime_error("Normalization is not idempotent");
}

void verifyPayload(uint32_t seed)
{
    std::mt19937 random(seed);
    std::array<std::vector<uint8_t>, 16> mirrors;
    std::array<uint64_t, 16> bases{};
    for (uint32_t target = 0; target < 16; ++target)
    {
        bases[target] = uint64_t(target) * 65536;
        mirrors[target].resize(512 + target); // every possible partial final row
        for (auto& b : mirrors[target]) b = static_cast<uint8_t>(random());
    }
    std::vector<RuntimeDirtyRange> writes;
    auto write = [&](uint32_t target, size_t at, size_t bytes) {
        for (size_t i = 0; i < bytes; ++i) mirrors[target][at + i] = static_cast<uint8_t>(random());
        const uint64_t first = (bases[target] + at) / 16;
        const uint64_t end = (bases[target] + at + bytes + 15) / 16;
        if (first < end) writes.push_back({target, first, end});
    };
    for (uint32_t target = 0; target < 16; ++target) write(target, 500, mirrors[target].size() - 500);
    for (uint32_t i = 0; i < 80; ++i)
    {
        const uint32_t target = random() % 16;
        const size_t at = random() % mirrors[target].size();
        write(target, at, random() % (mirrors[target].size() - at + 1));
    }
    const auto expected = original(writes);
    const uint64_t count = normalizeRuntimeDirtyRanges(writes);
    if (count != expected.size()) throw std::runtime_error("Payload count differs");
    const size_t bytes = static_cast<size_t>(SceneUpdateWriter::bytes(count));
    std::vector<uint8_t> before(bytes + 32, 0x6d), after = before;
    SceneUpdateWriter reference(before.data() + 8, static_cast<uint32_t>(count));
    for (const auto& [target, element] : expected)
    {
        const uint64_t at = uint64_t(element) * 16 - bases[target];
        uint8_t row[16]{};
        std::memcpy(row, mirrors[target].data() + at, std::min<uint64_t>(16, mirrors[target].size() - at));
        reference.append(target, element, row, 1);
    }
    SceneUpdateWriter optimized(after.data() + 8, static_cast<uint32_t>(count));
    for (const auto& r : writes)
    {
        const uint64_t at = r.first * 16 - bases[r.target];
        const uint64_t n = std::min<uint64_t>((r.end - r.first) * 16, mirrors[r.target].size() - at);
        optimized.appendBytes(r.target, static_cast<uint32_t>(r.first), mirrors[r.target].data() + at, static_cast<size_t>(n));
    }
    if (optimized.count() != count || before != after) throw std::runtime_error("Runtime upload bytes/guards differ");
}

void benchmark(uint32_t writeCount, uint32_t rows)
{
    std::vector<RuntimeDirtyRange> writes;
    for (uint32_t i = 0; i < writeCount; ++i)
    {
        const uint64_t first = uint64_t(i / 4) * (rows / 2);
        writes.push_back({i % 4, first, first + rows});
    }
    verifyRanges(writes);
    const uint64_t originalRecords = uint64_t(writeCount) * rows;
    std::vector<double> oldTimes, newTimes;
    uint64_t oldSum = 0, newSum = 0;
    auto time = [&](bool optimized) {
        const auto start = std::chrono::steady_clock::now();
        for (uint32_t repeat = 0; repeat < 3; ++repeat)
            if (optimized)
            {
                auto ranges = writes;
                newSum += normalizeRuntimeDirtyRanges(ranges);
            }
            else oldSum += original(writes).size();
        return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / 3;
    };
    for (uint32_t round = 0; round < 5; ++round)
        if (round % 2 == 0) { oldTimes.push_back(time(false)); newTimes.push_back(time(true)); }
        else { newTimes.push_back(time(true)); oldTimes.push_back(time(false)); }
    if (oldSum != newSum) throw std::runtime_error("Benchmark counts differ");
    std::sort(oldTimes.begin(), oldTimes.end()); std::sort(newTimes.begin(), newTimes.end());
    std::printf("writes=%u rows_per_write=%u tracked_records=%llu->%u old_us=%.2f new_us=%.2f checksum=%llu\n",
                writeCount, rows, static_cast<unsigned long long>(originalRecords), writeCount, oldTimes[2], newTimes[2],
                static_cast<unsigned long long>(newSum));
}
}

int main()
{
    try
    {
        verifyRanges({});
        verifyRanges({{4, 0, 0}, {4, 0, 1}, {4, 0, 1}, {4, 1, 5}, {4, 2, 3}, {4, 8, 9}, {5, 0, 5}});
        verifyRanges({{15, 0x0ffffffd, 0x10000000}, {15, 0x0ffffffe, 0x0fffffff}, {1, 0, 1}});
        std::mt19937 random(76);
        for (uint32_t test = 0; test < 1000; ++test)
        {
            std::vector<RuntimeDirtyRange> writes;
            for (uint32_t i = 0; i < test % 65; ++i)
            {
                const uint64_t first = random() % 1024;
                writes.push_back({random() % 16, first, first + random() % 256});
            }
            verifyRanges(writes);
            verifyPayload(test);
        }
        std::printf("PASS: 1000 randomized interval unions and final upload streams; overlap, gaps, adjacent ranges, targets, zero writes, partial rows, guards and 28-bit boundary.\n");
        std::printf("CPU dirty-list preparation only; GPU work and whole-frame timing excluded.\n");
        benchmark(1, 1); benchmark(16, 256); benchmark(128, 4096);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
