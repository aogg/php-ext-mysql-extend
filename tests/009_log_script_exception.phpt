--TEST--
mre log_script exception degrades once, process survives, later records keep working
(BUG-2 regression: PHP 7.4 SIGSEGV'd as soon as the log script threw)
--INI--
mre.enable=1
--FILE--
<?php
$dir = sys_get_temp_dir() . '/mre_t009_' . getmypid();
@mkdir($dir);
$ctxfile = "$dir/ctx.json";

// DANGER -> danger.log; BOOM -> throw; everything else -> null (main log)
file_put_contents("$dir/cb.php", <<<PHP
<?php
file_put_contents('$ctxfile', \$context['content'] . "\n", FILE_APPEND);
if (strpos(\$context['content'], 'DANGER') !== false) {
    return '$dir/danger.log';
}
if (strpos(\$context['content'], 'BOOM') !== false) {
    throw new RuntimeException('boom-9');
}
return null;
PHP);

// fake redis server: only the CLIENT side is a PHP stream
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

send_cmd($client, $server, $conn, 'SET', 'calm_1', '1'); // null -> main
send_cmd($client, $server, $conn, 'DEL', 'BOOM_1');      // throw #1 -> main + warn
send_cmd($client, $server, $conn, 'GET', 'calm_2');      // record AFTER the throw
send_cmd($client, $server, $conn, 'DEL', 'BOOM_2');      // throw #2 -> main, no second warn
send_cmd($client, $server, $conn, 'SET', 'DANGER_key', '1'); // routing still intact
socket_close($conn);
fclose($client);
usleep(150000);

$danger = @file_get_contents("$dir/danger.log");
$main   = @file_get_contents("$dir/main.log");

var_dump((bool)$main);
// every record survived: the two throws degraded instead of killing the process
var_dump(strpos($main, 'SET calm_1 1') !== false);
var_dump(strpos($main, 'DEL BOOM_1') !== false);
var_dump(strpos($main, 'GET calm_2') !== false);
var_dump(strpos($main, 'DEL BOOM_2') !== false);
// one-time warning carries the exception message summary
var_dump(strpos($main, '"type":"warn"') !== false);
var_dump(strpos($main, 'boom-9') !== false);
var_dump(substr_count($main, '"type":"warn"')); // exactly once, also for throw #2
// rule-style routing of the script still works after the throws
var_dump((bool)$danger);
var_dump(substr_count($danger, 'SET DANGER_key 1')); // the req line only
// the callback saw every single record (CONNECT + 5 req + 5 resp = 11)
var_dump(count(file($ctxfile, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES)));

@unlink("$dir/cb.php"); @unlink("$dir/danger.log"); @unlink("$dir/main.log");
@unlink($ctxfile);
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
bool(true)
int(1)
bool(true)
int(1)
int(11)
done
