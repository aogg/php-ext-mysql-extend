<?php
/**
 * oo_push.php —— 把日志记录推送到 OpenObserve 风格的写入接口（PHP 7.4，纯 CLI，单文件）。
 *
 * 目标接口（后端实测为 OpenSearch 7.17.1 + 兼容 shim）：
 *   POST {base}/api/{org}/{stream}/_json      HTTP Basic 认证
 *   body 必须是 **JSON 数组**（JSON Lines 会被 400，报 SerdeJsonError# trailing
 *   characters —— 这是 shim 的硬约束，不是本脚本的偏好）；
 *   成功：HTTP 200 + {"code":200,"status":[{"name":"default","successful":N,"failed":0}]}；
 *   认证失败：401 Unauthorized Access；body 非法 JSON：400。
 *   该账号的查询接口（_search 等）返回 401，所以本脚本只做写入，不做查询。
 *
 * 设计取舍（为什么这么写）：
 *   - 单文件、无 composer：脚本要能直接丢进任意 PHP 7.4 容器执行，不依赖项目
 *     autoload；容器里往往只有 php + 少量扩展，所以只用内置函数；
 *   - 先拼成 PHP 数组再分批编码：--dry-run 与真实发送共用同一份 JSON 编码结果，
 *     避免"预览的内容"和"真正发出去的内容"不一致；
 *   - curl 扩展优先、stream context 回退：容器常裁剪 curl，stream 版本用
 *     ignore_errors 也照样能拿到 4xx/5xx 的响应体，用来区分 401 与 400；
 *   - 认证只走 CURLOPT_USERPWD / Authorization 头，绝不拼进 URL：否则密码会
 *     出现在 --verbose 输出、进程列表和代理日志里；
 *   - 401/403 立即中止且不重试：认证问题重试只会让批量任务反复撞墙；
 *   - 输入解析失败 / JSON 编码失败一律 exit 1：属于"调用方写错了"，不是服务端
 *     故障，跟网络/HTTP 错误（exit 2）分开，便于上游据此 re-run；
 *   - 所有读 argv / 环境变量 / 文件的地方都做防御式判断，不假设类型。
 */

/* ---- 常量：默认值与退出码集中在这里，--help 与 docs/oo_push.md 以此为准 ---- */
define('OO_PUSH_NAME', 'oo_push');
define('OO_PUSH_VERSION', '1.0.0');
define('OO_PUSH_DEFAULT_URL', 'http://exclude.proxy.mac.path.openobserve.port.5080.proxy_sslip.192.168.137.2.sslip.io');
define('OO_PUSH_DEFAULT_ORG', 'default');
define('OO_PUSH_DEFAULT_STREAM', 'default');
define('OO_PUSH_DEFAULT_TIMEOUT', 10);
define('OO_PUSH_DEFAULT_RETRY', 2);
define('OO_PUSH_DEFAULT_BATCH', 200);
define('OO_PUSH_DEFAULT_LEVEL', 'info');
define('OO_PUSH_DEFAULT_JOB', 'oo_push');

/** 退出码：0 全成功；1 用法/配置/输入解析；2 认证或网络/HTTP；3 部分失败 */
define('OO_PUSH_EXIT_OK', 0);
define('OO_PUSH_EXIT_USAGE', 1);
define('OO_PUSH_EXIT_HTTP', 2);
define('OO_PUSH_EXIT_PARTIAL', 3);

/* --quiet 后置判定：解析参数时还不知道 quiet，所以用一个全局开关，供提示函数共享 */
$oo_push_quiet = false;

/**
 * 配置 / 用法类错误：写 stderr 后直接退出（默认 exit 1）。
 *
 * @param string $message 人类可读的错误描述
 * @param int    $code    退出码
 */
function oo_fail($message, $code = OO_PUSH_EXIT_USAGE)
{
    fwrite(STDERR, OO_PUSH_NAME . ': ' . $message . "\n");
    exit($code);
}

/** 真正的错误信息：不论 --quiet 都要打（否则失败时用户什么都看不到） */
function oo_error($message)
{
    fwrite(STDERR, OO_PUSH_NAME . ': ' . $message . "\n");
}

/** 进度 / 提示：--quiet 时静默，避免污染调用方的日志采集 */
function oo_notice($message)
{
    global $oo_push_quiet;
    if ($oo_push_quiet) {
        return;
    }
    fwrite(STDERR, OO_PUSH_NAME . ': ' . $message . "\n");
}

/**
 * 用法 + 参数表。--help 直接把它打到 stdout 并 exit 0（成功路径）。
 *
 * @return string
 */
