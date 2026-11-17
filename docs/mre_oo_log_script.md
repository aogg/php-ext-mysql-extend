# mre_oo_log_script.php —— 扩展日志脚本：本地路由 + OpenObserve 推送

一句话：`scripts/mre_oo_log_script.php` 是 mre 扩展 `mre.log_script` 的增强版脚本 ——
**本地日志照旧按 `mre_log_router.php` 落盘，同时把每条记录（补 `level`/`job`）推送到
OpenObserve 风格写入接口**。任何推送失败都静默降级，绝不影响本地日志与业务请求。

- 运行环境：PHP 7.4（扩展在同一请求进程中 include 本脚本）；有 curl 扩展走 curl，
  没有则回退 `stream_context_create`（`Authorization: Basic` 手动拼头）。
- 接口：`POST {url}/api/{org}/{stream}/_json`，HTTP Basic 认证；
  body **必须是 JSON 数组**（JSON Lines 会 400）。
  成功 `200 + {"code":200,"status":[{"name":"default","successful":N,"failed":0}]}`；
  认证失败 401；body 非法 JSON 400。
- 依赖文件：本脚本用 `include __DIR__ . '/mre_log_router.php'` 取原路由结果，
  所以**两个文件必须放在同一目录**；路由脚本缺失时本脚本返回 `null`（降级到
  yaml 规则 / `mre.log_file`），推送逻辑照常。

## 一、用途与契约

### 被扩展如何调用

扩展对**每条记录**（mysql 请求/响应、redis 命令/响应、warn）写日志前 include 一次本脚本：

```text
try { $___mre_ret = include '/path/to/mre_oo_log_script.php'; } catch (Throwable $e) { ... }
```

脚本 `return '绝对路径'` 决定**本条记录写哪个本地文件**；返回 `null` 表示降级到
yaml 规则 / `mre.log_file`。

### 本脚本的返回值

1. 先 `include __DIR__ . '/mre_log_router.php'`，把它的返回值**原样返回**：
   `string` = 本地日志绝对路径；`null` = 降级。
2. 路由文件不存在、或路由脚本抛异常 → 返回 `null`（不影响业务，可选 debug 记录原因）。

### 防御性约定（必须遵守，否则会破坏请求）

- 被同一进程重复 include：所有具名函数都包在 `function_exists()` 里，不声明类、
  不定义常量，绝不触发 `Cannot redeclare`；
- 不 `echo`/`print`（扩展侧另有 ob 捕获兜底）、不 `exit`/`die`、不抛异常；
- 除扩展注入的 `$context`（及路由脚本本就读取的 `$_SERVER`）外不依赖其他全局变量；
  请求级状态只放 `$GLOBALS['mre_oo_state']`（请求结束自动回收，供 shutdown 回调取用）；
- 不使用 `___mre_` 前缀的全局变量（扩展保留名）；
- 推送链路上所有网络/编码/配置错误都在脚本内吞掉，只落 debug 文件。

## 二、记录加工

推送的记录 = `$context` 原样复制（`type`/`dir`/`content`/`extra`/`headers`/`post`/
`cookie`/`get`/`session_id`/`uri`/`method`/`pid`/`request_id`/`script` 等字段全部保留）
**再补两个字段**：

| 字段 | 来源 | 规则 |
|---|---|---|
| `level` | `$context['type']` | `type == "warn"` → `warn`，否则 `info`；`$context` 里已有非空 `level` 时不覆盖 |
| `job` | 路由返回的**本地日志路径** basename | 依次去掉 `.query.log` / `.write.log` / `.redis.log` / `.log`（**长后缀优先**）；路径为 `null`（如 warn 记录、路由降级）时用 `mre`；已有非空 `job` 时不覆盖 |

示例（`/api/user/list` 的 SELECT 记录）：

