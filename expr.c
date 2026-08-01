/* expr.c -- lexer, recursive-descent parser, bytecode emitter, and VM.
 *
 * READ THIS FILE ON ITS OWN. It has no dependency on ALSA, ncurses, or any
 * other part of the project. Given a string it produces a Program; given a
 * Program and an ExprCtx it produces one int32 sample.
 *
 * The single most important idea here: *operator precedence is not a table
 * and not an algorithm*. It is the shape of the grammar. Each precedence
 * level is one function, and each function calls the next-tighter-binding
 * level for its operands. Because parse_or() can only ever get its operands
 * from parse_xor(), a '^' can never escape upward past a '|'. Precedence
 * falls out for free. See NOTES.md for the long version.
 */

#include "expr.h"

#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ======================================================================== */
/*  Lexer                                                                   */
/* ======================================================================== */

enum {
    TK_EOF = 0, TK_NUM,
    /* leaf identifiers */
    TK_T, TK_SR, TK_K, TK_N, TK_BT, TK_BL, TK_LL,
    TK_TR, TK_AGE, TK_VEL, TK_R, TK_P, TK_S,
    /* call-style identifiers */
    TK_D, TK_W, TK_LP, TK_HP, TK_BP,
    /* operators */
    TK_OR, TK_XOR, TK_AND, TK_LOR, TK_LAND,
    TK_SHL, TK_SHR,
    TK_PLUS, TK_MINUS, TK_STAR, TK_SLASH, TK_PCT,
    TK_TILDE, TK_BANG,
    TK_LT, TK_GT, TK_LE, TK_GE, TK_EQ, TK_NE,
    TK_ASSIGN, TK_QUEST, TK_COLON,
    TK_LPAR, TK_RPAR, TK_COMMA,
    TK_BAD
};

typedef struct {
    const char *src;
    int         pos;      /* read cursor into src                       */
    int         tk;       /* current (already-lexed) token              */
    int32_t     tval;     /* NUM value, or index for TK_P / TK_S        */
    int         tpos;     /* column where the current token starts      */
    Program    *out;
    int         depth;    /* current modelled operand-stack depth       */
    int         maxdepth;
    int         nfilt;    /* lp()/hp() state slots handed out so far    */
    uint32_t    used_p;   /* which p0..p7 the expression mentions       */
    int         uses_loop;/* mentions k / n / bt / bl / ll              */
    int         uses_rng; /* mentions r                                 */
    int         uses_trigger; /* mentions tr / age / vel                 */
    ExprError  *err;
} P;

static void fail(P *p, int col, const char *fmt, const char *arg)
{
    if (!p->err->ok) return;              /* keep the FIRST error, not the last */
    p->err->ok  = 0;
    p->err->col = col;
    if (arg) snprintf(p->err->msg, sizeof p->err->msg, fmt, arg);
    else     snprintf(p->err->msg, sizeof p->err->msg, "%s", fmt);
}

/* Identifier table. Order matters only in that we compare whole words, so
 * "sr" can never be mistaken for the register prefix "s". */
static int ident_token(const char *w, int len, int32_t *val)
{
    *val = 0;
    if (len == 1) {
        switch (w[0]) {
        case 't': return TK_T;
        case 'k': return TK_K;
        case 'n': return TK_N;
        case 'r': return TK_R;
        case 'd': return TK_D;
        case 'w': return TK_W;
        }
        return TK_BAD;
    }
    if (len == 2) {
        if (!memcmp(w, "sr", 2)) return TK_SR;
        if (!memcmp(w, "bt", 2)) return TK_BT;
        if (!memcmp(w, "bl", 2)) return TK_BL;
        if (!memcmp(w, "ll", 2)) return TK_LL;
        if (!memcmp(w, "tr", 2)) return TK_TR;
        if (!memcmp(w, "lp", 2)) return TK_LP;
        if (!memcmp(w, "hp", 2)) return TK_HP;
        if (!memcmp(w, "bp", 2)) return TK_BP;
        if (w[0] == 'p' && w[1] >= '0' && w[1] <= '7') {
            *val = w[1] - '0';
            return TK_P;
        }
        if (w[0] == 's' && w[1] >= '0' && w[1] <= '3') {
            *val = w[1] - '0';
            return TK_S;
        }
    }
    if (len == 3) {
        if (!memcmp(w, "age", 3)) return TK_AGE;
        if (!memcmp(w, "vel", 3)) return TK_VEL;
    }
    return TK_BAD;
}

