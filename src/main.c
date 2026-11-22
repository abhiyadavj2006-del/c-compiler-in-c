/*
 * cc0 — a minimal but real C compiler, written in C.
 *
 * Pipeline:  source text -> lexer -> recursive-descent parser (AST)
 *          -> x86-64 code generator (AT&T syntax assembly)
 *          -> gcc (assembler + linker) -> executable
 *
 * Language subset (all of it actually works — see tests/ and README.md):
 *   int f(int a, int b) { ... }   // user-defined functions, up to 6 params
 *   void f(...) { ... }           // void functions; bare `return;`
 *   int main() { ... }            // required entry point
 *   int x;  int x = <expr>;       // locals, optional initialiser
 *   char c = 'A';                 // char is stored like int (see README)
 *   int g = 5;                    // globals, outside any function
 *   x = <expr>;  x += <expr>; ... // assignment, += -= *= /= %=
 *   x++;  x--;  ++x;  --x;        // increment / decrement
 *   return <expr>;  return;       // exit code of the program
 *   print(<expr>);                // builtin: prints the value + newline
 *   if (<expr>) ... else if ... else ...
 *   while (<expr>) { ... }
 *   for (init; cond; incr) { ... }
 *   break;  continue;
 *   <expr> ;                      // expression statement
 *   Expressions: integer and character literals, variables, function
 *     calls (incl. recursion), parentheses, unary - ! ~ ++ --,
 *     binary + - * / %, << >>, < <= > >=, == !=, & ^ |, && ||
 *   Comments: // ... and slash-star ... star-slash
 *
 * Usage:
 *   cc0 program.c                -> ./a.out
 *   cc0 program.c -o program     -> ./program
 *   cc0 --asm program.c -o program.s   -> emit assembly only, no linking
 *
 * Only the C standard library is used. gcc is invoked only as the
 * assembler/linker for the generated .s file.
 *
 * Source / inspiration: "I made a C compiler in C" by lolzdev
 * (https://www.youtube.com/watch?v=UW8LgC-S_co) and his longer
 * "Writing a C compiler from scratch" video. See README.md for exactly
 * what of the video this follows and what it does not.
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
    T_EOF, T_INT, T_CHAR, T_VOID, T_RETURN, T_IF, T_ELSE, T_WHILE, T_FOR,
    T_BREAK, T_CONTINUE,
    T_IDENT, T_NUM,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT,
    T_ASSIGN, T_PLUS_ASSIGN, T_MINUS_ASSIGN, T_STAR_ASSIGN,
    T_SLASH_ASSIGN, T_PERCENT_ASSIGN,
    T_EQ, T_NE, T_LT, T_LE, T_GT, T_GE,
    T_AND, T_OR, T_NOT,
    T_BITAND, T_BITOR, T_BITXOR, T_SHL, T_SHR, T_BITNOT,
    T_INC, T_DEC,
    T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_SEMI, T_COMMA
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
        if (!strcmp(g_tok.ident, "int"))      g_tok.kind = T_INT;
        else if (!strcmp(g_tok.ident, "char"))     g_tok.kind = T_CHAR;
        else if (!strcmp(g_tok.ident, "void"))     g_tok.kind = T_VOID;
        else if (!strcmp(g_tok.ident, "return"))   g_tok.kind = T_RETURN;
        else if (!strcmp(g_tok.ident, "if"))       g_tok.kind = T_IF;
        else if (!strcmp(g_tok.ident, "else"))     g_tok.kind = T_ELSE;
        else if (!strcmp(g_tok.ident, "while"))    g_tok.kind = T_WHILE;
        else if (!strcmp(g_tok.ident, "for"))      g_tok.kind = T_FOR;
        else if (!strcmp(g_tok.ident, "break"))    g_tok.kind = T_BREAK;
        else if (!strcmp(g_tok.ident, "continue")) g_tok.kind = T_CONTINUE;
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
    if (c == '\'') {
        /* Character literal: 'a', '\n', '\t', '\\', '\'', '\0', ... */
        g_pos++;
        char ch = g_src[g_pos];
        long v;
        if (ch == '\\') {
            g_pos++;
            char esc = g_src[g_pos];
            switch (esc) {
            case 'n': v = '\n'; break;
            case 't': v = '\t'; break;
            case 'r': v = '\r'; break;
            case '0': v = 0; break;
            case '\\': v = '\\'; break;
            case '\'': v = '\''; break;
            case '"': v = '"'; break;
            default: die("unknown escape sequence '\\%c'", esc);
            }
            g_pos++;
        } else {
            if (ch == '\n' || ch == '\0') die("unterminated character literal");
            v = (unsigned char)ch;
            g_pos++;
        }
        if (g_src[g_pos] != '\'') die("character literal must hold a single character");
        g_pos++;
        g_tok.kind = T_NUM; g_tok.num = v;
        return;
    }
    g_pos++;
    switch (c) {
        case '+':
            if (g_src[g_pos] == '+') { g_pos++; g_tok.kind = T_INC; return; }
            if (g_src[g_pos] == '=') { g_pos++; g_tok.kind = T_PLUS_ASSIGN; return; }
            g_tok.kind = T_PLUS; return;
        case '-':
            if (g_src[g_pos] == '-') { g_pos++; g_tok.kind = T_DEC; return; }
            if (g_src[g_pos] == '=') { g_pos++; g_tok.kind = T_MINUS_ASSIGN; return; }
            g_tok.kind = T_MINUS; return;
        case '*':
            if (g_src[g_pos] == '=') { g_pos++; g_tok.kind = T_STAR_ASSIGN; return; }
            g_tok.kind = T_STAR; return;
        case '/':
            if (g_src[g_pos] == '=') { g_pos++; g_tok.kind = T_SLASH_ASSIGN; return; }
            g_tok.kind = T_SLASH; return;
        case '%':
            if (g_src[g_pos] == '=') { g_pos++; g_tok.kind = T_PERCENT_ASSIGN; return; }
            g_tok.kind = T_PERCENT; return;
        case '(': g_tok.kind = T_LPAREN; return;
        case ')': g_tok.kind = T_RPAREN; return;
        case '{': g_tok.kind = T_LBRACE; return;
        case '}': g_tok.kind = T_RBRACE; return;
        case ';': g_tok.kind = T_SEMI; return;
        case ',': g_tok.kind = T_COMMA; return;
        case '~': g_tok.kind = T_BITNOT; return;
        case '^': g_tok.kind = T_BITXOR; return;
        case '=': g_tok.kind = (g_src[g_pos] == '=') ? (g_pos++, T_EQ) : T_ASSIGN; return;
        case '!': g_tok.kind = (g_src[g_pos] == '=') ? (g_pos++, T_NE) : T_NOT; return;
        case '<':
            if (g_src[g_pos] == '<') { g_pos++; g_tok.kind = T_SHL; return; }
            g_tok.kind = (g_src[g_pos] == '=') ? (g_pos++, T_LE) : T_LT; return;
        case '>':
            if (g_src[g_pos] == '>') { g_pos++; g_tok.kind = T_SHR; return; }
            g_tok.kind = (g_src[g_pos] == '=') ? (g_pos++, T_GE) : T_GT; return;
        case '&': if (g_src[g_pos] == '&') { g_pos++; g_tok.kind = T_AND; return; }
                  g_tok.kind = T_BITAND; return;
        case '|': if (g_src[g_pos] == '|') { g_pos++; g_tok.kind = T_OR; return; }
                  g_tok.kind = T_BITOR; return;
    }
    die("unexpected character '%c'", c);
}

