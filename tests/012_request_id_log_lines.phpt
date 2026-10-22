--TEST--
mre request_id: every log line (json and text) carries <pid>-<seq>, identical within the request
--INI--
mre.enable=1
--FILE--
<?php
$dir = sys_get_temp_dir() . '/mre_t012_' . getmypid();
@mkdir($dir);
$jsonLog = "$dir/a.json";
$textLog = "$dir/a.text";

// fake redis server so the sniffer produces records (json phase, then text phase)
$server = socket_create(AF_INET, SOCK_STREAM, SOL_TCP);
socket_bind($server, '127.0.0.1', 0);
socket_listen($server);
socket_getsockname($server, $addr, $port);
$port = (int)$port;

ini_set('mre.log_file', $jsonLog);
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

// phase 1: json format
send_cmd($client, $server, $conn, 'SET', 'mre:t12:k1', 'v1');
send_cmd($client, $server, $conn, 'GET', 'mre:t12:k1');

// phase 2: switch to text format + another log file (same process, same request)
ini_set('mre.log_format', 'text');
ini_set('mre.log_file', $textLog);
mre_yaml_dump(); // config refresh

send_cmd($client, $server, $conn, 'DEL', 'mre:t12:k1');
socket_close($conn);
fclose($client);
socket_close($server);
usleep(150000);

$pid = getmypid();

// --- json phase assertions ---
$jlines = file($jsonLog, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES);
$jbad = 0;
$jrids = [];
foreach ($jlines as $l) {
    $j = json_decode($l, true);
    if (!is_array($j) || !isset($j['time'], $j['pid'], $j['request_id'])) { $jbad++; continue; }
    if (!preg_match('/^(\d+)-(\d+)$/', (string)$j['request_id'], $m)) { $jbad++; continue; }
    if ((int)$j['pid'] !== $pid) { $jbad++; continue; }
    if ($j['request_id'] !== $pid . '-' . $m[2]) { $jbad++; continue; }
    $jrids[] = $j['request_id'];
}
var_dump(count($jlines) >= 5);              // CONNECT + 2 req + 2 resp at least
var_dump($jbad);                            // 0 malformed / inconsistent lines
var_dump(count(array_unique($jrids)) === 1); // one request -> one request_id
var_dump(strpos($jrids[0], $pid . '-') === 0); // format <thispid>-<seq>
var_dump((int)substr($jrids[0], strlen($pid) + 1) >= 1);

// --- text phase assertions ---
$tlines = file($textLog, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES);
$tbad = 0;
$trids = [];
foreach ($tlines as $l) {
    if (!preg_match('/(^| )pid=(\d+) request_id=(\d+)-(\d+)( |$)/', $l, $m)) { $tbad++; continue; }
    if ((int)$m[2] !== $pid || (int)$m[3] !== $pid) { $tbad++; continue; }
    $trids[] = $m[3] . '-' . $m[4];
}
var_dump(count($tlines) >= 2);               // DEL req + resp (same conn -> no CONNECT line)
var_dump($tbad);
var_dump(count(array_unique($trids)) === 1);
// text phase shares the SAME request_id as json phase (same process+request)
var_dump($trids[0] === $jrids[0]);

@unlink($jsonLog); @unlink($textLog);
@rmdir($dir);
echo "done\n";
?>
--EXPECT--
bool(true)
int(0)
bool(true)
bool(true)
bool(true)
bool(true)
int(0)
bool(true)
bool(true)
done
