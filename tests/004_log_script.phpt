--TEST--
mre log_script decides the log path, degrades on null/exception
--INI--
mre.enable=1
--FILE--
<?php
$dir = sys_get_temp_dir() . '/mre_t004_' . getmypid();
@mkdir($dir);
$ctxfile = "$dir/ctx.json";

// callback: route DANGER records, throw on BOOM, null otherwise;
// also records the $context it received
file_put_contents("$dir/cb.php", <<<PHP
<?php
file_put_contents('$ctxfile', json_encode([
    'type'       => \$context['type'],
    'dir'        => \$context['dir'],
    'content'    => \$context['content'],
    'uri'        => \$context['uri'],
    'method'     => \$context['method'],
    'session_id' => \$context['session_id'],
    'headers'    => \$context['headers'],
    'post'       => \$context['post'],
    'get'        => \$context['get'],
    'cookie'     => \$context['cookie'],
]) . "\n", FILE_APPEND);
if (strpos(\$context['content'], 'DANGER') !== false) {
    return '$dir/danger.log';
}
if (strpos(\$context['content'], 'BOOM') !== false) {
    throw new RuntimeException('script exploded');
}
return null;
PHP);

// fake redis server so the sniffer produces records
// raw-socket fake server: only the CLIENT side is a PHP stream
$server = socket_create(AF_INET, SOCK_STREAM, SOL_TCP);
socket_bind($server, '127.0.0.1', 0);
socket_listen($server);
socket_getsockname($server, $addr, $port);
$port = (int)$port;

ini_set('mre.log_file', "$dir/main.log");
ini_set('mre.log_script', "$dir/cb.php");
ini_set('mre.redis_port', (string)(int)$port);
mre_yaml_dump(); // config refresh

// make the request context visible to the callback
$_SERVER['REQUEST_URI']    = '/mre/test?x=1';
$_SERVER['REQUEST_METHOD'] = 'GET';
$_SERVER['HTTP_X_MRE_PROBE'] = 'probe-value';
$_POST['flag'] = 'danger';
$_GET['q'] = 'mre';
$_COOKIE['c'] = 'mrecookie';

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

send_cmd($client, $server, $conn, 'SET', 'DANGER_key', '1'); // script -> danger.log
send_cmd($client, $server, $conn, 'GET', 'calm');            // script null -> main log
send_cmd($client, $server, $conn, 'DEL', 'BOOM_key');        // script throws -> main log
socket_close($conn);
fclose($client);
socket_close($server);
usleep(150000);

$danger = @file_get_contents("$dir/danger.log");
$main = @file_get_contents("$dir/main.log");

var_dump((bool)$danger);
var_dump(strpos($danger, 'SET DANGER_key 1') !== false);

// GET calm: script returned null -> main log
var_dump(strpos($main, 'GET calm') !== false);
// DEL BOOM: script threw -> degraded to main log, process alive
var_dump(strpos($main, 'DEL BOOM_key') !== false);
// one-time warning with the exception summary went to the main log
var_dump(strpos($main, 'script exploded') !== false);
var_dump(strpos($main, '"type":"warn"') !== false);

// the context the script received (one json line per record)
$lines = file($ctxfile, FILE_IGNORE_NEW_LINES);
$setline = null;
$getline = null;
foreach ($lines as $l) {
    $c = json_decode($l, true);
    if ($c['content'] === 'SET DANGER_key 1') $setline = $c;
    if ($c['content'] === 'GET calm') $getline = $c;
}
var_dump($setline !== null);
var_dump($setline['type'], $setline['dir']);
var_dump($setline['uri'], $setline['method']);
var_dump($setline['session_id'] === '');
var_dump($setline['headers']);
var_dump($setline['post']);
var_dump($getline !== null);      // script ran for the null-return record too
var_dump($getline['get']);

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
string(5) "redis"
string(3) "req"
string(13) "/mre/test?x=1"
string(3) "GET"
bool(true)
array(1) {
  ["x-mre-probe"]=>
  string(11) "probe-value"
}
array(1) {
  ["flag"]=>
  string(6) "danger"
}
bool(true)
array(1) {
  ["q"]=>
  string(3) "mre"
}
done
