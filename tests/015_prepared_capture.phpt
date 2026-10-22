--TEST--
mre log structure: extra.cmd (query/prepare/execute), extra.prepared {sql,params,truncated}, extra.sql_full literals, omission without binds, text format fields, router-script $context exposure
--INI--
mre.enable=1
--SKIPIF--
<?php
if (!function_exists('mysqli_connect')) die('skip mysqli extension not loaded');
if (!class_exists('PDO') || !extension_loaded('pdo_mysql')) die('skip pdo_mysql extension not loaded');
$h = getenv('MRE_TEST_DB_HOST') ?: 'mysql8';
$m = @mysqli_connect($h, 'root', 'root', 'mre_test');
if (!$m) { die('skip needs a mysql server with mre_test db root/root, set MRE_TEST_DB_HOST to override'); }
mysqli_close($m);
--FILE--
<?php
$host = getenv('MRE_TEST_DB_HOST') ?: 'mysql8';
$dir = sys_get_temp_dir() . '/mre_t015_' . getmypid();
@mkdir($dir);
$jsonLog = "$dir/a.json";
$textLog = "$dir/a.text";
$ctxfile = "$dir/ctx.jsonl";
$table = 'mre_t015';

// log_script callback: dump the cmd/prepared/sql_full parts of $context for
// every record (one json per line); return null -> record still hits main log
file_put_contents("$dir/cb.php", <<<PHP
<?php
\$dump = ['content' => \$context['content']];
\$extra = isset(\$context['extra']) && is_array(\$context['extra']) ? \$context['extra'] : [];
if (isset(\$extra['cmd'])) { \$dump['cmd'] = \$extra['cmd']; }
if (isset(\$extra['prepared'])) {
    \$dump['prepared_sql'] = \$extra['prepared']['sql'];
    \$dump['params_is_array'] = is_array(\$extra['prepared']['params']);
    \$dump['params'] = \$extra['prepared']['params'];
    \$dump['params_types'] = array_map('gettype', \$extra['prepared']['params']);
    if (isset(\$extra['prepared']['truncated'])) { \$dump['truncated'] = \$extra['prepared']['truncated']; }
}
if (isset(\$extra['sql_full'])) { \$dump['sql_full'] = \$extra['sql_full']; }
file_put_contents('$ctxfile', json_encode(\$dump) . "\\n", FILE_APPEND);
return null;
PHP);

ini_set('mre.log_file', $jsonLog);
ini_set('mre.log_script', "$dir/cb.php");
mre_yaml_dump(); // config refresh

$m = mysqli_connect($host, 'root', 'root', 'mre_test');
if (!$m) { echo "connect failed\n"; exit(1); }
$m->query("DROP TABLE IF EXISTS $table");
$m->query("CREATE TABLE $table (id INT, name VARCHAR(64), d DOUBLE, n INT NULL) ENGINE=InnoDB");

// find a logged record by content/dir; $last=true returns the last match
// (prepare and execute rows share the same content)
$find = function ($content, $dr = 'req', $last = false) use ($jsonLog) {
    $found = null;
    foreach (file($jsonLog, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) as $l) {
        $j = json_decode($l, true);
        if (is_array($j) && ($j['dir'] ?? null) === $dr
                && ($j['content'] ?? null) === $content) {
            $found = $j;
            if (!$last) { return $found; }
        }
    }
    return $found;
};

// --- 1) plain query row: cmd=query, no prepared/sql_full ---
$m->query('SELECT 1');
$q = $find('SELECT 1');
var_dump($q !== null && $q['extra']['cmd'] === 'query');
var_dump(!isset($q['extra']['prepared'], $q['extra']['sql_full']));

// --- 2) prepare + execute with binds: int / string-with-quote / double / null ---
$sql4 = "INSERT INTO $table (id, name, d, n) VALUES (?, ?, ?, ?)";
$st = $m->prepare($sql4);
$i = 42; $s = "it's bob"; $d = 3.25; $n = null;
$st->bind_param('isds', $i, $s, $d, $n);
$st->execute();
$st->close();

$pr = $find($sql4);               // first match = prepare row
$ex = $find($sql4, 'req', true);  // last match = execute row
var_dump($pr !== null && $pr['extra']['cmd'] === 'prepare');
var_dump($ex !== null && $ex['extra']['cmd'] === 'execute');
var_dump(isset($ex['extra']['stmt_id']) && $ex['extra']['stmt_id'] === $pr['extra']['stmt_id']);
var_dump($ex['extra']['prepared']['sql'] === $sql4 && $ex['content'] === $sql4);
var_dump($ex['extra']['prepared']['params'] === [42, "it's bob", 3.25, null]);
var_dump(!isset($ex['extra']['prepared']['truncated']));
var_dump($ex['extra']['sql_full'] === "INSERT INTO $table (id, name, d, n) VALUES (42, 'it''s bob', 3.25, NULL)");

