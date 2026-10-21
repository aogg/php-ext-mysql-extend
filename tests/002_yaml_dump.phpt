--TEST--
mre_yaml_dump(): null without config, tree with config, hot reload
--INI--
mre.enable=1
--FILE--
<?php
// not configured yet
var_dump(mre_yaml_dump());

$yaml = sys_get_temp_dir() . '/mre_test_rules_' . getmypid() . '.yaml';
file_put_contents($yaml, "# comment\nrules:\n  - match: \"^SELECT\"\n    log: \"/tmp/a.log\"\n  - match: \".*\"\n    log: '/tmp/b.log'\n");

ini_set('mre.yaml_config', $yaml);
$tree = mre_yaml_dump();
var_dump(is_array($tree));
var_dump(count($tree['rules']));
var_dump($tree['rules'][0]['match']);
var_dump($tree['rules'][1]['log']);

// edit + touch to force reload (mtime based)
clearstatcache();
file_put_contents($yaml, "rules:\n  - match: \"^UPDATE\"\n    log: \"/tmp/c.log\"\n");
touch($yaml, time() + 2);
ini_set('mre.yaml_config', $yaml);
$tree2 = mre_yaml_dump();
var_dump($tree2['rules'][0]['match']);

@unlink($yaml);
?>
--EXPECT--
NULL
bool(true)
int(2)
string(7) "^SELECT"
string(10) "/tmp/b.log"
string(7) "^UPDATE"
