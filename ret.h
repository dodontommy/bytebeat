/* ret.h -- the RETURN BUS effect core: the DSP half of MORGUE's send/return
 * system, and nothing else.
 *
 * WHAT THIS FILE IS. Eight pre-allocated return slots, each of which can hold
 * one of four effects. A slot takes one input sample and returns one raw wet
 * sample. That is the whole surface. There is no routing here, no atomics, no
 * knowledge of `bb`, no session I/O and no UI: engine.c owns the send matrix,
 * the link matrix, the create/destroy handshake and the safety arming, and it
 * calls into this file once per live slot per frame.
 *
 * WHY IT IS SPLIT OUT. engine.c is already ~3,000 lines and the render loop is
 * the most safety-critical code in the program. Four effect algorithms plus a
 * 256-entry tuning table plus two 8-slot memory pools do not belong inside it,
 * for the same reason dsp.c is not inside it.
 *
 * WHY IT OWNS BB_NRET AND THE RET_* IDS. Something has to declare them first,
 * and the DSP core is the thing that cannot be built without them -- it sizes
 * its pools from BB_NRET and switches on the type ids. bytebeat.h includes
 * THIS header to get them, rather than the reverse, so this file stays
 * buildable and testable on its own with no engine present. Adding a new
 * effect therefore touches exactly two files: this one and ret.c.
 *
 * THE RULES THIS FILE LIVES UNDER, all inherited from the audio thread:
 *
 *   - ret_run() is called once per sample per live slot. It never allocates,
 *     never locks, never calls libm, and never divides by a runtime value.
 *     Everything rate- or tempo-dependent is resolved into a RetSnap once per
 *     period by ret_period(), which is where all the division lives.
 *   - Every state variable is a file-scope static. Nothing is ever malloc'd.
 *   - Integer only. Every left shift of a possibly-negative value goes through
 *     the matching unsigned type, because a signed left shift of a negative
 *     value is undefined behaviour, the project deliberately dropped its
 *     dependence on -fwrapv, and MSVC has no -fwrapv to fall back on. This is
 *     the same U()/S() discipline expr.c and dsp.c already use.
 *   - Deterministic. No floats, no randomness, no time. The same input and the
 *     same params produce the same output on every platform, every run --
 *     which is what lets the regression suite hash a rendered buffer.
 *
 *   - THE CHAMBER IS BIT-EXACT AND MUST STAY THAT WAY. ret_chamber_process()
 *     is engine.c's old verb_process() moved verbatim and reindexed off a
 *     carved block instead of five file statics. Not one line inside the comb
 *     or allpass loop changed, including the signed `>> 2`, `>> 3` and `>> 1`
 *     that would otherwise be candidates for "cleanup". A session with one
 *     CHAMBER and no other returns renders bit-identically to the engine that
 *     had no return bus, the regression suite pins that with an FNV-1a hash,
 *     and 3,002 checks depend on it.
 */
#ifndef RET_H
#define RET_H

#include <stdint.h>
#include "dsp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- slot geometry -------------------------------------------------------
 *
 * WHY EIGHT SLOTS. Four is not enough for the genre this instrument exists
 * for: a big reverb, a long delay, a short metallic delay and a drive node is
 * already four, and the entire point is that the second reverb feeds the
 * first. Sixteen doubles the memory and the CPU, but its real cost is the
 * mixer -- 16 columns by 11 source rows is not a grid anyone can point at
 * under stage light, while 8x8 with a legible diagonal is about the limit.
 * Eight also matches BB_NLAYER, BB_SAMPLER and VERB_NCOMB, and eight
 * simultaneous CHAMBERs is the practical CPU ceiling anyway.
 *
 * BB_RET_NPARAM is 8 to match BB_NPARAM, even though no shipping type uses
 * more than six: the session line has fixed arity, so the two spare knobs
 * cost eight bytes a session and mean a later type never changes the format.
 *
 * These live here rather than in bytebeat.h because this file cannot be
 * compiled without them and bytebeat.h can (it includes this header). If you
 * are adding the routing matrix to bytebeat.h, do NOT paste a second copy of
 * these -- #include "ret.h" there instead. Two definitions that drift is the
 * failure mode this guard exists to make loud. */
