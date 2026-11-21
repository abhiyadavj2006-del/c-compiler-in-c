/*
 * cc0 — a minimal but real C compiler, written in C.
 *
 * Pipeline:  source text -> lexer -> recursive-descent parser (AST)
 *          -> x86-64 code generator (AT&T syntax assembly)
 *          -> gcc (assembler + linker) -> executable
 *
 * Language subset (all of it actually works — see tests/ and README.md):
 *   int main() { ... }            // the only function
 *   int x;  int x = <expr>;       // declarations, optional initialiser
 *   x = <expr>;                   // assignment
 *   return <expr>;                // exit code of the program
 *   print(<expr>);                // builtin: prints the value + newline
 *   if (<expr>) { ... } else { ... }
 *   while (<expr>) { ... }
 *   <expr> ;                      // expression statement
 *   Expressions: integer literals, variables, parentheses,
 *     unary - and !, binary + - * / %,
 *     == != < <= > >=, && ||
 *   Comments: // ... and slash-star ... star-slash
 *
 * Usage:
 *   cc0 program.c                -> ./a.out
 *   cc0 program.c -o program     -> ./program
 *   cc0 --asm program.c -o program.s   -> emit assembly only, no linking
 *
 * Only the C standard library is used. gcc is invoked only as the
 * assembler/linker for the generated .s file.
 */
#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/* Errors                                                              */
/* ------------------------------------------------------------------ */
static int g_line = 1;
static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fprintf(stderr, "cc0: error (line %d): ", g_line);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

/* ------------------------------------------------------------------ */
/* Lexer                                                               */
/* ------------------------------------------------------------------ */
typedef enum {
    T_EOF, T_INT, T_RETURN, T_IF, T_ELSE, T_WHILE,
    T_IDENT, T_NUM,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT,
    T_ASSIGN, T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE,
    T_AND, T_OR, T_NOT,
    T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_SEMI
} TokKind;

typedef struct {
    TokKind kind;
    long    num;              /* T_NUM */
    char    ident[64];        /* T_IDENT */
} Token;

static const char *g_src;     /* source text */
static size_t g_pos;          /* offset into g_src */
static Token g_tok;           /* current token (lookahead of 1) */

static void next_token(void) {
    for (;;) {
        char c = g_src[g_pos];
        if (c == '\0') { g_tok.kind = T_EOF; return; }
        if (c == '\n') { g_line++; g_pos++; continue; }
        if (isspace((unsigned char)c)) { g_pos++; continue; }
        if (c == '/' && g_src[g_pos + 1] == '/') {
            while (g_src[g_pos] && g_src[g_pos] != '\n') g_pos++;
            continue;
        }
        if (c == '/' && g_src[g_pos + 1] == '*') {
            g_pos += 2;
            while (g_src[g_pos] && !(g_src[g_pos] == '*' && g_src[g_pos + 1] == '/')) {
                if (g_src[g_pos] == '\n') g_line++;
                g_pos++;
            }
            if (!g_src[g_pos]) die("unterminated block comment");
            g_pos += 2;
            continue;
        }
        break;
    }

    char c = g_src[g_pos];
    if (isalpha((unsigned char)c) || c == '_') {
        size_t start = g_pos;
        while (isalnum((unsigned char)g_src[g_pos]) || g_src[g_pos] == '_') g_pos++;
        size_t len = g_pos - start;
        if (len >= sizeof g_tok.ident) die("identifier too long");
        memcpy(g_tok.ident, g_src + start, len);
        g_tok.ident[len] = '\0';
        if (!strcmp(g_tok.ident, "int"))    g_tok.kind = T_INT;
        else if (!strcmp(g_tok.ident, "return")) g_tok.kind = T_RETURN;
        else if (!strcmp(g_tok.ident, "if"))     g_tok.kind = T_IF;
        else if (!strcmp(g_tok.ident, "else"))   g_tok.kind = T_ELSE;
        else if (!strcmp(g_tok.ident, "while"))  g_tok.kind = T_WHILE;
        else g_tok.kind = T_IDENT;
        return;
    }
    if (isdigit((unsigned char)c)) {
        long v = 0;
        while (isdigit((unsigned char)g_src[g_pos])) {
            v = v * 10 + (g_src[g_pos] - '0');
            g_pos++;
        }
        g_tok.kind = T_NUM; g_tok.num = v;
        return;
    }
    g_pos++;
    switch (c) {
        case '+': g_tok.kind = T_PLUS; return;
        case '-': g_tok.kind = T_MINUS; return;
        case '*': g_tok.kind = T_STAR; return;
        case '/': g_tok.kind = T_SLASH; return;
        case '%': g_tok.kind = T_PERCENT; return;
        case '(': g_tok.kind = T_LPAREN; return;
        case ')': g_tok.kind = T_RPAREN; return;
        case '{': g_tok.kind = T_LBRACE; return;
        case '}': g_tok.kind = T_RBRACE; return;
        case ';': g_tok.kind = T_SEMI; return;
        case '=': g_tok.kind = (g_src[g_pos] == '=') ? (g_pos++, T_EQ) : T_ASSIGN; return;
        case '!': g_tok.kind = (g_src[g_pos] == '=') ? (g_pos++, T_NE) : T_NOT; return;
        case '<': g_tok.kind = (g_src[g_pos] == '=') ? (g_pos++, T_LE) : T_LT; return;
        case '>': g_tok.kind = (g_src[g_pos] == '=') ? (g_pos++, T_GE) : T_GT; return;
        case '&': if (g_src[g_pos] == '&') { g_pos++; g_tok.kind = T_AND; return; } break;
        case '|': if (g_src[g_pos] == '|') { g_pos++; g_tok.kind = T_OR; return; } break;
    }
    die("unexpected character '%c'", c);
}

