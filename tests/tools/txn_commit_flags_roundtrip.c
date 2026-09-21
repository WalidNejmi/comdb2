/*
 * Serialization round-trip tests for the flag-carrying commit records
 * (DB___txn_regop_flags / DB___txn_regop_gen_flags[_endianize]).
 *
 * These records exist so that a commit-map omission decided by the master can
 * be made durable.  The whole design rests on two properties that are easy to
 * break silently and impossible to notice without a test:
 *
 *   1. ROUND TRIP -- every field survives encode -> decode, for both the
 *      utxnid-logged (+2000) and non-utxnid forms, with commit_flags clear,
 *      set, and carrying unknown bits.
 *
 *   2. PARENT-PREFIX EQUIVALENCE -- the flag-carrying record is byte-identical
 *      to its parent record up to the appended commit_flags field.  Several
 *      consumers (rep_verify's timestamp parse, __txn_force_abort's opcode
 *      overwrite, __log_put_int_int's generation parse) locate fields by
 *      computed byte offset rather than by decoding, and they are only correct
 *      for the new records because of this property.  If someone reorders the
 *      layout so commit_flags is no longer last-before-locks, those consumers
 *      start reading garbage at runtime -- here it fails loudly instead.
 *
 * The encoders below deliberately open-code the wire layout rather than
 * calling __txn_*_log(): the log functions require a live DB_ENV, and an
 * independent encoder is what makes the prefix comparison meaningful (a shared
 * encoder would agree with itself no matter how wrong it was).  The decoders
 * are the real production ones.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include "db_config.h"
#include "db_int.h"
#include "dbinc/db_swap.h"
#include "dbinc/txn.h"

typedef int (*dispatch_fn)(DB_ENV *, DBT *, DB_LSN *, db_recops, void *);

int
__db_add_recovery(DB_ENV *dbenv, dispatch_fn **dtabp, size_t *dtabsizep,
                  dispatch_fn func, u_int32_t ndx)
{
    dispatch_fn *new_table;
    size_t old_size = *dtabsizep;

    (void)dbenv;
    if (ndx >= old_size) {
        new_table = realloc(*dtabp, (ndx + 1) * sizeof(*new_table));
        if (new_table == NULL)
            return ENOMEM;
        memset(new_table + old_size, 0,
               (ndx + 1 - old_size) * sizeof(*new_table));
        *dtabp = new_table;
        *dtabsizep = ndx + 1;
    }
    (*dtabp)[ndx] = func;
    return 0;
}

extern int __txn_init_print(DB_ENV *, dispatch_fn **, size_t *);
#ifdef HAVE_REPLICATION
extern int __txn_init_getpgnos(DB_ENV *, dispatch_fn **, size_t *);
extern int __txn_init_getallpgnos(DB_ENV *, dispatch_fn **, size_t *);
#endif
extern int __txn_init_recover(DB_ENV *, dispatch_fn **, size_t *);

/*
 * This test links against txn_auto.c alone, so that it exercises the real
 * production decoders without dragging in the entire server.  The decoders
 * themselves need only __os_malloc/__os_free and gbl_utxnid_log; the remaining
 * symbols are referenced by other functions in the same translation unit and
 * are never called from here.  They abort rather than returning a plausible
 * value so that a future change which does start calling them fails loudly
 * instead of silently testing a stub.
 */
int gbl_utxnid_log = 1;
int gbl_is_physical_replicant = 0;

int
__os_malloc(DB_ENV *dbenv, size_t size, void *storep)
{
    void *p = malloc(size ? size : 1);
    (void)dbenv;
    *(void **)storep = p;
    return p == NULL ? ENOMEM : 0;
}

void
__os_free(DB_ENV *dbenv, void *ptr)
{
    (void)dbenv;
    free(ptr);
}

void
__db_err(const DB_ENV *dbenv, const char *fmt, ...)
{
    (void)dbenv;
    (void)fmt;
    abort();
}

/* Remaining link stubs live in txn_commit_flags_roundtrip_stubs.c. */