#ifndef BB_NRET
#define BB_NRET        8      /* pre-allocated return slots                  */
#define BB_RET_NPARAM  8      /* per-effect knobs; meaning is per type        */
#define BB_RET_NAME    16     /* UI name, NUL-terminated                      */

/* Effect type ids. APPEND ONLY -- these numbers are written into session.conf.
 * Ids 5..9 are RESERVED ON DAY ONE and must not be reused for anything else,
 * so that a session written by a future build and read by this one keeps its
 * meaning. An id >= RET_NTYPE is a slot written by a newer build: it is
 * preserved verbatim through load/save and renders as silence.
 *
 * That last sentence is why dispatch in ret_run() is a switch with
 * `default: return 0;` and MUST NEVER become a function-pointer table. An
 * unknown id has to be silence, not an out-of-bounds jump. Treat any
 * table-of-function-pointers dispatch in this file as a defect. */
enum {
    RET_NONE = 0,
    RET_CHAMBER,        /* 1  the existing reverb; bit-exact, must not change */
    RET_DELAY,          /* 2  long delay, filtered and driven repeats         */
    RET_DRIVE,          /* 3  parallel saturate / clip / wavefold             */
    RET_CHOIR,          /* 4  four tuned resonator combs                      */
    RET_NTYPE,          /* 5..9 reserved: GATE, FILTER, CRUSH, RING, SHIFT    */
    RET_RESERVED_MAX = 10
};
#endif /* BB_NRET */

/* ---- the two shared memory pools -----------------------------------------
 *
 * Two pools, not a per-slot union of every type's worst case:
 *
 *   g_ret_dl[8][1<<19] int16  = 8,388,608 B   general delay pool
 *   g_ret_vb[8][40960] int32  = 1,310,720 B   carved reverb blocks
 *   RetState[8]               =     1,344 B   positions and filter states
 *                               -----------
 *                                9,700,672 B  = 9.25 MiB exactly
 *
 * WHY int16 FOR THE DELAY POOL. SPACE's line is int32, but every write into it
 * goes through the saturator, so nothing in a delay line ever leaves int16
 * range anyway -- storing int32 would double the memory and the cache traffic
 * to hold sign extension. 1<<19 samples is 11.9 s at 44.1 kHz and 5.46 s at
 * 96 kHz, twice the reach of the per-voice SPACE lines, which is the right
 * scale for a return. Power of two so the wrap is an AND and never a divide.
 *
 * WHY int32 FOR THE REVERB BLOCK. The comb write genuinely exceeds +/-32767
 * and the CHAMBER's bit-exactness depends on it. Non-negotiable.
 *
 * WHY ~9.25 MiB IS AFFORDABLE. The engine already spends 8 MiB on the
 * per-voice SPACE lines plus 2 MiB of loop buffer and 2 MiB of sink ring --
 * about 12.5 MiB of statics today. These pools are BSS, i.e. demand-zero: an
 * untouched slot costs ZERO resident pages, so a session with one CHAMBER
 * resides exactly what it resides today plus the chamber block.
 *
 * WHICH IS WHY ret_init() DELIBERATELY DOES NOT CLEAR THE POOLS. Clearing
 * 9.25 MiB at startup faults in every page of a session that uses one return,
 * and bb_engine_init() is called seven times by the regression suite. A slot
 * is cleared only when its type changes, and only over that type's footprint.
 * A well-meaning "initialise everything" commit destroys this property. */
#define RET_DL_BITS  19
#define RET_DL_LEN   (1u << RET_DL_BITS)
#define RET_DL_MASK  (RET_DL_LEN - 1u)

/* CHOIR quarters the delay line into four independent comb buffers. */
#define RET_Q_LEN    (RET_DL_LEN >> 2)
#define RET_Q_MASK   (RET_Q_LEN - 1u)