static bool accept(TokKind k) {
    if (g_tok.kind == k) { next_token(); return true; }
    return false;
}
static void expect(TokKind k, const char *what) {
    if (g_tok.kind != k) die("expected %s, found something else", what);
    next_token();
}

/* ------------------------------------------------------------------ */
/* AST                                                                 */
/* ------------------------------------------------------------------ */
typedef enum { E_NUM, E_VAR, E_BIN, E_UN } ExprKind;
typedef struct Expr Expr;
struct Expr {
    ExprKind kind;
    long num;            /* E_NUM */
    int var;             /* E_VAR: index into symbol table */
    TokKind op;          /* E_BIN / E_UN */
    Expr *left, *right;  /* E_BIN: both; E_UN: left only */
};

typedef enum { S_DECL, S_ASSIGN, S_RETURN, S_PRINT, S_IF, S_WHILE, S_BLOCK, S_EXPR } StmtKind;
typedef struct Stmt Stmt;
struct Stmt {
    StmtKind kind;
    int var;             /* S_DECL / S_ASSIGN */
    Expr *expr;          /* S_DECL (may be NULL), S_ASSIGN, S_RETURN, S_PRINT, S_EXPR */
    Expr *cond;          /* S_IF / S_WHILE */
    Stmt *then_branch;  /* S_IF / S_WHILE (a block) */
    Stmt *else_branch;  /* S_IF only (a block, or NULL) */
    Stmt *first;         /* S_BLOCK: first child */
    Stmt *next;          /* linked list inside a block */
};

static Expr *new_expr(ExprKind k) {
    Expr *e = calloc(1, sizeof *e);
    if (!e) die("out of memory");
    e->kind = k;
    return e;
}
static Stmt *new_stmt(StmtKind k) {
    Stmt *s = calloc(1, sizeof *s);
    if (!s) die("out of memory");
    s->kind = k;
    return s;
}

/* Symbol table: one scope, the body of main. Slots are 8 bytes each. */
#define MAX_VARS 256
static char g_vars[MAX_VARS][64];
static int g_nvars;
static int find_var(const char *name) {
    for (int i = 0; i < g_nvars; i++)
        if (!strcmp(g_vars[i], name)) return i;
    return -1;
}
static int add_var(const char *name) {
    if (find_var(name) >= 0) die("variable '%s' redeclared", name);
    if (g_nvars >= MAX_VARS) die("too many variables");
    snprintf(g_vars[g_nvars], sizeof g_vars[0], "%s", name);
    return g_nvars++;
}