static void next(P *p)
{
    const char *s = p->src;
    while (s[p->pos] == ' ' || s[p->pos] == '\t') p->pos++;
    p->tpos = p->pos;
    p->tval = 0;

    char c = s[p->pos];
    if (c == '\0') { p->tk = TK_EOF; return; }

    /* --- numbers: decimal or 0x hex. Accumulated in uint32 so that a
     *     literal like 0xffffffff wraps rather than being "too big". */
    if (isdigit((unsigned char)c)) {
        uint32_t v = 0;
        if (c == '0' && (s[p->pos + 1] == 'x' || s[p->pos + 1] == 'X')) {
            p->pos += 2;
            if (!isxdigit((unsigned char)s[p->pos])) {
                fail(p, p->tpos, "hex literal has no digits", NULL);
                p->tk = TK_BAD; return;
            }
            while (isxdigit((unsigned char)s[p->pos])) {
                char h = s[p->pos++];
                int  d = (h <= '9') ? h - '0' : (tolower((unsigned char)h) - 'a' + 10);
                v = v * 16u + (uint32_t)d;
            }
        } else {
            while (isdigit((unsigned char)s[p->pos]))
                v = v * 10u + (uint32_t)(s[p->pos++] - '0');
        }
        p->tval = (int32_t)v;
        p->tk   = TK_NUM;
        return;
    }

    /* --- identifiers */
    if (isalpha((unsigned char)c) || c == '_') {
        int start = p->pos;
        while (isalnum((unsigned char)s[p->pos]) || s[p->pos] == '_') p->pos++;
        int len = p->pos - start;
        int tk  = ident_token(s + start, len, &p->tval);
        if (tk == TK_BAD) {
            char buf[32];
            int  cp = len < 24 ? len : 24;
            memcpy(buf, s + start, (size_t)cp);
            buf[cp] = '\0';
            fail(p, start, "unknown name '%s'", buf);
        }
        p->tk = tk;
        return;
    }

    /* --- operators. Two-character forms must be tested before one-character
     *     ones, or "<<" lexes as "<" "<" and you get a comparison chain. */
    p->pos++;
    switch (c) {
    case '|': if (s[p->pos] == '|') { p->pos++; p->tk = TK_LOR;  } else p->tk = TK_OR;  return;
    case '&': if (s[p->pos] == '&') { p->pos++; p->tk = TK_LAND; } else p->tk = TK_AND; return;
    case '^': p->tk = TK_XOR;   return;
    case '+': p->tk = TK_PLUS;  return;
    case '-': p->tk = TK_MINUS; return;
    case '*': p->tk = TK_STAR;  return;
    case '/': p->tk = TK_SLASH; return;
    case '%': p->tk = TK_PCT;   return;
    case '~': p->tk = TK_TILDE; return;
    case '(': p->tk = TK_LPAR;  return;
    case ')': p->tk = TK_RPAR;  return;
    case ',': p->tk = TK_COMMA; return;
    case '?': p->tk = TK_QUEST; return;
    case ':': p->tk = TK_COLON; return;
    case '<':
        if (s[p->pos] == '<') { p->pos++; p->tk = TK_SHL; }
        else if (s[p->pos] == '=') { p->pos++; p->tk = TK_LE; }
        else p->tk = TK_LT;
        return;
    case '>':
        if (s[p->pos] == '>') { p->pos++; p->tk = TK_SHR; }
        else if (s[p->pos] == '=') { p->pos++; p->tk = TK_GE; }
        else p->tk = TK_GT;
        return;
    case '=':
        if (s[p->pos] == '=') { p->pos++; p->tk = TK_EQ; }
        else p->tk = TK_ASSIGN;
        return;
    case '!':
        if (s[p->pos] == '=') { p->pos++; p->tk = TK_NE; }
        else p->tk = TK_BANG;
        return;
    }
    fail(p, p->tpos, "stray character in expression", NULL);
    p->tk = TK_BAD;
}

/* ======================================================================== */
/*  Emitter                                                                 */
/* ======================================================================== */

/* `delta` is the net effect on the operand stack. Tracking it here, at emit
 * time, is what lets us guarantee at compile time that the VM can never
 * overflow or underflow its stack -- so expr_eval() has no bounds checks in
 * its inner loop. */
static int emit(P *p, int op, int32_t a, int delta)
{
    if (!p->err->ok) return -1;
    if (p->out->n >= EXPR_CODE_MAX) {
        fail(p, p->tpos, "expression too long", NULL);
        return -1;
    }
    p->depth += delta;
    if (p->depth > p->maxdepth) p->maxdepth = p->depth;
    if (p->maxdepth >= EXPR_STACK_MAX) {
        fail(p, p->tpos, "expression nests too deeply", NULL);
        return -1;
    }
    int at = p->out->n;
    p->out->code[at].op = op;
    p->out->code[at].a  = a;
    p->out->n = at + 1;
    return at;
}

