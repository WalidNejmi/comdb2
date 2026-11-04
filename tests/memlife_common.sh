# Memory-lifecycle suite helpers, sourced by memlife_*.test/runit.
#
# A test repeats one resource lifecycle (create -> use -> release) and checks
# that comdb2_memstats "used" stays bounded per allocator and per node.
# The suite fails closed: a memory verdict is only produced when the workload
# definitely ran, every node reached the expected state and every snapshot
# exists. A result has two parts:
#   execution: WORKLOAD_PASS WORKLOAD_FAIL INFRA_FAIL INCOMPLETE NOT_APPLICABLE
#   memory:    STABLE ONE_TIME_GROWTH PLATEAU RETENTION_BOUNDED
#              SUSPICIOUS_GROWTH STRONG_GROWTH INCONCLUSIVE
#
# A test defines:
#   memlife_iteration <i>   one repetition of the lifecycle (required)
#   memlife_setup           create long-lived objects (optional)
#   memlife_warmup          default: memlife_iteration 0
#   memlife_settle          default: sleep $MEMLIFE_SETTLE; override with a
#                           state-based wait that returns nonzero on timeout
#   memlife_reclaim         optional: release event of a structure retained by
#                           design; returns nonzero if the release did not happen
#   memlife_cleanup         optional
# and may set, before memlife_main (": ${MEMLIFE_ITERS:=24}" keeps an
# environment override working):
#   MEMLIFE_ITERS (8), MEMLIFE_SETTLE (3), MEMLIFE_RETENTION ("alloc:reason ..."),
#   MEMLIFE_RECLAIM_EVERY (0: reclaim once after the last iteration; k: after
#   every k iterations), MEMLIFE_NEEDS_CLUSTER=1, MEMLIFE_VALGRIND_HINT (the
#   variables that shrink the workload for a targeted Valgrind rerun),
#   MEMLIFE_RESTARTED_NODES (nodes the test restarts: reported, not classified).
# Reclaim snapshots are labelled R1..Rm.
#
# Workloads use the helpers below (memlife_sql, memlife_batch, memlife_bg_*,
# memlife_wait_until, memlife_require): any failure stops the test with
# WORKLOAD_FAIL and no memory verdict. Commands expected to fail must say so
# (memlife_sql_expect_failure, or an explicit "|| true" with a comment).
#
# Noise control: every memlife test except memlife_time_metrics sets
# timeseries_metrics_maxpoints 100 in lrl.options, so the time-series metric
# averagers (util allocator) stop growing before snapshot A; memlife_time_metrics
# tests that subsystem with production defaults. Do not add allocator ignore lists.
#
# Environment: MEMLIFE_DEBUG_ALLOC=<allocator> turns on memstat debug stack
# capture (escalation rerun); MEMLIFE_FAIL_ON_GROWTH=1 makes STRONG_GROWTH fail
# the test (off by default: the suite reports, it does not gate);
# MEMLIFE_SOURCE_SHA, MEMLIFE_BBPLUGINS_SHA, MEMLIFE_BUILDDB_SHA are recorded in
# metadata.json. Under DEBUGGER=valgrind/memcheck (non-native allocator)
# comdb2_memstats is empty: the workload runs, snapshots are skipped and the
# memory verdict is left to the Valgrind logs.
#
# Artifacts in $TESTDIR/memlife/$TESTCASE: metadata.json, execution.json,
# snapshots/snapshots.tsv, timing.tsv, comparison.csv, summary.txt,
# summary.json (and debug.*, stacks.txt).
set -o pipefail
dbnm=$1
MEMLIFE_DIR=${TESTDIR}/memlife/${TESTCASE}
MEMLIFE_SNAP=$MEMLIFE_DIR/snapshots/snapshots.tsv
MEMLIFE_NO_MEMSTATS=0
[[ ${DEBUGGER:-} == valgrind || ${DEBUGGER:-} == memcheck ]] && MEMLIFE_NO_MEMSTATS=1
mkdir -p "$MEMLIFE_DIR/snapshots"
rm -f "$MEMLIFE_DIR/execution.json"
master= memlife_nodes=

# ---------------------------------------------------------------- status
memlife_exec_status() { sed -n 's/.*"execution_status": "\([A-Z_]*\)".*/\1/p' "$MEMLIFE_DIR/execution.json" 2>/dev/null; }

