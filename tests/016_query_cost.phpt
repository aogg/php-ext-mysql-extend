--TEST--
mre query timing: mysql req rows (query/prepare/execute) carry time/time_end/cost_ms, SLEEP duration window, ERROR rows still timed, resp/redis rows carry none, text tail format, router $context exposure, gtx deferred COMMIT time_end==time && cost_ms==0
--INI--
mre.enable=1
--SKIPIF--
<?php
if (!function_exists('mysqli_connect')) die('skip mysqli extension not loaded');
if (!function_exists('socket_create')) die('skip sockets extension not loaded');
if (!function_exists('mre_gtx_begin')) die('skip mre gtx functions not available');
$h = getenv('MRE_TEST_DB_HOST') ?: 'mysql8';
$m = @mysqli_connect($h, 'root', 'root', 'mre_test');
if (!$m) { die('skip needs a mysql server with mre_test db root/root, set MRE_TEST_DB_HOST to override'); }
mysqli_close($m);
--FILE--
<?php
$host = getenv('MRE_TEST_DB_HOST') ?: 'mysql8';
$dir = sys_get_temp_dir() . '/mre_t016_' . getmypid();
@mkdir($dir);
$jsonLog = "$dir/a.json";
$textLog = "$dir/a.text";
$ctxfile = "$dir/ctx.jsonl";
$table = 'mre_t016';
$tsfmt = '/^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}[+-]\d{2}:\d{2}$/';

mysqli_report(MYSQLI_REPORT_OFF);

// fake redis server so real redis rows share the log (negative-scan targets)
$server = socket_create(AF_INET, SOCK_STREAM, SOL_TCP);
socket_bind($server, '127.0.0.1', 0);
socket_listen($server);
socket_getsockname($server, $addr, $port);
$port = (int)$port;

ini_set('mre.log_file', $jsonLog);
ini_set('mre.log_format', 'json');
ini_set('mre.redis_port', (string)$port);
mre_yaml_dump(); // config refresh

$client = stream_socket_client("tcp://127.0.0.1:$port");
stream_set_blocking($client, false);
$conn = false;
$cmd = function ($c, ...$args) use ($client, $server, &$conn) {
    $out = "*" . (count($args) + 1) . "\r\n\$" . strlen($c) . "\r\n$c\r\n";
    foreach ($args as $a) { $out .= "\$" . strlen($a) . "\r\n$a\r\n"; }
    fwrite($client, $out);
    usleep(80000);
    if ($conn === false) { $conn = socket_accept($server); }
    socket_read($conn, 8192);
    socket_write($conn, "+OK\r\n");
    fread($client, 8192);
};
$cmd('SET', 'mre:t16:k', 'v');
$cmd('GET', 'mre:t16:k');

// find a logged json row by type/dir/content; $last=true returns the last match;
// resp rows carry OK / ERROR (not the SQL) as content
$find = function ($content, $ty = 'mysql', $dr = 'req', $last = false, $file = null) use ($jsonLog) {
    $found = null;
    foreach (file($file ?: $jsonLog, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) as $l) {
        $j = json_decode($l, true);
        if (is_array($j) && ($j['type'] ?? null) === $ty && ($j['dir'] ?? null) === $dr
                && ($j['content'] ?? null) === $content) {
            $found = $j;
            if (!$last) { return $found; }
        }
    }
    return $found;
};
// timing block assertions shared by every mysql req row
$timing_ok = function ($j) use ($tsfmt) {
    return is_array($j)
        && isset($j['time'], $j['extra']['time_end'], $j['extra']['cost_ms'])
        && is_string($j['extra']['time_end'])
        && preg_match($tsfmt, $j['time'])
        && preg_match($tsfmt, $j['extra']['time_end'])
        && strtotime($j['time']) <= strtotime($j['extra']['time_end'])
        && is_numeric($j['extra']['cost_ms'])
        && (float)$j['extra']['cost_ms'] >= 0.0;
};

$m = mysqli_connect($host, 'root', 'root', 'mre_test');
if (!$m) { echo "connect failed\n"; exit(1); }
$m->query("DROP TABLE IF EXISTS $table");
$m->query("CREATE TABLE $table (id INT, name VARCHAR(32)) ENGINE=InnoDB");

