--TEST--
mre_gtx deferred COMMIT: intercepted commit stays open until the window commits
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
$sdir = sys_get_temp_dir() . '/mre_t006_' . getmypid();
@mkdir($sdir);
ini_set('mre.state_dir', $sdir);
mre_yaml_dump(); // config refresh
$sock = '/run/mysqld/mysqld.sock';

$me = mysqli_connect('localhost', 'root', '', 'mretest', 3306, $sock);
$me->query('DROP TABLE IF EXISTS mre_gtx_t');
$me->query('CREATE TABLE mre_gtx_t (id INT AUTO_INCREMENT PRIMARY KEY, tag VARCHAR(20)) ENGINE=InnoDB');

file_put_contents("$sdir/child.php", <<<PHP
<?php
[\$_, \$sock, \$sdir] = \$argv;
\$m = mysqli_connect('localhost', 'root', '', 'mretest', 3306, \$sock);
\$wait = function (\$cond, \$timeout = 25) {
    \$dl = time() + \$timeout;
    do { usleep(50000); \$s = mre_gtx_status(); } while (!\$cond(\$s) && time() < \$dl);
    return \$s;
};
if (!mre_gtx_begin()) { fwrite(STDERR, "begin failed\\n"); exit(1); }
\$m->query('START TRANSACTION');
\$m->query("INSERT INTO mre_gtx_t (tag) VALUES ('gtx-committed')");
\$ok = \$m->query('COMMIT'); // intercepted: true, but the tx stays open
echo "child: commit()=", var_export(\$ok, true), "\\n";
\$peek = mysqli_connect('localhost', 'root', '', 'mretest', 3306, \$sock);
\$r = \$peek->query("SELECT COUNT(*) c FROM mre_gtx_t WHERE tag='gtx-committed'");
echo "child: visible-before-finalize=", \$r->fetch_assoc()['c'], "\\n";
\$peek->close();
\$s = \$wait(fn(\$s) => \$s['state'] === 'mark_commit');
echo "child: saw mark_commit\\n";
\$m->query('SELECT 1'); // entry hook sends the real COMMIT
\$s2 = mre_gtx_status();
echo "child: after-select state=", \$s2['state'], "\\n";
\$m->close();
PHP);

$env = ['MRE_STATE_DIR' => $sdir, 'MRE_GTX' => '1', 'PATH' => '/usr/bin:/bin'];
$cmd = sprintf('%s -d extension=%s -d mre.gtx=1 -d mre.state_dir=%s %s %s %s',
    escapeshellarg(PHP_BINARY), escapeshellarg($so), escapeshellarg($sdir),
    escapeshellarg("$sdir/child.php"), escapeshellarg($sock), escapeshellarg($sdir));
$spec = [0 => ['pipe', 'r'], 1 => ['file', "$sdir/out.log", 'w'], 2 => ['file', "$sdir/err.log", 'w']];
$pC = proc_open($cmd, $spec, $p);

// wait until the child transaction is tracked
$s = null;
$dl = time() + 25;
do {
    usleep(100000);
    $s = mre_gtx_status();
} while ($s['tracked_connections'] < 1 && time() < $dl);
echo "parent: tracked=", $s['tracked_connections'], "\n";
echo "parent: commit=", var_export(mre_gtx_commit(), true), "\n";

proc_close($pC);

$final = mre_gtx_status();
echo "parent: final-state=", $final['state'], "\n";
$r = $me->query("SELECT COUNT(*) c FROM mre_gtx_t WHERE tag='gtx-committed'");
echo "parent: visible-after-finalize=", $r->fetch_assoc()['c'], "\n";

echo "C-log:\n", file_get_contents("$sdir/out.log");

$me->query('DELETE FROM mre_gtx_t WHERE tag=\'gtx-committed\'');
$me->query('DROP TABLE IF EXISTS mre_gtx_t');
array_map('unlink', glob("$sdir/*"));
@rmdir($sdir);
echo "done\n";
?>
--EXPECT--
parent: tracked=1
parent: commit=true
parent: final-state=none
parent: visible-after-finalize=1
C-log:
child: commit()=true
child: visible-before-finalize=0
child: saw mark_commit
child: after-select state=none
done
