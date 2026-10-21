--TEST--
mre_gtx cross-fpm rollback: child transactions are rolled back globally
--INI--
mre.gtx=1
--SKIPIF--
<?php
if (!function_exists('proc_open')) die('skip proc_open unavailable');
$m = @mysqli_connect('localhost', 'root', '', 'mretest', 3306, '/run/mysqld/mysqld.sock');
if (!$m) die('skip needs local mariadb with root socket auth');
--FILE--
<?php
$so = realpath(__DIR__ . '/../modules/mre.so');
if (!$so) $so = ini_get('extension_dir') . '/mre.so';
$sdir = sys_get_temp_dir() . '/mre_t005_' . getmypid();
@mkdir($sdir);
ini_set('mre.state_dir', $sdir);
mre_yaml_dump(); // config refresh
$sock = '/run/mysqld/mysqld.sock';

$me = mysqli_connect('localhost', 'root', '', 'mretest', 3306, $sock);
$me->query('DROP TABLE IF EXISTS mre_gtx_t');
$me->query('CREATE TABLE mre_gtx_t (id INT AUTO_INCREMENT PRIMARY KEY, tag VARCHAR(20)) ENGINE=InnoDB');

// child worker: opens its own transaction inside the global window
file_put_contents("$sdir/child.php", <<<PHP
<?php
[\$_, \$step, \$sock, \$sdir] = \$argv;
\$m = mysqli_connect('localhost', 'root', '', 'mretest', 3306, \$sock);
\$wait = function (\$cond, \$timeout = 25) {
    \$dl = time() + \$timeout;
    do { usleep(50000); \$s = mre_gtx_status(); } while (!\$cond(\$s) && time() < \$dl);
    return \$s;
};
if (\$step === 'A') {
    if (!mre_gtx_begin()) { fwrite(STDERR, "begin failed\\n"); exit(1); }
} else {
    \$wait(fn(\$s) => \$s['active'], 25);
}
\$m->query('START TRANSACTION');
\$m->query("INSERT INTO mre_gtx_t (tag) VALUES ('from-\$step')");
\$s = \$wait(fn(\$s) => \$s['tracked_connections'] >= 2);
echo "\$step: tracked=", \$s['tracked_connections'], "\\n";
\$s = \$wait(fn(\$s) => \$s['state'] === 'mark_rollback');
echo "\$step: saw mark_rollback\\n";
\$m->query('SELECT 1'); // entry hook sends the real ROLLBACK
\$s2 = mre_gtx_status();
echo "\$step: after-select tracked=", \$s2['tracked_connections'], "\\n";
\$r = \$m->query("SELECT COUNT(*) c FROM mre_gtx_t WHERE tag='from-\$step'");
echo "\$step: my-rows-after-rollback=", \$r->fetch_assoc()['c'], "\\n";
\$m->close();
PHP);

$env = ['MRE_STATE_DIR' => $sdir, 'MRE_GTX' => '1', 'PATH' => '/usr/bin:/bin'];
$specs = [0 => ['pipe', 'r'], 1 => ['file', "$sdir/outA.log", 'w'], 2 => ['file', "$sdir/errA.log", 'w']];
$cmdA = sprintf('%s -d extension=%s -d mre.gtx=1 -d mre.state_dir=%s %s A %s %s',
    escapeshellarg(PHP_BINARY), escapeshellarg($so), escapeshellarg($sdir),
    escapeshellarg("$sdir/child.php"), escapeshellarg($sock), escapeshellarg($sdir));
$pA = proc_open($cmdA, $specs, $p);
$specs[1] = ['file', "$sdir/outB.log", 'w'];
$specs[2] = ['file', "$sdir/errB.log", 'w'];
$cmdB = sprintf('%s -d extension=%s -d mre.gtx=1 -d mre.state_dir=%s %s B %s %s',
    escapeshellarg(PHP_BINARY), escapeshellarg($so), escapeshellarg($sdir),
    escapeshellarg("$sdir/child.php"), escapeshellarg($sock), escapeshellarg($sdir));
$pB = proc_open($cmdB, $specs, $p);

// wait until both children are tracked
$s = null;
$dl = time() + 25;
do {
    usleep(100000);
    $s = mre_gtx_status();
} while ($s['tracked_connections'] < 2 && time() < $dl);
echo "parent: tracked=", $s['tracked_connections'], "\n";
echo "parent: rollback=", var_export(mre_gtx_rollback(), true), "\n";

proc_close($pA);
proc_close($pB);

$final = mre_gtx_status();
echo "parent: final-state=", $final['state'], "\n";
$r = $me->query('SELECT COUNT(*) c FROM mre_gtx_t');
echo "parent: total-rows=", $r->fetch_assoc()['c'], "\n";
echo "parent: reg-removed=", var_export(!file_exists("$sdir/mre-gtx.reg"), true), "\n";

echo "A-log:\n", file_get_contents("$sdir/outA.log");
echo "B-log:\n", file_get_contents("$sdir/outB.log");

$me->query('DROP TABLE IF EXISTS mre_gtx_t');
array_map('unlink', glob("$sdir/*"));
@rmdir($sdir);
echo "done\n";
?>
--EXPECT--
parent: tracked=2
parent: rollback=true
parent: final-state=none
parent: total-rows=0
parent: reg-removed=true
A-log:
A: tracked=2
A: saw mark_rollback
A: after-select tracked=1
A: my-rows-after-rollback=0
B-log:
B: tracked=2
B: saw mark_rollback
B: after-select tracked=0
B: my-rows-after-rollback=0
done
