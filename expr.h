/* expr.h -- the expression language: parser, bytecode, and VM.
 *
 * This header is deliberately the *entire* public surface of the language.
 * If you want to understand how a typed expression becomes sound, read
 * expr.c top to bottom; nothing else in the project is needed.
 */
#ifndef EXPR_H
#define EXPR_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stddef.h>

/* Hard ceilings. Everything is fixed size on purpose: a compiled Program is
 * one flat allocation with no internal pointers, so the audio thread can use
 * it without chasing anything and the UI thread can free it with one free(). */
#define EXPR_CODE_MAX    768   /* instructions in one program        */
#define EXPR_STACK_MAX   64    /* VM operand stack depth             */
#define EXPR_NPARAM      8     /* p0..p7                             */
#define EXPR_NREG        4     /* s0..s3                             */
#define EXPR_NFILT       24    /* distinct lp()/hp() call sites      */

/* The expression-level delay line, ONE PER LAYER. 2^17 samples is 3.0s at
 * 44.1kHz -- long enough for the slow feedback smears that make a drone sit
 * still instead of buzzing. Eight of them is 4MB, which is what independent
 * layers cost. Allocated ONCE at startup and never resized, because the
 * audio thread reads and writes it every single sample and must never touch
 * the allocator. */
#define EXPR_DELAY_BITS  17
#define EXPR_DELAY_LEN   (1u << EXPR_DELAY_BITS)
#define EXPR_DELAY_MASK  (EXPR_DELAY_LEN - 1u)

/* ---- opcodes -------------------------------------------------------------
 * The VM is a stack machine. Each opcode's comment shows its stack effect as
 * ( before -- after ). Keeping the effect explicit is what lets the compiler
 * compute the maximum stack depth statically, which is why the VM never has
 * to bounds-check the stack at audio rate. */
enum {
    OP_CONST,   /* (        -- v )  push imm                                */
    OP_T,       /* (        -- t )  free-running sample counter             */
    OP_SR,      /* (        -- v )  current sample rate                     */
    OP_K,       /* (        -- v )  loop position, 0..loop_len-1            */
    OP_N,       /* (        -- v )  monotonic bar counter                   */
    OP_BT,      /* (        -- v )  position within current beat            */
    OP_BL,      /* (        -- v )  beat length in samples                  */
    OP_LL,      /* (        -- v )  loop length in samples                  */
    OP_TR,      /* (        -- v )  1 on the first sample of a hit          */
    OP_AGE,     /* (        -- v )  samples since the latest hit            */
    OP_VEL,     /* (        -- v )  hit velocity, 0..256                    */
    OP_P,       /* (        -- v )  knob imm, 0..255                        */
    OP_R,       /* (        -- v )  xorshift PRNG, advances on every read   */
    OP_REG,     /* (        -- v )  read register imm                       */
    OP_SETREG,  /* ( v      -- v )  store to register imm, leave value      */
    OP_DLY,     /* ( n      -- v )  read delay line n samples back          */
    OP_WRT,     /* ( v      -- v )  write v to delay line, leave value      */
    OP_LP,      /* ( x c    -- v )  one-pole lowpass, state slot imm        */
    OP_HP,      /* ( x c    -- v )  one-pole highpass, state slot imm       */
    OP_BP,      /* ( x f q  -- v )  resonant state-variable bandpass        */
    OP_OR, OP_XOR, OP_AND,
    OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE,
    OP_SHL, OP_SHR,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MOD,
    OP_NEG,     /* ( a      -- v )  unary minus                             */
    OP_NOT,     /* ( a      -- v )  bitwise ~                               */
    OP_LNOT,    /* ( a      -- v )  logical !                               */
    OP_JZ,      /* ( a      --   )  pop; jump to imm if zero                */
    OP_JNZ,     /* ( a      --   )  pop; jump to imm if nonzero             */
    OP_JMP      /* (        --   )  jump to imm                             */
};

/* 8 bytes. Two int32s rather than a packed byte+operand because the VM
 * dispatch loop wants aligned loads more than it wants a small program. */
