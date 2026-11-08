---
name: comdb2-dev
description: How to build comdb2, run its test suite, start a throwaway database, and investigate bugs and performance (op logs, isolation levels, perf, uprobes). Use when building or testing comdb2, writing or running a test in tests/, changing db/, bdb/, berkdb/ or sqlite/src, or debugging locking, replication, schema change or planner behaviour.
---

# Working on comdb2

Everything here was checked on Ubuntu 24.04. Paths are relative to the repository root.

## Build

Packages beyond the README's list that the build or tests need:

```bash
apt-get install -y bison build-essential cmake ninja-build flex gawk jq tcl tcl-dev \
  libevent-dev liblz4-dev libprotobuf-c-dev protobuf-c-compiler libreadline-dev \
  libsqlite3-dev libssl-dev libunwind-dev libncurses-dev uuid-dev zlib1g-dev
```

`gawk` (used by `tests/runtestcase`) and `tcl-dev` (CMake fails without it) are not in the README.

```bash
cmake -S . -B build -G Ninja -DCOMDB2_TEST=ON
ninja -C build && ninja -C build test-tools
```

`-DCOMDB2_TEST=ON` is required for the test tools: without it about 20 of them fail to link
(`cdb2_cnonce`, `cdb2_cluster_info` and friends are only built with `CDB2API_TEST`).
A full build takes about 2 minutes on 4 cores and peaks around 1 GB of RAM.

To compare against unmodified code, build a second copy elsewhere (`/build*` is git-ignored):

```bash
git stash && cmake -S . -B build-base -G Ninja -DCOMDB2_TEST=ON && ninja -C build-base db/comdb2 && git stash pop
```

## Tests

```bash
cd tests
make basic                      # one test
make -kj3 basic cdb2bind        # several; -k keeps going after a failure
make reorder_upsert_noreorder_generated   # variant: <test>_<testopts file>_generated
make basic COMDB2_EXE=$PWD/../build-base/db/comdb2   # same test, another server binary
```

- Each test is a directory `tests/<name>.test/` with a `Makefile` (copy one), a `runit`
  script that gets the database name as `$1` and exits non-zero on failure, and optional
  `lrl.options` (extra server config) and `*.testopts` files (each one creates a variant).
- In `runit`, `source ${TESTSROOTDIR}/tools/runit_common.sh` gives `failexit "message"`.
  Pass `${CDB2_OPTIONS}` to every `cdb2sql` call.
- Output: `tests/test_<id>/logs/<name><id>.testcase`; `<name><id>.db` is the server log.
- A new regression test must fail against an unmodified build (`COMDB2_EXE=` above)
  and pass with the fix. Run it both ways before committing.
- `tests/test_*` directories pile up; delete them between runs.

Known failures that are not regressions (they fail on unmodified code too):
- `analyze`: "Analyze table 't1' failed" while sampling.
- `yast`: regexp1-2.20, 2.21, 2.22 and 3.1.
- `limitsortingtbl`: needs `ssh`.
- `reorder_deletes_noreorder_generated`: requires more than 110 deadlocks, which depends on
  machine load; it passes or fails from run to run.

## Repository rules

- Adding or changing a tunable: also update `tests/tunables.test` (see `AGENTS.md`).
- Changes to SQLite under `sqlite/src` go inside `#if defined(SQLITE_BUILDING_FOR_COMDB2)`
  blocks, like the existing comdb2 changes there.
- A new counter is easiest to expose through `comdb2_metrics`: a global in `db/comdb2.c`
  (declared in `db/comdb2.h`), a field and entry in `db/db_metrics.c`, set in
  `refresh_metrics()`. Use `ATOMIC_ADD64` from `comdb2_atomic.h` to increment it.

## A throwaway database

