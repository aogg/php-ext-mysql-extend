<?php
// S9: one SELECT + redis ops; switches selected by env outside
require __DIR__ . '/common.php';

$mk = (string)getmypid();

$m = mre_db();
$r9 = $m->query("SELECT id, name, age FROM users ORDER BY id LIMIT 4 /* s9-{$mk} */");
while ($row = $r9->fetch_assoc()) {}
$r9->close();
$m->close();

$r = mre_redis();
$r->set('mre:s9:' . $mk, 'x');
$v = $r->get('mre:s9:' . $mk);
$r->del('mre:s9:' . $mk);
$r->close();

echo "t09-marker={$mk} get={$v}\n";
echo "t09 done\n";
