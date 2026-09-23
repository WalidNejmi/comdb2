# Shared helpers for publication-fence cluster tests.

node_clminfo() {
	cdb2sql --allow-incoherent ${CDB2_OPTIONS} --host "$1" "${dbname:-$DBNAME}" \
		"exec procedure sys.cmd.send('bdb clminfo')" 2>/dev/null
}

node_fences() {
	node_clminfo "$1" | grep -oP '(?<=SC publication fences: )[0-9]+'
}

send_node() {
	cdb2sql ${CDB2_OPTIONS} --host "$1" "${dbname:-$DBNAME}" \
		"exec procedure sys.cmd.send('$2')" >/dev/null 2>&1
}

sql_node() {
	cdb2sql --tabs ${CDB2_OPTIONS} --host "$1" "${dbname:-$DBNAME}" "$2"
}

fail() {
	echo "FAIL: $*"
	exit 1
}

cluster_master() {
	getmaster
}

cluster_replicas() {
	local master node
	master=$(cluster_master)
	for node in $(getclusternodes); do
		[[ "$node" != "$master" ]] && echo "$node"
	done
}

require_cluster() {
	if [[ -z "${CLUSTER}" ]]; then
		echo "Test only suitable for a clustered setup"
		exit 0
	fi
	local configured live
	configured=$(wc -w <<<"${CLUSTER}")
	live=$(getclusternodes | grep -c .)
	if (( configured < 2 || live < 2 )); then
		echo "Test needs at least two live cluster nodes; skipping"
		exit 0
	fi
}

cluster_log_delete_off() {
	local node
	for node in $(getclusternodes); do
		send_node "$node" "sync log-delete off"
	done
}

cluster_log_delete_on() {
	local node
	for node in $(getclusternodes); do
		send_node "$node" "sync log-delete on"
	done
}

seed_table() {
	local host=$1 rows=$2
	sql_node "$host" "create table t(i int)" >/dev/null
	sql_node "$host" \
		"insert into t select value from generate_series(1, $rows)" >/dev/null
	[[ "$(sql_node "$host" 'select count(*) from t')" == "$rows" ]] || \
		fail "seed row count is not $rows"
}

wait_for_exact_fences() {
	local node=$1 expected=$2 attempts=${3:-150} actual="" i
	for ((i = 0; i < attempts; i++)); do
		actual=$(node_fences "$node")
		[[ -n "$actual" && "$actual" == "$expected" ]] && {
			echo "$actual"
			return 0
		}
		sleep 2
	done
	echo "${actual:-}"
	return 1
}