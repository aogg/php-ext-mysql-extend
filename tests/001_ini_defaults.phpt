--TEST--
mre ini defaults are exposed
--INI--
mre.enable=0
--FILE--
<?php
var_dump(ini_get('mre.enable'));
var_dump(ini_get('mre.log_format'));
var_dump(ini_get('mre.redis_port'));
var_dump(ini_get('mre.sample_rows'));
var_dump(ini_get('mre.log_file'));
?>
--EXPECT--
string(1) "0"
string(4) "json"
string(4) "6379"
string(1) "1"
string(0) ""
