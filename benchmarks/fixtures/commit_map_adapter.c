#include "db_config.h"
#include "db_int.h"
#include "dbinc/txn.h"
#include <mem_restore.h>
#include "commit_map_adapter.h"

/* Production callers also declare these entry points: the checked-in txn_ext
   header predates the map API. Keep the actual DB_ENV/DB_LSN signatures. */
int __txn_commit_map_init(DB_ENV *);
int __txn_commit_map_destroy(DB_ENV *);
int __txn_commit_map_add(DB_ENV *, u_int64_t, DB_LSN);
int __txn_commit_map_get(DB_ENV *, u_int64_t, DB_LSN *);
int __txn_commit_map_delete_logfile_txns(DB_ENV *, u_int32_t);

struct commit_map_fixture {
    DB_ENV environment;
};

static pthread_once_t allocator_once = PTHREAD_ONCE_INIT;
static int allocator_status;

static void initialize_allocator(void)
{
    allocator_status = comdb2ma_init(0, 0);
}

struct commit_map_fixture *commit_map_create(void)
{
    /* Same private-map setup as __txn_commit_map_selftest; map allocations
       still use the real named comdb2 allocator, not system-allocation stubs. */
    Pthread_once(&allocator_once, initialize_allocator);
    if (allocator_status != 0)
        return NULL;
    struct commit_map_fixture *fixture = calloc(1, sizeof(*fixture));
    if (!fixture)
        return NULL;
    fixture->environment.use_sys_malloc = 1;
    if (__txn_commit_map_init(&fixture->environment) != 0) {
        free(fixture);
        return NULL;
    }
    return fixture;
}

void commit_map_destroy(struct commit_map_fixture *fixture)
{
    if (fixture) {
        __txn_commit_map_destroy(&fixture->environment);
        free(fixture);
    }
}

int commit_map_add(struct commit_map_fixture *fixture, uint64_t id, uint32_t file)
{
    DB_LSN lsn = {file, (uint32_t)id};
    return __txn_commit_map_add(&fixture->environment, id, lsn);
}

int commit_map_get(struct commit_map_fixture *fixture, uint64_t id, uint32_t *file, uint32_t *offset)
{
    DB_LSN lsn = {0, 0};
    int rc = __txn_commit_map_get(&fixture->environment, id, &lsn);
    *file = lsn.file;
    *offset = lsn.offset;
    return rc;
}

int commit_map_purge(struct commit_map_fixture *fixture, uint32_t file)
{
    return __txn_commit_map_delete_logfile_txns(&fixture->environment, file);
}

int commit_map_notfound(void)
{
    return DB_NOTFOUND;
}

struct commit_map_stats commit_map_snapshot(struct commit_map_fixture *fixture)
{
    DB_TXN_COMMIT_MAP *map = fixture->environment.txmap;
    struct commit_map_stats stats;
    Pthread_mutex_lock(&map->txmap_mutexp);
    stats = (struct commit_map_stats){map->nentries, (int64_t)1 << map->bits,
                                      map->nresizes, map->nslabs, map->nslots};
    Pthread_mutex_unlock(&map->txmap_mutexp);
    return stats;
}
