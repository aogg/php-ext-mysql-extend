<?php
// shared helpers for mre container integration tests (runs INSIDE the php container)
define('MRE_DB_HOST', 'mysql8');
define('MRE_DB_USER', 'root');
define('MRE_DB_PASS', 'root');
define('MRE_DB_NAME', 'mre_test');
define('MRE_DB_PORT', 3306);
define('MRE_REDIS_HOST', 'redis');
define('MRE_REDIS_PORT', 6379);

function mre_db() {
    $m = new mysqli(MRE_DB_HOST, MRE_DB_USER, MRE_DB_PASS, MRE_DB_NAME, MRE_DB_PORT);
    if ($m->connect_errno) {
        fwrite(STDERR, 'mysqli connect failed: ' . $m->connect_error . "\n");
        exit(2);
    }
    return $m;
}

function mre_pdo() {
    return new PDO(
        'mysql:host=' . MRE_DB_HOST . ';port=' . MRE_DB_PORT . ';dbname=' . MRE_DB_NAME,
        MRE_DB_USER,
        MRE_DB_PASS,
        array(PDO::ATTR_ERRMODE => PDO::ERRMODE_EXCEPTION)
    );
}

function mre_redis() {
    $r = new Redis();
    if (!@$r->connect(MRE_REDIS_HOST, MRE_REDIS_PORT, 3.0)) {
        fwrite(STDERR, "redis connect failed\n");
        exit(2);
    }
    return $r;
}