memlife_set_status() {   # memlife_set_status <execution status> <detail>: the first status wins
    [[ -s $MEMLIFE_DIR/execution.json ]] && return 0
    python3 -c 'import json, sys; json.dump({"execution_status": sys.argv[1], "detail": sys.argv[2]}, open(sys.argv[3], "w"))' \
        "$1" "$2" "$MEMLIFE_DIR/execution.json"
}

memlife_fail() {   # memlife_fail <WORKLOAD_FAIL|INFRA_FAIL|INCOMPLETE> <detail>: stop, no memory verdict
    local st=$1; shift
    echo "memlife: $st: $*" >&2
    memlife_set_status "$st" "$*"
    printf 'TEST: %s\nEXECUTION: %s\nMEMORY: INCONCLUSIVE\nDETAIL: %s\n' "$TESTCASE" "$(memlife_exec_status)" \
        "$(sed -n 's/.*"detail": "\(.*\)"}.*/\1/p' "$MEMLIFE_DIR/execution.json")" > "$MEMLIFE_DIR/summary.txt"
    exit 1
}

memlife_check() {   # stop if a subshell or background worker already failed
    local st
    st=$(memlife_exec_status)
    [[ -z $st ]] && return 0
    printf 'TEST: %s\nEXECUTION: %s\nMEMORY: INCONCLUSIVE\nDETAIL: %s\n' "$TESTCASE" "$st" \
        "$(sed -n 's/.*"detail": "\(.*\)"}.*/\1/p' "$MEMLIFE_DIR/execution.json")" > "$MEMLIFE_DIR/summary.txt"
    echo "memlife: $st (recorded by a subshell or worker): $(cat "$MEMLIFE_DIR/execution.json")" >&2
    exit 1
}

memlife_not_applicable() {   # memlife_not_applicable <reason>: report and pass
    memlife_set_status NOT_APPLICABLE "$*"
    printf 'TEST: %s\nEXECUTION: NOT_APPLICABLE\nMEMORY: INCONCLUSIVE\nDETAIL: %s\n' "$TESTCASE" "$*" |
        tee "$MEMLIFE_DIR/summary.txt"
    exit 0
}

# ---------------------------------------------------------------- workload helpers
memlife_sql() {   # memlife_sql <sql> [cdb2sql options]: on the master; any failure stops the test
    local sql=$1; shift
    cdb2sql ${CDB2_OPTIONS} "$@" $dbnm --host $master "$sql" || memlife_fail WORKLOAD_FAIL "SQL failed: $sql"
}

memlife_sql_node() {   # memlife_sql_node <node> <sql> [cdb2sql options]
    local node=$1 sql=$2; shift 2
    cdb2sql ${CDB2_OPTIONS} "$@" $dbnm --host $node "$sql" || memlife_fail WORKLOAD_FAIL "SQL failed on $node: $sql"
}

memlife_sql_expect_failure() {   # memlife_sql_expect_failure <sql>: the statement must fail
    if cdb2sql ${CDB2_OPTIONS} $dbnm --host $master "$1" >/dev/null 2>&1; then
        memlife_fail WORKLOAD_FAIL "SQL expected to fail but succeeded: $1"
    fi
}

memlife_batch() {   # memlife_batch <generator> [args]: run the generator's SQL lines in one session
    local st
    "$@" | cdb2sql ${CDB2_OPTIONS} $dbnm --host $master - >/dev/null
    st=("${PIPESTATUS[@]}")
    (( st[0] == 0 )) || memlife_fail WORKLOAD_FAIL "SQL generator '$*' failed (rc ${st[0]})"
    (( st[1] == 0 )) || memlife_fail WORKLOAD_FAIL "SQL batch from '$*' had failing statements (rc ${st[1]})"
}

memlife_require() {   # memlife_require <description> <command...>
    local what=$1; shift
    "$@" || memlife_fail WORKLOAD_FAIL "$what"
}

declare -A memlife_bg_pids
memlife_bg_start() {   # memlife_bg_start <name> <command...>: background worker
    local name=$1; shift
    "$@" &
    memlife_bg_pids[$name]=$!
}
memlife_bg_running() { kill -0 "${memlife_bg_pids[$1]}" 2>/dev/null; }
memlife_bg_wait() {   # memlife_bg_wait <name>: the worker must exit 0
    local rc=0
    wait "${memlife_bg_pids[$1]}" || rc=$?
    (( rc == 0 )) || memlife_fail WORKLOAD_FAIL "background worker '$1' failed (rc $rc)"
}

