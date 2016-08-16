--TEST--
COALESCE operator
--FILE--
<?php

echo $a ?? 42;
--EXPECT--
42
