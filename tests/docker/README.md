# mre 容器实测（docker 真实环境集成测试）

对 `mre` 扩展在真实 docker 容器（Alpine + php:7.4/8.4 + mysql8 + redis）里的集成验证，
覆盖 README「配置项全表」中的主要组合与跨 fpm 全局事务。详细结果与 bug 清单见
`docs/测试报告-容器实测.md`。

## 环境

| 角色 | 容器 | 说明 |
|---|---|---|
| PHP 7.4 主力 | `7.4-alpine-php-dev` | PHP 7.4.33 NTS，内置 mysqlnd，已带 phpredis/sockets |
| PHP 8.4 兼容 | `8.4-alpine-php-dev` | PHP 8.4.25 NTS，同上 |
| MySQL | `mysql8` | mysql:8.0，root 密码 `root`，测试库 `mre_test`（users/gtx_t） |
| Redis | `redis` | 无密码，6379 |

php 容器与 mysql8/redis 需在同一 docker 网络且能用容器名解析（脚本里写死主机名
`mysql8` / `redis`，见 `scripts/common.php`，不同环境请改这里）。

编译工具链（两容器各自执行一次）：

```sh
apk add build-base autoconf gcc g++ make linux-headers
```

## 编译

```sh
cd /tmp/mre-test/src
phpize && ./configure --enable-mre && make -j"$(nproc)"
# 产物 /tmp/mre-test/src/modules/mre.so
```

> **PHP 7.4 / 8.0 注意（BUG-1，已修复）**：7.4.33 上曾原样 `make` 失败
> （`mre_mysql.c:201: error: unknown type name 'bool'`），不改源码的绕过：
>
> ```sh
> CF=$(grep -E '^CFLAGS = ' Makefile | head -1 | sed 's/^CFLAGS = //')
> make -j"$(nproc)" CFLAGS="$CF -include stdbool.h"
> ```
>
> a315d6e 起 fetch_row 钩子定义已加版本守卫：7.4/8.0 原样 `make clean && make` 通过
> （复测见 `docs/测试报告-容器实测.md` 复测轮2 R1），上述绕过不再需要。8.1+ 无此问题。

## 测试库表

```sh
docker exec mysql8 mysql -uroot -proot -e "
CREATE DATABASE IF NOT EXISTS mre_test;
CREATE TABLE mre_test.users (id INT AUTO_INCREMENT PRIMARY KEY, name VARCHAR(64), age INT) ENGINE=InnoDB;
INSERT INTO mre_test.users (name, age) VALUES ('alice',30),('bob',25),('carol',28),('dave',22),('eve',35);
CREATE TABLE mre_test.gtx_t (id INT AUTO_INCREMENT PRIMARY KEY, tag VARCHAR(32)) ENGINE=InnoDB;"
```

## 跑矩阵

在**有 docker CLI 的机器**上执行（每项 = docker exec 里的独立 php 进程 + 日志校验）：

```sh
CONTAINER=7.4-alpine-php-dev bash run-matrix.sh s1      # 单场景
CONTAINER=7.4-alpine-php-dev bash run-matrix.sh all     # S1~S9 + 稳定性
CONTAINER=8.4-alpine-php-dev bash run-matrix.sh s10     # 8.4 复跑 S1/S4/S5
```

场景与脚本对照：

| 场景 | 内容 | 脚本 |
|---|---|---|
| s1 | mysqli+PDO+phpredis 全链路 json 捕获 | t01_basic.php |
| s2 | MRE_LOG_FILE=/proc/1/fd/1 → docker logs | t02_proc1.php |
| s3 | MRE_LOG_FORMAT=text | t03_text.php |
| s4 | yaml 规则路由 SELECT/DELETE/UPDATE/default | t04_yaml.php |
| s5 | log_script：headers/post/uri 路由 + 异常降级 + CLI 空 headers | t05_script.php + cb_s5.php |
| s6 | phpredis 全命令族 + 裸 stream RESP（Predis 式） | t06_redis.php |
| s7 | 跨进程 gtx 统一回滚（双 php 进程） | t_gtx.php rollback A/B |
| s8 | 跨进程 gtx 统一提交 | t_gtx.php commit A/B |
| s9 | 开关项 capture_redis=0 / sample_rows=0,3 / enable=0 | t09_ops.php |
| s10 | 8.4：重跑 s1 + s4 + s5 | 同上 |
| probe | 采样路径矩阵（assoc/array/fetch_row/object/use_result/stmt 两路径） | t11_fetch.php |
| repro | BUG-2 最小复现（script 抛异常 → 7.4 SIGSEGV / 8.4 正常降级；2963935 修复后轮2 复测 7.4 仍 139，fee8b3d 改 PHP 层 try/catch 包装后见复测轮3） | t12_crashrepro.php + cb_t12.php |
| repro5 | BUG-2 关单口径：crash 复现连跑 5 次，每次 exit=0 + 降级主日志 + warn 恰一次（含摘要）+ 后续记录继续（fee8b3d 起可用） | t12_crashrepro.php + cb_t12.php |
| t14 | 脚本回调扩展面：抛 RuntimeException / 回调文件 ParseError / return 123 / return null / 正常返回路由 五场景（fee8b3d 起可用） | t14_script_edge.php + cb_t14.php + cb_t14_parse.php |
| t15 | mre_log_router.php 容器路径映射：/app/cache→/app/origin 正向映射、origin 目标缺失维持原路径、真实 zuodian 项目映射（需 /app/cache 与 /app/origin 双前缀存在，部署时把 scripts/mre_log_router.php 一并拷到 /tmp/mre-test/scripts/） | t15_router_mapping.php |
| inionly | BUG-3 复现/回归（纯 ini 部署，无 MRE_* env；7c19265 修复后 7.4 日志创建 + mre.gtx_timeout 原值生效） | t13_inionly.php |

ini-only 用法（容器内直跑，不得带任何 `-e MRE_*`）：

```sh
docker exec 7.4-alpine-php-dev php -d extension=/tmp/mre-test/src/modules/mre.so \
  -d mre.enable=1 -d mre.log_file=/tmp/mre-test/logs/t13-ini.json \
  -d mre.gtx_timeout=777 /tmp/mre-test/scripts/t13_inionly.php
# PASS：t13-timeout=777、exit=0、/tmp/mre-test/logs/t13-ini.json 创建且含 mysql req/resp 行
```

日志校验用 `scripts/check_log.php`（json-lines 逐行解析、计数、must_contain、
text 格式正则、空行/半行检测）。

## 已知测试环境注意

- PHP 8.1+ 的 mysqli 默认抛异常，负向用例脚本需 `mysqli_report(MYSQLI_REPORT_OFF)`
  （t01 已处理）。
- 8.4 容器带 Xdebug，CLI 有 200ms 调试超时噪音，可加 `-d xdebug.mode=off`。
