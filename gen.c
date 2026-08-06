/* gen.c -- procedural patch generator.
 *
 * The design problem: random bytebeat is almost always silence. Pick eight
 * random knob values and a random combination of operators and the odds are
 * excellent that some `& 0` or `>> 31` collapses the whole thing to a flat
 * line. A "randomise" button that produces silence four times out of five is
 * worse than no button.
 *
 * Three things fix that:
 *
 *   1. STRUCTURED CHOICES. The candidate is a Rack, not a string, so the
 *      structure is always one that works and every slot is filled from the
 *      ladder for its kind -- a shift slot never receives 200, a mask slot
 *      only ever gets 2^n-1. This used to be a private table in this file;
 *      it is now rack.c, shared with the panel, so what the dice can reach
 *      and what your hands can reach are by construction the same set.
 *
 *   2. LADDER WINDOWS. Within a kind, the roll is confined to the stretch of
 *      the ladder that is worth landing on -- knob_gen_range(). The extremes
 *      stay reachable by hand.
 *
 *   3. AUDITION. The candidate is compiled and actually run through the VM
 *      right here, offline, before anyone hears it. Measure the RMS; if it is
 *      silent or slammed into the rails, throw it away and try the next seed.
 *      By the time gen_roll() returns, the patch is known to make a sound.
 *
 * Everything is deterministic in the seed, so a patch you like can be
 * recovered by writing down one number.
 */

#include "gen.h"
#include "dsp.h"
#include "knob.h"

#include <string.h>
#include <stdio.h>

/* ---- rng --------------------------------------------------------------- */
typedef struct { uint32_t s; } Rng;

static uint32_t rnd(Rng *r)
{
    uint32_t x = r->s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->s = x ? x : 0x1234567u;
    return r->s;
}

static int rrange(Rng *r, int lo, int hi)   /* inclusive */
{
    if (hi <= lo) return lo;
    return lo + (int)(rnd(r) % (uint32_t)(hi - lo + 1));
}

/* Roll a value for a slot: pick a rung inside the kind's generator window,
 * then read the ladder. This is the whole of "the generator cannot produce a
 * bad number", and it is three lines because the policy lives in knob.c. */
static int roll_slot(Rng *r, int kind)
{
    int lo, hi;
    knob_gen_range(kind, &lo, &hi);
    return knob_value(kind, rrange(r, lo, hi));
}

/* ---- rhythm ------------------------------------------------------------- */

void gen_euclid(int n, int pulses, int *gate)
{
    if (n < 1) n = 1;
    if (n > BB_STEPS) n = BB_STEPS;
    if (pulses < 0) pulses = 0;
    if (pulses > n) pulses = n;

    for (int i = 0; i < BB_STEPS; i++) gate[i] = GATE_OFF;

    /* Prime the bucket to n-1 so the first pulse lands on step 0. Starting
     * at 0 makes E(1,16) fire on the LAST step instead of the downbeat,
     * which is musically backwards. */
    int bucket = n - 1;
    for (int i = 0; i < n; i++) {
        bucket += pulses;
        if (bucket >= n) {
            bucket -= n;
            gate[i] = (i == 0) ? GATE_ACCENT : GATE_ON;
        }
    }
}

/* ---- assembling one candidate -------------------------------------------
 * `sr` is the structure stream and `vr` the value stream. Keeping them
 * separate is what makes "mutate" possible: reuse the structure seed, roll a
 * new value seed, and you get the same idea with different numbers. */
