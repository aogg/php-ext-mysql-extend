<?php
// S5 callback script: route by headers/post/uri, throw on BOOM, null otherwise.
// Appends the received $context summary to ctx.jsonl for inspection.
$ctxFile = '/tmp/mre-test/logs/s5/ctx.jsonl';
@file_put_contents(
    $ctxFile,
    json_encode(array(
        'type'    => $context['type'],
        'dir'     => $context['dir'],
        'content' => $context['content'],
        'headers' => $context['headers'],
        'post'    => $context['post'],
        'uri'     => $context['uri'],
        'method'  => $context['method'],
    )) . "\n",
    FILE_APPEND
);

if (!empty($context['headers']['x-mre-route'])) {
    return '/tmp/mre-test/logs/s5/header.log';
}
if (!empty($context['post']['route'])) {
    return '/tmp/mre-test/logs/s5/post.log';
}
if (strpos((string)$context['uri'], '/special-uri') !== false) {
    return '/tmp/mre-test/logs/s5/uri.log';
}
if (strpos((string)$context['content'], 'BOOM') !== false) {
    throw new RuntimeException('s5 script exploded');
}
return null;
