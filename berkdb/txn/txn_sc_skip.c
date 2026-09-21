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
 * The match bit records that this transaction wrote one of its own build's
 * replacement files.  A separate bit rejects any transaction that also writes
 * an unregistered user-table file.  Internal metadata such as llmeta progress
 * is not a user-table file and remains allowed.
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
int gbl_sc_fence_publish_fail_after = INT_MAX;

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
 * PUBLIC:	   const sc_build_id_t *));
 */
int
__sc_private_file_register(dbenv, fileid, build_id)
	DB_ENV *dbenv;
	const u_int8_t *fileid;
	const sc_build_id_t *build_id;
{
	SC_PRIVATE_FILE_REGISTRY *reg = dbenv->sc_private_files;
	SC_PRIVATE_FILE *f;
	int ret = 0;

	if (reg == NULL || fileid == NULL || sc_build_id_is_zero(build_id))
		return (EINVAL);

	Pthread_mutex_lock(&reg->lk);

	f = hash_find(reg->files, fileid);
	if (f != NULL) {
		/*
		 * A different build claiming the same physical file means the
		 * lifecycle is not what we think it is.  Refuse rather than
		 * silently re-point it.
		 */
		if (!sc_build_id_equal(&f->build_id, build_id)) {
			logmsg(LOGMSG_ERROR,
			    "%s: file already belongs to another schema-change build\n",
			    __func__);
			ret = EEXIST;
		}
		goto done;
	}

	if ((ret = __os_calloc(dbenv, 1, sizeof(SC_PRIVATE_FILE), &f)) != 0) {
		ret = ENOMEM;
		goto done;
	}

	memcpy(f->fileid, fileid, DB_FILE_ID_LEN);
	f->build_id = *build_id;
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
 * PUBLIC:	   sc_build_id_t *));
 */
int
__sc_private_file_lookup(dbenv, fileid, build_id_out)
	DB_ENV *dbenv;
	const u_int8_t *fileid;
	sc_build_id_t *build_id_out;
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
	const sc_build_id_t *build_id;
	SC_PRIVATE_FILE *found;
};

static int
find_one_build_file(void *obj, void *arg)
{
	SC_PRIVATE_FILE *f = obj;
	struct sc_private_find_arg *a = arg;

	if (!sc_build_id_equal(&f->build_id, a->build_id))
		return (0);

	a->found = f;
	return (1); /* stop the walk */
}

/*
 * __sc_private_file_unregister_build --
 *	Drop every file belonging to a build.  Idempotent, so terminal
 *	schema-change paths can call it unconditionally.
 *
 * PUBLIC: int __sc_private_file_unregister_build __P((DB_ENV *,
 * PUBLIC:	   const sc_build_id_t *));
 */
