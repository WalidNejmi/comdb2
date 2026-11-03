# Memory-lifecycle suite helpers, sourced by memlife_*.test/runit.
#
# A test repeats one resource lifecycle (create -> use -> release) and checks
# that comdb2_memstats "used" stays bounded per allocator and per node.
# It defines:
#   memlife_iteration <i>   one repetition of the lifecycle (required)
#   memlife_setup           create long-lived objects (optional)
#   memlife_warmup          default: memlife_iteration 0
#   memlife_settle          default: sleep $MEMLIFE_SETTLE; override with a
#                           state-based wait (replication caught up, ...)
#   memlife_reclaim         optional: trigger the release event of a structure
#                           that is retained by design (snapshot RECLAIM after)
#   memlife_cleanup         optional
# and may set MEMLIFE_ITERS (8), MEMLIFE_SETTLE (3), MEMLIFE_RETENTION
# ("allocator:reason ..."), then calls memlife_main.
#
# Environment: MEMLIFE_DEBUG_ALLOC=<allocator> turns on memstat debug stack
# capture for that allocator (escalation rerun); MEMLIFE_FAIL_ON_GROWTH=1 makes
# STRONG_GROWTH fail the test (off by default: the suite does not gate).
#
# Artifacts in $TESTDIR/memlife/$TESTCASE: metadata.json, snapshots/,
# timing.tsv, comparison.csv, summary.txt, summary.json (and stacks.txt).
dbnm=$1
MEMLIFE_ITERS=${MEMLIFE_ITERS:-8}
MEMLIFE_SETTLE=${MEMLIFE_SETTLE:-3}
MEMLIFE_RETENTION=${MEMLIFE_RETENTION:-}
MEMLIFE_DIR=${TESTDIR}/memlife/${TESTCASE}
MEMLIFE_SNAP=$MEMLIFE_DIR/snapshots/snapshots.tsv
mkdir -p "$MEMLIFE_DIR/snapshots"

master=$(cdb2sql --tabs ${CDB2_OPTIONS} $dbnm default "SELECT host FROM comdb2_cluster WHERE is_master='Y'")
[[ -z $master ]] && master=$(cdb2sql --tabs ${CDB2_OPTIONS} $dbnm default 'SELECT comdb2_host()')
memlife_nodes=$(cdb2sql --tabs ${CDB2_OPTIONS} $dbnm default 'SELECT host FROM comdb2_cluster' | sort)
[[ -z $memlife_nodes ]] && memlife_nodes=$master
SQLM="cdb2sql ${CDB2_OPTIONS} $dbnm --host $master"

memlife_snapshot() {
    local n
    for n in $memlife_nodes; do
        ${TESTSROOTDIR}/tools/memlife_snapshot.sh $dbnm $n "$MEMLIFE_SNAP" "$1" ${CDB2_OPTIONS}
    done
}

memlife_settle() { sleep $MEMLIFE_SETTLE; }   # no explicit state to wait on
memlife_warmup() { memlife_iteration 0; }

memlife_debug() {   # memlife_debug on|dump <label>
    local n
    for n in $memlife_nodes; do
        if [[ $1 == on ]]; then
            cdb2sql ${CDB2_OPTIONS} $dbnm --host $n "EXEC PROCEDURE sys.cmd.send('memstat debug on $MEMLIFE_DEBUG_ALLOC')" >/dev/null
            cdb2sql ${CDB2_OPTIONS} $dbnm --host $n "EXEC PROCEDURE sys.cmd.send('memstat debug start')" >/dev/null
        else
            cdb2sql --tabs ${CDB2_OPTIONS} $dbnm --host $n \
                "EXEC PROCEDURE sys.cmd.send('memstat debug dump $MEMLIFE_DEBUG_ALLOC 1')" > "$MEMLIFE_DIR/debug.$2.$n.dump"
        fi
    done
}

