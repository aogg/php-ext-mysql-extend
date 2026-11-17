# mre — mysql-redis-extend

一个 PHP C 扩展（扩展名 `mre`），在 PHP 进程内部捕获并记录：

- **MySQL 流量**：通过 mysqlnd 扩展点拦截，覆盖 **mysqli、PDO_MYSQL**（两者底层都走 mysqlnd）以及一切基于 mysqlnd 的连接类；记录 SQL 文本、执行起止时间与耗时（`extra.time_end`/`extra.cost_ms`）、影响行数/返回行数、错误码/错误信息，返回行内容可按条数采样（默认 1 行）。
- **Redis 流量**：通过 PHP streams 传输层嗅探 RESP 协议，覆盖 **phpredis（Redis/RedisCluster 扩展类）与 Predis（用户态类）**——两者建连最终都走 PHP streams，记录发送的命令与返回的响应。
- 日志一行一条，默认 JSON lines，`flock` 保证多进程（fpm）并发安全；支持写入文件、`stdout`、`stderr` 以及 **`/proc/1/fd/1`（docker 容器内输出到 pid1 stdout）**。

兼容 PHP **7.4 与 8.0 ~ 8.4**（源码内用 `PHP_VERSION_ID` 宏分支处理 API 差异），无任何外部库依赖（YAML 为内置精简解析器，不链接 libyaml）。

7.x/8.x 差异处理要点：mysqlnd `fetch_row` 钩子（8.1+ 才有 `zval**/bool` 原型）整段用版本宏守卫，7.4/8.0 裸 `make` 通过；ini 读取不走 `INI_STR()` 宏（7.x 宏对指针参数做 `sizeof`，会把长键名截断），改为按真实长度调用 `zend_ini_string_ex()`；`mre.log_script` 用 **PHP 层 `try { include } catch (Throwable)` 包装串**执行（`zend_eval_stringl` 编译执行，7.4 与 8.x 签名差异仅 const 修饰，传 `char[]` 字面量零告警），脚本抛出的异常/解析错误在用户态被 catch、从不越过 op_array 边界进入 C 层，7.4 下脚本抛异常同样降级告警而不是段错误（容器实测 BUG-2 的根治方案）。

> 阶段A（核心：env/ini 配置、mysqlnd 捕获、redis 嗅探、日志写入、GA 编译）与阶段B
> （YAML 规则引擎、脚本回调日志路径、跨 fpm 全局事务）均已完成，见下文与 `docs/设计.md`。

## 编译（phpize 三步）

要求：PHP dev 环境（含 `phpize`/`php-config`，且 php 编译时带 mysqlnd——发行版默认都带）。

```bash
phpize
./configure --enable-mre
make -j"$(nproc)"
```

产物在 `modules/mre.so`。CI 会为 7.4~8.4 各编译一份并打包为
`mre-<phpver>-linux-x86_64.tar.gz`（见 `.github/workflows/build.yml` 的 Artifacts）。

## 安装与启用

```ini
; php.ini —— 注意顺序（针对 Debian/Ubuntu 等把 mysqlnd 拆成独立 .so 的发行版）：
extension=mysqlnd.so   ; 必须先于 mre.so 加载（源码编译内置 mysqlnd 的 PHP 无此要求）
extension=mre.so
```

验证：

```bash
php -m | grep mre
php -i | grep -A 14 '^mre$'
```

## 配置项全表（env 与 ini 对照）

优先级：**Linux 环境变量（`MRE_` 前缀）> php.ini**。env 让 docker 部署不用改 php.ini。

| ini 名 | env 名 | 默认 | 说明 |
|---|---|---|---|
| `mre.enable` | `MRE_ENABLE` | `0` | 总开关，`1/true/on/yes` 均视为开 |
| `mre.log_file` | `MRE_LOG_FILE` | 空 | 日志文件路径；支持特殊值 `stdout`、`stderr`；支持 `/proc/1/fd/1`（容器内直达 pid1 stdout）；**父目录不存在会自动创建**；为空且开启时降级写 stderr 并告警一次 |
| `mre.log_format` | `MRE_LOG_FORMAT` | `json` | `json`（json-lines）或 `text`（其余任何非 `text` 值都按 json） |
| `mre.capture_mysql` | `MRE_CAPTURE_MYSQL` | `1` | MySQL 捕获开关 |
| `mre.capture_redis` | `MRE_CAPTURE_REDIS` | `1` | Redis 捕获开关 |
| `mre.redis_port` | `MRE_REDIS_PORT` | `6379` | redis 嗅探端口（只有连到该端口的 tcp 流会被嗅探） |
| `mre.sample_rows` | `MRE_SAMPLE_ROWS` | `1` | 每个结果集采样记录的行数（0 表示不采样内容，只记元信息；上限 100） |
| `mre.state_dir` | `MRE_STATE_DIR` | 空（默认 `/tmp`） | 状态目录（全局事务注册表位置；为空时 gtx 使用 `/tmp`） |
| `mre.yaml_config` | `MRE_YAML_CONFIG` | 空 | YAML 规则文件路径（规则格式见下文；文件变化自动热加载并重编译正则） |
| `mre.log_script` | `MRE_LOG_SCRIPT` | 空 | PHP 脚本绝对路径。启用后每条记录写日志前**同步**执行该脚本，脚本 `return '路径'` 决定本条记录的日志文件（详见下文） |
| `mre.log_script_async` | `MRE_LOG_SCRIPT_ASYNC` | 空 | 异步脚本绝对路径。非空且 `mre.enable=1` 时**每条记录 fork 一个子进程执行脚本（请求零等待）**：**扩展不再落盘任何日志**、也不执行 `mre.log_script`；脚本自己 `flock + fwrite($context['line']."\n")`。`$context` 在此模式额外带 `line`/`format`。fork 失败或 ZTS 构建下丢弃该条并每进程告警一次（不回落同步）。详见 [docs/异步脚本.md](docs/异步脚本.md) |
| `mre.gtx` | `MRE_GTX` | `0` | 跨 fpm 全局事务开关。**默认关闭**，防止生产误开；开启后 `mre_gtx_*` 函数才可用 |
| `mre.gtx_timeout` | `MRE_GTX_TIMEOUT` | `300` | 全局事务窗口最长存活秒数（10~86400），超时后第一次访问自动判定窗口失效并全部回滚（安全优先） |