```json
{
  "type": "mysql", "dir": "req", "content": "SELECT 1",
  "extra": {"cmd": "query", "rows": 1},
  "pid": 4242, "request_id": "4242-1",
  "script": "/opt/app/public/index.php",
  "level": "info", "job": "api-user-list"
}
```

`job` 与本地日志文件名一一对应：`api-user-list.query.log` → `job=api-user-list`，
`cli.redis.log` → `job=cli`；路由降级（`mre.log_file`）→ `job=mre`。

## 三、配置

优先级：**单键环境变量 > JSON 配置文件 > 内置默认值**。

- 配置文件路径：默认 `/usr/local/etc/mre-oo.json`，可用 `MRE_OO_CONFIG` 指定别的路径
  （只改路径，不是单键覆盖）。
- 配置文件内容是一个 JSON 对象，键名与下表一致；未知键忽略（避免拼错键名悄悄生效）。
- 单键环境变量值为空串时视为未设置，继续用文件/默认值。

| 键 | 环境变量 | 默认 | 说明 |
|---|---|---|---|
| `enable` | `MRE_OO_ENABLE` | `1` | 推送总开关。`0` = 只写本地日志、不推送（本地路由照旧执行） |
| `url` | `MRE_OO_URL` | `http://exclude.proxy.mac.path.openobserve.port.5080.proxy_sslip.192.168.137.2.sslip.io` | 写入接口 base；脚本自行拼 `/api/{org}/{stream}/_json`；必须以 `http://` / `https://` 开头，否则不推送（`bad_url`） |
| `org` | `MRE_OO_ORG` | `default` | 组织名；为空回退 `default`，含 `/` 视为配置错误（不推送，`bad_org_stream`） |
| `stream` | `MRE_OO_STREAM` | `default` | 流名；规则同上 |
| `user` | `MRE_OO_USER` | 无（空） | HTTP Basic 用户名；为空 = 不推送（`no_user`） |
| `pass` | `MRE_OO_PASS` | 无（空） | HTTP Basic 密码；为空 = 不推送（`no_pass`）；任何输出/debug 都不回显 |
| `timeout` | `MRE_OO_TIMEOUT` | `2` | 单次请求总超时（秒，1~30），同时作为 connect timeout 的上限 |
| `insecure` | `MRE_OO_INSECURE` | `1` | `1` = 跳过 TLS 证书校验（等价 `curl -k`：`CURLOPT_SSL_VERIFYPEER=false` + `VERIFYHOST=0`；stream 分支对应 `verify_peer/verify_peer_name=false`）；`0` = 正常校验 |
| `batch` | `MRE_OO_BATCH` | `1` | `1` = 请求内累积、shutdown 时合并发送；`0` = 每条记录即时单条 POST |
| `debug` | `MRE_OO_DEBUG` | `0` | `1` = 把推送结果/跳过原因写进 `debug_file`（一行一个 JSON，**不含密码**） |
| `debug_file` | 无 | `/tmp/mre-oo-push.log` | debug 输出文件；追加写 + `LOCK_EX` |
| `max_buffer` | 无 | `1000` | 缓冲上限（1~100000）：达到即提前 flush，防止长时间请求内存膨胀 |

`enable`/`insecure`/`batch`/`debug` 接受 `1`/`true`/`on`/`yes` 为真，其余为假。

配置示例（`/usr/local/etc/mre-oo.json`）：

```json
{
  "enable": 1,
  "url": "http://exclude.proxy.mac.path.openobserve.port.5080.proxy_sslip.192.168.137.2.sslip.io",
  "org": "default",
  "stream": "default",
  "user": "your-user",
  "pass": "your-pass",
  "timeout": 2,
  "insecure": 1,
  "batch": 1,
  "debug": 0,
  "debug_file": "/tmp/mre-oo-push.log",
  "max_buffer": 1000
}
```

只给环境变量的部署（docker/编排器）等价：

