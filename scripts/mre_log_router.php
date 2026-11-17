<?php
/**
 * mre 自带日志路由脚本 —— 部署时让 mre.log_script / MRE_LOG_SCRIPT 指向本文件。
 *
 * 每条记录（mysql/redis）写日志前被扩展 include 一次，收到：
 *   - $context：type / dir / content / extra / pid / request_id / script（入口脚本）等，
 *     详见 README「脚本回调」一节；
 *   - $_SERVER 等超全局原样可读。
 * 返回目标日志文件的绝对路径字符串；返回 null 则降级到 yaml 规则 / mre.log_file 主日志。
 *
 * 目录与文件名规则：
 *   1. 入口脚本 = $context['script']，回退 $_SERVER['SCRIPT_FILENAME']；
 *      从其所在目录逐级向上找项目根（标记：composer.json / think /
 *      thinkphp 目录 / application 目录，任一命中即停；兼容 ThinkPHP5 的
 *      public/index.php 入口 —— public 无标记，上一层即根）。
 *   2. 日志目录 = <项目根>/runtime/mre-logs/，不存在时本脚本递归创建
 *      （扩展写日志自身也支持父目录自动创建，二者互为兜底）。
 *   3. 容器路径映射：若项目根在 /app/cache/ 前缀下，且把前缀换成 /app/origin/
 *      后对应目录存在，则改用 /app/origin/... （宿主 /code/www 挂载在容器
 *      /app/origin，日志直接落到宿主机项目目录，即 Windows D:\code\www 的映射）。
 *   4. 文件名 = <URL路径>.query.log / <URL路径>.write.log / <URL路径>.redis.log：
 *      - URL 路径取 $_SERVER['REQUEST_URI'] 的 path 部分（去掉 ? 与 # 之后），
 *        rawurldecode 后安全化：字母数字与 . _ - 保留，其余字符转 '-'，去掉首尾
 *        - 与 .，最长 100 字符（超长截断，可能合并同前缀路径）；不压缩连续 '-'，
 *        尽量保持不同路径 → 不同文件的 1:1 映射；
 *      - 无 REQUEST_URI（CLI）或为空 → `cli`；有 URI 但安全化后为空（如 `/`）→ `root`；
 *      - mysql 请求（dir=req）：SQL 去前导空白与注释（-- 行注释、# 行注释、
 *        C 风格块注释）后，首关键字为 SELECT/SHOW/DESCRIBE/DESC/EXPLAIN
 *        （大小写不敏感）归 .query.log，其余 mysql 语句归 .write.log；
 *      - mysql 响应（dir=resp）：路由按条记录独立执行、无跨记录状态，拿不到对应
 *        语句，按 extra 元信息归类——含 rows / fields / fetched_rows（查询结果侧）
 *        归 .query.log，其余（affected_rows / OK / ERROR 等）归 .write.log；
 *      - redis 记录归 .redis.log；
 *      - 其他类型（warn 不走路由）返回 null 降级。
 *
 * 异步模式（mre.log_script_async / MRE_LOG_SCRIPT_ASYNC，扩展不落盘）：
 *   扩展会在 $context 里额外提供 line（已渲染好的整行，不含结尾换行）与
 *   format（json|text）。本脚本检测到 `line` 后，在算出的路径上自行
 *   flock + fwrite(line."\n")（目录上面已确保存在）；返回值照旧，
 *   异步模式下扩展忽略返回值，同步模式行为完全不变（无重复写）。
 *
 * 防御性约定：本脚本被同一进程每条记录重复 include，因此不定义顶层具名函数、
 * 不声明类（避免 Cannot redeclare）；全部逻辑在闭包内完成，除扩展注入并回收的
 * $context 外不产生任何全局变量；不使用 ___mre_ 前缀变量（扩展保留名）；
 * 不输出、不抛异常（扩展侧另有 ob 捕获 + try/catch 包装兜底）；任何未识别
 * 情况一律 return null 静默降级，绝不影响业务请求。
 */

$context = isset($context) && is_array($context) ? $context : array();