php.ini 示例：

```ini
mre.enable = 1
mre.log_file = "/var/log/mre/mre.log"
mre.log_format = json
mre.redis_port = 6379
mre.sample_rows = 1
```

docker 环境变量示例（日志直达 pid1 stdout，被 `docker logs` 收集）：

```bash
docker run \
  -e MRE_ENABLE=1 \
  -e MRE_LOG_FILE=/proc/1/fd/1 \
  -e MRE_LOG_FORMAT=json \
  -e MRE_REDIS_PORT=6379 \
  your-php-image
```

## 日志格式

JSON lines（默认），字段：`time`（ISO8601 本地时区；mysql 请求行为执行**开始**时刻，见下文「执行耗时与起止时间」）、`pid`、`request_id`（请求级唯一标识，见下）、`type`（`mysql|redis`）、`dir`（`req|resp`）、`content`、`extra`（行数/错误/耗时等，可能为 `null`）：

```json
{"time":"2026-10-21T12:24:16+08:00","pid":131951,"request_id":"131951-1","type":"mysql","dir":"req","content":"SELECT id, name, age FROM users ORDER BY id","extra":{"cmd":"query","host":"127.0.0.1","port":3306,"time_end":"2026-10-21T12:24:16+08:00","cost_ms":0.412}}
{"time":"2026-10-21T12:24:16+08:00","pid":131951,"request_id":"131951-1","type":"mysql","dir":"resp","content":"result","extra":{"rows":4,"fields":3,"host":"127.0.0.1","port":3306}}
{"time":"2026-10-21T12:24:16+08:00","pid":131951,"request_id":"131951-1","type":"mysql","dir":"resp","content":"1|alice|30","extra":{"rows":4,"fetched_rows":1,"host":"127.0.0.1","port":3306}}
{"time":"2026-10-21T12:24:17+08:00","pid":131951,"request_id":"131951-1","type":"mysql","dir":"req","content":"INSERT INTO users (name, age) VALUES ('dave', 22)","extra":{"cmd":"query","host":"127.0.0.1","port":3306,"time_end":"2026-10-21T12:24:17+08:00","cost_ms":0.385}}
{"time":"2026-10-21T12:24:17+08:00","pid":131951,"request_id":"131951-1","type":"mysql","dir":"resp","content":"OK","extra":{"affected_rows":1,"host":"127.0.0.1","port":3306}}
{"time":"2026-10-21T12:24:17+08:00","pid":131951,"request_id":"131951-1","type":"mysql","dir":"req","content":"SELECT * FROM no_such_table","extra":{"cmd":"query","host":"127.0.0.1","port":3306,"time_end":"2026-10-21T12:24:17+08:00","cost_ms":0.298}}
{"time":"2026-10-21T12:24:17+08:00","pid":131951,"request_id":"131951-1","type":"mysql","dir":"resp","content":"ERROR","extra":{"error_code":1146,"error":"Table 'db.no_such_table' doesn't exist","sqlstate":"42S02","host":"127.0.0.1","port":3306}}
{"time":"2026-10-21T12:24:17+08:00","pid":131951,"request_id":"131951-1","type":"mysql","dir":"req","content":"SELECT name FROM users WHERE age > ?","extra":{"cmd":"prepare","stmt_id":1,"host":"127.0.0.1","port":3306,"time_end":"2026-10-21T12:24:17+08:00","cost_ms":0.301}}
{"time":"2026-10-21T12:24:17+08:00","pid":131951,"request_id":"131951-1","type":"mysql","dir":"resp","content":"PREPARED","extra":{"stmt_id":1,"fields":1,"host":"127.0.0.1","port":3306}}
{"time":"2026-10-21T12:24:17+08:00","pid":131951,"request_id":"131951-1","type":"mysql","dir":"req","content":"SELECT name FROM users WHERE age > ? AND name = ?","extra":{"cmd":"execute","stmt_id":1,"host":"127.0.0.1","port":3306,"prepared":{"sql":"SELECT name FROM users WHERE age > ? AND name = ?","params":[18,"bob"]},"sql_full":"SELECT name FROM users WHERE age > 18 AND name = 'bob'","time_end":"2026-10-21T12:24:17+08:00","cost_ms":0.356}}
{"time":"2026-10-21T12:24:17+08:00","pid":131951,"request_id":"131951-1","type":"mysql","dir":"req","content":"SELECT SLEEP(0.2)","extra":{"cmd":"query","host":"127.0.0.1","port":3306,"time_end":"2026-10-21T12:24:17+08:00","cost_ms":200.471}}
{"time":"2026-10-21T12:24:17+08:00","pid":131951,"request_id":"131951-1","type":"redis","dir":"req","content":"SET k1 v1","extra":{"host":"127.0.0.1","port":6379}}
{"time":"2026-10-21T12:24:17+08:00","pid":131951,"request_id":"131951-1","type":"redis","dir":"resp","content":"+OK","extra":{"port":6379}}
{"time":"2026-10-21T12:24:17+08:00","pid":131951,"request_id":"131951-1","type":"redis","dir":"resp","content":"$3000:xxx…","extra":{"port":6379,"truncated":true}}
```