```bash
MRE_OO_CONFIG=/usr/local/etc/mre-oo.json   # 可选，改配置文件路径
MRE_OO_ENABLE=1
MRE_OO_URL=http://...                      # 默认值即为目标 base，通常不用写
MRE_OO_ORG=default
MRE_OO_STREAM=default
MRE_OO_USER=your-user
MRE_OO_PASS=your-pass
MRE_OO_TIMEOUT=2
MRE_OO_INSECURE=1
MRE_OO_BATCH=1
MRE_OO_DEBUG=0
```

## 四、部署步骤

1. **拷贝两个文件到同一目录**（例如 `/opt/mre/scripts/`）：

   ```bash
   install -m 0644 scripts/mre_log_router.php scripts/mre_oo_log_script.php /opt/mre/scripts/
   ```

   本脚本用 `__DIR__ . '/mre_log_router.php'` 找路由脚本，**分开放会导致本地日志全部降级**。

2. **把 `mre.log_script` 指到本脚本**（php.ini 或 conf.d，例如 `/usr/local/etc/php/conf.d/zz-mre.ini`）：

   ```ini
   mre.enable = 1
   mre.log_file = "/var/log/mre/mre.log"
   mre.log_format = json
   mre.log_script = "/opt/mre/scripts/mre_oo_log_script.php"
   ; 或用环境变量（优先级更高）：MRE_LOG_SCRIPT=/opt/mre/scripts/mre_oo_log_script.php
   ```

3. **写配置文件**（见上节示例；`chmod 640`、属主为 fpm 运行用户，里面含密码）：

   ```bash
   install -m 0640 -o www-data -g www-data /path/to/mre-oo.json /usr/local/etc/mre-oo.json
   ```

4. **重载 fpm**（改 ini 必须 reload，不是 restart 也行，但 reload 更平滑）：

   ```bash
   kill -USR2 "$(cat /usr/local/var/run/php-fpm.pid)"
   # 或 docker: docker exec <容器> kill -USR2 1
   # 或 service php-fpm reload
   ```

5. **用 debug 日志验证**（先 `MRE_OO_DEBUG=1`，或配置文件里 `"debug": 1`）：

   ```bash
   curl -s 'http://<host>/api/user/list?a=1'
   tail -n 20 /tmp/mre-oo-push.log
   ```

   看到 `{"event":"send",...,"http_code":200,"ok":true,"response":"{\"code\":200,\"successful\":N,\"failed\":0}"}`
   即表示推送成功；同时确认本地日志文件（`runtime/mre-logs/api-user-list.query.log`）照常在写。

6. 验证通过后把 `debug` 改回 `0`（debug 打开时每条记录都会多一次追加写：`file_put_contents(..., FILE_APPEND|LOCK_EX)`）。

### 上线前自检（不影响线上）

```bash
# 语法检查
php -d xdebug.mode=off -l /opt/mre/scripts/mre_oo_log_script.php
```

## 五、行为说明（发送时机）

### `batch=1`（默认）：请求内累积 + shutdown 合并发送

- 首次有记录入队时 `register_shutdown_function()` 注册**一次** flush（一个请求只注册一次）。
- 请求结束时 flush 把所有记录**合并成一个 JSON 数组、一次 curl POST**（不是一条一个请求）。
- flush 时若 PHP 有 `fastcgi_finish_request()`（FPM 常态）先 `@fastcgi_finish_request()`：
  响应先交给客户端，日志推送发生在客户端拿到响应之后，不增加用户等待时间。

### late record：flush 之后到达的记录

`fastcgi_finish_request()` 之后请求仍在执行（如扩展 `mre_gtx_*` 收尾阶段补写的记录），
此时 shutdown flush 已经跑过，缓冲不会再被发送。本脚本标记「已 flush」，
此后到达的记录**按单条即时发送**（每条一次 POST），保证不丢尾记录。

### `max_buffer` 提前 flush