memlife_wait_until() {   # memlife_wait_until <seconds> <description> <condition> [diagnostics]
    local t0=$SECONDS what=$2 cond=$3 diag=${4:-}
    until $cond; do
        if (( SECONDS - t0 >= $1 )); then
            echo "memlife: timeout after $(( SECONDS - t0 ))s waiting for: $what" >&2
            [[ -n $diag ]] && $diag >&2
            return 1
        fi
        sleep 1
    done
}

memlife_settle() { sleep $MEMLIFE_SETTLE; }   # no explicit state to wait on
memlife_warmup() { memlife_iteration 0; }

# Release event of the commit LSN map (and anything else kept per log file):
# allow log deletion, switch log files, checkpoint, and wait until no log file
# is left to archive on any node. Needs small log files and an aggressive
# deletion policy in lrl.options (see memlife_commitmap_purge).
memlife_archive_entries() {   # memlife_archive_entries <node>: log files waiting to be archived; fails if unreachable
    local out
    out=$(cdb2sql --tabs ${CDB2_OPTIONS} $dbnm --host $1 "EXEC PROCEDURE sys.cmd.send('bdb log_archive')") || return 1
    grep -c 'log\.0' <<< "$out" || true
}
memlife_logs_deleted() {
    local n left
    for n in $memlife_nodes; do
        cdb2sql ${CDB2_OPTIONS} $dbnm --host $n "EXEC PROCEDURE sys.cmd.send('flush')" >/dev/null || return 1
        cdb2sql ${CDB2_OPTIONS} $dbnm --host $n "EXEC PROCEDURE sys.cmd.send('bdb checkpoint')" >/dev/null || return 1
    done
    for n in $memlife_nodes; do
        left=$(memlife_archive_entries $n) || return 1
        [[ $left == 0 ]] || return 1
    done
}
memlife_logfiles() {   # memlife_logfiles <node>: number of log files in the node's database directory
    local d=${DBDIR:-$TESTDIR/$dbnm}/logs
    if [[ $1 == "$(hostname -s)" || $1 == "$(hostname -f)" ]]; then
        ls $d 2>/dev/null | grep -c '^log\.'
    else
        ssh -o BatchMode=yes -o StrictHostKeyChecking=no $1 "ls $d 2>/dev/null | grep -c '^log\.'"
    fi
}
memlife_log_diag() {
    local n
    echo "  master: $(cdb2sql --tabs ${CDB2_OPTIONS} $dbnm default "SELECT host FROM comdb2_cluster WHERE is_master='Y'")"
    for n in $memlife_nodes; do
        echo "  $n: $(memlife_archive_entries $n) log files left to archive, $(memlife_logfiles $n) log files"
    done
}
memlife_force_log_deletion() {
    local n
    for n in $memlife_nodes; do
        memlife_sql_node $n "EXEC PROCEDURE sys.cmd.send('bdb setattr MIN_KEEP_LOGS 1')" >/dev/null
        memlife_sql_node $n "EXEC PROCEDURE sys.cmd.send('sync log-delete on')" >/dev/null
    done
    for n in 1 2 3; do memlife_sql "EXEC PROCEDURE sys.cmd.send('pushnext')" >/dev/null; done
    memlife_wait_until 300 "log files archived and deleted on every node" memlife_logs_deleted memlife_log_diag ||
        return 1
    sleep 5
}

# ---------------------------------------------------------------- snapshots
memlife_snapshot() {   # memlife_snapshot <label>: every node, or the test stops
    local n
    (( MEMLIFE_NO_MEMSTATS )) && return 0
    for n in $memlife_nodes; do
        ${TESTSROOTDIR}/tools/memlife_snapshot.sh $dbnm $n "$MEMLIFE_SNAP" "$1" ${CDB2_OPTIONS} ||
            memlife_fail INFRA_FAIL "snapshot $1 failed on $n"
    done
}