/* The CHAMBER's predelay gets its own smaller power-of-two window at the head
 * of the same line. 1<<17 samples is 1.36 s at 96 kHz and 0.68 s at 192 kHz,
 * comfortably over the 500 ms the knob can ask for at any rate this engine
 * runs at, and it means a type change clears 256 KB instead of 1 MB. */
#define RET_PRE_BITS 17
#define RET_PRE_LEN  (1u << RET_PRE_BITS)
#define RET_PRE_MASK (RET_PRE_LEN - 1u)

/* Moved verbatim from engine.c. The buffers are sized for the longest comb at
 * 96 kHz -- 1617 * 96000/44100 = 3520 < 4096, 556 * 96000/44100 = 1210 < 2048
 * -- and the lengths are rescaled to the actual device rate once per period. */
#define VERB_NCOMB   8
#define VERB_NAP     4
#define VERB_CSIZE   4096
#define VERB_ASIZE   2048
#define RET_VB_WORDS (VERB_NCOMB * VERB_CSIZE + VERB_NAP * VERB_ASIZE)  /* 40960 */

/* ---- the DC blocker ------------------------------------------------------
 *
 * There is ONE implementation of this filter in the tree and it is dsp.c's.
 * The contract refactors dsp_dcblock() into a reusable dsp_dc(DcState*, x)
 * with dsp_dcblock() left as a one-line forwarder, so the returns can carry
 * their own without a second copy that drifts.
 *
 * Until that refactor lands, this is the bridge: an identical body, same
 * DC_R, same uint64_t shift, compiled only while dsp.h does not provide the
 * real thing. dsp.h's owner defines DSP_HAS_DCSTATE next to the DcState
 * typedef and this block vanishes. If it lands WITHOUT that #define you get a
 * duplicate-typedef error, which is exactly the right failure: loud,
 * immediate, and impossible to ship. */
#ifndef DSP_HAS_DCSTATE
typedef struct { int32_t x1; int64_t y1; } DcState;

/* R = 65500/65536 puts the corner near 4 Hz at 44.1 kHz -- below anything
 * audible, so nothing musical is touched, but DC is killed outright. */
#define RET_DC_R 65500
static inline int32_t dsp_dc(DcState *d, int32_t x)
{
    /* THE FEEDBACK TERM ROUNDS TOWARD ZERO, AND THAT IS NOT COSMETIC.
     *
     * dsp.c spells this term `(y1 * DC_R) >> 16`, and an arithmetic right
     * shift rounds toward MINUS INFINITY. That gives the recursion a fixed
     * point that should not exist: y1 = -1 maps to (-65500) >> 16 = -1, so a
     * state that has decayed to a single negative count NEVER decays any
     * further, and the filter reports -1 forever. On a voice that is a curio,
     * because the voice always has a signal on top of it. On a return it is
     * the whole failure: a return whose input has stopped is supposed to go
     * silent, and a permanent -1 is a meter that never returns to zero, a
     * DRIVE that is not silent on silence, and DC -- the exact quantity this
     * filter exists to remove -- left sitting on the bus.
     *
     * Rounding the magnitude and reapplying the sign makes the filter
     * symmetric, which is what it always claimed to be, and makes zero the
     * only fixed point. Every positive state behaves exactly as before, so
     * nothing that was already correct moves.
     *
     * dsp.c's copy is deliberately NOT changed to match: its output is pinned
     * sample-for-sample by the CHAMBER's golden hash, and this file's copy is
     * reachable only from the return bus, which the hash never touches. The
     * two are allowed to differ here precisely because one of them is load
     * bearing for back-compatibility and the other is not. */
    int64_t fb = d->y1 * RET_DC_R;
    fb = fb >= 0 ? (fb >> 16) : -((-fb) >> 16);

    /* The scaling shift goes through uint64_t: x - x1 is negative for half of
     * every waveform and a signed left shift of a negative value is undefined.
     * Shifting the bit pattern and reinterpreting gives the identical value on
     * every compiler. Nothing about the filter changes. */
    int64_t y = (int64_t)((uint64_t)(int64_t)(x - d->x1) << 16) + fb;
    d->x1 = x;
    d->y1 = y;
    return (int32_t)(y >> 16);
}
#endif /* DSP_HAS_DCSTATE */

