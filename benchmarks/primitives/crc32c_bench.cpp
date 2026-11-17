#include <benchmark/benchmark.h>
#include <crc32c.h>
#include <stdexcept>
#include <vector>

static uint32_t reference_crc(const uint8_t* data, size_t size)
{
    uint32_t crc = 0;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ ((crc & 1) ? 0x82f63b78U : 0);
    }
    return crc;
}

template <bool Software>
static void checksum(benchmark::State& state)
{
    const uint32_t size = state.range(0);
    std::vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; ++i)
        bytes[i] = (i * 73 + i / 7) & 255;
    crc32c_init(0);
    const uint8_t known[] = "123456789";
    if (crc32c_software(known, 9, 0) != 0x58e3fa20U ||
        crc32c_comdb2(known, 9) != 0x58e3fa20U ||
        crc32c_software(bytes.data(), size, 0) != reference_crc(bytes.data(), size) ||
        crc32c_comdb2(bytes.data(), size) != reference_crc(bytes.data(), size))
        throw std::runtime_error("CRC32C fixture/reference mismatch");
    for (auto _ : state) {
        const auto value = Software ? crc32c_software(bytes.data(), size, CRC32C_SEED)
                                    : crc32c_comdb2(bytes.data(), size);
        benchmark::DoNotOptimize(value);
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * size);
    state.counters["bytes_in"] = size;
}

BENCHMARK_TEMPLATE(checksum, false)->Name("CRC32C_Comdb2")->Arg(64)->Arg(256)->Arg(4096)->Arg(65536);
BENCHMARK_TEMPLATE(checksum, true)->Name("CRC32C_Software")->Arg(64)->Arg(256)->Arg(4096)->Arg(65536);
