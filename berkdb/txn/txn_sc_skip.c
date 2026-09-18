/*
 * Omitting schema-change replacement-file transactions from the commit-LSN map.
 *
 * A rebuilding schema change commits roughly once per converted row, and every
 * one of those commits currently gets an entry in the in-memory
 * utxnid -> commit_lsn map.  The map is retained while the WAL is, so its
 * memory grows with the number of rows copied.
 *
 * The rule implemented here is deliberately narrow:
 *
 *   schema-change build B
 *       registers its rebuilt replacement files as  fileid -> B
 *
 *   the base converter marks its transactions with  sc_build_id = B
 *
 *   a physical write by such a transaction to a file registered to B
 *       sets sc_skip_commit_map
 *
 *   at commit, that transaction is not added to the map
 *
 * The bit records one positive fact: this transaction wrote one of its own
 * build's replacement files.  It is never cleared.  A converter transaction
 * that also writes llmeta progress is still skipped -- that is intended, not
 * an oversight.
 *
 * Everything else is unchanged.  The transaction, its writes and its commit
 * record are all logged normally; only the in-memory map insertion is omitted.
 */

#include "db_config.h"

#ifndef lint
static const char revid[] = "$Id: txn_sc_skip.c,v 1.0 2026/09/15 00:00:00 comdb2 Exp $";
#endif /* not lint */

#ifndef NO_SYSTEM_INCLUDES
#include <sys/types.h>
#include <string.h>
#endif

#include "db_int.h"
#include "dbinc/db_page.h"
#include "dbinc/db_shash.h"
#include "dbinc/txn.h"
#include "dbinc/db_am.h"
#include "logmsg.h"
#include "comdb2_atomic.h"

/*
 * Classification counters.
 *
 * These exist to separate "a transaction was never a copy transaction" from
 * "it was, but did not match a file".  Without them a test can only watch the
 * skip count, which cannot tell those two apart.
 *
 * marked   a root transaction was given a build id by the base converter
 * matched  such a transaction wrote a file registered to that same build
 *
 * Atomic rather than lock-protected: marked is on the per-converter-
 * transaction path and matched is on the physical-write path, neither of
 * which should take the registry lock just to count.  Reads in
 * __sc_direct_copy_stats() use the same atomic load so a stats caller never
 * sees a torn 64-bit value.
 */
static u_int64_t sc_direct_copy_txns_marked = 0;
static u_int64_t sc_direct_copy_txns_matched = 0;

/*
 * Durable-flag OBSERVATION counters.
 *
 * The flag-carrying commit records (regop_flags / regop_gen_flags) are written
 * and replicated, but nothing acts on them yet: master, replicas and recovery
 * all still add every root entry.  These counters are how we prove, before
 * changing any behaviour, that all four consumers would make the SAME decision
 * for the same transactions -- which is the gate the plan puts in front of
 * actually honouring the flag.
 *
 * "emitted/decoded" counts records that carried a non-zero commit_flags at that
 * consumer; "would_skip" counts those whose flags say SC_PRIVATE_SKIP_MAP and
 * which that consumer would therefore have omitted.  They are separate because
 * a future flag bit could be set without implying a skip.
 *
 * IMPORTANT -- these count DECODE EVENTS, not distinct transactions.  The
 * master emits each record once, and each live apply path sees each record
 * once, so for those three the count equals the transaction count.  Recovery
 * does NOT: it replays the log in more than one pass, so its counters are a
 * whole-number multiple of the transactions involved (measured 4x on a
 * replica bounce).  Compare recovery against itself -- decoded vs would_skip --
 * rather than against the master's absolute number.
 *
 * The unsupported_* counters record why a transaction that was otherwise a
 * direct-copy candidate did NOT get the flag.  They exist so a zero skip count
 * can be explained rather than merely observed.
 *
 * Atomic for the same reason as the counters above: these sit on the commit and
 * replication-apply paths and must not take a lock merely to count.
 */