// --- 1) query row: time / time_end / cost_ms present, formats, ordering ---
$m->query('SELECT 1');
$q = $find('SELECT 1');
var_dump($q !== null);
var_dump($timing_ok($q)); // keys + ISO8601 formats + strtotime(time)<=strtotime(time_end)

// --- 2) prepare + execute rows carry the same timing block ---
$sql = "INSERT INTO $table (id, name) VALUES (?, ?)";
$st = $m->prepare($sql);
$i = 7; $s = 'bob';
$st->bind_param('is', $i, $s);
$st->execute();
$st->close();
$pr = $find($sql);
$ex = $find($sql, 'mysql', 'req', true);
var_dump($pr !== null && $timing_ok($pr)); // cmd=prepare row
var_dump($ex !== null && $timing_ok($ex)); // cmd=execute row
var_dump($ex['extra']['cmd'] === 'execute' && $pr['extra']['cmd'] === 'prepare');

// --- 3) SLEEP(0.2): cost_ms inside the 150..500 window ---
$m->query('SELECT SLEEP(0.2) AS s');
$sl = $find('SELECT SLEEP(0.2) AS s');
var_dump($timing_ok($sl));
var_dump((float)$sl['extra']['cost_ms'] >= 150.0 && (float)$sl['extra']['cost_ms'] < 500.0);

// --- 4) failing query: req row still fully timed, ERROR resp row untimed ---
$m->query("SELECT * FROM ${table}_no_such_table");
$bad = $find("SELECT * FROM ${table}_no_such_table");
$badr = $find('ERROR', 'mysql', 'resp', true);
var_dump($timing_ok($bad));                       // ERROR path keeps req timing
var_dump($badr !== null && isset($badr['extra']['error_code']));
var_dump(!isset($badr['extra']['time_end'], $badr['extra']['cost_ms']));

// --- 5) negative scan: ONLY mysql req rows carry the timing fields ---
$redisRows = 0; $respRows = 0; $timingRows = 0; $badCarrier = 0;
foreach (file($jsonLog, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) as $l) {
    $j = json_decode($l, true);
    if (!is_array($j)) { continue; }
    if (($j['type'] ?? '') === 'redis') { $redisRows++; }
    if (($j['type'] ?? '') === 'mysql' && ($j['dir'] ?? '') === 'resp') { $respRows++; }
    if (($j['type'] ?? '') === 'mysql' && ($j['dir'] ?? '') === 'req') { $timingRows++; }
    if (isset($j['extra']['time_end'], $j['extra']['cost_ms'])
            && !(($j['type'] ?? '') === 'mysql' && ($j['dir'] ?? '') === 'req')) {
        $badCarrier++;
    }
}
var_dump($redisRows >= 4);   // redis req+resp present (CONNECT + SET/GET x2 sides)
var_dump($respRows >= 4);    // mysql resp rows present
var_dump($timingRows >= 6);  // drop/create/select/sleep/bad + prepare + execute
var_dump($badCarrier === 0); // resp / redis rows never carry time_end+cost_ms

// --- 6) text format: mysql req line ends with time_end=... cost_ms=N.NNN ---
ini_set('mre.log_format', 'text');
ini_set('mre.log_file', $textLog);
mre_yaml_dump(); // config refresh

$m->query('SELECT SLEEP(0.05) AS s');
$tlines = file($textLog, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES);
$tl = '';
foreach ($tlines as $l) {
    if (strpos($l, 'type=mysql dir=req') !== false
            && strpos($l, 'SELECT SLEEP(0.05) AS s') !== false) { $tl = $l; break; }
}
var_dump($tl !== '');
var_dump((bool)preg_match('/time_end=\S+ cost_ms=[0-9]+\.[0-9]{3}$/', $tl));
if (preg_match('/cost_ms=([0-9]+\.[0-9]{3})$/', $tl, $mm)) {
    var_dump((float)$mm[1] >= 30.0 && (float)$mm[1] < 500.0);
} else {
    var_dump(false);
}