// --- 3) execute without bound params: prepared/sql_full omitted ---
$sqlnb = "SELECT COUNT(*) c FROM $table";
$st = $m->prepare($sqlnb);
$st->execute();
$st->close();
$exnb = $find($sqlnb, 'req', true);
var_dump($exnb !== null && $exnb['extra']['cmd'] === 'execute');
var_dump(!isset($exnb['extra']['prepared'], $exnb['extra']['sql_full']));

// --- 4) more than 32 binds: params capped at 32 + truncated=true ---
$marks = rtrim(str_repeat('?,', 33), ',');
$sql33 = "SELECT id FROM $table WHERE id IN ($marks)";
$st = $m->prepare($sql33);
$types = str_repeat('i', 33);
$args = [];
for ($k = 1; $k <= 33; $k++) { $args[] = $k; }
$st->bind_param($types, ...$args);
$st->execute();
$st->close();
$ex33 = $find($sql33, 'req', true);
var_dump($ex33 !== null && $ex33['extra']['prepared']['truncated'] === true);
var_dump(count($ex33['extra']['prepared']['params']) === 32);
var_dump($ex33['extra']['prepared']['params'][31] === 32);
var_dump($ex33['extra']['sql_full'] === "SELECT id FROM $table WHERE id IN (" . implode(',', range(1, 32)) . ",?)");

// --- 5) PDO native prepares hit the same capture (7.4 binds params as STR) ---
$pdo = new PDO("mysql:host=$host;dbname=mre_test", 'root', 'root',
    [PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION, PDO::ATTR_EMULATE_PREPARES => false]);
$sqlp = "SELECT name FROM $table WHERE id = ? AND name = ?";
$q = $pdo->prepare($sqlp);
$q->execute([42, "it's bob"]);
$rows = $q->fetchAll(PDO::FETCH_ASSOC);
$q->closeCursor();
$exp = $find($sqlp, 'req', true);
var_dump(count($rows) === 1);
var_dump($exp !== null && $exp['extra']['cmd'] === 'execute');
var_dump($exp['extra']['prepared']['params'] === ['42', "it's bob"]);
var_dump($exp['extra']['sql_full'] === "SELECT name FROM $table WHERE id = '42' AND name = 'it''s bob'");
$pdo = null;

// --- 6) resp rows never carry cmd ---
$nocmd = true;
foreach (file($jsonLog, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) as $l) {
    $j = json_decode($l, true);
    if (is_array($j) && ($j['dir'] ?? null) === 'resp' && isset($j['extra']['cmd'])) { $nocmd = false; break; }
}
var_dump($nocmd);

// --- 7) text format: cmd=/params=[...]/sql_full="..." appended ---
ini_set('mre.log_format', 'text');
ini_set('mre.log_file', $textLog);
mre_yaml_dump(); // config refresh

$st = $m->prepare($sql4);
$st->bind_param('isds', $i, $s, $d, $n);
$st->execute();
$st->close();

$tlines = file($textLog, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES);
$tl = '';
foreach ($tlines as $l) { if (strpos($l, ' cmd=execute ') !== false) { $tl = $l; break; } }
var_dump($tl !== '');
var_dump(strpos($tl, 'params=[42,"it\'s bob",3.25,null]') !== false);
var_dump(strpos($tl, 'sql_full="INSERT INTO ' . $table . ' (id, name, d, n) VALUES (42, \'it\'\'s bob\', 3.25, NULL)"') !== false);
var_dump(strpos($tl, 'params_truncated') === false);
var_dump(count(preg_grep('/ cmd=prepare /', $tlines)) > 0);

// --- 8) router-script $context: cmd / prepared (sql + params PHP array) / sql_full ---
$ctxEx = []; $ctxPrep = []; $ctxNb = [];
foreach (file($ctxfile, FILE_IGNORE_NEW_LINES | FILE_SKIP_EMPTY_LINES) as $l) {
    $c = json_decode($l, true);
    if (!is_array($c)) { continue; }
    if (($c['content'] ?? null) === $sql4) {
        if (($c['cmd'] ?? '') === 'execute') { $ctxEx[] = $c; }
        if (($c['cmd'] ?? '') === 'prepare') { $ctxPrep[] = $c; }
    }
    if (($c['content'] ?? null) === $sqlnb && ($c['cmd'] ?? '') === 'execute') { $ctxNb[] = $c; }
}
$ce = $ctxEx[0] ?? null;
var_dump($ce !== null && $ce['prepared_sql'] === $sql4);
var_dump($ce['params_is_array'] === true);
var_dump($ce['params'] === [42, "it's bob", 3.25, null]);
var_dump($ce['params_types'] === ['integer', 'string', 'double', 'NULL']);
var_dump($ce['sql_full'] === "INSERT INTO $table (id, name, d, n) VALUES (42, 'it''s bob', 3.25, NULL)");
var_dump(!isset($ce['truncated']));
var_dump(!isset($ctxPrep[0]['prepared_sql']));
var_dump(!isset($ctxNb[0]['prepared_sql'], $ctxNb[0]['sql_full']));

// --- cleanup ---
$m->query("DROP TABLE IF EXISTS $table");
$m->close();
@unlink("$dir/cb.php"); @unlink($jsonLog); @unlink($textLog); @unlink($ctxfile);
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
done