static u_int64_t sc_flags_emitted_master = 0;
static u_int64_t sc_would_skip_master = 0;
static u_int64_t sc_flags_decoded_serial = 0;
static u_int64_t sc_would_skip_serial = 0;
static u_int64_t sc_flags_decoded_concurrent = 0;
static u_int64_t sc_would_skip_concurrent = 0;
static u_int64_t sc_flags_decoded_recovery = 0;
static u_int64_t sc_would_skip_recovery = 0;
static u_int64_t sc_unsupported_children = 0;
static u_int64_t sc_unsupported_rowlock = 0;
static u_int64_t sc_unsupported_distributed = 0;
static u_int64_t sc_unsupported_unknown_family = 0;

/*
 * __sc_commit_flags_note --
 *	Record one observation.  `which` selects the counter; see
 *	SC_OBS_* in dbinc/txn.h.
 *
 * PUBLIC: void __sc_commit_flags_note __P((int, u_int32_t));
 */
void
__sc_commit_flags_note(which, commit_flags)
	int which;
	u_int32_t commit_flags;
{
	int would_skip = (commit_flags & TXN_COMMIT_F_SC_PRIVATE_SKIP_MAP) != 0;

	switch (which) {
	case SC_OBS_MASTER:
		(void)ATOMIC_ADD64(sc_flags_emitted_master, 1);
		if (would_skip)
			(void)ATOMIC_ADD64(sc_would_skip_master, 1);
		break;
	case SC_OBS_SERIAL:
		(void)ATOMIC_ADD64(sc_flags_decoded_serial, 1);
		if (would_skip)
			(void)ATOMIC_ADD64(sc_would_skip_serial, 1);
		break;
	case SC_OBS_CONCURRENT:
		(void)ATOMIC_ADD64(sc_flags_decoded_concurrent, 1);
		if (would_skip)
			(void)ATOMIC_ADD64(sc_would_skip_concurrent, 1);
		break;
	case SC_OBS_RECOVERY:
		(void)ATOMIC_ADD64(sc_flags_decoded_recovery, 1);
		if (would_skip)
			(void)ATOMIC_ADD64(sc_would_skip_recovery, 1);
		break;
	case SC_OBS_UNSUP_CHILDREN:
		(void)ATOMIC_ADD64(sc_unsupported_children, 1);
		break;
	case SC_OBS_UNSUP_ROWLOCK:
		(void)ATOMIC_ADD64(sc_unsupported_rowlock, 1);
		break;
	case SC_OBS_UNSUP_DISTRIBUTED:
		(void)ATOMIC_ADD64(sc_unsupported_distributed, 1);
		break;
	case SC_OBS_UNSUP_UNKNOWN_FAMILY:
		(void)ATOMIC_ADD64(sc_unsupported_unknown_family, 1);
		break;
	default:
		break;
	}
}

/*
 * __sc_commit_flags_stats --
 *	Snapshot the observation counters into the caller's struct.
 *
 * PUBLIC: void __sc_commit_flags_stats __P((SC_COMMIT_FLAGS_STATS *));
 */
void
__sc_commit_flags_stats(st)
	SC_COMMIT_FLAGS_STATS *st;
{
	st->flags_emitted_master = ATOMIC_LOAD64(sc_flags_emitted_master);
	st->would_skip_master = ATOMIC_LOAD64(sc_would_skip_master);
	st->flags_decoded_serial = ATOMIC_LOAD64(sc_flags_decoded_serial);
	st->would_skip_serial = ATOMIC_LOAD64(sc_would_skip_serial);
	st->flags_decoded_concurrent = ATOMIC_LOAD64(sc_flags_decoded_concurrent);
	st->would_skip_concurrent = ATOMIC_LOAD64(sc_would_skip_concurrent);
	st->flags_decoded_recovery = ATOMIC_LOAD64(sc_flags_decoded_recovery);
	st->would_skip_recovery = ATOMIC_LOAD64(sc_would_skip_recovery);
	st->unsupported_children = ATOMIC_LOAD64(sc_unsupported_children);
	st->unsupported_rowlock = ATOMIC_LOAD64(sc_unsupported_rowlock);
	st->unsupported_distributed = ATOMIC_LOAD64(sc_unsupported_distributed);
	st->unsupported_unknown_family =
	    ATOMIC_LOAD64(sc_unsupported_unknown_family);
}