static void build(Rng *sr, Rng *vr, Voice *v)
{
    memset(v, 0, sizeof *v);

    v->rack.src   = (unsigned char)(rnd(sr) % (uint32_t)rack_nsrc());
    /* A lowpass is heavily favoured because it is what turns a bright
     * bitwise scream into something you would put in a track. */
    v->rack.body  = (unsigned char)((rnd(sr) % 100u) < 75u);
    v->rack.space = (unsigned char)((rnd(sr) % 100u) < 40u);
    v->rack.mode  = (unsigned char)rack_src_mode(v->rack.src);
    v->custom     = 0;
    v->mode       = v->rack.mode;

    RackBuild b;
    rack_build(&v->rack, &b);
    snprintf(v->expr, sizeof v->expr, "%s", b.expr);
    for (int i = 0; i < b.nslot; i++)
        v->p[b.slot[i].pidx] = roll_slot(vr, b.slot[i].kind);

    /* Post chain, biased dark and heavy rather than uniformly random. */
    v->ctl[LCTL_LEVEL]    = 150 + (int)(rnd(vr) % 90u);
    v->ctl[LCTL_DRIVE]    = (int)(rnd(vr) % 120u);
    v->ctl[LCTL_TONE]     = 40 + (int)(rnd(vr) % 170u);
    v->ctl[LCTL_CRUSH]    = (rnd(vr) % 3u == 0) ? (int)(rnd(vr) % 90u) : 0;
    v->ctl[LCTL_SPC_MIX]  = (rnd(vr) % 2u == 0) ? 40 + (int)(rnd(vr) % 120u) : 0;
    v->ctl[LCTL_SPC_FB]   = 80 + (int)(rnd(vr) % 130u);
    v->ctl[LCTL_SPC_TIME] = 40 + (int)(rnd(vr) % 200u);
    v->ctl[LCTL_STEPS]    = 16;
    v->ctl[LCTL_DECAY]    = 0;
    v->ctl[LCTL_SPC_SYNC] = 0;
    v->ctl[LCTL_SPC_FREEZE] = 0;

    /* A third of the time, hand back something already rhythmic. A gated
     * euclidean pattern with a short decay is the difference between a drone
     * and a part, and it is the single fastest way to discover that this
     * instrument does percussion. */
    for (int i = 0; i < BB_STEPS; i++) {
        v->ratchet[i] = 1;
        v->prob[i] = 100;
        for (int k = 0; k < BB_LOCK_COUNT; k++) v->lock[k][i] = -1;
    }
    v->seq_on = rack_src_triggered(v->rack.src) || (rnd(sr) % 100u) < 35u;
    if (v->seq_on) {
        static const int PULSES[] = { 2, 3, 4, 5, 6, 7, 9, 11 };
        gen_euclid(16, PULSES[rnd(vr) % (sizeof PULSES / sizeof PULSES[0])],
                   v->gate);
        v->ctl[LCTL_DECAY] = 40 + (int)(rnd(vr) % 160u);
        /* Trigger engines benefit from articulation immediately. A small
         * chance of a ratchet/probability variation demonstrates those lanes
         * without making generated patterns incoherent. */
        if (rack_src_triggered(v->rack.src)) {
            for (int i = 0; i < BB_STEPS; i++) if (v->gate[i]) {
                if ((rnd(vr) % 100u) < 20u) v->ratchet[i] = 2 + (int)(rnd(vr) % 3u);
                if ((rnd(vr) % 100u) < 18u) v->prob[i] = 55 + (int)(rnd(vr) % 41u);
            }
        }
    }
}

/* ---- audition ----------------------------------------------------------
 * UI-thread only. These are big but static: the generator runs while you are
 * holding a key down, not while the audio thread has a deadline. */
static Program  g_scratch;
static int32_t  g_scratch_delay[EXPR_DELAY_LEN];

/* RMS of one 12000-sample window starting at t0, as a percentage of full
 * scale. The program must already be compiled into g_scratch. */
static int measure_window(const Voice *v, uint32_t t0)
{
    ExprCtx c;
    memset(&c, 0, sizeof c);
    memset(g_scratch_delay, 0, sizeof g_scratch_delay);
    c.dly = g_scratch_delay;
    c.rng = 0x1234567u;
    c.sr  = 44100;
    c.bl  = 11025;
    c.ll  = 88200;
    for (int i = 0; i < BB_NPARAM; i++) c.p[i] = v->p[i];

    const int WARM = 2000, N = 12000;
    uint64_t sum = 0;
    int32_t dc_x1 = 0;
    int64_t dc_y1 = 0;
    int used = 0;

    for (int i = 0; i < WARM + N; i++) {
        uint32_t t = t0 + (uint32_t)i;
        c.t  = (int32_t)t;
        c.k  = (int32_t)(t % (uint32_t)(c.ll ? c.ll : 1));
        c.bt = (int32_t)(t % (uint32_t)(c.bl ? c.bl : 1));
        c.n  = (int32_t)(t / (uint32_t)(c.bl * 4));
        /* Audition trigger-aware engines as instruments, not as a silent
         * expression waiting for a sequencer that is not running here. */
        int hit_age = i % 2048;
        c.tr  = hit_age == 0;
        c.age = hit_age;
        c.vel = (i % 8192 == 0) ? 256 : 148;
        int32_t x = expr_eval(&g_scratch, &c);
        c.dw = (c.dw + 1u) & EXPR_DELAY_MASK;

        int32_t s;
        switch (v->mode) {
        case BB_BYTE:   s = (int32_t)((x & 0xff) - 128) << 8; break;
        case BB_SIGNED: s = (int32_t)(int8_t)(x & 0xff) << 8; break;
        default:        s = (int32_t)(int16_t)(x & 0xffff);   break;
        }

        /* Same DC blocker the real chain applies, so the level we measure is
         * the level you will actually hear rather than a DC offset. */
        int64_t y = (((int64_t)(s - dc_x1)) << 16) + ((dc_y1 * 65500) >> 16);
        dc_x1 = s;
        dc_y1 = y;
        s = (int32_t)(y >> 16);

        /* Clamp before squaring: this is what the output stage does anyway,
         * and it keeps the accumulator inside int64 for any input. */
        s = dsp_clip16(s);
        if (i >= WARM) { sum += (uint64_t)((int64_t)s * s); used++; }
    }

    /* Integer sqrt so the generator needs no libm. The engine deliberately
     * links against nothing at all: bbengine is built and the whole suite is
     * run with third_party/JUCE empty, which is what lets the instrument be
     * proven on a machine that has only a C compiler. */
    uint64_t ms = used ? sum / (uint64_t)used : 0;
    uint64_t r = 0, bit = 1ULL << 40;
    while (bit > ms) bit >>= 2;
    while (bit) {
        if (ms >= r + bit) { ms -= r + bit; r = (r >> 1) + bit; }
        else               { r >>= 1; }
        bit >>= 2;
    }
    return (int)(r * 100u / 32768u);
}

