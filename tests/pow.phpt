--TEST--
POW and POW_EQUAL in source
--FILE--
<?php

echo 2 ** 3, "\n";
$a = 3;
$a **= 4;
echo $a, "\n";
--EXPECT--
8
81
