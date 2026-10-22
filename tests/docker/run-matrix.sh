#!/usr/bin/env bash
# mre 容器实测矩阵驱动（在有 docker CLI 的宿主机上运行）
# 用法: CONTAINER=7.4-alpine-php-dev [EXT=/path/mre.so] bash run-matrix.sh <s1|...|s10|probe|repro|repro5|t14|all>
# 前置: 容器内已部署 /tmp/mre-test/{src,scripts}，mysql8 已建 mre_test 库表（见 README.md）
set -u
CONTAINER="${CONTAINER:-7.4-alpine-php-dev}"
EXT="${EXT:-/tmp/mre-test/src/modules/mre.so}"
SC="${1:-all}"
LOGS=/tmp/mre-test/logs
SDIR=/tmp/mre-test/gtxstate
MYSQLC="${MYSQL_CONTAINER:-mysql8}"
MYSQLARGS="${MYSQL_ARGS:--uroot -proot}"

dexec() { docker exec "$@"; }
run_php() { # run_php <logfile> [env k=v ...] -- php args...
  local log="$1"; shift
  # docker exec stops flag parsing at the first positional (container name):
  # leading "-e V" pairs must stay in front of $CONTAINER, the script path
  # must follow php.  Accumulate env pairs unquoted -- busybox ash has no
  # arrays; every env value used below is space-free (r2 fix, was: "$@"
  # placed before "$CONTAINER" so docker exec took the script path as the
  # container name).
  local envs=""
  while [ "$#" -ge 2 ] && [ "$1" = "-e" ]; do
    envs="$envs -e $2"
    shift 2
  done
  # shellcheck disable=SC2086
  dexec -e MRE_ENABLE=1 -e MRE_LOG_FILE="$log" $envs "$CONTAINER" \
    php -d extension="$EXT" -d xdebug.mode=off "$@"
}
check() { dexec "$CONTAINER" php /tmp/mre-test/scripts/check_log.php "$@"; }
db() { docker exec "$MYSQLC" mysql $MYSQLARGS -e "$1" 2>/dev/null; }

s1() {
  dexec "$CONTAINER" mkdir -p $LOGS; dexec "$CONTAINER" rm -f $LOGS/s1.json
  run_php $LOGS/s1.json -e MRE_SAMPLE_ROWS=1 /tmp/mre-test/scripts/t01_basic.php; echo "t01_exit=$?"
  check $LOGS/s1.json '{"nonempty":1,"valid_json":1,"no_empty_lines":1,"min_counts":{"mysql_req":10,"mysql_resp":10,"redis_req":4,"redis_resp":4},"must_contain":["affected_rows","error_code","fetched_rows","INSERT INTO users","EXECUTE SELECT name, age","table_not_exist_xyz","no_such_table_pdo","SET mre:s1:key","START TRANSACTION","COMMIT","ROLLBACK","\"type\":\"mysql\"","\"type\":\"redis\""],"fetched_rows_gte":2}'
}

s2() {
  local out mk
  out=$(dexec -e MRE_ENABLE=1 -e MRE_LOG_FILE=/proc/1/fd/1 "$CONTAINER" php -d extension="$EXT" /tmp/mre-test/scripts/t02_proc1.php 2>&1)
  echo "$out"; mk=$(echo "$out" | grep -o 't02-marker=[0-9]*' | head -1 | cut -d= -f2)
  sleep 1
  echo "s2_mysql_lines=$(docker logs --tail 400 "$CONTAINER" 2>&1 | grep -cF "t02-mysql-$mk")"
  echo "s2_redis_lines=$(docker logs --tail 400 "$CONTAINER" 2>&1 | grep -cF "mre:s2:$mk")"
  docker logs --tail 400 "$CONTAINER" 2>&1 | grep -F "t02-mysql-$mk" | head -2
}

s3() {
  dexec "$CONTAINER" rm -f $LOGS/s3.log
  run_php $LOGS/s3.log -e MRE_LOG_FORMAT=text /tmp/mre-test/scripts/t03_text.php; echo "t03_exit=$?"
  check $LOGS/s3.log '{"nonempty":1,"text_format":1,"no_empty_lines":1,"min_counts":{"mysql_req":3,"redis_req":3}}'
}