function oo_help()
{
    return <<<'TXT'
oo_push 1.0.0 —— 把日志记录推送到 OpenObserve 风格的写入接口（POST /api/{org}/{stream}/_json）

用法:
  php oo_push.php [选项] [--]
  php oo_push.php --log='hello' --level=warn --job=demo --user=u --pass=p
  php oo_push.php --file=/path/app.query.log --mre --user=u --pass=p
  cat records.jsonl | php oo_push.php --user=u --pass=p

输入来源（至少一个；一个都没有且 stdin 是管道时读 stdin）:
  --log=TEXT            生成合成记录正文（配合 --level / --job / --count）
  --file=PATH           读文件，可重复；PATH 为 - 时表示 stdin
  --glob=PATTERN        glob() 展开成文件列表，可重复，与 --file 合并去重
  --data=JSON           命令行内联 JSON（数组或单个对象）

连接参数:
  --url=URL             写入接口 base                   默认 $OO_URL 或内置默认地址
  --org=NAME            组织名                          默认 $OO_ORG 或 default
  --stream=NAME         流名                            默认 $OO_STREAM 或 default
  --user=USER           HTTP Basic 用户名               默认 $OO_USER
  --pass=PASS           HTTP Basic 密码                 默认 $OO_PASS / $OO_PASSWORD
  --auth=USER:PASS      用户名密码一起给（--user/--pass 优先）
  --insecure            跳过 TLS 证书校验
  --timeout=SEC         单次请求超时（秒，整数）        默认 10
  --retry=N             失败重试次数（网络/429/5xx）    默认 2

输入参数:
  --level=LEVEL         合成记录的 level                默认 info
  --job=JOB             合成记录的 job                  默认 oo_push
  --count=N             合成记录条数                    默认 1
  --mre                 按 mre 扩展日志处理（补 level/job，保留原字段）
  --from=TIME           只保留 time >= 该时间的记录（strtotime 可解析）
  --to=TIME             只保留 time <= 该时间的记录
  --limit=N             总记录上限                      默认不限（0 表示不要记录）

发送参数:
  --batch=N             每请求记录数                    默认 200
  --dry-run             只把每批 payload 打到 stdout，不发请求
  --print-response      在汇总 JSON 里附带每批原始响应
  --verbose, -v         stderr 打每批进度与连接信息（密码脱敏）
  --quiet, -q           关闭非错误提示
  --help, -h            显示本帮助并退出 0
  --version             显示版本并退出 0

退出码:
  0  全部成功（sent == records 且服务端 failed == 0）
  1  用法 / 配置 / 输入解析错误
  2  认证（401/403）或网络 / HTTP 错误（重试后仍失败）
  3  部分失败（服务端 failed > 0 或部分批次失败）

TXT;
}

/**
 * 选项定义：规范名 => 是否必须带值。--help 表格、解析器、缺值判断都共用这一份，
 * 避免"文档里有、解析器不认"的漂移。
 *
 * @return array
 */
function oo_option_defs()
{
    return array(
        // 连接
        'url' => true,
        'org' => true,
        'stream' => true,
        'user' => true,
        'pass' => true,
        'auth' => true,
        'insecure' => false,
        'timeout' => true,
        'retry' => true,
        // 输入
        'log' => true,
        'level' => true,
        'job' => true,
        'count' => true,
        'file' => true,
        'glob' => true,
        'data' => true,
        'mre' => false,
        'from' => true,
        'to' => true,
        'limit' => true,
        // 发送
        'batch' => true,
        'dry-run' => false,        // 开关型；规范名 dry_run
        'print-response' => false, // 开关型；规范名 print_response
        'verbose' => false,
        'quiet' => false,
        'help' => false,
        'version' => false,
    );
}

/**
 * 短选项 -> 规范名。只提供 -h / -v / -q，其余短选项一律报错，避免歧义。
 *
 * @param string $name 去掉前导 - 的短选项名
 * @return string|null
 */
function oo_option_short($name)
{
    $map = array(
        'h' => 'help',       // -h
        'v' => 'verbose',    // -v
        'q' => 'quiet',      // -q
    );
    return isset($map[$name]) ? $map[$name] : null;
}

/**
 * 规范化长选项名：--dry-run 在代码里用 dry_run，其余原样。
 *
 * @param string $name 去掉前导 -- 的名字
 * @return string
 */
function oo_option_canonical($name)
{
    return str_replace('-', '_', $name);
}

/**
 * 判断某个 token 是否"看起来是一个选项"，用于 --opt value 形式下的缺值检测。
 * 只把 '--xxx' 与已注册的短选项当选项，这样 --limit -1、--file - 这类取值不受影响。
 *
 * @param string $token 紧随选项之后的 token
 * @return bool
 */
function oo_token_is_option($token)
{
    if (!is_string($token) || $token === '' || $token === '-') {
        return false;
    }
    if (substr($token, 0, 2) === '--') {
        return true;
    }
    if (strlen($token) === 2 && $token[0] === '-' && oo_option_short(substr($token, 1)) !== null) {
        return true;
    }
    return false;
}

/**
 * 读环境变量；未设置或空串都当"没有"，让默认值生效。
 *
 * @param string $name 环境变量名
 * @return string|null
 */
function oo_env($name)
{
    $value = getenv($name);
    if ($value === false || !is_string($value) || $value === '') {
        return null;
    }
    return $value;
}

/**
 * 判断 PHP 数组是不是"列表"（键 0..n-1 连续）。用来区分 JSON 数组与 JSON 对象：
 * json_decode(..., true) 之后两者都是数组，只能靠键判断。
 *
 * @param mixed $arr 待判断的值
 * @return bool
 */
function oo_is_list($arr)
{
    if (!is_array($arr)) {
        return false;
    }
    $expect = 0;
    foreach ($arr as $key => $unused) { // 只读键，不复制值，大数组也不额外占内存
        if ($key !== $expect) {
            return false;
        }
        $expect++;
    }
    return true;
}

/**
 * 给错误信息用的类型名（json 解码后的标量可能是 int/float/bool/null）。
 *
 * @param mixed $value 任意值
 * @return string
 */
function oo_type_name($value)
{
    if (is_array($value)) {
        return oo_is_list($value) ? 'JSON 数组' : 'JSON 对象';
    }
    if (is_object($value)) {
        return 'object';
    }
    if ($value === null) {
        return 'null';
    }
    if (is_bool($value)) {
        return 'bool';
    }
    if (is_int($value)) {
        return 'int';
    }
    if (is_float($value)) {
        return 'float';
    }
    return 'string';
}

/**
 * 解析整数选项。只接受十进制整数字面量（不接受 "1e3" / "10.5"），
 * 这样 --batch=1.5 之类会被明确拒绝而不是悄悄截断。
 *
 * @param mixed  $value 原始值（可能来自 argv 或 null 表示未给）
 * @param string $label 选项名，用于报错
 * @param int    $min   允许的最小值
 * @return int|null
 */
