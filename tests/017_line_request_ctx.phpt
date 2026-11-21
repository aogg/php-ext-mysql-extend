--TEST--
mre request context on log lines: json lines carry http_host/http_path/http_scheme/http_ip/headers (same source and rules as $context['headers']) together with extra.cost_ms; CLI lines carry none; HTTPS/REQUEST_SCHEME handling; oversized headers truncate but stay valid json; text lines carry the four http_* keys and never expand headers
--INI--
mre.enable=1
--SKIPIF--
<?php
if (!function_exists('mysqli_connect')) die('skip mysqli extension not loaded');
$h = getenv('MRE_TEST_DB_HOST') ?: 'mysql8';
$m = @mysqli_connect($h, 'root', 'root', 'mre_test');
if (!$m) { die('skip needs a mysql server with mre_test db root/root, set MRE_TEST_DB_HOST to override'); }
mysqli_close($m);
?>
--FILE--
<?php
$host = getenv('MRE_TEST_DB_HOST') ?: 'mysql8';
$dir = sys_get_temp_dir() . '/mre_t017_' . getmypid();
@mkdir($dir);
$jsonLog = "$dir/a.json";
$textLog = "$dir/a.text";

ini_set('mre.log_file', $jsonLog);
ini_set('mre.log_format', 'json');
mre_yaml_dump(); // config refresh

// CLI never populates these: fake a full HTTP request (the extension snapshot
// must be refreshed when $_SERVER is replaced mid-request)
$_SERVER = array(
    'HTTP_HOST'        => 'api.example.test:8080',
    'HTTP_USER_AGENT'  => 'mre-t017/1.0',
    'HTTP_X_FOO_BAR'   => 'baz',
    'CONTENT_TYPE'     => 'application/json',
    'REQUEST_URI'      => '/api/user/list?a=1&b=2',
    'REQUEST_METHOD'   => 'POST',
    'REMOTE_ADDR'      => '203.0.113.9',
    'SERVER_NAME'      => 'api.example.test',
);

$decode = function ($file) {
    $out = array();
    if (!is_file($file)) { return $out; }
    foreach (file($file, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) as $l) {
        $j = json_decode($l, true);
        $out[] = array('raw' => $l, 'j' => $j);
    }
    return $out;
};
// find the mysql req row whose content is $sql (last match wins: prepare and
// execute rows share the SQL)
$find = function ($file, $sql) use ($decode) {
    $found = null;
    foreach ($decode($file) as $row) {
        $j = $row['j'];
        if (is_array($j) && ($j['type'] ?? null) === 'mysql' && ($j['dir'] ?? null) === 'req'
                && ($j['content'] ?? null) === $sql) {
            $found = $j;
        }
    }
    return $found;
};
// every line must be decodable json (truncation must never emit garbage)
$allValidJson = function ($file) use ($decode) {
    foreach ($decode($file) as $row) {
        if (!is_array($row['j'])) { return false; }
    }
    return true;
};

$m = mysqli_connect($host, 'root', 'root', 'mre_test');
if (!$m) { echo "connect failed\n"; exit(1); }

// --- 1) json line carries the full request context ---
$m->query('SELECT 1');
$r = $find($jsonLog, 'SELECT 1');
var_dump($r !== null);
var_dump(($r['http_host'] ?? null) === 'api.example.test:8080'); // port kept
var_dump(($r['http_path'] ?? null) === '/api/user/list');        // no query
var_dump(($r['http_scheme'] ?? null) === 'http');                // no HTTPS, no REQUEST_SCHEME
var_dump(($r['http_ip'] ?? null) === '203.0.113.9');
var_dump(isset($r['headers']) && is_array($r['headers']));
var_dump(($r['headers']['host'] ?? null) === 'api.example.test:8080');
var_dump(($r['headers']['user-agent'] ?? null) === 'mre-t017/1.0');
var_dump(($r['headers']['x-foo-bar'] ?? null) === 'baz');        // HTTP_ stripped, _ -> -
var_dump(($r['headers']['content-type'] ?? null) === 'application/json');
var_dump(isset($r['extra']['cost_ms']) && is_numeric($r['extra']['cost_ms']));
// fields are appended after extra, existing order untouched
var_dump(array_keys($r) === array('time', 'pid', 'request_id', 'type', 'dir', 'content', 'extra',
    'http_host', 'http_path', 'http_scheme', 'http_ip', 'headers'));
var_dump($allValidJson($jsonLog));

// --- 2) HTTPS non-empty and not "off" -> https; REQUEST_SCHEME lowered ---
$_SERVER['HTTPS'] = 'on';
$_SERVER['REQUEST_URI'] = '/secure?x=1';
$m->query('SELECT 2');
$r = $find($jsonLog, 'SELECT 2');
var_dump(($r['http_scheme'] ?? null) === 'https');
var_dump(($r['http_path'] ?? null) === '/secure');
unset($_SERVER['HTTPS']);
$_SERVER['REQUEST_SCHEME'] = 'HTTPS';
$m->query('SELECT 3');
$r = $find($jsonLog, 'SELECT 3');
var_dump(($r['http_scheme'] ?? null) === 'https'); // lowered REQUEST_SCHEME
unset($_SERVER['REQUEST_SCHEME']);