static int failures = 0;
static int checks = 0;

#define CHECK(cond, fmt, ...)                                                  \
    do {                                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            fprintf(stderr, "FAIL %s:%d: " fmt "\n", __func__, __LINE__,       \
                    ##__VA_ARGS__);                                            \
        }                                                                      \
    } while (0)

/* Values used by every case, chosen so a field swap cannot go unnoticed. */
#define T_TXNID      0x11223344u
#define T_UTXNID     0xAABBCCDD00112233ull
#define T_PREV_FILE  7
#define T_PREV_OFF   0x2000
#define T_OPCODE     TXN_COMMIT
#define T_GENERATION 0x0000BEEFu
#define T_TS32       0x5F5E0100
#define T_TS64       0x000000005F5E0100ull
#define T_CONTEXT    0x0123456789ABCDEFull

static const char T_LOCKS[] = "lockpayload-0123456789abcdef";
#define T_LOCKS_SZ ((u_int32_t)sizeof(T_LOCKS))

/* Common header: rectype, txnid, prev_lsn, [utxnid]. */
static u_int8_t *
put_header(u_int8_t *bp, u_int32_t rectype, int utxnid_logged)
{
    u_int32_t t32 = rectype + (utxnid_logged ? 2000 : 0);
    u_int32_t txnid = T_TXNID;
    u_int64_t utxnid = T_UTXNID;
    DB_LSN prev;

    prev.file = T_PREV_FILE;
    prev.offset = T_PREV_OFF;

    LOGCOPY_32(bp, &t32);
    bp += sizeof(t32);
    LOGCOPY_32(bp, &txnid);
    bp += sizeof(txnid);
    LOGCOPY_FROMLSN(bp, &prev);
    bp += sizeof(DB_LSN);
    if (utxnid_logged) {
        LOGCOPY_64(bp, &utxnid);
        bp += sizeof(utxnid);
    }
    return bp;
}

static u_int8_t *
put_locks(u_int8_t *bp)
{
    u_int32_t sz = T_LOCKS_SZ;
    LOGCOPY_32(bp, &sz);
    bp += sizeof(sz);
    memcpy(bp, T_LOCKS, sz);
    bp += sz;
    return bp;
}

/* regop: opcode, timestamp, locks */
static size_t
encode_regop(u_int8_t *buf, int utxnid_logged)
{
    u_int8_t *bp = put_header(buf, DB___txn_regop, utxnid_logged);
    u_int32_t t32;

    t32 = T_OPCODE;
    LOGCOPY_32(bp, &t32);
    bp += sizeof(t32);
    t32 = (u_int32_t)T_TS32;
    LOGCOPY_32(bp, &t32);
    bp += sizeof(t32);
    bp = put_locks(bp);
    return (size_t)(bp - buf);
}

/* regop_flags: opcode, timestamp, commit_flags, locks */
static size_t
encode_regop_flags(u_int8_t *buf, int utxnid_logged, u_int32_t commit_flags)
{
    u_int8_t *bp = put_header(buf, DB___txn_regop_flags, utxnid_logged);
    u_int32_t t32;

    t32 = T_OPCODE;
    LOGCOPY_32(bp, &t32);
    bp += sizeof(t32);
    t32 = (u_int32_t)T_TS32;
    LOGCOPY_32(bp, &t32);
    bp += sizeof(t32);
    t32 = commit_flags;
    LOGCOPY_32(bp, &t32);
    bp += sizeof(t32);
    bp = put_locks(bp);
    return (size_t)(bp - buf);
}

/* regop_gen: opcode, generation, context, timestamp, locks */
static size_t
encode_regop_gen(u_int8_t *buf, int utxnid_logged)
{
    u_int8_t *bp = put_header(buf, DB___txn_regop_gen, utxnid_logged);
    u_int32_t t32;
    u_int64_t t64;

    t32 = T_OPCODE;
    LOGCOPY_32(bp, &t32);
    bp += sizeof(t32);
    t32 = T_GENERATION;
    LOGCOPY_32(bp, &t32);
    bp += sizeof(t32);
    t64 = T_CONTEXT; /* written raw, as __txn_regop_gen_read_int memcpy's it */
    memcpy(bp, &t64, sizeof(t64));
    bp += sizeof(t64);
    t64 = T_TS64;
    LOGCOPY_64(bp, &t64);
    bp += sizeof(t64);
    bp = put_locks(bp);
    return (size_t)(bp - buf);
}

