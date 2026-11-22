#!/bin/sh
# Build every test program with cc0, run it, and check exit code / stdout.
# Usage: ./run_tests.sh   (builds cc0 first via make)
set -u
cd "$(dirname "$0")"
make cc0 >/dev/null || exit 1

pass=0; fail=0
check() { # check <name> <expected_exit> <expected_stdout>
  name=$1; want_code=$2; want_out=$3
  ./cc0 "tests/$name.c" -o "tests/$name.out" || { echo "FAIL $name: compile failed"; fail=$((fail+1)); return; }
  got_out=$(./"tests/$name.out"); got_code=$?
  if [ "$got_code" = "$want_code" ] && [ "$got_out" = "$want_out" ]; then
    echo "PASS $name: exit=$got_code stdout='$got_out'"
    pass=$((pass+1))
  else
    echo "FAIL $name: exit=$got_code (want $want_code) stdout='$got_out' (want '$want_out')"
    fail=$((fail+1))
  fi
}

check test1_return_constant 42 ""
check test2_arithmetic       14 ""
check test3_variables        50 ""
check test4_print_and_loop   0  "15
55"
check test5_div_mod_compare  1  "3
2
1
1"
check test6_functions        3  "7
42
120
10"
check test7_for_break        0  "55
3
2"
check test8_char_ops         0  "65
32
10
6
7
6
18
14
8
14
6
16
16
-1
15
0
42"
check test9_void_globals     8  "7
8
2
55
100
101
102
5"

echo "----"
echo "$pass passed, $fail failed"
[ "$fail" = 0 ]