memlife_debug() {   # memlife_debug on | dump <label>
    local n
    for n in $memlife_nodes; do
        if [[ $1 == on ]]; then
            cdb2sql ${CDB2_OPTIONS} $dbnm --host $n "EXEC PROCEDURE sys.cmd.send('memstat debug on $MEMLIFE_DEBUG_ALLOC')" >/dev/null &&
                cdb2sql ${CDB2_OPTIONS} $dbnm --host $n "EXEC PROCEDURE sys.cmd.send('memstat debug start')" >/dev/null ||
                memlife_fail INFRA_FAIL "cannot turn on memstat debug on $n"
            # load address of the executable on that node, to symbolize PIE builds
            # (memlife_stacks.py refuses to symbolize a PIE build without it)
            ${TESTSROOTDIR}/tools/memlife_snapshot.sh --base $dbnm $n > "$MEMLIFE_DIR/debug.base.$n" ||
                { rm -f "$MEMLIFE_DIR/debug.base.$n"; echo "memlife: no executable load address for $n" >&2; }
        else
            cdb2sql --tabs ${CDB2_OPTIONS} $dbnm --host $n \
                "EXEC PROCEDURE sys.cmd.send('memstat debug dump $MEMLIFE_DEBUG_ALLOC 1')" > "$MEMLIFE_DIR/debug.$2.$n.dump" ||
                memlife_fail INFRA_FAIL "memstat debug dump $2 failed on $n"
        fi
    done
}

# ---------------------------------------------------------------- metadata
memlife_metadata() {
    local n kernels="" reclaim=0 lrl=$PWD/lrl.options
    declare -F memlife_reclaim >/dev/null && reclaim=1
    [[ -f $lrl ]] || lrl=${TESTSROOTDIR}/${TESTCASE}.test/lrl.options
    for n in $memlife_nodes; do
        if [[ $n == "$(hostname -s)" || $n == "$(hostname -f)" ]]; then
            kernels+="$n=$(uname -r) "
        else
            kernels+="$n=$(ssh -o BatchMode=yes -o StrictHostKeyChecking=no $n uname -r 2>/dev/null || echo unknown) "
        fi
    done
    MEMLIFE_RECLAIM=$reclaim MEMLIFE_NODES="$(echo $memlife_nodes)" MEMLIFE_MASTER=$master MEMLIFE_KERNELS="$kernels" \
    MEMLIFE_LRL=$lrl MEMLIFE_RUNIT=${TESTSROOTDIR}/${TESTCASE}.test/runit MEMLIFE_DB=$dbnm \
    MEMLIFE_ITERS=$MEMLIFE_ITERS MEMLIFE_SETTLE=$MEMLIFE_SETTLE MEMLIFE_RETENTION="$MEMLIFE_RETENTION" \
    MEMLIFE_RECLAIM_EVERY=$MEMLIFE_RECLAIM_EVERY MEMLIFE_NO_MEMSTATS=$MEMLIFE_NO_MEMSTATS \
    MEMLIFE_VALGRIND_HINT="${MEMLIFE_VALGRIND_HINT:-}" MEMLIFE_RESTARTED_NODES="${MEMLIFE_RESTARTED_NODES:-}" \
        python3 ${TESTSROOTDIR}/tools/memlife_compare.py --write-metadata "$MEMLIFE_DIR/metadata.json" ||
        memlife_fail INFRA_FAIL "cannot write metadata.json"
}

# ---------------------------------------------------------------- main
memlife_init() {
    master=$(cdb2sql --tabs ${CDB2_OPTIONS} $dbnm default "SELECT host FROM comdb2_cluster WHERE is_master='Y'") ||
        memlife_fail INFRA_FAIL "cannot reach $dbnm"
    if [[ -z $master ]]; then
        master=$(cdb2sql --tabs ${CDB2_OPTIONS} $dbnm default 'SELECT comdb2_host()') ||
            memlife_fail INFRA_FAIL "cannot reach $dbnm"
    fi
    memlife_nodes=$(cdb2sql --tabs ${CDB2_OPTIONS} $dbnm default 'SELECT host FROM comdb2_cluster' | sort) ||
        memlife_fail INFRA_FAIL "cannot list the nodes of $dbnm"
    [[ -z $memlife_nodes ]] && memlife_nodes=$master
}

