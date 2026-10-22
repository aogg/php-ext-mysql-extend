--TEST--
mre log_script: a callback with a parse error (ParseError) degrades every
record to the main log with exactly one warning, process survives
(BUG-2 follow-up: ParseError is a Throwable, caught by the wrapper)
--INI--
mre.enable=1
--FILE--
<?php
$dir = sys_get_temp_dir() . '/mre_t010_' . getmypid();
@mkdir($dir);

// broken callback: unterminated string literal -> ParseError on every include
file_put_contents("$dir/bad.php", "<?php\nreturn '$dir/broken.log\n");

// fake redis server so the sniffer produces records
$server = socket_create(AF_INET, SOCK_STREAM, SOL_TCP);
socket_bind($server, '127.0.0.1', 0);
socket_listen($server);
socket_getsockname($server, $addr, $port);
$port = (int)$port;

ini_set('mre.log_file', "$dir/main.log");
ini_set('mre.log_script', "$dir/bad.php");
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

// every record makes the wrapper include the broken script -> ParseError
send_cmd($client, $server, $conn, 'SET', 'calm_1', '1');
send_cmd($client, $server, $conn, 'DEL', 'BROKEN_1'); // would have routed
send_cmd($client, $server, $conn, 'GET', 'calm_2');
socket_close($conn);
fclose($client);
usleep(150000);

$main = @file_get_contents("$dir/main.log");

// the process survived and every record degraded to the main log
var_dump((bool)$main);
var_dump(strpos($main, 'SET calm_1 1') !== false);
var_dump(strpos($main, 'DEL BROKEN_1') !== false);
var_dump(strpos($main, 'GET calm_2') !== false);
// one-time warning carrying the parse error summary (ParseError message)
var_dump(strpos($main, '"type":"warn"') !== false);
var_dump(strpos($main, 'syntax error') !== false);
var_dump(substr_count($main, '"type":"warn"')); // exactly once, not per record
// the broken script never produced its routed file
var_dump(@file_exists("$dir/broken.log"));

@unlink("$dir/bad.php"); @unlink("$dir/broken.log"); @unlink("$dir/main.log");
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
int(1)
bool(false)
done
