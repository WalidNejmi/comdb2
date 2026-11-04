#!/usr/bin/env bash
# memlife_snapshot.sh <dbname> <host> <snapshots.tsv> <label> [cdb2sql options...]
#   Append one grouped comdb2_memstats snapshot of <dbname> on <host> to
#   <snapshots.tsv>, one row per allocator name:
#     label  epoch  host  pid  rss_kb  role  name  used  total  free  peak
#   Fails (and appends nothing) if the node cannot be queried or returns no
#   allocator rows. pid/rss are auxiliary ('-' when the probe fails).
#   comdb2_memstats needs the native allocator: it is empty with USE_SYS_ALLOC.
# memlife_snapshot.sh --base <dbname> <host>
#   Print the load address of the comdb2 executable on <host> (hex), from
#   /proc/<pid>/maps, for symbolizing memstat debug stacks of PIE builds.
set -u
local_host() { [[ $1 == "$(hostname -s)" || $1 == "$(hostname -f)" || $1 == localhost ]]; }
on_host() {   # on_host <host> <script>
    if local_host "$1"; then bash -c "$2"; else ssh -o BatchMode=yes -o StrictHostKeyChecking=no "$1" "$2"; fi
}
# Select by process name: on cluster nodes the server is started through a
# 'bash -c "... comdb2 <db> ..."' wrapper whose command line also matches.
pid_of() { echo "pgrep -a -x comdb2 | grep -E ' $1( |\$)' | head -1 | cut -d' ' -f1"; }

if [[ ${1:-} == --base ]]; then
    db=$2 host=$3
    on_host "$host" "p=\$($(pid_of $db)); [ -n \"\$p\" ] || exit 1
        exe=\$(readlink /proc/\$p/exe) || exit 1
        awk -v exe=\"\$exe\" '\$6 == exe && \$3 == \"00000000\" { split(\$1, a, \"-\"); print a[1]; found = 1; exit }
                             END { exit !found }' /proc/\$p/maps"
    exit
fi

db=$1 host=$2 out=$3 label=$4; shift 4
ts=$(date +%s.%N)
pr=$(on_host "$host" "p=\$($(pid_of $db)); [ -n \"\$p\" ] && echo \"\$p \$(ps -o rss= -p \$p)\"" 2>/dev/null)
read -r pid rss <<< "${pr:-- -}"
role=$(cdb2sql "$@" --tabs "$db" --host "$host" \
    "SELECT CASE WHEN host = comdb2_host() THEN 'master' ELSE 'replicant' END FROM comdb2_cluster WHERE is_master = 'Y'" 2>/dev/null | head -1)
rows=$(cdb2sql "$@" --tabs "$db" --host "$host" \
    'SELECT name, SUM(used), SUM(total), SUM("free"), SUM(peak) FROM comdb2_memstats GROUP BY name ORDER BY name') || {
    echo "memlife_snapshot: comdb2_memstats query failed on $host" >&2; exit 1; }
[[ -n $rows ]] || { echo "memlife_snapshot: comdb2_memstats returned no rows on $host" >&2; exit 1; }
while IFS=$'\t' read -r name used total free peak; do
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$label" "$ts" "$host" "${pid:--}" "${rss:--}" "${role:--}" \
        "$name" "$used" "$total" "$free" "$peak"
done <<< "$rows" >> "$out"
