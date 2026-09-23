#include <stdio.h>
#include <stdlib.h>

int gbl_log_cksum_prev = 0;

#define UNUSED_STUB(name)                                                      \
    void name(void);                                                           \
    void name(void)                                                            \
    {                                                                          \
        fprintf(stderr, "txn_commit_flags_roundtrip: stub %s was called\n",  \
                #name);                                                        \
        abort();                                                               \
    }

UNUSED_STUB(__lock_get_list)
UNUSED_STUB(__log_put)
UNUSED_STUB(__log_put_commit_context)
UNUSED_STUB(__rep_check_alloc)
UNUSED_STUB(__txn_activekids)
UNUSED_STUB(__txn_child_recover)
UNUSED_STUB(__txn_ckp_recover)
UNUSED_STUB(__txn_dist_abort_recover)
UNUSED_STUB(__txn_dist_commit_recover)
UNUSED_STUB(__txn_dist_prepare_recover)
UNUSED_STUB(__txn_recycle_recover)
UNUSED_STUB(__txn_regop_flags_recover)
UNUSED_STUB(__txn_regop_gen_flags_recover)
UNUSED_STUB(__txn_regop_gen_recover)
UNUSED_STUB(__txn_regop_recover)
UNUSED_STUB(__txn_regop_rowlocks_recover)
UNUSED_STUB(__txn_xa_regop_recover)
UNUSED_STUB(fsnapf)

void *comdb2_malloc_static(void *pool, unsigned long size)
{
    (void)pool;
    return malloc(size ? size : 1);
}

void *comdb2_realloc_static(void *pool, void *ptr, unsigned long size)
{
    (void)pool;
    return realloc(ptr, size ? size : 1);
}

void comdb2_free(void *ptr)
{
    free(ptr);
}