/* regop_gen_flags: opcode, generation, context, timestamp, commit_flags, locks */
static size_t
encode_regop_gen_flags(u_int8_t *buf, u_int32_t rectype, int utxnid_logged,
                       u_int32_t commit_flags)
{
    u_int8_t *bp = put_header(buf, rectype, utxnid_logged);
    u_int32_t t32;
    u_int64_t t64;

    t32 = T_OPCODE;
    LOGCOPY_32(bp, &t32);
    bp += sizeof(t32);
    t32 = T_GENERATION;
    LOGCOPY_32(bp, &t32);
    bp += sizeof(t32);
    t64 = T_CONTEXT;
    memcpy(bp, &t64, sizeof(t64));
    bp += sizeof(t64);
    t64 = T_TS64;
    LOGCOPY_64(bp, &t64);
    bp += sizeof(t64);
    t32 = commit_flags;
    LOGCOPY_32(bp, &t32);
    bp += sizeof(t32);
    bp = put_locks(bp);
    return (size_t)(bp - buf);
}

static void
check_common(u_int32_t type, u_int32_t want_type, DB_TXN *txnid, DB_LSN prev_lsn,
             int utxnid_logged, const char *label)
{
    checks++;
    if (type != want_type)
        fprintf(stderr, "FAIL %s: type %u != %u\n", label, type, want_type),
            failures++;

    checks++;
    if (txnid->txnid != T_TXNID)
        fprintf(stderr, "FAIL %s: txnid %#x != %#x\n", label, txnid->txnid,
                T_TXNID),
            failures++;

    checks++;
    if (txnid->utxnid != (utxnid_logged ? T_UTXNID : 0))
        fprintf(stderr, "FAIL %s: utxnid %" PRIx64 " wrong\n", label,
                txnid->utxnid),
            failures++;

    checks++;
    if (prev_lsn.file != T_PREV_FILE || prev_lsn.offset != T_PREV_OFF)
        fprintf(stderr, "FAIL %s: prev_lsn [%u][%u] wrong\n", label,
                prev_lsn.file, prev_lsn.offset),
            failures++;
}

static void
test_regop_flags(int utxnid_logged, u_int32_t commit_flags)
{
    u_int8_t buf[512];
    __txn_regop_flags_args *argp = NULL;
    size_t len;
    int ret;

    memset(buf, 0, sizeof(buf));
    len = encode_regop_flags(buf, utxnid_logged, commit_flags);
    (void)len;

    ret = __txn_regop_flags_read_int(NULL, buf, len, 0, &argp);
    CHECK(ret == 0 && argp != NULL, "read_int failed ret=%d", ret);
    if (ret != 0 || argp == NULL)
        return;

    check_common(argp->type,
                 DB___txn_regop_flags + (utxnid_logged ? 2000 : 0),
                 argp->txnid, argp->prev_lsn, utxnid_logged,
                 "regop_flags");
    CHECK(argp->opcode == T_OPCODE, "opcode %u", argp->opcode);
    CHECK(argp->timestamp == T_TS32, "timestamp %ld", (long)argp->timestamp);
    CHECK(argp->commit_flags == commit_flags, "commit_flags %#x != %#x",
          argp->commit_flags, commit_flags);
    CHECK(argp->locks.size == T_LOCKS_SZ, "locks.size %u", argp->locks.size);
    CHECK(argp->locks.data != NULL &&
              memcmp(argp->locks.data, T_LOCKS, T_LOCKS_SZ) == 0,
          "locks payload mismatch");

    free(argp);
}