static void patch(P *p, int at, int32_t target)
{
    if (at >= 0 && at < p->out->n) p->out->code[at].a = target;
}

static int accept(P *p, int tk)
{
    if (p->tk == tk) { next(p); return 1; }
    return 0;
}

static void expect(P *p, int tk, const char *what)
{
    if (!accept(p, tk)) fail(p, p->tpos, "expected %s", what);
}

/* ======================================================================== */
/*  Recursive-descent parser                                                */
/*                                                                          */
/*  Grammar, loosest binding first. This is literally C's precedence table   */
/*  rewritten as productions:                                               */
/*                                                                          */
/*    assign  := S '=' assign | ternary                                     */
/*    ternary := lor ('?' assign ':' assign)?                               */
/*    lor     := land ('||' land)*                                          */
/*    land    := bor  ('&&' bor )*                                          */
/*    bor     := bxor ('|'  bxor)*                                          */
/*    bxor    := band ('^'  band)*                                          */
/*    band    := equal('&'  equal)*                                         */
/*    equal   := rel  (('=='|'!=') rel)*                                    */
/*    rel     := shift(('<'|'>'|'<='|'>=') shift)*                          */
/*    shift   := add  (('<<'|'>>') add)*                                    */
/*    add     := mul  (('+'|'-') mul)*                                      */
/*    mul     := unary(('*'|'/'|'%') unary)*                                */
/*    unary   := ('-'|'~'|'!'|'+') unary | primary                          */
/*    primary := NUM | ident | call | '(' assign ')'                        */
/* ======================================================================== */

static void p_assign(P *p);

static void p_primary(P *p)
{
    switch (p->tk) {
    case TK_NUM: emit(p, OP_CONST, p->tval, +1); next(p); return;
    case TK_T:   emit(p, OP_T,  0, +1); next(p); return;
    case TK_SR:  emit(p, OP_SR, 0, +1); next(p); return;
    case TK_K:   p->uses_loop = 1; emit(p, OP_K,  0, +1); next(p); return;
    case TK_N:   p->uses_loop = 1; emit(p, OP_N,  0, +1); next(p); return;
    case TK_BT:  p->uses_loop = 1; emit(p, OP_BT, 0, +1); next(p); return;
    case TK_BL:  p->uses_loop = 1; emit(p, OP_BL, 0, +1); next(p); return;
    case TK_LL:  p->uses_loop = 1; emit(p, OP_LL, 0, +1); next(p); return;
    case TK_TR:  p->uses_trigger = 1; emit(p, OP_TR,  0, +1); next(p); return;
    case TK_AGE: p->uses_trigger = 1; emit(p, OP_AGE, 0, +1); next(p); return;
    case TK_VEL: p->uses_trigger = 1; emit(p, OP_VEL, 0, +1); next(p); return;
    case TK_R:   p->uses_rng  = 1; emit(p, OP_R,  0, +1); next(p); return;
    case TK_P:   p->used_p |= 1u << (unsigned)p->tval;
                 emit(p, OP_P,  p->tval, +1); next(p); return;

    case TK_S: {
        int32_t idx = p->tval;
        next(p);
        if (accept(p, TK_ASSIGN)) {           /* s0 = <expr> */
            p_assign(p);                      /* right-assoc: s0 = s1 = x works */
            emit(p, OP_SETREG, idx, 0);       /* consumes and re-pushes: net 0 */
        } else {
            emit(p, OP_REG, idx, +1);
        }
        return;
    }

    /* d          -- one sample back (the primitive for any recursive filter)
     * d(expr)    -- expr samples back */
    case TK_D:
        next(p);
        if (accept(p, TK_LPAR)) {
            p_assign(p);
            expect(p, TK_RPAR, "')' after d(");
        } else {
            emit(p, OP_CONST, 1, +1);
        }
        emit(p, OP_DLY, 0, 0);
        return;

    case TK_W:
        next(p);
        expect(p, TK_LPAR, "'(' after w");
        p_assign(p);
        expect(p, TK_RPAR, "')' after w(");
        emit(p, OP_WRT, 0, 0);
        return;

    case TK_LP:
    case TK_HP: {
        int op = (p->tk == TK_LP) ? OP_LP : OP_HP;
        int at = p->tpos;
        next(p);
        expect(p, TK_LPAR, "'(' after lp/hp");
        p_assign(p);
        expect(p, TK_COMMA, "',' -- lp/hp take (signal, cutoff)");
        p_assign(p);
        expect(p, TK_RPAR, "')' after lp/hp(");
        /* Each textual call site gets its own filter memory slot. Two lp()s
         * in one expression are two independent filters, which is what you
         * want; the same lp() evaluated on successive samples is one filter,
         * which is also what you want. */
        if (p->nfilt >= EXPR_NFILT) {
            fail(p, at, "too many lp()/hp() filters", NULL);
            return;
        }
        emit(p, op, p->nfilt++, -1);
        return;
    }

    case TK_BP: {
        int at = p->tpos;
        next(p);
        expect(p, TK_LPAR, "'(' after bp");
        p_assign(p);
        expect(p, TK_COMMA, "',' -- bp takes (signal, frequency, resonance)");
        p_assign(p);
        expect(p, TK_COMMA, "',' -- bp takes (signal, frequency, resonance)");
        p_assign(p);
        expect(p, TK_RPAR, "')' after bp(");
        if (p->nfilt >= EXPR_NFILT) {
            fail(p, at, "too many lp()/hp()/bp() filters", NULL);
            return;
        }
        emit(p, OP_BP, p->nfilt++, -2);
        return;
    }

    case TK_LPAR:
        next(p);
        p_assign(p);
        expect(p, TK_RPAR, "')'");
        return;

    case TK_EOF:
        fail(p, p->tpos, "expression ends early", NULL);
        return;

    default:
        if (p->err->ok) fail(p, p->tpos, "expected a value here", NULL);
        return;
    }
}

