#include "../../Native/Render/Frame/SceneUpdateWriter.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <vector>

using unx::render::detail::SceneUpdateWriter;

namespace
{
using Row = std::array<uint32_t, 4>;
struct Write
{
    uint32_t target, first;
    std::vector<Row> rows;
};

uint32_t countRows(const std::vector<Write>& writes)
{
    uint32_t result = 0;
    for (const auto& w : writes) result += static_cast<uint32_t>(w.rows.size());
    return result;
}

// Pre-change two-vector packing, including its final copies into the mapping.
void reference(const std::vector<Write>& writes, uint8_t* destination)
{
    std::vector<uint32_t> headers;
    std::vector<Row> payload;
    for (const auto& w : writes)
        for (uint32_t k = 0; k < w.rows.size(); ++k)
        {
            headers.push_back(w.target << 28 | (w.first + k));
            payload.push_back(w.rows[k]);
        }
    if (headers.empty()) return;
    const size_t headerBytes = (headers.size() * 4 + 15) & ~size_t(15);
    std::memcpy(destination, headers.data(), headers.size() * 4);
    std::memcpy(destination + headerBytes, payload.data(), payload.size() * 16);
}

void direct(const std::vector<Write>& writes, uint8_t* destination)
{
    SceneUpdateWriter writer(destination, countRows(writes));
    for (const auto& w : writes) writer.append(w.target, w.first, w.rows.data(), static_cast<uint32_t>(w.rows.size()));
    if (writer.count() != countRows(writes)) throw std::runtime_error("Incorrect emitted count");
}

void verify(const std::vector<Write>& writes)
{
    // Unaligned CPU destination and guards also detect changed padding/overwrites.
    const uint32_t count = countRows(writes);
    std::vector<uint8_t> before(static_cast<size_t>(SceneUpdateWriter::bytes(count)) + 39, 0xa5);
    auto after = before;
    reference(writes, before.data() + 7);
    direct(writes, after.data() + 7);
    if (before != after) throw std::runtime_error("Upload bytes or guard bytes differ");
    // Independently decode the unchanged shader ABI (SceneUpdate.hlsl).
    size_t row = 0;
    const size_t payloadAt = (size_t(count) * 4 + 15) & ~size_t(15);
    for (const auto& w : writes)
        for (uint32_t k = 0; k < w.rows.size(); ++k, ++row)
        {
            uint32_t header;
            std::memcpy(&header, after.data() + 7 + row * 4, 4);
            if ((header >> 28) != w.target || (header & 0x0fffffffu) != w.first + k ||
                std::memcmp(after.data() + 7 + payloadAt + row * 16, w.rows[k].data(), 16) != 0)
                throw std::runtime_error("Shader ABI decoder differs");
        }
}

Write makeWrite(uint32_t target, uint32_t first, uint32_t count, std::mt19937& random)
{
    Write w{target, first, std::vector<Row>(count)};
    for (auto& row : w.rows) for (auto& word : row) word = random();
    return w;
}

void benchmark(uint32_t instances, uint32_t skins)
{
    std::mt19937 random(871);
    std::vector<Write> writes;
    for (uint32_t i = 0; i < instances; ++i) writes.push_back(makeWrite(0, i * 12, 12, random));
    for (uint32_t i = 0; i < skins; ++i)
    {
        writes.push_back(makeWrite(1, i * 192, 192, random));
        writes.push_back(makeWrite(2, i * 192, 192, random));
    }
    verify(writes);
    std::vector<uint8_t> destination(static_cast<size_t>(SceneUpdateWriter::bytes(countRows(writes))), 0);
    std::vector<double> oldTimes, newTimes;
    uint64_t checksum = 0;
    auto time = [&](bool optimized) {
        const auto start = std::chrono::steady_clock::now();
        for (uint32_t repeat = 0; repeat < 8; ++repeat)
        {
            if (optimized) direct(writes, destination.data()); else reference(writes, destination.data());
            checksum += destination[(repeat * 31) % destination.size()];
        }
        return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / 8;
    };
    for (uint32_t round = 0; round < 5; ++round)
        if (round % 2 == 0) { oldTimes.push_back(time(false)); newTimes.push_back(time(true)); }
        else { newTimes.push_back(time(true)); oldTimes.push_back(time(false)); }
    std::sort(oldTimes.begin(), oldTimes.end()); std::sort(newTimes.begin(), newTimes.end());
    std::printf("instances=%u skins=%u rows=%u bytes=%llu old_us=%.2f new_us=%.2f speedup=%.2fx checksum=%llu\n",
                instances, skins, countRows(writes), static_cast<unsigned long long>(destination.size()), oldTimes[2], newTimes[2],
                oldTimes[2] / newTimes[2], static_cast<unsigned long long>(checksum));
}
}

int main()
{
    try
    {
        std::mt19937 random(91);
        verify({});
        for (uint32_t n : {0u, 1u, 2u, 3u, 4u, 63u, 64u, 65u}) verify({makeWrite(0, 0, n, random)});
        std::vector<Write> targets;
        for (uint32_t target = 0; target < 16; ++target) targets.push_back(makeWrite(target, 0x0ffffffe, 2, random));
        verify(targets);
        // Preserve signed zero, NaN payloads and every other bit, not float equality.
        verify({{0, 0, {{0x80000000u, 0x7f800000u, 0x7fc12345u, 0xffffffffu}}}, {2, 7, {{1, 0, 0, 0}}}});
        for (uint32_t test = 0; test < 1000; ++test)
        {
            std::vector<Write> writes;
            for (uint32_t k = 0; k < test % 49; ++k) writes.push_back(makeWrite(random() % 16, random() % 100000, random() % 193, random));
            verify(writes);
        }
        if (SceneUpdateWriter::bytes((1u << 28) - 1) != 5368709104ull) throw std::runtime_error("Large layout arithmetic changed");
        std::printf("PASS: 1000 randomized streams, empty/alignment/dispatch boundaries, all 16 targets, 28-bit indices, raw float bits and guards.\n");
        std::printf("CPU packing into ordinary RAM only; no GPU or mapped-upload timing, not frame time.\n");
        benchmark(1, 0); benchmark(128, 16); benchmark(10000, 256);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
