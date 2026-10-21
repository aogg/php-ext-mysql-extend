<?php
// S1: mysqli + PDO_MYSQL + phpredis against real mysql8/redis, json log
require __DIR__ . '/common.php';

mysqli_report(MYSQLI_REPORT_OFF); // 8.1+ defaults to exceptions; we want silent errors for the negative test

$mk = (string)getmypid();

// ---------- mysqli ----------
$m = mre_db();
$r1 = $m->query('SELECT id, name, age FROM users ORDER BY id');                 // rows + sampled line
while ($row = $r1->fetch_assoc()) {}
$r1->close();
$m->query("INSERT INTO users (name, age) VALUES ('s1-mysqli-{$mk}', 31)");      // affected_rows
$m->query("UPDATE users SET age = age + 1 WHERE name LIKE 's1-mysqli-%'");      // affected_rows
$st = $m->prepare('SELECT name, age FROM users WHERE id > ?');                  // prepare
$ge = 0;
$st->bind_param('i', $ge);
$st->execute();                                                                  // execute
$res = $st->get_result();
while ($res->fetch_assoc()) {}
$st->close();
$m->begin_transaction();
$m->query("INSERT INTO users (name, age) VALUES ('s1-tx-{$mk}', 40)");
$m->commit();
$m->begin_transaction();
$m->query("INSERT INTO users (name, age) VALUES ('s1-txrb-{$mk}', 41)");
$m->rollback();
$m->query('SELECT * FROM table_not_exist_xyz');                                  // error_code 1146
$m->close();

// ---------- PDO_MYSQL ----------
$pdo = mre_pdo();
foreach ($pdo->query('SELECT id, name FROM users ORDER BY id') as $row) {
}
$pdo->exec("INSERT INTO users (name, age) VALUES ('s1-pdo-{$mk}', 32)");
$q = $pdo->prepare('SELECT age FROM users WHERE name = ?');
$q->execute(array("s1-pdo-{$mk}"));
$rows = $q->fetchAll(PDO::FETCH_ASSOC);
$pdo->beginTransaction();
$pdo->exec("INSERT INTO users (name, age) VALUES ('s1-pdotx-{$mk}', 42)");
$pdo->commit();
try {
    $pdo->query('SELECT * FROM no_such_table_pdo');                              // error_code
} catch (Throwable $e) {
}
$pdo = null;

// ---------- redis (phpredis) ----------
$r = mre_redis();
$r->set('mre:s1:key', 'hello-' . $mk);
$v = $r->get('mre:s1:key');
$r->del('mre:s1:key');
$r->hset('mre:s1:h', 'f1', 'v1');
$r->hgetall('mre:s1:h');
$r->del('mre:s1:h');
$r->close();

// cleanup test rows
$m2 = mre_db();
$m2->query("DELETE FROM users WHERE name LIKE 's1-%'");
$m2->close();

echo "t01-marker={$mk} get={$v}\n";
echo "t01 done\n";