/* ------------------------------------------------------------------ */
/* Parser (recursive descent, precedence climbing)                     */
/*                                                                     */
/*   expr    := or                                                      */
/*   or      := and ( "||" and )*                                      */
/*   and     := equality ( "&&" equality )*                            */
/*   equality:= relational ( ("==" | "!=") relational )*               */
/*   relational := additive ( ("<" | "<=" | ">" | ">=") additive )*    */
/*   additive:= multiplicative ( ("+" | "-") multiplicative )*         */
/*   multiplicative := unary ( ("*" | "/" | "%") unary )*              */
/*   unary   := ("-" | "!") unary | primary                            */
/*   primary := NUMBER | IDENT | "(" expr ")"                          */
/* ------------------------------------------------------------------ */
static Expr *parse_expr(void);

static Expr *parse_primary(void) {
    if (g_tok.kind == T_NUM) {
        Expr *e = new_expr(E_NUM); e->num = g_tok.num; next_token(); return e;
    }
    if (g_tok.kind == T_IDENT) {
        int v = find_var(g_tok.ident);
        if (v < 0) die("use of undeclared variable '%s'", g_tok.ident);
        Expr *e = new_expr(E_VAR); e->var = v; next_token(); return e;
    }
    if (accept(T_LPAREN)) {
        Expr *e = parse_expr();
        expect(T_RPAREN, "')'");
        return e;
    }
    die("expected an expression");
    return NULL;
}

static Expr *parse_unary(void) {
    if (g_tok.kind == T_MINUS || g_tok.kind == T_NOT) {
        TokKind op = g_tok.kind; next_token();
        Expr *e = new_expr(E_UN); e->op = op; e->left = parse_unary(); return e;
    }
    return parse_primary();
}

static Expr *parse_bin_level(Expr *(*next_level)(void),
                             const TokKind *ops, int nops) {
    Expr *left = next_level();
    for (;;) {
        TokKind op = g_tok.kind;
        bool match = false;
        for (int i = 0; i < nops; i++) if (op == ops[i]) match = true;
        if (!match) return left;
        next_token();
        Expr *e = new_expr(E_BIN); e->op = op;
        e->left = left; e->right = next_level();
        left = e;
    }
}

static Expr *parse_mul(void) {
    static const TokKind ops[] = { T_STAR, T_SLASH, T_PERCENT };
    return parse_bin_level(parse_unary, ops, 3);
}
static Expr *parse_add(void) {
    static const TokKind ops[] = { T_PLUS, T_MINUS };
    return parse_bin_level(parse_mul, ops, 2);
}
static Expr *parse_rel(void) {
    static const TokKind ops[] = { T_LT, T_LE, T_GT, T_GE };
    return parse_bin_level(parse_add, ops, 4);
}
static Expr *parse_eq(void) {
    static const TokKind ops[] = { T_EQ, T_NE };
    return parse_bin_level(parse_rel, ops, 2);
}
static Expr *parse_and(void) {
    static const TokKind ops[] = { T_AND };
    return parse_bin_level(parse_eq, ops, 1);
}
static Expr *parse_expr(void) {
    static const TokKind ops[] = { T_OR };
    return parse_bin_level(parse_and, ops, 1);
}

/* Statements. parse_block expects the '{' to have been consumed by the
 * caller for the function body, or consumes it itself for if/while. */
