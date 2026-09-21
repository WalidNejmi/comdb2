# Update read-only tunable
SELECT name, read_only FROM comdb2_tunables WHERE name = 'dir';
PUT TUNABLE dir '/tmp';
SELECT value AS 'nowatch' FROM comdb2_tunables WHERE name = 'nowatch';
PUT TUNABLE nowatch 1;
SELECT value AS 'nowatch' FROM comdb2_tunables WHERE name = 'nowatch';

# Invalid tunable
PUT TUNABLE nonexistent 100;

# Invalid tunable value
SELECT value AS 'allow_broken_datetimes' FROM comdb2_tunables WHERE name = 'allow_broken_datetimes';
PUT TUNABLE allow_broken_datetimes;
SELECT value AS 'allow_broken_datetimes' FROM comdb2_tunables WHERE name = 'allow_broken_datetimes';
PUT TUNABLE allow_broken_datetimes 100;
SELECT value AS 'allow_broken_datetimes' FROM comdb2_tunables WHERE name = 'allow_broken_datetimes';
PUT TUNABLE allow_broken_datetimes 'onn';
SELECT value AS 'allow_broken_datetimes' FROM comdb2_tunables WHERE name = 'allow_broken_datetimes';
PUT TUNABLE allow_broken_datetimes 'of';

SELECT name, value, read_only FROM comdb2_tunables WHERE name = 'latch_max_poll';

PUT TUNABLE latch_max_poll 100;
SELECT value FROM comdb2_tunables WHERE name = 'latch_max_poll';

PUT TUNABLE latch_max_poll 'xx';
PUT TUNABLE latch_max_poll xx;
SELECT value FROM comdb2_tunables WHERE name = 'latch_max_poll';

PUT TUNABLE latch_max_poll '10';
SELECT value FROM comdb2_tunables WHERE name = 'latch_max_poll';

# Test dynamic tunable using 'exec procedure' & 'put tunable'.
SELECT value AS 'lock_conflict_trace' FROM comdb2_tunables WHERE name = 'lock_conflict_trace';
PUT TUNABLE lock_conflict_trace 1;
SELECT value AS 'lock_conflict_trace' FROM comdb2_tunables WHERE name = 'lock_conflict_trace';
exec procedure sys.cmd.send('lock_conflict_trace')
SELECT value AS 'lock_conflict_trace' FROM comdb2_tunables WHERE name = 'lock_conflict_trace';
exec procedure sys.cmd.send('no_lock_conflict_trace')
SELECT value AS 'lock_conflict_trace' FROM comdb2_tunables WHERE name = 'lock_conflict_trace';
SELECT value AS 'no_lock_conflict_trace' FROM comdb2_tunables WHERE name = 'no_lock_conflict_trace';

# Test composite tunables.
SELECT name AS 'logmsg tunables' FROM comdb2_tunables WHERE name LIKE 'logmsg%' order by name;
SELECT value AS 'logmsg.level' FROM comdb2_tunables WHERE name = 'logmsg.level';
PUT TUNABLE 'logmsg.level' 'xxx';
PUT TUNABLE 'logmsg.level' 'error';
SELECT value AS 'logmsg.level' FROM comdb2_tunables WHERE name = 'logmsg.level';
exec procedure sys.cmd.send('logmsg level xxx');
exec procedure sys.cmd.send('logmsg level info');
SELECT value AS 'logmsg.level' FROM comdb2_tunables WHERE name = 'logmsg.level';

PUT TUNABLE logmsg.level 'info';
SELECT value AS 'logmsg.level' FROM comdb2_tunables WHERE name = 'logmsg.level';

PUT TUNABLE logmsg.level='debug';
SELECT value AS 'logmsg.level' FROM comdb2_tunables WHERE name = 'logmsg.level';

PUT TUNABLE 'logmsg.level'='error';
SELECT value AS 'logmsg.level' FROM comdb2_tunables WHERE name = 'logmsg.level';

SELECT name AS 'appsockpool tunables' FROM comdb2_tunables WHERE name LIKE 'appsockpool%' order by name;
SELECT value AS 'appsockpool.maxt' FROM comdb2_tunables WHERE name = 'appsockpool.maxt';
PUT TUNABLE 'appsockpool.maxt' 'xxx';
PUT TUNABLE 'appsockpool.maxt' 101;
SELECT value AS 'appsockpool.maxt' FROM comdb2_tunables WHERE name = 'appsockpool.maxt';
exec procedure sys.cmd.send('appsockpool maxt xxx');
exec procedure sys.cmd.send('appsockpool maxt 102');
SELECT value AS 'appsockpool.maxt' FROM comdb2_tunables WHERE name = 'appsockpool.maxt';

# Test joins on comdb2_tunables (added "order by + limit" so that the output
# remains mostly unchanged on every tunable addition)
select c.name from comdb2_tunables c, comdb2_tunables d where c.name like '%colum%' order by c.name limit 1;

# Test 'max_query_fingerprints'
select value from comdb2_tunables where name = 'max_query_fingerprints'
put tunable 'max_query_fingerprints' 2000;
select value from comdb2_tunables where name = 'max_query_fingerprints'

# Test schema-change fence hardening controls.
PUT TUNABLE mask_internal_tunables 0;
SELECT name, value FROM comdb2_tunables WHERE name IN ('mempv_test_bump_fence_epoch', 'mempv_test_pause_before_cache_put', 'mempv_test_paused', 'sc_commit_flags_advertise', 'sc_fence_force_unready', 'sc_fence_persist_fail_after', 'sc_fence_publish_fail_after', 'sc_fence_test_drop_pending', 'sc_fence_test_extra_files', 'sc_pause_after_fence') ORDER BY name;
PUT TUNABLE mempv_test_bump_fence_epoch 1;
PUT TUNABLE mempv_test_pause_before_cache_put 1;
PUT TUNABLE sc_commit_flags_advertise 0;
PUT TUNABLE sc_fence_force_unready 1;
PUT TUNABLE sc_fence_persist_fail_after 0;
PUT TUNABLE sc_fence_publish_fail_after 1;
PUT TUNABLE sc_fence_test_drop_pending 1;
PUT TUNABLE sc_fence_test_extra_files 300;
PUT TUNABLE sc_pause_after_fence 1;
SELECT name, value FROM comdb2_tunables WHERE name IN ('mempv_test_bump_fence_epoch', 'mempv_test_pause_before_cache_put', 'mempv_test_paused', 'sc_commit_flags_advertise', 'sc_fence_force_unready', 'sc_fence_persist_fail_after', 'sc_fence_publish_fail_after', 'sc_fence_test_drop_pending', 'sc_fence_test_extra_files', 'sc_pause_after_fence') ORDER BY name;
PUT TUNABLE mempv_test_bump_fence_epoch 0;
PUT TUNABLE mempv_test_pause_before_cache_put 0;
PUT TUNABLE sc_commit_flags_advertise 1;
PUT TUNABLE sc_fence_force_unready 0;
PUT TUNABLE sc_fence_persist_fail_after 2147483647;
PUT TUNABLE sc_fence_publish_fail_after 2147483647;
PUT TUNABLE sc_fence_test_drop_pending 0;
PUT TUNABLE sc_fence_test_extra_files 0;
PUT TUNABLE sc_pause_after_fence 0;