/*
 * __sc_private_file_registry_init --
 *	Create the replacement-file registry.  Called once from env open.
 *
 * PUBLIC: int __sc_private_file_registry_init __P((DB_ENV *));
 */
int
__sc_private_file_registry_init(dbenv)
	DB_ENV *dbenv;
{
	SC_PRIVATE_FILE_REGISTRY *reg;
	int ret;

	if ((ret = __os_calloc(dbenv, 1,
	    sizeof(SC_PRIVATE_FILE_REGISTRY), &reg)) != 0)
		goto err;

	reg->files = hash_init_o(offsetof(SC_PRIVATE_FILE, fileid),
	    DB_FILE_ID_LEN);
	if (reg->files == NULL) {
		__os_free(dbenv, reg);
		ret = ENOMEM;
		goto err;
	}

	Pthread_mutex_init(&reg->lk, NULL);
	dbenv->sc_private_files = reg;

	return (0);
err:
	logmsg(LOGMSG_ERROR,
	    "Failed to initialize schema-change replacement-file registry\n");
	return (ret);
}

static int
free_sc_private_file(void *obj, void *arg)
{
	__os_free((DB_ENV *)arg, obj);
	return (0);
}

/*
 * __sc_private_file_registry_destroy --
 *
 * PUBLIC: int __sc_private_file_registry_destroy __P((DB_ENV *));
 */
int
__sc_private_file_registry_destroy(dbenv)
	DB_ENV *dbenv;
{
	SC_PRIVATE_FILE_REGISTRY *reg = dbenv->sc_private_files;

	if (reg == NULL)
		return (0);

	hash_for(reg->files, &free_sc_private_file, (void *)dbenv);
	hash_clear(reg->files);
	hash_free(reg->files);

	Pthread_mutex_destroy(&reg->lk);
	__os_free(dbenv, reg);
	dbenv->sc_private_files = NULL;

	return (0);
}

/*
 * __sc_private_file_register --
 *	Record that a physical file is a replacement file of an active build.
 *
 *	Only newly rebuilt, not-yet-public files may be passed here.  A planned
 *	schema change can rename and reuse an existing file instead of
 *	rebuilding it; registering one of those would make ordinary writes to a
 *	public file look like private construction.  The caller owns that
 *	decision.
 *
 * PUBLIC: int __sc_private_file_register __P((DB_ENV *, const u_int8_t *,
 * PUBLIC:	   u_int64_t));
 */
int
__sc_private_file_register(dbenv, fileid, build_id)
	DB_ENV *dbenv;
	const u_int8_t *fileid;
	u_int64_t build_id;
{
	SC_PRIVATE_FILE_REGISTRY *reg = dbenv->sc_private_files;
	SC_PRIVATE_FILE *f;
	int ret = 0;

	if (reg == NULL || fileid == NULL || build_id == 0)
		return (EINVAL);

	Pthread_mutex_lock(&reg->lk);

	f = hash_find(reg->files, fileid);
	if (f != NULL) {
		/*
		 * A different build claiming the same physical file means the
		 * lifecycle is not what we think it is.  Refuse rather than
		 * silently re-point it.
		 */
		if (f->build_id != build_id) {
			logmsg(LOGMSG_ERROR,
			    "%s: file already registered to build 0x%"PRIx64
			    ", refusing to re-register to 0x%"PRIx64"\n",
			    __func__, f->build_id, build_id);
			ret = EEXIST;
		}
		goto done;
	}

	if ((ret = __os_calloc(dbenv, 1, sizeof(SC_PRIVATE_FILE), &f)) != 0) {
		ret = ENOMEM;
		goto done;
	}

	memcpy(f->fileid, fileid, DB_FILE_ID_LEN);
	f->build_id = build_id;
	hash_add(reg->files, f);

done:
	Pthread_mutex_unlock(&reg->lk);
	return (ret);
}