static Stmt *parse_block(void);  /* forward */
static Stmt *parse_stmt(void) {
    if (g_tok.kind == T_INT) {
        next_token();
        if (g_tok.kind != T_IDENT) die("expected a variable name after 'int'");
        int v = add_var(g_tok.ident);
        next_token();
        Stmt *s = new_stmt(S_DECL); s->var = v;
        if (accept(T_ASSIGN)) s->expr = parse_expr();
        expect(T_SEMI, "';'");
        return s;
    }
    if (g_tok.kind == T_RETURN) {
        next_token();
        Stmt *s = new_stmt(S_RETURN); s->expr = parse_expr();
        expect(T_SEMI, "';'");
        return s;
    }
    if (g_tok.kind == T_IF) {
        next_token();
        expect(T_LPAREN, "'(' after 'if'");
        Stmt *s = new_stmt(S_IF); s->cond = parse_expr();
        expect(T_RPAREN, "')'");
        expect(T_LBRACE, "'{' after if condition");
        s->then_branch = parse_block();
        if (g_tok.kind == T_ELSE) {
            next_token();
            expect(T_LBRACE, "'{' after 'else'");
            s->else_branch = parse_block();
        }
        return s;
    }
    if (g_tok.kind == T_WHILE) {
        next_token();
        expect(T_LPAREN, "'(' after 'while'");
        Stmt *s = new_stmt(S_WHILE); s->cond = parse_expr();
        expect(T_RPAREN, "')'");
        expect(T_LBRACE, "'{' after while condition");
        s->then_branch = parse_block();
        return s;
    }
    if (g_tok.kind == T_LBRACE) {
        next_token();
        return parse_block();
    }
    if (g_tok.kind == T_IDENT) {
        /* print(expr);  |  x = expr;  |  expr; */
        char name[64];
        snprintf(name, sizeof name, "%s", g_tok.ident);
        next_token();
        if (!strcmp(name, "print") && g_tok.kind == T_LPAREN) {
            next_token();
            Stmt *s = new_stmt(S_PRINT); s->expr = parse_expr();
            expect(T_RPAREN, "')'");
            expect(T_SEMI, "';'");
            return s;
        }
        if (g_tok.kind == T_ASSIGN) {
            int v = find_var(name);
            if (v < 0) die("assignment to undeclared variable '%s'", name);
            next_token();
            Stmt *s = new_stmt(S_ASSIGN); s->var = v; s->expr = parse_expr();
            expect(T_SEMI, "';'");
            return s;
        }
        /* A bare identifier as the start of an expression statement is
         * not supported (no function calls besides print) — fail clearly. */
        die("unexpected identifier '%s' (only assignment and print() are supported)", name);
        return NULL;
    }
    /* Expression statement starting with a number / '(' / unary op. */
    Stmt *s = new_stmt(S_EXPR); s->expr = parse_expr();
    expect(T_SEMI, "';'");
    return s;
}

static Stmt *parse_block(void) {
    Stmt *block = new_stmt(S_BLOCK);
    Stmt **tail = &block->first;
    while (g_tok.kind != T_RBRACE) {
        if (g_tok.kind == T_EOF) die("unexpected end of file — missing '}'");
        *tail = parse_stmt();
        tail = &(*tail)->next;
    }
    next_token(); /* consume '}' */
    return block;
}

static Stmt *parse_program(void) {
    expect(T_INT, "'int'");
    if (g_tok.kind != T_IDENT || strcmp(g_tok.ident, "main") != 0)
        die("only 'int main()' is supported as a function");
    next_token();
    expect(T_LPAREN, "'(' after 'main'");
    /* Allow "int main()" and "int main(void)". */
    if (g_tok.kind == T_IDENT && !strcmp(g_tok.ident, "void")) next_token();
    expect(T_RPAREN, "')' after 'main'");
    expect(T_LBRACE, "'{' after 'int main()'");
    Stmt *body = parse_block();
    if (g_tok.kind != T_EOF) die("unexpected code after the end of main()");
    return body;
}

/* ------------------------------------------------------------------ */
/* Code generation — x86-64, System V, AT&T syntax                    */
/*                                                                     */
/* Expression values are left in %rax. Binary operators evaluate the   */
/* left operand, push it, evaluate the right, then combine. Variables  */
/* live in 8-byte slots at -8, -16, ... (%rbp).                        */
/* ------------------------------------------------------------------ */
static FILE *g_out;
static int g_label;

static int fresh_label(void) { return g_label++; }

