<?php
// minimal repro for log_script exception path (bug probe)
require __DIR__ . '/common.php';

$r = mre_redis();
echo "before-boom\n";
$r->set('mre:t12:BOOM', '1'); // cb_t12 throws on BOOM content
echo "after-boom\n";
$r->set('mre:t12:after', '1');
echo "t12 done\n";
