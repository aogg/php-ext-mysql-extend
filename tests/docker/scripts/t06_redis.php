<?php
// S6: phpredis coverage + raw php-stream RESP (Predis-style userland path)
require __DIR__ . '/common.php';

$mk = (string)getmypid();

$r = mre_redis();
$r->set('mre:s6:str', 'v6-' . $mk);
$v = $r->get('mre:s6:str');
$r->expire('mre:s6:str', 100);
$r->exists('mre:s6:str');
$r->hset('mre:s6:h', 'f', 'vv');
$r->hgetall('mre:s6:h');
$r->lpush('mre:s6:l', 'a');
$r->lpush('mre:s6:l', 'b');
$r->lrange('mre:s6:l', 0, -1);
$r->del(array('mre:s6:str', 'mre:s6:h', 'mre:s6:l'));
$r->close();

// raw stream socket, pipelined RESP (what Predis effectively does over streams)
$fp = stream_socket_client('tcp://' . MRE_REDIS_HOST . ':' . MRE_REDIS_PORT, $en, $es, 3.0);
if (!$fp) {
    fwrite(STDERR, "raw stream connect failed: $es\n");
    exit(2);
}
fwrite($fp, "*3\r\n\$3\r\nSET\r\n\$10\r\nmre:s6:raw\r\n\$5\r\nrawv6\r\n");
fwrite($fp, "*2\r\n\$3\r\nGET\r\n\$10\r\nmre:s6:raw\r\n");
usleep(300000);
$resp = fread($fp, 8192);
fclose($fp);

echo "t06-marker={$mk} get={$v} raw-resp-first-bytes=", substr((string)$resp, 0, 20), "\n";
echo "t06 done\n";