缓冲达到 `max_buffer`（默认 1000 条）时立即发送一次，避免单个长请求把记录全攒在内存里。
**注意这是一次请求内的同步 curl**：最坏情况会阻塞当前请求 `timeout` 秒（默认 2 秒）。
因此 `max_buffer` 不要设得太小（设成 1 等于退化成逐条同步发送）。

### `batch=0`：每条即时单条发送

每条记录立即 POST 一次（不注册 shutdown 回调）。适合记录量小、更看重实时性的场景；
记录量大时请求数会显著增加。

### 失败语义

- 超时、连接失败、401/403、4xx/5xx、`json_encode` 失败、配置不全：**一律不重试、不抛异常**，
  只在 debug 打开时记一行原因；本地日志与业务响应不受任何影响。
- 缺 `user`/`pass`、`enable=0`、`url` 非法、`org`/`stream` 含 `/`：完全不推送，连缓冲都不做
  （同一请求只记一次 skip 原因，避免刷屏）。

### debug 行示例

```json
{"time":"2026-11-17T11:15:36+08:00","pid":397329,"event":"send","endpoint":"http://.../api/default/default/_json","count":1,"body_bytes":226,"http_code":0,"ok":false,"error":"curl(7): Failed to connect to 127.0.0.1 port 1 after 3 ms: Couldn't connect to server","response":"","cost_ms":12.1}
{"time":"2026-11-17T11:15:36+08:00","pid":397329,"event":"skip","reason":"no_user","count":1}
```

`event` 取值：`config`（配置文件缺失/JSON 非法）、`skip`（不推送及原因）、
`send`（一次 POST 的结果）、`router_failed`（路由脚本异常）、`push_failed`/`flush_failed`（兜底）。

## 六、注意与限制

- **超时后 worker 占用**：`batch=0` 或 `max_buffer` 提前 flush 时，丢包/服务不可达会让当前
  PHP worker 最多阻塞 `timeout` 秒（默认 2 秒；会占用该请求的 fpm worker）。shutdown flush
  在 `fastcgi_finish_request()` 之后，阻塞的是 worker 而不是客户端。生产建议
  `timeout` ≤ 2、`max_buffer` 保持默认量级。
- **无幂等键 / 无重试**：接口没有幂等键，脚本也不重试。所以「发失败 = 丢这一批」，
  不会重复写入，也不会无限重试拖垮 worker。记录本身带 `pid` + `request_id`，可用于去重排查。
- **本地日志不受影响**：推送链路完全独立于本地写日志。即使日志服务全挂、`user/pass` 配错、
  `enable=0`，本地 `runtime/mre-logs/*.log` 照常写；反过来，路由脚本异常也只是让本地日志降级，
  推送逻辑不因此中断（两边互不阻塞、互不依赖）。
- **脚本内 `exit()`** 会终止请求（扩展本身的限制），本脚本不调用 `exit`/`die`。
- **递归保护**：脚本内部发起的 SQL/redis 不会再次回调本脚本（扩展侧防重入），因此不会出现
  「推送 → 产生日志 → 再推送」的递归。
- **密码安全**：只通过 `CURLOPT_USERPWD` / `Authorization` 头发送，不进 URL、不进 debug、
  不进异常信息；配置文件的权限请自行收紧（640）。
- **单条记录的字段**：`extra` 等嵌套对象原样发（接口实测接受嵌套对象）；若 `$context` 含非法
  UTF-8 字节（如二进制参数），这些字节会被替换为 U+FFFD（`JSON_INVALID_UTF8_SUBSTITUTE`），
  **不会导致整批丢弃**；其余编码错误仍会失败并记 `skip/json_encode_failed`。
- **与 `scripts/oo_push.php` 的分工**：`oo_push.php` 是离线补发/批量导入的 CLI（可读历史日志文件
  重推）；本脚本是**在线**推送（扩展每条记录实时推）。两者复用同一套接口语义与 `level`/`job` 派生规则。