### content 恒为纯 SQL，执行动作看 extra.cmd

mysql 的 **`dir=req` 记录 `content` 恒为纯 SQL 文本，永不加前缀**。历史版本会在预处理执行行拼 `"EXECUTE <SQL>"` 前缀，导致按首关键字分类的路由把 `EXECUTE SELECT` 误归写日志——现在该信息迁移到结构化字段：

- `extra.cmd`：每条 mysql `dir=req` 记录必带，取值 `query`（COM_QUERY 普通/事务控制语句）/ `prepare`（预处理准备）/ `execute`（预处理执行）。gtx 收尾与延迟 COMMIT 场景实际发送的是裸 `ROLLBACK`/`COMMIT` 语句，记 `cmd=query`。`dir=resp` 记录不带 `cmd`。
- `extra.prepared`：仅 execute 且**存在绑定参数**时出现，为对象 `{"sql": "<同 content 的 ? SQL>", "params": [...]}`。`params` 按绑定类型渲染：整型→JSON 数字、浮点→JSON 数字、字符串→JSON 字符串原文、NULL→`null`、blob→`"0x<hex截断>…(N bytes)"`。超过 32 个参数只记前 32 个，并在 `prepared.truncated=true` 标记（单个值超 256 字节尾部截断为 `…(N)`）。
- `extra.sql_full`：仅 execute 且存在绑定参数时出现，为把 `?` 依次替换成字面量后的完整 SQL 文本（字符串字面量单引号包裹、内部 `'` 转义成 `''`；**不做反斜杠转义**——在 `NO_BACKSLASH_ESCAPES` 模式下可原样重放，其它 sql_mode 下含 `\` 的字符串字面量会差一个字节）。总长沿用 4096 字节截断。
- execute 时没有绑定参数（`param_count=0` 或取不到）则**省略** `prepared` 与 `sql_full` 两字段，`cmd` 仍为 `execute`。
- `content` 本身始终是那条 `?` SQL（三件套之外的"纯 SQL"主字段）。

> 类型忠实度说明：绑定值按 mysqlnd 记录的声明类型（`MYSQLND_STMT_DATA.param_bind[i].type`）渲染——`i`→整型、`d`→浮点、`s`→字符串、`b`→blob 十六进制摘要、NULL→`null`；声明类型与变量实际类型不一致时按声明类型做与 mysqlnd 相同的数值转换（如 `'i'` 绑定字符串 `"5"` 渲染为 `5`）。

### 执行耗时与起止时间（extra.time_end / extra.cost_ms）

mysql 的 `dir=req` 记录（`cmd=query` / `prepare` / `execute` 三类）在 `extra` 里携带执行计时（`dir=resp` 与 redis/gtx 收尾行不带；resp 行自身的 `time` 即响应时刻，语义不变）：

- `extra.time_end`：**返回时间**。格式与顶层 `time` 完全一致（ISO8601 带本地时区，如 `2026-10-21T12:24:16+08:00`，秒级精度）。
- `extra.cost_ms`：**花费时间**，毫秒，JSON 数字，固定保留 3 位小数。内部用 `CLOCK_MONOTONIC` 时钟算差值（纳秒级分辨率、不受系统改表/回拨影响），墙钟只用来取 `time`/`time_end` 字符串。
- 顶层 `time` 语义不变 = **执行开始时刻**（SQL 发起时间）。实现上 req 行在原始调用执行完成后才落盘（写入时刻已晚于发起），行内 `time` 由扩展显式回填为调用开始时刻，因此 `time < time_end` 恒成立（或相等）。
- **执行失败同样记录**：后续 resp 为 `ERROR` 的查询，其 req 行照常带 `time_end`/`cost_ms`——慢失败查询正是排查重点。
- gtx 延迟 COMMIT 分支（语句未真正发给 server）：`time_end`=`time`、`cost_ms=0`。
- 脚本路由 `$context['extra']` 同步暴露 `time_end`（字符串）与 `cost_ms`（浮点数）。

> **已知取舍**：req 行改为"执行完落盘"后，执行中途进程被杀（崩溃/`kill -9`）的查询不会再产生 req 行。此前"只有 req 没有 resp"可以推断查询挂起，现在这类查询**整行缺失**（两者都没有）；需要监控挂起时请结合业务超时与 `cost_ms` 分布。

text 格式（`mre.log_format=text`），新字段以 `cmd=` / `params=` / `sql_full=` 追加，计时以 `time_end=` / `cost_ms=` 追加在行尾：

```
2026-10-21T12:18:44+08:00 pid=130769 request_id=130769-3 type=redis dir=req content="PING" host=127.0.0.1 port=16379
2026-10-21T12:18:45+08:00 pid=130769 request_id=130769-4 type=mysql dir=req content="SELECT name FROM users WHERE age > ? AND name = ?" cmd=execute stmt_id=1 host=127.0.0.1 port=3306 params=[18,"bob"] sql_full="SELECT name FROM users WHERE age > 18 AND name = 'bob'" time_end=2026-10-21T12:18:45+08:00 cost_ms=0.356
2026-10-21T12:18:46+08:00 pid=130769 request_id=130769-5 type=mysql dir=req content="SELECT SLEEP(0.2)" cmd=query host=127.0.0.1 port=3306 time_end=2026-10-21T12:18:46+08:00 cost_ms=200.471
```

### request_id（请求级唯一标识）

fpm 下同一个 worker 进程（同一个 `pid`）会**串行处理多个请求**，光靠 `pid` 无法区分"哪行日志属于哪个请求"。为此扩展在每次请求初始化（RINIT）时生成请求级标识：

- 格式：`<pid>-<请求序号>`，如 `131951-3` 表示进程 131951 处理的第 3 个请求；序号是**进程内单调递增**计数器（RINIT 递增；ZTS 下为进程级共享计数器，跨线程也不撞号），首个请求之前写的日志（如 MINIT 期告警）为 `<pid>-0`。
- 同一请求的所有日志行 `request_id` 相同，不同请求不同——json 与 text 两种格式都带该字段，`pid` 字段保持不变。
- 该标识同时注入脚本回调的 `$context['request_id']`（见下文），路由脚本可用它把"自己写的辅助日志"与扩展日志对齐。

截断策略：SQL ≤ 4096 字节；绑定参数单个值 ≤ 256 字节（超出尾部加 `…(N)`，N 为原始字节数）、最多记 32 个（超出置 `prepared.truncated=true`，params 片段整体 ≤ 2048 字节同样触发）；blob 参数记 `0x` + 前 32 字节 hex + `…(N bytes)`；redis 命令 ≤ 2048 字节（单参数 ≤ 256）；redis 响应摘要 ≤ 512 字节；采样行 ≤ 512 字节（16 列、单列 ≤ 64）。超限置 `extra.truncated=true`（redis/采样行）或 `prepared.truncated=true`（绑定参数）。

## 日志路由（阶段B）

每条记录写日志前会先做一次“目标解析”，优先级：**脚本返回路径 > yaml 规则 > `mre.log_file`**。路由只改变“写到哪个文件”，记录格式与 flock 机制不变。脚本与 yaml 可同时启用。

### YAML 规则文件（`mre.yaml_config`）

```yaml
rules:
  - name: slow-select          # 可选，排障用
    type: mysql                # mysql | redis，省略表示都匹配
    match: "/SELECT .* FROM slow_.*/i"   # PCRE 正则（preg 风格定界符+修饰符）
    log_file: /var/log/mre/slow.log
  - name: redis-del
    type: redis
    match: "^DEL "
    log_file: /var/log/mre/redis-danger.log