s4() { s4run /tmp/mre-test/logs/s4; }
s4run() {
  local BASE="$1"
  dexec "$CONTAINER" mkdir -p "$BASE"
  dexec "$CONTAINER" sh -c 'rm -f "$0"/rules.yaml "$0"/a.log "$0"/b.log "$0"/c.log "$0"/main.log' "$BASE"
  dexec "$CONTAINER" sh -c 'printf "rules:\n  - name: select-rule\n    type: mysql\n    match: \"/^SELECT /i\"\n    log_file: %s/a.log\n  - name: write-rule\n    type: mysql\n    match: \"/^(DELETE|UPDATE) /i\"\n    log_file: %s/b.log\ndefault_log_file: %s/c.log\n" "$0" "$0" "$0" > "$0"/rules.yaml' "$BASE"
  dexec -e MRE_ENABLE=1 -e MRE_LOG_FILE="$BASE/main.log" -e MRE_YAML_CONFIG="$BASE/rules.yaml" "$CONTAINER" \
    php -d extension="$EXT" /tmp/mre-test/scripts/t04_yaml.php "$BASE"; echo "t04_exit=$?"
  check "$BASE/a.log" '{"nonempty":1,"must_contain":["SELECT id FROM users LIMIT 2"],"must_not_contain":["INSERT INTO users","UPDATE users","DELETE FROM users"]}'
  check "$BASE/b.log" '{"nonempty":1,"must_contain":["UPDATE users SET age = 51","DELETE FROM users WHERE name = '"'"'s4-ins'"'"'"],"must_not_contain":["SELECT id FROM users","INSERT INTO users"]}'
  check "$BASE/c.log" '{"nonempty":1,"must_contain":["INSERT INTO users (name, age) VALUES ('"'"'s4-ins'"'"', 50)"]}'
  check "$BASE/main.log" '{"absent":1}'
}

s5() { s5run /tmp/mre-test/logs/s5 /tmp/mre-test/scripts/cb_s5.php; }
s5run() {
  local BASE="$1" CB="$2"
  dexec "$CONTAINER" mkdir -p "$BASE"
  dexec -e MRE_ENABLE=1 -e MRE_LOG_FILE="$BASE/main.log" -e MRE_LOG_SCRIPT="$CB" "$CONTAINER" \
    php -d extension="$EXT" /tmp/mre-test/scripts/t05_script.php; echo "t05_exit=$?"
  check "$BASE/header.log" '{"nonempty":1,"must_contain":["SET mre:s5:hdr"]}'
  check "$BASE/post.log" '{"nonempty":1,"must_contain":["SET mre:s5:post"]}'
  check "$BASE/uri.log" '{"nonempty":1,"must_contain":["SET mre:s5:uri"]}'
  check "$BASE/main.log" '{"nonempty":1,"valid_json":1,"must_contain":["SET mre:s5:plain","SET mre:s5:BOOM","\"type\":\"warn\"","s5 script exploded","SELECT id FROM users LIMIT 1"]}'
  echo 'first ctx line (headers must be [] in CLI):'
  dexec "$CONTAINER" sh -c 'head -1 "$0"/ctx.jsonl' "$BASE"
}

s6() {
  dexec "$CONTAINER" rm -f $LOGS/s6.json
  run_php $LOGS/s6.json /tmp/mre-test/scripts/t06_redis.php; echo "t06_exit=$?"
  check $LOGS/s6.json '{"nonempty":1,"valid_json":1,"no_empty_lines":1,"min_counts":{"redis_req":10,"redis_resp":10},"must_contain":["SET mre:s6:str","GET mre:s6:str","EXPIRE mre:s6:str 100","HGETALL mre:s6:h","LRANGE mre:s6:l 0 -1","SET mre:s6:raw rawv6","+OK"]}'
}

