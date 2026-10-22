<?php
// t14: log_script edge-case scenarios against real redis (BUG-2 closure evidence).
// usage: php t14_script_edge.php <rte|parse|ret123|retnull|ok>
// Each scenario runs in its OWN php process; the matching callback is selected
// outside via MRE_LOG_SCRIPT (cb_t14.php, or cb_t14_parse.php for "parse").
// Semantics asserted by the driver (run-matrix.sh t14):
//   rte     : callback throws RuntimeException -> record degrades to main log,
//             exactly one type:"warn" (message digest), later records continue
//   parse   : callback file itself has a parse error -> ParseError caught by the
//             PHP-level wrapper, same degrade+one warn, later records continue
//   ret123  : callback returns int 123            -> silent degrade, NO warn
//   retnull : callback returns null               -> silent degrade, NO warn
//   ok      : callback returns a route path       -> records land there, main log absent
require __DIR__ . '/common.php';

$sc = isset($argv[1]) ? $argv[1] : '';
if (!in_array($sc, array('rte', 'parse', 'ret123', 'retnull', 'ok'), true)) {
    fwrite(STDERR, "usage: php t14_script_edge.php <rte|parse|ret123|retnull|ok>\n");
    exit(2);
}

$mark = 'mre:t14:' . $sc . ':' . (string)getmypid();

$r = mre_redis();
echo "before-{$sc}\n";
$r->set($mark, '1');            // record 1: drives the scenario inside the callback
echo "after-{$sc}\n";
$r->set($mark . ':after', '1'); // record 2: must still be logged after the scenario
echo "t14 {$sc} done\n";