static void gen_expr(Expr *e) {
    switch (e->kind) {
    case E_NUM:
        fprintf(g_out, "    mov $%ld, %%rax\n", e->num);
        return;
    case E_VAR:
        fprintf(g_out, "    mov %d(%%rbp), %%rax\n", -8 * (e->var + 1));
        return;
    case E_UN:
        gen_expr(e->left);
        if (e->op == T_MINUS) fprintf(g_out, "    neg %%rax\n");
        else { /* ! */
            fprintf(g_out, "    test %%rax, %%rax\n");
            fprintf(g_out, "    sete %%al\n");
            fprintf(g_out, "    movzb %%al, %%rax\n");
        }
        return;
    case E_BIN: {
        TokKind op = e->op;
        if (op == T_AND || op == T_OR) {
            /* Normalise each side to 0/1, then AND / OR. */
            gen_expr(e->left);
            fprintf(g_out, "    test %%rax, %%rax\n");
            fprintf(g_out, "    setne %%al\n");
            fprintf(g_out, "    movzb %%al, %%rax\n");
            fprintf(g_out, "    push %%rax\n");
            gen_expr(e->right);
            fprintf(g_out, "    test %%rax, %%rax\n");
            fprintf(g_out, "    setne %%al\n");
            fprintf(g_out, "    movzb %%al, %%rax\n");
            fprintf(g_out, "    pop %%rcx\n");
            fprintf(g_out, "    %s %%rcx, %%rax\n", op == T_AND ? "and" : "or");
            return;
        }
        gen_expr(e->left);
        fprintf(g_out, "    push %%rax\n");
        gen_expr(e->right);
        fprintf(g_out, "    mov %%rax, %%rcx\n");
        fprintf(g_out, "    pop %%rax\n");
        switch (op) {
        case T_PLUS:  fprintf(g_out, "    add %%rcx, %%rax\n"); break;
        case T_MINUS: fprintf(g_out, "    sub %%rcx, %%rax\n"); break;
        case T_STAR:  fprintf(g_out, "    imul %%rcx, %%rax\n"); break;
        case T_SLASH:
            fprintf(g_out, "    cqo\n");
            fprintf(g_out, "    idiv %%rcx\n");
            break;
        case T_PERCENT:
            fprintf(g_out, "    cqo\n");
            fprintf(g_out, "    idiv %%rcx\n");
            fprintf(g_out, "    mov %%rdx, %%rax\n");
            break;
        case T_EQ: case T_NE: case T_LT: case T_LE: case T_GT: case T_GE: {
            const char *cc = op == T_EQ ? "sete" : op == T_NE ? "setne"
                : op == T_LT ? "setl" : op == T_LE ? "setle"
                : op == T_GT ? "setg" : "setge";
            fprintf(g_out, "    cmp %%rcx, %%rax\n");
            fprintf(g_out, "    %s %%al\n", cc);
            fprintf(g_out, "    movzb %%al, %%rax\n");
            break;
        }
        default: die("internal error: unknown binary operator");
        }
        return;
    }
    }
}

static void gen_stmt(Stmt *s) {
    while (s) {
        switch (s->kind) {
        case S_BLOCK:
            gen_stmt(s->first);
            break;
        case S_DECL:
            if (s->expr) gen_expr(s->expr);
            else fprintf(g_out, "    mov $0, %%rax\n");
            fprintf(g_out, "    mov %%rax, %d(%%rbp)\n", -8 * (s->var + 1));
            break;
        case S_ASSIGN:
            gen_expr(s->expr);
            fprintf(g_out, "    mov %%rax, %d(%%rbp)\n", -8 * (s->var + 1));
            break;
        case S_RETURN:
            gen_expr(s->expr);
            fprintf(g_out, "    jmp .Lret\n");
            break;
        case S_PRINT:
            gen_expr(s->expr);
            fprintf(g_out, "    mov %%rax, %%rsi\n");
            fprintf(g_out, "    lea .Lfmt(%%rip), %%rdi\n");
            fprintf(g_out, "    xor %%eax, %%eax\n");
            fprintf(g_out, "    call printf@PLT\n");
            break;
        case S_IF: {
            int lelse = fresh_label(), lend = fresh_label();
            gen_expr(s->cond);
            fprintf(g_out, "    test %%rax, %%rax\n");
            fprintf(g_out, "    je .L%d\n", s->else_branch ? lelse : lend);
            gen_stmt(s->then_branch);
            if (s->else_branch) {
                fprintf(g_out, "    jmp .L%d\n", lend);
                fprintf(g_out, ".L%d:\n", lelse);
                gen_stmt(s->else_branch);
            }
            fprintf(g_out, ".L%d:\n", lend);
            break;
        }
        case S_WHILE: {
            int lstart = fresh_label(), lend = fresh_label();
            fprintf(g_out, ".L%d:\n", lstart);
            gen_expr(s->cond);
            fprintf(g_out, "    test %%rax, %%rax\n");
            fprintf(g_out, "    je .L%d\n", lend);
            gen_stmt(s->then_branch);
            fprintf(g_out, "    jmp .L%d\n", lstart);
            fprintf(g_out, ".L%d:\n", lend);
            break;
        }
        case S_EXPR:
            gen_expr(s->expr);
            break;
        }
        s = s->next;
    }
}

