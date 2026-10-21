<?php
// S7/S8: cross-process global transaction.
// usage: php t_gtx.php <rollback|commit> <A|B>
require __DIR__ . '/common.php';

$mode = isset($argv[1]) ? $argv[1] : '';   // rollback | commit
$role = isset($argv[2]) ? $argv[2] : '';   // A | B
$tag = 'gtx-' . $mode;
if ($mode === '' || $role === '') {
    fwrite(STDERR, "usage: php t_gtx.php <rollback|commit> <A|B>\n");
    exit(2);
}

$wait = function ($what, $cond, $timeout = 40) use ($role) {
    $dl = microtime(true) + $timeout;
    do {
        usleep(80000);
        $s = mre_gtx_status();
    } while (!$cond($s) && microtime(true) < $dl);
    if (!$cond($s)) {
        echo "[{$role}] TIMEOUT waiting {$what} status=" . json_encode($s) . "\n";
        exit(3);
    }
    return $s;
};

if ($role === 'A') {
    if (!mre_gtx_begin()) {
        echo "[{$role}] begin-failed\n";
        exit(3);
    }
    echo "[{$role}] began window\n";
    $wait('tracked>=1', function ($s) { return $s['tracked_connections'] >= 1; });
    echo "[{$role}] saw tracked>=1\n";
    usleep(2500000); // let B insert + hit the deferred COMMIT

    $peek = mre_db();
    $cnt = $peek->query("SELECT COUNT(*) c FROM gtx_t WHERE tag = '{$tag}'")->fetch_assoc()['c'];
    echo "[{$role}] visible-before-finalize={$cnt} (expect 0)\n";

    $ok = $mode === 'rollback' ? mre_gtx_rollback() : mre_gtx_commit();
    echo "[{$role}] mark-{$mode}=", var_export((bool)$ok, true), "\n";

    $wait('window-end', function ($s) { return $s['state'] === 'none' || $s['active'] === false; });
    $fin = mre_gtx_status();
    echo "[{$role}] final-state={$fin['state']}\n";

    $cnt2 = $peek->query("SELECT COUNT(*) c FROM gtx_t WHERE tag = '{$tag}'")->fetch_assoc()['c'];
    echo "[{$role}] final-visible={$cnt2} (expect " . ($mode === 'commit' ? '1' : '0') . ")\n";
    $peek->close();
    echo "[{$role}] done\n";
} else {
    $wait('active-window', function ($s) { return $s['active'] === true; });
    echo "[{$role}] window active\n";

    $m = mre_db();
    $m->query('START TRANSACTION');
    $m->query("INSERT INTO gtx_t (tag) VALUES ('{$tag}')");
    $ok = $m->query('COMMIT'); // intercepted inside the window: stays open
    echo "[{$role}] deferred-commit=", var_export((bool)$ok, true), "\n";

    $want = $mode === 'rollback' ? 'mark_rollback' : 'mark_commit';
    $wait($want, function ($s) use ($want) { return $s['state'] === $want; });
    echo "[{$role}] saw {$want}\n";

    $m->query('SELECT 1'); // next mysqlnd entry performs the real ROLLBACK/COMMIT
    $cnt = $m->query("SELECT COUNT(*) c FROM gtx_t WHERE tag = '{$tag}'")->fetch_assoc()['c'];
    echo "[{$role}] own-visible-after={$cnt} (expect " . ($mode === 'commit' ? '1' : '0') . ")\n";
    $m->close();
    echo "[{$role}] done\n";
}
