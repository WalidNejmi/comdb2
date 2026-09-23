/*
 * Link stubs for txn_commit_flags_roundtrip.
 *
 * The round-trip test links against berkdb's txn_auto.c object so that it
 * exercises the real production decoders rather than a copy of them.  That
 * object also contains the log writers and the dispatch-registration tables,
 * which reference symbols from the rest of the server.  None of them are
 * reachable from the decode paths the test drives.
 *
 * This file is compiled WITHOUT the berkdb headers on purpose: declaring these
 * with their real prototypes would require pulling in the very headers whose
 * types drag the rest of the tree along.  Every stub aborts, so if a future
 * change makes the test actually reach one, it fails loudly instead of quietly
 * validating a stub.
 */

#include <stdio.h>
#include <stdlib.h>

int gbl_log_cksum_prev = 0;

#define UNUSED_STUB(name)                                                      \
    void name(void);                                                           \
    void name(void)                                                            \
    {                                                                          \
        fprintf(stderr, "txn_commit_flags_roundtrip: stub %s was called; the " \
                        "test is no longer exercising only the decoders\n",    \
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

/*
 * mem_berkdb.h turns __os_malloc's fallback into comdb2_malloc_berkdb(), an
 * inline wrapper over the per-thread allocator.  The test's __os_malloc does
 * not use it, but the inline definition is emitted anyway; satisfy it with
 * plain malloc.
 */
void *comdb2_malloc_static(void *pool, unsigned long size);
void *comdb2_malloc_static(void *pool, unsigned long size)
{
    (void)pool;
    return malloc(size ? size : 1);
}

void *comdb2_realloc_static(void *pool, void *ptr, unsigned long size);
void *comdb2_realloc_static(void *pool, void *ptr, unsigned long size)
{
    (void)pool;
    return realloc(ptr, size ? size : 1);
}

/*
 * With PER_THREAD_MALLOC, mem_berkdb.h maps berkdb's allocator onto
 * comdb2_malloc_static/comdb2_free.  Route both to the C library so the
 * decoders allocate and release argument structs normally.
 */
void comdb2_free(void *ptr);
void comdb2_free(void *ptr)
{
    free(ptr);
}
