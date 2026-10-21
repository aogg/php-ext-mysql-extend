--TEST--
mre resolves long ini keys without truncation (BUG-3 regression: mre.gtx_timeout)
--INI--
mre.gtx=0
mre.gtx_timeout=777
--FILE--
<?php
// the resolved gtx timeout is observable through mre_gtx_status();
// on the broken build INI_STR() truncated every lookup to sizeof(char*)
// bytes, so "mre.gtx_timeout" (15 chars) hit the "mre.gtx" entry instead
// and resolved to 0 -> clamped to 10 regardless of the ini value.

$s = mre_gtx_status();
var_dump($s['state']);      // short key mre.gtx=0 must still resolve
var_dump($s['timeout']);    // int(777) from the 15-char key

// runtime change re-resolved through the same path
ini_set('mre.gtx_timeout', '640');
mre_yaml_dump(); // forces a config re-resolve (same trick as 004)
$s = mre_gtx_status();
var_dump($s['timeout']);    // int(640), NOT the 300 default

ini_set('mre.gtx_timeout', '99999');
mre_yaml_dump();
$s = mre_gtx_status();
var_dump($s['timeout']);    // int(86400): the value 99999 was really read, then clamped
?>
--EXPECT--
string(8) "disabled"
int(777)
int(640)
int(86400)
