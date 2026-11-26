/* test_cc0_js.js — Node harness for demo/cc0.js (the pure-JS C interpreter).
 *
 * Rebuilt 2026-11-26: the original harness lived in /tmp and was wiped by
 * service restarts, so this copy lives durably in tests/ next to the engine.
 * Run with:  node tests/test_cc0_js.js     (from 05-c-compiler/)
 * No network, no dependencies. Exit code 0 iff every check passes.
 *
 * Coverage (36 checks):
 *  - all 9 repo test programs (tests/test*.c) run with the exact stdout and
 *    exit code from run_tests.sh (18 checks: stdout + exit code each);
 *  - the 9 one-click UI examples embedded in cc0.js (CC0.EXAMPLES) are
 *    byte-identical (trimmed) to the matching .c files (9 checks);
 *  - the printf extra example (1 check);
 *  - infinite-loop safety: while(1){}, for(;;){} hit the step limit and
 *    infinite recursion hits the call-depth cap, all quickly (3 checks);
 *  - error cases: undeclared variable, division by zero, break outside a
 *    loop (3 checks);
 *  - the assembly listing generator: all 9 programs produce a listing with
 *    .globl main / push %rbp / ret, and test6's listing calls fact (2 checks).
 * Expected values come from run_tests.sh and the .c files in this repo —
 * nothing here is invented.
 */
"use strict";

var fs = require("fs");
var path = require("path");
var CC0 = require("../demo/cc0.js");

var TESTS_DIR = __dirname;
var passed = 0, failed = 0, failures = [];
function check(name, cond) {
  if (cond) { passed++; }
  else { failed++; failures.push(name); console.log("FAIL: " + name); }
}
function read(name) { return fs.readFileSync(path.join(TESTS_DIR, name), "utf8"); }

/* The 9 repo test programs with the exact expected stdout / exit codes
 * from run_tests.sh (the C compiler's own ground truth). */
var TESTS = [
  { file: "test1_return_constant.c", out: "", code: 42 },
  { file: "test2_arithmetic.c", out: "", code: 14 },
  { file: "test3_variables.c", out: "", code: 50 },
  { file: "test4_print_and_loop.c", out: "15\n55\n", code: 0 },
  { file: "test5_div_mod_compare.c", out: "3\n2\n1\n1\n", code: 1 },
  { file: "test6_functions.c", out: "7\n42\n120\n10\n", code: 3 },
  { file: "test7_for_break.c", out: "55\n3\n2\n", code: 0 },
  { file: "test8_char_ops.c",
    out: "65\n32\n10\n6\n7\n6\n18\n14\n8\n14\n6\n16\n16\n-1\n15\n0\n42\n", code: 0 },
  { file: "test9_void_globals.c", out: "7\n8\n2\n55\n100\n101\n102\n5\n", code: 8 }
];

/* ------------------------------------------------------------------ */
/* 1. All 9 test programs: exact stdout + exit code (18 checks)      */
/* ------------------------------------------------------------------ */
TESTS.forEach(function (t, i) {
  var src = read(t.file);
  var res;
  try {
    res = CC0.run(src);
  } catch (e) {
    check("test" + (i + 1) + " stdout (" + t.file + ")", false);
    check("test" + (i + 1) + " exit code (" + t.file + ")", false);
    console.log("  threw: " + (e && e.message));
    return;
  }
  check("test" + (i + 1) + " stdout (" + t.file + ")", res.output === t.out);
  check("test" + (i + 1) + " exit code (" + t.file + ") = " + t.code, res.exitCode === t.code);
});

/* ------------------------------------------------------------------ */
/* 2. UI examples byte-identical (trimmed) to the .c files (9 checks) */
/* ------------------------------------------------------------------ */
TESTS.forEach(function (t, i) {
  var ex = CC0.EXAMPLES[i];
  check("example test" + (i + 1) + " identical to " + t.file,
    !!ex && ex.src.trim() === read(t.file).trim());
});

/* ------------------------------------------------------------------ */
/* 3. printf extra example (1 check)                                  */
/* ------------------------------------------------------------------ */
(function () {
  var ex = CC0.EXAMPLES[9];
  var res = CC0.run(ex.src);
  check("printf example stdout + exit code 0",
    res.output === "Hello, world!\n40 + 2 = 42\nchar: ABC\n100% done\n" && res.exitCode === 0);
})();

/* ------------------------------------------------------------------ */
/* 4. Infinite-loop / recursion safety (3 checks, all fast)           */
/* ------------------------------------------------------------------ */
function throwsWith(src, re, opts) {
  var start = Date.now();
  try { CC0.run(src, opts); return { threw: false, ms: Date.now() - start }; }
  catch (e) { return { threw: re.test(e.message || ""), ms: Date.now() - start, msg: e.message }; }
}
(function () {
  var r1 = throwsWith("int main() { while (1) {} return 0; }", /step limit exceeded/, { maxSteps: 50000 });
  check("safety: while(1){} stops at the step limit", r1.threw && r1.ms < 10000);
  var r2 = throwsWith("int main() { for (;;) {} return 0; }", /step limit exceeded/, { maxSteps: 50000 });
  check("safety: for(;;){} stops at the step limit", r2.threw && r2.ms < 10000);
  var r3 = throwsWith("int f(int n) { return f(n + 1); }\nint main() { return f(0); }", /call depth exceeded/);
  check("safety: infinite recursion stops at the call-depth cap", r3.threw && r3.ms < 10000);
})();

/* ------------------------------------------------------------------ */
/* 5. Error cases (3 checks)                                          */
/* ------------------------------------------------------------------ */
(function () {
  var e1 = throwsWith("int main() { return x; }", /use of undeclared variable 'x'/);
  check("error: undeclared variable", e1.threw);
  var e2 = throwsWith("int main() { print(1 / 0); return 0; }", /division by zero/);
  check("error: division by zero", e2.threw);
  var threwBreak = false, isBreakSignal = false;
  try { CC0.run("int main() { break; return 0; }"); }
  catch (e) {
    threwBreak = true;
    isBreakSignal = !!e && e.constructor && e.constructor.name === "BreakSignal";
  }
  check("error: break outside a loop throws a BreakSignal", threwBreak && isBreakSignal);
})();

/* ------------------------------------------------------------------ */
/* 6. Assembly listings (2 checks)                                    */
/* ------------------------------------------------------------------ */
(function () {
  var allOk = TESTS.every(function (t) {
    var asm = CC0.asmListing(read(t.file));
    return asm.indexOf(".globl main") !== -1 &&
      asm.indexOf("push %rbp") !== -1 &&
      asm.indexOf("ret") !== -1;
  });
  check("asm: all 9 listings contain .globl main, push %rbp, ret", allOk);
  check("asm: test6 listing calls fact",
    CC0.asmListing(read("test6_functions.c")).indexOf("call fact") !== -1);
})();

console.log("----");
console.log(passed + " passed, " + failed + " failed (of " + (passed + failed) + " checks)");
if (failures.length) console.log("failures:\n  " + failures.join("\n  "));
process.exit(failed === 0 ? 0 : 1);
