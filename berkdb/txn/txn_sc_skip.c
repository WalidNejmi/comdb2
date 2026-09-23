/*
 * Classify direct schema-change converter transactions.
 *
 * A rebuilding schema change registers each private replacement file against
 * its exact build identity.  The base converter marks its transactions with
 * the same identity.  Physical writes then establish two independent facts:
 * whether the transaction wrote one of its own private files, and whether it
 * also wrote an unsafe public user file.
 *
 * This module only records classification state.  It does not change commit
 * records or commit-map behavior.
 */

#include "db_config.h"

#ifndef lint
static const char revid[] = "$Id: txn_sc_skip.c,v 1.0 2026/09/15 00:00:00 comdb2 Exp $";
#endif

#ifndef NO_SYSTEM_INCLUDES
#include <sys/types.h>
#include <limits.h>
#include <string.h>
#endif

#include "db_int.h"
#include "dbinc/db_shash.h"
#include "dbinc/txn.h"
#include "logmsg.h"
#include "comdb2_atomic.h"

extern int __mempv_cache_invalidate_file __P((DB_ENV *, u_int8_t *));

static u_int64_t sc_direct_copy_txns_marked = 0;
static u_int64_t sc_direct_copy_txns_matched = 0;
static u_int64_t sc_direct_copy_txns_unsafe = 0;
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