/* One-token lookahead without consuming: save the lexer state, lex one
 * token, then restore. Used to tell `x = ...` from `x++` / `f(...)`. */
static Token peek_token(void) {
    size_t save_pos = g_pos;
    int save_line = g_line;
    Token save_tok = g_tok;
    next_token();
    Token t = g_tok;
    g_pos = save_pos; g_line = save_line; g_tok = save_tok;
    return t;
}

static bool accept(TokKind k) {
    if (g_tok.kind == k) { next_token(); return true; }
    return false;
}
static void expect(TokKind k, const char *what) {
    if (g_tok.kind != k) die("expected %s, found something else", what);
    next_token();
}
static bool is_assign_op(TokKind k) {
    return k == T_ASSIGN || k == T_PLUS_ASSIGN || k == T_MINUS_ASSIGN ||
           k == T_STAR_ASSIGN || k == T_SLASH_ASSIGN || k == T_PERCENT_ASSIGN;
}

/* ------------------------------------------------------------------ */
/* AST                                                                 */
/* ------------------------------------------------------------------ */
#define MAX_ARGS 6

typedef enum { E_NUM, E_VAR, E_BIN, E_UN, E_CALL } ExprKind;
typedef struct Expr Expr;
struct Expr {
    ExprKind kind;
    int line;            /* source line, for error messages */
    long num;            /* E_NUM */
    int var;             /* E_VAR: local slot, or global index */
    bool is_global;      /* E_VAR */
    TokKind op;          /* E_BIN / E_UN */
    bool postfix;        /* E_UN ++/--: true for x++, false for ++x */
    Expr *left, *right;  /* E_BIN: both; E_UN: left only */
    char fname[64];      /* E_CALL */
    Expr *args[MAX_ARGS];/* E_CALL */
    int nargs;           /* E_CALL */
};

typedef enum {
    S_DECL, S_ASSIGN, S_RETURN, S_PRINT, S_IF, S_WHILE, S_FOR,
    S_BREAK, S_CONTINUE, S_BLOCK, S_EXPR
} StmtKind;
typedef struct Stmt Stmt;
struct Stmt {
    StmtKind kind;
    int line;
    int var;             /* S_DECL / S_ASSIGN: local slot, or global index */
    bool is_global;      /* S_DECL (never true) / S_ASSIGN */
    TokKind assign_op;   /* S_ASSIGN: =, +=, -=, *=, /=, %= */
    Expr *expr;          /* S_DECL init (may be NULL), S_ASSIGN rhs,
                            S_RETURN, S_PRINT, S_EXPR */
    Expr *cond;          /* S_IF / S_WHILE / S_FOR (NULL = always true) */
    Stmt *init;          /* S_FOR only (may be NULL) */
    Stmt *incr;          /* S_FOR only (may be NULL) */
    Stmt *then_branch;  /* S_IF / S_WHILE / S_FOR */
    Stmt *else_branch;  /* S_IF only (a block, a nested if, or NULL) */
    Stmt *first;         /* S_BLOCK: first child */
    Stmt *next;          /* linked list inside a block */
};

static Expr *new_expr(ExprKind k) {
    Expr *e = calloc(1, sizeof *e);
    if (!e) die("out of memory");
    e->kind = k;
    e->line = g_line;
    return e;
}
static Stmt *new_stmt(StmtKind k) {
    Stmt *s = calloc(1, sizeof *s);
    if (!s) die("out of memory");
    s->kind = k;
    s->line = g_line;
    return s;
}

