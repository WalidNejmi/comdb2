#!/usr/bin/env bash
# memlife_snapshot.sh <dbname> <host> <snapshots.tsv> <label> [cdb2sql options...]
#
# Append one grouped comdb2_memstats snapshot of <dbname> on <host> to
# <snapshots.tsv>, one row per allocator name:
#   label  epoch  host  pid  rss_kb  role  name  used  total  free  peak
# pid/rss come from the node itself (ssh when the node is not this host).
# comdb2_memstats needs the native allocator: it is empty with USE_SYS_ALLOC.
set -u
db=$1 host=$2 out=$3 label=$4; shift 4
ts=$(date +%s.%N)
# Select by process name: on cluster nodes the server is started through a
# 'bash -c "... comdb2 <db> ..."' wrapper whose command line also matches.
probe="p=\$(pgrep -a -x comdb2 | grep -E ' $db( |\$)' | head -1 | cut -d' ' -f1); [ -n \"\$p\" ] && echo \"\$p \$(ps -o rss= -p \$p)\""
if [[ $host == "$(hostname -s)" || $host == "$(hostname -f)" || $host == localhost ]]; then
    pr=$(bash -c "$probe")
else
    pr=$(ssh -o BatchMode=yes -o StrictHostKeyChecking=no "$host" "$probe" 2>/dev/null)
fi
read -r pid rss <<< "${pr:-- -}"
role=$(cdb2sql "$@" --tabs "$db" --host "$host" \
    "SELECT CASE WHEN host = comdb2_host() THEN 'master' ELSE 'replicant' END FROM comdb2_cluster WHERE is_master = 'Y'" 2>/dev/null | head -1)
cdb2sql "$@" --tabs "$db" --host "$host" \
    'SELECT name, SUM(used), SUM(total), SUM("free"), SUM(peak) FROM comdb2_memstats GROUP BY name ORDER BY name' |
while IFS=$'\t' read -r name used total free peak; do
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$label" "$ts" "$host" "${pid:--}" "${rss:--}" "${role:--}" \
        "$name" "$used" "$total" "$free" "$peak"
done >> "$out"