static void p_unary(P *p)
{
    if (accept(p, TK_MINUS)) { p_unary(p); emit(p, OP_NEG,  0, 0); return; }
    if (accept(p, TK_TILDE)) { p_unary(p); emit(p, OP_NOT,  0, 0); return; }
    if (accept(p, TK_BANG))  { p_unary(p); emit(p, OP_LNOT, 0, 0); return; }
    if (accept(p, TK_PLUS))  { p_unary(p); return; }   /* unary + is a no-op */
    p_primary(p);
}

/* Every binary level below is the same shape. Parse a tighter-binding
 * operand, then while the next token is one of ours, parse another tighter
 * operand and emit the op. Left-associativity is automatic because we emit
 * as we go. */
static void p_mul(P *p)
{
    p_unary(p);
    for (;;) {
        int op;
        if      (accept(p, TK_STAR))  op = OP_MUL;
        else if (accept(p, TK_SLASH)) op = OP_DIV;
        else if (accept(p, TK_PCT))   op = OP_MOD;
        else return;
        p_unary(p);
        emit(p, op, 0, -1);
    }
}

static void p_add(P *p)
{
    p_mul(p);
    for (;;) {
        int op;
        if      (accept(p, TK_PLUS))  op = OP_ADD;
        else if (accept(p, TK_MINUS)) op = OP_SUB;
        else return;
        p_mul(p);
        emit(p, op, 0, -1);
    }
}

static void p_shift(P *p)
{
    p_add(p);
    for (;;) {
        int op;
        if      (accept(p, TK_SHL)) op = OP_SHL;
        else if (accept(p, TK_SHR)) op = OP_SHR;
        else return;
        p_add(p);
        emit(p, op, 0, -1);
    }
}

static void p_rel(P *p)
{
    p_shift(p);
    for (;;) {
        int op;
        if      (accept(p, TK_LT)) op = OP_LT;
        else if (accept(p, TK_GT)) op = OP_GT;
        else if (accept(p, TK_LE)) op = OP_LE;
        else if (accept(p, TK_GE)) op = OP_GE;
        else return;
        p_shift(p);
        emit(p, op, 0, -1);
    }
}

static void p_equal(P *p)
{
    p_rel(p);
    for (;;) {
        int op;
        if      (accept(p, TK_EQ)) op = OP_EQ;
        else if (accept(p, TK_NE)) op = OP_NE;
        else return;
        p_rel(p);
        emit(p, op, 0, -1);
    }
}

static void p_band(P *p)
{
    p_equal(p);
    while (accept(p, TK_AND)) { p_equal(p); emit(p, OP_AND, 0, -1); }
}

static void p_bxor(P *p)
{
    p_band(p);
    while (accept(p, TK_XOR)) { p_band(p); emit(p, OP_XOR, 0, -1); }
}

static void p_bor(P *p)
{
    p_bxor(p);
    while (accept(p, TK_OR)) { p_bxor(p); emit(p, OP_OR, 0, -1); }
}

/* && and || short-circuit, which means they need real control flow. The
 * emitted shape for `a && b` is:
 *
 *      <a>
 *      JZ  false
 *      <b>
 *      JZ  false
 *      CONST 1
 *      JMP end
 *  false:
 *      CONST 0
 *  end:
 *
 * Note the manual depth bookkeeping around the branch: the two arms each
 * leave exactly one value, but a linear walk of the instruction stream would
 * double-count them, so we rewind `p->depth` before emitting the second arm.
 */
