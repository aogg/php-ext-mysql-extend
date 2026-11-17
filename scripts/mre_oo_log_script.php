<?php
/**
 * mre_oo_log_script.php —— mre 扩展日志脚本：本地路由照旧 + 记录推送到 OpenObserve 风格写入接口。
 *
 * 部署时把 mre.log_script 指向本文件（替代直接指向 mre_log_router.php）：
 *   1. 先 `include __DIR__ . '/mre_log_router.php'`，拿到原路由结果并**原样返回**
 *      —— 本地日志文件、文件名规则、yaml 规则降级行为全部不变；
 *   2. 再按配置把本条记录（$context 原样 + level/job）推送到
 *      `POST {url}/api/{org}/{stream}/_json`（HTTP Basic，body 为 JSON 数组）。
 *
 * 调用契约（与 mre_log_router.php 相同，必须严格遵守）：
 *   - 本文件被扩展**每条记录** include 一次，因此所有具名函数都包在
 *     `function_exists()` 里，绝不重复声明；不声明类、不定义常量；
 *   - 不 echo/print（扩展侧另有 ob 捕获兜底）、不 exit、不抛异常；
 *   - 除扩展注入的 `$context`（以及路由脚本本来就会读的 `$_SERVER`）外不依赖任何
 *     全局变量；请求级状态只放在 `$GLOBALS['mre_oo_state']`（请求结束自动回收，
 *     供 shutdown flush 回调取用）；
 *   - 不使用 `___mre_` 前缀的全局变量（扩展保留名）。
 *
 * 推送失败（缺 user/pass、enable=0、url 非法、没有 curl 且 allow_url_fopen 关闭、
 * json_encode 失败、网络错误、HTTP 4xx/5xx、超时）一律**静默降级**：本地日志
 * 照常写、业务请求不受影响；打开 debug 时才把原因写进 debug_file（一行一个 JSON，
 * 绝不包含密码）。
 *
 * 配置：JSON 文件（默认 /usr/local/etc/mre-oo.json，MRE_OO_CONFIG 可改）+ 单键
 * 环境变量覆盖（MRE_OO_*）。完整参数表见 docs/mre_oo_log_script.md。
 *
 * 运行环境：PHP 7.4（mre 扩展的 log_script 在同一请求进程内被 include）。
 */

/* ====================================================================
 * 一、请求级状态（$GLOBALS['mre_oo_state']）
 * ==================================================================== */

/**
 * 初始化请求级状态；已存在则不动。
 *
 * 状态只放 $GLOBALS（扩展清理的是 $context 与 $___mre_*，这里用独立名字）。
 * 每条记录都会被 include 一次，所以这里必须幂等。
 *
 * @return void
 */
if (!function_exists('mre_oo_state_init')) {
    function mre_oo_state_init()
    {
        if (isset($GLOBALS['mre_oo_state']) && is_array($GLOBALS['mre_oo_state'])) {
            return;
        }
        $GLOBALS['mre_oo_state'] = array(
            // 合并后的配置；null 表示尚未加载（见 mre_oo_config()）
            'cfg' => null,
            // 待发送记录队列（batch=1 时使用）
            'buffer' => array(),
            // shutdown flush 是否已注册（保证一个请求只注册一次）
            'registered' => false,
            // shutdown flush 是否已执行；此后到达的记录（如 gtx 收尾）走单条即时发送
            'flushed' => false,
            // "不推送"的原因是否已在 debug 里记过（同请求只记一次，避免刷屏）
            'skip_logged' => false,
        );
    }
}

/* ====================================================================
 * 二、工具函数（全部 function_exists 保护）
 * ==================================================================== */

/**
 * 转字符串（配置值可能是 json 里的数字/布尔）。
 *
 * @param mixed $value 任意值
 * @return string
 */
if (!function_exists('mre_oo_str')) {
    function mre_oo_str($value)
    {
        if (is_string($value)) {
            return $value;
        }
        if (is_scalar($value)) {
            return (string) $value;
        }
        return '';
    }
}

/**
 * 布尔归一化：1/true/on/yes（大小写不敏感）= 1，其余 = 0。
 *
 * @param mixed $value 配置值或环境变量值
 * @return int 0 或 1
 */
