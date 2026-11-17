#include <benchmark/benchmark.h>
extern "C" {
#include <comdb2rle.h>
}
#include <stdexcept>
#include <vector>

enum class Shape { Zeros, Repeated8, Mixed, Incompressible };

template <Shape Input, bool Decompress>
static void rle(benchmark::State& state)
{
    const size_t size = state.range(0);
    std::vector<uint8_t> bytes(size), compressed(size * 2 + 64), decoded(size);
    uint32_t random = 0x5a17c9e3U;
    for (size_t i = 0; i < size; ++i) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        bytes[i] = Input == Shape::Zeros ? 0 :
                   Input == Shape::Repeated8 ? (i % 8) * 31 + 1 :
                   Input == Shape::Mixed && (i / 64) % 2 == 0 ? 0x5a : random & 255;
    }
    Comdb2RLE encode{bytes.data(), size, compressed.data(), compressed.size()};
    if (compressComdb2RLE(&encode) || encode.outsz == 0)
        throw std::runtime_error("RLE fixture compression failed");
    const size_t compressed_size = encode.outsz;
    Comdb2RLE decode{compressed.data(), compressed_size, decoded.data(), size};
    if (decompressComdb2RLE(&decode) || decode.outsz != size || decoded != bytes)
        throw std::runtime_error("RLE fixture round trip failed");
    int errors = 0;
    Comdb2RLE operation = Decompress ? decode : encode;
    for (auto _ : state) {
        operation.outsz = Decompress ? decoded.size() : compressed.size();
        errors |= Decompress ? decompressComdb2RLE(&operation) : compressComdb2RLE(&operation);
        benchmark::DoNotOptimize(operation.outsz);
        benchmark::ClobberMemory();
    }
    if (errors || operation.outsz != (Decompress ? size : compressed_size))
        throw std::runtime_error("RLE measured operation failed");
    decode.outsz = size;
    if (decompressComdb2RLE(&decode) || decoded != bytes)
        throw std::runtime_error("RLE final round trip failed");
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * size);
    state.counters["bytes_in"] = Decompress ? compressed_size : size;
    state.counters["bytes_out"] = Decompress ? size : compressed_size;
    state.counters["compressed_over_original"] = static_cast<double>(compressed_size) / size;
}

#define RLE_CASE(shape) \
    BENCHMARK_TEMPLATE(rle, Shape::shape, false)->Name("RLE_Compress/" #shape)->Arg(256)->Arg(4096)->Arg(65536); \
    BENCHMARK_TEMPLATE(rle, Shape::shape, true)->Name("RLE_Decompress/" #shape)->Arg(256)->Arg(4096)->Arg(65536)
RLE_CASE(Zeros);
RLE_CASE(Repeated8);
RLE_CASE(Mixed);
RLE_CASE(Incompressible);