int
__sc_private_file_unregister_build(dbenv, build_id)
	DB_ENV *dbenv;
	const sc_build_id_t *build_id;
{
	SC_PRIVATE_FILE_REGISTRY *reg = dbenv->sc_private_files;
	struct sc_private_find_arg arg;

	if (reg == NULL || sc_build_id_is_zero(build_id))
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
 * __sc_publication_fence_registry_init --
 *	Create the publication-fence registry.  Called once from env open.
 *
 * PUBLIC: int __sc_publication_fence_registry_init __P((DB_ENV *));
 */
int
__sc_publication_fence_registry_init(dbenv)
	DB_ENV *dbenv;
{
	SC_PUBLICATION_FENCE_REGISTRY *reg;
	int ret;

	if ((ret = __os_calloc(dbenv, 1,
	    sizeof(SC_PUBLICATION_FENCE_REGISTRY), &reg)) != 0)
		goto err;

	reg->files = hash_init_o(offsetof(SC_PUBLICATION_FENCE, fileid),
	    DB_FILE_ID_LEN);
	if (reg->files == NULL) {
		__os_free(dbenv, reg);
		ret = ENOMEM;
		goto err;
	}

	Pthread_mutex_init(&reg->lk, NULL);
	reg->epoch = 1;
	dbenv->sc_publication_fences = reg;

	return (0);
err:
	logmsg(LOGMSG_ERROR,
	    "Failed to initialize schema-change publication-fence registry\n");
	return (ret);
}

static int
free_sc_publication_fence(void *obj, void *arg)
{
	__os_free((DB_ENV *)arg, obj);
	return (0);
}

/*
 * __sc_publication_fence_registry_destroy --
 *
 * PUBLIC: int __sc_publication_fence_registry_destroy __P((DB_ENV *));
 */
int
__sc_publication_fence_registry_destroy(dbenv)
	DB_ENV *dbenv;
{
	SC_PUBLICATION_FENCE_REGISTRY *reg = dbenv->sc_publication_fences;

	if (reg == NULL)
		return (0);

	hash_for(reg->files, &free_sc_publication_fence, (void *)dbenv);
	hash_clear(reg->files);
	hash_free(reg->files);

	Pthread_mutex_destroy(&reg->lk);
	__os_free(dbenv, reg);
	dbenv->sc_publication_fences = NULL;

	return (0);
}

/*
 * __sc_publication_fence_pend --
 *	Record a rebuilt physical file whose generation is not published yet.
 *
 *	Called with the exact set of files the schema-change plan rebuilt, at
 *	the point where that set is authoritative.  A reused file must never be
 *	passed here: a fence on a public file would let reconstruction stop
 *	early and surface writes that did not exist at the snapshot's target.
 *
 *	The entry starts with a zero fence, which never satisfies the stopping
 *	rule, so nothing changes until the build publishes.
 *
 * PUBLIC: int __sc_publication_fence_pend __P((DB_ENV *, const u_int8_t *,
 * PUBLIC:	   const sc_build_id_t *));
 */
int
__sc_publication_fence_pend(dbenv, fileid, build_id)
	DB_ENV *dbenv;
	const u_int8_t *fileid;
	const sc_build_id_t *build_id;
{
	SC_PUBLICATION_FENCE_REGISTRY *reg = dbenv->sc_publication_fences;
	SC_PUBLICATION_FENCE *f;
	int ret = 0;

	if (reg == NULL || fileid == NULL || sc_build_id_is_zero(build_id))
		return (EINVAL);

	Pthread_mutex_lock(&reg->lk);

	f = hash_find(reg->files, fileid);
	if (f != NULL) {
		/*
		 * A file id left behind by an earlier build of the same
		 * physical file.  The new build owns it now; reset it to
		 * unpublished so a stale fence cannot outlive its generation.
		 */
		ZERO_LSN(f->fence_lsn);
		f->build_id = *build_id;
		goto done;
	}

	if ((ret = __os_calloc(dbenv, 1, sizeof(*f), &f)) != 0)
		goto done;

	memcpy(f->fileid, fileid, DB_FILE_ID_LEN);
	ZERO_LSN(f->fence_lsn);
	f->build_id = *build_id;

	if (hash_add(reg->files, f) != 0) {
		__os_free(dbenv, f);
		ret = ENOMEM;
	}

done:
	Pthread_mutex_unlock(&reg->lk);
	return (ret);
}

extern int __mempv_cache_invalidate_file __P((DB_ENV *, u_int8_t *));

/*
 * __sc_publication_fence_drop_cached_pages --
 *	Throw away any reconstructed pages cached for a file whose fence has
 *	just become known.
 *
 *	A page reconstructed while the fence was absent can have been unwound
 *	past the published image -- exactly the bug the fence prevents -- and
 *	the bad image is cached under the target LSN that produced it.
 *	Installing the fence fixes later reconstructions but not what is
 *	already cached, and __mempv_cache_get() serves a cached image to any
 *	snapshot with the same target LSN.  Drop them; the re-reconstruction
 *	they force now has a fence to stop at.
 *
 *	Called with no registry lock held, so the cache lock is never nested
 *	inside it.
 */
static void
__sc_publication_fence_drop_cached_pages(dbenv, fileid)
	DB_ENV *dbenv;
	const u_int8_t *fileid;
{
	SC_PUBLICATION_FENCE_REGISTRY *reg = dbenv->sc_publication_fences;
	int ndropped;

	ndropped = __mempv_cache_invalidate_file(dbenv, (u_int8_t *)fileid);
	if (ndropped <= 0)
		return;

	logmsg(LOGMSG_INFO,
	    "Dropped %d cached page version(s) for a file that just gained a "
	    "schema-change publication fence\n", ndropped);

	if (reg != NULL) {
		Pthread_mutex_lock(&reg->lk);
		reg->cached_pages_dropped += (u_int64_t)ndropped;
		Pthread_mutex_unlock(&reg->lk);
	}
}

/*
 * __sc_publication_fence_install --
 *	Install an already-published fence directly.
 *
 *	Used when rebuilding the registry from its durable records, where each
 *	file's fence is known up front and there is no pending phase.
 *
 * PUBLIC: int __sc_publication_fence_install __P((DB_ENV *, const u_int8_t *,
 * PUBLIC:	   const sc_build_id_t *, DB_LSN));
 */
int
__sc_publication_fence_install(dbenv, fileid, build_id, fence_lsn)
	DB_ENV *dbenv;
	const u_int8_t *fileid;
	const sc_build_id_t *build_id;
	DB_LSN fence_lsn;
{
	SC_PUBLICATION_FENCE_REGISTRY *reg = dbenv->sc_publication_fences;
	SC_PUBLICATION_FENCE *f;
	int ret = 0, changed = 0;

	if (reg == NULL || fileid == NULL || IS_ZERO_LSN(fence_lsn))
		return (EINVAL);

	Pthread_mutex_lock(&reg->lk);

	f = hash_find(reg->files, fileid);
	if (f != NULL) {
		changed = log_compare(&f->fence_lsn, &fence_lsn) != 0;
		f->fence_lsn = fence_lsn;
		f->build_id = *build_id;
		if (changed)
			reg->epoch++;
		goto done;
	}

	if ((ret = __os_calloc(dbenv, 1, sizeof(*f), &f)) != 0)
		goto done;

	memcpy(f->fileid, fileid, DB_FILE_ID_LEN);
	f->fence_lsn = fence_lsn;
	f->build_id = *build_id;

	if (hash_add(reg->files, f) != 0) {
		__os_free(dbenv, f);
		ret = ENOMEM;
	} else {
		changed = 1;
		reg->epoch++;
	}

done:
	Pthread_mutex_unlock(&reg->lk);

	/*
	 * Only when the fence actually moved.  This runs once per fence per
	 * scdone -- the reload reinstalls every fence it finds -- so
	 * invalidating unconditionally would flush the cache for every fenced
	 * file on every schema change, for no benefit: a fence that did not
	 * change cannot have invalidated anything cached under it.
	 */
	if (ret == 0 && changed)
		__sc_publication_fence_drop_cached_pages(dbenv, fileid);

	return (ret);
}

struct sc_fence_pending_copy_arg {
	DB_ENV *dbenv;
	hash_t *files;
	int ret;
};

static int
copy_pending_fence(void *obj, void *arg)
{
	SC_PUBLICATION_FENCE *f = obj, *copy, *existing;
	struct sc_fence_pending_copy_arg *a = arg;

	if (!IS_ZERO_LSN(f->fence_lsn))
		return (0);

	existing = hash_find(a->files, f->fileid);
	if (existing != NULL) {
		hash_del(a->files, existing);
		__os_free(a->dbenv, existing);
	}

	if ((a->ret = __os_malloc(a->dbenv, sizeof(*copy), &copy)) != 0)
		return (1);
	*copy = *f;
	if (hash_add(a->files, copy) != 0) {
		__os_free(a->dbenv, copy);
		a->ret = ENOMEM;
		return (1);
	}
	return (0);
}

struct sc_fence_changed_arg {
	hash_t *other;
	u_int8_t *fileids;
	size_t n;
	int removed;
};

static int
collect_changed_fence(void *obj, void *arg)
{
	SC_PUBLICATION_FENCE *f = obj;
	SC_PUBLICATION_FENCE *other;
	struct sc_fence_changed_arg *a = arg;

	if (IS_ZERO_LSN(f->fence_lsn))
		return (0);

	other = hash_find(a->other, f->fileid);
	if (a->removed ? other == NULL :
	    (other == NULL || IS_ZERO_LSN(other->fence_lsn) ||
	     log_compare(&f->fence_lsn, &other->fence_lsn) != 0)) {
		memcpy(a->fileids + a->n * DB_FILE_ID_LEN, f->fileid,
		    DB_FILE_ID_LEN);
		a->n++;
	}
	return (0);
}

static void
free_fence_hash(dbenv, files)
	DB_ENV *dbenv;
	hash_t *files;
{
	hash_for(files, &free_sc_publication_fence, dbenv);
	hash_clear(files);
	hash_free(files);
}

/*
 * __sc_publication_fence_reconcile --
 *	Atomically replace published fences with a validated durable snapshot.
 *	Pending zero-LSN entries belong to active builds and are preserved.
 *
 * PUBLIC: int __sc_publication_fence_reconcile __P((DB_ENV *,
 * PUBLIC:     const SC_PUBLICATION_FENCE_RECORD *, int));
 */
int
__sc_publication_fence_reconcile(dbenv, records, nrecords)
	DB_ENV *dbenv;
	const SC_PUBLICATION_FENCE_RECORD *records;
	int nrecords;
{
	SC_PUBLICATION_FENCE_REGISTRY *reg = dbenv->sc_publication_fences;
	SC_PUBLICATION_FENCE *f;
	hash_t *newfiles, *oldfiles;
	struct sc_fence_pending_copy_arg pending;
	struct sc_fence_changed_arg changed;
	u_int8_t *changed_fileids = NULL;
	size_t maxchanged, i;
	int ret = 0;

	if (reg == NULL || nrecords < 0 || (nrecords > 0 && records == NULL))
		return (EINVAL);

	newfiles = hash_init_o(offsetof(SC_PUBLICATION_FENCE, fileid),
	    DB_FILE_ID_LEN);
	if (newfiles == NULL)
		return (ENOMEM);

	for (i = 0; i < (size_t)nrecords; i++) {
		if (IS_ZERO_LSN(records[i].fence_lsn) ||
		    sc_build_id_is_zero(&records[i].build_id) ||
		    hash_find(newfiles, records[i].fileid) != NULL ||
		    (ret = __os_malloc(dbenv, sizeof(*f), &f)) != 0) {
			ret = ret != 0 ? ret : EINVAL;
			goto err;
		}
		memcpy(f->fileid, records[i].fileid, DB_FILE_ID_LEN);
		f->fence_lsn = records[i].fence_lsn;
		f->build_id = records[i].build_id;
		if (hash_add(newfiles, f) != 0) {
			__os_free(dbenv, f);
			ret = ENOMEM;
			goto err;
		}
	}

	Pthread_mutex_lock(&reg->lk);
	oldfiles = reg->files;
	pending.dbenv = dbenv;
	pending.files = newfiles;
	pending.ret = 0;
	hash_for(oldfiles, &copy_pending_fence, &pending);
	if (pending.ret != 0) {
		Pthread_mutex_unlock(&reg->lk);
		ret = pending.ret;
		goto err;
	}

	maxchanged = hash_get_num_entries(oldfiles) +
	    hash_get_num_entries(newfiles);
	if (maxchanged != 0 &&
	    __os_malloc(dbenv, maxchanged * DB_FILE_ID_LEN,
	    &changed_fileids) != 0) {
		Pthread_mutex_unlock(&reg->lk);
		ret = ENOMEM;
		goto err;
	}

	changed.other = oldfiles;
	changed.fileids = changed_fileids;
	changed.n = 0;
	changed.removed = 0;
	hash_for(newfiles, &collect_changed_fence, &changed);
	changed.other = newfiles;
	changed.removed = 1;
	hash_for(oldfiles, &collect_changed_fence, &changed);
	reg->files = newfiles;
	if (changed.n != 0)
		reg->epoch++;
	Pthread_mutex_unlock(&reg->lk);

	for (i = 0; i < changed.n; i++)
		__sc_publication_fence_drop_cached_pages(dbenv,
		    changed_fileids + i * DB_FILE_ID_LEN);
	if (changed_fileids != NULL)
		__os_free(dbenv, changed_fileids);
	free_fence_hash(dbenv, oldfiles);
	return (0);

err:
	free_fence_hash(dbenv, newfiles);
	return (ret);
}

struct sc_fence_build_arg {
	const sc_build_id_t *build_id;
	DB_LSN fence_lsn;
	SC_PUBLICATION_FENCE *found;
	u_int64_t nstamped;
	int fail_after;
	int failed;
};

static int
stamp_one_build_fence(void *obj, void *arg)
{
	SC_PUBLICATION_FENCE *f = obj;
	struct sc_fence_build_arg *a = arg;

	if (!sc_build_id_equal(&f->build_id, a->build_id))
		return (0);
	if (a->nstamped == (u_int64_t)a->fail_after) {
		a->failed = 1;
		return (1);
	}

	f->fence_lsn = a->fence_lsn;
	a->nstamped++;
	return (0);
}

/*
 * __sc_publication_fence_publish --
 *	Stamp every pending file of a build with the generation's fence.
 *
 *	fence_lsn must be ordered after every modification that forms the
 *	initial published image and before the publication commits.  The
 *	schema change's own scdone record satisfies both: it is written after
 *	the file versions are switched and inside the publication transaction.
 *
 * PUBLIC: int __sc_publication_fence_publish __P((DB_ENV *,
 * PUBLIC:     const sc_build_id_t *,
 * PUBLIC:     DB_LSN, int));
 */
int
__sc_publication_fence_publish(dbenv, build_id, fence_lsn, expected_count)
	DB_ENV *dbenv;
	const sc_build_id_t *build_id;
	DB_LSN fence_lsn;
	int expected_count;
{
	SC_PUBLICATION_FENCE_REGISTRY *reg = dbenv->sc_publication_fences;
	struct sc_fence_build_arg arg;
	u_int8_t *fileids = NULL;
	int nfiles = 0, i;

	if (reg == NULL || sc_build_id_is_zero(build_id) || IS_ZERO_LSN(fence_lsn) ||
	    expected_count <= 0)
		return (EINVAL);

	if (__sc_publication_fence_list_build(dbenv, build_id, NULL, 0,
	    &nfiles) != 0 || nfiles != expected_count)
		return (EINVAL);

	if (__os_malloc(dbenv, (size_t)nfiles * DB_FILE_ID_LEN,
	    &fileids) != 0)
		return (ENOMEM);

	if (__sc_publication_fence_list_build(dbenv, build_id, fileids,
	    nfiles, &nfiles) != 0 || nfiles != expected_count) {
		__os_free(dbenv, fileids);
		return (EINVAL);
	}

	arg.build_id = build_id;
	arg.fence_lsn = fence_lsn;
	arg.nstamped = 0;
	arg.fail_after = gbl_sc_fence_publish_fail_after;
	arg.failed = 0;

	Pthread_mutex_lock(&reg->lk);
	hash_for(reg->files, &stamp_one_build_fence, &arg);
	if (!arg.failed && arg.nstamped == (u_int64_t)expected_count)
		reg->epoch++;
	Pthread_mutex_unlock(&reg->lk);
	if (arg.failed || arg.nstamped != (u_int64_t)expected_count) {
		if (arg.failed)
			logmsg(LOGMSG_ERROR,
			    "%s: injected failure after %"PRIu64
			    " in-memory fence installs\n", __func__, arg.nstamped);
		__os_free(dbenv, fileids);
		return (EINVAL);
	}

	/*
	 * These files have just gained a fence, so anything reconstructed for
	 * them before now was reconstructed without one.  Collect them and drop
	 * their cached pages, outside the registry lock.
	 */
	for (i = 0; i < nfiles; i++)
		__sc_publication_fence_drop_cached_pages(dbenv,
		    fileids + (size_t)i * DB_FILE_ID_LEN);
	__os_free(dbenv, fileids);

	return (0);
}

struct sc_fence_list_arg {
	const sc_build_id_t *build_id;
	u_int8_t *out;
	int max;
	int n;
};

static int
collect_one_build_fence(void *obj, void *arg)
{
	SC_PUBLICATION_FENCE *f = obj;
	struct sc_fence_list_arg *a = arg;

	if (!sc_build_id_equal(&f->build_id, a->build_id))
		return (0);

	if (a->out == NULL) {
		a->n++;
		return (0);
	}

	if (a->n >= a->max) {
		a->n = -1;	/* caller's buffer is too small; say so */
		return (1);
	}

	memcpy(a->out + (size_t)a->n * DB_FILE_ID_LEN, f->fileid,
	    DB_FILE_ID_LEN);
	a->n++;
	return (0);
}

/*
 * __sc_publication_fence_list_build --
 *	Copy out the file ids belonging to a build, so its caller can make them
 *	durable.  Returns -1 if they do not fit in max entries.
 *
 * PUBLIC: int __sc_publication_fence_list_build __P((DB_ENV *,
 * PUBLIC:     const sc_build_id_t *,
 * PUBLIC:	   u_int8_t *, int, int *));
 */
int
__sc_publication_fence_list_build(dbenv, build_id, out, max, nout)
	DB_ENV *dbenv;
	const sc_build_id_t *build_id;
	u_int8_t *out;
	int max;
	int *nout;
{
	SC_PUBLICATION_FENCE_REGISTRY *reg = dbenv->sc_publication_fences;
	struct sc_fence_list_arg arg;

	*nout = 0;

	if (reg == NULL || sc_build_id_is_zero(build_id) || max < 0 ||
	    (out == NULL && max != 0))
		return (EINVAL);

	arg.build_id = build_id;
	arg.out = out;
	arg.max = out == NULL ? INT_MAX : max;
	arg.n = 0;

	Pthread_mutex_lock(&reg->lk);
	hash_for(reg->files, &collect_one_build_fence, &arg);
	Pthread_mutex_unlock(&reg->lk);

	if (arg.n < 0)
		return (ENOMEM);

	*nout = arg.n;
	return (0);
}

static int
find_one_build_fence(void *obj, void *arg)
{
	SC_PUBLICATION_FENCE *f = obj;
	struct sc_fence_build_arg *a = arg;

	if (!sc_build_id_equal(&f->build_id, a->build_id))
		return (0);

	a->found = f;
	return (1); /* stop the walk */
}

/*
 * __sc_publication_fence_discard_build --
 *	Drop every pending file of a build that will not publish.  Idempotent,
 *	so terminal schema-change paths can call it unconditionally.
 *
 * PUBLIC: int __sc_publication_fence_discard_build __P((DB_ENV *,
 * PUBLIC:     const sc_build_id_t *));
 */
int
__sc_publication_fence_discard_build(dbenv, build_id)
	DB_ENV *dbenv;
	const sc_build_id_t *build_id;
{
	SC_PUBLICATION_FENCE_REGISTRY *reg = dbenv->sc_publication_fences;
	struct sc_fence_build_arg arg;

	if (reg == NULL || sc_build_id_is_zero(build_id))
		return (0);

	Pthread_mutex_lock(&reg->lk);

	/* Allocation-free, for the reason given in the sibling registry. */
	arg.build_id = build_id;
	for (;;) {
		arg.found = NULL;
		hash_for(reg->files, &find_one_build_fence, &arg);

		if (arg.found == NULL)
			break;

		hash_del(reg->files, arg.found);
		__os_free(dbenv, arg.found);
	}

	Pthread_mutex_unlock(&reg->lk);
	return (0);
}

/*
 * __sc_publication_fence_get --
 *	Fetch a published file's fence.  Returns DB_NOTFOUND when the file is
 *	not a rebuilt schema-change file, or is one that has not published.
 *
 * PUBLIC: int __sc_publication_fence_get __P((DB_ENV *, const u_int8_t *,
 * PUBLIC:	   DB_LSN *));
 */
int
__sc_publication_fence_get(dbenv, fileid, fence_lsn)
	DB_ENV *dbenv;
	const u_int8_t *fileid;
	DB_LSN *fence_lsn;
{
	SC_PUBLICATION_FENCE_REGISTRY *reg = dbenv->sc_publication_fences;
	SC_PUBLICATION_FENCE *f;
	int ret = DB_NOTFOUND;

	if (reg == NULL || fileid == NULL)
		return (DB_NOTFOUND);

	Pthread_mutex_lock(&reg->lk);

	f = hash_find(reg->files, fileid);
	if (f != NULL && !IS_ZERO_LSN(f->fence_lsn)) {
		*fence_lsn = f->fence_lsn;
		reg->lookup_hits++;
		ret = 0;
	} else {
		reg->lookup_misses++;
	}

	Pthread_mutex_unlock(&reg->lk);
	return (ret);
}

/* PUBLIC: u_int64_t __sc_publication_fence_epoch __P((DB_ENV *)); */
u_int64_t
__sc_publication_fence_epoch(dbenv)
	DB_ENV *dbenv;
{
	SC_PUBLICATION_FENCE_REGISTRY *reg = dbenv->sc_publication_fences;
	u_int64_t epoch = 0;

	if (reg == NULL)
		return (0);
	Pthread_mutex_lock(&reg->lk);
	epoch = reg->epoch;
	Pthread_mutex_unlock(&reg->lk);
	return (epoch);
}

/* PUBLIC: void __sc_publication_fence_note_epoch_retry __P((DB_ENV *)); */
void
__sc_publication_fence_note_epoch_retry(dbenv)
	DB_ENV *dbenv;
{
	SC_PUBLICATION_FENCE_REGISTRY *reg = dbenv->sc_publication_fences;

	if (reg == NULL)
		return;
	Pthread_mutex_lock(&reg->lk);
	reg->epoch_retries++;
	Pthread_mutex_unlock(&reg->lk);
}

/*
 * __sc_publication_fence_note --
 *	Count an outcome of the fence rule.  'stopped' says reconstruction
 *	stopped at the fence; otherwise the snapshot's target predates the
 *	generation's publication, which the rule must not treat as covered.
 *
 * PUBLIC: void __sc_publication_fence_note __P((DB_ENV *, int));
 */
void
__sc_publication_fence_note(dbenv, stopped)
	DB_ENV *dbenv;
	int stopped;
{
	SC_PUBLICATION_FENCE_REGISTRY *reg = dbenv->sc_publication_fences;

	if (reg == NULL)
		return;

	Pthread_mutex_lock(&reg->lk);
	if (stopped)
		reg->stops++;
	else
		reg->target_before_publication++;
	Pthread_mutex_unlock(&reg->lk);
}

/*
 * __sc_publication_fence_stats --
 *
 * PUBLIC: void __sc_publication_fence_stats __P((DB_ENV *, u_int64_t *,
 * PUBLIC:	   u_int64_t *, u_int64_t *, u_int64_t *, u_int64_t *,
 * PUBLIC:	   u_int64_t *));
 */
void
__sc_publication_fence_stats(dbenv, entries, stops, hits, misses, early, dropped)
	DB_ENV *dbenv;
	u_int64_t *entries;
	u_int64_t *stops;
	u_int64_t *hits;
	u_int64_t *misses;
	u_int64_t *early;
	u_int64_t *dropped;
{
	SC_PUBLICATION_FENCE_REGISTRY *reg = dbenv->sc_publication_fences;

	if (reg == NULL) {
		*entries = *stops = *hits = *misses = *early = *dropped = 0;
		return;
	}

	Pthread_mutex_lock(&reg->lk);
	*entries = (u_int64_t)hash_get_num_entries(reg->files);
	*stops = reg->stops;
	*hits = reg->lookup_hits;
	*misses = reg->lookup_misses;
	*early = reg->target_before_publication;
	*dropped = reg->cached_pages_dropped;
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
 * PUBLIC: void __txn_set_sc_build __P((DB_TXN *, const sc_build_id_t *));
 */
void
__txn_set_sc_build(txnp, build_id)
	DB_TXN *txnp;
	const sc_build_id_t *build_id;
{
	if (txnp == NULL || sc_build_id_is_zero(build_id))
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

	txnp->sc_build_id = *build_id;
	txnp->sc_skip_commit_map = 0;
	txnp->sc_unsafe_public_write = 0;

	(void)ATOMIC_ADD64(sc_direct_copy_txns_marked, 1);
}

/*
 * __txn_note_sc_file_write_int --
 *	Slow path of the physical-write check; see __txn_note_sc_file_write().
 *
 *	A private-file match qualifies the transaction; any public user-file write
 *	disqualifies it.  Internal metadata writes do neither.
 *
 * PUBLIC: void __txn_note_sc_file_write_int __P((DB_TXN *, DB *));
 */
void
__txn_note_sc_file_write_int(txnp, dbp)
	DB_TXN *txnp;
	DB *dbp;
{
	sc_build_id_t file_build_id;

	if (__sc_private_file_lookup(txnp->mgrp->dbenv, dbp->fileid,
	    &file_build_id) != 0) {
		if (dbp->sc_is_user_file)
			txnp->sc_unsafe_public_write = 1;
		return;
	}

	if (sc_build_id_equal(&file_build_id, &txnp->sc_build_id)) {
		if (!txnp->sc_skip_commit_map)
			(void)ATOMIC_ADD64(sc_direct_copy_txns_matched, 1);
		txnp->sc_skip_commit_map = 1;
	} else if (dbp->sc_is_user_file)
		txnp->sc_unsafe_public_write = 1;
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
