<?php
// generic log-file checker.
// usage: php check_log.php <logfile> '<expect-json>'
// expect keys:
//   absent          : file must NOT exist
//   nonempty        : file must exist and be non-empty
//   no_empty_lines  : no blank lines, must end with newline
//   valid_json      : every line is a json object with type/dir/content
//   text_format     : every line matches mre text line shape
//   min_counts      : {mysql_req:N, ...}  at least N
//   count_eq        : {redis_req:0, ...}  exactly N
//   must_contain    : [substr, ...]
//   must_not_contain: [substr, ...]
//   fetched_rows_eq : exact number of lines whose extra.fetched_rows is set
//   fetched_rows_gte: at least N such lines
$file = isset($argv[1]) ? $argv[1] : '';
$expect = json_decode(isset($argv[2]) ? $argv[2] : '{}', true);
if (!is_array($expect)) { $expect = array(); }
$fail = array();
$exists = is_file($file);
$raw = $exists ? (string)file_get_contents($file) : '';

if (!empty($expect['absent'])) {
    if ($exists) { $fail[] = 'file-exists-but-absent-expected'; }
} else {
    if (!$exists) { $fail[] = 'file-missing'; }
    elseif ($raw === '' && !empty($expect['nonempty'])) { $fail[] = 'file-empty'; }
}

$lines = array();
if ($raw !== '') {
    if (!empty($expect['no_empty_lines'])) {
        if (strpos($raw, "\n\n") !== false) { $fail[] = 'blank-line-inside'; }
        if (substr($raw, -1) !== "\n") { $fail[] = 'no-trailing-newline'; }
    }
    foreach (explode("\n", rtrim($raw, "\n")) as $l) {
        if ($l !== '') { $lines[] = $l; }
    }
}

if (($exists || $raw !== '') && (!empty($expect['valid_json']) || (isset($expect['min_counts']) && empty($expect['text_format'])) || (isset($expect['count_eq']) && empty($expect['text_format'])) || isset($expect['fetched_rows_eq']) || isset($expect['fetched_rows_gte']))) {
    $counts = array('mysql_req' => 0, 'mysql_resp' => 0, 'redis_req' => 0, 'redis_resp' => 0, 'warn' => 0, 'other' => 0);
    $fetched = 0; $nb = 0;
    $text_mode = !empty($expect['text_format']);
    foreach ($lines as $i => $line) {
        if ($text_mode) {
            if (preg_match('/ type=([a-z]+) dir=([a-z]+) /', $line, $mm)) {
                $k = $mm[1] . '_' . $mm[2];
                if (array_key_exists($k, $counts)) { $counts[$k]++; } else { $counts['other']++; }
            }
            if (strpos($line, 'fetched_rows=') !== false) { $fetched++; }
            continue;
        }
        $j = json_decode($line, true);
        if (!is_array($j) || !isset($j['type'], $j['dir'], $j['content'])) {
            $nb++;
            if ($nb <= 3) { $fail[] = "bad-json-line-$i: " . substr($line, 0, 120); }
            continue;
        }
        $k = $j['type'] . '_' . $j['dir'];
        if (array_key_exists($k, $counts)) { $counts[$k]++; } else { $counts['other']++; }
        if (isset($j['extra']) && is_array($j['extra']) && array_key_exists('fetched_rows', $j['extra'])) { $fetched++; }
    }
    if ($nb) { $fail[] = "bad-json-total=$nb"; }
    foreach ((isset($expect['min_counts']) ? $expect['min_counts'] : array()) as $k => $v) {
        $have = isset($counts[$k]) ? $counts[$k] : 0;
        if ($have < (int)$v) { $fail[] = "count[$k]=$have<$v"; }
    }
    foreach ((isset($expect['count_eq']) ? $expect['count_eq'] : array()) as $k => $v) {
        $have = isset($counts[$k]) ? $counts[$k] : 0;
        if ($have !== (int)$v) { $fail[] = "count[$k]=$have!=$v"; }
    }
    if (isset($expect['fetched_rows_eq']) && $fetched !== (int)$expect['fetched_rows_eq']) {
        $fail[] = "fetched_lines=$fetched!=" . $expect['fetched_rows_eq'];
    }
    if (isset($expect['fetched_rows_gte']) && $fetched < (int)$expect['fetched_rows_gte']) {
        $fail[] = "fetched_lines=$fetched<" . $expect['fetched_rows_gte'];
    }
}

if (!empty($expect['text_format']) && $raw !== '') {
    foreach ($lines as $i => $line) {
        if (!preg_match('/^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9:+\-]+ pid=[0-9]+ type=[a-z]+ dir=[a-z]+ content="/', $line)) {
            $fail[] = "bad-text-line-$i: " . substr($line, 0, 120);
            break;
        }
    }
}

foreach ((isset($expect['must_contain']) ? $expect['must_contain'] : array()) as $s) {
    if (strpos($raw, $s) === false) { $fail[] = "missing: $s"; }
}
foreach ((isset($expect['must_not_contain']) ? $expect['must_not_contain'] : array()) as $s) {
    if ($raw !== '' && strpos($raw, $s) !== false) { $fail[] = "unexpected-present: $s"; }
}

if ($fail) {
    echo "CHECK-FAIL [$file]\n  - " . implode("\n  - ", $fail) . "\n";
    exit(1);
}
echo "CHECK-OK [$file]\n";