default_log_file: /var/log/mre/default.log
```

- 规则**有序，首条命中生效**；都不中落到 `default_log_file`；yaml 没有 `default_log_file` 时用 `mre.log_file` 主日志。
- `match` 支持 preg 风格定界符与修饰符 `i m s x u U A`（如 `/pattern/i`）；不带合法定界符时整个字符串按 pattern 本体使用。编译失败会在 stderr 告警（含规则名与 PCRE2 错误信息）并跳过该条，不影响其他规则。
- 正则在 yaml 加载/热加载时用 PCRE2 编译一次并缓存，匹配路径零编译开销。
- 兼容别名：`log:` 等价于 `log_file:`（设计文档历史写法）。
- `mre_yaml_dump()` 输出包含 `rules_compiled`（当前生效规则列表，含 `compiled` 编译状态）与 `default_log_file`，是规则排障入口。

### 脚本回调（`mre.log_script`）

值为 PHP 脚本**绝对路径**。每条记录写日志前执行一次该脚本。实现为一段固定的 PHP 层包装串（`try { $___mre_ret = include $___mre_path; } catch (Throwable $___mre_e) { $___mre_ret = null; $___mre_warn = $___mre_e->getMessage(); }`）经 `zend_eval_stringl` 编译执行：脚本路径经全局符号表变量 `$___mre_path` 注入（路径不拼进包装串，杜绝引号注入）；包装串约 140 字节，每条记录编译执行一次（实测 ~2µs/条）；脚本本身的编译走引擎 `include` 标准路径，启用 opcache 时命中共享内存缓存，未启用 opcache 时每条记录重新读盘编译（取舍：旧实现按 (mtime,size) 每请求缓存脚本 op_array，现改为不缓存——opcache 部署（fpm 常态）无差异，且引擎代管 op_array 生命周期，不再有共享内存 op_array 被误销毁的风险类）。脚本 `return '路径'` 即本条记录的目标日志文件：

```php
<?php
// $context 由扩展注入；$_SERVER/$_POST/$_GET/$_COOKIE 原样可读
if ($context['type'] === 'mysql' && strpos($context['content'], 'slow_') !== false) {
    return '/var/log/mre/slow.log';       // 本条记录写这里
}
if (!empty($_POST['debug'])) {
    return '/tmp/mre-debug.log';
}
return null;                              // null/空/非字符串 -> 降级到 规则/主日志
```

脚本执行时收到的 `$context` 数组（全部字段）：

| 键 | 值 |
|---|---|
| `type` | `mysql` 或 `redis` |
| `dir` | `req` 或 `resp` |
| `content` | SQL 或 redis 命令/响应文本 |
| `extra` | 行数/错误信息数组（`cmd`（mysql req 专有：`query`/`prepare`/`execute`）/`affected_rows`/`rows`/`fetched_rows`/`fields`/`stmt_id`/`error_code`/`error`/`sqlstate`/`host`/`port`/`truncated`/`prepared`（execute 且有绑定参数时：`sql` 同 content、`params` 数组、超 32 个时 `truncated=true`）/`sql_full`（execute 且有绑定参数时：`?` 替换为字面量后的 SQL）/`time_end`（mysql req 专有：返回时间字符串，同顶层 `time` 格式）/`cost_ms`（mysql req 专有：执行耗时毫秒，浮点），仅含存在项） |
| `headers` | 当前 HTTP 请求全部 header（由 `$_SERVER` 的 `HTTP_*` 与 `CONTENT_TYPE`/`CONTENT_LENGTH` 整理成 `小写名 => 值`，如 `x-foo-bar`；CLI 下为 `[]`） |
| `post` | `$_POST` 原样 |
| `cookie` | `$_COOKIE` 原样 |
| `get` | `$_GET` 原样 |
| `session_id` | `session_id()` 的值（未启动为空串） |
| `uri` | `$_SERVER['REQUEST_URI'] ?? ''` |
| `method` | `$_SERVER['REQUEST_METHOD'] ?? ''` |
| `pid` | 当前进程 id（与日志行 `pid` 字段一致） |
| `request_id` | 请求级唯一标识 `<pid>-<请求序号>`（与日志行 `request_id` 字段一致，见上文） |
| `script` | 当前请求 PHP 入口脚本路径：优先 `$_SERVER['SCRIPT_FILENAME']`；CLI/为空回退 `get_included_files()[0]`（首个包含文件即主脚本），再回退 SAPI 的 `path_translated`；都拿不到时为 `''`。用于让路由脚本从入口识别项目 |

`$_SERVER` 原样可读（扩展只读不写，其余超全局同样不被污染；脚本返回后注入的 `$context` 与内部变量 `$___mre_path/$___mre_ret/$___mre_warn` 会被立即移除，脚本不应使用 `___mre_` 前缀的全局变量）。

降级与防护：
- 脚本返回 `null`/空串/非字符串：静默降级写 **规则路由结果或 `mre.log_file`**，不告警。
- 脚本抛异常/解析错误（`ParseError` 也是 `Throwable`，同样被包装串 catch）：降级写规则/主日志，并在主日志记一条一次性 `type:"warn"` 告警（含异常消息摘要）。异常在 PHP 层被捕获，从不进入 C 层引擎状态，脚本错误绝不会让 PHP 进程崩溃或请求失败。
- 执行前后仅保存/恢复两样东西：调用方执行帧（`EG(current_execute_data)`，置 NULL 让脚本以顶层帧跑在全局符号表上，与引擎执行 {main} 同一机制）与调用方已挂起的异常指针；脚本输出（echo）被捕获丢弃。
- 局限：脚本内 `exit()/die()` 会终止当前请求（7.4 走 bailout 立即终止；8.x 为优雅 unwind，请求在下一条用户态边界处终止、`exit` 状态码保留，期间的记录仍会落盘）；脚本内发起的 SQL/redis 记录不会再递归调用脚本本身（防重入），而是直接走规则/主日志。

### 异步脚本（`mre.log_script_async`）

值为 PHP 脚本**绝对路径**。`mre.log_script_async`（或 `MRE_LOG_SCRIPT_ASYNC`）非空且 `mre.enable=1` 时进入异步模式：

- **扩展不写任何日志**：既不再写 `mre.log_file`，也不执行 `mre.log_script`、不跑 yaml 规则；每条记录只交给一个 fork 出来的子进程。
- **请求零等待**：父进程 `fork()` 后立即返回；子进程再 fork 一次后退出，孙进程执行脚本（见 `docs/设计.md` §9）。
- **脚本自己落盘**：`$context` 在同步字段之外额外提供 `line`（扩展已渲染好的整行，json 或 text，**不含结尾换行**）与 `format`（`json`/`text`）。脚本负责 `mkdir`（目录不存在时）、`flock(LOCK_EX)`、`fwrite($line."\n")`。因为 `line` 就是扩展原本会写出的字节，异步与同步的日志格式零漂移。
- **故障降级**：fork 失败或 ZTS 构建下**丢弃该条记录**并每进程告警一次，绝不回落同步、绝不阻塞请求；子进程内脚本 `stat` 失败/异常则静默结束该子进程（脚本自己的错误请写自己的错误日志）。
- 仓库自带的 `scripts/mre_log_router.php`（以及 include 它的 `mre_oo_log_script.php`）**已内置自写模式**：检测到 `$context['line']` 就自行追加落盘，脚本文件不用改。迁移只需把 ini 里的 `mre.log_script` 换成 `mre.log_script_async`（路径不变），详见 [docs/异步脚本.md](docs/异步脚本.md)。

```ini
; 同步配置
; mre.log_script = "/opt/mre/scripts/mre_log_router.php"
; 迁移到异步配置（同一脚本路径，仅键名改变）：
mre.log_script_async = "/opt/mre/scripts/mre_log_router.php"
```

### 自带路由脚本（scripts/mre_log_router.php）

仓库自带一个"按项目 + 按接口"分文件的日志路由脚本，部署时把 `mre.log_script`（同步）或 `mre.log_script_async`（异步，脚本自写）指向它即可：

```ini
mre.log_script = "/opt/mre/scripts/mre_log_router.php"
; 或环境变量 MRE_LOG_SCRIPT=/opt/mre/scripts/mre_log_router.php
; 异步（扩展不落盘、脚本自己 flock+fwrite）：
; mre.log_script_async = "/opt/mre/scripts/mre_log_router.php"
```

行为（脚本头部有完整注释）：

1. **识别项目**：入口脚本取 `$context['script']`（回退 `$_SERVER['SCRIPT_FILENAME']`），从其所在目录逐级向上找**项目根**——存在 `composer.json`、`think`、`thinkphp/` 目录、`application/` 目录之一即命中（兼容 ThinkPHP5 的 `public/index.php` 入口：`public/` 无标记，上一层即根）。找不到项目根时返回 `null` 降级，不在错误位置建目录。
2. **日志目录** = `<项目根>/runtime/mre-logs/`，不存在时脚本内 `@mkdir(..., 0755, true)` 递归创建（扩展写日志自身也支持父目录自动创建，二者互为兜底）。
3. **容器路径映射（/app/cache → /app/origin）**：若项目根位于容器内 `/app/cache/` 前缀下，且把前缀换成 `/app/origin/` 后对应目录存在，则日志落到 `/app/origin/...`——宿主机把 `/code/www`（即 Windows `D:\code\www`）挂载到容器 `/app/origin`，这样日志直接落到宿主机项目目录，无需进容器取日志；映射目标不存在则维持 `/app/cache/...` 原路径。
4. **文件命名** = `<url路径>.query.log` / `<url路径>.write.log` / `<url路径>.redis.log`（`url路径` 为 base，文件都在 `runtime/mre-logs/` 下）：
   - base 取 `$_SERVER['REQUEST_URI']` 的 path 部分（去掉 `?`/`#` 之后）→ `rawurldecode` → 安全化：字母数字与 `._-` 保留、其余转 `-`，去首尾 `-`/`.`，截断到 100 字符；不压缩连续 `-`（尽量保持不同路径 → 不同文件）；CLI 或无 `REQUEST_URI` 用 `cli`；有 URI 但安全化后为空（如 `/`）用 `root`。例：`/api/user/list?a=1` → `api-user-list`。
   - mysql 请求（`dir=req`）：SQL 去前导空白与注释（`--` 行 / `#` 行 / `/*...*/` 块）后，首关键字为 `SELECT/SHOW/DESCRIBE/DESC/EXPLAIN`（大小写不敏感）→ `.query.log`；其余 mysql 语句 → `.write.log`。预处理执行行（execute）的 `content` 自 0.1.x 起恒为纯 SQL（无 `EXECUTE ` 前缀），因此 `EXECUTE SELECT` 天然按首关键字归 `.query.log`，无需脚本做任何特判——**自带路由脚本对该结构变化零改动**；需要更细区分时可在脚本内读 `$context['extra']['cmd']`（`query`/`prepare`/`execute`）。
   - mysql 响应（`dir=resp`）：路由脚本每条记录独立执行、无跨记录状态，拿不到"响应对应的语句"，按 `extra` 元信息归类——含 `rows`/`fields`/`fetched_rows`（查询结果侧）→ `.query.log`，其余（`affected_rows`/`OK`/`ERROR` 等）→ `.write.log`（SELECT 失败的 ERROR 行会落在 `.write.log`，属已知取舍）。
   - redis 记录 → `.redis.log`；其他类型 → 返回 `null` 降级。