static void p_land(P *p)
{
    p_bor(p);
    while (p->tk == TK_LAND) {
        next(p);
        int j1 = emit(p, OP_JZ, 0, -1);
        p_bor(p);
        int j2 = emit(p, OP_JZ, 0, -1);
        int base = p->depth;
        emit(p, OP_CONST, 1, +1);
        int j3 = emit(p, OP_JMP, 0, 0);
        patch(p, j1, p->out->n);
        patch(p, j2, p->out->n);
        p->depth = base;
        emit(p, OP_CONST, 0, +1);
        patch(p, j3, p->out->n);
    }
}

static void p_lor(P *p)
{
    p_land(p);
    while (p->tk == TK_LOR) {
        next(p);
        int j1 = emit(p, OP_JNZ, 0, -1);
        p_land(p);
        int j2 = emit(p, OP_JNZ, 0, -1);
        int base = p->depth;
        emit(p, OP_CONST, 0, +1);
        int j3 = emit(p, OP_JMP, 0, 0);
        patch(p, j1, p->out->n);
        patch(p, j2, p->out->n);
        p->depth = base;
        emit(p, OP_CONST, 1, +1);
        patch(p, j3, p->out->n);
    }
}

static void p_ternary(P *p)
{
    p_lor(p);
    if (accept(p, TK_QUEST)) {
        int j1 = emit(p, OP_JZ, 0, -1);
        int base = p->depth;
        p_assign(p);
        int j2 = emit(p, OP_JMP, 0, 0);
        patch(p, j1, p->out->n);
        p->depth = base;                    /* else-arm starts from `base` */
        expect(p, TK_COLON, "':' in ?:");
        p_assign(p);
        patch(p, j2, p->out->n);
    }
}

static void p_assign(P *p)
{
    /* Assignment is handled inside p_primary for the `sN = ...` case, so at
     * this level we only need to hand off. Keeping the function makes the
     * grammar comment above honest and gives every '(' one obvious entry
     * point for a full expression. */
    p_ternary(p);
}

/* Work out what each knob is FOR by walking the finished bytecode.
 *
 * This is a miniature dataflow analysis. We re-walk the program with a stack
 * that holds provenance instead of values: each slot remembers "this came
 * from knob N" or "this came from somewhere else". When a binary op consumes
 * a slot that came from a knob, that op tells us the knob's role -- a knob
 * feeding >> is an octave control, one feeding lp() is a filter cutoff.
 *
 * The one refinement worth having: provenance propagates through +/- with a
 * constant, so the extremely common `t % (p0+1)` idiom still reports p0 as a
 * PERIOD control rather than as an anonymous offset. */
static int role_of_op(int op, int is_right)
{
    switch (op) {
    case OP_SHL: case OP_SHR: return is_right ? ROLE_SHIFT : ROLE_MISC;
    case OP_MUL:              return ROLE_MUL;
    case OP_AND: case OP_OR: case OP_XOR: return ROLE_MASK;
    case OP_MOD: case OP_DIV: return is_right ? ROLE_PERIOD : ROLE_MISC;
    case OP_ADD: case OP_SUB: return ROLE_LEVEL;
    case OP_LT:  case OP_GT: case OP_LE: case OP_GE:
    case OP_EQ:  case OP_NE:  return ROLE_LEVEL;
    default:                  return ROLE_MISC;
    }
}

static void infer_roles(Program *pr)
{
    signed char org[EXPR_STACK_MAX];   /* knob index, or -1 / -2 for const */
    int sp = 0;

    for (int i = 0; i < EXPR_NPARAM; i++) pr->role[i] = ROLE_NONE;

#define PUSH(v) do { if (sp < EXPR_STACK_MAX) org[sp++] = (signed char)(v); } while (0)
#define POP()   (sp > 0 ? org[--sp] : (signed char)-1)
#define MARK(k, r) do {         if ((k) >= 0 && pr->role[(int)(k)] == ROLE_NONE) pr->role[(int)(k)] = (unsigned char)(r);     } while (0)

    for (int i = 0; i < pr->n; i++) {
        int op = pr->code[i].op;
        switch (op) {
        case OP_CONST: PUSH(-2); break;                 /* -2 = literal      */
        case OP_P:     PUSH(pr->code[i].a); break;
        case OP_T: case OP_SR: case OP_K: case OP_N: case OP_BT:
        case OP_BL: case OP_LL: case OP_TR: case OP_AGE: case OP_VEL:
        case OP_R: case OP_REG:
            PUSH(-1); break;

        case OP_DLY: { signed char a = POP(); MARK(a, ROLE_DELAY); PUSH(-1); break; }

        case OP_LP: case OP_HP: {
            signed char c = POP(), x = POP();
            MARK(c, ROLE_CUT);
            (void)x;
            PUSH(-1);
            break;
        }

        case OP_BP: {
            signed char q = POP(), f = POP(), x = POP();
            MARK(f, ROLE_RESON);
            MARK(q, ROLE_Q);
            (void)x;
            PUSH(-1);
            break;
        }

        case OP_SETREG: case OP_WRT: case OP_NEG: case OP_NOT: case OP_LNOT:
            break;                                       /* unary: unchanged  */

        case OP_JZ: case OP_JNZ: POP(); break;
        case OP_JMP: break;

        default: {
            signed char b = POP(), a = POP();
            MARK(b, role_of_op(op, 1));
            MARK(a, role_of_op(op, 0));
            /* p0+1 is still "about p0" -- keep the provenance alive so the
             * enclosing operator gets to name the role. */
            if ((op == OP_ADD || op == OP_SUB) &&
                ((a >= 0 && b == -2) || (b >= 0 && a == -2))) {
                signed char keep = (a >= 0) ? a : b;
                if (pr->role[(int)keep] == ROLE_LEVEL) pr->role[(int)keep] = ROLE_NONE;
                PUSH(keep);
            } else {
                PUSH(-1);
            }
            break;
        }
        }
    }

    /* Anything mentioned but never classified is at least known to be used. */
    for (int i = 0; i < EXPR_NPARAM; i++)
        if ((pr->used_p >> i) & 1u) { if (pr->role[i] == ROLE_NONE) pr->role[i] = ROLE_MISC; }

#undef PUSH
#undef POP
#undef MARK
}