static void gen_program(FILE *out, Stmt *body) {
    g_out = out;
    /* Frame size: 8 bytes per variable, rounded up to a multiple of 16
     * so the stack stays 16-byte aligned at every call (printf). */
    int frame = ((g_nvars * 8 + 15) / 16) * 16;
    fprintf(out, "    .text\n");
    fprintf(out, "    .globl main\n");
    fprintf(out, "main:\n");
    fprintf(out, "    push %%rbp\n");
    fprintf(out, "    mov %%rsp, %%rbp\n");
    if (frame) fprintf(out, "    sub $%d, %%rsp\n", frame);
    gen_stmt(body);
    /* Falling off the end of main() returns 0, like C. */
    fprintf(out, "    mov $0, %%rax\n");
    fprintf(out, ".Lret:\n");
    fprintf(out, "    leave\n");
    fprintf(out, "    ret\n");
    fprintf(out, "    .section .rodata\n");
    fprintf(out, ".Lfmt:\n");
    fprintf(out, "    .string \"%%d\\n\"\n");
    /* Mark the stack non-executable, so the linker stays quiet. */
    fprintf(out, "    .section .note.GNU-stack,\"\",@progbits\n");
}

/* ------------------------------------------------------------------ */
/* Driver                                                              */
/* ------------------------------------------------------------------ */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cc0: cannot open '%s'\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (!buf) die("out of memory");
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) die("cannot read '%s'", path);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* Run: gcc <asm> -o <output>  (fork/exec, no shell). */
static void run_gcc(const char *asm_path, const char *output) {
    pid_t pid = fork();
    if (pid < 0) die("fork failed");
    if (pid == 0) {
        execlp("gcc", "gcc", asm_path, "-o", output, (char *)NULL);
        _exit(127);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) die("waitpid failed");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        die("gcc failed to assemble/link the generated code");
}

static void usage(void) {
    fprintf(stderr,
        "usage: cc0 [--asm] <program.c> [-o <output>]\n"
        "  default output is ./a.out; --asm emits assembly instead of linking\n");
    exit(1);
}

int main(int argc, char **argv) {
    const char *input = NULL, *output = "a.out";
    bool asm_only = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--asm")) asm_only = true;
        else if (!strcmp(argv[i], "-o")) {
            if (++i >= argc) usage();
            output = argv[i];
        } else if (argv[i][0] == '-') usage();
        else if (!input) input = argv[i];
        else usage();
    }
    if (!input) usage();

    char *src = read_file(input);
    g_src = src; g_pos = 0; g_line = 1;
    next_token();
    Stmt *body = parse_program();

    if (asm_only) {
        FILE *out = fopen(output, "w");
        if (!out) { fprintf(stderr, "cc0: cannot write '%s'\n", output); return 1; }
        gen_program(out, body);
        fclose(out);
        return 0;
    }

    /* Generate assembly to a temp file, then assemble + link with gcc. */
    char asm_path[] = "/tmp/cc0-XXXXXX.s";
    int fd = mkstemps(asm_path, 2);
    if (fd < 0) die("cannot create temporary assembly file");
    FILE *out = fdopen(fd, "w");
    if (!out) die("cannot open temporary assembly file");
    gen_program(out, body);
    fclose(out);
    run_gcc(asm_path, output);
    unlink(asm_path);
    return 0;
}