return call_user_func(function () use ($context) {
    /* ---- $context / $_SERVER 读取（全部防御式） ---- */
    $v = isset($context['type']) ? $context['type'] : '';
    $type = is_string($v) ? $v : '';
    $v = isset($context['dir']) ? $context['dir'] : '';
    $dir = is_string($v) ? $v : '';
    $v = isset($context['content']) ? $context['content'] : '';
    $content = is_string($v) ? $v : '';
    $extra = isset($context['extra']) && is_array($context['extra']) ? $context['extra'] : array();
    $v = isset($context['script']) ? $context['script'] : '';
    $script = is_string($v) ? $v : '';
    if ($script === ''
            && isset($_SERVER['SCRIPT_FILENAME'])
            && is_string($_SERVER['SCRIPT_FILENAME'])) {
        $script = $_SERVER['SCRIPT_FILENAME'];
    }

    /* ---- 1) 入口脚本向上找项目根 ---- */
    $root = '';
    $d = ($script !== '') ? dirname($script) : '';
    for ($i = 0; $i < 40 && $d !== '' && $d !== '.' && $d !== '/'; $i++) {
        if (file_exists($d . '/composer.json')
                || file_exists($d . '/think')
                || is_dir($d . '/thinkphp')
                || is_dir($d . '/application')) {
            $root = $d;
            break;
        }
        $parent = dirname($d);
        if ($parent === $d) {
            break;
        }
        $d = $parent;
    }
    if ($root === '') {
        return null; /* 未识别到项目根：降级，不在错误位置建目录 */
    }

    /* ---- 2) 容器 /app/cache → /app/origin 路径映射 ---- */
    // if (strpos($root, '/app/cache/') === 0) {
    //     $alt = '/app/origin/' . substr($root, strlen('/app/cache/'));
    //     if (is_dir($alt)) {
    //         $root = $alt;
    //     }
    // }

    /* ---- 3) 日志目录（脚本侧确保存在，扩展写日志另有 mkdir -p 兜底） ---- */
    $logDir = $root . '/runtime/mre-logs';
    if (!is_dir($logDir)) {
        @mkdir($logDir, 0755, true);
    }

    /* ---- 4) URL 路径 → 文件名 base ---- */
    $uri = (isset($_SERVER['REQUEST_URI']) && is_string($_SERVER['REQUEST_URI']))
        ? $_SERVER['REQUEST_URI'] : '';
    $base = 'cli';
    if ($uri !== '') {
        $p = $uri;
        $cut = strpos($p, '?');
        if ($cut !== false) {
            $p = substr($p, 0, $cut);
        }
        $cut = strpos($p, '#');
        if ($cut !== false) {
            $p = substr($p, 0, $cut);
        }
        $p = rawurldecode($p);
        $safe = '';
        $len = strlen($p);
        for ($i = 0; $i < $len; $i++) {
            $c = $p[$i];
            $keep = ($c >= 'a' && $c <= 'z') || ($c >= 'A' && $c <= 'Z')
                || ($c >= '0' && $c <= '9')
                || $c === '.' || $c === '_' || $c === '-';
            $safe .= $keep ? $c : '-';
        }
        $safe = trim($safe, '-.');
        if (strlen($safe) > 100) {
            $safe = substr($safe, 0, 100);
        }
        if ($safe === '') {
            /* 安全化后为空：'/'（或空 path）记 root；其余（如纯多字节路径）
             * 用原 path 的 md5 前缀消歧，保证不同路径 → 不同文件 */
            $base = ($p === '/' || $p === '') ? 'root' : 'x-' . substr(md5($p), 0, 8);
        } else {
            $base = $safe;
        }
    }

    /* ---- 5) 记录类型 → 后缀 ---- */
    if ($type === 'redis') {
        $suffix = '.redis.log';
    } elseif ($type === 'mysql') {
        if ($dir === 'req') {
            /* 去前导空白与注释后按 SQL 首关键字分类 */
            $sql = ltrim($content, " \t\r\n");
            $guard = 0;
            while ($sql !== '' && $guard < 20) {
                $guard++;
                if (strncmp($sql, '--', 2) === 0 || $sql[0] === '#') {
                    $nl = strpos($sql, "\n");
                    $sql = ($nl === false) ? '' : substr($sql, $nl + 1);
                } elseif (strncmp($sql, '/*', 2) === 0) {
                    $end = strpos($sql, '*/');
                    $sql = ($end === false) ? '' : substr($sql, $end + 2);
                } else {
                    break;
                }
                $sql = ltrim($sql, " \t\r\n");
            }
            $word = '';
            if ($sql !== '') {
                $word = strtoupper(substr($sql, 0, strcspn($sql, " \t\r\n(;,")));
            }
            $suffix = in_array($word, array('SELECT', 'SHOW', 'DESCRIBE', 'DESC', 'EXPLAIN'), true)
                ? '.query.log' : '.write.log';
        } else {
            /* resp：无对应语句可查，按结果集元信息归类 */
            $suffix = (isset($extra['rows']) || isset($extra['fields'])
                || isset($extra['fetched_rows']))
                ? '.query.log' : '.write.log';
        }
    } else {
        return null; /* 未知类型：降级到 yaml 规则 / 主日志 */
    }

    $path = $logDir . '/' . $base . $suffix;

    /* ---- 6) 异步自写模式（mre.log_script_async） ----
     * 异步模式下扩展不落盘，$context 额外带两个键：
     *   - line：扩展已渲染好的整行日志（json 或 text，不含结尾换行）；
     *   - format："json" 或 "text"。
     * 脚本职责：自己 flock + fwrite(line."\n")，目录上面已确保存在。
     * 同步模式下 `line` 不存在，行为与旧版完全一致（扩展照旧用返回值
     * 决定写入文件，不会有重复写）。返回值保持原样：异步模式下扩展忽略它。 */
    $v = isset($context['line']) ? $context['line'] : null;
    $line = is_string($v) ? $v : null;
    if ($line !== null) {
        $fp = @fopen($path, 'ab');
        if ($fp !== false) {
            if (flock($fp, LOCK_EX)) {
                fwrite($fp, $line . "\n");
                fflush($fp);
                flock($fp, LOCK_UN);
            }
            fclose($fp);
        }
    }

    return $path;
});