memlife_main() {
    local _it _t0 status
    cat > "$MEMLIFE_DIR/metadata.json" <<EOF
{"test": "$TESTCASE", "database": "$dbnm", "nodes": "$(echo $memlife_nodes)", "master_at_start": "$master",
 "iterations": $MEMLIFE_ITERS, "settle_seconds": $MEMLIFE_SETTLE, "retention": "$MEMLIFE_RETENTION",
 "debug_allocator": "${MEMLIFE_DEBUG_ALLOC:-}", "comdb2_exe": "${COMDB2_EXE:-}", "started": "$(date -u +%FT%TZ)"}
EOF
    echo "memlife: $TESTCASE, nodes: $(echo $memlife_nodes), $MEMLIFE_ITERS iterations"
    declare -F memlife_setup >/dev/null && memlife_setup
    memlife_warmup
    memlife_settle
    memlife_snapshot A.1; sleep 1; memlife_snapshot A.2; sleep 1; memlife_snapshot A.3
    [[ -n ${MEMLIFE_DEBUG_ALLOC:-} ]] && memlife_debug on
    for _it in $(seq 1 $MEMLIFE_ITERS); do
        _t0=$(date +%s.%N)
        memlife_iteration $_it
        echo -e "$_it\t$_t0\t$(date +%s.%N)" >> "$MEMLIFE_DIR/timing.tsv"
        memlife_settle
        memlife_snapshot I$_it
        if [[ -n ${MEMLIFE_DEBUG_ALLOC:-} && ( $_it == 1 || $_it == $MEMLIFE_ITERS ) ]]; then
            memlife_debug dump rep$_it
        fi
    done
    if declare -F memlife_reclaim >/dev/null; then
        memlife_reclaim
        memlife_settle
        memlife_snapshot RECLAIM
    fi
    declare -F memlife_cleanup >/dev/null && memlife_cleanup
    python3 ${TESTSROOTDIR}/tools/memlife_compare.py --dir "$MEMLIFE_DIR" --test $TESTCASE --retention "$MEMLIFE_RETENTION"
    if [[ -n ${MEMLIFE_DEBUG_ALLOC:-} ]]; then
        python3 ${TESTSROOTDIR}/tools/memlife_stacks.py --dir "$MEMLIFE_DIR" --exe "$COMDB2_EXE" --first rep1 --last rep$MEMLIFE_ITERS
    fi
    cat "$MEMLIFE_DIR/summary.txt"
    status=$(sed -n 's/^STATUS: //p' "$MEMLIFE_DIR/summary.txt")
    if [[ ${MEMLIFE_FAIL_ON_GROWTH:-0} == 1 && $status == STRONG_GROWTH ]]; then
        echo "memlife: STRONG_GROWTH and MEMLIFE_FAIL_ON_GROWTH=1" >&2
        exit 1
    fi
}

# Release event of the commit LSN map (and anything else kept per log file):
# allow log deletion, switch to new log files, checkpoint, and wait until the
# oldest log files are gone on every node. Needs small log files (lrl:
# setattr LOGFILESIZE) and an aggressive deletion policy (setattr
# MIN_KEEP_LOGS 1, MIN_KEEP_LOGS_AGE 0, LOGDELETELOWFILENUM -1).
memlife_force_log_deletion() {
    local n i left
    for n in $memlife_nodes; do
        cdb2sql ${CDB2_OPTIONS} $dbnm --host $n "EXEC PROCEDURE sys.cmd.send('bdb setattr MIN_KEEP_LOGS 1')" >/dev/null
        cdb2sql ${CDB2_OPTIONS} $dbnm --host $n "EXEC PROCEDURE sys.cmd.send('sync log-delete on')" >/dev/null
    done
    for i in 1 2 3; do $SQLM "EXEC PROCEDURE sys.cmd.send('pushnext')" >/dev/null; done
    # log deletion runs from the checkpoint/purge threads: flush, checkpoint and
    # wait until no log file is waiting to be archived (as tests/logdelete does)
    for (( i = 0; i < 60; i++ )); do
        for n in $memlife_nodes; do
            cdb2sql ${CDB2_OPTIONS} $dbnm --host $n "EXEC PROCEDURE sys.cmd.send('flush')" >/dev/null
            cdb2sql ${CDB2_OPTIONS} $dbnm --host $n "EXEC PROCEDURE sys.cmd.send('bdb checkpoint')" >/dev/null
        done
        left=$($SQLM --tabs "EXEC PROCEDURE sys.cmd.send('bdb log_archive')" | grep -c 'log.00000')
        echo -e "reclaim_wait\t$i\t$left" >> $MEMLIFE_DIR/reclaim.tsv
        (( left == 0 )) && break
        sleep 5
    done
    sleep 5
}
