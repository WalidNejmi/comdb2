#include <benchmark/benchmark.h>
#include "commit_map_adapter.h"

#include <memory>
#include <stdexcept>

using Map = std::unique_ptr<commit_map_fixture, decltype(&commit_map_destroy)>;

static Map make_map()
{
    Map map(commit_map_create(), commit_map_destroy);
    if (!map)
        throw std::runtime_error("commit map initialization failed");
    return map;
}

static void add(commit_map_fixture *map, uint64_t id, uint32_t file)
{
    if (commit_map_add(map, id, file) != 0)
        throw std::runtime_error("commit map insertion failed");
}

static void require(bool condition)
{
    if (!condition)
        throw std::runtime_error("commit map fixture invariant failed");
}

static void lookup(benchmark::State &state, bool hit)
{
    auto map = make_map();
    const uint64_t count = state.range(0);
    for (uint64_t id = 1; id <= count; ++id)
        add(map.get(), id, 100);
    uint32_t file, offset;
    require(commit_map_get(map.get(), count, &file, &offset) == 0 && file == 100 && offset == count);
    require(commit_map_get(map.get(), count + 1, &file, &offset) == commit_map_notfound());
    uint64_t cursor = 0;
    bool valid = true;
    for (auto _ : state) {
        const uint64_t id = 1 + (cursor++ % count) + (hit ? 0 : count);
        const int rc = commit_map_get(map.get(), id, &file, &offset);
        valid = valid && (hit ? rc == 0 && file == 100 && offset == id : rc == commit_map_notfound());
        benchmark::DoNotOptimize(rc);
        benchmark::DoNotOptimize(offset);
    }
    require(valid);
    const auto stats = commit_map_snapshot(map.get());
    require(stats.entries == static_cast<int64_t>(count));
    state.counters["entries"] = stats.entries;
    state.counters["buckets"] = stats.buckets;
    state.SetItemsProcessed(state.iterations());
}

static void LookupHit(benchmark::State &state) { lookup(state, true); }
static void LookupMiss(benchmark::State &state) { lookup(state, false); }

static void InsertGrow(benchmark::State &state)
{
    for (auto _ : state) {
        state.PauseTiming();
        auto map = make_map();
        for (uint64_t id = 1; id <= 1024; ++id)
            add(map.get(), id, 100);
        const auto before = commit_map_snapshot(map.get());
        require(before.entries == 1024 && before.buckets == 1024);
        state.ResumeTiming();
        const int rc = commit_map_add(map.get(), 1025, 100);
        benchmark::DoNotOptimize(rc);
        state.PauseTiming();
        const auto after = commit_map_snapshot(map.get());
        require(rc == 0 && after.entries == 1025 && after.buckets == 2048 &&
                after.resizes == before.resizes + 1);
        map.reset();
        state.ResumeTiming();
    }
    state.counters["entries_rehashed"] = 1025;
    state.counters["resizes_per_op"] = 1;
    state.SetItemsProcessed(state.iterations());
}

static void InsertBatch(benchmark::State &state)
{
    constexpr uint64_t batch = 128;
    for (auto _ : state) {
        state.PauseTiming();
        auto map = make_map();
        add(map.get(), 1, 100);
        const auto before = commit_map_snapshot(map.get());
        state.ResumeTiming();
        int rc = 0;
        for (uint64_t id = 2; id <= batch + 1; ++id)
            rc |= commit_map_add(map.get(), id, 100);
        benchmark::DoNotOptimize(rc);
        state.PauseTiming();
        const auto after = commit_map_snapshot(map.get());
        require(rc == 0 && after.entries == batch + 1 && after.resizes == before.resizes &&
                after.slabs == before.slabs);
        map.reset();
        state.ResumeTiming();
    }
    state.counters["inserts_per_iteration"] = batch;
    state.SetItemsProcessed(state.iterations() * batch);
}

static void Purge(benchmark::State &state, bool shrink)
{
    constexpr uint64_t count = 1024;
    const uint64_t survivors = shrink ? 1 : 1024;
    for (auto _ : state) {
        state.PauseTiming();
        auto map = make_map();
        for (uint64_t id = 1; id <= count; ++id)
            add(map.get(), id, 100);
        for (uint64_t id = count + 1; id <= count + survivors; ++id)
            add(map.get(), id, 101);
        const auto before = commit_map_snapshot(map.get());
        require(before.buckets == 2048);
        state.ResumeTiming();
        const int rc = commit_map_purge(map.get(), 100);
        benchmark::DoNotOptimize(rc);
        state.PauseTiming();
        const auto after = commit_map_snapshot(map.get());
        require(rc == 0 && after.entries == static_cast<int64_t>(survivors) &&
                after.slabs < before.slabs && after.slots < before.slots &&
                after.buckets == (shrink ? 1024 : 2048) &&
                after.resizes == before.resizes + (shrink ? 1 : 0));
        uint32_t file, offset;
        for (uint64_t id = 1; id <= count; ++id)
            require(commit_map_get(map.get(), id, &file, &offset) == commit_map_notfound());
        for (uint64_t id = count + 1; id <= count + survivors; ++id)
            require(commit_map_get(map.get(), id, &file, &offset) == 0 && file == 101 && offset == id);
        map.reset();
        state.ResumeTiming();
    }
    state.counters["entries_purged_per_iteration"] = count;
    state.counters["survivors"] = survivors;
    state.counters["resizes_per_op"] = shrink ? 1 : 0;
    state.SetItemsProcessed(state.iterations() * count);
}

static void PurgeLogfile(benchmark::State &state) { Purge(state, false); }
static void PurgeShrink(benchmark::State &state) { Purge(state, true); }

BENCHMARK(LookupHit)->Name("CommitMap_LookupHit")->Arg(1024);
BENCHMARK(LookupMiss)->Name("CommitMap_LookupMiss")->Arg(1024);
BENCHMARK(InsertBatch)->Name("CommitMap_InsertBatch");
BENCHMARK(InsertGrow)->Name("CommitMap_InsertGrow");
BENCHMARK(PurgeLogfile)->Name("CommitMap_PurgeLogfile");
BENCHMARK(PurgeShrink)->Name("CommitMap_PurgeShrink");