function oo_parse_int($value, $label, $min)
{
    if ($value === null) {
        return null;
    }
    $text = is_string($value) ? trim($value) : '';
    if ($text === '' || !preg_match('/^-?\d+$/', $text)) {
        oo_fail($label . ' 必须是整数，收到: ' . var_export($value, true));
    }
    $number = (int) $text;
    if ($number < $min) {
        oo_fail($label . ' 不能小于 ' . $min . '，收到: ' . $number);
    }
    return $number;
}

/**
 * 解析时间选项（--from / --to）。strtotime 不能解析就报用法错误 ——
 * 静默忽略时间过滤是最容易让调用方误判"推上去了"的坑。
 *
 * @param string|null $value 原始值
 * @param string      $label 选项名
 * @return int|null 时间戳
 */
function oo_parse_time($value, $label)
{
    if ($value === null) {
        return null;
    }
    $text = trim($value);
    if ($text === '') {
        oo_fail($label . ' 不能为空');
    }
    $ts = strtotime($text);
    if ($ts === false) {
        oo_fail($label . ' 时间格式无法解析: ' . $text);
    }
    return $ts;
}

/** stdin 是否来自管道/重定向（true）而不是交互终端（false）。 */
function oo_stdin_is_pipe()
{
    if (function_exists('stream_isatty')) {
        return !@stream_isatty(STDIN);
    }
    if (function_exists('posix_isatty')) { // 老环境（无 stream_isatty 扩展）兜底
        return !@posix_isatty(STDIN);
    }
    return true; // 判断不了就按管道处理：最多读到空内容，不会卡住
}

/**
 * 读 stdin 并缓存。--file=- 与隐式 stdin 共用，避免第二次读拿到空串。
 *
 * @param string|null $cache 缓存变量（引用）
 * @return string
 */
function oo_read_stdin(&$cache)
{
    if ($cache !== null) {
        return $cache;
    }
    $data = @stream_get_contents(STDIN);
    $cache = ($data === false || !is_string($data)) ? '' : $data;
    return $cache;
}

/**
 * 从文件名派生 mre 日志的 job：去掉 .query.log / .write.log / .redis.log / .log 后缀。
 * 后缀必须按长度从长到短尝试，否则 ".query.log" 会先被 ".log" 命中，留下 ".query"。
 *
 * @param string $path 文件路径
 * @return string job 名（派生不出时为空串）
 */
function oo_job_from_filename($path)
{
    $base = basename($path);
    $suffixes = array('.query.log', '.write.log', '.redis.log', '.log'); // 顺序敏感
    foreach ($suffixes as $suffix) {
        $len = strlen($suffix);
        if (strlen($base) > $len && substr($base, -$len) === $suffix) {
            return substr($base, 0, -$len);
        }
    }
    return '';
}

/**
 * 校验一条记录必须是 JSON 对象（解码后的关联数组），不能是标量或 JSON 数组。
 *
 * @param mixed  $record 解码结果
 * @param string $label  出错时定位用的标签（文件名 / 行号）
 * @return array
 */
function oo_normalize_record($record, $label)
{
    if (!is_array($record)) {
        oo_fail($label . '：记录必须是 JSON 对象，收到 ' . oo_type_name($record));
    }
    if ($record !== array() && oo_is_list($record)) {
        oo_fail($label . '：记录不能是 JSON 数组（要对象/字段集合）');
    }
    return $record;
}

/**
 * 解析一段输入文本：首个非空白字符是 '[' 按 JSON 数组整体解析，否则按 JSON Lines。
 * 两种形态都来自实测约束——shim 只接受数组，而文件里常见一行一条。
 *
 * @param string $content 文本内容
 * @param string $label   出错时定位用的标签
 * @return array 记录数组
 */
function oo_parse_json_text($content, $label)
{
    $records = array();
    $trimmed = ltrim($content, " \t\r\n");
    if ($trimmed !== '' && $trimmed[0] === '[') {
        $decoded = json_decode($content, true);
        if (json_last_error() !== JSON_ERROR_NONE) {
            oo_fail($label . '：JSON 数组解析失败（' . json_last_error_msg() . '）');
        }
        if (!is_array($decoded)) {
            oo_fail($label . '：JSON 数组解析结果不是数组，收到 ' . oo_type_name($decoded));
        }
        if ($decoded === array()) {
            return $records; // 空数组：没有记录，不算错误
        }
        if (!oo_is_list($decoded)) {
            $records[] = oo_normalize_record($decoded, $label . '：'); // '[...]' 里套了个对象
            return $records;
        }
        foreach ($decoded as $index => $item) {
            $records[] = oo_normalize_record($item, $label . ' 第 ' . ($index + 1) . ' 条');
        }
        return $records;
    }

    /* JSON Lines：一行一条；空行跳过（文件末尾换行、手工编辑的空白行都很常见） */
    $lines = preg_split("/\r\n|\n|\r/", $content);
    if (!is_array($lines)) {
        return $records;
    }
    foreach ($lines as $offset => $line) {
        $text = trim($line);
        if ($text === '') {
            continue;
        }
        $decoded = json_decode($text, true);
        if (json_last_error() !== JSON_ERROR_NONE) {
            oo_fail($label . ' 第 ' . ($offset + 1) . ' 行不是合法 JSON（'
                . json_last_error_msg() . '）');
        }
        $records[] = oo_normalize_record($decoded, $label . ' 第 ' . ($offset + 1) . ' 行');
    }
    return $records;
}

/**
 * 严格 JSON 编码：不转义中文与斜杠（日志正文里的路径/时间可读），
 * JSON_PRESERVE_ZERO_FRACTION 让 1.0 不会退化成 1（避免悄悄改变日志里的数值语义）。
 *
 * @param mixed  $value 待编码值
 * @param string $label 出错定位标签
 * @return string
 */
function oo_encode_json($value, $label)
{
    $flags = JSON_UNESCAPED_UNICODE | JSON_UNESCAPED_SLASHES | JSON_PRESERVE_ZERO_FRACTION;
    $json = json_encode($value, $flags);
    if ($json === false || !is_string($json)) {
        oo_fail($label . '：JSON 编码失败（' . json_last_error_msg() . '）');
    }
    return $json;
}

