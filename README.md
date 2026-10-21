# mre — mysql-redis-extend

一个 PHP C 扩展（扩展名 `mre`），在 PHP 进程内部捕获并记录：

- **MySQL 流量**：通过 mysqlnd 扩展点拦截，覆盖 **mysqli、PDO_MYSQL**（两者底层都走 mysqlnd）以及一切基于 mysqlnd 的连接类；记录 SQL 文本、影响行数/返回行数、错误码/错误信息，返回行内容可按条数采样（默认 1 行）。
- **Redis 流量**：通过 PHP streams 传输层嗅探 RESP 协议，覆盖 **phpredis（Redis/RedisCluster 扩展类）与 Predis（用户态类）**——两者建连最终都走 PHP streams，记录发送的命令与返回的响应。
- 日志一行一条，默认 JSON lines，`flock` 保证多进程（fpm）并发安全；支持写入文件、`stdout`、`stderr` 以及 **`/proc/1/fd/1`（docker 容器内输出到 pid1 stdout）**。

兼容 PHP **7.4 与 8.0 ~ 8.4**（源码内用 `PHP_VERSION_ID` 宏分支处理 API 差异），无任何外部库依赖（YAML 为内置精简解析器，不链接 libyaml）。

> 阶段A（本仓库当前状态）：扩展核心。YAML 规则引擎、脚本回调日志路径、跨 fpm 全局事务为下一阶段（设计见 `docs/设计.md`）。

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
| `mre.state_dir` | `MRE_STATE_DIR` | 空 | 状态文件目录（下一阶段全局事务的注册表位置；本阶段仅读取配置） |
| `mre.yaml_config` | `MRE_YAML_CONFIG` | 空 | YAML 规则文件路径（本阶段仅加载解析，规则应用在下一阶段；文件变化自动重载） |

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

JSON lines（默认），字段：`time`（ISO8601 本地时区）、`pid`、`type`（`mysql|redis`）、`dir`（`req|resp`）、`content`、`extra`（行数/错误等，可能为 `null`）：

```json
{"time":"2026-10-21T12:24:16+08:00","pid":131951,"type":"mysql","dir":"req","content":"SELECT id, name, age FROM users ORDER BY id","extra":{"host":"127.0.0.1","port":3306}}
{"time":"2026-10-21T12:24:16+08:00","pid":131951,"type":"mysql","dir":"resp","content":"result","extra":{"rows":4,"fields":3,"host":"127.0.0.1","port":3306}}
{"time":"2026-10-21T12:24:16+08:00","pid":131951,"type":"mysql","dir":"resp","content":"1|alice|30","extra":{"rows":4,"fetched_rows":1,"host":"127.0.0.1","port":3306}}
{"time":"2026-10-21T12:24:16+08:00","pid":131951,"type":"mysql","dir":"req","content":"INSERT INTO users (name, age) VALUES ('dave', 22)","extra":{"host":"127.0.0.1","port":3306}}
{"time":"2026-10-21T12:24:16+08:00","pid":131951,"type":"mysql","dir":"resp","content":"OK","extra":{"affected_rows":1,"host":"127.0.0.1","port":3306}}
{"time":"2026-10-21T12:24:16+08:00","pid":131951,"type":"mysql","dir":"resp","content":"ERROR","extra":{"error_code":1146,"error":"Table 'db.t' doesn't exist","sqlstate":"42S02","host":"127.0.0.1","port":3306}}
{"time":"2026-10-21T12:24:16+08:00","pid":131951,"type":"mysql","dir":"req","content":"SELECT name FROM users WHERE age > ?","extra":{"stmt_id":1,"host":"127.0.0.1","port":3306}}
{"time":"2026-10-21T12:24:16+08:00","pid":131951,"type":"mysql","dir":"req","content":"EXECUTE SELECT name FROM users WHERE age > ?","extra":{"stmt_id":1,"host":"127.0.0.1","port":3306}}
{"time":"2026-10-21T12:24:16+08:00","pid":131951,"type":"redis","dir":"req","content":"SET k1 v1","extra":{"host":"127.0.0.1","port":6379}}
{"time":"2026-10-21T12:24:16+08:00","pid":131951,"type":"redis","dir":"resp","content":"+OK","extra":{"port":6379}}
{"time":"2026-10-21T12:24:16+08:00","pid":131951,"type":"redis","dir":"resp","content":"$3000:xxx…","extra":{"port":6379,"truncated":true}}
```

text 格式（`mre.log_format=text`）：

```
2026-10-21T12:18:44+08:00 pid=130769 type=redis dir=req content="PING" host=127.0.0.1 port=16379
```

截断策略：SQL ≤ 4096 字节；redis 命令 ≤ 2048 字节（单参数 ≤ 256）；redis 响应摘要 ≤ 512 字节；采样行 ≤ 512 字节（16 列、单列 ≤ 64）。超限置 `extra.truncated=true`。

## 导出的 PHP 函数

| 函数 | 说明 |
|---|---|
| `mre_yaml_dump(): ?array` | 返回当前已加载的 yaml 配置树（数组形式），未配置/解析失败返回 `null`。用于验证规则文件（下一阶段规则引擎的排障入口） |

## 捕获范围与已知限制

MySQL（mysqlnd 层）：
- 覆盖 mysqli / PDO_MYSQL 的普通查询、预处理语句（prepare/execute/get_result）、多语句 `next_result`、缓冲与非缓冲结果。
- 行内容采样：mysqli 全覆盖；PDO 在 PHP 8.1+ 通过 `fetch_row` 包装覆盖，7.4/8.0 的 PDO 取行走 C 数组路径，仅记录元信息（SQL/行数/错误不受影响）。
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
├── mre_util.c                  # ISO8601 时间、JSON 转义、安全拷贝
├── mre_log.c                   # 统一日志写入器（flock/句柄缓存/降级）
├── mre_yaml.c                  # 内置精简 YAML 解析器（映射/序列/块标量/注释）
├── mre_mysql.c                 # mysqlnd conn_data+stmt 方法表挂钩
├── mre_redis.c                 # tcp 传输工厂包装 + RESP 双向增量解析
├── docs/设计.md                 # 架构设计、版本兼容、下一阶段规划
└── .github/workflows/build.yml # 7.4~8.4 矩阵编译打包
```

## License

（未指定；如需开源请补充 SPDX 头。）
