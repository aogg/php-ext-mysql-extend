<?php
// S4: yaml rules route records; argv[1] = base dir for rule files/logs
$base = isset($argv[1]) && $argv[1] !== '' ? $argv[1] : '/tmp/mre-test/logs/s4';
require __DIR__ . '/common.php';

if (!is_dir($base)) { @mkdir($base, 0777, true); }
foreach (array('a', 'b', 'c', 'main') as $f) { @unlink("{$base}/{$f}.log"); }
@unlink("{$base}/rules.yaml");

file_put_contents(
    "{$base}/rules.yaml",
    "rules:\n" .
    "  - name: select-rule\n" .
    "    type: mysql\n" .
    "    match: \"/^SELECT /i\"\n" .
    "    log_file: {$base}/a.log\n" .
    "  - name: write-rule\n" .
    "    type: mysql\n" .
    "    match: \"/^(DELETE|UPDATE) /i\"\n" .
    "    log_file: {$base}/b.log\n" .
    "default_log_file: {$base}/c.log\n"
);

$dump = mre_yaml_dump();
echo 't04 rules_compiled=', (is_array($dump) && isset($dump['rules_compiled'])) ? count($dump['rules_compiled']) : 'null', "\n";

$m = mre_db();
$m->query('SELECT id FROM users LIMIT 2');                                       // -> a.log (req)
$m->query("INSERT INTO users (name, age) VALUES ('s4-ins', 50)");               // -> default c.log
$m->query("UPDATE users SET age = 51 WHERE name = 's4-ins'");                   // -> b.log
$m->query("DELETE FROM users WHERE name = 's4-ins'");                           // -> b.log
$m->close();

echo "t04 base={$base}\n";
echo "t04 done\n";
