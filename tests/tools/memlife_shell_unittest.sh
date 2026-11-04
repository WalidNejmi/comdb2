#!/usr/bin/env bash
# Unit tests of memlife_common.sh failure handling, with a fake cdb2sql (no
# database needed). Run: bash tests/tools/memlife_shell_unittest.sh
# Each case is a small runit that sources memlife_common.sh; the case checks
# the exit code, execution.json and that no memory verdict was produced when
# the workload or a snapshot failed.
set -u
TOOLS=$(cd "$(dirname "$0")" && pwd)
export TESTSROOTDIR=$(dirname "$TOOLS")
WORK=$(mktemp -d /tmp/memlife_shell_ut.XXXXXX)
trap '[[ -n ${KEEP:-} ]] || rm -rf "$WORK"' EXIT
mkdir -p "$WORK/bin"
cat > "$WORK/bin/cdb2sql" <<'EOF'
#!/usr/bin/env bash
# fake cdb2sql: FAKE_FAIL_SQL (regex) fails matching statements,
# FAKE_EMPTY_MEMSTATS=1 returns no comdb2_memstats rows
sql=${!#}
check() { [[ -n ${FAKE_FAIL_SQL:-} && $1 =~ $FAKE_FAIL_SQL ]] && { echo "fake error: $1" >&2; return 1; }; return 0; }
if [[ $sql == - ]]; then
    rc=0
    while IFS= read -r line; do check "$line" || rc=1; done
    exit $rc
fi
check "$sql" || exit 1
case $sql in
*comdb2_memstats*) [[ ${FAKE_EMPTY_MEMSTATS:-0} == 1 ]] || printf 'berkdb\t1000000\t2000000\t1000000\t1500000\nutil\t500000\t600000\t100000\t500000\n' ;;
*"CASE WHEN host"*) echo master ;;
*comdb2_cluster*|*comdb2_host*) hostname -s ;;
esac
exit 0
EOF
chmod +x "$WORK/bin/cdb2sql"
export PATH=$WORK/bin:$PATH CDB2_OPTIONS= MEMLIFE_SETTLE=0 MEMLIFE_ITERS=3
unset CLUSTER DEBUGGER MEMLIFE_DEBUG_ALLOC MEMLIFE_RETENTION
pass=0 fail=0

case_() {   # case_ <name> <expected rc> <expected execution status> <runit body>; env via FAKE_*
    local name=$1 want_rc=$2 want_st=$3 body=$4 rc st dir
    export TESTDIR=$WORK/$name TESTCASE=$name
    mkdir -p "$TESTDIR"
    printf 'source %s/memlife_common.sh\n%s\nmemlife_main\n' "$TESTSROOTDIR" "$body" > "$TESTDIR/runit"
    (cd "$TESTDIR" && bash ./runit fakedb > "$TESTDIR/out" 2>&1)
    rc=$?
    dir=$TESTDIR/memlife/$name
    st=$(sed -n 's/.*"execution_status": "\([A-Z_]*\)".*/\1/p' "$dir/execution.json" 2>/dev/null)
    local ok=1
    [[ $rc == "$want_rc" && $st == "$want_st" ]] || ok=0
    if [[ $want_st != WORKLOAD_PASS ]]; then
        [[ -e $dir/comparison.csv ]] && ok=0
        grep -q '^MEMORY: INCONCLUSIVE' "$dir/summary.txt" 2>/dev/null || ok=0
    fi
    if (( ok )); then
        pass=$((pass + 1)); echo "ok   $name ($st)"
    else
        fail=$((fail + 1)); echo "FAIL $name: rc $rc (want $want_rc), status '$st' (want $want_st)"; sed 's/^/     /' "$TESTDIR/out"
    fi
}

case_ pass 0 WORKLOAD_PASS 'memlife_iteration() { memlife_sql "SELECT 1" >/dev/null; }'
grep -q '^MEMORY: STABLE' "$WORK/pass/memlife/pass/summary.txt" || { fail=$((fail + 1)); echo "FAIL pass: no STABLE verdict"; }

FAKE_FAIL_SQL=BAD case_ sql_failure 1 WORKLOAD_FAIL \
    'memlife_iteration() { memlife_sql "SELECT 1" >/dev/null; [[ $1 == 2 ]] && memlife_sql "BAD" >/dev/null; true; }'
FAKE_FAIL_SQL=BAD case_ sql_failure_in_subshell 1 WORKLOAD_FAIL \
    'memlife_iteration() { local x; x=$(memlife_sql "BAD"); true; }'
case_ generator_failure 1 WORKLOAD_FAIL \
    'gen() { echo "SELECT 1"; return 3; }
memlife_iteration() { memlife_batch gen; }'
FAKE_FAIL_SQL=BAD case_ batch_statement_failure 1 WORKLOAD_FAIL \
    'gen() { echo "SELECT 1"; echo "BAD"; echo "SELECT 2"; }
memlife_iteration() { memlife_batch gen; }'
case_ background_worker_failure 1 WORKLOAD_FAIL \
    'memlife_iteration() { memlife_bg_start w bash -c "sleep 0.2; exit 4"; memlife_bg_wait w; }'
FAKE_FAIL_SQL=BAD case_ background_sql_failure_unwaited 1 WORKLOAD_FAIL \
    'memlife_iteration() { ( memlife_sql "BAD" ) & wait; true; }'
case_ require_failure 1 WORKLOAD_FAIL 'memlife_iteration() { memlife_require "the updater ran" false; }'
case_ expect_failure_succeeded 1 WORKLOAD_FAIL 'memlife_iteration() { memlife_sql_expect_failure "SELECT 1"; }'
case_ settle_timeout 1 WORKLOAD_FAIL \
    'memlife_iteration() { :; }
memlife_settle() { memlife_wait_until 2 "a condition that never holds" false; }'
case_ reclaim_timeout 1 WORKLOAD_FAIL \
    'MEMLIFE_RETENTION="util:test"
memlife_iteration() { :; }
memlife_reclaim() { memlife_wait_until 1 "logs deleted" false; }'
FAKE_EMPTY_MEMSTATS=1 case_ empty_snapshot 1 INFRA_FAIL 'memlife_iteration() { :; }'
FAKE_FAIL_SQL='memstat debug dump' MEMLIFE_DEBUG_ALLOC=berkdb case_ debug_dump_failure 1 INFRA_FAIL \
    'memlife_iteration() { :; }'
case_ retention_without_reclaim 1 INCOMPLETE 'MEMLIFE_RETENTION="util:test"
memlife_iteration() { :; }'
case_ needs_cluster 0 NOT_APPLICABLE 'MEMLIFE_NEEDS_CLUSTER=1
memlife_iteration() { :; }'

echo "memlife_shell_unittest: $pass passed, $fail failed"
(( fail == 0 ))