/**
 * 把一批记录编码成请求体（JSON 数组）。
 *
 * 空记录必须编成 {} 而不是 []：PHP 空数组 json_encode 出来是 []，而成批 payload 里的
 * 每个元素都应该是对象，[[]] 会被服务端当成"数组元素"解析（写不进去或不报错地丢弃）。
 *
 * @param array  $records 一批记录
 * @param string $label   出错定位标签
 * @return string
 */
function oo_batch_payload(array $records, $label)
{
    $encoded = array();
    foreach ($records as $record) {
        $encoded[] = ($record === array()) ? new stdClass() : $record;
    }
    return oo_encode_json($encoded, $label);
}

/**
 * 响应体摘要：错误信息只带前 200 字节，且压成单行，避免把整个响应灌进 stderr。
 *
 * @param string $body 响应体
 * @return string
 */
function oo_snippet($body)
{
    $text = str_replace(array("\r", "\n", "\t"), ' ', (string) $body);
    $text = trim($text);
    if ($text === '') {
        return '(空响应体)';
    }
    if (strlen($text) > 200) {
        $text = substr($text, 0, 200) . '...';
    }
    return $text;
}

/**
 * 发一个 POST 请求。curl 扩展可用时用 curl，否则回退 stream context。
 *
 * @param string $endpoint 完整接口地址
 * @param string $payload  JSON 数组字符串
 * @param array  $config   user/pass/timeout/insecure
 * @return array{status:int,body:string,error:string}
 */
function oo_http_post($endpoint, $payload, array $config)
{
    if (function_exists('curl_init')) {
        return oo_http_post_curl($endpoint, $payload, $config);
    }
    return oo_http_post_stream($endpoint, $payload, $config);
}

/**
 * curl 路径。
 *
 * @param string $endpoint 完整接口地址
 * @param string $payload  JSON 数组字符串
 * @param array  $config   配置
 * @return array{status:int,body:string,error:string}
 */
function oo_http_post_curl($endpoint, $payload, array $config)
{
    $ch = curl_init();
    if ($ch === false) {
        return array('status' => 0, 'body' => '', 'error' => 'curl_init 失败');
    }
    $headers = array(
        'Content-Type: application/json', // shim 只按 JSON 解析，必须显式声明
        'Accept: application/json',
        'Content-Length: ' . strlen($payload), // 显式长度，避免代理层改成 chunked 传输
    );
    $options = array(
        CURLOPT_URL => $endpoint,
        CURLOPT_POST => true,
        CURLOPT_POSTFIELDS => $payload,
        CURLOPT_RETURNTRANSFER => true,
        CURLOPT_HEADER => false,
        CURLOPT_CONNECTTIMEOUT => $config['timeout'],
        CURLOPT_TIMEOUT => $config['timeout'],
        CURLOPT_HTTPAUTH => CURLAUTH_BASIC, // 认证走 header，不拼 URL（密码不外泄）
        CURLOPT_USERPWD => $config['user'] . ':' . $config['pass'],
        CURLOPT_HTTPHEADER => $headers,
    );
    if ($config['insecure']) {
        $options[CURLOPT_SSL_VERIFYPEER] = false;
        $options[CURLOPT_SSL_VERIFYHOST] = 0;
    }
    curl_setopt_array($ch, $options);
    $body = curl_exec($ch);
    $status = (int) curl_getinfo($ch, CURLINFO_HTTP_CODE);
    $errno = curl_errno($ch);
    $error = '';
    if ($errno !== 0) {
        $error = 'curl(' . $errno . '): ' . curl_error($ch);
    }
    curl_close($ch);
    return array(
        'status' => $status,
        'body' => is_string($body) ? $body : '',
        'error' => $error,
    );
}

/**
 * stream context 回退路径（容器里没有 curl 扩展时）。
 *
 * @param string $endpoint 完整接口地址
 * @param string $payload  JSON 数组字符串
 * @param array  $config   配置
 * @return array{status:int,body:string,error:string}
 */
function oo_http_post_stream($endpoint, $payload, array $config)
{
    $headers = "Content-Type: application/json\r\n"
        . "Accept: application/json\r\n"
        . 'Authorization: Basic ' . base64_encode($config['user'] . ':' . $config['pass']) . "\r\n"
        . 'Content-Length: ' . strlen($payload) . "\r\n";
    $context = stream_context_create(array(
        'http' => array(
            'method' => 'POST',
            'header' => $headers,
            'content' => $payload,
            'timeout' => $config['timeout'],
            'ignore_errors' => true, // 4xx/5xx 也要拿到 body，否则分不清 401 与 400
            'follow_location' => 0,  // 不跟重定向：写入接口重定向基本意味着 URL 写错了
            'protocol_version' => 1.1,
        ),
        'ssl' => array(
            'verify_peer' => !$config['insecure'],
            'verify_peer_name' => !$config['insecure'],
            'allow_self_signed' => $config['insecure'],
        ),
    ));
    $body = @file_get_contents($endpoint, false, $context);
    $status = 0;
    if (isset($http_response_header) && is_array($http_response_header)
            && $http_response_header !== array()) {
        if (preg_match('#^HTTP/\S+\s+(\d{3})#', $http_response_header[0], $matches)) {
            $status = (int) $matches[1];
        }
    }
    $error = '';
    if ($body === false) {
        $last = error_get_last();
        $error = 'stream: ' . (is_array($last) && isset($last['message'])
            ? $last['message'] : '请求失败');
        $body = '';
    }
    return array(
        'status' => $status,
        'body' => $body,
        'error' => $error,
    );
}

/**
 * 解析响应体里的业务码与逐条统计。
 * 只看 status[] 的 successful/failed 累加，不假设只有一个 stream。
 *
 * @param string $body 响应体
 * @return array{parsed:bool,code:int|null,successful:int|null,failed:int|null}
 */