const char *expr_role_name(int role)
{
    switch (role) {
    case ROLE_SHIFT:  return "shift";
    case ROLE_MUL:    return "pitch";
    case ROLE_MASK:   return "timbre";
    case ROLE_CUT:    return "cutoff";
    case ROLE_PERIOD: return "period";
    case ROLE_DELAY:  return "delay";
    case ROLE_LEVEL:  return "amount";
    case ROLE_RESON:  return "resonator";
    case ROLE_Q:      return "resonance";
    case ROLE_MISC:   return "misc";
    default:          return "";
    }
}

int expr_compile(const char *src, Program *out, ExprError *err)
{
    P p;
    memset(&p, 0, sizeof p);
    p.src = src;
    p.out = out;
    p.err = err;

    err->ok  = 1;
    err->col = 0;
    err->msg[0] = '\0';

    out->n     = 0;
    out->depth = 0;
    out->nfilt = 0;
    out->used_p    = 0;
    out->uses_loop = 0;
    out->uses_rng  = 0;
    out->uses_trigger = 0;
    out->next  = NULL;
    out->retire_epoch = 0;

    /* An empty expression is silence, not an error -- you clear the line
     * mid-set and the instrument goes quiet instead of yelling at you. */
    next(&p);
    if (p.tk == TK_EOF) {
        emit(&p, OP_CONST, 0, +1);
        out->depth = 1;
        infer_roles(out);
        return 1;
    }

    p_assign(&p);

    if (err->ok && p.tk != TK_EOF)
        fail(&p, p.tpos, "trailing junk after expression", NULL);

    if (!err->ok) return 0;

    out->depth     = p.maxdepth;
    out->nfilt     = p.nfilt;
    out->used_p    = p.used_p;
    out->uses_loop = p.uses_loop;
    out->uses_rng  = p.uses_rng;
    out->uses_trigger = p.uses_trigger;
    infer_roles(out);
    return 1;
}

/* ======================================================================== */
/*  VM                                                                      */
/* ======================================================================== */

/* Signed overflow is undefined behaviour in C, and bytebeat overflows on
 * essentially every sample -- `t*t` wraps constantly and that wrapping IS
 * the instrument. So every arithmetic op is done in uint32_t, where wrapping
 * is defined, and reinterpreted as int32_t on the way out. (The Makefile also
 * passes -fwrapv as a belt-and-braces measure.) */
static inline uint32_t U(int32_t x) { return (uint32_t)x; }
static inline int32_t  S(uint32_t x) { return (int32_t)x; }

/* One-pole lowpass, integer, Q8 state.
 *
 *   state += (input - state) * cutoff
 *
 * is the whole filter. Done naively in int32 with a 0..255 coefficient the
 * update rounds to zero for small differences and the filter silently stalls,
 * which is why the state is kept 256x oversized (Q8) and the multiply is done
 * in int64. This is the single most important function for the sound: sweep
 * `c` down and any bright bitwise scream turns into subterranean rumble. */
static inline int32_t onepole(int64_t *st, int32_t x, int32_t c)
{
    if (c < 1)   c = 1;
    if (c > 255) c = 255;
    int64_t target = (int64_t)x << 8;
    *st += ((target - *st) * c) >> 8;
    return (int32_t)(*st >> 8);
}