void
__sc_commit_flags_note(which, commit_flags)
	int which;
	u_int32_t commit_flags;
{
	int would_skip = !__txn_commit_map_should_add(commit_flags);

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

int
__sc_private_file_register(dbenv, fileid, build_id)
	DB_ENV *dbenv;
	const u_int8_t *fileid;
	const sc_build_id_t *build_id;
{
	SC_PRIVATE_FILE_REGISTRY *reg = dbenv->sc_private_files;
	SC_PRIVATE_FILE *file;
	int ret = 0;

	if (reg == NULL || fileid == NULL || sc_build_id_is_zero(build_id))
		return (EINVAL);

	Pthread_mutex_lock(&reg->lk);
	file = hash_find(reg->files, fileid);
	if (file != NULL) {
		if (!sc_build_id_equal(&file->build_id, build_id)) {
			logmsg(LOGMSG_ERROR,
			    "%s: file already belongs to another schema-change build\n",
			    __func__);
			ret = EEXIST;
		}
		goto done;
	}

	if ((ret = __os_calloc(dbenv, 1, sizeof(SC_PRIVATE_FILE), &file)) != 0) {
		ret = ENOMEM;
		goto done;
	}

	memcpy(file->fileid, fileid, DB_FILE_ID_LEN);
	file->build_id = *build_id;
	hash_add(reg->files, file);

done:
	Pthread_mutex_unlock(&reg->lk);
	return (ret);
}

int
__sc_private_file_lookup(dbenv, fileid, build_id_out)
	DB_ENV *dbenv;
	const u_int8_t *fileid;
	sc_build_id_t *build_id_out;
{
	SC_PRIVATE_FILE_REGISTRY *reg = dbenv->sc_private_files;
	SC_PRIVATE_FILE *file;
	int ret = DB_NOTFOUND;

	if (reg == NULL || fileid == NULL)
		return (DB_NOTFOUND);

	Pthread_mutex_lock(&reg->lk);
	file = hash_find(reg->files, fileid);
	if (file != NULL) {
		*build_id_out = file->build_id;
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
	SC_PRIVATE_FILE *file = obj;
	struct sc_private_find_arg *find = arg;

	if (!sc_build_id_equal(&file->build_id, find->build_id))
		return (0);

	find->found = file;
	return (1);
}

int
__sc_private_file_unregister_build(dbenv, build_id)
	DB_ENV *dbenv;
	const sc_build_id_t *build_id;
{
	SC_PRIVATE_FILE_REGISTRY *reg = dbenv->sc_private_files;
	struct sc_private_find_arg find;

	if (reg == NULL || sc_build_id_is_zero(build_id))
		return (0);

	Pthread_mutex_lock(&reg->lk);
	find.build_id = build_id;
	for (;;) {
		find.found = NULL;
		hash_for(reg->files, &find_one_build_file, &find);
		if (find.found == NULL)
			break;
		hash_del(reg->files, find.found);
		__os_free(dbenv, find.found);
	}
	Pthread_mutex_unlock(&reg->lk);
	return (0);
}

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

int
__sc_publication_fence_registry_init(dbenv)
	DB_ENV *dbenv;
{
	SC_PUBLICATION_FENCE_REGISTRY *reg;
	int ret;

	if ((ret = __os_calloc(dbenv, 1, sizeof(*reg), &reg)) != 0)
		return (ret);

	reg->files = hash_init_o(offsetof(SC_PUBLICATION_FENCE, fileid),
	    DB_FILE_ID_LEN);
	if (reg->files == NULL) {
		__os_free(dbenv, reg);
		return (ENOMEM);
	}

	Pthread_mutex_init(&reg->lk, NULL);
	dbenv->sc_publication_fences = reg;
	return (0);
}

static int
free_sc_publication_fence(void *obj, void *arg)
{
	__os_free((DB_ENV *)arg, obj);
	return (0);
}

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

static void
__sc_publication_fence_drop_cached_pages(dbenv, fileid)
	DB_ENV *dbenv;
	const u_int8_t *fileid;
{
	SC_PUBLICATION_FENCE_REGISTRY *reg = dbenv->sc_publication_fences;
	int ndropped = __mempv_cache_invalidate_file(dbenv, (u_int8_t *)fileid);

	if (ndropped <= 0)
		return;
	if (reg != NULL) {
		Pthread_mutex_lock(&reg->lk);
		reg->cached_pages_dropped += (u_int64_t)ndropped;
		Pthread_mutex_unlock(&reg->lk);
	}
}

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
		ZERO_LSN(f->fence_lsn);
		f->publication_utxnid = 0;
		f->build_id = *build_id;
		goto done;
	}

	if ((ret = __os_calloc(dbenv, 1, sizeof(*f), &f)) != 0)
		goto done;

	memcpy(f->fileid, fileid, DB_FILE_ID_LEN);
	f->build_id = *build_id;
	if (hash_add(reg->files, f) != 0) {
		__os_free(dbenv, f);
		ret = ENOMEM;
	}

done:
	Pthread_mutex_unlock(&reg->lk);
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
	struct sc_fence_pending_copy_arg *copy_arg = arg;

	if (!IS_ZERO_LSN(f->fence_lsn))
		return (0);

	existing = hash_find(copy_arg->files, f->fileid);
	if (existing != NULL) {
		hash_del(copy_arg->files, existing);
		__os_free(copy_arg->dbenv, existing);
	}

	if ((copy_arg->ret = __os_malloc(copy_arg->dbenv, sizeof(*copy),
	    &copy)) != 0)
		return (1);
	*copy = *f;
	if (hash_add(copy_arg->files, copy) != 0) {
		__os_free(copy_arg->dbenv, copy);
		copy_arg->ret = ENOMEM;
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
	struct sc_fence_changed_arg *changed = arg;

	if (IS_ZERO_LSN(f->fence_lsn))
		return (0);
	other = hash_find(changed->other, f->fileid);
	if (changed->removed ? other == NULL :
	    (other == NULL || IS_ZERO_LSN(other->fence_lsn) ||
	    log_compare(&f->fence_lsn, &other->fence_lsn) != 0 ||
	    f->publication_utxnid != other->publication_utxnid)) {
		memcpy(changed->fileids + changed->n * DB_FILE_ID_LEN, f->fileid,
		    DB_FILE_ID_LEN);
		changed->n++;
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
		f->publication_utxnid = records[i].publication_utxnid;
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
	if (maxchanged != 0 && __os_malloc(dbenv,
	    maxchanged * DB_FILE_ID_LEN, &changed_fileids) != 0) {
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
	reg->readiness = SC_FENCE_READY;
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

int
__sc_publication_fence_ready(dbenv)
	DB_ENV *dbenv;
{
	SC_PUBLICATION_FENCE_REGISTRY *reg = dbenv->sc_publication_fences;
	int ready;

	if (reg == NULL)
		return (0);
	Pthread_mutex_lock(&reg->lk);
	ready = reg->readiness == SC_FENCE_READY;
	Pthread_mutex_unlock(&reg->lk);
	return (ready);
}

void
__sc_publication_fence_set_failed(dbenv)
	DB_ENV *dbenv;
{
	SC_PUBLICATION_FENCE_REGISTRY *reg = dbenv->sc_publication_fences;

	if (reg == NULL)
		return;
	Pthread_mutex_lock(&reg->lk);
	reg->readiness = SC_FENCE_FAILED;
	reg->epoch++;
	Pthread_mutex_unlock(&reg->lk);
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
	struct sc_fence_list_arg *list = arg;

	if (!sc_build_id_equal(&f->build_id, list->build_id))
		return (0);
	if (list->out == NULL) {
		list->n++;
		return (0);
	}
	if (list->n >= list->max) {
		list->n = -1;
		return (1);
	}
	memcpy(list->out + (size_t)list->n * DB_FILE_ID_LEN, f->fileid,
	    DB_FILE_ID_LEN);
	list->n++;
	return (0);
}

int
__sc_publication_fence_list_build(dbenv, build_id, out, max, nout)
	DB_ENV *dbenv;
	const sc_build_id_t *build_id;
	u_int8_t *out;
	int max;
	int *nout;
{
	SC_PUBLICATION_FENCE_REGISTRY *reg = dbenv->sc_publication_fences;
	struct sc_fence_list_arg list;

	if (nout == NULL)
		return (EINVAL);
	*nout = 0;
	if (reg == NULL || sc_build_id_is_zero(build_id) || max < 0 ||
	    (out == NULL && max != 0))
		return (EINVAL);

	list.build_id = build_id;
	list.out = out;
	list.max = out == NULL ? INT_MAX : max;
	list.n = 0;
	Pthread_mutex_lock(&reg->lk);
	hash_for(reg->files, &collect_one_build_fence, &list);
	Pthread_mutex_unlock(&reg->lk);
	if (list.n < 0)
		return (ENOMEM);

	*nout = list.n;
	return (0);
}

struct sc_fence_build_arg {
	const sc_build_id_t *build_id;
	DB_LSN fence_lsn;
	u_int64_t publication_utxnid;
	SC_PUBLICATION_FENCE *found;
	u_int64_t nstamped;
	int fail_after;
	int failed;
};

static int
stamp_one_build_fence(void *obj, void *arg)
{
	SC_PUBLICATION_FENCE *f = obj;
	struct sc_fence_build_arg *build = arg;

	if (!sc_build_id_equal(&f->build_id, build->build_id))
		return (0);
	if (build->nstamped == (u_int64_t)build->fail_after) {
		build->failed = 1;
		return (1);
	}
	f->fence_lsn = build->fence_lsn;
	f->publication_utxnid = build->publication_utxnid;
	build->nstamped++;
	return (0);
}

extern int gbl_sc_fence_publish_fail_after;

int
__sc_publication_fence_publish(dbenv, build_id, fence_lsn,
    publication_utxnid, expected_count)
	DB_ENV *dbenv;
	const sc_build_id_t *build_id;
	DB_LSN fence_lsn;
	u_int64_t publication_utxnid;
	int expected_count;
{
	SC_PUBLICATION_FENCE_REGISTRY *reg = dbenv->sc_publication_fences;
	struct sc_fence_build_arg build;
	u_int8_t *fileids = NULL;
	int nfiles, i;

	if (reg == NULL || sc_build_id_is_zero(build_id) ||
	    IS_ZERO_LSN(fence_lsn) || publication_utxnid == 0 ||
	    expected_count <= 0)
		return (EINVAL);
	if (__sc_publication_fence_list_build(dbenv, build_id, NULL, 0,
	    &nfiles) != 0 || nfiles != expected_count)
		return (EINVAL);
	if (__os_malloc(dbenv, (size_t)nfiles * DB_FILE_ID_LEN, &fileids) != 0)
		return (ENOMEM);
	if (__sc_publication_fence_list_build(dbenv, build_id, fileids,
	    nfiles, &nfiles) != 0 || nfiles != expected_count) {
		__os_free(dbenv, fileids);
		return (EINVAL);
	}

	build.build_id = build_id;
	build.fence_lsn = fence_lsn;
	build.publication_utxnid = publication_utxnid;
	build.nstamped = 0;
	build.fail_after = gbl_sc_fence_publish_fail_after;
	build.failed = 0;
	Pthread_mutex_lock(&reg->lk);
	hash_for(reg->files, &stamp_one_build_fence, &build);
	if (!build.failed && build.nstamped == (u_int64_t)expected_count)
		reg->epoch++;
	Pthread_mutex_unlock(&reg->lk);
	if (build.failed || build.nstamped != (u_int64_t)expected_count) {
		__os_free(dbenv, fileids);
		return (EINVAL);
	}
	for (i = 0; i < nfiles; i++)
		__sc_publication_fence_drop_cached_pages(dbenv,
		    fileids + (size_t)i * DB_FILE_ID_LEN);
	__os_free(dbenv, fileids);
	return (0);
}

static int
find_one_build_fence(void *obj, void *arg)
{
	SC_PUBLICATION_FENCE *f = obj;
	struct sc_fence_build_arg *build = arg;

	if (!sc_build_id_equal(&f->build_id, build->build_id))
		return (0);
	build->found = f;
	return (1);
}

int
__sc_publication_fence_discard_build(dbenv, build_id)
	DB_ENV *dbenv;
	const sc_build_id_t *build_id;
{
	SC_PUBLICATION_FENCE_REGISTRY *reg = dbenv->sc_publication_fences;
	struct sc_fence_build_arg build;

	if (reg == NULL || sc_build_id_is_zero(build_id))
		return (0);

	build.build_id = build_id;
	Pthread_mutex_lock(&reg->lk);
	for (;;) {
		build.found = NULL;
		hash_for(reg->files, &find_one_build_fence, &build);
		if (build.found == NULL)
			break;
		hash_del(reg->files, build.found);
		__os_free(dbenv, build.found);
	}
	Pthread_mutex_unlock(&reg->lk);
	return (0);
}

int
__sc_publication_fence_get(dbenv, fileid, fence_lsn, publication_utxnid)
	DB_ENV *dbenv;
	const u_int8_t *fileid;
	DB_LSN *fence_lsn;
	u_int64_t *publication_utxnid;
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
		*publication_utxnid = f->publication_utxnid;
		reg->lookup_hits++;
		ret = 0;
	} else
		reg->lookup_misses++;
	Pthread_mutex_unlock(&reg->lk);
	return (ret);
}

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

void
__sc_publication_fence_test_bump_epoch(dbenv)
	DB_ENV *dbenv;
{
	SC_PUBLICATION_FENCE_REGISTRY *reg = dbenv->sc_publication_fences;

	if (reg == NULL)
		return;
	Pthread_mutex_lock(&reg->lk);
	reg->epoch++;
	Pthread_mutex_unlock(&reg->lk);
}

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

	if (reg != NULL) {
		Pthread_mutex_lock(&reg->lk);
		*entries = (u_int64_t)hash_get_num_entries(reg->files);
		*stops = reg->stops;
		*hits = reg->lookup_hits;
		*misses = reg->lookup_misses;
		*early = reg->target_before_publication;
		*dropped = reg->cached_pages_dropped;
		Pthread_mutex_unlock(&reg->lk);
	} else
		*entries = *stops = *hits = *misses = *early = *dropped = 0;
}

void
__txn_set_sc_build(txnp, build_id)
	DB_TXN *txnp;
	const sc_build_id_t *build_id;
{
	if (txnp == NULL || sc_build_id_is_zero(build_id))
		return;

	if (txnp->parent != NULL) {
		static int warned = 0;

		if (!warned) {
			warned = 1;
			logmsg(LOGMSG_WARN,
			    "%s: converter transaction has a parent; "
			    "classification will not apply\n", __func__);
		}
		return;
	}

	txnp->sc_build_id = *build_id;
	txnp->sc_skip_commit_map = 0;
	txnp->sc_unsafe_public_write = 0;
	(void)ATOMIC_ADD64(sc_direct_copy_txns_marked, 1);
}

void
__txn_note_sc_file_write_int(txnp, dbp)
	DB_TXN *txnp;
	DB *dbp;
{
	sc_build_id_t file_build_id;

	if (__sc_private_file_lookup(txnp->mgrp->dbenv, dbp->fileid,
	    &file_build_id) != 0) {
		if (dbp->sc_is_user_file) {
			txnp->sc_unsafe_public_write = 1;
			(void)ATOMIC_ADD64(sc_direct_copy_txns_unsafe, 1);
		}
		return;
	}

	if (sc_build_id_equal(&file_build_id, &txnp->sc_build_id)) {
		if (!txnp->sc_skip_commit_map)
			(void)ATOMIC_ADD64(sc_direct_copy_txns_matched, 1);
		txnp->sc_skip_commit_map = 1;
	} else if (dbp->sc_is_user_file) {
		txnp->sc_unsafe_public_write = 1;
		(void)ATOMIC_ADD64(sc_direct_copy_txns_unsafe, 1);
	}
}

void
__sc_direct_copy_stats(marked, matched, unsafe)
	u_int64_t *marked;
	u_int64_t *matched;
	u_int64_t *unsafe;
{
	*marked = ATOMIC_LOAD64(sc_direct_copy_txns_marked);
	*matched = ATOMIC_LOAD64(sc_direct_copy_txns_matched);
	*unsafe = ATOMIC_LOAD64(sc_direct_copy_txns_unsafe);
}