function oo_parse_response($body)
{
    $result = array(
        'parsed' => false,     // body 是否为可解析的 JSON 对象
        'code' => null,        // 服务端业务码，200 才算成功
        'successful' => null,  // status[].successful 累加值
        'failed' => null,      // status[].failed 累加值
    );
    $decoded = json_decode($body, true);
    if (!is_array($decoded)) {
        return $result;
    }
    $result['parsed'] = true;
    if (isset($decoded['code']) && is_numeric($decoded['code'])) {
        $result['code'] = (int) $decoded['code'];
    }
    $successful = 0;
    $failed = 0;
    $seen = false;
    if (isset($decoded['status']) && is_array($decoded['status'])) {
        foreach ($decoded['status'] as $entry) {
            if (!is_array($entry)) {
                continue;
            }
            if (isset($entry['successful']) && is_numeric($entry['successful'])) {
                $successful += (int) $entry['successful'];
                $seen = true;
            }
            if (isset($entry['failed']) && is_numeric($entry['failed'])) {
                $failed += (int) $entry['failed'];
                $seen = true;
            }
        }
    }
    if ($seen) {
        $result['successful'] = $successful;
        $result['failed'] = $failed;
    }
    return $result;
}

/**
 * 一次请求结果的简短描述，用于重试提示。
 *
 * @param array $response oo_http_post 的返回
 * @return string
 */
function oo_resp_brief(array $response)
{
    if ($response['status'] === 0) {
        return '网络错误 ' . ($response['error'] !== '' ? $response['error'] : '未知');
    }
    $text = 'http=' . $response['status'];
    if ($response['body'] !== '') {
        $text .= ' ' . oo_snippet($response['body']);
    }
    return $text;
}

/**
 * 发送一批（含重试）。只对网络错误 / 429 / 5xx 重试，退避 200ms * attempt；
 * 401/403 立刻返回 abort 标记，由调用方中止整个任务。
 *
 * @param string $endpoint   完整接口地址
 * @param string $payload    JSON 数组字符串
 * @param array  $config     user/pass/timeout/insecure/retry
 * @param int    $batchNo    当前批序号（1 起）
 * @param int    $batchTotal 总批数
 * @return array 原响应 + abort/attempt
 */
function oo_send_batch($endpoint, $payload, array $config, $batchNo, $batchTotal)
{
    $maxAttempts = $config['retry'] + 1;
    $response = array('status' => 0, 'body' => '', 'error' => '', 'abort' => false, 'attempt' => 0);
    for ($attempt = 1; $attempt <= $maxAttempts; $attempt++) {
        $response = oo_http_post($endpoint, $payload, $config);
        $response['attempt'] = $attempt;
        $response['abort'] = false;
        $status = $response['status'];
        if ($status === 401 || $status === 403) {
            $response['abort'] = true; // 认证问题：不重试、不继续
            return $response;
        }
        $retryable = ($status === 0 || $status === 429 || $status >= 500);
        if (!$retryable || $attempt >= $maxAttempts) {
            return $response;
        }
        $delayMs = 200 * $attempt; // 退避 200ms * attempt
        oo_notice(sprintf(
            'batch %d/%d 第 %d/%d 次尝试失败（%s），%dms 后重试',
            $batchNo,
            $batchTotal,
            $attempt,
            $maxAttempts,
            oo_resp_brief($response),
            $delayMs
        ));
        usleep($delayMs * 1000);
    }
    return $response;
}

/* ===================== 主流程 ===================== */

/* ---- 1) 参数解析：支持 --opt=value 与 --opt value，-- 终止选项解析 ---- */
$argvList = isset($argv) && is_array($argv) ? $argv : array();
$defs = oo_option_defs();
$raw = array(
    // 连接
    'url' => null,
    'org' => null,
    'stream' => null,
    'user' => null,
    'pass' => null,
    'auth' => null,
    'insecure' => false,
    'timeout' => null,
    'retry' => null,
    // 输入
    'log' => null,
    'level' => null,
    'job' => null,
    'count' => null,
    'file' => array(),
    'glob' => array(),
    'data' => null,
    'mre' => false,
    'from' => null,
    'to' => null,
    'limit' => null,
    // 发送
    'batch' => null,
    'dry_run' => false,
    'print_response' => false,
    'verbose' => false,
    'quiet' => false,
    'help' => false,
    'version' => false,
);
/* 文件类输入保留命令行出现顺序：--file a --glob b --file c 与 --file a --file c --glob b
 * 结果一致（排序无关），但 --limit 截断时顺序会影响"留下哪几条"，所以按出现顺序取。 */
$fileSources = array();