/* ---- the limiter ---------------------------------------------------------
 *
 * This is not a mixing feature bolted on the side; it IS the stability proof
 * for the whole return graph. Every return->return edge in the bus is delayed
 * by exactly one sample, so the bus is x[n+1] = f(A*x[n] + B*u[n]) where f
 * ends in this function. Because this function has a hard, unconditional
 * ceiling, |x[n]| <= CEIL for every n, every matrix A and every patch the user
 * can possibly build -- no eigenvalue analysis, no cycle detection, no
 * restriction on what may be linked to what. Feedback is the instrument here;
 * this is what makes designing for it safe rather than reckless.
 *
 * WHY 24576 AND NOT 32767. A frozen slot -- a self-link at unity -- sits 2.5 dB
 * below the rails permanently, which leaves the dry bus headroom so that the
 * MASTER clipper is never the thing doing the limiting. A loop pinned by a hard
 * clipper settles into a full-scale square wave, and into headphones that is
 * the worst waveform at the worst level. This ceiling makes the limit a wall
 * you push into instead of a rail you are welded to.
 *
 * WHY HALVING AND NOT A DIVIDE. dsp.c's header forbids a runtime division in
 * the hot path. Halving the gain converges from any overshoot in at most eight
 * samples, and the hard clamp underneath makes the bound hold on the very
 * first sample regardless of what the gain rider is doing.
 *
 * The 256 floor is 48 dB of range: enough to bury any runaway, not so much
 * that recovery from a transient takes a perceptible age. */
#define BB_RET_CEIL      24576   /* -2.5 dBFS per-return ceiling             */
#define BB_RET_BUS_CEIL  30000   /* -0.8 dBFS on the summed wet bus          */
#define BB_RET_REL          12   /* Q16 per sample, ~123 ms full recovery    */
#define BB_RET_GAIN_UNITY 65536  /* Q16; *g starts here and returns to it    */

static inline int32_t ret_limit(int32_t *g, int32_t x, int32_t ceil_)
{
    int32_t y = (int32_t)(((int64_t)x * *g) >> 16);
    int32_t a = y < 0 ? -y : y;
    if (a > ceil_) {
        *g >>= 1;                        /* instant attack, no divide       */
        if (*g < 256) *g = 256;
        y = (int32_t)(((int64_t)x * *g) >> 16);
        if (y >  ceil_) y =  ceil_;      /* the bound is unconditional      */
        if (y < -ceil_) y = -ceil_;
    } else if (*g < BB_RET_GAIN_UNITY) {
        *g += BB_RET_REL;
        if (*g > BB_RET_GAIN_UNITY) *g = BB_RET_GAIN_UNITY;
    }
    return y;
}

/* ---- per-slot scratch ----------------------------------------------------
 *
 * Positions, filter states and one-sample histories. The big lines live in the
 * pools; nothing in here is bigger than a cache line's worth of geometry, so
 * eight slots' worth stays resident and a type change is cheap to zero. The
 * union of every type's needs rather than a per-type union: 200 bytes times
 * eight slots is not worth the aliasing hazard of overlapping them. */
typedef struct {
    /* CHAMBER */
    int32_t   vdamp[VERB_NCOMB];  /* per-comb damping filter state          */
    uint32_t  vcpos[VERB_NCOMB];
    uint32_t  vapos[VERB_NAP];
    uint32_t  prew;               /* predelay write head into g_ret_dl      */
    /* DELAY */
    uint32_t  dw;
    int64_t   dtone;              /* Q8 one-pole inside the feedback loop   */
    /* CHOIR */
    uint32_t  qw;
    int64_t   qdamp[4];
    /* DRIVE */
    int64_t   htone;              /* Q8 post lowpass                        */
    int32_t   hprev;              /* one-sample shaper feedback             */
    int32_t   hhp;                /* BITE pre-emphasis state                */
    /* Every type that can emit DC from silence carries its own, always on.
     * The voice chain's DC blocker is upstream of the returns and protects
     * nothing here, and the bus's safety DC blocker is armed only on slots
     * that have an incoming feedback edge -- so a type that makes an offset
     * out of nothing has to clean up after itself. */
    DcState   dc;
} RetState;