5. **防御性**：不输出、不抛异常、不定义顶层具名函数/类（同进程每条记录重复 include，避免 `Cannot redeclare`）、不使用 `___mre_` 前缀变量、除 `$context` 外不产生全局变量（扩展侧另有 ob 捕获 + try/catch 包装兜底）。

### 优先级示例

脚本与 yaml 同时启用时：命中 `DANGER` 的记录走脚本返回的文件；脚本返回 `null` 的记录走 yaml 规则；无规则命中走 `mre.log_file`。

## 跨 fpm 全局事务（mre.gtx=1）

通过一组 PHP 函数开启“全局事务窗口”，窗口期间**任意 fpm 进程、任意 mysqli/PDO 连接**开启的普通事务都归属其中，最后由一次命令统一提交或回滚：

| 函数 | 说明 |
|---|---|
| `mre_gtx_begin(): bool` | 开启窗口（生成 gtx_id，注册表写入 `mre.state_dir`）。已有存活窗口时返回 `false` |
| `mre_gtx_rollback(): bool` | 注册表打 rollback 标记 → 各持有跟踪连接的进程在下一次 mysqlnd 调用入口对自己本地的打开事务**真实发送 ROLLBACK**，全部移除后窗口自动结束 |
| `mre_gtx_commit(): bool` | 同上，标记 commit → 各进程真实发送 COMMIT 后清场结束窗口 |
| `mre_gtx_status(): array` | 返回 `active`(bool)、`state`(`active`/`mark_rollback`/`mark_commit`/`none`/`disabled`)、`gtx_id`、`started_at`、`tracked_connections`、`owner_pid`、`timeout` |

