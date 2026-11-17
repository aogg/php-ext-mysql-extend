# oo_push.php —— OpenObserve 风格写入接口的日志推送 CLI

一句话：`scripts/oo_push.php` 把日志记录（命令合成 / JSONL 文件 / JSON 数组 / stdin）分批 POST 到
OpenObserve 风格的 `_json` 写入接口，只做写入、不做查询。

- 运行环境：PHP 7.4+、纯 CLI、单文件、无 composer 依赖；有 curl 扩展走 curl，没有则回退
  `stream_context_create`（两条路径行为一致）。
- 接口路径：`POST {base}/api/{org}/{stream}/_json`，HTTP Basic 认证。
- 请求体：**必须是 JSON 数组**（`[{"a":1},{"a":2}]`）。服务端实测为 OpenSearch 7.17.1 + 兼容 shim，
  给它 JSON Lines（一行一条）会返回 400 `SerdeJsonError# trailing characters`，因此脚本内部永远先聚成
  数组再按 `--batch` 切分。
- 成功响应：HTTP 200 + `{"code":200,"status":[{"name":"default","successful":N,"failed":0}]}`。
  脚本统计 `status[].successful` / `status[].failed` 累加值作为 `sent` / `failed`。
- 认证失败：401 `Unauthorized Access` → 立即中止、退出码 2、不重试；403 同样处理。
- 请求体非法 JSON：400 → 该批按失败计。
- 该账号的查询接口（`_search` 等）返回 401，所以本脚本不实现查询。

## 参数

支持 `--opt=value` 与 `--opt value` 两种写法；`--` 之后不再解析选项。同名选项重复出现时后者生效
（`--file` / `--glob` 例外，它们是可重复累积的）。未在命令行给出的连接参数按「环境变量 → 内置默认」补齐。

### 连接

| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `--url=URL` | string | 环境变量 `OO_URL`，否则 `http://exclude.proxy.mac.path.openobserve.port.5080.proxy_sslip.192.168.137.2.sslip.io` | 接口 base，必须以 `http://` / `https://` 开头；脚本自行拼 `/api/{org}/{stream}/_json` |
| `--org=NAME` | string | 环境变量 `OO_ORG`，否则 `default` | 组织名，不能为空、不能含 `/` |
| `--stream=NAME` | string | 环境变量 `OO_STREAM`，否则 `default` | 流名，不能为空、不能含 `/` |
| `--user=USER` | string | 环境变量 `OO_USER` | HTTP Basic 用户名；与 `--pass` 一起必填（`--dry-run` 除外） |
| `--pass=PASS` | string | 环境变量 `OO_PASS`，兼容 `OO_PASSWORD` | HTTP Basic 密码；任何输出都不回显 |
| `--auth=USER:PASS` | string | 无 | 一次给齐用户名密码；`--user`/`--pass` 优先级更高，缺哪项由 `--auth` 补哪项 |
| `--insecure` | flag | 关闭 | 跳过 TLS 证书校验（`CURLOPT_SSL_VERIFYPEER/VERIFYHOST` 或 `ssl.verify_peer`） |
| `--timeout=SEC` | int | `10` | 单次请求超时（秒），必须 ≥ 1 |
| `--retry=N` | int | `2` | 失败重试次数，必须 ≥ 0；仅对网络错误 / 429 / 5xx 重试，退避 `200ms × attempt`，重试前在 stderr 提示 |

### 输入

| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `--log=TEXT` | string | 无 | 生成合成记录正文；生成条数由 `--count` 决定，多条时正文追加 ` #i`（i 从 1 起） |
| `--level=LEVEL` | string | `info` | 合成记录的 `level` 字段 |
| `--job=JOB` | string | `oo_push` | 合成记录的 `job` 字段；`--mre` 下也作为派生不出的 job 兜底 |
| `--count=N` | int | `1` | 合成记录条数，必须 ≥ 1（仅配合 `--log` 生效） |
| `--file=PATH` | string，可重复 | 无 | 读文件；`-` 表示 stdin。文件首个非空白字符是 `[` 按 JSON 数组整体解析，否则按 JSON Lines 逐行解析 |
| `--glob=PATTERN` | string，可重复 | 无 | 用 `glob()` 展开成文件列表（只取普通文件），与 `--file` 合并；同一文件只读一次；匹配不到文件报错 |
| `--data=JSON` | string | 无 | 命令行内联 JSON，数组或单个对象都行 |
| `--mre` | flag | 关闭 | 按 mre 扩展日志处理：补 `level`/`job`，并按 `time` 过滤；原有字段原样保留 |
| `--from=TIME` | string | 无 | 只保留 `time >= TIME` 的记录（`strtotime` 可解析）；无法解析的时间原样保留、不参与过滤 |
| `--to=TIME` | string | 无 | 只保留 `time <= TIME` 的记录；规则同 `--from` |
| `--limit=N` | int | 不限 | 总记录上限，必须 ≥ 0；`0` 表示一条都不发 |

### 发送

| 参数 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `--batch=N` | int | `200` | 每个请求的记录数，必须 ≥ 1 |
| `--dry-run` | flag | 关闭 | 只把每批 payload（JSON 数组）打到 stdout，不发请求，退出 0；不要求认证 |
| `--print-response` | flag | 关闭 | 在汇总 JSON 里附带 `responses` 数组（每批 HTTP 状态、尝试次数、原始响应体） |
| `--verbose` / `-v` | flag | 关闭 | 在 stderr 打连接信息与每批进度（用户名可见、密码只显示 `***`） |
| `--quiet` / `-q` | flag | 关闭 | 关闭非错误提示（重试提示、进度）；错误照常打 stderr |
| `--help` / `-h` | flag | — | 打印用法与参数表，退出 0 |
| `--version` | flag | — | 打印版本，退出 0 |

### 环境变量

| 变量 | 对应参数 | 说明 |
|---|---|---|
| `OO_URL` | `--url` | 命令行优先 |
| `OO_ORG` | `--org` | 命令行优先 |
| `OO_STREAM` | `--stream` | 命令行优先 |
| `OO_USER` | `--user` | 命令行优先 |
| `OO_PASS` | `--pass` | 命令行优先；`OO_PASSWORD` 作为兼容名，两者都在时 `OO_PASS` 优先 |

## 输入来源与解析规则

1. 收集顺序：`--log` → `--data` → `--file`/`--glob`（按命令行出现顺序）→ 隐式 stdin。
2. 一个输入源都没给时：stdin 是管道/重定向就读 stdin，是交互终端则报用法错误（不会静默发 0 条）。
3. 单个输入文件的判定：`ltrim` 后首个字符是 `[` → 整段当 JSON 数组解析；否则按 JSON Lines
   （`\r\n` / `\n` / `\r` 分行），空行跳过，**任何一行非法 JSON 就报错并退出 1**（带行号）。
4. 每条记录必须是 JSON 对象；数组元素是标量或子数组时报错（带序号）。
5. `--mre` 规则：
   - `level`：记录里没有 `level` 时补——`type == "warn"` → `warn`，否则 `info`；已有 `level` 不覆盖。
   - `job`：记录里没有 `job` 时补——先取来源文件名去掉 `.query.log` / `.write.log` / `.redis.log` /
     `.log` 后缀（按此顺序，长后缀优先）；stdin 来源用 `stdin`；仍派生不出时用 `--job`。
   - 其余字段（`type`、`extra`、`time`、嵌套对象等）原样保留。
6. `--limit` 在 `--mre` 过滤之后执行，保留前 N 条。
7. 记录内容不做任何改写；`--mre` 之外的输入按原样发送。

## 输出

- 成功或部分成功时，stdout 输出**一行** JSON 汇总：

  ```json
  {"ok":true,"url":"http://...","org":"default","stream":"default","records":3,"sent":3,"failed":0,"batches":1,"elapsed_ms":12}
  ```

  | 字段 | 含义 |
  |---|---|
  | `ok` | 全部成功为 `true`（`sent == records` 且 `failed == 0` 且无失败批次） |
  | `url` / `org` / `stream` | 实际使用的连接参数 |
  | `records` | 待发送记录总数（过滤、去重、截断之后） |
  | `sent` | 服务端确认成功条数（`status[].successful` 累加） |
  | `failed` | 失败条数（服务端 `failed` 累加；整批失败时按批大小计） |
  | `batches` | 批总数 |
  | `elapsed_ms` | 发送阶段总耗时（毫秒） |

