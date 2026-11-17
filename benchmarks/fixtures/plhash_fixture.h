#pragma once

#include <plhash.h>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>

struct PlHashEntry {
    uint64_t key;
    uint64_t value;
};

class PlHashFixture {
  public:
    static constexpr size_t batch = 256;
    std::vector<PlHashEntry> entries;
    std::unique_ptr<hash_t, decltype(&hash_free)> table{nullptr, hash_free};

    explicit PlHashFixture(size_t count)
    {
        if (count == 0 || count > 1000000)
            throw std::invalid_argument("plhash fixture size must be in [1, 1000000]");
        entries.resize(count + batch);
        table.reset(hash_init_o(offsetof(PlHashEntry, key), sizeof(uint64_t)));
        if (!table)
            throw std::runtime_error("hash_init_o failed");
        for (size_t i = 0; i < entries.size(); ++i)
            entries[i] = {static_cast<uint64_t>(i + 1), static_cast<uint64_t>(i ^ 0x5a5a)};
        for (size_t i = 0; i < count; ++i)
            add(i);
        if (hash_get_num_entries(table.get()) != static_cast<int>(count))
            throw std::runtime_error("plhash fixture population mismatch");
        for (size_t i = 0; i < count; ++i) {
            if (hash_find(table.get(), &entries[i].key) != &entries[i] ||
                hash_find_readonly(table.get(), &entries[i].key) != &entries[i])
                throw std::runtime_error("plhash fixture lookup mismatch");
        }
        uint64_t missing = count + batch + 1;
        if (hash_find(table.get(), &missing) != nullptr)
            throw std::runtime_error("plhash fixture miss unexpectedly found");
    }

    void add(size_t index)
    {
        if (hash_add(table.get(), &entries[index]) != 0)
            throw std::runtime_error("hash_add failed");
    }

    void remove(size_t index)
    {
        if (hash_del(table.get(), &entries[index]) != 0)
            throw std::runtime_error("hash_del failed");
    }
};

struct HashStats {
    int hits, misses, steps, buckets, entries, max_steps;
    explicit HashStats(hash_t* table)
    {
        hash_info2(table, &hits, &misses, &steps, &buckets, &entries, nullptr, nullptr, &max_steps);
    }
};

class CollisionFixture {
  public:
    std::vector<uint64_t> keys;
    std::unique_ptr<hash_t, decltype(&hash_free)> table{nullptr, hash_free};

    static unsigned int constant_hash(const void*, int) { return 0; }
    static int compare(const void* a, const void* b, int n) { return std::memcmp(a, b, n); }

    explicit CollisionFixture(size_t depth) : keys(depth)
    {
        table.reset(hash_init_user(constant_hash, compare, 0, sizeof(uint64_t)));
        if (!table)
            throw std::runtime_error("collision hash initialization failed");
        hash_config_query_stats(table.get(), 1);
        for (size_t i = 0; i < depth; ++i) {
            keys[i] = i + 1;
            if (hash_add(table.get(), &keys[i]))
                throw std::runtime_error("collision hash insertion failed");
        }
        for (size_t i = 0; i < depth; ++i) {
            const HashStats before(table.get());
            if (hash_find_readonly(table.get(), &keys[0]) != &keys[0])
                throw std::runtime_error("collision readonly mismatch");
            const HashStats after(table.get());
            // Native "steps" counts skipped nodes, excluding the matching node.
            if (after.steps - before.steps != static_cast<int>(depth - 1))
                throw std::runtime_error("collision chain depth mismatch");
        }
        // A complete ascending sweep restores the original reverse insertion order.
        for (size_t i = 0; i < depth; ++i) {
            const HashStats before(table.get());
            if (hash_find(table.get(), &keys[i]) != &keys[i])
                throw std::runtime_error("collision moving lookup mismatch");
            const HashStats after(table.get());
            if (after.steps - before.steps != static_cast<int>(depth - 1))
                throw std::runtime_error("collision moving depth mismatch");
        }
    }
};
