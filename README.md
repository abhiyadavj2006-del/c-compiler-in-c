# c-compiler-in-c

A minimal but **real** C compiler, written in C — now at an advanced
level: user-defined functions (with parameters, calls, and recursion),
`for` loops, `else if`, `break`/`continue`, block scoping, global
variables, `char`, `void` functions, and bitwise operators, on top of
the original lexer → parser → x86-64 code generator pipeline.

Suggested repo name: **`c-compiler-in-c`** (to be confirmed once GitHub is
connected). This folder is a **standalone repo in its own right** — it is
not a subfolder of a monorepo. It has its own `README.md`, MIT `LICENSE`,
and `.gitignore` at its root, and everything it needs lives inside it.

`cc0` lexes, parses, and generates x86-64 assembly for a small subset of C,
then assembles and links it with `gcc` into a runnable program. It is not a
simulation or pseudocode: the test programs below were actually compiled
with it and run.

## Source

This project follows the tutorial videos by **lolzdev**:

- **"I made a C compiler in C"** (10 min 12 s) —
  <https://www.youtube.com/watch?v=UW8LgC-S_co> — the video from the
  Instagram reel. Its description reads: *"Since GCC is bloated and
  Clang is even worse, I decided to create my own C compiler, in C."*
  and links its source code to <https://github.com/lolzdev/cc>.
- **"Writing a C compiler from scratch"** (~3 hours) — lolzdev's longer
  video on the same project (same repo link), whose chapters cover
  Introduction, Parsing, Data Types, and Program Synthesis.

Honest note on what was actually verified (22 Nov 2026): the video
pages were read (title, description, duration), but **no transcript
of either video could be retrieved** — YouTube's caption endpoint
returned nothing, the transcript library could not reach YouTube from
this machine, and no transcript mirror for the video is indexed. The
linked repo **github.com/lolzdev/cc contains no compiler source code
at all** (verified via the GitHub API: the whole repo is just
`.build.yml` and a BSD-3-Clause `LICENSE`, 2 commits). So this project
is **not** a copy of lolzdev's code — there is no published code to
copy. Instead it implements, from scratch and in the same
architecture the videos teach (lexer → recursive-descent parser →
AST → x86-64 code generation), the standard advanced stages of a
write-a-C-compiler tutorial: functions, data types, and code
generation ("program synthesis"). Where the video's exact feature list
could not be confirmed from a transcript, the README says so below
rather than claiming the video teaches something it may not.

## How it works

```
source .c  →  lexer  →  recursive-descent parser (AST)
           →  x86-64 code generator (AT&T syntax assembly)
           →  gcc (assembler + linker)  →  executable
```

Implementation: a single C file, `src/main.c` (~1250 lines, C11, standard
library only), organised into clear phases — lexer, AST + parser
(precedence climbing: `||` → `&&` → `|` → `^` → `&` → `==`/`!=` →
comparisons → `<<`/`>>` → `+`/`-` → `*`/`/`/`%` → unary → postfix →
primary), scoped symbol tables, and code generator.

- Variables live in 8-byte stack slots: `-8(%rbp)`, `-16(%rbp)`, …
- Each `{ ... }` block is its own scope: a variable declared inside a
  block (or in a `for` init) stops existing at the block's end, and its
  slot is reused. Shadowing an outer variable is allowed.
- Global variables live in `.data` (initialised) or `.bss` (zero) and
  are addressed RIP-relative (`name(%rip)`).
- Functions follow the System V AMD64 calling convention: up to 6
  integer arguments in `%rdi, %rsi, %rdx, %rcx, %r8, %r9`, result in
  `%rax`. Arguments are evaluated left to right, pushed, then popped
  into the registers in reverse order, so nested calls cannot clobber
  an argument that is already waiting.
- The stack is kept 16-byte aligned at every `call`: the code generator
  counts outstanding pushes, and emits a temporary `sub $8, %rsp`
  around a call whenever the depth is odd.
- Expressions are evaluated onto `%rax` with a push/pop stack machine
  (e.g. `a + b * 2` pushes `a`, evaluates `b * 2`, then adds).
- The frame size is rounded up to a multiple of 16 so the stack stays
  16-byte aligned at every call.
- `print(x)` is a builtin that calls `printf("%d\n")`.
- Generated assembly is written to a temp file and linked by invoking
  `gcc` via `fork`/`exec` (no shell), so the compiler never shells out
  to an unquoted command line.

## What it supports