/* ---- the per-period snapshot ---------------------------------------------
 *
 * One of these per slot, refilled once per render period. It exists so that
 * ret_run() never reads an atomic, never divides and never rescales anything:
 * the whole per-period cost of a slot is one ret_period() call. This is the
 * same discipline as engine.c's LSnap and space_samples().
 *
 * IN  -- filled by the engine's snapshot from the slot's atomics, BEFORE it
 *        calls ret_period(). Deliberately not read from `bb` in here: this
 *        file has no engine knowledge, which is what lets the effect core be
 *        unit-tested with no `bb` in the link.
 * OUT -- filled by ret_period() and read by ret_run().
 * BUS -- owned by engine.c end to end; carried here only so a slot's whole
 *        per-period story is one struct. ret_run() ignores both. */
typedef struct {
    /* IN */
    int type;                    /* raw id; >= RET_NTYPE renders as silence */
    int level;                   /* 0..256, ALREADY zeroed if muted         */
    int p[BB_RET_NPARAM];        /* 0..255                                  */
    int sync;                    /* 0 = free, 1..10 = step division         */

    /* BUS */
    int hot;                     /* safety stage armed (sticky, engine-side) */
    int fade_tgt;                /* 65536 or 0                              */

    /* OUT */
    int rate;
    int vclen[VERB_NCOMB];       /* CHAMBER, computed byte-identically to   */
    int valen[VERB_NAP];         /* engine.c's old per-period block         */
    int vfb;                     /* 22000 + p0*40, or 32700 while HOLD      */
    int vdamp_c;                 /* 4096 + p1*112                           */
    int vpre;                    /* predelay in samples; 0 = stage skipped  */
    int dtap, dtap2;             /* DELAY primary and SPREAD taps           */
    int qlen[4];                 /* CHOIR comb lengths                      */
    int qn;                      /* CHOIR combs actually running: 1 or 4    */
    /* DRIVE: what the shaper emits for an input of exactly zero, i.e. the
     * constant BIAS alone produces. ret_drive_run() subtracts it, which is
     * what makes silence in mean silence out at every BIAS setting. Resolved
     * here rather than per sample because it is a per-period constant. */
    int hbias;
} RetSnap;

/* ---- lifecycle. UI thread only. ------------------------------------------ */

/* Zeroes the per-slot scratch and NOTHING ELSE. See the pool comment above:
 * touching the arenas here would fault in 9.25 MiB on every bb_engine_init(),
 * and the suite calls that seven times. */
void ret_init(void);

/* Zero one slot's scratch and clear the arena, over the UNION of what the
 * outgoing type left behind and what `type` is about to touch. Called on a
 * type change, on create/destroy and from bb_engine_init().
 *
 * THE UNION IS THE WHOLE POINT, and clearing only the incoming footprint --
 * which is what this used to do -- is a residue bug with a long fuse. A DELAY
 * writes all 512 K words of the line; a CHAMBER reads only the first 128 K and
 * so clears only those. Swap DELAY for CHAMBER and 384 K words of the previous
 * effect's audio are still sitting there, silent but present, waiting for the
 * next DELAY or CHOIR to read them back as a tail nobody played. That is why
 * "create and destroy every return" stops rendering bit-identically and why a
 * second identical run does not match the first.
 *
 * Which type a slot's arena was last prepared for is remembered inside ret.c,
 * so this stays correct no matter what the caller believes the outgoing type
 * was -- bb_engine_init() in particular calls this AFTER the session defaults
 * have already rewritten bb.ret[r].type, and so cannot know.
 *
 * The clear stays proportional: the union of two footprints is at most the
 * larger one, so a CHAMBER following a CHAMBER still costs 160 KB + 256 KB and
 * never the full 1.25 MB. */
