#include "../../Native/Render/Frame/SkeletonInstances.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <random>
#include <stdexcept>

using namespace unx;
using namespace unx::render;

namespace
{
struct Fixture
{
    std::vector<scene::Instance> source;
    std::vector<gpu::Instance> packed;
    std::vector<std::vector<uint32_t>> index;

    void rebuild(size_t skeletons) { index = detail::skeletonInstances(skeletons, source, packed); }
    void add(uint32_t skeleton, bool skinned)
    {
        scene::Instance s{};
        s.skeleton = skeleton;
        s.flags = skinned ? scene::InstanceSkinned : 0;
        source.push_back(s);
        gpu::Instance p{};
        p.bonePalette = skinned ? static_cast<uint32_t>(packed.size()) * 3 : gpu::kNone;
        packed.push_back(p);
    }
    void verify() const
    {
        for (uint32_t skeleton = 0; skeleton < index.size(); ++skeleton)
        {
            std::vector<uint32_t> original;
            for (uint32_t i = 0; i < packed.size(); ++i)
                if (packed[i].bonePalette != gpu::kNone && source[i].skeleton == skeleton) original.push_back(i);
            if (original != index[skeleton]) throw std::runtime_error("Skeleton visitation order differs");
        }
    }
};

template<bool Indexed>
uint64_t visit(const Fixture& f, uint32_t repetitions)
{
    uint64_t checksum = 0;
    for (uint32_t repeat = 0; repeat < repetitions; ++repeat)
        for (uint32_t skeleton = 0; skeleton < f.index.size(); ++skeleton)
        {
            auto consume = [&](uint32_t i) { checksum += (uint64_t(i) + 1) * (skeleton + 1) + f.packed[i].bonePalette; };
            if constexpr (Indexed)
                for (uint32_t i : f.index[skeleton]) consume(i);
            else
                for (uint32_t i = 0; i < f.packed.size(); ++i)
                    if (f.packed[i].bonePalette != gpu::kNone && f.source[i].skeleton == skeleton) consume(i);
        }
    return checksum;
}
}

int main()
{
    try
    {
        Fixture empty;
        empty.rebuild(0); empty.verify();
        empty.rebuild(3); empty.verify();
        Fixture shared;
        shared.add(1, true); shared.add(scene::kNone, false); shared.add(0, true);
        shared.add(1, true); shared.add(0, false); shared.add(1, true);
        shared.rebuild(3); shared.verify();
        // Visibility and view-model flags do not remove palette consumers.
        shared.packed[0].flags |= gpu::kInstanceHidden;
        shared.packed[3].flags |= gpu::kInstanceViewModel;
        shared.verify();
        // setInstances can append or replace rigid records, never palette members.
        shared.add(2, false);
        shared.source[1].skeleton = 2;
        shared.verify();
        // Runtime records have no corresponding source instance and no palette.
        gpu::Instance runtime{};
        runtime.bonePalette = gpu::kNone;
        shared.packed.push_back(runtime);
        shared.verify();
        shared.packed.back().flags |= gpu::kInstanceHidden;
        shared.verify();
        // A new upload replaces the index, including a smaller skeleton roster.
        shared.source.resize(1); shared.packed.resize(1);
        shared.source[0].skeleton = 0;
        shared.rebuild(1); shared.verify();
        shared.source.clear(); shared.packed.clear();
        shared.rebuild(0); shared.verify();

        for (uint32_t seed = 0; seed < 500; ++seed)
        {
            std::mt19937 random(seed);
            Fixture f;
            const uint32_t skeletons = 1 + random() % 64;
            for (uint32_t i = 0; i < seed * 3; ++i) f.add(random() % skeletons, random() % 4 == 0);
            f.rebuild(skeletons); f.verify();
            if (visit<false>(f, 2) != visit<true>(f, 2)) throw std::runtime_error("Consumer sequence differs");
        }
        std::printf("PASS: shared/unused skeletons, rigid edits, hidden/view-model instances, runtime append/removal, reload and 500 randomized rosters.\n");
        // Only CPU consumer lookup is timed. Palette math and GPU work are excluded.
        Fixture f;
        for (uint32_t i = 0; i < 10000; ++i) f.add(i % 128, i < 256);
        f.rebuild(128); f.verify();
        std::vector<double> before, after;
        uint64_t oldChecksum = 0, newChecksum = 0;
        auto time = [&](bool indexed) {
            const auto start = std::chrono::steady_clock::now();
            if (indexed) newChecksum += visit<true>(f, 8);
            else oldChecksum += visit<false>(f, 8);
            return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - start).count() / 8;
        };
        for (uint32_t repeat = 0; repeat < 5; ++repeat)
            if (repeat % 2 == 0) { before.push_back(time(false)); after.push_back(time(true)); }
            else { after.push_back(time(true)); before.push_back(time(false)); }
        if (oldChecksum != newChecksum) throw std::runtime_error("Benchmark consumer output differs");
        std::sort(before.begin(), before.end()); std::sort(after.begin(), after.end());
        std::printf("Lookup only: instances=10000 skeleton_updates=128 palette_consumers=256; examined records 1280000 -> 256.\n");
        std::printf("CPU lookup median old_us=%.2f new_us=%.2f; not complete skeleton update or frame time.\n", before[2], after[2]);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