| Feature | Status |
|---|---|
| `int main() { … }` (also `int main(void)`) | ✅ works |
| User-defined functions `int f(int a, int b) { … }`, calls, recursion | ✅ works (new) |
| `void` functions + bare `return;` | ✅ works (new) |
| Up to 6 parameters / arguments (System V registers) | ✅ works (new) |
| Global variables `int g = 5;` / `int h;` (constant initialisers) | ✅ works (new) |
| `int x;` / `int x = <expr>;` | ✅ works |
| `char c = 'A';`, character literals (`'a'`, `'\n'`, `'\t'`, …) | ✅ works (new) |
| `x = <expr>;`, `x +=/-=/*=//=/%= <expr>;` | ✅ works (new) |
| `x++;` `x--;` `++x;` `--x;` (also inside expressions) | ✅ works (new) |
| `return <expr>;` (becomes the program's exit code) | ✅ works |
| `print(<expr>);` builtin | ✅ works |
| `if (<expr>) { … } else if … else { … }` (braces optional) | ✅ works (new) |
| `while (<expr>) { … }` | ✅ works |
| `for (init; cond; incr) { … }` (init may declare a variable) | ✅ works (new) |
| `break;` / `continue;` | ✅ works (new) |
| Block scoping + shadowing (`{ int x = 2; }`) | ✅ works (new) |
| `+ - * / %`, unary `-`, `!`, `~`, `<<`, `>>`, `&`, `^`, `\|` | ✅ works (new) |
| `== != < <= > >=`, `&&`, `\|\|` | ✅ works |
| Parentheses, integer literals, `//` and `/* … */` comments | ✅ works |
| Pointers, arrays, structs, strings, floats | ❌ not supported |
| Preprocessor (`#include`, `#define`) | ❌ not supported |
| More than 6 function parameters / arguments | ❌ not supported |
| Anything other than 64-bit x86-64 Linux | ❌ not supported |

Errors are reported with a line number, e.g. `cc0: error (line 1): use of
undeclared variable 'z'`, and compilation stops (exit code 1). Redeclaring
a variable in the same scope, calling an undefined function, passing the
wrong number of arguments, using a `void` function as a value, and
`break`/`continue` outside a loop are also errors.

## How to run it

Requirements: `gcc` on 64-bit x86-64 Linux. No other dependencies, no
network, no API keys, no environment variables.

```sh
# Build the compiler (produces ./cc0)
make

# Compile a program (default output is ./a.out)
./cc0 tests/test3_variables.c -o prog
./prog; echo $?
# → 50

# See the assembly cc0 generates, instead of linking
./cc0 --asm tests/test3_variables.c -o prog.s

# Run the whole test suite
./run_tests.sh
```

Usage:

```
cc0 [--asm] <program.c> [-o <output>]
```

## What was tested

Run on 2026-11-22, gcc 13.3.0, x86-64 Linux. Build is clean — no
warnings with `-Wall -Wextra`. `./run_tests.sh`: **9 passed, 0 failed.**
(The original 5 tests still pass unchanged; tests 6–9 cover the new
advanced features.)

### Test 1 — return a constant (`tests/test1_return_constant.c`)

```c
int main() {
    return 42;
}
```

Output: no stdout; **exit code 42**. ✅ PASS

### Test 2 — arithmetic and precedence (`tests/test2_arithmetic.c`)

```c
int main() {
    // Precedence: * binds tighter than +, parentheses override both.
    return 2 + 3 * 4;
}
```

Output: no stdout; **exit code 14**. ✅ PASS

### Test 3 — variables with assignment and arithmetic (`tests/test3_variables.c`)

```c
int main() {
    int a = 10;
    int b = 20;
    int c = a + b * 2;
    return c;
}
```

Output: no stdout; **exit code 50**. ✅ PASS

Generated assembly for this program (`./cc0 --asm tests/test3_variables.c -o out.s`):

```asm
    .text
    .globl main
main:
    push %rbp
    mov %rsp, %rbp
    sub $32, %rsp
    mov $10, %rax
    mov %rax, -8(%rbp)
    mov $20, %rax
    mov %rax, -16(%rbp)
    mov -8(%rbp), %rax
    push %rax
    mov -16(%rbp), %rax
    push %rax
    mov $2, %rax
    mov %rax, %rcx
    pop %rax
    imul %rcx, %rax
    mov %rax, %rcx
    pop %rax
    add %rcx, %rax
    mov %rax, -24(%rbp)
    mov -24(%rbp), %rax
    jmp .L0
    mov $0, %rax
.L0:
    leave
    ret
```

### Test 4 — print and a loop (`tests/test4_print_and_loop.c`)

```c
int main() {
    int i = 1;
    int total = 0;
    while (i <= 5) {
        total = total + i;
        i = i + 1;
    }
    print(total);

    int k = 1;
    int total2 = 0;
    while (k <= 10) {
        total2 = total2 + k;
        k = k + 1;
    }
    print(total2);
    return 0;
}
```

Output:

```
15
55
```

Exit code 0. ✅ PASS

### Test 5 — division, modulo, comparison, if/else (`tests/test5_div_mod_compare.c`)

```c
int main() {
    print(17 / 5);
    print(17 % 5);
    print(3 < 5);
    if (10 > 5) {
        print(1);
    } else {
        print(0);
    }
    return 3 < 5;
}
```

Output:

```
3
2
1
1
```

Exit code 1. ✅ PASS

### Test 6 — functions, nested calls, recursion (`tests/test6_functions.c`)

```c
int add(int a, int b) {
    return a + b;
}

int mul(int a, int b) {
    return a * b;
}

int fact(int n) {
    if (n <= 1) {
        return 1;
    }
    return n * fact(n - 1);
}

int main() {
    print(add(3, 4));
    print(mul(6, 7));
    print(fact(5));
    print(add(mul(2, 3), 4));
    return add(1, 2);
}
```

Output:

```
7
42
120
10
```

Exit code 3. ✅ PASS

### Test 7 — for loops, break/continue, else-if (`tests/test7_for_break.c`)

```c
int main() {
    int total = 0;
    for (int i = 1; i <= 10; i++) {
        total += i;
    }
    print(total);

    int count = 0;
    for (int i = 1; i <= 100; i++) {
        if (i % 2 == 0) {
            continue;
        }
        count++;
        if (count == 3) {
            break;
        }
    }
    print(count);

    int x = 75;
    if (x >= 90) {
        print(1);
    } else if (x >= 60) {
        print(2);
    } else {
        print(3);
    }
    return 0;
}
```

Output:

```
55
3
2
```

Exit code 0. ✅ PASS

### Test 8 — char, ++/--, compound assignment, bitwise ops, globals (`tests/test8_char_ops.c`)

```c
int g = 10;
int h;

int bump(int x) {
    return x + 1;
}

int main() {
    char c = 'A';
    print(c);            // 65
    print('a' - 'A');    // 32
    print('\n');         // 10

    int x = 5;
    x++;
    print(x);            // 6
    ++x;
    print(x);            // 7
    x--;
    print(x);            // 6
    x *= 3;
    print(x);            // 18
    x -= 4;
    print(x);            // 14

    print(12 & 10);      // 8
    print(12 | 10);      // 14
    print(12 ^ 10);      // 6
    print(1 << 4);       // 16
    print(256 >> 4);     // 16
    print(~0);           // -1

    g = g + 5;
    print(g);            // 15
    print(h);            // 0
    h = 41;
    print(bump(h));      // 42
    return 0;
}
```

Output:

```
65
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
42
```

Exit code 0. ✅ PASS

### Test 9 — void functions, shared globals, fibonacci (`tests/test9_void_globals.c`)

```c
int counter;

void greet(int n) {
    print(n);
    counter = counter + 1;
}

int fib(int n) {
    if (n < 2) {
        return n;
    }
    return fib(n - 1) + fib(n - 2);
}

int main() {
    greet(7);
    greet(8);
    print(counter);
    print(fib(10));

    int i = 0;
    while (i < 3) {
        greet(100 + i);
        i++;
    }
    print(counter);
    return fib(6);
}
```

Output:

```
7
8
2
55
100
101
102
5
```

Exit code 8. ✅ PASS

### Extra checks (run by hand, not in `run_tests.sh`)

| Program | Result |
|---|---|
| `(2 + 3) * 4`, unary minus, `if (y == 80)` | printed `1` then `80`, exit code 20 ✅ |
| `while` loop + `!0` + `1 && 0` | printed `6`, `1`, `0`, exit code 6 ✅ |
| `return z;` with `z` undeclared | `cc0: error (line 1): use of undeclared variable 'z'`, exit code 1 ✅ |
| Shadowing: `{ int x = 2; print(x); }` inside `int x = 1;` | printed `2` then `1` ✅ |
| `for (int i = 0; ; i++) { if (i == 3) break; print(i); }` | printed `0`, `1`, `2` ✅ |
| 6-argument function `sum6(1, 2, 3, 4, 5, 6)` | printed `21` ✅ |
| `{ int y = 3; } print(y);` (out-of-scope use) | `cc0: error (line 5): use of undeclared variable 'y'`, exit code 1 ✅ |
| `print(nope(1));` (undefined function) | `cc0: error (line 2): call to undefined function 'nope'`, exit code 1 ✅ |
| `add(1)` where `add` takes 2 params | `cc0: error (line 3): function 'add' expects 2 argument(s), got 1`, exit code 1 ✅ |
| `break;` outside a loop | `cc0: error (line 2): 'break' outside a loop`, exit code 1 ✅ |
| `int x = f();` where `f` is `void` | `cc0: error (line 3): void function 'f' used as a value`, exit code 1 ✅ |

## Limitations

- Only `int` and `char` — and `char` is stored in a full 8-byte slot,
  exactly like `int`. There is no 1-byte storage, no `char *`, and no
  real type checking: a `char` variable can hold any integer.
- No pointers, arrays, structs, unions, enums, strings, or floats.
  (Adding pointers would need a real type system, address-of /
  dereference, and pointer arithmetic — the biggest remaining step.)
- No preprocessor, so no `#include` / `#define`.
- At most 6 function parameters and arguments (System V register
  arguments only — nothing is passed on the stack).
- No assignment as an expression (`if (x = 3) …`, `y = x = 5;` do not
  parse); assignment is a statement only. No ternary `?:`, no `switch`,
  no `do … while`, no `goto`.
- A function must be defined in the same file (no separate compilation,
  no linking against other object files); calls to functions defined
  later in the file are fine.
- Only 64-bit x86-64 Linux (the code generator emits x86-64 assembly).
- Division by zero crashes the compiled program, as in C.
- Exit codes are 8-bit, so `return 300;` shows up as exit code 44.
- Error messages name the line but not the column; there is no
  optimisation — generated code is straightforward stack-machine code.
- `main` must be `int main()` with no parameters.

## What was intentionally left out, and why

The video's exact feature list could not be confirmed (no transcript,
no published source — see "Source" above), so the advanced set above
is the standard write-a-C-compiler progression (functions → types →
globals → control flow), implemented from scratch. The next steps in
that progression were deliberately **not** done here, because each one
needs a bigger redesign than a single update should carry:

1. **Pointers and arrays** — need a type system (every variable and
   expression gains a type), `&` / `*`, pointer arithmetic, and array
   indexing. That is roughly as much work again as everything above.
2. **Structs / unions** — need aggregate types, member offsets, and
   `.` / `->` in the parser and code generator.
3. **Strings** — need `char *` plus string literals in `.rodata`,
   which depends on (1).
4. **Preprocessor** (`#include`, `#define`) — a separate pass before
   lexing, with file loading and macro expansion.
5. **More than 6 arguments** — stack-passed arguments under the
   System V ABI, plus keeping the stack aligned while doing so.
6. **An optimiser** (constant folding, dead-code removal, register
   allocation instead of push/pop for every sub-expression).
7. **Better diagnostics** (column numbers, "did you mean …"
   suggestions) and a test suite that also checks that invalid
   programs are rejected (the extra checks above are run by hand).

## Live / Deploy

"Live" for a C compiler does not mean a hosted compiler service. The
live artefact for this repo is the **static demo page** in `demo/`:
`demo/index.html` explains what the compiler does and shows example C
programs, their compiled outputs, and real generated assembly. It needs
no server, no build step, and no environment variables — it is plain
HTML/CSS with no external dependencies.

**Live:** the repo is already on GitHub as `c-compiler-in-c` under
`abhiyadavj2006-del`, and GitHub Pages is live — the demo page is at
`https://abhiyadavj2006-del.github.io/c-compiler-in-c/demo/`. This
update (functions, `for`, globals, `char`, …) is **not pushed yet** —
it lives only in this folder until someone runs the push below.

