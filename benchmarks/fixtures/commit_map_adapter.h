#ifndef MICROBENCH_COMMIT_MAP_ADAPTER_H
#define MICROBENCH_COMMIT_MAP_ADAPTER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct commit_map_fixture;
struct commit_map_stats {
    int64_t entries, buckets, resizes, slabs, slots;
};

struct commit_map_fixture *commit_map_create(void);
void commit_map_destroy(struct commit_map_fixture *);
int commit_map_add(struct commit_map_fixture *, uint64_t id, uint32_t file);
int commit_map_get(struct commit_map_fixture *, uint64_t id, uint32_t *file, uint32_t *offset);
int commit_map_purge(struct commit_map_fixture *, uint32_t file);
int commit_map_notfound(void);
struct commit_map_stats commit_map_snapshot(struct commit_map_fixture *);

#ifdef __cplusplus
}
#endif
#endif
