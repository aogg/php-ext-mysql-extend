<?php
// R3 / BUG-3 regression: pure-ini deployment probe (no MRE_* env may be set).
// Deploy with: php -d extension=mre.so -d mre.enable=1 -d mre.log_file=<file>
//              [-d mre.gtx_timeout=777] t13_inionly.php
// PASS evidence: <file> gets created with mysql req/resp lines (ini-only config
// works on 7.4), and mre_gtx_status()['timeout'] equals the -d value.
require __DIR__ . '/common.php';

mysqli_report(MYSQLI_REPORT_OFF);

$m = mre_db();
$r = $m->query('SELECT id, name FROM users WHERE name = \'alice\'');
$row = $r ? $r->fetch_assoc() : null;
if ($r) { $r->close(); }
$m->close();

$timeout = null;
if (function_exists('mre_gtx_status')) {
    $g = mre_gtx_status();
    $timeout = isset($g['timeout']) ? $g['timeout'] : null;
}

echo 't13-row=' . ($row ? $row['name'] : 'null') . ' t13-timeout=' . var_export($timeout, true) . "\n";
echo "t13 done\n";