**To push this update (not done by the agent — no push was made):**

```sh
cd c-compiler-in-c
git add src/main.c tests/ run_tests.sh README.md demo/index.html
git commit -m "Advanced level: functions, for loops, globals, char, block scoping"
git push
```

GitHub Pages redeploys automatically from the `main` branch, so the
demo updates itself after the push. No settings need changing.

The compiler itself is not deployed anywhere: it is a command-line tool
you build and run locally with `make`, as described above.

## Project structure

```
c-compiler-in-c/
├── README.md            # this file
├── LICENSE              # MIT
├── .gitignore           # excludes cc0, *.o, *.s, *.out / a.out
├── Makefile             # make → builds ./cc0 from src/main.c
├── run_tests.sh         # builds cc0, compiles + runs all tests, checks outputs
├── src/
│   └── main.c           # the whole compiler: lexer, parser, code generator, driver
├── tests/
│   ├── test1_return_constant.c
│   ├── test2_arithmetic.c
│   ├── test3_variables.c
│   ├── test4_print_and_loop.c
│   ├── test5_div_mod_compare.c
│   ├── test6_functions.c        # functions, nested calls, recursion
│   ├── test7_for_break.c        # for, break/continue, else-if
│   ├── test8_char_ops.c         # char, ++/--, +=, bitwise, globals
│   └── test9_void_globals.c     # void functions, shared globals, fib
└── demo/
    └── index.html       # static demo page for GitHub Pages / any static host
```

## License

MIT — see [LICENSE](LICENSE).
