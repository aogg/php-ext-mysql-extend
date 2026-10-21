<?php
// S5: log_script callback routing against real redis
require __DIR__ . '/common.php';

$base = '/tmp/mre-test/logs/s5';
if (!is_dir($base)) { @mkdir($base, 0777, true); }
foreach (array('main', 'header', 'post', 'uri') as $f) { @unlink("{$base}/{$f}.log"); }
@unlink("{$base}/ctx.jsonl");

$m = mre_db();
$r = mre_redis();

// case a/f: CLI, nothing touched -> headers must be [] and record falls to main
$r->set('mre:s5:plain', '1');

// case b: header routing (headers derived from $_SERVER HTTP_*)
$_SERVER['HTTP_X_MRE_ROUTE'] = 'from-headers';
$r->set('mre:s5:hdr', '1');
unset($_SERVER['HTTP_X_MRE_ROUTE']);

// case c: post routing
$_POST['route'] = 'from-post';
$r->set('mre:s5:post', '1');
unset($_POST['route']);

// case d: uri routing
$_SERVER['REQUEST_URI'] = '/special-uri?x=1';
$_SERVER['REQUEST_METHOD'] = 'POST';
$r->set('mre:s5:uri', '1');
unset($_SERVER['REQUEST_URI'], $_SERVER['REQUEST_METHOD']);

// case e: script throws -> degrade to main + one-time warn
$r->set('mre:s5:BOOM', '1');

$m->query('SELECT id FROM users LIMIT 1'); // mysql record also passes the script

$r->close();
$m->close();
echo "t05 done\n";