void ret_reset(int slot, int type);

/* The per-effect resets, exposed so a caller can re-arm one type's state
 * without deciding what its arena footprint is. ret_reset() dispatches to
 * these. */
void ret_chamber_reset(int slot);
void ret_delay_reset  (int slot);
void ret_drive_reset  (int slot);
void ret_choir_reset  (int slot);

/* Everything, including both pools. This is the honest "reset the machine"
 * call, for tests that need a provably pristine starting state.
 *
 * DO NOT call it from bb_engine_init(). It writes 9.25 MiB, which is the one
 * thing the demand-zero memory argument depends on not happening. Init should
 * loop ret_reset(slot, type) over the eight slots instead. */
void ret_reset_all(void);

/* Words of each pool that `type` touches, so a clear stays proportional. */
unsigned ret_footprint_dl(int type, int rate);   /* int16 words */
unsigned ret_footprint_vb(int type);             /* int32 words */

/* ---- per period ----------------------------------------------------------
 * Resolves the IN fields of `s` into the OUT fields. All of this file's
 * division lives in here. `slot` is accepted for symmetry with the rest of the
 * API and because a future type may need per-slot geometry; it is unused
 * today. */
void ret_period(int slot, RetSnap *s, int rate,
                uint32_t beat_len, uint32_t bar_len);

/* ---- the hot path --------------------------------------------------------
 * One frame in, one RAW wet frame out -- no level, no fade, no limiter, no
 * peak; those belong to the bus. Called only for live slots.
 *
 * CONTRACT ON EVERY TYPE, and it is a hard requirement rather than advice:
 * internally BIBO-stable at EVERY parameter setting. Internal feedback is
 * capped below unity in code, the way dsp.c caps SPACE at 244/256 and the way
 * the CHAMBER's feedback tops out at 32200/32768. Graph feedback is the bus's
 * problem and the bus's limiter answers it; internal feedback is the type's
 * own problem, because a type that self-oscillates from silence has no
 * incoming link and therefore gets no bus guard at all. */
int32_t ret_run(int slot, const RetSnap *s, int32_t in);

/* ---- the CHAMBER, exposed ------------------------------------------------
 * engine.c keeps thin verb_reset()/verb_process() forwarders onto slot 0 so
 * that the chamber self-test block in the suite compiles and passes UNEDITED,
 * with its existing hard-coded fb 28880, damp 14848 and line lengths. If that
 * block ever needs new numbers, the chamber's arithmetic moved and the
 * bit-exactness argument is already broken -- stop and bisect. */
int32_t ret_chamber_process(int slot, int32_t in, int32_t fb, int32_t damp,
                            const int *clen, const int *alen);

/* The wavefolder, exposed for the suite: the contract asks for a test that it
 * stays inside +/-8192 across +/-2^20 and that |fold(x+1) - fold(x)| <= 1
 * everywhere, which is the assertion that it is a continuous triangle and not
 * a sawtooth with a cliff in it. */
int32_t ret_fold(int32_t x);

/* ---- metadata ------------------------------------------------------------
 * One table drives the whole UI and the session writer. There is no per-type
 * panel and no per-type session key.
 *
 * The vocabulary is FIXED across types, and that is the point: DARK is always
 * the loop or post lowpass, FEED is always regeneration, HOLD is always
 * freeze, SPREAD is always a detuned or offset second voice. Six words learned
 * once instead of thirty learned per effect. A NULL label means that knob is
 * unused by this type and the UI draws it greyed. */
extern const char * const  ret_type_name [RET_NTYPE];
extern const char * const  ret_param_name[RET_NTYPE][BB_RET_NPARAM];
extern const unsigned char ret_param_def [RET_NTYPE][BB_RET_NPARAM];
extern const short         ret_level_def [RET_NTYPE];

#ifdef __cplusplus
}
#endif

#endif /* RET_H */
