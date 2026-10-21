<?php
// S3: text format log content
require __DIR__ . '/common.php';

$mk = (string)getmypid();

$m = mre_db();
$m->query("SELECT id, name FROM users WHERE name = 'alice' /* t03-{$mk} */");
$m->query("INSERT INTO users (name, age) VALUES ('s3-{$mk}', 33)");
$m->query("DELETE FROM users WHERE name = 's3-{$mk}'");
$m->close();

$r = mre_redis();
$r->set('mre:s3:' . $mk, 'text');
$v = $r->get('mre:s3:' . $mk);
$r->del('mre:s3:' . $mk);
$r->close();

echo "t03-marker={$mk} get={$v}\n";
echo "t03 done\n";