```bash
B=$PWD/build; D=/some/scratch/dir; mkdir -p $D/db
pgrep -x pmux || (nohup $B/tools/pmux/pmux -n -f > $D/pmux.log 2>&1 &)   # required: port registry
printf "name scratchdb\ndir $D/db\nsc_headroom 1\n" > $D/db/scratchdb.lrl
(cd $D/db && $B/db/comdb2 --create scratchdb --no-global-lrl --lrl $D/db/scratchdb.lrl)
(cd $D/db && nohup $B/db/comdb2 scratchdb --no-global-lrl --lrl $D/db/scratchdb.lrl --pidfile $D/pid > $D/db.log 2>&1 &)
echo "comdb2_config:default_type=local" > $D/cfg
for i in $(seq 30); do $B/tools/cdb2sql/cdb2sql -c $D/cfg scratchdb local "select 1" && break; sleep 1; done
```

- Without `pmux` running the server exits with "pmux register replication failed".
- `sc_headroom 1`: schema changes refuse to run when the filesystem has less than 10% free
  (the default), which happens on large shared disks with plenty of absolute space.
- Stop it with `kill $(cat $D/pid)`. Do not use `pkill -f "comdb2 scratchdb"`: the pattern
  also matches the shell running the command, so it kills itself.
- Useful lrl lines: `enable_snapshot_isolation`, `enable_serial_isolation`,
  `on enable_osql_logging` with `logmsg level debug` (see below).
- `exec procedure sys.cmd.verify('t')` checks every index of table `t` against its data.

## Investigating behaviour

**Isolation levels change which code runs.** `cdb2sql`'s default (`blocksql`) does not let a
transaction see its own writes, so "insert then update the same row" behaves differently there.
Run order-sensitive cases at all four levels, one session per transaction:

```bash
printf "set transaction read committed\nbegin\n...\ncommit\n" | cdb2sql -c $CFG db local -
```

Levels: `blocksql`, `read committed`, `snapshot isolation`, `serializable`.

**What the master actually applied.** With `on enable_osql_logging` and `logmsg level debug`
in the lrl, the server log shows each op the master receives and applies
(`OSQL_DELREC <genid>`, `OSQL_INSERT`, "Added new record failed"). A transaction that keeps
reappearing under new uuids is being replayed; "failed 500 times with verify errors" means it
gave up.

**Counters.** `select name, value from comdb2_metrics` and
`select name, value from comdb2_tunables where name like '%...%'`. Useful metrics:
`deadlocks`, `rep_deadlocks` (replication apply was the deadlock victim and retried),
`retries`, `sql_reprepares` (SQLite recompiled a cached statement).

**Profiling.** The binary has no frame pointers, so use `cpu-clock` sampling for "where does
time go" and DWARF unwinding for call stacks:

```bash
apt-get install -y linux-tools-generic; P=$(ls /usr/lib/linux-tools/*/perf | head -1)
$P record -e cpu-clock -F 999 -g -p $(cat $D/pid) -o prof.data -- sleep 15
$P report -i prof.data --no-children --sort sym -g none --stdio | head -40
```

To count calls or find who calls a function, add a uprobe and record DWARF stacks:

```bash
$P probe -x $B/db/comdb2 --add sqlite3RunParser
$P record -e probe_comdb2:sqlite3RunParser --call-graph dwarf,16384 -a -o p.data -- sleep 1
$P report -i p.data --stdio --no-children -G | head -40
$P probe --del 'probe_comdb2:*'
```

Use `perf stat -a` rather than `-p <pid>` for counts: `-p` only follows threads that exist
when it starts, and comdb2 creates SQL threads on demand.

## Background worth knowing

- Writes go through the master: the node running the SQL ships row ops (osql) to the master,
  which applies them in its own transaction. Only one node commits.
- With `reorder_socksql_no_deadlock` and `reorder_idx_writes` (both off by default) the master
  sorts a transaction's ops and defers index writes to commit, which removes most deadlocks
  between updates of existing rows. Transactions containing an upsert are applied in issue
  order (`tests/reorder_upsert.test`).
- On a replica, the apply thread takes all of a transaction's locks up front. With the default
  `deadlock_least_writes_ever`, readers holding no write locks are never the deadlock victim,
  so replication is the one that retries (`rep_deadlocks`).
