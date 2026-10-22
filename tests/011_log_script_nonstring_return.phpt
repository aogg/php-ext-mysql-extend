--TEST--
mre log_script: a non-string return (int/array/false) degrades to the next
resolver without a warning; only string paths route; no $___mre_* leak
--INI--
mre.enable=1
--FILE--
<?php
$dir = sys_get_temp_dir() . '/mre_t011_' . getmypid();
@mkdir($dir);

// callback: non-string returns must degrade silently (no warn, no routing)
file_put_contents("$dir/cb.php", <<<PHP
<?php
if (strpos(\$context['content'], 'NUM') !== false) {
    return 42;
}
if (strpos(\$context['content'], 'ARR') !== false) {
    return ['not', 'a', 'path'];
}
if (strpos(\$context['content'], 'ROUTE') !== false) {
    return '$dir/routed.log';
}
return null;
PHP);

// fake redis server so the sniffer produces records
$server = socket_create(AF_INET, SOCK_STREAM, SOL_TCP);
socket_bind($server, '127.0.0.1', 0);
socket_listen($server);
socket_getsockname($server, $addr, $port);
$port = (int)$port;

ini_set('mre.log_file', "$dir/main.log");
ini_set('mre.log_script', "$dir/cb.php");
ini_set('mre.redis_port', (string)$port);
mre_yaml_dump(); // config refresh

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

send_cmd($client, $server, $conn, 'SET', 'NUM_key', '1');    // int 42 -> main
send_cmd($client, $server, $conn, 'GET', 'ARR_key');         // array -> main
send_cmd($client, $server, $conn, 'SET', 'ROUTE_key', '1');  // string -> routed.log
send_cmd($client, $server, $conn, 'DEL', 'calm');            // null -> main
socket_close($conn);
fclose($client);
usleep(150000);

$routed = @file_get_contents("$dir/routed.log");
$main   = @file_get_contents("$dir/main.log");

// string return still routes
var_dump((bool)$routed);
var_dump(strpos($routed, 'SET ROUTE_key 1') !== false);
// non-string returns degraded to the main log
var_dump((bool)$main);
var_dump(strpos($main, 'SET NUM_key 1') !== false);
var_dump(strpos($main, 'GET ARR_key') !== false);
var_dump(strpos($main, 'DEL calm') !== false);
// no degradation warning at all: nothing failed, these are plain degrades
var_dump(substr_count($main, '"type":"warn"'));
// our reserved globals never leak into userland after a record
var_dump(isset($GLOBALS['___mre_ret'], $GLOBALS['___mre_path'],
               $GLOBALS['___mre_warn'], $GLOBALS['context']));

@unlink("$dir/cb.php"); @unlink("$dir/routed.log"); @unlink("$dir/main.log");
@rmdir($dir);
echo "done\n";
?>
--EXPECT--
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
bool(true)
int(0)
bool(false)
done