/* Three windows, spread across two minutes of the sample counter, and the
 * loudest one wins.
 *
 * Measuring only from t=0 quietly biases the whole generator. A modulator
 * written `t>>12` has only reached 3 by the end of a 14000-sample window, so
 * any patch whose structure lives in the high bits of a slow shift looks like
 * silence and gets thrown away -- it has not started yet. Starting later, and
 * taking the maximum rather than the mean, judges a patch on what it does
 * once it is under way, and lets a source legitimately be quiet for a stretch
 * without being rejected for it. */
int gen_measure(const Voice *v)
{
    ExprError err;
    if (!expr_compile(v->expr, &g_scratch, &err)) return -1;

    /* An expression that mentions no knobs at all is not a patch, it is a
     * fixed noise -- reject so the generator always gives you something to
     * turn. */
    if (g_scratch.used_p == 0) return -1;

    static const uint32_t WINDOW[] = { 0, 1u << 18, 1u << 21 };
    int best = 0;
    for (int i = 0; i < (int)(sizeof WINDOW / sizeof WINDOW[0]); i++) {
        int lvl = measure_window(v, WINDOW[i]);
        if (lvl > best) best = lvl;
    }
    return best;
}

/* ---- public ------------------------------------------------------------ */

/* The published seed packs the structure choice in the low 16 bits and the
 * value choice in the high 16. Both halves are therefore kept to 16 bits all
 * the way through, so a seed ALWAYS round-trips: gen_roll() on a seed it
 * previously returned reproduces that patch exactly. Write the number down
 * and you can get the sound back.
 *
 * The 16-bit halves are expanded through a multiplicative mix before seeding
 * the RNG, so adjacent seeds are not adjacent patches. */
static uint32_t expand(unsigned half)
{
    uint32_t x = (uint32_t)(half & 0xffffu) * 0x9E3779B1u;
    x ^= x >> 15;
    x *= 0x85EBCA6Bu;
    x ^= x >> 13;
    return x ? x : 0x1234567u;
}

static unsigned roll_from(unsigned ss, unsigned vs, int restructure, Voice *out)
{
    ss &= 0xffffu;
    vs &= 0xffffu;

    for (int attempt = 0; attempt < 48; attempt++) {
        Rng sr = { expand(ss) };
        Rng vr = { expand(vs) };
        build(&sr, &vr, out);

        int lvl = gen_measure(out);
        if (lvl >= 6 && lvl <= 85) {
            out->level = lvl;
            out->seed  = (vs << 16) | ss;
            return out->seed;
        }
        /* 40503 is odd, so repeatedly adding it visits every 16-bit value. */
        vs = (vs + 40503u) & 0xffffu;
        if (restructure && (attempt % 4) == 3) ss = (ss + 1u) & 0xffffu;
    }

    /* Nothing passed -- hand back something known good rather than nothing. */
    memset(out, 0, sizeof *out);
    rack_default(&out->rack);
    out->mode = out->rack.mode;
    RackBuild b;
    rack_build(&out->rack, &b);
    snprintf(out->expr, sizeof out->expr, "%s", b.expr);
    rack_seed_params(&b, out->p);
    out->ctl[LCTL_LEVEL] = 190;
    out->ctl[LCTL_DRIVE] = 40;
    out->ctl[LCTL_TONE]  = 90;
    out->ctl[LCTL_STEPS] = 16;
    out->ctl[LCTL_SPC_TIME] = 140;
    out->ctl[LCTL_SPC_FB]   = 150;
    out->ctl[LCTL_SPC_MIX]  = 60;
    out->ctl[LCTL_SPC_SYNC] = 0;
    out->ctl[LCTL_SPC_FREEZE] = 0;
    for (int i = 0; i < BB_STEPS; i++) {
        out->ratchet[i] = 1;
        out->prob[i] = 100;
        for (int k = 0; k < BB_LOCK_COUNT; k++) out->lock[k][i] = -1;
    }
    out->level = gen_measure(out);
    out->seed  = 0;
    return 0;
}

unsigned gen_roll(unsigned seed, Voice *out)
{
    return roll_from(seed & 0xffffu, seed >> 16, 1, out);
}

/* Same structure, different numbers: restructure is off, so whatever source
 * and stages the seed picked survive and only the values move. */
unsigned gen_mutate(unsigned seed, unsigned salt, Voice *out)
{
    return roll_from(seed & 0xffffu, ((seed >> 16) + salt) & 0xffffu, 0, out);
}