if (!function_exists('mre_oo_bool')) {
    function mre_oo_bool($value)
    {
        if (is_bool($value)) {
            return $value ? 1 : 0;
        }
        if (is_int($value) || is_float($value)) {
            return $value ? 1 : 0;
        }
        $text = strtolower(trim(mre_oo_str($value)));
        if ($text === '') {
            return 0;
        }
        return in_array($text, array('1', 'true', 'on', 'yes', 'y'), true) ? 1 : 0;
    }
}

/**
 * 整数归一化 + 区间裁剪（解析失败用默认值）。
 *
 * @param mixed $value   配置值或环境变量值
 * @param int   $default 无法解析时的默认值
 * @param int   $min     下界
 * @param int   $max     上界（<=0 表示不限）
 * @return int
 */
if (!function_exists('mre_oo_int')) {
    function mre_oo_int($value, $default, $min, $max)
    {
        $number = null;
        if (is_int($value)) {
            $number = $value;
        } elseif (is_float($value)) {
            $number = (int) $value;
        } elseif (is_string($value) && preg_match('/^-?\d+$/', trim($value))) {
            $number = (int) trim($value);
        }
        if ($number === null) {
            $number = $default;
        }
        if ($number < $min) {
            $number = $min;
        }
        if ($max > 0 && $number > $max) {
            $number = $max;
        }
        return $number;
    }
}

/* ====================================================================
 * 三、debug 落盘（一行一个 JSON，不含密码）
 * ==================================================================== */

/**
 * 记一条 debug 行；debug=0 或写入失败时什么都不做。
 *
 * 这里是**唯一**的日志出口：所有推送失败原因都从这里落盘，绝不输出到响应。
 * 任何异常都在内部吞掉（debug 自身不能干扰业务）。
 *
 * @param string $event 事件名（config/skip/send/router_failed/push_failed）
 * @param array  $extra 附加字段（不得包含密码）
 * @return void
 */