/* ------------------------------------------------------------------ */
/* Symbol tables                                                       */
/*                                                                     */
/* Locals: one flat scope per function (parameters first, then locals). */
/* Slots are 8 bytes each. Globals live in .data / .bss instead.       */
/* Functions are collected while parsing, so a call may refer to a     */
/* function defined later in the file; calls resolve by name at        */
/* code-generation time.                                               */
/* ------------------------------------------------------------------ */
#define MAX_VARS 256
static char g_vars[MAX_VARS][64];
static int g_var_scope[MAX_VARS]; /* lexical scope depth of each slot */
static int g_nvars;
static int g_scope;              /* current lexical scope depth */
static int g_maxvars;            /* high-water mark of g_nvars */
static int find_var(const char *name) {
    /* Search innermost scope first, so shadowing works. */
    for (int i = g_nvars - 1; i >= 0; i--)
        if (!strcmp(g_vars[i], name)) return i;
    return -1;
}
static int add_var(const char *name) {
    /* Redeclaration is only an error within the same scope. */
    for (int i = g_nvars - 1; i >= 0 && g_var_scope[i] == g_scope; i--)
        if (!strcmp(g_vars[i], name)) die("variable '%s' redeclared", name);
    if (g_nvars >= MAX_VARS) die("too many variables");
    snprintf(g_vars[g_nvars], sizeof g_vars[0], "%s", name);
    g_var_scope[g_nvars] = g_scope;
    int slot = g_nvars++;
    if (g_nvars > g_maxvars) g_maxvars = g_nvars;
    return slot;
}

#define MAX_GLOBALS 256
typedef struct { char name[64]; long init; bool has_init; } Global;
static Global g_globals[MAX_GLOBALS];
static int g_nglobals;
static int find_global(const char *name) {
    for (int i = 0; i < g_nglobals; i++)
        if (!strcmp(g_globals[i].name, name)) return i;
    return -1;
}

#define MAX_FUNCS 64
typedef struct {
    char name[64];
    int nparams;
    bool is_void;
    int nvars;           /* params + locals (frame size) */
    Stmt *body;
} Func;
static Func g_funcs[MAX_FUNCS];
static int g_nfuncs;
static Func *find_func(const char *name) {
    for (int i = 0; i < g_nfuncs; i++)
        if (!strcmp(g_funcs[i].name, name)) return &g_funcs[i];
    return NULL;
}

/* True while parsing a void function body (for `return;` checks). */
static bool g_cur_void;

/* ------------------------------------------------------------------ */
/* Parser (recursive descent, precedence climbing)                     */
/*                                                                     */
/*   expr    := or                                                      */
/*   or      := and ( "||" and )*                                      */
/*   and     := bitor ( "&&" bitor )*                                  */
/*   bitor   := bitxor ( "|" bitxor )*                                 */
/*   bitxor  := bitand ( "^" bitand )*                                 */
/*   bitand  := equality ( "&" equality )*                             */
/*   equality:= relational ( ("==" | "!=") relational )*               */
/*   relational := shift ( ("<" | "<=" | ">" | ">=") shift )*          */
/*   shift   := additive ( ("<<" | ">>") additive )*                   */
/*   additive:= multiplicative ( ("+" | "-") multiplicative )*         */
/*   multiplicative := unary ( ("*" | "/" | "%") unary )*              */
/*   unary   := ("-" | "!" | "~" | "++" | "--") unary | postfix        */
/*   postfix := primary ( ("++" | "--") )*                             */
/*   primary := NUMBER | IDENT | IDENT "(" args ")" | "(" expr ")"     */
/* ------------------------------------------------------------------ */
static Expr *parse_expr(void);

static Expr *parse_primary(void) {
    if (g_tok.kind == T_NUM) {
        Expr *e = new_expr(E_NUM); e->num = g_tok.num; next_token(); return e;
    }
    if (g_tok.kind == T_IDENT) {
        char name[64];
        snprintf(name, sizeof name, "%s", g_tok.ident);
        next_token();
        if (g_tok.kind == T_LPAREN) {
            /* Function call. `print` is a statement, not an expression. */
            if (!strcmp(name, "print"))
                die("print() is a statement, not an expression");
            next_token();
            Expr *e = new_expr(E_CALL);
            snprintf(e->fname, sizeof e->fname, "%s", name);
            if (g_tok.kind != T_RPAREN) {
                for (;;) {
                    if (e->nargs >= MAX_ARGS)
                        die("at most %d arguments are supported", MAX_ARGS);
                    e->args[e->nargs++] = parse_expr();
                    if (!accept(T_COMMA)) break;
                }
            }
            expect(T_RPAREN, "')'");
            return e;
        }
        int v = find_var(name);
        if (v >= 0) {
            Expr *e = new_expr(E_VAR); e->var = v; return e;
        }
        int gv = find_global(name);
        if (gv >= 0) {
            Expr *e = new_expr(E_VAR); e->var = gv; e->is_global = true; return e;
        }
        die("use of undeclared variable '%s'", name);
        return NULL;
    }
    if (accept(T_LPAREN)) {
        Expr *e = parse_expr();
        expect(T_RPAREN, "')'");
        return e;
    }
    die("expected an expression");
    return NULL;
}