/* Chamberlin state-variable resonator. `f` is deliberately a compact
 * coefficient rather than a pretend-Hz value: 1..128 spans sub-bass through
 * the upper mids and stays stable at audio rate. `q` runs in the intuitive
 * direction -- higher rings longer. State is Q8 so low-frequency impulses do
 * not vanish into integer rounding. Hard state bounds make hostile live-code
 * inputs saturate instead of overflowing the audio thread. */
static inline int32_t bandpass(int64_t *lo, int64_t *band,
                               int32_t x, int32_t f, int32_t q)
{
    if (f < 1) f = 1;
    if (f > 128) f = 128;
    if (q < 0) q = 0;
    if (q > 255) q = 255;

    int32_t damp = 260 - q;       /* 260..5; never truly lossless */
    int64_t in = (int64_t)x << 8;
    *lo += (*band * f) >> 8;
    int64_t high = in - *lo - ((*band * damp) >> 8);
    *band += (high * f) >> 8;

    const int64_t lim = (int64_t)INT32_MAX << 8;
    if (*lo > lim) *lo = lim;
    else if (*lo < -lim) *lo = -lim;
    if (*band > lim) *band = lim;
    else if (*band < -lim) *band = -lim;
    return (int32_t)(*band >> 8);
}

int32_t expr_eval(const Program *pr, ExprCtx *c)
{
    int32_t     st[EXPR_STACK_MAX];
    int         sp   = 0;
    int         pc   = 0;
    const Insn *code = pr->code;
    const int   n    = pr->n;

    /* No bounds checks on sp anywhere below: expr_compile() proved
     * pr->depth < EXPR_STACK_MAX before this program was ever published. */
    while (pc < n) {
        const Insn in = code[pc++];
        switch (in.op) {

        case OP_CONST: st[sp++] = in.a;         break;
        case OP_T:     st[sp++] = c->t;         break;
        case OP_SR:    st[sp++] = c->sr;        break;
        case OP_K:     st[sp++] = c->k;         break;
        case OP_N:     st[sp++] = c->n;         break;
        case OP_BT:    st[sp++] = c->bt;        break;
        case OP_BL:    st[sp++] = c->bl;        break;
        case OP_LL:    st[sp++] = c->ll;        break;
        case OP_TR:    st[sp++] = c->tr;        break;
        case OP_AGE:   st[sp++] = c->age;       break;
        case OP_VEL:   st[sp++] = c->vel;       break;
        case OP_P:     st[sp++] = c->p[in.a];   break;
        case OP_REG:   st[sp++] = c->reg[in.a]; break;

        case OP_SETREG:
            c->reg[in.a] = st[sp - 1];
            break;

        /* xorshift32. Three shifts and three xors, no multiply, no memory
         * traffic beyond one word of state. It advances on every read, so
         * `r^r` is noise rather than zero -- deliberate, see NOTES.md. */
        case OP_R: {
            uint32_t x = c->rng;
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            c->rng = x;
            st[sp++] = S(x);
            break;
        }

        /* Delay read. The tap is clamped rather than wrapped so that a knob
         * sweeping past the end of the buffer parks at maximum delay instead
         * of jumping back to zero. */
        case OP_DLY: {
            int32_t nn = st[sp - 1];
            if (nn < 1) nn = 1;
            if (nn > (int32_t)(EXPR_DELAY_LEN - 1)) nn = (int32_t)(EXPR_DELAY_LEN - 1);
            st[sp - 1] = c->dly[(c->dw - (uint32_t)nn) & EXPR_DELAY_MASK];
            break;
        }

        /* Delay write. Note that the cursor is NOT advanced here -- it moves
         * exactly once per sample, in the audio thread. So multiple w() calls
         * in one expression all target the same slot and the last one wins,
         * and d(1) always means "one sample ago" no matter how you wrote it. */
        case OP_WRT:
            c->dly[c->dw] = st[sp - 1];
            break;

        case OP_LP:
            sp--;
            st[sp - 1] = onepole(&c->filt[in.a], st[sp - 1], st[sp]);
            break;

        case OP_HP: {
            /* highpass = signal minus its own lowpass */
            sp--;
            int32_t x = st[sp - 1];
            st[sp - 1] = S(U(x) - U(onepole(&c->filt[in.a], x, st[sp])));
            break;
        }

        case OP_BP: {
            sp -= 2;
            st[sp - 1] = bandpass(&c->bp_lo[in.a], &c->bp_band[in.a],
                                  st[sp - 1], st[sp], st[sp + 1]);
            break;
        }

        case OP_OR:  sp--; st[sp - 1] = S(U(st[sp - 1]) |  U(st[sp])); break;
        case OP_XOR: sp--; st[sp - 1] = S(U(st[sp - 1]) ^  U(st[sp])); break;
        case OP_AND: sp--; st[sp - 1] = S(U(st[sp - 1]) &  U(st[sp])); break;
        case OP_ADD: sp--; st[sp - 1] = S(U(st[sp - 1]) +  U(st[sp])); break;
        case OP_SUB: sp--; st[sp - 1] = S(U(st[sp - 1]) -  U(st[sp])); break;
        case OP_MUL: sp--; st[sp - 1] = S(U(st[sp - 1]) *  U(st[sp])); break;

        case OP_EQ:  sp--; st[sp - 1] = (st[sp - 1] == st[sp]); break;
        case OP_NE:  sp--; st[sp - 1] = (st[sp - 1] != st[sp]); break;
        case OP_LT:  sp--; st[sp - 1] = (st[sp - 1] <  st[sp]); break;
        case OP_GT:  sp--; st[sp - 1] = (st[sp - 1] >  st[sp]); break;
        case OP_LE:  sp--; st[sp - 1] = (st[sp - 1] <= st[sp]); break;
        case OP_GE:  sp--; st[sp - 1] = (st[sp - 1] >= st[sp]); break;

        /* Shift counts are masked to 0..31. Shifting an int32 by 32 or more
         * is undefined in C, and knobs go to 255, so something has to give.
         * Masking is what JavaScript does, which matters because every
         * classic bytebeat formula on the internet was written against JS
         * semantics -- paste one in and it sounds the way its author heard
         * it. Left shift goes through uint32 to dodge overflow UB; right
         * shift stays signed so it is arithmetic, again matching JS. */
        case OP_SHL: sp--; st[sp - 1] = S(U(st[sp - 1]) << (U(st[sp]) & 31u)); break;
        case OP_SHR: sp--; st[sp - 1] =    st[sp - 1] >>  (int)(U(st[sp]) & 31u); break;

        /* Divide by zero yields 0 instead of raising SIGFPE. A live-coded
         * expression WILL divide by zero the moment a knob crosses a value,
         * and a hardware trap in the audio thread kills the instrument
         * mid-performance. INT32_MIN/-1 also traps on x86, so it is special
         * cased for the same reason. */
        case OP_DIV:
            sp--;
            if (st[sp] == 0)                                   st[sp - 1] = 0;
            else if (st[sp] == -1 && st[sp - 1] == INT32_MIN)   st[sp - 1] = INT32_MIN;
            else                                               st[sp - 1] /= st[sp];
            break;

        case OP_MOD:
            sp--;
            if (st[sp] == 0)                                   st[sp - 1] = 0;
            else if (st[sp] == -1 && st[sp - 1] == INT32_MIN)   st[sp - 1] = 0;
            else                                               st[sp - 1] %= st[sp];
            break;

        case OP_NEG:  st[sp - 1] = S(0u - U(st[sp - 1])); break;
        case OP_NOT:  st[sp - 1] = S(~U(st[sp - 1]));     break;
        case OP_LNOT: st[sp - 1] = !st[sp - 1];           break;

        case OP_JZ:  sp--; if (st[sp] == 0) pc = in.a; break;
        case OP_JNZ: sp--; if (st[sp] != 0) pc = in.a; break;
        case OP_JMP: pc = in.a; break;

        default: break;
        }
    }

    return sp > 0 ? st[sp - 1] : 0;
}

