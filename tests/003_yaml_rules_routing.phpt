--TEST--
mre yaml rules route records to different log files
--INI--
mre.enable=1
--FILE--
<?php
$dir = sys_get_temp_dir() . '/mre_t003_' . getmypid();
@mkdir($dir);

file_put_contents("$dir/rules.yaml",
    "rules:\n" .
    "  - name: del-rule\n" .
    "    type: redis\n" .
    "    match: \"/^DEL /i\"\n" .
    "    log_file: $dir/del.log\n" .
    "  - name: slow-rule\n" .
    "    match: \"slow_\"\n" .
    "    log_file: $dir/slow.log\n" .
    "default_log_file: $dir/default.log\n");

// raw-socket fake server: only the CLIENT side is a PHP stream
// (an accepted php stream on the same port would be sniffed as well)
$server = socket_create(AF_INET, SOCK_STREAM, SOL_TCP);
socket_bind($server, '127.0.0.1', 0);
socket_listen($server);
socket_getsockname($server, $addr, $port);
$port = (int)$port;

ini_set('mre.log_file', "$dir/main.log");
ini_set('mre.yaml_config', "$dir/rules.yaml");
ini_set('mre.redis_port', (string)(int)$port);
mre_yaml_dump(); // triggers a config refresh (documented stage-A behaviour)

// effective rules must be visible through the dump
$dump = mre_yaml_dump();
var_dump(count($dump['rules_compiled']));
var_dump($dump['rules_compiled'][0]['name']);
var_dump($dump['rules_compiled'][0]['compiled']);
var_dump($dump['rules_compiled'][1]['name']);
var_dump($dump['default_log_file'] === "$dir/default.log");

$client = stream_socket_client("tcp://127.0.0.1:$port");
stream_set_blocking($client, false);
$conn = false;

function send_cmd($client, $server, &$conn, $cmd, ...$args) {
    $out = "*" . (count($args) + 1) . "\r\n\$" . strlen($cmd) . "\r\n$cmd\r\n";
    foreach ($args as $a) {
        $out .= "\$" . strlen($a) . "\r\n$a\r\n";
    }
    fwrite($client, $out);
    usleep(80000);
    if ($conn === false) {
        $conn = socket_accept($server);
    }
    socket_read($conn, 8192);
    socket_write($conn, "+OK\r\n");
    fread($client, 8192);
}

send_cmd($client, $server, $conn, 'DEL', 'slow_key');    // rule 1 (type redis, ^DEL)
send_cmd($client, $server, $conn, 'SET', 'slow_x', '1'); // rule 2 (slow_)
send_cmd($client, $server, $conn, 'GET', 'calm');        // default_log_file
socket_close($conn);
fclose($client);
socket_close($server);
usleep(120000);

$del = @file_get_contents("$dir/del.log");
$slow = @file_get_contents("$dir/slow.log");
$def = @file_get_contents("$dir/default.log");

var_dump((bool)$del, (bool)$slow, (bool)$def);
var_dump(strpos($del, 'DEL slow_key') !== false);
var_dump(strpos($slow, 'SET slow_x 1') !== false);
var_dump(strpos($slow, 'DEL slow_key') === false);   // first match wins, no spillover
var_dump(strpos($def, 'GET calm') !== false);
var_dump(strpos($def, 'CONNECT') !== false);         // connect record falls to default
var_dump(strpos((string)@file_get_contents("$dir/main.log"), 'redis') === false);

@unlink("$dir/rules.yaml");
@unlink("$dir/del.log"); @unlink("$dir/slow.log");
@unlink("$dir/default.log"); @unlink("$dir/main.log");
@rmdir($dir);
echo "done\n";
?>
--EXPECT--
int(2)
string(8) "del-rule"
bool(true)
string(9) "slow-rule"
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
done
