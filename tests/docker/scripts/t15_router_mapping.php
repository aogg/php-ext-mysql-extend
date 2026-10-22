<?php
/**
 * t15: scripts/mre_log_router.php 的 /app/cache → /app/origin 容器路径映射（R4 场景）。
 *
 * 依赖容器内真实存在的 /app/cache/ 与 /app/origin/ 前缀目录（宿主 /code/www 挂载），
 * 因此放在 tests/docker/scripts/ 由 run-matrix.sh t15 触发，而不是 phpt。
 * 纯 include 受控 $context/$_SERVER 断言返回路径，不需要 mre 扩展本身。
 *
 * 用法: php t15_router_mapping.php [/path/to/mre_log_router.php]
 * 退出码: 0 全过；1 有失败；2 路由脚本不存在。
 */

$router = isset($argv[1]) ? $argv[1] : '/tmp/mre-test/scripts/mre_log_router.php';
if (!is_file($router)) {
    fwrite(STDERR, "router not found: $router\n");
    exit(2);
}

$fail = 0;
function ok($name, $cond, $got = '') {
    global $fail;
    echo ($cond ? 'PASS ' : 'FAIL ') . $name;
    if (!$cond) {
        echo " got=" . var_export($got, true);
        $fail++;
    }
    echo "\n";
}

// include the router with controlled $context/$_SERVER; $uri === null -> no REQUEST_URI (CLI)
function route($router, array $ctx, $uri) {
    if ($uri === null) {
        unset($_SERVER['REQUEST_URI']);
    } else {
        $_SERVER['REQUEST_URI'] = $uri;
    }
    unset($_SERVER['SCRIPT_FILENAME']);
    $context = $ctx;
    return include $router;
}

$tag = 'mre-t15-' . getmypid();
$mkctx = function ($script, $content = 'SELECT 1') {
    return ['type' => 'mysql', 'dir' => 'req', 'content' => $content, 'extra' => [], 'script' => $script];
};

/* 1) 正向映射：/app/cache 前缀且 /app/origin 对应目录存在 -> 日志落到 /app/origin */
$cacheProj = "/app/cache/.dsh/tmp/$tag/map/proj";
$originProj = "/app/origin/.dsh/tmp/$tag/map/proj";
@mkdir($cacheProj, 0755, true);
@mkdir($originProj, 0755, true); // 映射目标存在即可，内容无关
file_put_contents("$cacheProj/composer.json", '{}');

$p = route($router, $mkctx("$cacheProj/public/index.php"), "/$tag/map?x=1");
ok('map-to-origin-prefix', is_string($p) && strpos($p, "$originProj/runtime/mre-logs/") === 0, $p);
ok('map-file-name', $p === "$originProj/runtime/mre-logs/$tag-map.query.log", $p);

/* 2) 反向：/app/origin 对应目录不存在 -> 维持 /app/cache 原路径 */
$cacheOnly = "/app/cache/.dsh/tmp/$tag/nomap/proj";
@mkdir($cacheOnly, 0755, true);
file_put_contents("$cacheOnly/composer.json", '{}');

$p2 = route($router, $mkctx("$cacheOnly/public/index.php"), "/$tag/nomap");
ok('nomap-stays-cache', is_string($p2)
    && $p2 === "$cacheOnly/runtime/mre-logs/$tag-nomap.query.log", $p2);

/* 3) 真实业务项目：zuodian 根映射到宿主可见目录（只断言前缀，runtime/mre-logs 已存在） */
$p3 = route($router, $mkctx('/app/cache/gs/zuodian/zuodian/public/login.php'), '/t15/real');
ok('real-zuodian-maps-origin', is_string($p3)
    && $p3 === '/app/origin/gs/zuodian/zuodian/runtime/mre-logs/t15-real.query.log', $p3);

/* 清理容器与宿主两侧的临时目录 */
shell_exec('rm -rf ' . escapeshellarg("/app/cache/.dsh/tmp/$tag") . ' '
    . escapeshellarg("/app/origin/.dsh/tmp/$tag"));

echo $fail === 0 ? "T15-ALL-OK\n" : "T15-FAILED=$fail\n";
exit($fail === 0 ? 0 : 1);
