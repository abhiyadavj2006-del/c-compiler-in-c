/* cc0.js — a pure-JavaScript interpreter for the C subset that cc0
 * (the minimal C compiler in src/main.c) supports, plus a small
 * assembly-style listing generator in the same stack-machine style.
 *
 * Runs 100% statically: no backend, no CDN, no build step.
 * Works in a browser (window.CC0) and in Node (module.exports).
 *
 * Supported: int/char/void functions (recursion, up to 6 params),
 * globals, locals, block scoping, if/else if/else, while, for,
 * break/continue, return, print(x) builtin, printf(fmt, ...) builtin
 * with %d/%c/%s/%% and string literals (a web-interpreter extension —
 * the real compiler has no strings), + - * / %, << >>, & | ^, ~, !,
 * == != < <= > >=, && ||, ++/-- (prefix and postfix), += -= *= /= %=,
 * integer and character literals, // and block comments.
 *
 * Safety: every run is bounded by a step limit and a wall-clock
 * limit, so an infinite loop can never hang the page.
 */
(function (global) {
  'use strict';

  var MAX_STEPS_DEFAULT = 1000000;
  var MAX_MS_DEFAULT = 3000;
  var MAX_CALL_DEPTH = 500;

  function CC0Error(message, line) {
    var e = new Error(line ? 'line ' + line + ': ' + message : message);
    e.name = 'CC0Error';
    e.cc0Line = line || 0;
    return e;
  }
  function i32(v) { return v | 0; }

  /* ---------------------------------------------------------------- */
  /* Lexer                                                             */
  /* ---------------------------------------------------------------- */
  var KEYWORDS = {
    'int': 'INT', 'char': 'CHAR', 'void': 'VOID', 'return': 'RETURN',
    'if': 'IF', 'else': 'ELSE', 'while': 'WHILE', 'for': 'FOR',
    'break': 'BREAK', 'continue': 'CONTINUE'
  };
  var TWO_CHAR_OPS = ['++', '--', '+=', '-=', '*=', '/=', '%=', '==',
    '!=', '<=', '>=', '&&', '||', '<<', '>>'];
  var ONE_CHAR_OPS = '+-*/%=<>!&|^~(){};,';

  function escapeValue(ch) {
    switch (ch) {
      case 'n': return 10;
      case 't': return 9;
      case 'r': return 13;
      case '0': return 0;
      case '\\': return 92;
      case '\'': return 39;
      case '"': return 34;
      default: return -1;
    }
  }

  function tokenize(src) {
    var toks = [];
    var i = 0, line = 1;
    function err(msg) { throw CC0Error(msg, line); }
    while (i < src.length) {
      var c = src[i];
      if (c === '\n') { line++; i++; continue; }
      if (c === ' ' || c === '\t' || c === '\r' || c === '\f' || c === '\v') { i++; continue; }
      if (c === '/' && src[i + 1] === '/') {
        while (i < src.length && src[i] !== '\n') i++;
        continue;
      }
      if (c === '/' && src[i + 1] === '*') {
        i += 2;
        var closed = false;
        while (i < src.length) {
          if (src[i] === '\n') line++;
          if (src[i] === '*' && src[i + 1] === '/') { i += 2; closed = true; break; }
          i++;
        }
        if (!closed) err('unterminated block comment');
        continue;
      }
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c === '_') {
        var start = i;
        while (i < src.length &&
               ((src[i] >= 'a' && src[i] <= 'z') ||
                (src[i] >= 'A' && src[i] <= 'Z') ||
                (src[i] >= '0' && src[i] <= '9') || src[i] === '_')) i++;
        var word = src.slice(start, i);
        var kw = KEYWORDS[word];
        toks.push({ kind: kw || 'IDENT', value: word, line: line });
        continue;
      }
      if (c >= '0' && c <= '9') {
        var s2 = i;
        while (i < src.length && src[i] >= '0' && src[i] <= '9') i++;
        toks.push({ kind: 'NUM', value: parseInt(src.slice(s2, i), 10), line: line });
        continue;
      }
      if (c === '\'') {
        i++;
        if (i >= src.length) err('unterminated character literal');
        var v;
        if (src[i] === '\\') {
          i++;
          if (i >= src.length) err('unterminated character literal');
          v = escapeValue(src[i]);
          if (v < 0) err("unknown escape sequence '\\" + src[i] + "'");
          i++;
        } else {
          if (src[i] === '\n') err('unterminated character literal');
          v = src.charCodeAt(i);
          i++;
        }
        if (src[i] !== '\'') err('character literal must hold a single character');
        i++;
        toks.push({ kind: 'NUM', value: v, line: line });
        continue;
      }
      if (c === '"') {
        i++;
        var out = '';
        while (i < src.length && src[i] !== '"') {
          if (src[i] === '\n') err('unterminated string literal');
          if (src[i] === '\\') {
            i++;
            if (i >= src.length) err('unterminated string literal');
            var ev = escapeValue(src[i]);
            if (ev < 0) err("unknown escape sequence '\\" + src[i] + "'");
            out += String.fromCharCode(ev);
            i++;
          } else {
            out += src[i];
            i++;
          }
        }
        if (i >= src.length) err('unterminated string literal');
        i++;
        toks.push({ kind: 'STR', value: out, line: line });
        continue;
      }
      var two = src.substr(i, 2);
      if (TWO_CHAR_OPS.indexOf(two) >= 0) {
        toks.push({ kind: two, value: two, line: line });
        i += 2;
        continue;
      }
      if (ONE_CHAR_OPS.indexOf(c) >= 0) {
        toks.push({ kind: c, value: c, line: line });
        i++;
        continue;
      }
      err("unexpected character '" + c + "'");
    }
    toks.push({ kind: 'EOF', value: '', line: line });
    return toks;
  }

  /* ---------------------------------------------------------------- */
  /* Parser (recursive descent, precedence climbing — same order as    */
  /* the C compiler: || -> && -> | -> ^ -> & -> ==/!= -> rel ->       */
  /* shift -> add -> mul -> unary -> postfix -> primary)               */
  /* ---------------------------------------------------------------- */
  function Parser(toks) {
    this.toks = toks;
    this.pos = 0;
  }
  Parser.prototype.peek = function (k) {
    if (k === undefined) return this.toks[this.pos];
    return this.toks[this.pos + (k || 0)];
  };
  Parser.prototype.next = function () { return this.toks[this.pos++]; };
  Parser.prototype.err = function (msg, tok) {
    throw CC0Error(msg, (tok || this.peek()).line);
  };
  Parser.prototype.accept = function (kind) {
    if (this.peek().kind === kind) { this.pos++; return true; }
    return false;
  };
  Parser.prototype.expect = function (kind, what) {
    if (this.peek().kind !== kind) this.err('expected ' + (what || kind));
    return this.next();
  };

  Parser.prototype.parseProgram = function () {
    var prog = { globals: [], funcs: {} };
    while (this.peek().kind !== 'EOF') {
      var isVoid = false;
      if (this.peek().kind === 'VOID') { isVoid = true; this.next(); }
      else if (this.peek().kind === 'INT' || this.peek().kind === 'CHAR') this.next();
      else this.err('expected a function definition or a global variable');
      var nameTok = this.expect('IDENT', 'a name after the type');
      var name = nameTok.value;
      if (this.peek().kind === '(') {
        if (prog.funcs[name]) this.err("function '" + name + "' is already defined", nameTok);
        if (name === 'print' || name === 'printf') this.err("'" + name + "' is a builtin and cannot be redefined", nameTok);
        prog.funcs[name] = this.parseFunction(name, isVoid);
      } else {
        if (isVoid) this.err("'void' cannot be the type of a variable", nameTok);
        if (prog.globals.some(function (g) { return g.name === name; }))
          this.err("global variable '" + name + "' is already defined", nameTok);
        var g = { name: name, init: 0 };
        if (this.accept('=')) {
          var e = this.parseExpr();
          var v = evalConst(e);
          if (v === null) this.err('global initialiser must be a constant expression');
          g.init = v;
        }
        this.expect(';', "';'");
        prog.globals.push(g);
      }
    }
    var main = prog.funcs.main;
    if (!main) throw CC0Error("no 'int main()' function found");
    if (main.isVoid) throw CC0Error("'main' must return int");
    if (main.params.length !== 0) throw CC0Error("'main' must take no parameters");
    return prog;
  };

  Parser.prototype.parseFunction = function (name, isVoid) {
    this.expect('(', "'(' after the function name");
    var params = [];
    if (this.peek().kind === ')') {
      /* no parameters */
    } else if (this.peek().kind === 'IDENT' && this.peek().value === 'void') {
      this.next();
    } else {
      for (;;) {
        if (this.peek().kind !== 'INT' && this.peek().kind !== 'CHAR')
          this.err("expected a parameter type ('int' or 'char')");
        this.next();
        var p = this.expect('IDENT', 'a parameter name');
        params.push(p.value);
        if (params.length > 6) this.err('at most 6 parameters are supported');
        if (!this.accept(',')) break;
      }
    }
    this.expect(')', "')' after the parameter list");
    this.expect('{', "'{' before the function body");
    var body = this.parseBlock();
    return { name: name, isVoid: isVoid, params: params, body: body };
  };

  Parser.prototype.parseBlock = function () {
    var stmts = [];
    while (this.peek().kind !== '}') {
      if (this.peek().kind === 'EOF') this.err("unexpected end of file — missing '}'");
      var st = this.parseStmt();
      if (st) stmts.push(st);
    }
    this.next(); /* '}' */
    return { t: 'block', stmts: stmts };
  };

  Parser.prototype.parseBody = function () {
    if (this.peek().kind === '{') { this.next(); return this.parseBlock(); }
    return this.parseStmt();
  };

  Parser.prototype.parseStmt = function () {
    if (this.accept(';')) return null; /* empty statement */
    var k = this.peek().kind;
    if (k === 'INT' || k === 'CHAR') {
      this.next();
      var nameTok = this.expect('IDENT', 'a variable name after the type');
      var decl = { t: 'decl', name: nameTok.value, init: null, line: nameTok.line };
      if (this.accept('=')) decl.init = this.parseExpr();
      this.expect(';', "';'");
      return decl;
    }
    if (k === 'RETURN') {
      this.next();
      var ret = { t: 'return', expr: null };
      if (this.accept(';')) return ret;
      ret.expr = this.parseExpr();
      this.expect(';', "';'");
      return ret;
    }
    if (k === 'IF') {
      this.next();
      this.expect('(', "'(' after 'if'");
      var cond = this.parseExpr();
      this.expect(')', "')'");
      var thenB = this.parseBody();
      var elseB = null;
      if (this.peek().kind === 'ELSE') {
        this.next();
        if (this.peek().kind === 'IF') elseB = this.parseStmt();
        else elseB = this.parseBody();
      }
      return { t: 'if', cond: cond, then: thenB, els: elseB };
    }
    if (k === 'WHILE') {
      this.next();
      this.expect('(', "'(' after 'while'");
      var wc = this.parseExpr();
      this.expect(')', "')'");
      return { t: 'while', cond: wc, body: this.parseBody() };
    }
    if (k === 'FOR') {
      this.next();
      this.expect('(', "'(' after 'for'");
      var init = null;
      if (this.accept(';')) init = null;
      else if (this.peek().kind === 'INT' || this.peek().kind === 'CHAR') init = this.parseStmt();
      else { init = this.parseAssignOrExpr(); this.expect(';', "';'"); }
      var fc = null;
      if (this.peek().kind !== ';') fc = this.parseExpr();
      this.expect(';', "';'");
      var incr = null;
      if (this.peek().kind !== ')') incr = this.parseAssignOrExpr();
      this.expect(')', "')'");
      return { t: 'for', init: init, cond: fc, incr: incr, body: this.parseBody() };
    }
    if (k === 'BREAK') { this.next(); this.expect(';', "';'"); return { t: 'break' }; }
    if (k === 'CONTINUE') { this.next(); this.expect(';', "';'"); return { t: 'continue' }; }
    if (k === '{') { this.next(); return this.parseBlock(); }
    if (k === 'IDENT' && (this.peek().value === 'print' || this.peek().value === 'printf') &&
        this.peek(1).kind === '(') {
      var builtin = this.next().value;
      this.next(); /* '(' */
      if (builtin === 'print') {
        var e = this.parseExpr();
        this.expect(')', "')'");
        this.expect(';', "';'");
        return { t: 'print', expr: e };
      }
      var args = [];
      if (this.peek().kind !== ')') {
        for (;;) {
          args.push(this.parseExpr());
          if (!this.accept(',')) break;
        }
      }
      this.expect(')', "')'");
      this.expect(';', "';'");
      return { t: 'expr', expr: { t: 'call', name: 'printf', args: args } };
    }
    var s = this.parseAssignOrExpr();
    this.expect(';', "';'");
    return s;
  };

  var ASSIGN_OPS = ['=', '+=', '-=', '*=', '/=', '%='];
  Parser.prototype.parseAssignOrExpr = function () {
    if (this.peek().kind === 'IDENT' && ASSIGN_OPS.indexOf(this.peek(1).kind) >= 0) {
      var name = this.next().value;
      var op = this.next().kind;
      var rhs = this.parseExpr();
      return { t: 'assign', name: name, op: op, expr: rhs };
    }
    return { t: 'expr', expr: this.parseExpr() };
  };

  Parser.prototype.parseExpr = function () { return this.parseOr(); };
  Parser.prototype.parseBinLevel = function (nextFn, ops) {
    var left = nextFn.call(this);
    for (;;) {
      var op = this.peek().kind;
      if (ops.indexOf(op) < 0) return left;
      this.next();
      var right = nextFn.call(this);
      left = { t: 'bin', op: op, l: left, r: right };
    }
  };
  Parser.prototype.parseOr = function () { return this.parseBinLevel(this.parseAnd, ['||']); };
  Parser.prototype.parseAnd = function () { return this.parseBinLevel(this.parseBitor, ['&&']); };
  Parser.prototype.parseBitor = function () { return this.parseBinLevel(this.parseBitxor, ['|']); };
  Parser.prototype.parseBitxor = function () { return this.parseBinLevel(this.parseBitand, ['^']); };
  Parser.prototype.parseBitand = function () { return this.parseBinLevel(this.parseEq, ['&']); };
  Parser.prototype.parseEq = function () { return this.parseBinLevel(this.parseRel, ['==', '!=']); };
  Parser.prototype.parseRel = function () { return this.parseBinLevel(this.parseShift, ['<', '<=', '>', '>=']); };
  Parser.prototype.parseShift = function () { return this.parseBinLevel(this.parseAdd, ['<<', '>>']); };
  Parser.prototype.parseAdd = function () { return this.parseBinLevel(this.parseMul, ['+', '-']); };
  Parser.prototype.parseMul = function () { return this.parseBinLevel(this.parseUnary, ['*', '/', '%']); };

  Parser.prototype.parseUnary = function () {
    var k = this.peek().kind;
    if (k === '-' || k === '!' || k === '~') {
      this.next();
      return { t: 'un', op: k, e: this.parseUnary(), postfix: false };
    }
    if (k === '++' || k === '--') {
      this.next();
      var operand = this.parseUnary();
      if (operand.t !== 'var') this.err("'++' / '--' can only be applied to a variable");
      return { t: 'un', op: k, e: operand, postfix: false };
    }
    return this.parsePostfix();
  };

  Parser.prototype.parsePostfix = function () {
    var e = this.parsePrimary();
    while (this.peek().kind === '++' || this.peek().kind === '--') {
      var op = this.next().kind;
      if (e.t !== 'var') this.err("'++' / '--' can only be applied to a variable");
      e = { t: 'un', op: op, e: e, postfix: true };
    }
    return e;
  };

  Parser.prototype.parsePrimary = function () {
    var tok = this.peek();
    if (tok.kind === 'NUM') { this.next(); return { t: 'num', v: tok.value }; }
    if (tok.kind === 'STR') { this.next(); return { t: 'str', v: tok.value }; }
    if (tok.kind === 'IDENT') {
      this.next();
      if (this.peek().kind === '(') {
        if (tok.value === 'print') this.err('print() is a statement, not an expression', tok);
        this.next();
        var args = [];
        if (this.peek().kind !== ')') {
          for (;;) {
            if (args.length >= 6) this.err('at most 6 arguments are supported');
            args.push(this.parseExpr());
            if (!this.accept(',')) break;
          }
        }
        this.expect(')', "')'");
        return { t: 'call', name: tok.value, args: args };
      }
      return { t: 'var', name: tok.value };
    }
    if (this.accept('(')) {
      var e = this.parseExpr();
      this.expect(')', "')'");
      return e;
    }
    this.err('expected an expression', tok);
    return null;
  };

  /* Constant-expression evaluation (global initialisers only). */
  function evalConst(e) {
    if (!e) return null;
    if (e.t === 'num') return e.v;
    if (e.t === 'un' && e.op === '-') {
      var v = evalConst(e.e);
      return v === null ? null : -v;
    }
    if (e.t === 'bin') {
      var a = evalConst(e.l), b = evalConst(e.r);
      if (a === null || b === null) return null;
      switch (e.op) {
        case '+': return a + b;
        case '-': return a - b;
        case '*': return a * b;
        case '/': return b === 0 ? null : Math.trunc(a / b);
        case '%': return b === 0 ? null : a % b;
        default: return null;
      }
    }
    return null;
  }

  /* ---------------------------------------------------------------- */
  /* Interpreter                                                       */
  /* ---------------------------------------------------------------- */
  function Env(parent) {
    this.vars = Object.create(null);
    this.parent = parent || null;
  }
  Env.prototype.define = function (name, value) { this.vars[name] = value; };
  Env.prototype.lookup = function (name) {
    var e = this;
    while (e) {
      if (Object.prototype.hasOwnProperty.call(e.vars, name)) return e;
      e = e.parent;
    }
    return null;
  };
  Env.prototype.get = function (name) {
    var e = this.lookup(name);
    if (!e) throw CC0Error("use of undeclared variable '" + name + "'");
    return e.vars[name];
  };
  Env.prototype.set = function (name, value) {
    var e = this.lookup(name);
    if (!e) throw CC0Error("assignment to undeclared variable '" + name + "'");
    e.vars[name] = value;
  };

  function ReturnSignal(value) { this.value = value; }
  function BreakSignal() {}
  function ContinueSignal() {}

  function run(source, opts) {
    opts = opts || {};
    var maxSteps = opts.maxSteps || MAX_STEPS_DEFAULT;
    var maxMs = opts.maxMs || MAX_MS_DEFAULT;
    var prog = new Parser(tokenize(source)).parseProgram();

    var globals = new Env(null);
    prog.globals.forEach(function (g) { globals.define(g.name, i32(g.init)); });
    var output = '';
    var steps = 0;
    var start = (typeof performance !== 'undefined' && performance.now)
      ? performance.now() : Date.now();
    var now = function () {
      return (typeof performance !== 'undefined' && performance.now)
        ? performance.now() : Date.now();
    };
    function tick() {
      steps++;
      if (steps > maxSteps)
        throw CC0Error('step limit exceeded (' + maxSteps + ' steps) — possible infinite loop');
      if ((steps & 1023) === 0 && now() - start > maxMs)
        throw CC0Error('time limit exceeded (' + maxMs + ' ms) — possible infinite loop');
    }

    function printfFmt(fmt, args) {
      var out = '', ai = 0;
      for (var i = 0; i < fmt.length; i++) {
        if (fmt[i] !== '%') { out += fmt[i]; continue; }
        i++;
        if (i >= fmt.length) { out += '%'; break; }
        var spec = fmt[i];
        if (spec === '%') { out += '%'; continue; }
        if (spec === 'd' || spec === 'i') {
          var dv = args[ai++];
          out += String(i32(dv === undefined ? 0 : dv));
        } else if (spec === 'c') {
          var cv = args[ai++];
          out += String.fromCharCode(i32(cv === undefined ? 0 : cv) & 0xff);
        } else if (spec === 's') {
          var sv = args[ai++];
          out += sv === undefined ? '(null)' : String(sv);
        } else {
          out += '%' + spec;
        }
      }
      return out;
    }

    function callFunction(name, args, needValue, depth) {
      if (name === 'printf') {
        if (args.length === 0) throw CC0Error('printf() needs a format string');
        var fmt = args[0];
        if (typeof fmt !== 'string') throw CC0Error('printf() format must be a string literal');
        var text = printfFmt(fmt, args.slice(1));
        output += text;
        return text.length;
      }
      var fn = prog.funcs[name];
      if (!fn) throw CC0Error("call to undefined function '" + name + "'");
      if (fn.params.length !== args.length)
        throw CC0Error("function '" + name + "' expects " + fn.params.length +
          ' argument(s), got ' + args.length);
      if (needValue && fn.isVoid)
        throw CC0Error("void function '" + name + "' used as a value");
      if (depth > MAX_CALL_DEPTH) throw CC0Error('call depth exceeded — possible infinite recursion');
      var frame = new Env(globals);
      fn.params.forEach(function (p, i) { frame.define(p, i32(args[i])); });
      try {
        execStmt(fn.body, frame, depth);
      } catch (e) {
        if (e instanceof ReturnSignal) return e.value === undefined ? 0 : i32(e.value);
        throw e;
      }
      return 0;
    }

    function evalExpr(e, env, depth, needValue) {
      tick();
      switch (e.t) {
        case 'num': return i32(e.v);
        case 'str': return e.v;
        case 'var': return env.get(e.name);
        case 'un': {
          if (e.op === '++' || e.op === '--') {
            var delta = e.op === '++' ? 1 : -1;
            var old = i32(env.get(e.e.name));
            var nw = i32(old + delta);
            env.set(e.e.name, nw);
            return e.postfix ? old : nw;
          }
          var v = evalExpr(e.e, env, depth);
          if (e.op === '-') return i32(-i32(v));
          if (e.op === '~') return i32(~i32(v));
          return i32(v) === 0 ? 1 : 0; /* ! */
        }
        case 'bin': {
          if (e.op === '&&') {
            var lv = i32(evalExpr(e.l, env, depth));
            if (lv === 0) return 0;
            return i32(evalExpr(e.r, env, depth)) === 0 ? 0 : 1;
          }
          if (e.op === '||') {
            var lv2 = i32(evalExpr(e.l, env, depth));
            if (lv2 !== 0) return 1;
            return i32(evalExpr(e.r, env, depth)) === 0 ? 0 : 1;
          }
          var a = i32(evalExpr(e.l, env, depth));
          var b = i32(evalExpr(e.r, env, depth));
          switch (e.op) {
            case '+': return i32(a + b);
            case '-': return i32(a - b);
            case '*': return i32(a * b);
            case '/':
              if (b === 0) throw CC0Error('division by zero');
              return i32(Math.trunc(a / b));
            case '%':
              if (b === 0) throw CC0Error('division by zero');
              return i32(a - Math.trunc(a / b) * b);
            case '<<': return i32(a << (b & 31));
            case '>>': return i32(a >> (b & 31));
            case '&': return i32(a & b);
            case '|': return i32(a | b);
            case '^': return i32(a ^ b);
            case '==': return a === b ? 1 : 0;
            case '!=': return a !== b ? 1 : 0;
            case '<': return a < b ? 1 : 0;
            case '<=': return a <= b ? 1 : 0;
            case '>': return a > b ? 1 : 0;
            case '>=': return a >= b ? 1 : 0;
            default: throw CC0Error('unknown operator ' + e.op);
          }
        }
        case 'call': {
          var vals = e.args.map(function (arg) { return evalExpr(arg, env, depth); });
          return callFunction(e.name, vals, needValue !== false, depth + 1);
        }
        default: throw CC0Error('internal error: unknown expression');
      }
    }

    function execStmt(s, env, depth) {
      if (!s) return;
      tick();
      switch (s.t) {
        case 'block': {
          var benv = new Env(env);
          for (var i = 0; i < s.stmts.length; i++) execStmt(s.stmts[i], benv, depth);
          return;
        }
        case 'decl': {
          env.define(s.name, s.init ? i32(evalExpr(s.init, env, depth)) : 0);
          return;
        }
        case 'assign': {
          var rhs = i32(evalExpr(s.expr, env, depth));
          if (s.op === '=') { env.set(s.name, rhs); return; }
          var cur = i32(env.get(s.name));
          var res;
          if (s.op === '+=') res = i32(cur + rhs);
          else if (s.op === '-=') res = i32(cur - rhs);
          else if (s.op === '*=') res = i32(cur * rhs);
          else if (s.op === '/=') {
            if (rhs === 0) throw CC0Error('division by zero');
            res = i32(Math.trunc(cur / rhs));
          } else {
            if (rhs === 0) throw CC0Error('division by zero');
            res = i32(cur - Math.trunc(cur / rhs) * rhs);
          }
          env.set(s.name, res);
          return;
        }
        case 'return':
          throw new ReturnSignal(s.expr ? evalExpr(s.expr, env, depth) : undefined);
        case 'print': {
          var pv = evalExpr(s.expr, env, depth);
          output += (typeof pv === 'string' ? pv : String(i32(pv))) + '\n';
          return;
        }
        case 'expr': {
          evalExpr(s.expr, env, depth, false);
          return;
        }
        case 'if': {
          if (i32(evalExpr(s.cond, env, depth)) !== 0) execStmt(s.then, env, depth);
          else if (s.els) execStmt(s.els, env, depth);
          return;
        }
        case 'while': {
          while (i32(evalExpr(s.cond, env, depth)) !== 0) {
            try { execStmt(s.body, env, depth); }
            catch (e) {
              if (e instanceof BreakSignal) break;
              if (e instanceof ContinueSignal) continue;
              throw e;
            }
          }
          return;
        }
        case 'for': {
          var fenv = new Env(env);
          if (s.init) execStmt(s.init, fenv, depth);
          for (;;) {
            if (s.cond && i32(evalExpr(s.cond, fenv, depth)) === 0) break;
            try { execStmt(s.body, fenv, depth); }
            catch (e) {
              if (e instanceof BreakSignal) break;
              if (e instanceof ContinueSignal) { /* fall through to incr */ }
              else throw e;
            }
            if (s.incr) execStmt(s.incr, fenv, depth);
          }
          return;
        }
        case 'break': throw new BreakSignal();
        case 'continue': throw new ContinueSignal();
        default: throw CC0Error('internal error: unknown statement');
      }
    }

    var exitCode = 0;
    try {
      exitCode = callFunction('main', [], true, 0);
    } catch (e) {
      if (e instanceof ReturnSignal) exitCode = e.value === undefined ? 0 : i32(e.value);
      else throw e;
    }
    exitCode = ((i32(exitCode) % 256) + 256) % 256; /* exit codes are 8-bit, like C */
    return { output: output, exitCode: exitCode, steps: steps };
  }

  /* ---------------------------------------------------------------- */
  /* Assembly-style listing (illustrative, same stack-machine style    */
  /* as cc0's x86-64 code generator: values in %rax, push/pop).        */
  /* ---------------------------------------------------------------- */
  function asmListing(source) {
    var prog = new Parser(tokenize(source)).parseProgram();
    var out = [];
    var label = 0;
    function fresh() { return label++; }

    /* Collect local slot numbers per function (params first, then
     * declarations in source order). */
    function collectSlots(fn) {
      var slots = {};
      fn.params.forEach(function (p, i) { slots[p] = i; });
      var n = fn.params.length;
      (function walk(s) {
        if (!s) return;
        if (s.t === 'decl' && !(s.name in slots)) slots[s.name] = n++;
        if (s.t === 'block') s.stmts.forEach(walk);
        if (s.t === 'if') { walk(s.then); walk(s.els); }
        if (s.t === 'while' || s.t === 'for') { walk(s.init); walk(s.body); walk(s.incr); }
      })(fn.body);
      return { slots: slots, count: n };
    }

    function genExpr(e, fn, info) {
      if (e.t === 'num') { out.push('    mov $' + e.v + ', %rax'); return; }
      if (e.t === 'str') { out.push('    lea .Lstr, %rax   # string literal'); return; }
      if (e.t === 'var') {
        if (prog.globals.some(function (g) { return g.name === e.name; }))
          out.push('    mov ' + e.name + '(%rip), %rax');
        else if (e.name in info.slots)
          out.push('    mov ' + (-8 * (info.slots[e.name] + 1)) + '(%rbp), %rax');
        else out.push('    mov ' + e.name + ', %rax   # unresolved');
        return;
      }
      if (e.t === 'un') {
        if (e.op === '++' || e.op === '--') {
          var op = e.op === '++' ? 'add' : 'sub';
          genExpr(e.e, fn, info);
          if (e.postfix) {
            out.push('    push %rax');
            out.push('    ' + op + ' $1, %rax');
            storeVar(e.e.name, info);
            out.push('    pop %rax');
          } else {
            out.push('    ' + op + ' $1, %rax');
            storeVar(e.e.name, info);
          }
          return;
        }
        genExpr(e.e, fn, info);
        if (e.op === '-') out.push('    neg %rax');
        else if (e.op === '~') out.push('    not %rax');
        else {
          out.push('    test %rax, %rax');
          out.push('    sete %al');
          out.push('    movzb %al, %rax');
        }
        return;
      }
      if (e.t === 'call') {
        var regs = ['%rdi', '%rsi', '%rdx', '%rcx', '%r8', '%r9'];
        e.args.forEach(function (a) { genExpr(a, fn, info); out.push('    push %rax'); });
        for (var i = e.args.length - 1; i >= 0; i--) out.push('    pop ' + regs[i]);
        out.push('    call ' + e.name);
        return;
      }
      /* binary */
      genExpr(e.l, fn, info);
      out.push('    push %rax');
      genExpr(e.r, fn, info);
      out.push('    mov %rax, %rcx');
      out.push('    pop %rax');
      switch (e.op) {
        case '+': out.push('    add %rcx, %rax'); break;
        case '-': out.push('    sub %rcx, %rax'); break;
        case '*': out.push('    imul %rcx, %rax'); break;
        case '/': out.push('    cqo'); out.push('    idiv %rcx'); break;
        case '%': out.push('    cqo'); out.push('    idiv %rcx'); out.push('    mov %rdx, %rax'); break;
        case '<<': out.push('    sal %cl, %rax'); break;
        case '>>': out.push('    sar %cl, %rax'); break;
        case '&': out.push('    and %rcx, %rax'); break;
        case '|': out.push('    or %rcx, %rax'); break;
        case '^': out.push('    xor %rcx, %rax'); break;
        case '==': case '!=': case '<': case '<=': case '>': case '>=': {
          var cc = { '==': 'sete', '!=': 'setne', '<': 'setl', '<=': 'setle', '>': 'setg', '>=': 'setge' }[e.op];
          out.push('    cmp %rcx, %rax');
          out.push('    ' + cc + ' %al');
          out.push('    movzb %al, %rax');
          break;
        }
        case '&&': out.push('    and %rcx, %rax'); break;
        case '||': out.push('    or %rcx, %rax'); break;
        default: out.push('    # op ' + e.op);
      }
    }
    function storeVar(name, info) {
      if (prog.globals.some(function (g) { return g.name === name; }))
        out.push('    mov %rax, ' + name + '(%rip)');
      else if (name in info.slots)
        out.push('    mov %rax, ' + (-8 * (info.slots[name] + 1)) + '(%rbp)');
      else out.push('    mov %rax, ' + name);
    }
    function genStmt(s, fn, info, retLabel, loopStack) {
      if (!s) return;
      switch (s.t) {
        case 'block': s.stmts.forEach(function (c) { genStmt(c, fn, info, retLabel, loopStack); }); break;
        case 'decl':
          if (s.init) genExpr(s.init, fn, info); else out.push('    mov $0, %rax');
          storeVar(s.name, info);
          break;
        case 'assign': {
          if (s.op === '=') { genExpr(s.expr, fn, info); }
          else {
            genExpr({ t: 'var', name: s.name }, fn, info);
            out.push('    push %rax');
            genExpr(s.expr, fn, info);
            out.push('    mov %rax, %rcx');
            out.push('    pop %rax');
            var m = { '+=': 'add', '-=': 'sub', '*=': 'imul' }[s.op];
            if (m) out.push('    ' + m + ' %rcx, %rax');
            else if (s.op === '/=') { out.push('    cqo'); out.push('    idiv %rcx'); }
            else { out.push('    cqo'); out.push('    idiv %rcx'); out.push('    mov %rdx, %rax'); }
          }
          storeVar(s.name, info);
          break;
        }
        case 'return':
          if (s.expr) genExpr(s.expr, fn, info);
          out.push('    jmp .L' + retLabel);
          break;
        case 'print':
          genExpr(s.expr, fn, info);
          out.push('    mov %rax, %rsi');
          out.push('    lea .Lfmt(%rip), %rdi');
          out.push('    xor %eax, %eax');
          out.push('    call printf@PLT');
          break;
        case 'expr': genExpr(s.expr, fn, info); break;
        case 'if': {
          var lelse = fresh(), lend = fresh();
          genExpr(s.cond, fn, info);
          out.push('    test %rax, %rax');
          out.push('    je .L' + (s.els ? lelse : lend));
          genStmt(s.then, fn, info, retLabel, loopStack);
          if (s.els) {
            out.push('    jmp .L' + lend);
            out.push('.L' + lelse + ':');
            genStmt(s.els, fn, info, retLabel, loopStack);
          }
          out.push('.L' + lend + ':');
          break;
        }
        case 'while': {
          var ls = fresh(), le = fresh();
          out.push('.L' + ls + ':');
          genExpr(s.cond, fn, info);
          out.push('    test %rax, %rax');
          out.push('    je .L' + le);
          loopStack.push({ cont: ls, brk: le });
          genStmt(s.body, fn, info, retLabel, loopStack);
          loopStack.pop();
          out.push('    jmp .L' + ls);
          out.push('.L' + le + ':');
          break;
        }
        case 'for': {
          var lc = fresh(), li = fresh(), lend2 = fresh();
          if (s.init) genStmt(s.init, fn, info, retLabel, loopStack);
          out.push('.L' + lc + ':');
          if (s.cond) {
            genExpr(s.cond, fn, info);
            out.push('    test %rax, %rax');
            out.push('    je .L' + lend2);
          }
          loopStack.push({ cont: li, brk: lend2 });
          genStmt(s.body, fn, info, retLabel, loopStack);
          loopStack.pop();
          out.push('.L' + li + ':');
          if (s.incr) genStmt(s.incr, fn, info, retLabel, loopStack);
          out.push('    jmp .L' + lc);
          out.push('.L' + lend2 + ':');
          break;
        }
        case 'break':
          if (loopStack.length) out.push('    jmp .L' + loopStack[loopStack.length - 1].brk);
          break;
        case 'continue':
          if (loopStack.length) out.push('    jmp .L' + loopStack[loopStack.length - 1].cont);
          break;
      }
    }

    var regs = ['%rdi', '%rsi', '%rdx', '%rcx', '%r8', '%r9'];
    Object.keys(prog.funcs).forEach(function (fname) {
      var fn = prog.funcs[fname];
      var info = collectSlots(fn);
      var frame = Math.ceil(info.count * 8 / 16) * 16;
      var retLabel = fresh();
      out.push('    .text');
      out.push('    .globl ' + fname);
      out.push(fname + ':');
      out.push('    push %rbp');
      out.push('    mov %rsp, %rbp');
      if (frame) out.push('    sub $' + frame + ', %rsp');
      fn.params.forEach(function (p, i) {
        out.push('    mov ' + regs[i] + ', ' + (-8 * (i + 1)) + '(%rbp)');
      });
      genStmt(fn.body, fn, info, retLabel, []);
      if (!fn.isVoid) out.push('    mov $0, %rax');
      out.push('.L' + retLabel + ':');
      out.push('    leave');
      out.push('    ret');
      out.push('');
    });
    var anyData = prog.globals.some(function (g) { return g.init !== 0; });
    var anyBss = prog.globals.some(function (g) { return g.init === 0; });
    if (anyData) {
      out.push('    .data');
      prog.globals.forEach(function (g) {
        if (g.init !== 0) out.push(g.name + ':\n    .quad ' + g.init);
      });
    }
    if (anyBss) {
      out.push('    .bss');
      prog.globals.forEach(function (g) {
        if (g.init === 0) out.push(g.name + ':\n    .zero 8');
      });
    }
    out.push('    .section .rodata');
    out.push('.Lfmt:');
    out.push('    .string "%d\\n"');
    out.push('    .section .note.GNU-stack,"",@progbits');
    return out.join('\n');
  }

  /* ---------------------------------------------------------------- */
  /* The 9 test programs, as one-click examples (same sources as       */
  /* tests/test1..test9 in the repo).                                  */
  /* ---------------------------------------------------------------- */
  var EXAMPLES = [
    { id: 'test1', title: '1 — return a constant', src: 'int main() {\n    return 42;\n}\n' },
    { id: 'test2', title: '2 — arithmetic & precedence', src: 'int main() {\n    // Precedence: * binds tighter than +, parentheses override both.\n    return 2 + 3 * 4;\n}\n' },
    { id: 'test3', title: '3 — variables', src: 'int main() {\n    int a = 10;\n    int b = 20;\n    int c = a + b * 2;\n    return c;\n}\n' },
    { id: 'test4', title: '4 — print & while loop', src: 'int main() {\n    int i = 1;\n    int total = 0;\n    while (i <= 5) {\n        total = total + i;\n        i = i + 1;\n    }\n    print(total);\n\n    int k = 1;\n    int total2 = 0;\n    while (k <= 10) {\n        total2 = total2 + k;\n        k = k + 1;\n    }\n    print(total2);\n    return 0;\n}\n' },
    { id: 'test5', title: '5 — div/mod, compare, if/else', src: 'int main() {\n    print(17 / 5);\n    print(17 % 5);\n    print(3 < 5);\n    if (10 > 5) {\n        print(1);\n    } else {\n        print(0);\n    }\n    return 3 < 5;\n}\n' },
    { id: 'test6', title: '6 — functions & recursion (factorial)', src: '/* Functions with parameters, nested calls, and recursion. */\nint add(int a, int b) {\n    return a + b;\n}\n\nint mul(int a, int b) {\n    return a * b;\n}\n\nint fact(int n) {\n    if (n <= 1) {\n        return 1;\n    }\n    return n * fact(n - 1);\n}\n\nint main() {\n    print(add(3, 4));\n    print(mul(6, 7));\n    print(fact(5));\n    print(add(mul(2, 3), 4));\n    return add(1, 2);\n}\n' },
    { id: 'test7', title: '7 — for, break/continue, else-if', src: '/* for loops, break / continue, and else-if chains. */\nint main() {\n    int total = 0;\n    for (int i = 1; i <= 10; i++) {\n        total += i;\n    }\n    print(total);\n\n    int count = 0;\n    for (int i = 1; i <= 100; i++) {\n        if (i % 2 == 0) {\n            continue;\n        }\n        count++;\n        if (count == 3) {\n            break;\n        }\n    }\n    print(count);\n\n    int x = 75;\n    if (x >= 90) {\n        print(1);\n    } else if (x >= 60) {\n        print(2);\n    } else {\n        print(3);\n    }\n    return 0;\n}\n' },
    { id: 'test8', title: '8 — char, ++/--, bitwise, globals', src: '/* char type + character literals, ++/--, compound assignment,\n   bitwise operators, and global variables. */\nint g = 10;\nint h;\n\nint bump(int x) {\n    return x + 1;\n}\n\nint main() {\n    char c = \'A\';\n    print(c);\n    print(\'a\' - \'A\');\n    print(\'\\n\');\n\n    int x = 5;\n    x++;\n    print(x);\n    ++x;\n    print(x);\n    x--;\n    print(x);\n    x *= 3;\n    print(x);\n    x -= 4;\n    print(x);\n\n    print(12 & 10);\n    print(12 | 10);\n    print(12 ^ 10);\n    print(1 << 4);\n    print(256 >> 4);\n    print(~0);\n\n    g = g + 5;\n    print(g);\n    print(h);\n    h = 41;\n    print(bump(h));\n    return 0;\n}\n' },
    { id: 'test9', title: '9 — void functions, globals, fibonacci', src: '/* void functions, globals shared between functions, and recursion\n   (fibonacci) with calls inside expressions. */\nint counter;\n\nvoid greet(int n) {\n    print(n);\n    counter = counter + 1;\n}\n\nint fib(int n) {\n    if (n < 2) {\n        return n;\n    }\n    return fib(n - 1) + fib(n - 2);\n}\n\nint main() {\n    greet(7);\n    greet(8);\n    print(counter);\n    print(fib(10));\n\n    int i = 0;\n    while (i < 3) {\n        greet(100 + i);\n        i++;\n    }\n    print(counter);\n    return fib(6);\n}\n' },
    { id: 'printf', title: 'Extra — printf with %d / %c / %s', src: 'int main() {\n    printf("Hello, %s!\\n", "world");\n    printf("%d + %d = %d\\n", 40, 2, 40 + 2);\n    printf("char: %c%c%c\\n", 65, 66, 67);\n    printf("100%% done\\n");\n    return 0;\n}\n' }
  ];

  var CC0 = {
    run: run,
    asmListing: asmListing,
    EXAMPLES: EXAMPLES,
    MAX_STEPS_DEFAULT: MAX_STEPS_DEFAULT,
    MAX_MS_DEFAULT: MAX_MS_DEFAULT
  };

  if (typeof module !== 'undefined' && module.exports) module.exports = CC0;
  global.CC0 = CC0;
})(typeof window !== 'undefined' ? window : globalThis);