static void
test_regop_gen_flags(u_int32_t rectype, int utxnid_logged,
                     u_int32_t commit_flags)
{
    u_int8_t buf[512];
    __txn_regop_gen_flags_args *argp = NULL;
    size_t len;
    int ret;

    memset(buf, 0, sizeof(buf));
    len = encode_regop_gen_flags(buf, rectype, utxnid_logged, commit_flags);
    (void)len;

    ret = __txn_regop_gen_flags_read_int(NULL, buf, len, 0, &argp);
    CHECK(ret == 0 && argp != NULL, "read_int failed ret=%d", ret);
    if (ret != 0 || argp == NULL)
        return;

    check_common(argp->type, rectype + (utxnid_logged ? 2000 : 0), argp->txnid,
                 argp->prev_lsn, utxnid_logged, "regop_gen_flags");
    CHECK(argp->opcode == T_OPCODE, "opcode %u", argp->opcode);
    CHECK(argp->generation == T_GENERATION, "generation %u", argp->generation);
    CHECK(argp->context == T_CONTEXT, "context %" PRIx64, argp->context);
    CHECK(argp->timestamp == T_TS64, "timestamp %" PRIu64, argp->timestamp);
    CHECK(argp->commit_flags == commit_flags, "commit_flags %#x != %#x",
          argp->commit_flags, commit_flags);
    CHECK(argp->locks.size == T_LOCKS_SZ, "locks.size %u", argp->locks.size);
    CHECK(argp->locks.data != NULL &&
              memcmp(argp->locks.data, T_LOCKS, T_LOCKS_SZ) == 0,
          "locks payload mismatch");

    free(argp);
}

/*
 * The parents must still decode exactly as before -- these records are on disk
 * in every existing database and nothing about them may shift.
 */
static void
test_parents_unchanged(int utxnid_logged)
{
    u_int8_t buf[512];
    __txn_regop_args *r = NULL;
    __txn_regop_gen_args *g = NULL;
    int ret;

    memset(buf, 0, sizeof(buf));
    encode_regop(buf, utxnid_logged);
    ret = __txn_regop_read(NULL, buf, &r);
    CHECK(ret == 0 && r != NULL, "regop read_int failed ret=%d", ret);
    if (r != NULL) {
        check_common(r->type, DB___txn_regop + (utxnid_logged ? 2000 : 0),
                     r->txnid, r->prev_lsn, utxnid_logged, "regop");
        CHECK(r->opcode == T_OPCODE, "regop opcode %u", r->opcode);
        CHECK(r->timestamp == T_TS32, "regop timestamp %ld",
              (long)r->timestamp);
        CHECK(r->locks.size == T_LOCKS_SZ, "regop locks.size %u",
              r->locks.size);
        free(r);
    }

    memset(buf, 0, sizeof(buf));
    encode_regop_gen(buf, utxnid_logged);
    ret = __txn_regop_gen_read(NULL, buf, &g);
    CHECK(ret == 0 && g != NULL, "regop_gen read_int failed ret=%d", ret);
    if (g != NULL) {
        check_common(g->type, DB___txn_regop_gen + (utxnid_logged ? 2000 : 0),
                     g->txnid, g->prev_lsn, utxnid_logged, "regop_gen");
        CHECK(g->opcode == T_OPCODE, "regop_gen opcode %u", g->opcode);
        CHECK(g->generation == T_GENERATION, "regop_gen generation %u",
              g->generation);
        CHECK(g->context == T_CONTEXT, "regop_gen context %" PRIx64,
              g->context);
        CHECK(g->timestamp == T_TS64, "regop_gen timestamp %" PRIu64,
              g->timestamp);
        free(g);
    }
}

/*
 * PARENT-PREFIX EQUIVALENCE.  Everything up to commit_flags must be laid out
 * identically to the parent record, so that the byte-offset consumers stay
 * correct.  Compare the encodings with the rectype word masked out (that is
 * the one field that legitimately differs).
 */
