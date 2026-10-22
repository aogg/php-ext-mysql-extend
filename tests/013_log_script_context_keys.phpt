--TEST--
mre log_script $context carries pid / request_id / script; script points at the entry script
--INI--
mre.enable=1
--FILE--
<?php
$dir = sys_get_temp_dir() . '/mre_t013_' . getmypid();
@mkdir($dir);
$ctxfile = "$dir/ctx.json";

// callback: dump only the keys under test for every record, return null (-> main log)
file_put_contents("$dir/cb.php", <<<PHP
<?php
file_put_contents('$ctxfile', json_encode([
    'pid'        => \$context['pid'],
    'request_id' => \$context['request_id'],
    'script'     => \$context['script'],
]) . "\n", FILE_APPEND);
return null;
PHP);

// the entry script this test runs as (run-tests executes the .phpt.php copy)
$entry = isset($_SERVER['SCRIPT_FILENAME']) && $_SERVER['SCRIPT_FILENAME'] !== ''
    ? $_SERVER['SCRIPT_FILENAME'] : get_included_files()[0];
$entryReal = realpath($entry);

// fake redis server so the sniffer produces records
$server = socket_create(AF_INET, SOCK_STREAM, SOL_TCP);
socket_bind($server, '127.0.0.1', 0);
socket_listen($server);
socket_getsockname($server, $addr, $port);
$port = (int)$port;

ini_set('mre.log_file', "$dir/main.log");
ini_set('mre.log_script', "$dir/cb.php");
ini_set('mre.redis_port', (string)(int)$port);
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

send_cmd($client, $server, $conn, 'SET', 'mre:t13:k', 'v');
send_cmd($client, $server, $conn, 'GET', 'mre:t13:k');
socket_close($conn);
fclose($client);
socket_close($server);
usleep(150000);

$pid = getmypid();

// --- $context assertions (one json line per record) ---
$lines = file($ctxfile, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES);
$bad = 0;
$rids = [];
$scripts = [];
foreach ($lines as $l) {
    $c = json_decode($l, true);
    if (!is_array($c) || !isset($c['pid'], $c['request_id'], $c['script'])) { $bad++; continue; }
    if ((int)$c['pid'] !== $pid) { $bad++; continue; }
    if (!preg_match('/^(\d+)-(\d+)$/', (string)$c['request_id'], $m)
            || (int)$m[1] !== $pid) { $bad++; continue; }
    if (!is_string($c['script']) || $c['script'] === ''
            || realpath($c['script']) !== $entryReal) { $bad++; continue; }
    $rids[] = $c['request_id'];
    $scripts[] = $c['script'];
}
var_dump(count($lines) >= 4);                 // CONNECT + 2 req + 2 resp at least
var_dump($bad);                               // every record passed all key checks
var_dump(count(array_unique($rids)) === 1);   // one request -> one request_id
var_dump($rids !== [] && $rids[0] === $pid . '-1'); // CLI: first (and only) request is <pid>-1
var_dump(count(array_unique($scripts)) === 1);

// --- the log lines carry the SAME request_id the callback saw ---
$mlog = file("$dir/main.log", FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES);
$lrids = [];
foreach ($mlog as $l) {
    $j = json_decode($l, true);
    if (is_array($j) && isset($j['request_id'])) { $lrids[] = $j['request_id']; }
}
var_dump(count($lrids) >= 4);
var_dump(count(array_unique($lrids)) === 1);
var_dump($lrids !== [] && $lrids[0] === $rids[0]);

@unlink("$dir/cb.php"); @unlink("$dir/main.log");
@unlink($ctxfile);
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
bool(true)
bool(true)
done