$index = 1;
$countArgs = count($argvList);
$endOfOptions = false;
while ($index < $countArgs) {
    $token = $argvList[$index];
    if (!is_string($token)) {
        $index++;
        continue;
    }
    if ($endOfOptions) {
        oo_fail('不支持位置参数: ' . $token . '（所有输入都通过选项给出）');
    }
    if ($token === '--') {
        $endOfOptions = true;
        $index++;
        continue;
    }
    if ($token === '' || $token === '-') {
        oo_fail('意外参数: ' . var_export($token, true) . '（stdin 请用 --file=-）');
    }
    if ($token[0] !== '-') {
        oo_fail('不支持位置参数: ' . $token . '（所有输入都通过选项给出）');
    }

    $isLong = substr($token, 0, 2) === '--';
    $body = $isLong ? substr($token, 2) : substr($token, 1);
    $inline = null;
    $hasInline = false;
    $eq = strpos($body, '=');
    if ($eq !== false) {
        $inline = substr($body, $eq + 1);
        $hasInline = true;
        $body = substr($body, 0, $eq);
    }

    if ($isLong) {
        $name = $body;
    } else {
        $name = oo_option_short($body);
        if ($name === null) {
            oo_fail('未知参数: -' . $body . '（短选项只有 -h / -v / -q，其余请用 --长选项）');
        }
    }
    $canonical = oo_option_canonical($name);
    if (!isset($defs[$name])) {
        oo_fail('未知参数: ' . $token . '（--help 查看全部参数）');
    }
    $needsValue = $defs[$name];

    if (!$needsValue) {
        if ($hasInline) {
            oo_fail('参数 --' . $name . ' 不带值');
        }
        $raw[$canonical] = true;
        $index++;
        continue;
    }

    /* 取选项值 */
    if ($hasInline) {
        $value = $inline;
    } else {
        if ($index + 1 >= $countArgs) {
            oo_fail('参数 --' . $name . ' 缺少值');
        }
        $next = $argvList[$index + 1];
        if (oo_token_is_option($next)) {
            oo_fail('参数 --' . $name . ' 缺少值（后面跟的是选项 ' . $next . '）');
        }
        $value = is_string($next) ? $next : '';
        $index++;
    }

    if ($canonical === 'file' || $canonical === 'glob') {
        /* --file / --glob 可重复：按出现顺序追加，同时保留类型用于后续展开 */
        $raw[$canonical][] = $value;
        $fileSources[] = array(
            'type' => $canonical, // file | glob
            'value' => $value,
        );
    } else {
        $raw[$canonical] = $value; // 同名重复时后者覆盖（getopt 惯例）
    }
    $index++;
}

/* ---- 2) --help / --version：成功路径，先于一切校验 ---- */
if ($raw['help']) {
    echo oo_help();
    exit(OO_PUSH_EXIT_OK);
}
if ($raw['version']) {
    echo OO_PUSH_NAME . ' ' . OO_PUSH_VERSION . "\n";
    exit(OO_PUSH_EXIT_OK);
}

$oo_push_quiet = $raw['quiet'] ? true : false;
$verbose = $raw['verbose'] ? true : false;

/* ---- 3) 连接配置：CLI > 环境变量 > 默认；--auth 只在 --user/--pass 缺失处补位 ---- */
$authUser = null;
$authPass = null;
if ($raw['auth'] !== null) {
    $authText = $raw['auth'];
    $sep = strpos($authText, ':');
    if ($sep === false) {
        oo_fail('--auth 格式应为 USER:PASS');
    }
    $authUser = substr($authText, 0, $sep);
    $authPass = substr($authText, $sep + 1);
}

$user = $raw['user'] !== null ? trim($raw['user'])
    : ($authUser !== null ? $authUser : oo_env('OO_USER'));
$pass = $raw['pass'] !== null ? $raw['pass']
    : ($authPass !== null ? $authPass
        : (oo_env('OO_PASS') !== null ? oo_env('OO_PASS') : oo_env('OO_PASSWORD')));

$url = $raw['url'] !== null ? trim($raw['url'])
    : (oo_env('OO_URL') !== null ? oo_env('OO_URL') : OO_PUSH_DEFAULT_URL);
$org = $raw['org'] !== null ? trim($raw['org'])
    : (oo_env('OO_ORG') !== null ? oo_env('OO_ORG') : OO_PUSH_DEFAULT_ORG);
$stream = $raw['stream'] !== null ? trim($raw['stream'])
    : (oo_env('OO_STREAM') !== null ? oo_env('OO_STREAM') : OO_PUSH_DEFAULT_STREAM);

if ($url === '' || !preg_match('#^https?://#i', $url)) {
    oo_fail('--url 必须以 http:// 或 https:// 开头: ' . var_export($url, true));
}
if ($org === '' || strpos($org, '/') !== false) {
    oo_fail('--org 不能为空且不能包含 /');
}
if ($stream === '' || strpos($stream, '/') !== false) {
    oo_fail('--stream 不能为空且不能包含 /');
}

$timeout = oo_parse_int($raw['timeout'], '--timeout', 1);
$timeout = ($timeout === null) ? OO_PUSH_DEFAULT_TIMEOUT : $timeout;
$retry = oo_parse_int($raw['retry'], '--retry', 0);
$retry = ($retry === null) ? OO_PUSH_DEFAULT_RETRY : $retry;
$batchSize = oo_parse_int($raw['batch'], '--batch', 1);
$batchSize = ($batchSize === null) ? OO_PUSH_DEFAULT_BATCH : $batchSize;
$count = oo_parse_int($raw['count'], '--count', 1);
$count = ($count === null) ? 1 : $count;
$limit = oo_parse_int($raw['limit'], '--limit', 0);
$fromTs = oo_parse_time($raw['from'], '--from');
$toTs = oo_parse_time($raw['to'], '--to');
$level = ($raw['level'] !== null && trim($raw['level']) !== '')
    ? trim($raw['level']) : OO_PUSH_DEFAULT_LEVEL;
$job = ($raw['job'] !== null && trim($raw['job']) !== '')
    ? trim($raw['job']) : OO_PUSH_DEFAULT_JOB;

$dryRun = $raw['dry_run'] ? true : false;
$printResponse = $raw['print_response'] ? true : false;
$mre = $raw['mre'] ? true : false;

$endpoint = rtrim($url, '/') . '/api/' . rawurlencode($org) . '/' . rawurlencode($stream) . '/_json';

/* ---- 4) 认证校验（dry-run 不发请求，不要求认证，方便先看 payload） ---- */
if (!$dryRun) {
    if ($user === null || $user === '' || $pass === null || $pass === '') {
        oo_fail('缺少认证信息：用 --user/--pass、--auth=USER:PASS 或环境变量 '
            . 'OO_USER/OO_PASS 提供（dry-run 不需要）');
    }
}

/* ---- 5) 读输入：按 --log / --data / 文件顺序收集，记录每条记录的"来源 job 提示" ---- */
$items = array(); // 每项 array('record'=>array,'srcJob'=>string)，srcJob 供 --mre 派生 job
$stdinCache = null;