memlife_main() {
    local _it _t0 _r=0 status
    : ${MEMLIFE_ITERS:=8} ${MEMLIFE_SETTLE:=3} ${MEMLIFE_RECLAIM_EVERY:=0} ${MEMLIFE_RETENTION:=}
    if [[ ${MEMLIFE_NEEDS_CLUSTER:-0} == 1 && -z ${CLUSTER:-} ]]; then
        memlife_not_applicable "needs a cluster (CLUSTER is not set)"
    fi
    if [[ -n $MEMLIFE_RETENTION ]] && ! declare -F memlife_reclaim >/dev/null; then
        memlife_fail INCOMPLETE "MEMLIFE_RETENTION is declared but the test has no memlife_reclaim"
    fi
    memlife_init
    echo "memlife: $TESTCASE, nodes: $(echo $memlife_nodes), $MEMLIFE_ITERS iterations"
    if declare -F memlife_setup >/dev/null; then memlife_setup; memlife_check; fi
    memlife_metadata
    memlife_warmup; memlife_check
    memlife_settle || memlife_fail WORKLOAD_FAIL "settle after warm-up timed out"
    memlife_snapshot A.1; sleep 1; memlife_snapshot A.2; sleep 1; memlife_snapshot A.3
    [[ -n ${MEMLIFE_DEBUG_ALLOC:-} ]] && memlife_debug on
    for _it in $(seq 1 $MEMLIFE_ITERS); do
        _t0=$(date +%s.%N)
        memlife_iteration $_it
        memlife_check
        echo -e "$_it\t$_t0\t$(date +%s.%N)" >> "$MEMLIFE_DIR/timing.tsv"
        memlife_settle || memlife_fail WORKLOAD_FAIL "settle after iteration $_it timed out"
        memlife_snapshot I$_it
        if [[ -n ${MEMLIFE_DEBUG_ALLOC:-} ]] && (( _it == 1 || _it == MEMLIFE_ITERS )); then
            memlife_debug dump rep$_it
        fi
        if declare -F memlife_reclaim >/dev/null &&
            (( (MEMLIFE_RECLAIM_EVERY > 0 && _it % MEMLIFE_RECLAIM_EVERY == 0) ||
               (MEMLIFE_RECLAIM_EVERY == 0 && _it == MEMLIFE_ITERS) )); then
            _r=$(( _r + 1 ))
            memlife_reclaim || memlife_fail WORKLOAD_FAIL "reclaim $_r (after iteration $_it) did not complete"
            memlife_check
            memlife_settle || memlife_fail WORKLOAD_FAIL "settle after reclaim $_r timed out"
            memlife_snapshot R$_r
        fi
    done
    if declare -F memlife_cleanup >/dev/null; then memlife_cleanup; memlife_check; fi
    memlife_set_status WORKLOAD_PASS "lifecycle executed $MEMLIFE_ITERS times"
    if (( MEMLIFE_NO_MEMSTATS )); then
        printf 'TEST: %s\nEXECUTION: WORKLOAD_PASS\nMEMORY: INCONCLUSIVE\nDETAIL: %s\n' "$TESTCASE" \
            "comdb2_memstats is not available under DEBUGGER=$DEBUGGER; see the $DEBUGGER logs" | tee "$MEMLIFE_DIR/summary.txt"
        return 0
    fi
    python3 ${TESTSROOTDIR}/tools/memlife_compare.py --dir "$MEMLIFE_DIR" || {
        cat "$MEMLIFE_DIR/summary.txt" 2>/dev/null
        echo "memlife: snapshot validation failed: no memory verdict" >&2
        exit 1
    }
    if [[ -n ${MEMLIFE_DEBUG_ALLOC:-} ]]; then
        python3 ${TESTSROOTDIR}/tools/memlife_stacks.py --dir "$MEMLIFE_DIR" --exe "$COMDB2_EXE" \
            --first rep1 --last rep$MEMLIFE_ITERS || echo "memlife: stack aggregation failed (see stacks.txt)" >&2
    fi
    cat "$MEMLIFE_DIR/summary.txt"
    status=$(sed -n 's/^MEMORY: //p' "$MEMLIFE_DIR/summary.txt")
    if [[ ${MEMLIFE_FAIL_ON_GROWTH:-0} == 1 && $status == STRONG_GROWTH ]]; then
        echo "memlife: STRONG_GROWTH and MEMLIFE_FAIL_ON_GROWTH=1" >&2
        exit 1
    fi
}