gtx_run() { # gtx_run <rollback|commit> <logname>
  local mode="$1" log="$LOGS/$2.json"
  dexec "$CONTAINER" sh -c 'rm -rf "$0"; mkdir -p "$0" '"$LOGS" "$SDIR"
  dexec "$CONTAINER" rm -f "$log"
  db "TRUNCATE mre_test.gtx_t"
  dexec -e MRE_ENABLE=1 -e MRE_GTX=1 -e MRE_STATE_DIR=$SDIR -e MRE_LOG_FILE="$log" "$CONTAINER" \
    php -d extension="$EXT" /tmp/mre-test/scripts/t_gtx.php "$mode" B > /tmp/mre-gtx-b.out 2>&1 &
  local bp=$!
  sleep 2
  dexec -e MRE_ENABLE=1 -e MRE_GTX=1 -e MRE_STATE_DIR=$SDIR -e MRE_LOG_FILE="$log" "$CONTAINER" \
    php -d extension="$EXT" /tmp/mre-test/scripts/t_gtx.php "$mode" A; echo "A_exit=$?"
  wait $bp; echo "B_exit=$?"
  cat /tmp/mre-gtx-b.out
  local want=0; [ "$mode" = commit ] && want=1
  db "SELECT CONCAT('rows=', COUNT(*)) FROM mre_test.gtx_t WHERE tag='gtx-$mode'"
  check "$log" "{\"nonempty\":1,\"valid_json\":1,\"must_contain\":[\"$(echo "$mode" | tr a-z A-Z)\"]}"
  dexec "$CONTAINER" sh -c "cat $SDIR/mre-gtx.reg 2>/dev/null || echo reg-file-gone"
}

s7() { gtx_run rollback s7; }
s8() { gtx_run commit s8; }

s9() {
  dexec "$CONTAINER" mkdir -p $LOGS
  local L=$LOGS
  dexec "$CONTAINER" rm -f $L/s9-noredis.json $L/s9-sample0.json $L/s9-sample3.json $L/s9-disabled.json
  run_php $L/s9-noredis.json -e MRE_CAPTURE_REDIS=0 /tmp/mre-test/scripts/t09_ops.php >/dev/null; echo "s91_exit=$?"
  check $L/s9-noredis.json '{"nonempty":1,"valid_json":1,"min_counts":{"mysql_req":1},"count_eq":{"redis_req":0,"redis_resp":0},"fetched_rows_eq":1}'
  run_php $L/s9-sample0.json -e MRE_SAMPLE_ROWS=0 /tmp/mre-test/scripts/t09_ops.php >/dev/null; echo "s92_exit=$?"
  check $L/s9-sample0.json '{"nonempty":1,"valid_json":1,"min_counts":{"mysql_req":1,"redis_req":3},"fetched_rows_eq":0}'
  run_php $L/s9-sample3.json -e MRE_SAMPLE_ROWS=3 /tmp/mre-test/scripts/t09_ops.php >/dev/null; echo "s93_exit=$?"
  check $L/s9-sample3.json '{"nonempty":1,"valid_json":1,"min_counts":{"mysql_req":1},"fetched_rows_eq":3}'
  dexec -e MRE_ENABLE=0 -e MRE_LOG_FILE=$L/s9-disabled.json "$CONTAINER" php -d extension="$EXT" /tmp/mre-test/scripts/t09_ops.php >/dev/null; echo "s94_exit=$?"
  check $L/s9-disabled.json '{"absent":1}'
}

s10() {
  if [ "$CONTAINER" = "7.4-alpine-php-dev" ]; then echo "WARN: s10 意图在 8.4 容器跑"; fi
  s1; s4run /tmp/mre-test/logs/s10s4
  s5run /tmp/mre-test/logs/s5-84 /tmp/mre-test/scripts/cb_s5_84.php
}

probe() {
  local v log
  for v in assoc array fetchrow object useresult stmtgetresult stmtbind; do
    log=$LOGS/probe-$v.json
    dexec "$CONTAINER" rm -f "$log"
    run_php "$log" /tmp/mre-test/scripts/t11_fetch.php "$v" >/dev/null 2>&1
    echo "$v: fetched_lines=$(dexec "$CONTAINER" sh -c "grep -c fetched_rows $log 2>/dev/null || echo 0")"
  done
}

repro() {
  local log=$LOGS/repro-t12.json
  dexec "$CONTAINER" rm -f "$log"
  dexec -e MRE_ENABLE=1 -e MRE_LOG_FILE="$log" -e MRE_LOG_SCRIPT=/tmp/mre-test/scripts/cb_t12.php "$CONTAINER" \
    php -d extension="$EXT" -d xdebug.mode=off /tmp/mre-test/scripts/t12_crashrepro.php 2>&1
  echo "repro_exit=$?  (BUG-2: 7.4 预期 139 SIGSEGV；8.1+ 预期 0 且日志有 type:warn)"
}

