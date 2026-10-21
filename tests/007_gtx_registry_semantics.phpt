--TEST--
mre_gtx registry semantics: disabled by default, dead-pid purge, expiry, begin exclusivity
--INI--
mre.gtx=0
--FILE--
<?php
$sdir = sys_get_temp_dir() . '/mre_t007_' . getmypid();
@mkdir($sdir);
ini_set('mre.state_dir', $sdir);
mre_yaml_dump(); // config refresh (documented stage-A behaviour)

// 1) disabled by default: every command refuses, status is inactive
var_dump(mre_gtx_begin());
var_dump(mre_gtx_commit());
var_dump(mre_gtx_rollback());
$s = mre_gtx_status();
var_dump($s['active'], $s['state'], $s['tracked_connections']);

// enable via ini for the rest of the test (config refresh via dump)
ini_set('mre.gtx', '1');
mre_yaml_dump();

// 2) window lifecycle
var_dump(mre_gtx_begin());
var_dump(mre_gtx_begin()); // already open
$s = mre_gtx_status();
var_dump($s['active'], $s['state'], strlen($s['gtx_id']) === 32);
var_dump(file_exists("$sdir/mre-gtx.reg"));

// 3) a live entry (our own pid) survives the dead-pid purge
$reg = "version 1\nstate active\ngtx_id aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n"
     . "started_at " . time() . "\ntimeout 300\nowner_pid " . getmypid() . "\n"
     . "conn fakehost:3306:99:999999 999999 " . time() . " 0\n"   // dead pid
     . "conn fakehost:3306:98:" . getmypid() . " " . getmypid() . " " . time() . " 0\n";
file_put_contents("$sdir/mre-gtx.reg", $reg);
$s = mre_gtx_status();
var_dump($s['tracked_connections']); // only the live one
var_dump(mre_gtx_rollback());        // marks; live entry kept for finalize
echo file_exists("$sdir/mre-gtx.reg") ? "reg-kept\n" : "reg-gone\n";
echo strpos(file_get_contents("$sdir/mre-gtx.reg"), 'mark_rollback') !== false
    ? "state-marked\n" : "state-wrong\n";
echo strpos(file_get_contents("$sdir/mre-gtx.reg"), (string) getmypid()) !== false
    ? "live-kept\n" : "live-dropped\n";
echo strpos(file_get_contents("$sdir/mre-gtx.reg"), '999999') !== false
    ? "dead-kept\n" : "dead-purged\n";

// 4) expired window degrades to rollback and ends when nobody is left
$reg = "version 1\nstate active\ngtx_id bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\n"
     . "started_at " . (time() - 4000) . "\ntimeout 300\nowner_pid 1\n";
file_put_contents("$sdir/mre-gtx.reg", $reg);
$s = mre_gtx_status();
var_dump($s['active'], $s['state']);
echo file_exists("$sdir/mre-gtx.reg") ? "expired-kept\n" : "expired-closed\n";

// 5) begin refuses while a finishing window still has live members
$reg = "version 1\nstate mark_rollback\ngtx_id cccccccccccccccccccccccccccccccc\n"
     . "started_at " . time() . "\ntimeout 300\nowner_pid 1\n"
     . "conn fakehost:3306:97:" . getmypid() . " " . getmypid() . " " . time() . " 1\n";
file_put_contents("$sdir/mre-gtx.reg", $reg);
var_dump(mre_gtx_begin()); // refused

// 6) cleanup of the finishing window by removing the entry
var_dump(mre_gtx_commit()); // switch the mark to commit
echo file_exists("$sdir/mre-gtx.reg") ? "reg-kept\n" : "reg-gone\n";

array_map('unlink', glob("$sdir/*"));
@rmdir($sdir);
echo "done\n";
?>
--EXPECT--
bool(false)
bool(false)
bool(false)
bool(false)
string(8) "disabled"
int(0)
bool(true)
bool(false)
bool(true)
string(6) "active"
bool(true)
bool(true)
int(1)
bool(true)
reg-kept
state-marked
live-kept
dead-purged
bool(false)
string(4) "none"
expired-closed
bool(false)
bool(true)
reg-kept
done
