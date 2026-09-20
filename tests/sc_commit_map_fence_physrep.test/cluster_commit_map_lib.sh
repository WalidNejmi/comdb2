# Shared helpers for the sc_commit_map_cluster_* characterization tests.
#
# These tests all measure the per-node commit-LSN map ("clminfo") while a
# rebuilding schema change runs on a real cluster, to characterize the
# CURRENT durability gap:
#
#   * The MASTER omits the converter transactions' commit-map entries
#     (berkdb/txn/txn.c:1558-1579, gated on txnp->sc_skip_commit_map, an
#     in-memory flag).  This is implemented on this branch.
#
#   * A REPLICA applying the master's WAL adds those same commit records to
#     its OWN commit-LSN map unconditionally -- the apply path only checks
#     __txn_commit_map_enabled() (berkdb/rep/rep_record.c:5364 and :6285),
#     it never consults sc_skip_commit_map, because sc_skip_commit_map is
#     never written to the WAL.  So the replica regrows one entry per copied
#     row while the master stays flat.  That divergence IS the gap.
#
#   * The same is true of crash recovery on a freshly promoted master
#     (berkdb/txn/txn_rec.c:527-537, berkdb/env/env_recover.c:2178-2221):
#     the backward/forward rebuild re-adds the converter entries because the
#     WAL carries no omission marker.
#
# WHEN THE DURABLE-FLAG FIX LANDS these assertions must be INVERTED: the
# replica / promoted master must then omit the same entries as the master, so
# DIVERGENCE_MIN drops to 0 and a convergence bound is added.  Until then the
# tests pin the gap so the fix visibly changes their outcome.  This is the same
# philosophy as sc_commit_map_growth.test: they characterize what the code does
# today, not what is desirable.
#
# Empirically-derived bounds are marked TO-CONFIRM: they are written from the
# single-node characterization plus the code paths above and must be confirmed
# (and tightened) on the first real multi-node run, exactly as the growth
# test's MAX_MAP_DELTA was.  Every test prints an SC_COMMIT_MAP_CLUSTER_* line
# with the raw numbers for that purpose.

# --- per-node commit-map introspection -------------------------------------

# Emit the raw `bdb clminfo` block for a given host.
node_clminfo() {
	cdb2sql ${CDB2_OPTIONS} --host "$1" "$DBNAME" \
		"exec procedure sys.cmd.send('bdb clminfo')"
}

# Parse a single counter out of a clminfo block passed on stdin/arg.
clminfo_current() { grep -oP '(?<=Current entries: )[0-9]+' <<<"$1"; }
clminfo_added()   { grep -oP '(?<=Entries added: )[0-9]+' <<<"$1"; }
clminfo_skipped() { grep -oP '(?<=SC commit-map entries skipped: )[0-9]+' <<<"$1"; }

# Convenience: fetch just the "Current entries" number for a host.
node_current() { clminfo_current "$(node_clminfo "$1")"; }
node_skipped() { clminfo_skipped "$(node_clminfo "$1")"; }

# Send a message-trap command to one host.
send_node() {
	cdb2sql ${CDB2_OPTIONS} --host "$1" "$DBNAME" \
		"exec procedure sys.cmd.send('$2')" > /dev/null 2>&1
}

# Run a SQL statement against one host (tab-separated, single value friendly).
sql_node() {
	cdb2sql --tabs ${CDB2_OPTIONS} --host "$1" "$DBNAME" "$2"
}

fail() { echo "FAIL: $*"; exit 1; }

# --- cluster topology ------------------------------------------------------

# Print the master host.
cluster_master() { getmaster; }

# Print every replica host (all cluster nodes except the master), space list.
cluster_replicas() {
	local m
	m=$(getmaster)
	local n
	for n in $(getclusternodes); do
		[[ "$n" != "$m" ]] && echo "$n"
	done
}

# Skip cleanly unless we have a real cluster of at least two nodes.  Both the
# environment CLUSTER list and the live comdb2_cluster view must agree, so the
# test never runs half-configured.
require_cluster() {
	[ -z "${CLUSTER}" ] && {
		echo "Test only suitable for a clustered setup"; exit 0; }
	local nenv
	nenv=$(echo ${CLUSTER} | wc -w)
	if (( nenv < 2 )); then
		echo "Test needs >= 2 nodes (CLUSTER has $nenv); skipping"
		exit 0
	fi
	local nlive
	nlive=$(getclusternodes | grep -c .)
	if (( nlive < 2 )); then
		echo "Test needs >= 2 live nodes (comdb2_cluster has $nlive); skipping"
		exit 0
	fi
}

# --- settling / measurement ------------------------------------------------

# Hold log deletion off on every node so retained commit-map entries are not
# pruned out from under the measurement.
cluster_log_delete_off() {
	local n
	for n in $(getclusternodes); do send_node "$n" "sync log-delete off"; done
}
cluster_log_delete_on() {
	local n
	for n in $(getclusternodes); do send_node "$n" "sync log-delete on"; done
}

# Wait until a host's "Current entries" count stops moving (3 equal reads,
# ~1s apart), then echo the settled value.  Used after a schema change or a
# restart so master and replica are compared at rest, not mid-apply.
settle_current() {
	local host=$1 label=$2
	local prev="" cur same=0 i
	for (( i=0; i<180; i++ )); do
		cur=$(node_current "$host")
		if [[ -n "$cur" && "$cur" == "$prev" ]]; then
			(( same++ ))
			if (( same >= 3 )); then echo "$cur"; return 0; fi
		else
			same=0
		fi
		prev=$cur
		sleep 1
	done
	echo "$cur"
	echo "WARN ($label): commit map on $host did not settle; using $cur" >&2
	return 0
}

# Create table t and bulk-insert N rows in a single transaction on the master
# (so setup adds one commit-map entry, not N).
seed_table() {
	local host=$1 n=$2
	sql_node "$host" "create table t(i int)" > /dev/null
	sql_node "$host" "insert into t select value from generate_series(1, $n)" \
		> /dev/null
	local rc
	rc=$(sql_node "$host" "select count(*) from t")
	if [[ "$rc" != "$n" ]]; then
		fail "expected $n rows after seed, got $rc"
	fi
}