// --- 7) router $context['extra']: time_end is string, cost_ms is float, mysql req only ---
ini_set('mre.log_format', 'json');
ini_set('mre.log_file', $jsonLog . '.2');
ini_set('mre.log_script', "$dir/cb.php");
file_put_contents("$dir/cb.php", <<<PHP
<?php
\$extra = isset(\$context['extra']) && is_array(\$context['extra']) ? \$context['extra'] : [];
file_put_contents('$ctxfile', json_encode([
    'type'         => \$context['type'],
    'dir'          => \$context['dir'],
    'content'      => \$context['content'],
    'time_end_set' => isset(\$extra['time_end']),
    'time_end_typ' => isset(\$extra['time_end']) ? gettype(\$extra['time_end']) : null,
    'cost_ms_set'  => isset(\$extra['cost_ms']),
    'cost_ms_typ'  => isset(\$extra['cost_ms']) ? gettype(\$extra['cost_ms']) : null,
]) . "\\n", FILE_APPEND);
return null;
PHP);
mre_yaml_dump(); // config refresh

$cmd('SET', 'mre:t16:k2', 'v2');   // redis record through the callback too
$m->query('SELECT 42');
$st = $m->prepare("SELECT name FROM $table WHERE id = ?");
$st->bind_param('i', $i);
$st->execute();
$st->close();
socket_close($conn); fclose($client); socket_close($server);
usleep(150000);

$creq = null; $cresp = null; $cred = null;
foreach (file($ctxfile, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) as $l) {
    $c = json_decode($l, true);
    if (!is_array($c)) { continue; }
    if ($c['content'] === 'SELECT 42' && $c['type'] === 'mysql' && $c['dir'] === 'req') { $creq = $c; }
    // resp rows carry OK/ERROR, not the SQL: first mysql OK resp = SELECT 42's
    if ($cresp === null && $c['content'] === 'OK' && $c['type'] === 'mysql' && $c['dir'] === 'resp') { $cresp = $c; }
    if ($c['content'] === 'SET mre:t16:k2 v2' && $c['type'] === 'redis') { $cred = $c; }
}
var_dump($creq !== null
    && $creq['time_end_set'] === true && $creq['time_end_typ'] === 'string'
    && $creq['cost_ms_set'] === true && $creq['cost_ms_typ'] === 'double');
var_dump($cresp !== null && $cresp['time_end_set'] === false && $cresp['cost_ms_set'] === false);
var_dump($cred !== null && $cred['time_end_set'] === false && $cred['cost_ms_set'] === false);

// --- 8) gtx deferred COMMIT: time_end === time, cost_ms === 0 ---
ini_set('mre.log_script', '');
ini_set('mre.log_file', $jsonLog); // phase 7 pointed the log elsewhere
ini_set('mre.gtx', '1');
ini_set('mre.state_dir', "$dir/gtx");
@mkdir("$dir/gtx");
mre_yaml_dump(); // config refresh

var_dump(mre_gtx_begin());
$m->query('START TRANSACTION');
$m->query("INSERT INTO $table (id, name) VALUES (99, 'gtx')");
$m->query('COMMIT'); // intercepted and deferred: nothing sent yet
$gc = $find('COMMIT');
var_dump($gc !== null && ($gc['extra']['cmd'] ?? '') === 'query');
var_dump($gc !== null && $gc['extra']['time_end'] === $gc['time']); // deferred: end == start
var_dump($gc !== null && (float)$gc['extra']['cost_ms'] === 0.0);   // deferred: zero duration

var_dump(mre_gtx_commit());
mre_gtx_status(); // refresh the per-process snapshot (same as 006's wait loop)
$m->query('SELECT 1'); // entry hook sends the real COMMIT now
var_dump(mre_gtx_status()['state'] === 'none');
$m->query("DELETE FROM $table WHERE id = 99");

// --- cleanup ---
$m->query("DROP TABLE IF EXISTS $table");
$m->close();
ini_set('mre.enable', '0');
mre_yaml_dump(); // config refresh: stop logging before deleting files
@unlink("$dir/cb.php");
foreach ([$jsonLog, $jsonLog . '.2', $textLog, $ctxfile] as $f) { @unlink($f); }
array_map('unlink', glob("$dir/gtx/*") ?: []);
@rmdir("$dir/gtx");
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