if (!function_exists('mre_oo_debug')) {
    function mre_oo_debug($event, $extra = array())
    {
        try {
            $cfg = mre_oo_config();
            if (empty($cfg['debug'])) {
                return;
            }
            $file = mre_oo_str($cfg['debug_file']);
            if ($file === '') {
                return;
            }
            $line = array(
                // ISO8601 带时区偏移，与扩展日志的 time 字段格式一致
                'time' => date('c'),
                // 进程 id：多 worker 下用来区分是哪个 fpm 进程在推送
                'pid' => function_exists('getmypid') ? getmypid() : 0,
                // 事件名，便于 grep
                'event' => $event,
            );
            if (is_array($extra)) {
                foreach ($extra as $key => $value) {
                    if (is_string($key) && $key !== 'pass') {
                        $line[$key] = $value;
                    }
                }
            }
            $json = json_encode($line, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
            if ($json === false) {
                return;
            }
            @file_put_contents($file, $json . "\n", FILE_APPEND | LOCK_EX);
        } catch (\Throwable $e) {
            // 静默：debug 只是排障手段，不能影响业务
        }
    }
}

/* ====================================================================
 * 四、配置：默认值 → JSON 文件 → 单键环境变量
 * ==================================================================== */

/**
 * 读取并缓存配置（一个请求只加载一次）。
 *
 * 优先级：单键环境变量（MRE_OO_*）> JSON 文件 > 内置默认值。
 * JSON 文件里只认下面 $cfg 里已存在的键（未知键忽略，避免拼错键名悄悄生效）。
 *
 * @return array 配置数组（键见默认值数组）
 */
if (!function_exists('mre_oo_config')) {
    function mre_oo_config()
    {
        mre_oo_state_init();
        $state = &$GLOBALS['mre_oo_state'];
        if (isset($state['cfg']) && is_array($state['cfg'])) {
            return $state['cfg'];
        }

        /* 内置默认值：改这里等于改全局默认，docs/mre_oo_log_script.md 的表格必须同步 */
        $cfg = array(
            // 推送总开关：0 = 只写本地日志，不推送（本地路由照旧执行）
            'enable' => 1,
            // 写入接口 base（脚本自行拼 /api/{org}/{stream}/_json）
            'url' => 'http://exclude.proxy.mac.path.openobserve.port.5080.proxy_sslip.192.168.137.2.sslip.io',
            // 组织名
            'org' => 'default',
            // 流名
            'stream' => 'default',
            // HTTP Basic 用户名；为空 = 不推送
            'user' => '',
            // HTTP Basic 密码；为空 = 不推送（任何输出都不回显）
            'pass' => '',
            // 单次请求总超时（秒），同时作为 connect timeout 的上限
            'timeout' => 2,
            // 1 = 跳过 TLS 证书校验（相当于 curl -k）
            'insecure' => 1,
            // 1 = 请求内累积、shutdown 时合并发送；0 = 每条即时单发
            'batch' => 1,
            // 1 = 把推送结果/跳过原因写进 debug_file
            'debug' => 0,
            // debug 输出文件（一行一个 JSON）
            'debug_file' => '/tmp/mre-oo-push.log',
            // 缓冲上限：达到即提前同步 flush（防止长时间请求内存膨胀）
            'max_buffer' => 1000,
        );

        // 配置加载过程中的问题，等 debug 判定后再落盘（此时 cfg 还没缓存，不能直接 log）
        $notes = array();
        $path = getenv('MRE_OO_CONFIG');
        if (!is_string($path) || trim($path) === '') {
            $path = '/usr/local/etc/mre-oo.json';
        }
        $path = trim($path);
        $raw = is_file($path) ? @file_get_contents($path) : false;
        if (is_string($raw) && $raw !== '') {
            $decoded = json_decode($raw, true);
            if (is_array($decoded)) {
                foreach ($decoded as $key => $value) {
                    if (is_string($key) && array_key_exists($key, $cfg)) {
                        $cfg[$key] = $value;
                    }
                }
            } else {
                $notes[] = 'config_json_invalid:' . $path;
            }
        } else {
            // 文件缺失不算错误：纯环境变量部署是正常用法
            $notes[] = 'config_missing:' . $path;
        }

        /* 单键环境变量覆盖；未设置（false）或空串时保留文件/默认值 */
        $env_map = array(
            // 配置键 => 环境变量名
            'enable' => 'MRE_OO_ENABLE',
            'url' => 'MRE_OO_URL',
            'org' => 'MRE_OO_ORG',
            'stream' => 'MRE_OO_STREAM',
            'user' => 'MRE_OO_USER',
            'pass' => 'MRE_OO_PASS',
            'timeout' => 'MRE_OO_TIMEOUT',
            'insecure' => 'MRE_OO_INSECURE',
            'batch' => 'MRE_OO_BATCH',
            'debug' => 'MRE_OO_DEBUG',
        );
        foreach ($env_map as $key => $name) {
            $value = getenv($name);
            if ($value === false || !is_string($value) || trim($value) === '') {
                continue;
            }
            $cfg[$key] = $value;
        }

        /* 归一化：类型固定下来，后面所有判断都基于干净的值 */
        $cfg['enable'] = mre_oo_bool($cfg['enable']);
        $cfg['insecure'] = mre_oo_bool($cfg['insecure']);
        $cfg['batch'] = mre_oo_bool($cfg['batch']);
        $cfg['debug'] = mre_oo_bool($cfg['debug']);
        // 超时限制在 1~30 秒：日志推送绝不能把 worker 占太久
        $cfg['timeout'] = mre_oo_int($cfg['timeout'], 2, 1, 30);
        // 缓冲上限限制在 1~100000 条
        $cfg['max_buffer'] = mre_oo_int($cfg['max_buffer'], 1000, 1, 100000);
        $cfg['url'] = rtrim(trim(mre_oo_str($cfg['url'])), '/');
        $cfg['org'] = trim(mre_oo_str($cfg['org']));
        $cfg['stream'] = trim(mre_oo_str($cfg['stream']));
        $cfg['user'] = mre_oo_str($cfg['user']);
        $cfg['pass'] = mre_oo_str($cfg['pass']);
        $cfg['debug_file'] = trim(mre_oo_str($cfg['debug_file']));

        // 先缓存，再 log：mre_oo_debug() 内部会调 mre_oo_config()，避免递归
        $state['cfg'] = $cfg;
        foreach ($notes as $note) {
            mre_oo_debug('config', array('note' => $note));
        }
        return $cfg;
    }
}

/* ====================================================================
 * 五、推送：记录加工 / 队列 / HTTP
 * ==================================================================== */

/**
 * 从本地日志文件名派生 job：去掉 .query.log / .write.log / .redis.log / .log。
 *
 * 后缀必须从长到短尝试，否则 ".query.log" 会先被 ".log" 命中，留下 "api-x.query"。
 * 路径为 null/空（路由没给路径，如 warn 记录）时用 "mre"（降级到主日志）。
 *
 * @param mixed $path 路由脚本返回的本地日志绝对路径（或 null）
 * @return string job 名
 */
if (!function_exists('mre_oo_job_from_path')) {
    function mre_oo_job_from_path($path)
    {
        if (!is_string($path) || $path === '') {
            return 'mre';
        }
        $base = basename($path);
        if (!is_string($base) || $base === '') {
            return 'mre';
        }
        // 顺序敏感：长后缀在前
        $suffixes = array('.query.log', '.write.log', '.redis.log', '.log');
        foreach ($suffixes as $suffix) {
            $len = strlen($suffix);
            if (strlen($base) > $len && substr($base, -$len) === $suffix) {
                return substr($base, 0, -$len);
            }
        }
        return $base;
    }
}

/**
 * 加工一条推送记录：$context 原样复制 + 补 level / job（已有值不覆盖）。
 *
 * @param array      $context 扩展注入的记录上下文
 * @param mixed      $path    路由脚本返回的本地日志路径（string 或 null）
 * @return array 待推送记录
 */
if (!function_exists('mre_oo_record')) {
    function mre_oo_record($context, $path)
    {
        $record = is_array($context) ? $context : array();

        // level：warn 类型的记录标 warn，其余 info；$context 里已有非空 level 时保留
        $level = isset($record['level']) ? $record['level'] : null;
        if (!is_scalar($level) || (string) $level === '') {
            $type = isset($record['type']) && is_string($record['type']) ? $record['type'] : '';
            $record['level'] = ($type === 'warn') ? 'warn' : 'info';
        }

        // job：由本地日志文件名派生（路由给不出路径时用 mre）；已有非空 job 时保留
        $job = isset($record['job']) ? $record['job'] : null;
        if (!is_scalar($job) || (string) $job === '') {
            $record['job'] = mre_oo_job_from_path($path);
        }

        return $record;
    }
}

/**
 * 推送前置条件检查；返回原因用于 debug（不含敏感信息）。
 *
 * @param array $cfg 配置数组
 * @return array 形如 array('ok'=>bool,'reason'=>string)
 */
if (!function_exists('mre_oo_push_ready')) {
    function mre_oo_push_ready($cfg)
    {
        if (empty($cfg['enable'])) {
            // 总开关关闭：只写本地日志
            return array('ok' => false, 'reason' => 'disabled');
        }
        if (mre_oo_str($cfg['user']) === '') {
            // 缺用户名：不推送
            return array('ok' => false, 'reason' => 'no_user');
        }
        if (mre_oo_str($cfg['pass']) === '') {
            // 缺密码：不推送
            return array('ok' => false, 'reason' => 'no_pass');
        }
        $url = mre_oo_str($cfg['url']);
        if ($url === '' || (strpos($url, 'http://') !== 0 && strpos($url, 'https://') !== 0)) {
            // url 缺失或不是 http(s)：不推送
            return array('ok' => false, 'reason' => 'bad_url');
        }
        if (!function_exists('curl_init') && !ini_get('allow_url_fopen')) {
            // curl 扩展没有、stream 回退又被关闭：不推送
            return array('ok' => false, 'reason' => 'no_transport');
        }
        return array('ok' => true, 'reason' => '');
    }
}

/**
 * 拼写入地址：{url}/api/{org}/{stream}/_json。
 *
 * org / stream 为空时回 default；含 "/" 视为配置错误（返回空串，调用方跳过并记 debug）。
 *
 * @param array $cfg 配置数组
 * @return string 完整地址（配置非法时为空串）
 */
if (!function_exists('mre_oo_endpoint')) {
    function mre_oo_endpoint($cfg)
    {
        $url = rtrim(trim(mre_oo_str($cfg['url'])), '/');
        $org = trim(mre_oo_str($cfg['org']));
        $stream = trim(mre_oo_str($cfg['stream']));
        if ($org === '') {
            $org = 'default';
        }
        if ($stream === '') {
            $stream = 'default';
        }
        if ($url === '' || strpos($org, '/') !== false || strpos($stream, '/') !== false) {
            return '';
        }
        return $url . '/api/' . $org . '/' . $stream . '/_json';
    }
}

/**
 * 把 HTTP 响应的关键信息压缩成一行摘要（供 debug 落盘，避免整段响应进日志）。
 *
 * 成功响应形如 {"code":200,"status":[{"name":"default","successful":N,"failed":0}]}，
 * 这里累加 successful / failed；解析不出结构时退化为响应体前 300 字节。
 *
 * @param mixed $raw 响应体
 * @return string
 */
if (!function_exists('mre_oo_http_summary')) {
    function mre_oo_http_summary($raw)
    {
        if (!is_string($raw) || $raw === '') {
            return '';
        }
        $decoded = json_decode($raw, true);
        if (is_array($decoded)) {
            // 摘要只保留服务端确认的写入结果
            $summary = array();
            if (isset($decoded['code']) && is_scalar($decoded['code'])) {
                $summary['code'] = $decoded['code'];
            }
            if (isset($decoded['status']) && is_array($decoded['status'])) {
                $successful = 0; // 服务端确认写入成功条数
                $failed = 0;     // 服务端确认写入失败条数
                foreach ($decoded['status'] as $item) {
                    if (!is_array($item)) {
                        continue;
                    }
                    if (isset($item['successful']) && is_numeric($item['successful'])) {
                        $successful += (int) $item['successful'];
                    }
                    if (isset($item['failed']) && is_numeric($item['failed'])) {
                        $failed += (int) $item['failed'];
                    }
                }
                $summary['successful'] = $successful;
                $summary['failed'] = $failed;
            }
            if ($summary) {
                $json = json_encode($summary, JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES);
                if ($json !== false) {
                    return $json;
                }
            }
        }
        // 非 JSON（如 "Unauthorized Access" / SerdeJsonError）：截断后原样保留，便于判断 400/401
        return substr($raw, 0, 300);
    }
}

/**
 * curl 通道（容器/生产常态）。
 *
 * 认证只走 CURLOPT_USERPWD（不拼进 URL），避免密码出现在进程列表/访问日志里。
 *
 * @param string $url     完整写入地址
 * @param string $body    JSON 数组请求体
 * @param array  $cfg     配置数组
 * @param int    $timeout 超时秒数
 * @return array 形如 array('ok'=>bool,'code'=>int,'error'=>string,'summary'=>string)
 */
if (!function_exists('mre_oo_http_post_curl')) {
    function mre_oo_http_post_curl($url, $body, $cfg, $timeout)
    {
        $result = array(
            // 是否 2xx
            'ok' => false,
            // HTTP 状态码（0 = 没拿到响应）
            'code' => 0,
            // curl 错误描述
            'error' => '',
            // 响应摘要
            'summary' => '',
        );
        $ch = @curl_init();
        if ($ch === false) {
            $result['error'] = 'curl_init_failed';
            return $result;
        }
        $options = array(
            CURLOPT_URL => $url,
            CURLOPT_POST => true,
            CURLOPT_POSTFIELDS => $body,
            CURLOPT_HTTPAUTH => CURLAUTH_BASIC,
            CURLOPT_USERPWD => mre_oo_str($cfg['user']) . ':' . mre_oo_str($cfg['pass']),
            CURLOPT_RETURNTRANSFER => true,
            // connect timeout 不超过总超时：直接取同一个上限
            CURLOPT_CONNECTTIMEOUT => $timeout,
            CURLOPT_TIMEOUT => $timeout,
            // 接口只收 JSON 数组，不声明类型会被按表单/纯文本处理
            CURLOPT_HTTPHEADER => array('Content-Type: application/json'),
        );
        if (!empty($cfg['insecure'])) {
            // 跳过 TLS 校验（自签/内网代理常见的部署方式，等价 curl -k）
            $options[CURLOPT_SSL_VERIFYPEER] = false;
            $options[CURLOPT_SSL_VERIFYHOST] = 0;
        }
        @curl_setopt_array($ch, $options);
        $raw = @curl_exec($ch);
        $result['code'] = (int) @curl_getinfo($ch, CURLINFO_HTTP_CODE);
        $errno = (int) @curl_errno($ch);
        $message = @curl_error($ch);
        @curl_close($ch);
        if ($raw === false || $errno !== 0) {
            $result['error'] = 'curl(' . $errno . '): ' . mre_oo_str($message);
        }
        $result['summary'] = mre_oo_http_summary($raw);
        $result['ok'] = ($result['code'] >= 200 && $result['code'] < 300);
        return $result;
    }
}

/**
 * stream context 回退通道（没有 curl 扩展时）。
 *
 * Authorization 头用 base64(user:pass) 现算，同样不进 URL；ignore_errors=true
 * 保证 4xx/5xx 也能拿到响应体与状态码。
 *
 * @param string $url     完整写入地址
 * @param string $body    JSON 数组请求体
 * @param array  $cfg     配置数组
 * @param int    $timeout 超时秒数
 * @return array 形如 array('ok'=>bool,'code'=>int,'error'=>string,'summary'=>string)
 */
if (!function_exists('mre_oo_http_post_stream')) {
    function mre_oo_http_post_stream($url, $body, $cfg, $timeout)
    {
        $result = array(
            // 是否 2xx
            'ok' => false,
            // HTTP 状态码（0 = 没拿到响应）
            'code' => 0,
            // 打开/读取失败描述
            'error' => '',
            // 响应摘要
            'summary' => '',
        );
        $header = "Content-Type: application/json\r\n"
            . 'Authorization: Basic '
            . base64_encode(mre_oo_str($cfg['user']) . ':' . mre_oo_str($cfg['pass']))
            . "\r\n";
        $http_options = array(
            // 只做 POST
            'method' => 'POST',
            // 上一步拼好的请求头
            'header' => $header,
            // JSON 数组请求体
            'content' => $body,
            // stream 包装器的超时（秒）
            'timeout' => $timeout,
            // 4xx/5xx 也返回句柄，便于区分 401 与 400
            'ignore_errors' => true,
            // 固定 1.1，避免服务端按 1.0 处理 keep-alive
            'protocol_version' => 1.1,
        );
        $context_options = array(
            // HTTP 包装器配置
            'http' => $http_options,
        );
        if (strpos($url, 'https://') === 0 && !empty($cfg['insecure'])) {
            // 跳过 TLS 校验，与 curl 分支保持一致
            $context_options['ssl'] = array(
                // 不校验证书链
                'verify_peer' => false,
                // 不校验证书 CN/SAN
                'verify_peer_name' => false,
                // 允许自签证书
                'allow_self_signed' => true,
            );
        }
        $context = @stream_context_create($context_options);
        $fp = @fopen($url, 'rb', false, $context);
        if ($fp === false) {
            $last = error_get_last();
            $result['error'] = 'stream_open_failed'
                . (is_array($last) && isset($last['message']) ? ': ' . $last['message'] : '');
            return $result;
        }
        $raw = @stream_get_contents($fp);
        $meta = @stream_get_meta_data($fp);
        @fclose($fp);
        // 状态码从响应头首行 "HTTP/1.1 200 OK" 解析
        if (is_array($meta) && isset($meta['wrapper_data']) && is_array($meta['wrapper_data'])) {
            foreach ($meta['wrapper_data'] as $line) {
                if (is_string($line) && preg_match('#^HTTP/\S+\s+(\d{3})#', $line, $match)) {
                    $result['code'] = (int) $match[1];
                    break;
                }
            }
        }
        if ($raw === false) {
            $result['error'] = 'stream_read_failed';
            $raw = '';
        }
        $result['summary'] = mre_oo_http_summary($raw);
        $result['ok'] = ($result['code'] >= 200 && $result['code'] < 300);
        return $result;
    }
}

/**
 * 发送一批记录（一次 POST JSON 数组）。
 *
 * 失败不重试、不抛异常；只会落一条 debug。返回 true 仅代表 2xx。
 *
 * @param array $records 记录列表（形如 array(array(...), array(...))）
 * @return bool
 */
if (!function_exists('mre_oo_push')) {
    function mre_oo_push($records)
    {
        if (!is_array($records) || !$records) {
            return false;
        }
        $cfg = mre_oo_config();
        $ready = mre_oo_push_ready($cfg);
        if (!$ready['ok']) {
            mre_oo_debug('skip', array(
                // 跳过原因（disabled/no_user/no_pass/bad_url/no_transport）
                'reason' => $ready['reason'],
                // 本可发送的条数，便于确认是哪一侧的问题
                'count' => count($records),
            ));
            return false;
        }
        $endpoint = mre_oo_endpoint($cfg);
        if ($endpoint === '') {
            mre_oo_debug('skip', array(
                // 地址拼不出来（url/org/stream 非法）
                'reason' => 'bad_org_stream',
                // 本可发送的条数
                'count' => count($records),
            ));
            return false;
        }
        // 接口硬约束：body 必须是 JSON **数组**（JSON Lines 会 400），所以先合并再编码
        // 记录里的 SQL/redis content 可能含非 UTF-8 字节，带 SUBSTITUTE 可把非法字节替换成 U+FFFD，避免单条坏字节导致整批丢弃
        $body = json_encode(array_values($records), JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES | JSON_INVALID_UTF8_SUBSTITUTE);
        if ($body === false) {
            mre_oo_debug('skip', array(
                // 编码失败（常见原因：记录里含非法 UTF-8 字节）
                'reason' => 'json_encode_failed',
                // json 自己的错误描述
                'error' => function_exists('json_last_error_msg') ? json_last_error_msg() : '',
                // 本可发送的条数
                'count' => count($records),
            ));
            return false;
        }
        $start = microtime(true);
        $timeout = mre_oo_int($cfg['timeout'], 2, 1, 30);
        if (function_exists('curl_init')) {
            $response = mre_oo_http_post_curl($endpoint, $body, $cfg, $timeout);
        } else {
            $response = mre_oo_http_post_stream($endpoint, $body, $cfg, $timeout);
        }
        mre_oo_debug('send', array(
            // 目标地址（不含凭据）
            'endpoint' => $endpoint,
            // 本次 POST 的记录条数
            'count' => count($records),
            // 请求体字节数
            'body_bytes' => strlen($body),
            // HTTP 状态码（0 = 连接/超时失败）
            'http_code' => $response['code'],
            // 是否 2xx
            'ok' => $response['ok'],
            // 传输层错误（超时/连接拒绝等）
            'error' => $response['error'],
            // 响应摘要（successful/failed 或截断后的响应体）
            'response' => $response['summary'],
            // 本次请求耗时（毫秒）
            'cost_ms' => round((microtime(true) - $start) * 1000, 1),
        ));
        return $response['ok'];
    }
}

/* ====================================================================
 * 六、缓冲与发送时机（batch=1 合并 / batch=0 即时）
 * ==================================================================== */

/**
 * 发送缓冲里的记录。
 *
 * @param bool $final true = shutdown 阶段的最后一次 flush
 * @return int 本次发送的条数
 */
if (!function_exists('mre_oo_flush')) {
    function mre_oo_flush($final)
    {
        mre_oo_state_init();
        $state = &$GLOBALS['mre_oo_state'];
        $records = array();
        if (isset($state['buffer']) && is_array($state['buffer']) && $state['buffer']) {
            $records = $state['buffer'];
        }
        // 先清空：即使发送失败也不重试、不重复发送
        $state['buffer'] = array();
        if ($final) {
            $state['flushed'] = true;
            /* shutdown 阶段：先把响应 flush 给客户端，日志推送不占用客户端等待时间。
             * 只在 final flush 里调用：请求进行中（max_buffer 提前 flush）调用
             * 会让后续业务输出被丢弃，等于破坏响应体。 */
            if (function_exists('fastcgi_finish_request')) {
                @fastcgi_finish_request();
            }
        }
        if ($records) {
            mre_oo_push($records);
        }
        return count($records);
    }
}

/**
 * shutdown 回调：注册一次、请求结束时把所有缓冲记录合并发送。
 *
 * @return void
 */
if (!function_exists('mre_oo_flush_at_shutdown')) {
    function mre_oo_flush_at_shutdown()
    {
        try {
            mre_oo_flush(true);
        } catch (\Throwable $e) {
            // shutdown 阶段抛异常会变成 fatal，这里必须吞掉
            mre_oo_debug('flush_failed', array('error' => $e->getMessage()));
        }
    }
}

/**
 * 收一条记录：按配置决定进缓冲还是立即发送。
 *
 * batch=1：首次入队时注册 shutdown flush（一个请求只注册一次）；
 *          缓冲达到 max_buffer 时提前同步 flush；
 * shutdown flush 之后到达的记录（如 gtx 收尾补的记录）走单条即时发送。
 * batch=0：每条即时单条 POST。
 *
 * @param array $record 待推送记录
 * @return void
 */
if (!function_exists('mre_oo_enqueue')) {
    function mre_oo_enqueue($record)
    {
        if (!is_array($record)) {
            return;
        }
        mre_oo_state_init();
        $state = &$GLOBALS['mre_oo_state'];
        $cfg = mre_oo_config();

        // 前置条件不满足时连缓冲都不做，避免白白占内存；debug 一个请求只记一次
        $ready = mre_oo_push_ready($cfg);
        if (!$ready['ok']) {
            if (empty($state['skip_logged'])) {
                $state['skip_logged'] = true;
                mre_oo_debug('skip', array('reason' => $ready['reason']));
            }
            return;
        }

        if (empty($cfg['batch']) || !empty($state['flushed'])) {
            // 单条即时发送：batch=0，或 shutdown flush 已跑过（late record）
            mre_oo_push(array($record));
            return;
        }

        $state['buffer'][] = $record;

        if (empty($state['registered'])) {
            $state['registered'] = true;
            register_shutdown_function('mre_oo_flush_at_shutdown');
        }

        /* 缓冲上限保护：达到 max_buffer 时提前同步发送。
         * 注意这一次 curl 是在**请求内同步执行**的（最坏阻塞 timeout 秒），
         * 所以 max_buffer 不要设得太小，否则等于退化成逐条同步发送。 */
        if (count($state['buffer']) >= $cfg['max_buffer']) {
            mre_oo_flush(false);
        }
    }
}

/* ====================================================================
 * 七、主流程：先本地路由，再推送，最后原样返回路由结果
 * ==================================================================== */

// 防御式：$context 由扩展注入；CLI 直接 include 时也允许调用方先自行设置
$context = isset($context) && is_array($context) ? $context : array();

return call_user_func(function () use ($context) {
    // 复制一份：include 路由脚本时它会给自己的 $context 赋值，避免互相干扰
    $current = $context;

    /* 1) 原路由：本地日志行为与"只装 mre_log_router.php"时完全一致 */
    $path = null;
    $router = __DIR__ . '/mre_log_router.php';
    if (is_file($router)) {
        try {
            $ret = include $router;
            if (is_string($ret) && $ret !== '') {
                // 原样返回：string = 本条记录写哪个本地文件
                $path = $ret;
            }
        } catch (\Throwable $e) {
            // 路由脚本自身异常：降级到 yaml 规则 / mre.log_file
            $path = null;
            mre_oo_debug('router_failed', array('error' => $e->getMessage()));
        }
    }

    /* 2) 推送：任何失败都静默，绝不影响本地日志与业务请求 */
    try {
        $cfg = mre_oo_config();
        if (!empty($cfg['enable'])) {
            mre_oo_enqueue(mre_oo_record($current, $path));
        }
    } catch (\Throwable $e) {
        mre_oo_debug('push_failed', array('error' => $e->getMessage()));
    }

    // 返回路由结果（string 或 null）：扩展据此决定本条记录写哪个文件
    return $path;
});