static Expr *parse_postfix(void) {
    Expr *e = parse_primary();
    while (g_tok.kind == T_INC || g_tok.kind == T_DEC) {
        TokKind op = g_tok.kind; next_token();
        if (e->kind != E_VAR)
            die("'++' / '--' can only be applied to a variable");
        Expr *w = new_expr(E_UN); w->op = op; w->postfix = true; w->left = e;
        e = w;
    }
    return e;
}

static Expr *parse_unary(void) {
    if (g_tok.kind == T_MINUS || g_tok.kind == T_NOT || g_tok.kind == T_BITNOT) {
        TokKind op = g_tok.kind; next_token();
        Expr *e = new_expr(E_UN); e->op = op; e->left = parse_unary(); return e;
    }
    if (g_tok.kind == T_INC || g_tok.kind == T_DEC) {
        TokKind op = g_tok.kind; next_token();
        Expr *operand = parse_unary();
        if (operand->kind != E_VAR)
            die("'++' / '--' can only be applied to a variable");
        Expr *e = new_expr(E_UN); e->op = op; e->postfix = false; e->left = operand;
        return e;
    }
    return parse_postfix();
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
static Expr *parse_shift(void) {
    static const TokKind ops[] = { T_SHL, T_SHR };
    return parse_bin_level(parse_add, ops, 2);
}
static Expr *parse_rel(void) {
    static const TokKind ops[] = { T_LT, T_LE, T_GT, T_GE };
    return parse_bin_level(parse_shift, ops, 4);
}
static Expr *parse_eq(void) {
    static const TokKind ops[] = { T_EQ, T_NE };
    return parse_bin_level(parse_rel, ops, 2);
}
static Expr *parse_bitand(void) {
    static const TokKind ops[] = { T_BITAND };
    return parse_bin_level(parse_eq, ops, 1);
}
static Expr *parse_bitxor(void) {
    static const TokKind ops[] = { T_BITXOR };
    return parse_bin_level(parse_bitand, ops, 1);
}
static Expr *parse_bitor(void) {
    static const TokKind ops[] = { T_BITOR };
    return parse_bin_level(parse_bitxor, ops, 1);
}
static Expr *parse_and(void) {
    static const TokKind ops[] = { T_AND };
    return parse_bin_level(parse_bitor, ops, 1);
}
static Expr *parse_expr(void) {
    static const TokKind ops[] = { T_OR };
    return parse_bin_level(parse_and, ops, 1);
}

/* Evaluate a constant expression (for global initialisers). */
static bool eval_const(Expr *e, long *out) {
    if (!e) return false;
    switch (e->kind) {
    case E_NUM: *out = e->num; return true;
    case E_UN:
        if (e->op == T_MINUS) {
            long v; if (!eval_const(e->left, &v)) return false;
            *out = -v; return true;
        }
        return false;
    case E_BIN: {
        long a, b;
        if (!eval_const(e->left, &a) || !eval_const(e->right, &b)) return false;
        switch (e->op) {
        case T_PLUS: *out = a + b; return true;
        case T_MINUS: *out = a - b; return true;
        case T_STAR: *out = a * b; return true;
        case T_SLASH:
            if (b == 0) die("division by zero in constant expression");
            *out = a / b; return true;
        case T_PERCENT:
            if (b == 0) die("division by zero in constant expression");
            *out = a % b; return true;
        default: return false;
        }
    }
    default: return false;
    }
}

/* Statements. */
static Stmt *parse_block(void);  /* forward */
static Stmt *parse_stmt(void);   /* forward */

/* A declaration statement: `int x;` / `char x = <expr>;` (consumes ';'). */
static Stmt *parse_decl(void) {
    next_token(); /* consume 'int' / 'char' */
    if (g_tok.kind != T_IDENT) die("expected a variable name after the type");
    int v = add_var(g_tok.ident);
    next_token();
    Stmt *s = new_stmt(S_DECL); s->var = v;
    if (accept(T_ASSIGN)) s->expr = parse_expr();
    expect(T_SEMI, "';'");
    return s;
}

/* An assignment or expression statement, WITHOUT the trailing ';'.
 * Used for `for` increments and shared by parse_stmt. */
static Stmt *parse_assign_or_expr(void) {
    if (g_tok.kind == T_IDENT) {
        Token nx = peek_token();
        if (is_assign_op(nx.kind)) {
            char name[64];
            snprintf(name, sizeof name, "%s", g_tok.ident);
            next_token();
            int v = find_var(name);
            int gv = (v < 0) ? find_global(name) : -1;
            if (v < 0 && gv < 0)
                die("assignment to undeclared variable '%s'", name);
            TokKind op = g_tok.kind; next_token();
            Stmt *s = new_stmt(S_ASSIGN);
            s->var = (v >= 0) ? v : gv;
            s->is_global = (v < 0);
            s->assign_op = op;
            s->expr = parse_expr();
            return s;
        }
    }
    Stmt *s = new_stmt(S_EXPR);
    s->expr = parse_expr();
    return s;
}

/* A statement body: `{ ... }`, a single statement, or nothing.
 * A braced block is its own scope: variables declared inside it stop
 * existing (and their stack slots get reused) when it ends. */
static Stmt *parse_block_scoped(void) {
    int saved_nvars = g_nvars;
    int saved_scope = g_scope;
    g_scope++;
    Stmt *b = parse_block();
    g_nvars = saved_nvars;
    g_scope = saved_scope;
    return b;
}

static Stmt *parse_body(void) {
    if (g_tok.kind == T_LBRACE) {
        next_token();
        return parse_block_scoped();
    }
    return parse_stmt();
}

static Stmt *parse_stmt(void) {
    if (accept(T_SEMI)) return NULL; /* empty statement: `;` */
    if (g_tok.kind == T_INT || g_tok.kind == T_CHAR) return parse_decl();
    if (g_tok.kind == T_RETURN) {
        next_token();
        Stmt *s = new_stmt(S_RETURN);
        if (accept(T_SEMI)) {
            if (!g_cur_void)
                die("'return' with no value in a function returning int");
            return s;
        }
        if (g_cur_void)
            die("a void function must not return a value");
        s->expr = parse_expr();
        expect(T_SEMI, "';'");
        return s;
    }
    if (g_tok.kind == T_IF) {
        next_token();
        expect(T_LPAREN, "'(' after 'if'");
        Stmt *s = new_stmt(S_IF); s->cond = parse_expr();
        expect(T_RPAREN, "')'");
        s->then_branch = parse_body();
        if (g_tok.kind == T_ELSE) {
            next_token();
            if (g_tok.kind == T_IF) s->else_branch = parse_stmt(); /* else if */
            else s->else_branch = parse_body();
        }
        return s;
    }
    if (g_tok.kind == T_WHILE) {
        next_token();
        expect(T_LPAREN, "'(' after 'while'");
        Stmt *s = new_stmt(S_WHILE); s->cond = parse_expr();
        expect(T_RPAREN, "')'");
        s->then_branch = parse_body();
        return s;
    }
    if (g_tok.kind == T_FOR) {
        next_token();
        expect(T_LPAREN, "'(' after 'for'");
        Stmt *s = new_stmt(S_FOR);
        /* The whole for statement is one scope, so a variable declared
         * in the init lives until the end of the loop body. */
        int saved_nvars = g_nvars;
        int saved_scope = g_scope;
        g_scope++;
        /* init: declaration, assignment, expression, or empty */
        if (accept(T_SEMI)) {
            s->init = NULL;
        } else if (g_tok.kind == T_INT || g_tok.kind == T_CHAR) {
            s->init = parse_decl();
        } else {
            s->init = parse_assign_or_expr();
            expect(T_SEMI, "';'");
        }
        /* condition (may be empty = always true) */
        if (g_tok.kind != T_SEMI) s->cond = parse_expr();
        expect(T_SEMI, "';'");
        /* increment (may be empty) */
        if (g_tok.kind != T_RPAREN) s->incr = parse_assign_or_expr();
        expect(T_RPAREN, "')'");
        s->then_branch = parse_body();
        g_nvars = saved_nvars;
        g_scope = saved_scope;
        return s;
    }
    if (g_tok.kind == T_BREAK) {
        next_token();
        Stmt *s = new_stmt(S_BREAK);
        expect(T_SEMI, "';'");
        return s;
    }
    if (g_tok.kind == T_CONTINUE) {
        next_token();
        Stmt *s = new_stmt(S_CONTINUE);
        expect(T_SEMI, "';'");
        return s;
    }
    if (g_tok.kind == T_LBRACE) {
        next_token();
        return parse_block_scoped();
    }
    if (g_tok.kind == T_IDENT && !strcmp(g_tok.ident, "print") &&
        peek_token().kind == T_LPAREN) {
        next_token(); /* 'print' */
        next_token(); /* '(' */
        Stmt *s = new_stmt(S_PRINT); s->expr = parse_expr();
        expect(T_RPAREN, "')'");
        expect(T_SEMI, "';'");
        return s;
    }
    /* Assignment or expression statement. */
    Stmt *s = parse_assign_or_expr();
    expect(T_SEMI, "';'");
    return s;
}

static Stmt *parse_block(void) {
    Stmt *block = new_stmt(S_BLOCK);
    Stmt **tail = &block->first;
    while (g_tok.kind != T_RBRACE) {
        if (g_tok.kind == T_EOF) die("unexpected end of file — missing '}'");
        Stmt *st = parse_stmt();
        if (!st) continue; /* empty statement */
        *tail = st;
        tail = &st->next;
    }
    next_token(); /* consume '}' */
    return block;
}

/* Top level: a function definition or a global variable. */
static void parse_function(const char *name, bool is_void) {
    if (!strcmp(name, "print"))
        die("'print' is a builtin and cannot be redefined");
    if (find_func(name))
        die("function '%s' is already defined", name);
    if (find_global(name) >= 0)
        die("'%s' is already defined as a global variable", name);
    if (g_nfuncs >= MAX_FUNCS) die("too many functions");

    expect(T_LPAREN, "'(' after the function name");
    g_nvars = 0; /* fresh locals: parameters first, then locals */
    g_scope = 0;
    g_maxvars = 0;
    int nparams = 0;
    if (g_tok.kind == T_RPAREN) {
        /* f() — no parameters */
    } else if (g_tok.kind == T_IDENT && !strcmp(g_tok.ident, "void")) {
        next_token(); /* f(void) — no parameters */
    } else {
        for (;;) {
            if (g_tok.kind != T_INT && g_tok.kind != T_CHAR)
                die("expected a parameter type ('int' or 'char')");
            next_token();
            if (g_tok.kind != T_IDENT) die("expected a parameter name");
            add_var(g_tok.ident);
            nparams++;
            if (nparams > MAX_ARGS)
                die("at most %d parameters are supported", MAX_ARGS);
            next_token();
            if (!accept(T_COMMA)) break;
        }
    }
    expect(T_RPAREN, "')' after the parameter list");

    Func *f = &g_funcs[g_nfuncs++];
    snprintf(f->name, sizeof f->name, "%s", name);
    f->nparams = nparams;
    f->is_void = is_void;
    f->body = NULL;

    expect(T_LBRACE, "'{' before the function body");
    bool saved_void = g_cur_void;
    g_cur_void = is_void;
    f->body = parse_block();
    /* The frame must cover the high-water mark of the slots, not just
     * the variables live at the end (nested scopes reuse slots). */
    f->nvars = g_maxvars;
    g_cur_void = saved_void;
}

static void parse_global(const char *name) {
    if (find_global(name) >= 0)
        die("global variable '%s' is already defined", name);
    if (find_func(name))
        die("'%s' is already defined as a function", name);
    if (g_nglobals >= MAX_GLOBALS) die("too many global variables");
    Global *g = &g_globals[g_nglobals++];
    snprintf(g->name, sizeof g->name, "%s", name);
    g->has_init = false;
    g->init = 0;
    if (accept(T_ASSIGN)) {
        Expr *e = parse_expr();
        long v;
        if (!eval_const(e, &v))
            die("global initialiser must be a constant expression");
        g->has_init = true;
        g->init = v;
    }
    expect(T_SEMI, "';'");
}

static void parse_program(void) {
    while (g_tok.kind != T_EOF) {
        bool is_void = false;
        if (g_tok.kind == T_VOID) { is_void = true; next_token(); }
        else if (g_tok.kind == T_INT || g_tok.kind == T_CHAR) next_token();
        else die("expected a function definition or a global variable");
        if (g_tok.kind != T_IDENT) die("expected a name after the type");
        char name[64];
        snprintf(name, sizeof name, "%s", g_tok.ident);
        next_token();
        if (g_tok.kind == T_LPAREN) {
            parse_function(name, is_void);
        } else {
            if (is_void) die("'void' cannot be the type of a variable");
            parse_global(name);
        }
    }
    Func *main_fn = find_func("main");
    if (!main_fn)
        die("no 'int main()' function found");
    if (main_fn->is_void)
        die("'main' must return int");
    if (main_fn->nparams != 0)
        die("'main' must take no parameters");
}

/* ------------------------------------------------------------------ */
/* Code generation — x86-64, System V, AT&T syntax                    */
/*                                                                     */
/* Expression values are left in %rax. Binary operators evaluate the   */
/* left operand, push it, evaluate the right, then combine. Local       */
/* variables live in 8-byte slots at -8, -16, ... (%rbp); globals in   */
/* .data / .bss and are addressed RIP-relative. Function arguments      */
/* arrive in %rdi, %rsi, %rdx, %rcx, %r8, %r9 (up to 6).               */
/*                                                                     */
/* Stack alignment: the System V ABI requires %rsp to be 16-byte        */
/* aligned just before a `call`. After the prologue it is aligned when  */
/* no values are pushed, so g_depth (the number of pushes currently     */
/* outstanding) tells us the alignment: odd depth means misaligned,    */
/* and we emit a temporary `sub $8, %rsp` around the call.              */
/* ------------------------------------------------------------------ */
static FILE *g_out;
static int g_label;
static int g_depth;          /* pushes outstanding during codegen */
static int g_ret_label;      /* return label of the function being generated */

static int fresh_label(void) { return g_label++; }

/* Break / continue targets: one entry per enclosing loop. */
#define MAX_LOOP_DEPTH 32
static int g_loop_cont[MAX_LOOP_DEPTH]; /* where `continue` jumps */
static int g_loop_brk[MAX_LOOP_DEPTH];  /* where `break` jumps */
static int g_loop_depth;

static void emit_push_rax(void) { fprintf(g_out, "    push %%rax\n"); g_depth++; }
static void emit_pop_rax(void)  { fprintf(g_out, "    pop %%rax\n"); g_depth--; }
static void emit_pop_rcx(void)  { fprintf(g_out, "    pop %%rcx\n"); g_depth--; }

/* Call a function or PLT symbol with the stack correctly aligned. */
static void gen_call_insn(const char *target) {
    bool misaligned = (g_depth % 2) != 0;
    if (misaligned) fprintf(g_out, "    sub $8, %%rsp\n");
    fprintf(g_out, "    call %s\n", target);
    if (misaligned) fprintf(g_out, "    add $8, %%rsp\n");
}

static void gen_load_var(int var, bool is_global) {
    if (is_global)
        fprintf(g_out, "    mov %s(%%rip), %%rax\n", g_globals[var].name);
    else
        fprintf(g_out, "    mov %d(%%rbp), %%rax\n", -8 * (var + 1));
}
static void gen_store_var(int var, bool is_global) {
    if (is_global)
        fprintf(g_out, "    mov %%rax, %s(%%rip)\n", g_globals[var].name);
    else
        fprintf(g_out, "    mov %%rax, %d(%%rbp)\n", -8 * (var + 1));
}

static const char *ARG_REGS[MAX_ARGS] = { "%rdi", "%rsi", "%rdx", "%rcx", "%r8", "%r9" };

/* need_value: false only for a call used as a bare statement, where a
 * void result is fine. Everywhere else a void call is an error. */
static void gen_expr(Expr *e, bool need_value) {
    switch (e->kind) {
    case E_NUM:
        fprintf(g_out, "    mov $%ld, %%rax\n", e->num);
        return;
    case E_VAR:
        gen_load_var(e->var, e->is_global);
        return;
    case E_UN: {
        if (e->op == T_INC || e->op == T_DEC) {
            /* ++x / --x / x++ / x-- : operand is a variable (checked
             * while parsing). Prefix yields the new value, postfix the
             * old one. */
            const char *op = e->op == T_INC ? "add" : "sub";
            Expr *v = e->left;
            gen_load_var(v->var, v->is_global);
            if (e->postfix) {
                emit_push_rax();
                fprintf(g_out, "    %s $1, %%rax\n", op);
                gen_store_var(v->var, v->is_global);
                emit_pop_rax();
            } else {
                fprintf(g_out, "    %s $1, %%rax\n", op);
                gen_store_var(v->var, v->is_global);
            }
            return;
        }
        gen_expr(e->left, true);
        if (e->op == T_MINUS) fprintf(g_out, "    neg %%rax\n");
        else if (e->op == T_BITNOT) fprintf(g_out, "    not %%rax\n");
        else { /* ! */
            fprintf(g_out, "    test %%rax, %%rax\n");
            fprintf(g_out, "    sete %%al\n");
            fprintf(g_out, "    movzb %%al, %%rax\n");
        }
        return;
    }
    case E_BIN: {
        TokKind op = e->op;
        if (op == T_AND || op == T_OR) {
            /* Normalise each side to 0/1, then AND / OR. */
            gen_expr(e->left, true);
            fprintf(g_out, "    test %%rax, %%rax\n");
            fprintf(g_out, "    setne %%al\n");
            fprintf(g_out, "    movzb %%al, %%rax\n");
            emit_push_rax();
            gen_expr(e->right, true);
            fprintf(g_out, "    test %%rax, %%rax\n");
            fprintf(g_out, "    setne %%al\n");
            fprintf(g_out, "    movzb %%al, %%rax\n");
            emit_pop_rcx();
            fprintf(g_out, "    %s %%rcx, %%rax\n", op == T_AND ? "and" : "or");
            return;
        }
        gen_expr(e->left, true);
        emit_push_rax();
        gen_expr(e->right, true);
        fprintf(g_out, "    mov %%rax, %%rcx\n");
        emit_pop_rax();
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
        case T_SHL: fprintf(g_out, "    sal %%cl, %%rax\n"); break;
        case T_SHR: fprintf(g_out, "    sar %%cl, %%rax\n"); break;
        case T_BITAND: fprintf(g_out, "    and %%rcx, %%rax\n"); break;
        case T_BITOR:  fprintf(g_out, "    or %%rcx, %%rax\n"); break;
        case T_BITXOR: fprintf(g_out, "    xor %%rcx, %%rax\n"); break;
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
    case E_CALL: {
        Func *f = find_func(e->fname);
        if (!f) {
            g_line = e->line;
            die("call to undefined function '%s'", e->fname);
        }
        if (f->nparams != e->nargs) {
            g_line = e->line;
            die("function '%s' expects %d argument(s), got %d",
                e->fname, f->nparams, e->nargs);
        }
        if (need_value && f->is_void) {
            g_line = e->line;
            die("void function '%s' used as a value", e->fname);
        }
        /* Evaluate arguments left to right, push each, then pop them
         * into the argument registers in reverse order. */
        for (int i = 0; i < e->nargs; i++) {
            gen_expr(e->args[i], true);
            emit_push_rax();
        }
        for (int i = e->nargs - 1; i >= 0; i--) {
            fprintf(g_out, "    pop %s\n", ARG_REGS[i]);
            g_depth--;
        }
        gen_call_insn(f->name);
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
            if (s->expr) gen_expr(s->expr, true);
            else fprintf(g_out, "    mov $0, %%rax\n");
            gen_store_var(s->var, false);
            break;
        case S_ASSIGN: {
            TokKind op = s->assign_op;
            if (op == T_ASSIGN) {
                gen_expr(s->expr, true);
            } else {
                gen_load_var(s->var, s->is_global);
                emit_push_rax();
                gen_expr(s->expr, true);
                fprintf(g_out, "    mov %%rax, %%rcx\n");
                emit_pop_rax();
                switch (op) {
                case T_PLUS_ASSIGN:  fprintf(g_out, "    add %%rcx, %%rax\n"); break;
                case T_MINUS_ASSIGN: fprintf(g_out, "    sub %%rcx, %%rax\n"); break;
                case T_STAR_ASSIGN:  fprintf(g_out, "    imul %%rcx, %%rax\n"); break;
                case T_SLASH_ASSIGN:
                    fprintf(g_out, "    cqo\n");
                    fprintf(g_out, "    idiv %%rcx\n");
                    break;
                case T_PERCENT_ASSIGN:
                    fprintf(g_out, "    cqo\n");
                    fprintf(g_out, "    idiv %%rcx\n");
                    fprintf(g_out, "    mov %%rdx, %%rax\n");
                    break;
                default: die("internal error: unknown assignment operator");
                }
            }
            gen_store_var(s->var, s->is_global);
            break;
        }
        case S_RETURN:
            if (s->expr) gen_expr(s->expr, true);
            fprintf(g_out, "    jmp .L%d\n", g_ret_label);
            break;
        case S_PRINT:
            gen_expr(s->expr, true);
            fprintf(g_out, "    mov %%rax, %%rsi\n");
            fprintf(g_out, "    lea .Lfmt(%%rip), %%rdi\n");
            fprintf(g_out, "    xor %%eax, %%eax\n");
            gen_call_insn("printf@PLT");
            break;
        case S_IF: {
            int lelse = fresh_label(), lend = fresh_label();
            gen_expr(s->cond, true);
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
            gen_expr(s->cond, true);
            fprintf(g_out, "    test %%rax, %%rax\n");
            fprintf(g_out, "    je .L%d\n", lend);
            if (g_loop_depth >= MAX_LOOP_DEPTH) die("loops nested too deeply");
            g_loop_cont[g_loop_depth] = lstart;
            g_loop_brk[g_loop_depth] = lend;
            g_loop_depth++;
            gen_stmt(s->then_branch);
            g_loop_depth--;
            fprintf(g_out, "    jmp .L%d\n", lstart);
            fprintf(g_out, ".L%d:\n", lend);
            break;
        }
        case S_FOR: {
            int lcond = fresh_label(), lincr = fresh_label(), lend = fresh_label();
            if (s->init) gen_stmt(s->init);
            fprintf(g_out, ".L%d:\n", lcond);
            if (s->cond) {
                gen_expr(s->cond, true);
                fprintf(g_out, "    test %%rax, %%rax\n");
                fprintf(g_out, "    je .L%d\n", lend);
            }
            if (g_loop_depth >= MAX_LOOP_DEPTH) die("loops nested too deeply");
            g_loop_cont[g_loop_depth] = lincr; /* continue runs the increment */
            g_loop_brk[g_loop_depth] = lend;
            g_loop_depth++;
            gen_stmt(s->then_branch);
            g_loop_depth--;
            fprintf(g_out, ".L%d:\n", lincr);
            if (s->incr) gen_stmt(s->incr);
            fprintf(g_out, "    jmp .L%d\n", lcond);
            fprintf(g_out, ".L%d:\n", lend);
            break;
        }
        case S_BREAK:
            if (g_loop_depth == 0) {
                g_line = s->line;
                die("'break' outside a loop");
            }
            fprintf(g_out, "    jmp .L%d\n", g_loop_brk[g_loop_depth - 1]);
            break;
        case S_CONTINUE:
            if (g_loop_depth == 0) {
                g_line = s->line;
                die("'continue' outside a loop");
            }
            fprintf(g_out, "    jmp .L%d\n", g_loop_cont[g_loop_depth - 1]);
            break;
        case S_EXPR:
            gen_expr(s->expr, false);
            break;
        }
        s = s->next;
    }
}

static void gen_function(FILE *out, Func *f) {
    g_out = out;
    g_depth = 0;
    g_loop_depth = 0;
    /* Frame size: 8 bytes per variable (params + locals), rounded up to
     * a multiple of 16 so the stack stays 16-byte aligned at calls. */
    int frame = ((f->nvars * 8 + 15) / 16) * 16;
    fprintf(out, "    .text\n");
    fprintf(out, "    .globl %s\n", f->name);
    fprintf(out, "%s:\n", f->name);
    fprintf(out, "    push %%rbp\n");
    fprintf(out, "    mov %%rsp, %%rbp\n");
    if (frame) fprintf(out, "    sub $%d, %%rsp\n", frame);
    /* Copy incoming arguments from registers into their stack slots. */
    for (int i = 0; i < f->nparams; i++)
        fprintf(out, "    mov %s, %d(%%rbp)\n", ARG_REGS[i], -8 * (i + 1));
    g_ret_label = fresh_label();
    gen_stmt(f->body);
    /* Falling off the end of an int function returns 0, like C; a void
     * function just returns. */
    if (!f->is_void) fprintf(out, "    mov $0, %%rax\n");
    fprintf(out, ".L%d:\n", g_ret_label);
    fprintf(out, "    leave\n");
    fprintf(out, "    ret\n");
}

static void gen_program(FILE *out) {
    for (int i = 0; i < g_nfuncs; i++)
        gen_function(out, &g_funcs[i]);

    /* Globals: initialised ones go in .data, the rest in .bss. */
    bool any_data = false, any_bss = false;
    for (int i = 0; i < g_nglobals; i++) {
        if (g_globals[i].has_init) any_data = true;
        else any_bss = true;
    }
    if (any_data) {
        fprintf(out, "    .data\n");
        for (int i = 0; i < g_nglobals; i++)
            if (g_globals[i].has_init)
                fprintf(out, "%s:\n    .quad %ld\n", g_globals[i].name, g_globals[i].init);
    }
    if (any_bss) {
        fprintf(out, "    .bss\n");
        for (int i = 0; i < g_nglobals; i++)
            if (!g_globals[i].has_init)
                fprintf(out, "%s:\n    .zero 8\n", g_globals[i].name);
    }
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
    parse_program();

    if (asm_only) {
        FILE *out = fopen(output, "w");
        if (!out) { fprintf(stderr, "cc0: cannot write '%s'\n", output); return 1; }
        gen_program(out);
        fclose(out);
        return 0;
    }

    /* Generate assembly to a temp file, then assemble + link with gcc. */
    char asm_path[] = "/tmp/cc0-XXXXXX.s";
    int fd = mkstemps(asm_path, 2);
    if (fd < 0) die("cannot create temporary assembly file");
    FILE *out = fdopen(fd, "w");
    if (!out) die("cannot open temporary assembly file");
    gen_program(out);
    fclose(out);
    run_gcc(asm_path, output);
    unlink(asm_path);
    return 0;
}