语义细节：

1. **归属**：窗口开启期间，连接的事务被自动登记——显式 `BEGIN`/`START TRANSACTION`，或 autocommit 关闭后的首个语句（通过服务端 `SERVER_STATUS_IN_TRANS` 状态检测），同时覆盖 mysqli/PDO 及一切 mysqlnd 连接类。
2. **延迟 COMMIT**：跟踪中的连接发起 `COMMIT`（`mysqli::commit()`/`PDO::commit()`/SQL `COMMIT`）时被拦截——不发给 server，函数返回 `true` 但事务保持打开；该连接“有未决 commit”记入注册表。
3. **统一收尾**：`mre_gtx_rollback()`/`mre_gtx_commit()` 只在注册表打标记（微秒级持锁）；各持有跟踪连接的进程在“下一次 mysqlnd 调用入口”（query/prepare/execute 钩子）检查标记，先真实发送 ROLLBACK/COMMIT，再关闭事务、清 pending、从注册表移除自己；最后一个条目移除后窗口自动结束（注册表文件删除）。
4. **崩溃安全**：注册表条目带 pid+时间戳；rollback/commit 清场时对已死进程（`kill(pid,0)` 判活）条目直接清理。窗口超过 `mre.gtx_timeout`（默认 300s）后，第一次访问自动判定窗口失效并标记全部回滚（安全优先）。
5. **开销控制**：注册表检查使用进程内 (mtime,size) 快照缓存——每请求每连接最多一次 `stat()`；事务控制语句（BEGIN/COMMIT/ROLLBACK）强制刷新检查。