repro5() { # BUG-2 关单口径：crash 复现连跑 5 次，每次 exit=0 且行为断言
  local i log out ec
  for i in 1 2 3 4 5; do
    log=$LOGS/repro5-$i.json
    dexec "$CONTAINER" rm -f "$log"
    out=$(dexec -e MRE_ENABLE=1 -e MRE_LOG_FILE="$log" -e MRE_LOG_SCRIPT=/tmp/mre-test/scripts/cb_t12.php "$CONTAINER" \
      php -d extension="$EXT" -d xdebug.mode=off /tmp/mre-test/scripts/t12_crashrepro.php 2>&1); ec=$?
    echo "run${i}_stdout: $(printf '%s' "$out" | tr '\n' '|')"
    echo "run${i}_exit=$ec"
    # 每次行为断言：BOOM 记录降级主日志、warn 恰一次含消息摘要、后续 after 记录继续落盘
    check "$log" '{"nonempty":1,"valid_json":1,"no_empty_lines":1,"warn_eq":1,"must_contain":["SET mre:t12:BOOM","t12 boom","SET mre:t12:after"]}'
  done
}

t14() { t14run /tmp/mre-test/logs/t14; }
t14run() { # Q3 脚本回调扩展面：抛异常/ParseError/return123/return null/正常路由
  local BASE="$1" SC cb
  dexec "$CONTAINER" mkdir -p "$BASE"
  dexec "$CONTAINER" sh -c 'rm -f "$0"/rte.main.log "$0"/parse.main.log "$0"/ret123.main.log "$0"/retnull.main.log "$0"/ok.main.log "$0"/ok-route.log' "$BASE"
  for SC in rte parse ret123 retnull ok; do
    cb=/tmp/mre-test/scripts/cb_t14.php
    [ "$SC" = parse ] && cb=/tmp/mre-test/scripts/cb_t14_parse.php
    dexec -e MRE_ENABLE=1 -e MRE_LOG_FILE="$BASE/$SC.main.log" -e MRE_LOG_SCRIPT="$cb" "$CONTAINER" \
      php -d extension="$EXT" -d xdebug.mode=off /tmp/mre-test/scripts/t14_script_edge.php "$SC"; echo "t14-$SC-exit=$?"
  done
  echo '--- assertions ---'
  check "$BASE/rte.main.log"    '{"nonempty":1,"valid_json":1,"no_empty_lines":1,"warn_eq":1,"must_contain":["SET mre:t14:rte","t14 rte exploded","mre:t14:rte:",":after"]}'
  check "$BASE/parse.main.log"  '{"nonempty":1,"valid_json":1,"no_empty_lines":1,"warn_eq":1,"must_contain":["SET mre:t14:parse","syntax error",":after"]}'
  check "$BASE/ret123.main.log" '{"nonempty":1,"valid_json":1,"no_empty_lines":1,"warn_eq":0,"must_not_contain":["\"type\":\"warn\""],"must_contain":["SET mre:t14:ret123",":after"]}'
  check "$BASE/retnull.main.log" '{"nonempty":1,"valid_json":1,"no_empty_lines":1,"warn_eq":0,"must_not_contain":["\"type\":\"warn\""],"must_contain":["SET mre:t14:retnull",":after"]}'
  check "$BASE/ok-route.log"    '{"nonempty":1,"valid_json":1,"no_empty_lines":1,"warn_eq":0,"must_contain":["SET mre:t14:ok",":after"]}'
  # ok 场景：带标记记录被路由走；不带标记的握手记录(CONNECT/+OK)按 null 降级回主日志
  check "$BASE/ok.main.log"     '{"nonempty":1,"valid_json":1,"no_empty_lines":1,"warn_eq":0,"must_not_contain":["mre:t14:ok","\"type\":\"warn\""],"must_contain":["CONNECT"]}'
}

case "$SC" in
  s1|s2|s3|s4|s5|s6|s7|s8|s9|s10|probe|repro|repro5|t14) "$SC" ;;
  all) s1; s2; s3; s4; s5; s6; s7; s8; s9; probe; repro5; t14 ;;
  *) echo "unknown scenario: $SC"; exit 2 ;;
esac
echo "MATRIX[$SC@$CONTAINER] DONE"
