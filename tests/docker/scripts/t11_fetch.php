<?php
// S-bug2 probe: which fetch paths sample rows on this PHP version?
// usage: php t11_fetch.php <variant>
require __DIR__ . '/common.php';

$variant = isset($argv[1]) ? $argv[1] : 'assoc';

$m = mre_db();

switch ($variant) {
    case 'assoc':          // buffered query + fetch_assoc
        $r = $m->query('SELECT id, name, age FROM users ORDER BY id');
        while ($row = $r->fetch_assoc()) {}
        break;
    case 'array':          // buffered query + fetch_array NUM
        $r = $m->query('SELECT id, name, age FROM users ORDER BY id');
        while ($row = $r->fetch_array(MYSQLI_NUM)) {}
        break;
    case 'fetchrow':       // buffered query + fetch_row
        $r = $m->query('SELECT id, name, age FROM users ORDER BY id');
        while ($row = $r->fetch_row()) {}
        break;
    case 'object':         // buffered query + fetch_object
        $r = $m->query('SELECT id, name, age FROM users ORDER BY id');
        while ($row = $r->fetch_object()) {}
        break;
    case 'useresult':      // unbuffered + fetch_assoc
        $r = $m->query('SELECT id, name, age FROM users ORDER BY id', MYSQLI_USE_RESULT);
        while ($row = $r->fetch_assoc()) {}
        $r->close();
        break;
    case 'stmtgetresult':  // prepare/execute/get_result/fetch_assoc
        $st = $m->prepare('SELECT id, name, age FROM users WHERE id > ?');
        $z = 0;
        $st->bind_param('i', $z);
        $st->execute();
        $r = $st->get_result();
        while ($row = $r->fetch_assoc()) {}
        $st->close();
        break;
    case 'stmtbind':       // prepare/execute/bind_result/fetch
        $st = $m->prepare('SELECT id, name, age FROM users WHERE id > ?');
        $z = 0;
        $st->bind_param('i', $z);
        $st->execute();
        $a = $b = $c = null;
        $st->bind_result($a, $b, $c);
        while ($st->fetch()) {}
        $st->close();
        break;
    default:
        fwrite(STDERR, "unknown variant $variant\n");
        exit(2);
}

$m->close();
echo "t11-$variant done\n";