typedef struct {
    int32_t op;
    int32_t a;
} Insn;

/* What a knob is being USED for, worked out from the bytecode. A knob wired
 * to a shift is an octave control; the same knob wired to a multiply is a
 * continuous pitch control; wired to lp() it is a filter cutoff. Showing the
 * role next to the value turns eight anonymous numbers into a labelled front
 * panel, which is the difference between an instrument and a puzzle. */
enum {
    ROLE_NONE = 0,
    ROLE_SHIFT,   /* >> or <<   : octaves            */
    ROLE_MUL,     /* *          : pitch / rate       */
    ROLE_MASK,    /* & | ^      : timbre             */
    ROLE_CUT,     /* lp() hp()  : filter cutoff      */
    ROLE_PERIOD,  /* %          : period, i.e. pitch */
    ROLE_DELAY,   /* d()        : delay time         */
    ROLE_LEVEL,   /* + -        : offset / amount    */
    ROLE_RESON,   /* bp() f     : resonator pitch    */
    ROLE_Q,       /* bp() q     : resonance          */
    ROLE_MISC
};

const char *expr_role_name(int role);

/* A compiled program.
 *
 * `next` and `retire_epoch` are used only by the UI thread's reclamation
 * list -- see main.c. The audio thread reads everything below them and
 * writes nothing, which is what makes the atomic pointer swap safe without
 * any lock at all. */
typedef struct Program {
    struct Program *next;
    uint64_t        retire_epoch;
    int             n;      /* instruction count                     */
    int             depth;  /* max operand stack depth (<= STACK_MAX) */
    int             nfilt;  /* how many lp()/hp() slots it uses       */

    /* Which controls this expression actually reads. The UI dims the rest,
     * because a knob that cannot possibly affect the sound should not look
     * identical to one that can. */
    uint32_t        used_p;     /* bit i set if p<i> appears          */
    unsigned char   role[EXPR_NPARAM];   /* ROLE_* per knob           */
    int             uses_loop;  /* references k / n / bt / bl / ll    */
    int             uses_rng;   /* references r                       */
    int             uses_trigger; /* references tr / age / vel         */

    Insn            code[EXPR_CODE_MAX];
} Program;

/* Everything the VM needs that is NOT part of the program.
 *
 * This lives in the audio thread and survives program swaps, which is the
 * whole point: you can edit the expression mid-drone and the delay line,
 * the filters and the registers keep their contents, so the sound morphs
 * instead of restarting. */
typedef struct {
    int32_t  t, sr, k, n, bt, bl, ll;
    int32_t  tr, age, vel;
    int32_t  p[EXPR_NPARAM];
    uint32_t rng;                  /* xorshift32 state, never zero  */
    int32_t  reg[EXPR_NREG];       /* s0..s3                        */
    int64_t  filt[EXPR_NFILT];     /* lp/hp state, Q8 fixed point   */
    int64_t  bp_lo[EXPR_NFILT];    /* bp() low state, Q8            */
    int64_t  bp_band[EXPR_NFILT];  /* bp() band state, Q8           */
    int32_t *dly;                  /* EXPR_DELAY_LEN entries        */
    uint32_t dw;                   /* delay write cursor            */
} ExprCtx;

typedef struct {
    int  ok;        /* 1 = compiled, 0 = failed          */
    int  col;       /* 0-based column of the offending token */
    char msg[96];
} ExprError;

/* Compile `src` into `out`. Returns 1 on success. Never allocates: `out` is
 * caller-provided storage. Safe to call as often as you like -- the UI
 * recompiles on every keystroke. */
int expr_compile(const char *src, Program *out, ExprError *err);

/* Evaluate one sample. This is called once per sample per channel-frame and
 * is the hottest function in the program. */
int32_t expr_eval(const Program *pr, ExprCtx *c);

/* Human-readable one-line summary of the language, for the help overlay. */
const char *const *expr_help_lines(int *count);

#ifdef __cplusplus
}
#endif

#endif /* EXPR_H */