if ($raw['log'] !== null) {
    for ($seq = 1; $seq <= $count; $seq++) {
        $line = $raw['log'];
        if ($count > 1) {
            $line .= ' #' . $seq; // 多条时带序号，服务端才能区分同一条模板
        }
        $items[] = array(
            'record' => array(
                'level' => $level,                  // 日志级别
                'job' => $job,                      // 任务名
                'log' => $line,                     // 日志正文
                'time' => date('Y-m-d H:i:s'),      // 采集时间（本地时区）
            ),
            'srcJob' => '',                         // 合成记录自带 job，无需派生
        );
    }
}

if ($raw['data'] !== null) {
    $decoded = json_decode($raw['data'], true);
    if (json_last_error() !== JSON_ERROR_NONE) {
        oo_fail('--data 不是合法 JSON（' . json_last_error_msg() . '）');
    }
    /* [] 与 {} 解码后都是空数组，只能回头看原文首字符：'[' 才按"多条记录"处理，
     * 否则 --data='[]' 会被当成一条空记录发出去 */
    $dataText = ltrim($raw['data'], " \t\r\n");
    $dataIsArray = ($dataText !== '' && $dataText[0] === '[');
    if ($dataIsArray && is_array($decoded)) {
        foreach ($decoded as $offset => $entry) {
            $items[] = array(
                'record' => oo_normalize_record($entry, '--data 第 ' . ($offset + 1) . ' 条'),
                'srcJob' => '',
            );
        }
    } elseif (is_array($decoded)) {
        $items[] = array(
            'record' => oo_normalize_record($decoded, '--data'),
            'srcJob' => '',
        );
    } else {
        oo_fail('--data 必须是 JSON 数组或对象，收到 ' . oo_type_name($decoded));
    }
}

$seenFiles = array(); // 绝对路径 => true，--file 与 --glob 重复命中同一文件时只读一次
foreach ($fileSources as $source) {
    $paths = array();
    if ($source['type'] === 'glob') {
        $matches = glob($source['value']);
        if ($matches === false) {
            oo_fail('--glob 展开失败: ' . $source['value']);
        }
        $hasMatch = false;
        foreach ($matches as $match) {
            if (is_file($match)) {
                $paths[] = $match;
                $hasMatch = true;
            }
        }
        if (!$hasMatch) {
            oo_fail('--glob 没有匹配到文件: ' . $source['value']);
        }
    } else {
        $paths[] = $source['value'];
    }

    foreach ($paths as $path) {
        if ($path === '-') {
            $content = oo_read_stdin($stdinCache);
            $label = 'stdin';
            $srcJob = 'stdin'; // --mre 约定：stdin 的 job 固定为 stdin
        } else {
            if (!is_file($path)) {
                oo_fail('--file 不存在或不是普通文件: ' . $path);
            }
            $real = realpath($path);
            $dedupeKey = ($real === false) ? $path : $real;
            if (isset($seenFiles[$dedupeKey])) {
                continue; // 同一个文件被 --file 与 --glob 同时命中：只读一次
            }
            $seenFiles[$dedupeKey] = true;
            $content = @file_get_contents($path);
            if ($content === false || !is_string($content)) {
                oo_fail('--file 读取失败: ' . $path);
            }
            $label = $path;
            $srcJob = oo_job_from_filename($path);
        }
        foreach (oo_parse_json_text($content, $label) as $record) {
            $items[] = array(
                'record' => $record,
                'srcJob' => $srcJob,
            );
        }
    }
}

/* 没有显式输入源：stdin 是管道就读 stdin，否则报用法错误（而不是静默发 0 条） */
if ($raw['log'] === null && $raw['data'] === null && $fileSources === array()) {
    if (!oo_stdin_is_pipe()) {
        oo_fail('没有输入：用 --log / --file / --glob / --data 指定，'
            . '或把 JSON 通过管道喂给 stdin');
    }
    $content = oo_read_stdin($stdinCache);
    foreach (oo_parse_json_text($content, 'stdin') as $record) {
        $items[] = array(
            'record' => $record,
            'srcJob' => 'stdin',
        );
    }
}

/* ---- 6) --mre：补 level / job，按 time 过滤 ---- */
$records = array();
foreach ($items as $item) {
    $record = $item['record'];
    if ($mre) {
        if (!array_key_exists('level', $record)) {
            $type = isset($record['type']) && is_scalar($record['type'])
                ? (string) $record['type'] : '';
            $record['level'] = ($type === 'warn') ? 'warn' : 'info';
        }
        $hasJob = isset($record['job']) && is_scalar($record['job'])
            && (string) $record['job'] !== '';
        if (!$hasJob) {
            // 派生顺序：来源文件名 -> stdin -> --job 兜底
            $record['job'] = ($item['srcJob'] !== '') ? $item['srcJob'] : $job;
        }
    }
    if ($fromTs !== null || $toTs !== null) {
        $keep = true;
        if (isset($record['time']) && is_scalar($record['time'])
                && (string) $record['time'] !== '') {
            $ts = strtotime((string) $record['time']);
            if ($ts !== false) {
                if ($fromTs !== null && $ts < $fromTs) {
                    $keep = false;
                }
                if ($toTs !== null && $ts > $toTs) {
                    $keep = false;
                }
            }
            // 解析不了的时间按原样保留：宁可多推也不要悄悄丢掉日志
        }
        if (!$keep) {
            continue;
        }
    }
    $records[] = $record;
}

if ($limit !== null && count($records) > $limit) {
    $records = array_slice($records, 0, $limit);
}

$totalRecords = count($records);

/* ---- 7) 分批。payload 统一在发送前一次性编码，dry-run 与真实发送同一条路径 ---- */
$batches = ($records === array()) ? array() : array_chunk($records, $batchSize);
$totalBatches = count($batches);