// --- 3) oversized headers: truncated, json stays valid ---
$_SERVER['HTTP_X_BIG'] = str_repeat('a', 3000);
$m->query('SELECT 4');
$r = $find($jsonLog, 'SELECT 4');
var_dump(isset($r['headers']['truncated']) && $r['headers']['truncated'] === true);
var_dump(($r['headers']['host'] ?? null) === 'api.example.test:8080'); // kept: budget filled later
var_dump($allValidJson($jsonLog));
unset($_SERVER['HTTP_X_BIG']);

// --- 3b) pathological oversized line: still complete, valid json ---
// 3000 control chars expand 6x when escaped; prepare+execute then carries
// content/prepared.sql/sql_full -> over MRE_LOG_LINE_MAX (16 KiB).
$_SERVER = array(
    'HTTP_HOST'   => 'api.example.test:8080',
    'REQUEST_URI' => '/trunc',
    'REMOTE_ADDR' => '203.0.113.9',
);
$big = "SELECT ? AS p, '" . str_repeat("\x01", 3000) . "' AS c";
$st = $m->prepare($big);
$i2 = 1;
$st->bind_param('i', $i2);
$st->execute();
$st->close();
$bigRows = array();
$bigPrefix = "SELECT ? AS p, '";
foreach ($decode($jsonLog) as $row) {
    $j = $row['j'];
    // the over-long row keeps only a truncated content, so match the prefix
    if (is_array($j) && strpos((string)($j['content'] ?? ''), $bigPrefix) === 0) { $bigRows[] = $j; }
}
$hasTrunc = false;
$truncOk = false;
foreach ($bigRows as $j) {
    if (!empty($j['truncated'])) {
        $hasTrunc = true;
        $truncOk = ($j['extra'] === null)                       // detail dropped for size
            && (($j['http_host'] ?? null) === 'api.example.test:8080')  // context kept
            && is_string($j['content']);
    }
}
var_dump(count($bigRows) >= 2);   // prepare + execute req rows
var_dump($hasTrunc);              // at least one over-long row rebuilt
var_dump($truncOk);               // valid compact shape with context
var_dump($allValidJson($jsonLog)); // no half-written string anywhere

// --- 4) CLI (no HTTP $_SERVER): none of the new keys show up ---
$_SERVER = array();
$m->query('SELECT 5');
$r = $find($jsonLog, 'SELECT 5');
var_dump($r !== null);
var_dump(!isset($r['http_host'], $r['http_path'], $r['http_scheme'], $r['http_ip'], $r['headers']));
var_dump(isset($r['extra']['cost_ms'])); // the old fields are untouched
var_dump($allValidJson($jsonLog));

// --- 5) text format: http_* appended as k="v"; headers never expanded ---
ini_set('mre.log_format', 'text');
ini_set('mre.log_file', $textLog);
mre_yaml_dump(); // config refresh
$_SERVER = array(
    'HTTP_HOST'   => 'api.example.test:8080',
    'REQUEST_URI' => '/api/user/list?a=1',
    'REMOTE_ADDR' => '203.0.113.9',
);
$m->query('SELECT 6');
$tl = '';
foreach (file($textLog, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) as $l) {
    if (strpos($l, 'type=mysql dir=req') !== false && strpos($l, 'SELECT 6') !== false) { $tl = $l; break; }
}
var_dump($tl !== '');
var_dump(strpos($tl, 'http_host="api.example.test:8080"') !== false);
var_dump(strpos($tl, 'http_path="/api/user/list"') !== false);
var_dump(strpos($tl, 'http_scheme="http"') !== false);
var_dump(strpos($tl, 'http_ip="203.0.113.9"') !== false);
var_dump(strpos($tl, 'headers=') === false);
// http_* comes after the extra tail (cost_ms is the last extra field)
var_dump((bool)preg_match('/cost_ms=[0-9]+\.[0-9]{3} http_host=/', $tl));

// --- 6) CLI text line stays exactly as before (no context tail) ---
ini_set('mre.log_file', "$dir/b.text");
$_SERVER = array();
mre_yaml_dump();
$m->query('SELECT 7');
$tl = '';
foreach (file("$dir/b.text", FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) as $l) {
    if (strpos($l, 'SELECT 7') !== false) { $tl = $l; break; }
}
var_dump($tl !== '');
var_dump(strpos($tl, 'http_host=') === false);
var_dump((bool)preg_match('/cost_ms=[0-9]+\.[0-9]{3}$/', $tl));

// --- cleanup ---
$m->close();
ini_set('mre.enable', '0');
mre_yaml_dump(); // stop logging before deleting files
foreach (array($jsonLog, $textLog, "$dir/b.text") as $f) { @unlink($f); }
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
