#include <benchmark/benchmark.h>
#include <cstddef>
#include <pool.h>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
constexpr size_t batch = 256;
using Pool = std::unique_ptr<pool_t, decltype(&pool_free)>;

void check_counts(pool_t* pool, int entries, int used, int blocks)
{
    int actual_entries, actual_used, actual_blocks;
    pool_info(pool, &actual_entries, &actual_used, &actual_blocks);
    if (actual_entries != entries || actual_used != used || actual_blocks != blocks)
        throw std::runtime_error("pool fixture counts mismatch");
}

void acquire(pool_t* pool, std::vector<void*>& slots)
{
    for (auto& slot : slots) {
        slot = pool_getablk(pool);
        if (!slot)
            throw std::runtime_error("pool fixture allocation failed");
    }
}

void release(pool_t* pool, const std::vector<void*>& slots)
{
    for (auto slot : slots)
        if (!slot || pool_relablk(pool, slot))
            throw std::runtime_error("pool fixture return failed");
}

enum class Operation { Get, Zeroed, Return, GetReturn };

template <Operation Op>
void steady(benchmark::State& state)
{
    const size_t size = state.range(0);
    Pool pool(pool_init(size, batch), pool_free);
    if (!pool)
        throw std::runtime_error("pool_init failed");
    std::vector<void*> slots(batch);
    acquire(pool.get(), slots);
    check_counts(pool.get(), batch, batch, 1);
    for (auto slot : slots)
        std::memset(slot, 0x5a, size);
    if (Op != Operation::Return)
        release(pool.get(), slots);

    while (state.KeepRunningBatch(batch)) {
        for (size_t i = 0; i < batch; ++i) {
            if (Op == Operation::Return) {
                benchmark::DoNotOptimize(pool_relablk(pool.get(), slots[i]));
            } else {
                slots[i] = Op == Operation::Zeroed ? pool_getzblk(pool.get()) : pool_getablk(pool.get());
                benchmark::DoNotOptimize(slots[i]);
                if (Op == Operation::GetReturn)
                    benchmark::DoNotOptimize(pool_relablk(pool.get(), slots[i]));
            }
        }
        benchmark::ClobberMemory();
        state.PauseTiming();
        check_counts(pool.get(), batch, Op == Operation::Get || Op == Operation::Zeroed ? batch : 0, 1);
        if (Op == Operation::Return) {
            acquire(pool.get(), slots);
        } else if (Op != Operation::GetReturn) {
            for (auto slot : slots) {
                if (!slot)
                    throw std::runtime_error("pool get returned NULL");
                if (Op == Operation::Zeroed) {
                    const auto* bytes = static_cast<unsigned char*>(slot);
                    for (size_t i = 0; i < size; ++i)
                        if (bytes[i] != 0)
                            throw std::runtime_error("pool_getzblk did not zero entry");
                    std::memset(slot, 0x5a, size);
                }
            }
            release(pool.get(), slots);
        }
        state.ResumeTiming();
    }
    state.counters["pool_entries"] = batch;
    state.counters["pool_blocks"] = 1;
    state.counters["used_after_timed_batch"] = Op == Operation::Get || Op == Operation::Zeroed ? batch : 0;
    state.counters["grow_events_per_op"] = 0;
    state.SetItemsProcessed(state.iterations());
}

void Pool_Grow(benchmark::State& state)
{
    constexpr size_t growth_batch = 32;
    const int size = state.range(0), step = state.range(1);
    std::vector<Pool> pools;
    std::vector<void*> slots(growth_batch);
    for (size_t i = 0; i < growth_batch; ++i) {
        pools.emplace_back(pool_init(size, step), pool_free);
        if (!pools.back())
            throw std::runtime_error("growth pool_init failed");
        check_counts(pools.back().get(), 0, 0, 0);
    }
    while (state.KeepRunningBatch(growth_batch)) {
        for (size_t i = 0; i < growth_batch; ++i) {
            slots[i] = pool_getablk(pools[i].get());
            benchmark::DoNotOptimize(slots[i]);
        }
        state.PauseTiming();
        for (size_t i = 0; i < growth_batch; ++i) {
            if (!slots[i])
                throw std::runtime_error("pool growth allocation failed");
            check_counts(pools[i].get(), step, 1, 1);
            pool_clear(pools[i].get());
            check_counts(pools[i].get(), 0, 0, 0);
        }
        state.ResumeTiming();
    }
    state.counters["pool_entries_after_growth"] = step;
    state.counters["pool_blocks_after_growth"] = 1;
    state.counters["used_after_growth"] = 1;
    state.counters["grow_events_per_op"] = 1;
    state.SetItemsProcessed(state.iterations());
}
} // namespace

BENCHMARK_TEMPLATE(steady, Operation::Get)->Name("Pool_Get")->Arg(32)->Arg(128)->Arg(512);
BENCHMARK_TEMPLATE(steady, Operation::Zeroed)->Name("Pool_GetZeroed")->Arg(32)->Arg(128)->Arg(512);
BENCHMARK_TEMPLATE(steady, Operation::Return)->Name("Pool_Return")->Arg(32)->Arg(128)->Arg(512);
BENCHMARK_TEMPLATE(steady, Operation::GetReturn)->Name("Pool_GetReturn")->Arg(32)->Arg(128)->Arg(512);
BENCHMARK(Pool_Grow)->ArgNames({"entry", "step"})->Args({32, 64})->Args({128, 64})
    ->Args({512, 64})->Args({128, 512});
