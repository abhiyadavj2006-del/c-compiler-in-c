# c-compiler-in-c

A minimal but **real** C compiler, written in C.

Suggested repo name: **`c-compiler-in-c`** (to be confirmed once GitHub is
connected). This folder is a **standalone repo in its own right** — it is
not a subfolder of a monorepo. It has its own `README.md`, MIT `LICENSE`,
and `.gitignore` at its root, and everything it needs lives inside it.

`cc0` lexes, parses, and generates x86-64 assembly for a small subset of C,
then assembles and links it with `gcc` into a runnable program. It is not a
simulation or pseudocode: the test programs below were actually compiled
with it and run.

## How it works

```
source .c  →  lexer  →  recursive-descent parser (AST)
           →  x86-64 code generator (AT&T syntax assembly)
           →  gcc (assembler + linker)  →  executable
```

Implementation: a single C file, `src/main.c` (~650 lines, C11, standard
library only), organised into clear phases — lexer, AST + parser
(precedence climbing: `||` → `&&` → `==`/`!=` → comparisons → `+`/`-` →
`*`/`/`/`%` → unary → primary), symbol table, and code generator.

- Variables live in 8-byte stack slots: `-8(%rbp)`, `-16(%rbp)`, …
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
| `int x;` / `int x = <expr>;` | ✅ works |
| `x = <expr>;` | ✅ works |
| `return <expr>;` (becomes the program's exit code) | ✅ works |
| `print(<expr>);` builtin | ✅ works |
| `if (<expr>) { … } else { … }` | ✅ works |
| `while (<expr>) { … }` | ✅ works |
| `+ - * / %`, unary `-` and `!` | ✅ works |
| `== != < <= > >=`, `&&`, `\|\|` | ✅ works |
| Parentheses, integer literals, `//` and `/* … */` comments | ✅ works |
| Other functions, function calls (except `print`) | ❌ not supported |
| Types other than `int` (`char`, pointers, arrays, structs, floats, strings) | ❌ not supported |
| Preprocessor (`#include`, `#define`) | ❌ not supported |
| Anything other than 64-bit x86-64 Linux | ❌ not supported |

Errors are reported with a line number, e.g. `cc0: error (line 1): use of
undeclared variable 'z'`, and compilation stops (exit code 1). Redeclaring
a variable is also an error.

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

Run on 2026-11-21, gcc 13.3.0, x86-64 Linux. Build is clean — no
warnings with `-Wall -Wextra`. `./run_tests.sh`: **5 passed, 0 failed.**

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
    jmp .Lret
    mov $0, %rax
.Lret:
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

### Extra checks (run by hand, not in `run_tests.sh`)

| Program | Result |
|---|---|
| `(2 + 3) * 4`, unary minus, `if (y == 80)` | printed `1` then `80`, exit code 20 ✅ |
| `while` loop + `!0` + `1 && 0` | printed `6`, `1`, `0`, exit code 6 ✅ |
| `return z;` with `z` undeclared | `cc0: error (line 1): use of undeclared variable 'z'`, exit code 1 ✅ |

## Limitations

- Only `int main()` — no other functions, no user-defined function calls.
- Only `int` — no `char`, pointers, arrays, structs, strings, or floats.
- No preprocessor, so no `#include` / `#define`.
- Only 64-bit x86-64 Linux (the code generator emits x86-64 assembly).
- Division by zero crashes the compiled program, as in C.
- Exit codes are 8-bit, so `return 300;` shows up as exit code 44.
- Error messages name the line but not the column; there is no
  optimisation — generated code is straightforward stack-machine code.

## Next steps to make it fuller

1. More types: `char`, pointers, arrays, and `struct`.
2. Real functions with parameters, local scopes, and calls (plus a
   proper calling convention for arguments).
3. `for` loops, `break`/`continue`, and `else if` chains.
4. A preprocessor (`#include`, `#define`) and string literals.
5. An optimiser (constant folding, dead-code removal, register
   allocation instead of push/pop for every sub-expression).
6. Better diagnostics (column numbers, "did you mean …" suggestions)
   and a test suite that also checks that invalid programs are rejected.

## Live / Deploy

"Live" for a C compiler does not mean a hosted compiler service. The
live artefact for this repo is the **static demo page** in `demo/`:
`demo/index.html` explains what the compiler does and shows example C
programs, their compiled outputs, and real generated assembly. It needs
no server, no build step, and no environment variables — it is plain
HTML/CSS with no external dependencies.

**To take it live (not done yet — GitHub is not connected, no remote
exists, nothing has been pushed or deployed):**

1. Create the repo on GitHub (suggested name: `c-compiler-in-c`).
2. Upload this folder as that repo's entire contents:
   ```sh
   cd c-compiler-in-c
   git init
   git add .
   git commit -m "Initial commit: cc0, a minimal C compiler in C"
   git remote add origin https://github.com/<username>/c-compiler-in-c.git
   git push -u origin main
   ```
   (`<username>` is not known yet — fill it in when GitHub is connected.)
3. Enable GitHub Pages: repo **Settings → Pages → Build and deployment
   → Deploy from a branch → Branch: `main`, Folder: `/ (root)`**.
   The demo is then live at
   `https://<username>.github.io/c-compiler-in-c/demo/`.
4. Alternatives for the same static page, no code changes needed:
   - **Local preview:** `python3 -m http.server` in the repo root, then
     open `http://localhost:8000/demo/`.
   - **Netlify Drop / Vercel / Cloudflare Pages:** drag-and-drop the
     `demo/` folder (or the repo) — static hosting only, no env vars.

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
│   └── test5_div_mod_compare.c
└── demo/
    └── index.html       # static demo page for GitHub Pages / any static host
```

## License

MIT — see [LICENSE](LICENSE).
