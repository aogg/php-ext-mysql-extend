<?php
// S2: markers written to /proc/1/fd/1 (log target selected by env outside)
require __DIR__ . '/common.php';

$mk = (string)getmypid();

$m = mre_db();
$m->query("SELECT COUNT(*) c FROM users /* t02-mysql-{$mk} */");
$m->close();

$r = mre_redis();
$r->set('mre:s2:' . $mk, 'proc1');
$v = $r->get('mre:s2:' . $mk);
$r->del('mre:s2:' . $mk);
$r->close();

echo "t02-marker={$mk} get={$v}\n";
echo "t02 done\n";