$httpConfig = array(
    'user' => ($user === null) ? '' : $user,      // Basic 认证用户名
    'pass' => ($pass === null) ? '' : $pass,      // Basic 认证密码（任何输出都不回显）
    'timeout' => $timeout,                        // 单次请求超时（秒）
    'insecure' => $raw['insecure'] ? true : false, // 跳过 TLS 校验
    'retry' => $retry,                            // 重试次数
);

if ($verbose) {
    oo_notice(sprintf(
        'endpoint=%s org=%s stream=%s records=%d batches=%d batch=%d retry=%d timeout=%ds%s',
        $endpoint,
        $org,
        $stream,
        $totalRecords,
        $totalBatches,
        $batchSize,
        $retry,
        $timeout,
        $dryRun ? ' dry-run' : ''
    ));
    oo_notice('auth_user=' . ($httpConfig['user'] === '' ? '(未设置)' : $httpConfig['user'])
        . ' auth_pass=' . ($httpConfig['pass'] === '' ? '(未设置)' : '***'));
}

/* ---- 8) dry-run：只打 payload，不发请求 ---- */
if ($dryRun) {
    foreach ($batches as $offset => $batchRecords) {
        echo oo_batch_payload($batchRecords, 'batch ' . ($offset + 1)) . "\n";
    }
    oo_notice(sprintf('dry-run：共 %d 条 / %d 批，未发送任何请求', $totalRecords, $totalBatches));
    exit(OO_PUSH_EXIT_OK);
}

/* ---- 9) 发送 + 统计 ---- */
$startedAt = microtime(true);
$sent = 0;
$failedRecords = 0;
$batchesFailed = 0;
$responses = array();

foreach ($batches as $offset => $batchRecords) {
    $batchNo = $offset + 1;
    $batchLen = count($batchRecords);
    $payload = oo_batch_payload($batchRecords, 'batch ' . $batchNo);
    $t0 = microtime(true);
    $response = oo_send_batch($endpoint, $payload, $httpConfig, $batchNo, $totalBatches);
    $elapsedMs = (int) round((microtime(true) - $t0) * 1000);

    if ($response['abort']) {
        oo_error('HTTP ' . $response['status'] . ' 认证失败，立即中止（不重试）：'
            . oo_snippet($response['body']));
        exit(OO_PUSH_EXIT_HTTP);
    }

    $parsed = oo_parse_response($response['body']);
    $httpOk = ($response['status'] === 200 && $parsed['code'] === 200);
    if ($httpOk) {
        $okCount = ($parsed['successful'] === null) ? $batchLen : $parsed['successful'];
        $failCount = ($parsed['failed'] === null) ? 0 : $parsed['failed'];
        /* 服务端数字加起来对不上批大小：差额按失败计（宁可报失败也不藏记录） */
        $gap = $batchLen - $okCount - $failCount;
        if ($gap > 0) {
            $failCount += $gap;
        }
        $sent += $okCount;
        $failedRecords += $failCount;
        if ($failCount > 0) {
            $batchesFailed++;
            oo_error(sprintf(
                'batch %d/%d 服务端报告 failed=%d（successful=%d）',
                $batchNo,
                $totalBatches,
                $failCount,
                $okCount
            ));
        }
    } else {
        $batchesFailed++;
        $failedRecords += $batchLen; // HTTP / 业务码不 200：拿不到逐条信息，整批按失败
        $detail = ($response['status'] === 0)
            ? $response['error']
            : ('http=' . $response['status'] . ' body=' . oo_snippet($response['body']));
        oo_error(sprintf(
            'batch %d/%d 失败（%d 条）：%s',
            $batchNo,
            $totalBatches,
            $batchLen,
            $detail
        ));
    }

    if ($verbose) {
        oo_notice(sprintf(
            'batch %d/%d records=%d http=%d code=%s successful=%s failed=%s %dms attempts=%d',
            $batchNo,
            $totalBatches,
            $batchLen,
            $response['status'],
            ($parsed['code'] === null ? 'n/a' : (string) $parsed['code']),
            ($parsed['successful'] === null ? 'n/a' : (string) $parsed['successful']),
            ($parsed['failed'] === null ? 'n/a' : (string) $parsed['failed']),
            $elapsedMs,
            $response['attempt']
        ));
    }

    if ($printResponse) {
        $responses[] = array(
            'batch' => $batchNo,               // 批序号，1 起
            'http_status' => $response['status'], // HTTP 状态码
            'attempts' => $response['attempt'],   // 实际尝试次数
            'body' => $response['body'],          // 原始响应体（字符串，未解析）
        );
    }
}

$elapsedTotalMs = (int) round((microtime(true) - $startedAt) * 1000);
$ok = ($sent === $totalRecords && $failedRecords === 0 && $batchesFailed === 0);

$summary = array(
    'ok' => $ok,                    // 是否全部成功
    'url' => $url,                  // 接口 base
    'org' => $org,                  // 组织
    'stream' => $stream,            // 流
    'records' => $totalRecords,     // 待发送记录总数
    'sent' => $sent,                // 服务端确认成功条数
    'failed' => $failedRecords,     // 失败条数
    'batches' => $totalBatches,     // 批总数
    'elapsed_ms' => $elapsedTotalMs, // 总耗时
);
if ($printResponse) {
    $summary['responses'] = $responses; // 每批原始响应（纯字符串，机器可读）
}

/* 汇总只在"有记录成功"或"全部成功"时输出：全失败属于错误路径，stdout 不留噪声 */
if ($ok || $sent > 0) {
    echo oo_encode_json($summary, '汇总') . "\n";
}

if ($ok) {
    exit(OO_PUSH_EXIT_OK);
}
if ($sent > 0) {
    exit(OO_PUSH_EXIT_PARTIAL); // 部分成功：有落库也有失败
}
exit(OO_PUSH_EXIT_HTTP);        // 全失败：网络/HTTP 错误