**局限**（务必了解）：

- 真实 ROLLBACK/COMMIT 依赖“该连接的下一次 mysqlnd 调用”：若一个进程的跟踪连接之后再无任何数据库调用，其收尾会推迟到连接关闭（服务端断开回滚）或窗口超时清理。
- 拦截针对连接级：全局窗口开启期间，窗口内连接的**所有** COMMIT 都会被延迟（这是特性也是约束——需要细粒度独立事务的连接不应在窗口期间开启事务）。
- 连接若带未读结果集（unbuffered）时执行强制 ROLLBACK/COMMIT 会触发“commands out of sync”，该次收尾失败仍会注销连接（服务端最终回滚）；建议在结果集取完后再进入全局事务窗口。
- 业务主动发 `ROLLBACK` 视为退出窗口（连接注销，不再被收尾管理）。
- 注册表为 `<state_dir>/mre-gtx.reg` 行式文本 + `<state_dir>/mre-gtx.lock`（flock 串行化读改写，数据文件 rename 原子替换），格式见 `docs/设计.md` §10。
- Windows 无 `kill()` 判活，死进程清理降级为仅超时清理（本扩展以 Linux 部署为主）。

## 导出的 PHP 函数

| 函数 | 说明 |
|---|---|
| `mre_yaml_dump(): ?array` | 返回当前已加载的 yaml 配置树 + `rules_compiled`（生效规则列表）+ `default_log_file`；未配置/解析失败返回 `null`。规则引擎排障入口 |
| `mre_gtx_begin(): bool` | 开启全局事务窗口（需 `mre.gtx=1`） |
| `mre_gtx_commit(): bool` | 全局提交标记 |
| `mre_gtx_rollback(): bool` | 全局回滚标记 |
| `mre_gtx_status(): array` | 窗口状态（详见上节） |