- `--print-response` 时汇总里多一个 `responses`：`[{"batch":1,"http_status":200,"attempts":1,"body":"<原始响应体>"}]`，
  `body` 是未解析的原始字符串，保持机器可读。
- `--dry-run` 只输出每批 payload（每行一个 JSON 数组），便于 `> out.json` 后直接 `curl` 复现。
- 错误一律写 stderr（前缀 `oo_push:`），stdout 不留错误文本；401/403 中止与全失败时不输出汇总。
- 密码永不回显：认证走 `CURLOPT_USERPWD` / `Authorization` 头，不拼进 URL；`--verbose` 也只有用户名。

## 退出码

| 退出码 | 含义 | 典型场景 |
|---|---|---|
| `0` | 全部成功 | `sent == records` 且服务端 `failed == 0` |
| `1` | 用法 / 配置 / 输入解析错误 | 缺认证信息、`--file` 不存在、`--batch=0`、`--limit<0`、时间写错、JSON 行非法、既没输入又不是管道 |
| `2` | 认证或网络 / HTTP 错误 | 401/403（立即中止、不重试）、重试后仍网络失败、非 200 且整批失败 |
| `3` | 部分失败 | 服务端 `failed > 0`，或部分批次失败但已有记录成功 |

## 示例

单条发送（合成记录）：

```bash
php scripts/oo_push.php \
  --log='cache miss for user 42' --level=warn --job=demo \
  --user=admin --pass=secret
```

JSONL 文件 + `--mre`（按 URL 路径分文件、自动补 level/job）：

```bash
php scripts/oo_push.php \
  --file=/code/www/my/project/runtime/mre-logs/api-user.query.log --mre \
  --user=admin --pass=secret
```

`--dry-run` 预览 payload，不发请求、不需要认证：

```bash
php scripts/oo_push.php --log='hello' --count=3 --batch=2 --dry-run
# -> [{"level":"info","job":"oo_push","log":"hello #1","time":"..."}, ...]
# -> [{"level":"info","job":"oo_push","log":"hello #3","time":"..."}]
```

stdin（管道）：

```bash
printf '%s\n' '{"level":"info","job":"pipe","log":"a"}' '{"level":"error","job":"pipe","log":"b"}' \
  | php scripts/oo_push.php --user=admin --pass=secret
```

容器内（宿主 `/code/www` 挂在容器 `/app/origin`，PHP 7.4 容器里执行）：

```bash
docker exec 7.4-1-alpine-php-dev php -d xdebug.mode=off \
  /app/origin/my/github/php-ext-mysql-extend/scripts/oo_push.php \
  --glob='/app/origin/my/github/php-ext-mysql-extend/runtime/mre-logs/*.log' --mre \
  --url=http://exclude.proxy.mac.path.openobserve.port.5080.proxy_sslip.192.168.137.2.sslip.io \
  --org=default --stream=default --user=admin --pass=secret --insecure --verbose
```

配合 `--limit` 与时间窗口（mre 日志的 `time` 字段）：

```bash
php scripts/oo_push.php --file=/app/origin/my/project/runtime/mre-logs/api.query.log --mre \
  --from='2026-11-17 00:00:00' --to='2026-11-17 23:59:59' --limit=500 \
  --user=admin --pass=secret
```

## 备注

- 认证失败（401/403）意味着 org/stream 或账号密码不对，脚本不重试——重试只会持续撞墙，交给调用方修配置。
- 服务端返回 `successful` 与 `failed` 之和对不上批大小时，差额按失败计（宁可报失败也不藏记录）。
- `--insecure` 只关校验、不改协议；生产接口请走 https 并保持校验开启。
- 脚本不做查询：该账号的 `_search` 返回 401，需要检索请用别的凭据/工具。
