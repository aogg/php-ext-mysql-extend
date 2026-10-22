--TEST--
mre_log_router.php core behaviour: query/write/redis routing, URL base, project-root detection
--SKIPIF--
<?php
if (!file_exists(__DIR__ . '/../scripts/mre_log_router.php')) die('skip scripts/mre_log_router.php not present');
?>
--FILE--
<?php
$router = __DIR__ . '/../scripts/mre_log_router.php';

$base = sys_get_temp_dir() . '/mre_t014_' . getmypid();
$proj = "$base/proj";                 // fake project root: composer.json marker
@mkdir("$proj/public", 0777, true);
file_put_contents("$proj/composer.json", '{}');
$proj2 = "$base/proj_tp5";            // ThinkPHP5 style root: application/ marker, public entry
@mkdir("$proj2/public", 0777, true);
@mkdir("$proj2/application", 0777, true);
$nr = "$base/noroot";                 // plain dir without any project marker
@mkdir($nr, 0777, true);
$logDir = "$proj/runtime/mre-logs";
$logDir2 = "$proj2/runtime/mre-logs";

$origUri = isset($_SERVER['REQUEST_URI']) ? $_SERVER['REQUEST_URI'] : null;
$origSfn = isset($_SERVER['SCRIPT_FILENAME']) ? $_SERVER['SCRIPT_FILENAME'] : null;

// include the router with a controlled $context/$_SERVER, capture the returned path
$route = function (array $ctx, $uri) use ($router) {
    if ($uri === null) {
        unset($_SERVER['REQUEST_URI']);
    } else {
        $_SERVER['REQUEST_URI'] = $uri;
    }
    if (isset($ctx['sfn'])) {
        $_SERVER['SCRIPT_FILENAME'] = $ctx['sfn'];
    } else {
        unset($_SERVER['SCRIPT_FILENAME']);
    }
    $context = $ctx;
    return include $router;
};
$mk = function ($content, $dir = 'req', $extra = [], $script = null) use ($proj) {
    return ['type' => 'mysql', 'dir' => $dir, 'content' => $content, 'extra' => $extra,
            'script' => $script === null ? "$proj/public/index.php" : $script];
};

$p = $route($mk('SELECT * FROM users'), '/mre/t014/select?x=1');
var_dump($p === "$logDir/mre-t014-select.query.log");
var_dump(is_dir($logDir)); // router created the log dir

var_dump($route($mk('  select 1'), '/a') === "$logDir/a.query.log");            // lowercase
var_dump($route($mk('UPDATE t SET x=1'), '/a') === "$logDir/a.write.log");
var_dump($route($mk("INSERT INTO t VALUES (1)"), '/a') === "$logDir/a.write.log");
var_dump($route($mk('SET NAMES utf8mb4'), '/a') === "$logDir/a.write.log");
var_dump($route($mk('SHOW TABLES'), '/a') === "$logDir/a.query.log");
var_dump($route($mk("-- hint\n/* block */\n  SELECT 1"), '/a') === "$logDir/a.query.log"); // comments stripped
var_dump($route($mk('EXECUTE SELECT x FROM t'), '/a') === "$logDir/a.write.log"); // first keyword = EXECUTE

// resp classification: result-set meta -> query; write meta -> write
var_dump($route($mk('result', 'resp', ['rows' => 3, 'fields' => 2]), '/a') === "$logDir/a.query.log");
var_dump($route($mk('OK', 'resp', ['affected_rows' => 1]), '/a') === "$logDir/a.write.log");

// redis -> .redis.log; unknown type -> null
$rd = ['type' => 'redis', 'dir' => 'req', 'content' => 'SET k v', 'extra' => [], 'script' => "$proj/public/index.php"];
var_dump($route($rd, '/a') === "$logDir/a.redis.log");
var_dump($route(['type' => 'warn', 'dir' => '', 'content' => 'x', 'extra' => [], 'script' => "$proj/public/index.php"], '/a') === null);

// URL base derivation
var_dump($route($mk('SELECT 1'), '/api/user/list?a=1') === "$logDir/api-user-list.query.log");
var_dump($route($mk('SELECT 1'), '/a%2Fb?x') === "$logDir/a-b.query.log");      // rawurldecoded first
var_dump($route($mk('SELECT 1'), '/') === "$logDir/root.query.log");
var_dump($route($mk('SELECT 1'), null) === "$logDir/cli.query.log");            // no REQUEST_URI (CLI)
$long = '/' . str_repeat('a', 130);
$p = $route($mk('SELECT 1'), $long);
var_dump($p === "$logDir/" . str_repeat('a', 100) . ".query.log");              // truncated to 100

// project-root detection: no marker upwards -> null, no dir created
var_dump($route($mk('SELECT 1', 'req', [], "$nr/x.php"), '/a') === null);
var_dump(!is_dir("$nr/runtime"));
// ThinkPHP5 root marker (application/) with public/ entry
var_dump($route(['type' => 'mysql', 'dir' => 'req', 'content' => 'SELECT 1', 'extra' => [],
                 'script' => "$proj2/public/index.php"], '/a') === "$logDir2/a.query.log");
// script='' falls back to $_SERVER['SCRIPT_FILENAME']
var_dump($route(['type' => 'mysql', 'dir' => 'req', 'content' => 'SELECT 1', 'extra' => [],
                 'script' => '', 'sfn' => "$proj/public/index.php"], '/a') === "$logDir/a.query.log");
// script='' and no SCRIPT_FILENAME at all -> no root -> null
var_dump($route($mk('SELECT 1', 'req', [], ''), '/a') === null);

// restore and clean up
if ($origUri === null) { unset($_SERVER['REQUEST_URI']); } else { $_SERVER['REQUEST_URI'] = $origUri; }
if ($origSfn === null) { unset($_SERVER['SCRIPT_FILENAME']); } else { $_SERVER['SCRIPT_FILENAME'] = $origSfn; }
array_map('unlink', glob("$logDir/*") ?: []);
array_map('unlink', glob("$logDir2/*") ?: []);
@rmdir($logDir); @rmdir("$proj/runtime"); @rmdir($logDir2); @rmdir("$proj2/runtime");
array_map('unlink', glob("$proj/public/*") ?: []);
@unlink("$proj/composer.json"); @unlink("$proj2/composer.json");
@rmdir("$proj/public"); @rmdir("$proj2/public"); @rmdir("$proj2/application");
@rmdir($proj); @rmdir($proj2); @rmdir($nr);
@rmdir($base);
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
done