/*
 * __sc_private_file_lookup --
 *	Returns 0 and the owning build if the file is a registered replacement
 *	file, DB_NOTFOUND otherwise.
 *
 * PUBLIC: int __sc_private_file_lookup __P((DB_ENV *, const u_int8_t *,
 * PUBLIC:	   u_int64_t *));
 */
int
__sc_private_file_lookup(dbenv, fileid, build_id_out)
	DB_ENV *dbenv;
	const u_int8_t *fileid;
	u_int64_t *build_id_out;
{
	SC_PRIVATE_FILE_REGISTRY *reg = dbenv->sc_private_files;
	SC_PRIVATE_FILE *f;
	int ret = DB_NOTFOUND;

	if (reg == NULL || fileid == NULL)
		return (DB_NOTFOUND);

	Pthread_mutex_lock(&reg->lk);

	f = hash_find(reg->files, fileid);
	if (f != NULL) {
		*build_id_out = f->build_id;
		ret = 0;
	}

	Pthread_mutex_unlock(&reg->lk);
	return (ret);
}

struct sc_private_find_arg {
	u_int64_t build_id;
	SC_PRIVATE_FILE *found;
};

static int
find_one_build_file(void *obj, void *arg)
{
	SC_PRIVATE_FILE *f = obj;
	struct sc_private_find_arg *a = arg;

	if (f->build_id != a->build_id)
		return (0);

	a->found = f;
	return (1); /* stop the walk */
}

/*
 * __sc_private_file_unregister_build --
 *	Drop every file belonging to a build.  Idempotent, so terminal
 *	schema-change paths can call it unconditionally.
 *
 * PUBLIC: int __sc_private_file_unregister_build __P((DB_ENV *, u_int64_t));
 */
int
__sc_private_file_unregister_build(dbenv, build_id)
	DB_ENV *dbenv;
	u_int64_t build_id;
{
	SC_PRIVATE_FILE_REGISTRY *reg = dbenv->sc_private_files;
	struct sc_private_find_arg arg;

	if (reg == NULL || build_id == 0)
		return (0);

	Pthread_mutex_lock(&reg->lk);

	/*
	 * Deliberately allocation-free: cleanup must not be able to fail under
	 * memory pressure, or a build's files would stay registered and a
	 * later transaction could match them.  hash_del() during hash_for() is
	 * unsafe, so find one victim per walk -- a registry holds one table's
	 * data, index and blob files, so this is a handful of entries once per
	 * schema change.
	 */
	arg.build_id = build_id;
	for (;;) {
		arg.found = NULL;
		hash_for(reg->files, &find_one_build_file, &arg);

		if (arg.found == NULL)
			break;

		hash_del(reg->files, arg.found);
		__os_free(dbenv, arg.found);
	}

	Pthread_mutex_unlock(&reg->lk);
	return (0);
}

/*
 * __sc_private_registry_note_failure --
 *	A build could not be registered; it runs without the optimization.
 *
 * PUBLIC: void __sc_private_registry_note_failure __P((DB_ENV *));
 */
void
__sc_private_registry_note_failure(dbenv)
	DB_ENV *dbenv;
{
	SC_PRIVATE_FILE_REGISTRY *reg = dbenv->sc_private_files;

	if (reg == NULL)
		return;

	Pthread_mutex_lock(&reg->lk);
	reg->failed_registrations++;
	Pthread_mutex_unlock(&reg->lk);
}

