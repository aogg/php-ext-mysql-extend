<?php
// t14 callback: switches on the record content marker coming from t14_script_edge.php.
// throws / returns non-string / returns null / routes - one file for four scenarios.
if (strpos((string)$context['content'], 'mre:t14:ok') !== false) {
    return '/tmp/mre-test/logs/t14/ok-route.log';
}
if (strpos((string)$context['content'], 'mre:t14:rte') !== false) {
    throw new RuntimeException('t14 rte exploded');
}
if (strpos((string)$context['content'], 'mre:t14:ret123') !== false) {
    return 123;
}
if (strpos((string)$context['content'], 'mre:t14:retnull') !== false) {
    return null;
}
return null;