/* ======================================================================== */

static const char *const HELP[] = {
"  t    free-running sample counter      sr   sample rate",
"  k    loop position, 0..ll-1           ll   loop length in samples",
"  n    bar counter (monotonic)          bl   beat length in samples",
"  bt   position within current beat     r    xorshift noise (new value each read)",
"  tr   1 on the first sample of a hit    age  samples since that hit",
"  vel  hit velocity, normal/accent = 148/256",
"  p0..p7  knobs, 0..255                 s0..s3  registers, persist across samples",
"",
"  d          delay line, one sample ago   (the primitive for feedback)",
"  d(n)       delay line, n samples ago",
"  w(x)       write x into the delay line, returns x",
"  lp(x,c)    one-pole lowpass,  c=1..255  (low c = dark)",
"  hp(x,c)    one-pole highpass, c=1..255",
"  bp(x,f,q)  resonant bandpass, f=1..128, high q rings longer",
"  s0 = x     assign a register, returns x",
"",
"  ops, C precedence:   = ?: || && | ^ & == != < > <= >= << >> + - * / %  ~ ! -",
"  literals: 12345, 0xdead      div/mod by zero = 0      shifts masked to 0..31",
};

const char *const *expr_help_lines(int *count)
{
    *count = (int)(sizeof HELP / sizeof HELP[0]);
    return HELP;
}