/*
 * __sc_private_registry_stats --
 *	Enough to tell "no schema change ran" apart from "the mechanism was
 *	never available".
 *
 * PUBLIC: void __sc_private_registry_stats __P((DB_ENV *, int *, u_int64_t *,
 * PUBLIC:	   u_int64_t *));
 */
void
__sc_private_registry_stats(dbenv, available, nfiles, failures)
	DB_ENV *dbenv;
	int *available;
	u_int64_t *nfiles;
	u_int64_t *failures;
{
	SC_PRIVATE_FILE_REGISTRY *reg = dbenv->sc_private_files;

	if (reg == NULL) {
		*available = 0;
		*nfiles = 0;
		*failures = 0;
		return;
	}

	Pthread_mutex_lock(&reg->lk);
	*available = 1;
	*nfiles = (u_int64_t)hash_get_num_entries(reg->files);
	*failures = reg->failed_registrations;
	Pthread_mutex_unlock(&reg->lk);
}

/*
 * __txn_set_sc_build --
 *	Mark a transaction as belonging to an active schema-change build.
 *
 *	Called only by the base converter, immediately after it starts the
 *	transaction.  Not from trans_start_sc_lowpri(), which also serves
 *	logical redo and other schema-change work.
 *
 * PUBLIC: void __txn_set_sc_build __P((DB_TXN *, u_int64_t));
 */
void
__txn_set_sc_build(txnp, build_id)
	DB_TXN *txnp;
	u_int64_t build_id;
{
	if (txnp == NULL || build_id == 0)
		return;

	/*
	 * The skip decision at commit only looks at root transactions, so a
	 * child would be marked and then ignored -- harmless, but it would
	 * silently disable the optimization.  Converter transactions are
	 * started with a NULL parent (trans_start_sc_lowpri), so this should
	 * not happen; say so if it ever does.
	 */
	if (txnp->parent != NULL) {
		static int warned = 0;

		if (!warned) {
			warned = 1;
			logmsg(LOGMSG_WARN,
			    "%s: converter transaction has a parent; "
			    "commit-map skip will not apply\n", __func__);
		}
		return;
	}

	txnp->sc_build_id = build_id;
	txnp->sc_skip_commit_map = 0;

	(void)ATOMIC_ADD64(sc_direct_copy_txns_marked, 1);
}

/*
 * __txn_note_sc_file_write_int --
 *	Slow path of the physical-write check; see __txn_note_sc_file_write().
 *
 *	This is the entire rule.  There is deliberately no else branch: a write
 *	to some other file must not undo an earlier match.
 *
 * PUBLIC: void __txn_note_sc_file_write_int __P((DB_TXN *, DB *));
 */
void
__txn_note_sc_file_write_int(txnp, dbp)
	DB_TXN *txnp;
	DB *dbp;
{
	u_int64_t file_build_id;

	if (__sc_private_file_lookup(txnp->mgrp->dbenv, dbp->fileid,
	    &file_build_id) != 0)
		return;

	if (file_build_id == txnp->sc_build_id) {
		if (!txnp->sc_skip_commit_map)
			(void)ATOMIC_ADD64(sc_direct_copy_txns_matched, 1);
		txnp->sc_skip_commit_map = 1;
	}
}

/*
 * __sc_direct_copy_stats --
 *	How many transactions the converter marked, and how many of those went
 *	on to write one of their own build's files.
 *
 * PUBLIC: void __sc_direct_copy_stats __P((u_int64_t *, u_int64_t *));
 */
void
__sc_direct_copy_stats(marked, matched)
	u_int64_t *marked;
	u_int64_t *matched;
{
	*marked = ATOMIC_LOAD64(sc_direct_copy_txns_marked);
	*matched = ATOMIC_LOAD64(sc_direct_copy_txns_matched);
}
