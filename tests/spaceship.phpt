--TEST--
SPACESHIP
--FILE--
<?php

var_dump(1 <=> 2);
var_dump(2 <=> 1);
var_dump(1 <=> 1);
--EXPECT--
int(-1)
int(1)
int(0)