static void
test_parent_prefix_equivalence(int utxnid_logged)
{
    u_int8_t a[512], b[512];
    size_t hdr = sizeof(u_int32_t) + sizeof(u_int32_t) + sizeof(DB_LSN) +
                 (utxnid_logged ? sizeof(u_int64_t) : 0);
    size_t prefix;

    /* regop vs regop_flags: opcode + timestamp precede commit_flags. */
    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));
    encode_regop(a, utxnid_logged);
    encode_regop_flags(b, utxnid_logged, TXN_COMMIT_F_SC_PRIVATE_SKIP_MAP);
    prefix = hdr + sizeof(u_int32_t) /*opcode*/ + sizeof(u_int32_t) /*ts*/;
    CHECK(memcmp(a + sizeof(u_int32_t), b + sizeof(u_int32_t),
                 prefix - sizeof(u_int32_t)) == 0,
          "regop_flags prefix differs from regop (utxnid=%d) -- byte-offset "
          "consumers would misread it",
          utxnid_logged);

    /* regop_gen vs regop_gen_flags: opcode+generation+context+timestamp. */
    memset(a, 0, sizeof(a));
    memset(b, 0, sizeof(b));
    encode_regop_gen(a, utxnid_logged);
    encode_regop_gen_flags(b, DB___txn_regop_gen_flags, utxnid_logged,
                           TXN_COMMIT_F_SC_PRIVATE_SKIP_MAP);
    prefix = hdr + sizeof(u_int32_t) /*opcode*/ + sizeof(u_int32_t) /*gen*/ +
             sizeof(u_int64_t) /*context*/ + sizeof(u_int64_t) /*ts*/;
    CHECK(memcmp(a + sizeof(u_int32_t), b + sizeof(u_int32_t),
                 prefix - sizeof(u_int32_t)) == 0,
          "regop_gen_flags prefix differs from regop_gen (utxnid=%d) -- "
          "byte-offset consumers would misread it",
          utxnid_logged);

    /*
     * The specific offsets the fragile consumers use, spelled out so a layout
     * change fails here with a name attached rather than as a mystery.
     */
    {
        u_int32_t v;
        /* __log_put_int_int(): generation at hdr + sizeof(opcode). */
        memset(b, 0, sizeof(b));
        encode_regop_gen_flags(b, DB___txn_regop_gen_flags, utxnid_logged,
                               TXN_COMMIT_F_SC_PRIVATE_SKIP_MAP);
        LOGCOPY_32(&v, b + hdr + sizeof(u_int32_t));
        CHECK(v == T_GENERATION,
              "log_put generation offset wrong for regop_gen_flags: %#x", v);

        /* rep_verify: timestamp at hdr + sizeof(opcode), for the regop family. */
        memset(b, 0, sizeof(b));
        encode_regop_flags(b, utxnid_logged,
                           TXN_COMMIT_F_SC_PRIVATE_SKIP_MAP);
        LOGCOPY_32(&v, b + hdr + sizeof(u_int32_t));
        CHECK(v == (u_int32_t)T_TS32,
              "rep_verify timestamp offset wrong for regop_flags: %#x", v);

        /* __txn_force_abort(): opcode at hdr. */
        LOGCOPY_32(&v, b + hdr);
        CHECK(v == T_OPCODE,
              "force_abort opcode offset wrong for regop_flags: %#x", v);
    }
}

