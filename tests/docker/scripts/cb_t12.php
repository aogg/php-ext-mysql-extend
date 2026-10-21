<?php
// throws for BOOM records; null otherwise
if (strpos((string)$context['content'], 'BOOM') !== false) {
    throw new RuntimeException('t12 boom');
}
return null;