## 捕获范围与已知限制

MySQL（mysqlnd 层）：
- 覆盖 mysqli / PDO_MYSQL 的普通查询、预处理语句（prepare/execute/get_result/**bind_result+fetch**）、多语句 `next_result`、缓冲与非缓冲结果。
- 绑定参数捕获（`extra.cmd`/`extra.prepared`/`extra.sql_full`）：值取自 mysqlnd `MYSQLND_STMT_DATA.param_bind[i]`（`zv` + 声明类型 `type`），在原始 execute 发包前读取，mysqli `bind_param('isdb')` 与 PDO `bindValue/bindParam` 两条路径都覆盖。局限：字符串字面量只做 `''` 转义不做反斜杠转义（见上文）；SQL 文本里的 `?` 依次替换，**引号字符串/注释内的 `?` 不识别**（服务端才是占位符定义方）；blob 只记 hex 截断摘要，超限 sql_full 不可原样重放。
- 行内容采样：mysqli 全覆盖——`get_result` 与 `bind_result+fetch` 两条取行路径都采样（后者经 mysqlnd stmt `fetch` 钩子读取绑定变量，采样行不带 `rows` 总数元信息）；PDO 在 PHP 8.1+ 通过 `fetch_row` 包装覆盖，7.4/8.0 的 PDO 取行走 C 数组路径，仅记录元信息（SQL/行数/错误不受影响）。
- 异步查询（`send_query/reap_query` 直用）未挂钩。

Redis（streams 层）：
- 覆盖 phpredis `Redis`/`RedisCluster` 与 Predis 的**非 TLS** tcp 连接（`tcp://`）；`tls://`/`ssl://` 与 unix socket 连接不嗅探。
- 数组回复只记条数摘要（如 `*3`），bulk/string/int/error 回复记录内容摘要。
- 极端二进制协议场景按截断标记降级，不影响连接本身（嗅探为旁路解析，失败只影响日志完整性）。

更多实现细节与取舍：见 [docs/设计.md](docs/设计.md)。

## 目录结构

```
├── config.m4 / config.w32      # 构建脚本（无外部依赖，自动探测 mysqlnd）
├── php_mre.h                   # 公共头：常量/配置结构/全局/各模块原型
├── mre.c                       # 模块生命周期、INI 注册、MINFO、函数表
├── mre_config.c                # env(MRE_*) > ini(mre.*) 配置解析
├── mre_util.c                    # ISO8601 时间、单调时钟（耗时测量）、JSON 转义、安全拷贝、mkdir -p
├── mre_log.c                   # 统一日志写入器（flock/句柄缓存/降级/路由接入）
├── mre_yaml.c                  # 内置精简 YAML 解析器（映射/序列/块标量/注释）
├── mre_rules.c                 # yaml 规则引擎（PCRE2 编译缓存 + 首中路由）
├── mre_script.c                # 日志路径脚本回调（php层 try/catch include 包装 + 全局符号表注入）
├── mre_async.c                 # 异步脚本执行（每条记录双 fork、父进程零等待、ZTS 禁用）
├── mre_gtx.c                   # 跨 fpm 全局事务（注册表 + flock + mysqlnd 入口检查）
├── mre_mysql.c                 # mysqlnd conn_data+stmt 方法表挂钩（含 gtx 拦截）
├── mre_redis.c                 # tcp 传输工厂包装 + RESP 双向增量解析
├── scripts/mre_log_router.php  # 自带日志路由脚本（mre.log_script 指向它；按项目根 + URL 路径 + 查询/非查询分文件）
├── docs/设计.md                 # 架构设计、版本兼容、阶段B实现取舍
├── tests/*.phpt                # 阶段A 2 条 + 阶段B 5 条 + 容器bug回归 2 条 + 脚本回调回归 2 条（ParseError/非字符串降级）+ request_id/路由脚本 3 条（012~014）
└── .github/workflows/build.yml # 7.4~8.4 矩阵编译打包
```

## License

（未指定；如需开源请补充 SPDX 头。）