static void
test_truncated_records(int utxnid_logged)
{
    u_int8_t buf[512];
    __txn_regop_flags_args *regop = NULL;
    __txn_regop_gen_flags_args *gen = NULL;
    size_t len, i, locks_offset;
    u_int32_t oversized = UINT32_MAX;
    int ret;

    len = encode_regop_flags(buf, utxnid_logged,
                             TXN_COMMIT_F_SC_PRIVATE_SKIP_MAP);
    for (i = 0; i < len; i++) {
        regop = NULL;
        ret = __txn_regop_flags_read_int(NULL, buf, i, 0, &regop);
        CHECK(ret != 0 && regop == NULL,
              "regop_flags accepted truncated size %zu/%zu", i, len);
        free(regop);
    }
    locks_offset = len - T_LOCKS_SZ - sizeof(u_int32_t);
    LOGCOPY_32(buf + locks_offset, &oversized);
    ret = __txn_regop_flags_read_int(NULL, buf, len, 0, &regop);
    CHECK(ret != 0 && regop == NULL,
          "regop_flags accepted oversized locks payload");
    free(regop);

    len = encode_regop_gen_flags(buf, DB___txn_regop_gen_flags_endianize,
                                 utxnid_logged,
                                 TXN_COMMIT_F_SC_PRIVATE_SKIP_MAP);
    for (i = 0; i < len; i++) {
        gen = NULL;
        ret = __txn_regop_gen_flags_read_int(NULL, buf, i, 0, &gen);
        CHECK(ret != 0 && gen == NULL,
              "regop_gen_flags accepted truncated size %zu/%zu", i, len);
        free(gen);
    }
    locks_offset = len - T_LOCKS_SZ - sizeof(u_int32_t);
    LOGCOPY_32(buf + locks_offset, &oversized);
    ret = __txn_regop_gen_flags_read_int(NULL, buf, len, 0, &gen);
    CHECK(ret != 0 && gen == NULL,
          "regop_gen_flags accepted oversized locks payload");
    free(gen);
}

static void
test_dispatch_initializer(const char *name,
                          int (*initializer)(DB_ENV *, dispatch_fn **,
                                             size_t *))
{
    static const u_int32_t rectypes[] = {
        DB___txn_regop_flags,
        DB___txn_regop_gen_flags,
        DB___txn_regop_gen_flags_endianize,
    };
    dispatch_fn *table = NULL;
    size_t table_size = 0;
    size_t i;
    int ret;

    ret = initializer(NULL, &table, &table_size);
    CHECK(ret == 0, "%s initializer returned %d", name, ret);
    for (i = 0; i < sizeof(rectypes) / sizeof(rectypes[0]); i++)
        CHECK(rectypes[i] < table_size && table[rectypes[i]] != NULL,
              "%s missing rectype %u", name, rectypes[i]);
    free(table);
}

static void
test_dispatch_registrations(void)
{
    test_dispatch_initializer("print", __txn_init_print);
#ifdef HAVE_REPLICATION
    test_dispatch_initializer("getpgnos", __txn_init_getpgnos);
    test_dispatch_initializer("getallpgnos", __txn_init_getallpgnos);
#endif
    test_dispatch_initializer("recover", __txn_init_recover);
}

int
main(int argc, char *argv[])
{
    int utxnid;

    (void)argc;
    (void)argv;

    for (utxnid = 0; utxnid <= 1; utxnid++) {
        test_parents_unchanged(utxnid);

        test_regop_flags(utxnid, 0);
        test_regop_flags(utxnid, TXN_COMMIT_F_SC_PRIVATE_SKIP_MAP);
        /* Unknown bits must survive decode untouched (forward compatibility:
         * a newer master may set bits this build does not understand). */
        test_regop_flags(utxnid, TXN_COMMIT_F_SC_PRIVATE_SKIP_MAP | 0x80000000u);

        test_regop_gen_flags(DB___txn_regop_gen_flags, utxnid, 0);
        test_regop_gen_flags(DB___txn_regop_gen_flags, utxnid,
                             TXN_COMMIT_F_SC_PRIVATE_SKIP_MAP);
        test_regop_gen_flags(DB___txn_regop_gen_flags_endianize, utxnid, 0);
        test_regop_gen_flags(DB___txn_regop_gen_flags_endianize, utxnid,
                             TXN_COMMIT_F_SC_PRIVATE_SKIP_MAP);
        test_regop_gen_flags(DB___txn_regop_gen_flags, utxnid, 0x00000002u);

        test_parent_prefix_equivalence(utxnid);
        test_truncated_records(utxnid);
    }
    test_dispatch_registrations();

    printf("txn_commit_flags_roundtrip: %d checks, %d failures\n", checks,
           failures);
    return failures == 0 ? 0 : 1;
}
