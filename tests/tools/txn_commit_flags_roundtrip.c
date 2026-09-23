#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db_config.h"
#include "db_int.h"
#include "dbinc/db_swap.h"
#include "dbinc/txn.h"
#include "tranlog_flags.h"

typedef int (*dispatch_fn)(DB_ENV *, DBT *, DB_LSN *, db_recops, void *);

int __db_add_recovery(DB_ENV *dbenv, dispatch_fn **dtabp, size_t *dtabsizep,
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

int gbl_utxnid_log = 1;
int gbl_is_physical_replicant = 0;

int __os_malloc(DB_ENV *dbenv, size_t size, void *storep)
{
    void *ptr = malloc(size ? size : 1);
    (void)dbenv;
    *(void **)storep = ptr;
    return ptr == NULL ? ENOMEM : 0;
}

void __os_free(DB_ENV *dbenv, void *ptr)
{
    (void)dbenv;
    free(ptr);
}

void __db_err(const DB_ENV *dbenv, const char *fmt, ...)
{
    (void)dbenv;
    (void)fmt;
    abort();
}

static int failures;
static int checks;

#define CHECK(cond, fmt, ...)                                                  \
    do {                                                                       \
        checks++;                                                              \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            fprintf(stderr, "FAIL %s:%d: " fmt "\n", __func__, __LINE__,    \
                    ##__VA_ARGS__);                                            \
        }                                                                      \
    } while (0)

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

static u_int8_t *put_header(u_int8_t *bp, u_int32_t rectype,
                            int utxnid_logged)
{
    u_int32_t type = rectype + (utxnid_logged ? 2000 : 0);
    u_int32_t txnid = T_TXNID;
    u_int64_t utxnid = T_UTXNID;
    DB_LSN prev = {T_PREV_FILE, T_PREV_OFF};

    LOGCOPY_32(bp, &type);
    bp += sizeof(type);
    LOGCOPY_32(bp, &txnid);
    bp += sizeof(txnid);
    LOGCOPY_FROMLSN(bp, &prev);
    bp += sizeof(prev);
    if (utxnid_logged) {
        LOGCOPY_64(bp, &utxnid);
        bp += sizeof(utxnid);
    }
    return bp;
}

static u_int8_t *put_locks(u_int8_t *bp)
{
    u_int32_t size = T_LOCKS_SZ;
    LOGCOPY_32(bp, &size);
    bp += sizeof(size);
    memcpy(bp, T_LOCKS, size);
    return bp + size;
}

static size_t encode_regop(u_int8_t *buf, int utxnid_logged)
{
    u_int8_t *bp = put_header(buf, DB___txn_regop, utxnid_logged);
    u_int32_t value = T_OPCODE;
    LOGCOPY_32(bp, &value);
    bp += sizeof(value);
    value = T_TS32;
    LOGCOPY_32(bp, &value);
    return (size_t)(put_locks(bp + sizeof(value)) - buf);
}

static size_t encode_regop_flags(u_int8_t *buf, int utxnid_logged,
                                 u_int32_t commit_flags)
{
    u_int8_t *bp = put_header(buf, DB___txn_regop_flags, utxnid_logged);
    u_int32_t value = T_OPCODE;
    LOGCOPY_32(bp, &value);
    bp += sizeof(value);
    value = T_TS32;
    LOGCOPY_32(bp, &value);
    bp += sizeof(value);
    LOGCOPY_32(bp, &commit_flags);
    return (size_t)(put_locks(bp + sizeof(commit_flags)) - buf);
}

static size_t encode_regop_gen(u_int8_t *buf, int utxnid_logged)
{
    u_int8_t *bp = put_header(buf, DB___txn_regop_gen, utxnid_logged);
    u_int32_t value = T_OPCODE;
    u_int64_t value64;
    LOGCOPY_32(bp, &value);
    bp += sizeof(value);
    value = T_GENERATION;
    LOGCOPY_32(bp, &value);
    bp += sizeof(value);
    value64 = T_CONTEXT;
    memcpy(bp, &value64, sizeof(value64));
    bp += sizeof(value64);
    value64 = T_TS64;
    LOGCOPY_64(bp, &value64);
    return (size_t)(put_locks(bp + sizeof(value64)) - buf);
}

static size_t encode_regop_gen_flags(u_int8_t *buf, u_int32_t rectype,
                                     int utxnid_logged,
                                     u_int32_t commit_flags)
{
    u_int8_t *bp = put_header(buf, rectype, utxnid_logged);
    u_int32_t value = T_OPCODE;
    u_int64_t value64;
    LOGCOPY_32(bp, &value);
    bp += sizeof(value);
    value = T_GENERATION;
    LOGCOPY_32(bp, &value);
    bp += sizeof(value);
    value64 = T_CONTEXT;
    memcpy(bp, &value64, sizeof(value64));
    bp += sizeof(value64);
    value64 = T_TS64;
    LOGCOPY_64(bp, &value64);
    bp += sizeof(value64);
    LOGCOPY_32(bp, &commit_flags);
    return (size_t)(put_locks(bp + sizeof(commit_flags)) - buf);
}

static void check_common(u_int32_t type, u_int32_t want_type, DB_TXN *txnid,
                         DB_LSN prev_lsn, int utxnid_logged, const char *label)
{
    CHECK(type == want_type, "%s type %u != %u", label, type, want_type);
    CHECK(txnid->txnid == T_TXNID, "%s txnid %#x", label, txnid->txnid);
    CHECK(txnid->utxnid == (utxnid_logged ? T_UTXNID : 0),
          "%s utxnid %" PRIx64, label, txnid->utxnid);
    CHECK(prev_lsn.file == T_PREV_FILE && prev_lsn.offset == T_PREV_OFF,
          "%s prev_lsn [%u][%u]", label, prev_lsn.file, prev_lsn.offset);
}

static void check_reader_semantics(const char *record_family,
                                   u_int32_t commit_flags)
{
    static const char *roles[] = {
        "serial", "concurrent", "recovery", "recovery-asof"
    };
    int expected_add =
        (commit_flags & TXN_COMMIT_F_SC_PRIVATE_SKIP_MAP) == 0;
    size_t role;

    for (role = 0; role < sizeof(roles) / sizeof(roles[0]); role++)
        CHECK(__txn_commit_map_should_add(commit_flags) == expected_add,
              "%s %s map decision for flags %#x", roles[role],
              record_family, commit_flags);
}

static void test_regop_flags(int utxnid_logged, u_int32_t commit_flags)
{
    u_int8_t buf[512] = {0};
    __txn_regop_flags_args *argp = NULL;
    size_t len = encode_regop_flags(buf, utxnid_logged, commit_flags);
    int ret = __txn_regop_flags_read_int(NULL, buf, len, 0, &argp);

    CHECK(ret == 0 && argp != NULL, "read failed: %d", ret);
    if (argp == NULL)
        return;
    check_common(argp->type, DB___txn_regop_flags +
                 (utxnid_logged ? 2000 : 0), argp->txnid, argp->prev_lsn,
                 utxnid_logged, "regop_flags");
    CHECK(argp->opcode == T_OPCODE, "opcode %u", argp->opcode);
    CHECK(argp->timestamp == T_TS32, "timestamp %ld", (long)argp->timestamp);
    CHECK(argp->commit_flags == commit_flags, "flags %#x", argp->commit_flags);
    check_reader_semantics("regop_flags", argp->commit_flags);
    CHECK(argp->locks.size == T_LOCKS_SZ, "locks size %u", argp->locks.size);
    CHECK(memcmp(argp->locks.data, T_LOCKS, T_LOCKS_SZ) == 0, "locks payload");
    free(argp);
}

static void test_gen_flags(u_int32_t rectype, int utxnid_logged,
                           u_int32_t commit_flags)
{
    u_int8_t buf[512] = {0};
    __txn_regop_gen_flags_args *argp = NULL;
    size_t len = encode_regop_gen_flags(buf, rectype, utxnid_logged,
                                        commit_flags);
    int ret = __txn_regop_gen_flags_read_int(NULL, buf, len, 0, &argp);

    CHECK(ret == 0 && argp != NULL, "read failed: %d", ret);
    if (argp == NULL)
        return;
    check_common(argp->type, rectype + (utxnid_logged ? 2000 : 0), argp->txnid,
                 argp->prev_lsn, utxnid_logged, "gen_flags");
    CHECK(argp->opcode == T_OPCODE, "opcode %u", argp->opcode);
    CHECK(argp->generation == T_GENERATION, "generation %u", argp->generation);
    CHECK(argp->context == T_CONTEXT, "context %" PRIx64, argp->context);
    CHECK(argp->timestamp == T_TS64, "timestamp %" PRIu64, argp->timestamp);
    CHECK(argp->commit_flags == commit_flags, "flags %#x", argp->commit_flags);
    check_reader_semantics("regop_gen_flags", argp->commit_flags);
    CHECK(argp->locks.size == T_LOCKS_SZ, "locks size %u", argp->locks.size);
    CHECK(memcmp(argp->locks.data, T_LOCKS, T_LOCKS_SZ) == 0, "locks payload");
    free(argp);
}

static void test_parents(int utxnid_logged)
{
    u_int8_t buf[512] = {0};
    __txn_regop_args *regop = NULL;
    __txn_regop_gen_args *gen = NULL;
    int ret;

    encode_regop(buf, utxnid_logged);
    ret = __txn_regop_read(NULL, buf, &regop);
    CHECK(ret == 0 && regop != NULL, "regop read %d", ret);
    if (regop != NULL) {
        check_common(regop->type, DB___txn_regop + (utxnid_logged ? 2000 : 0),
                     regop->txnid, regop->prev_lsn, utxnid_logged, "regop");
        CHECK(regop->opcode == T_OPCODE, "regop opcode");
        CHECK(regop->timestamp == T_TS32, "regop timestamp");
        CHECK(regop->locks.size == T_LOCKS_SZ, "regop locks");
        free(regop);
    }

    encode_regop_gen(buf, utxnid_logged);
    ret = __txn_regop_gen_read(NULL, buf, &gen);
    CHECK(ret == 0 && gen != NULL, "regop_gen read %d", ret);
    if (gen != NULL) {
        check_common(gen->type, DB___txn_regop_gen + (utxnid_logged ? 2000 : 0),
                     gen->txnid, gen->prev_lsn, utxnid_logged, "regop_gen");
        CHECK(gen->opcode == T_OPCODE, "gen opcode");
        CHECK(gen->generation == T_GENERATION, "gen generation");
        CHECK(gen->context == T_CONTEXT, "gen context");
        CHECK(gen->timestamp == T_TS64, "gen timestamp");
        free(gen);
    }
}

static void test_prefix(int utxnid_logged)
{
    u_int8_t parent[512] = {0}, flags[512] = {0};
    size_t header = sizeof(u_int32_t) * 2 + sizeof(DB_LSN) +
                    (utxnid_logged ? sizeof(u_int64_t) : 0);
    size_t prefix;
    u_int32_t value;

    encode_regop(parent, utxnid_logged);
    encode_regop_flags(flags, utxnid_logged, TXN_COMMIT_F_SC_PRIVATE_SKIP_MAP);
    prefix = header + sizeof(u_int32_t) * 2;
    CHECK(memcmp(parent + sizeof(u_int32_t), flags + sizeof(u_int32_t),
                 prefix - sizeof(u_int32_t)) == 0, "regop prefix");

    encode_regop_gen(parent, utxnid_logged);
    encode_regop_gen_flags(flags, DB___txn_regop_gen_flags, utxnid_logged,
                           TXN_COMMIT_F_SC_PRIVATE_SKIP_MAP);
    prefix = header + sizeof(u_int32_t) * 2 + sizeof(u_int64_t) * 2;
    CHECK(memcmp(parent + sizeof(u_int32_t), flags + sizeof(u_int32_t),
                 prefix - sizeof(u_int32_t)) == 0, "gen prefix");
    LOGCOPY_32(&value, flags + header + sizeof(u_int32_t));
    CHECK(value == T_GENERATION, "generation offset %#x", value);
    encode_regop_flags(flags, utxnid_logged, TXN_COMMIT_F_SC_PRIVATE_SKIP_MAP);
    LOGCOPY_32(&value, flags + header + sizeof(u_int32_t));
    CHECK(value == T_TS32, "timestamp offset %#x", value);
    LOGCOPY_32(&value, flags + header);
    CHECK(value == T_OPCODE, "opcode offset %#x", value);
}

static void test_truncation(int utxnid_logged)
{
    u_int8_t buf[512];
    __txn_regop_flags_args *regop;
    __txn_regop_gen_flags_args *gen;
    size_t len, index, locks_offset;
    u_int32_t oversized = UINT32_MAX;
    int ret;

    len = encode_regop_flags(buf, utxnid_logged,
                             TXN_COMMIT_F_SC_PRIVATE_SKIP_MAP);
    for (index = 0; index < len; index++) {
        regop = NULL;
        ret = __txn_regop_flags_read_int(NULL, buf, index, 0, &regop);
        CHECK(ret != 0 && regop == NULL, "regop accepted %zu/%zu", index, len);
        free(regop);
    }
    locks_offset = len - T_LOCKS_SZ - sizeof(u_int32_t);
    LOGCOPY_32(buf + locks_offset, &oversized);
    regop = NULL;
    ret = __txn_regop_flags_read_int(NULL, buf, len, 0, &regop);
    CHECK(ret != 0 && regop == NULL, "regop accepted oversized payload");
    free(regop);

    len = encode_regop_gen_flags(buf, DB___txn_regop_gen_flags_endianize,
                                 utxnid_logged,
                                 TXN_COMMIT_F_SC_PRIVATE_SKIP_MAP);
    for (index = 0; index < len; index++) {
        gen = NULL;
        ret = __txn_regop_gen_flags_read_int(NULL, buf, index, 0, &gen);
        CHECK(ret != 0 && gen == NULL, "gen accepted %zu/%zu", index, len);
        free(gen);
    }
    locks_offset = len - T_LOCKS_SZ - sizeof(u_int32_t);
    LOGCOPY_32(buf + locks_offset, &oversized);
    gen = NULL;
    ret = __txn_regop_gen_flags_read_int(NULL, buf, len, 0, &gen);
    CHECK(ret != 0 && gen == NULL, "gen accepted oversized payload");
    free(gen);
}

static void test_dispatch(const char *name,
                          int (*initializer)(DB_ENV *, dispatch_fn **, size_t *))
{
    static const u_int32_t types[] = {DB___txn_regop_flags,
        DB___txn_regop_gen_flags, DB___txn_regop_gen_flags_endianize};
    dispatch_fn *table = NULL;
    size_t table_size = 0;
    size_t index;

    CHECK(initializer(NULL, &table, &table_size) == 0, "%s initializer", name);
    for (index = 0; index < sizeof(types) / sizeof(types[0]); index++)
        CHECK(types[index] < table_size && table[types[index]] != NULL,
              "%s missing %u", name, types[index]);
    free(table);
}

static void test_transaction_log_capabilities(void)
{
    CHECK(!tranlog_has_commit_flags_v1(0),
        "missing transaction-log capability was treated as capable");
    CHECK(tranlog_has_commit_flags_v1(TRANLOG_CAP_TXN_COMMIT_FLAGS_V1),
        "V1 transaction-log capability was not recognized");
    CHECK(tranlog_has_commit_flags_v1(
          TRANLOG_CAP_TXN_COMMIT_FLAGS_V1 | 0x80000000u),
        "V1 capability was lost when an unknown bit was present");

    CHECK(tranlog_reader_accepts_commit_flags(0, 0),
        "old reader rejected legacy WAL");
    CHECK(!tranlog_reader_accepts_commit_flags(0, 1),
        "old reader accepted flag-bearing WAL");
    CHECK(tranlog_reader_accepts_commit_flags(1, 1),
        "V1 reader rejected flag-bearing WAL");
}

int main(void)
{
    int utxnid;

    for (utxnid = 0; utxnid <= 1; utxnid++) {
        test_parents(utxnid);
        test_regop_flags(utxnid, 0);
        test_regop_flags(utxnid, TXN_COMMIT_F_SC_PRIVATE_SKIP_MAP);
        test_regop_flags(utxnid,
                         TXN_COMMIT_F_SC_PRIVATE_SKIP_MAP | 0x80000000u);
        test_gen_flags(DB___txn_regop_gen_flags, utxnid, 0);
        test_gen_flags(DB___txn_regop_gen_flags, utxnid,
                       TXN_COMMIT_F_SC_PRIVATE_SKIP_MAP);
        test_gen_flags(DB___txn_regop_gen_flags_endianize, utxnid, 0);
        test_gen_flags(DB___txn_regop_gen_flags_endianize, utxnid,
                       TXN_COMMIT_F_SC_PRIVATE_SKIP_MAP);
        test_gen_flags(DB___txn_regop_gen_flags, utxnid, 0x00000002u);
        test_prefix(utxnid);
        test_truncation(utxnid);
    }
    test_dispatch("print", __txn_init_print);
#ifdef HAVE_REPLICATION
    test_dispatch("getpgnos", __txn_init_getpgnos);
    test_dispatch("getallpgnos", __txn_init_getallpgnos);
#endif
    test_dispatch("recover", __txn_init_recover);
    test_transaction_log_capabilities();

    printf("txn_commit_flags_roundtrip: %d checks, %d failures\n", checks,
           failures);
    return failures == 0 ? 0 : 1;
}