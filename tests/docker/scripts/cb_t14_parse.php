<?php
// t14 "parse" scenario callback: DELIBERATE syntax error, do NOT fix.
// The PHP-level try/catch include wrapper in mre_script.c must catch the
// resulting ParseError in userland (BUG-2 regression guard on 7.4).
$this file is deliberately not valid php ((( ;
