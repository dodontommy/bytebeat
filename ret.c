/* ret.c -- the four return effects, their shared pools, and the arithmetic
 * that keeps all of it bounded.
 *
 * Read ret.h first; it carries the design. What follows is the implementation:
 * the shared arithmetic, the four effects in type-id order, the dispatch, the
 * per-period resolver where all the division lives, then lifecycle and the
 * metadata tables that drive the mixer and the session writer.
 *
 * The one rule worth repeating here because it is the easiest to break by
 * accident: ret_run() and everything it calls must not divide by a runtime
 * value and must not shift a negative signed value left. Division by a
 * compile-time constant is fine -- the compiler turns it into a multiply, and
 * dsp.c's saturator already relies on exactly that.
 */

#include "ret.h"
#include <string.h>

/* Mirrors BB_RATE_MIN in bytebeat.h. Duplicated as a literal rather than
 * pulled in, because this file deliberately knows nothing about the engine;
 * it is only ever used the way engine.c used it, as a sanity floor before
 * rescaling the reverb's line lengths. */
#define RET_RATE_MIN 1000

/* The widest input ret_run() will accept; see the comment at the clamp. */
#define RET_IN_MAX (1 << 24)

/* ========================================================================= */
/*  THE POOLS                                                                */
/* ========================================================================= */

/* Both are BSS, i.e. demand-zero. Nothing in this file clears them except
 * ret_reset()/ret_reset_all(), and ret_init() deliberately does not -- see the
 * long comment in ret.h. A slot that is never used never costs a page. */
static int16_t  g_ret_dl[BB_NRET][RET_DL_LEN];    /* 8.00 MiB, int16          */
static int32_t  g_ret_vb[BB_NRET][RET_VB_WORDS];  /* 1.25 MiB, int32          */
static RetState g_ret_st[BB_NRET];

/* Which type each slot's ARENA was last prepared for -- not which type the
 * slot currently holds, which is the engine's business and not ours.
 *
 * This exists so a reset can clear the union of the outgoing and incoming
 * footprints without the caller having to tell us what the outgoing type was.
 * That matters because the two callers who most need it cannot say:
 * bb_engine_init() runs after bb_engine_set_defaults() has already rewritten
 * every slot's type, and the session loader rewrites types straight from the
 * file. Keeping the fact here, next to the memory it describes, is the only
 * place it cannot go stale.
 *
 * It is deliberately NOT cleared by ret_init(). ret_init() does not clear the
 * pools either, and a tracker that claimed a dirty arena was clean would
 * hand the next effect the previous one's tail. It starts at RET_NONE, whose
 * footprint is zero, which is exactly true of BSS on the first run. */
static int      g_ret_arena[BB_NRET];

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
/* If this trips, something big migrated into the per-slot scratch that
 * belongs in a pool. The budget exists so eight slots' worth of geometry
 * stays in cache while the lines stream past it. */
_Static_assert(sizeof(RetState) <= 512, "RetState grew past its budget");
_Static_assert(RET_VB_WORDS == 40960,   "the carved reverb block changed size");
#endif

/* ========================================================================= */
/*  SHARED ARITHMETIC                                                        */
/* ========================================================================= */

static inline int ret_clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* DARK, AS A KNOB, TURNS THE WAY ITS LABEL SAYS -- and getting that backwards
 * was not a cosmetic bug.
 *
 * The one-pole below takes a coefficient where 255 is wide open and 1 is a
 * corner down around 27 Hz at 44.1 kHz -- and around FIVE Hz at the 8 kHz the
 * regression suite runs at, because the coefficient is a per-sample fraction
 * and nothing rescales it. Handing it the raw knob meant DARK = 0 was the
 * darkest setting available, which is upside down twice over:
 *
 *   - It contradicts the label. Every other parameter in this file is neutral
 *     at zero -- BITE, FEED, SPREAD, DIRT, HOLD and PRE are all off at 0 --
 *     and a knob called DARK that is at its darkest when it reads zero is one
 *     the user has to be told about. It also matters to the ENGINE, not just
 *     to the UI: a slot that has never held an effect carries all zeros, so
 *     "zero is neutral" is what makes a freshly created return behave like the
 *     effect it says it is instead of like a broken one.
 *
 *   - Worse, it is a hearing-safety problem the moment there is feedback. A
 *     one-pole with a sub-audio corner sitting inside a delay's regeneration
 *     loop strips out everything you can actually hear and leaves the loop
 *     circulating what is left, which is a fraction of a Hertz. Measured: a
 *     self-linked DELAY at DARK 0 stopped producing its 50 Hz repeat tone
 *     entirely and settled into a full-amplitude one-Hertz ramp -- the
 *     limiter pinned, the meters lit, nothing audible, and the worst possible
 *     signal for a speaker cone. The DC blocker downstream cannot help,
 *     because a 1 Hz wander is not DC and its corner is 0.7 Hz.
 *
 * So the knob is inverted here, once, in the one place all three types read
 * it. 0 leaves the filter open and out of the way; 255 is as dark as it goes.
 * The floor of 1 is the same floor the raw form had: 0 would freeze the state
 * and hold the last sample forever. */
static inline int ret_dark_c(int p)
{
    int c = 255 - p;
    return c < 1 ? 1 : c;
}

/* The same one-pole as dsp.c's onepole() and expr.c's lp(): Q8 state, int64
 * multiply. The int32 version silently dies on loud material -- see the
 * comment in expr.c. `c` is 1..255, where 255 is fully open. */
static inline int32_t ret_lp(int64_t *st, int32_t x, int c)
{
    /* Unsigned shift for the usual reason: x is a signed sample, it is
     * negative half the time, and `<<` on a negative signed value is
     * undefined. Same bits, defined everywhere. */
    int64_t target = (int64_t)((uint64_t)(int64_t)x << 8);
    *st += ((target - *st) * (int64_t)c) >> 8;
    return (int32_t)(*st >> 8);
}

/* dsp.c's three-segment saturator, reproduced here because dsp.c's copy is
 * static and this file may not edit dsp.c. Same knee, same 3:1 ratio, same
 * final hard limit, so "saturate" means the same thing on a return that it
 * means on a voice -- which matters, because the user turns a knob called
 * DIRT here after learning one called DRIVE there.
 *
 * The /3 is a division by a compile-time constant, which the compiler turns
 * into a multiply; the hot-path rule is about dividing by a runtime value. */
#define RET_SAT_KNEE 18000

static inline int32_t ret_sat(int32_t x)
{
    if (x > RET_SAT_KNEE) {
        x = RET_SAT_KNEE + (x - RET_SAT_KNEE) / 3;
    } else if (x < -RET_SAT_KNEE) {
        x = -RET_SAT_KNEE + (x + RET_SAT_KNEE) / 3;
    }
    return dsp_clip16(x);
}

/* SYMMETRIC int16 clamp, for writes into a delay line that sits inside a
 * feedback loop.
 *
 * dsp_clip16() clamps to -32768..+32767, which is the honest int16 range and
 * exactly right on the master bus. Inside a feedback loop the two rails
 * differing by one LSB is a hazard: a signal driven hard enough to sit on both
 * of them has a mean of about -0.5 LSB per sample, and a delay loop's DC gain
 * is 1/(1-g), which at the 252/256 ceiling is 64x. That turns a rounding
 * detail into an offset.
 *
 * Measured honestly, it does not currently fire: ret_sat() compresses 3:1
 * above its knee, so reaching -32768 needs an input below -62,304 and the
 * loops here do not get there -- switching the delay and choir writes to this
 * function left a ten-second maximum-feedback runaway bit-identical. It is
 * kept because it is free, because it makes the "no DC from the loop"
 * statement true by construction rather than by measurement of one patch, and
 * because the code point it throws away is inaudible. */
static inline int32_t ret_clip16s(int32_t x)
{
    if (x >  32767) return  32767;
    if (x < -32767) return -32767;
    return x;
}

/* Crossfade, w = 0..256. Both inputs are already inside int16 range wherever
 * this is called, so the int64 is belt and braces rather than necessity. */
static inline int32_t ret_mix256(int32_t a, int32_t b, int w)
{
    return (int32_t)(((int64_t)a * (256 - w) + (int64_t)b * w) >> 8);
}

/* THE WAVEFOLDER.
 *
 * A triangle wave in the amplitude domain: instead of flattening everything
 * above the limit the way a clipper does, it reflects it back down, and then
 * reflects again, so a loud input comes out as a dense inharmonic mess rather
 * than a square. That is the difference between "distorted" and "destroyed".
 *
 * The limit is a power of two on purpose. Folding is naturally a modulo, and a
 * modulo of a negative number is a trap in C: the sign of the result is the
 * sign of the dividend, so the fold would be discontinuous through zero. Doing
 * the wrap as an AND on the UNSIGNED bit pattern sidesteps that entirely --
 * every step below is defined for every int32 input, including the conversion
 * of a negative x to uint32, which is modular and specified.
 *
 * Check it by hand: 0 -> 0, 8192 -> 8192, 16384 -> 0, 24576 -> -8192,
 * -8192 -> -8192. Continuous, period 32768, output always within +/-8192. */
int32_t ret_fold(int32_t x)
{
    uint32_t u = ((uint32_t)x + 8192u) & 32767u;
    int32_t  v = (int32_t)u - 8192;          /* -8192 .. 24575 */
    return v <= 8192 ? v : 16384 - v;        /* ->  -8192 .. 8192 */
}

/* THE SHAPER, and why it is one knob instead of a three-way menu.
 *
 * Saturation, hard clipping and folding are three different destructions and
 * you want to hear the boundary between them, not step over it. So SHAPE
 * sweeps: it is saturation at the bottom, hard clip in the middle, fold at the
 * top, and there is a crossfade window straddling each boundary so the knob
 * never switches, it always morphs. Sweepable beats discrete everywhere in
 * this instrument and this is no exception.
 *
 * The fold is scaled up by 4 on the way out. ret_fold() is bounded at +/-8192
 * by construction, which is 12 dB below what the other two shapers deliver,
 * and a SHAPE knob that gets quieter as you turn it reads as a bug rather than
 * as a sound. The multiply cannot overflow (|fold| <= 8192) and the result is
 * clipped back into range, so the top of the knob is as loud as the middle. */
#define RET_SHAPE_B1  85    /* saturate -> hard clip  */
#define RET_SHAPE_B2 170    /* hard clip -> wavefold  */
#define RET_SHAPE_XF  32    /* half-width of the crossfade, in knob steps */

static int32_t ret_shape(int32_t x, int shape)
{
    if (shape <= RET_SHAPE_B1 - RET_SHAPE_XF)
        return ret_sat(x);
    if (shape < RET_SHAPE_B1 + RET_SHAPE_XF) {
        int w = ((shape - (RET_SHAPE_B1 - RET_SHAPE_XF)) * 256)
              / (2 * RET_SHAPE_XF);
        return ret_mix256(ret_sat(x), dsp_clip16(x), w);
    }
    if (shape <= RET_SHAPE_B2 - RET_SHAPE_XF)
        return dsp_clip16(x);
    if (shape < RET_SHAPE_B2 + RET_SHAPE_XF) {
        int w = ((shape - (RET_SHAPE_B2 - RET_SHAPE_XF)) * 256)
              / (2 * RET_SHAPE_XF);
        return ret_mix256(dsp_clip16(x), dsp_clip16(ret_fold(x) * 4), w);
    }
    return dsp_clip16(ret_fold(x) * 4);
}

/* DIRT / drive, shaped exactly like dsp.c's DRIVE stage so the two feel the
 * same under the hand: (256 + n*8)/256 is unity at 0 and about 9x at 255,
 * which is enough to bury any signal in the knee and keep it there. */
static inline int32_t ret_dirt(int32_t x, int amount)
{
    int64_t g = 256 + (int64_t)amount * 8;
    return ret_sat((int32_t)(((int64_t)x * g) >> 8));
}

/* The ten tempo divisions, identical to the table engine.c's space_samples()
 * uses, so `sync` means the same thing on a return that it means on a voice
 * insert. Duplicated rather than shared because space_samples() is static in
 * engine.c and clamps to the SPACE line's length, not to ours. */
static uint32_t ret_sync_samples(int sync, uint32_t beat_len, uint32_t bar_len)
{
    uint64_t smp;
    switch (sync) {
    case 1:  smp = beat_len / 8u; break;
    case 2:  smp = beat_len / 6u; break;
    case 3:  smp = beat_len / 4u; break;
    case 4:  smp = beat_len / 3u; break;
    case 5:  smp = beat_len / 2u; break;
    case 6:  smp = (uint64_t)beat_len * 2u / 3u; break;
    case 7:  smp = beat_len; break;
    case 8:  smp = (uint64_t)beat_len * 2u; break;
    case 9:  smp = bar_len; break;
    case 10: smp = (uint64_t)bar_len * 2u; break;
    default: smp = 0; break;
    }
    if (smp < 1) smp = 1;
    if (smp > RET_DL_MASK) smp = RET_DL_MASK;
    return (uint32_t)smp;
}

/* ========================================================================= */
/*  THE CHAMBER  (type 1)                                                    */
/* ========================================================================= */

/* Eight parallel damped feedback combs into four serial allpasses -- the
 * classic Schroeder/Freeverb topology, with a one-pole inside each comb loop
 * so every reflection is darker than the last (same reasoning as SPACE's
 * filter-in-the-loop). The tunings are the classic 44.1k prime-ish lengths,
 * rescaled to the device rate once per period by ret_period(). */
static const int VERB_CTUNE[VERB_NCOMB] =
    { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
static const int VERB_ATUNE[VERB_NAP] = { 556, 441, 341, 225 };

/* The comb block and the allpass block are carved out of one contiguous
 * region per slot. Each line stays contiguous and stays exactly 4096 or 2048
 * long, so every index expression inside the loops below is arithmetically
 * identical to the one it replaced when this was five file statics. */
#define VB_COMB(vb, c) ((vb) + (c) * VERB_CSIZE)
#define VB_AP(vb, a)   ((vb) + VERB_NCOMB * VERB_CSIZE + (a) * VERB_ASIZE)

/* ONE FRAME IN, ONE WET FRAME OUT. fb/damp are Q15; clen/alen are the
 * rate-scaled line lengths for this period.
 *
 * NOT ONE LINE INSIDE THE COMB OR ALLPASS LOOP MAY CHANGE. This is
 * engine.c's verb_process() verbatim, only reindexed. The `in >>= 2` for
 * headroom, the `out >>= 3` back to unit gain and the allpass coefficient
 * spelled as `b >> 1` are all load-bearing: a session with one CHAMBER has to
 * render bit-identically to the engine that predated the return bus, the
 * suite pins that with a hash of 4096 rendered frames, and 3,002 existing
 * checks sit downstream of it. If you are tempted to "fix" a signed shift
 * here, don't -- fix it somewhere the output is allowed to move. */
int32_t ret_chamber_process(int slot, int32_t in, int32_t fb, int32_t damp,
                            const int *clen, const int *alen)
{
    RetState *st = &g_ret_st[slot];
    int32_t  *vb = g_ret_vb[slot];
    int32_t   out = 0;
    int       c, a;

    in >>= 2;                    /* headroom: eight combs sum below */

    for (c = 0; c < VERB_NCOMB; c++) {
        int32_t *line = VB_COMB(vb, c);
        uint32_t p = st->vcpos[c];
        int32_t  y = line[p];
        st->vdamp[c] += (int32_t)(((int64_t)(y - st->vdamp[c]) * damp) >> 15);
        line[p] = in + (int32_t)(((int64_t)st->vdamp[c] * fb) >> 15);
        if (++p >= (uint32_t)clen[c]) p = 0;
        st->vcpos[c] = p;
        out += y;
    }
    out >>= 3;                   /* back to unit-ish gain */

    for (a = 0; a < VERB_NAP; a++) {
        int32_t *line = VB_AP(vb, a);
        uint32_t p = st->vapos[a];
        int32_t  b = line[p];
        line[p] = out + (b >> 1);
        out = b - out;
        if (++p >= (uint32_t)alen[a]) p = 0;
        st->vapos[a] = p;
    }
    return out;
}

/* The predelay is the one thing the CHAMBER gained, and it is guarded so hard
 * that at PRE = 0 not a single memory access happens and the arithmetic is
 * byte for byte what it was. That is not politeness, it is the compatibility
 * requirement: PRE defaults to 0 and old sessions carry no value for it.
 *
 * Why it is worth having at all: a reverb with no gap between the dry hit and
 * the onset of the wash reads as a blanket thrown over the source. A few tens
 * of milliseconds of silence first and the same reverb reads as a large room,
 * because that gap IS the room's size to the ear. */
static int32_t ret_chamber_run(int slot, const RetSnap *s, int32_t in)
{
    RetState *st = &g_ret_st[slot];
    int32_t   x  = in;

    if (s->vpre) {
        int16_t *dl = g_ret_dl[slot];
        uint32_t w  = st->prew;
        int32_t  d  = dl[(w - (uint32_t)s->vpre) & RET_PRE_MASK];
        dl[w] = (int16_t)ret_clip16s(x);
        st->prew = (w + 1u) & RET_PRE_MASK;
        x = d;
    }

    /* HOLD freezes the room: feedback goes to just under unity and the input
     * stops being admitted, so the tail that was in the combs when you hit it
     * circulates until you let go. Resolved per period into vfb -- the only
     * per-sample cost is this compare. */
    if (s->p[3] > 127) x = 0;

    return ret_chamber_process(slot, x, s->vfb, s->vdamp_c, s->vclen, s->valen);
}

/* ========================================================================= */
/*  THE DELAY  (type 2)                                                      */
/* ========================================================================= */

/* SPACE's shape at return scale: one tap, feedback with a damper and a
 * saturator inside the loop. Everything that makes a delay musical happens
 * inside that loop rather than after it -- each repeat is darker than the last
 * and each repeat is a little more destroyed, which is what a tape machine
 * does and what a clean digital delay conspicuously does not.
 *
 * FEED is capped at 252/256 rather than dsp.c's 244/256. The extra 8 steps are
 * safe here for a reason that does not apply to a voice insert: the write into
 * the line goes through ret_sat() and then dsp_clip16() into int16, so the
 * loop's fixed point is bounded by the saturator no matter what the user does,
 * and the return bus's limiter sits downstream of that as well. */
static int32_t ret_delay_run(int slot, const RetSnap *s, int32_t in)
{
    RetState *st = &g_ret_st[slot];
    int16_t  *dl = g_ret_dl[slot];
    uint32_t  w  = st->dw;
    int       hold = s->p[5] > 127;
    int32_t   wet, d, into;

    wet = dl[(w - (uint32_t)s->dtap) & RET_DL_MASK];

    /* SPREAD is a second tap at two thirds of the time, half level. Two taps
     * in a non-integer ratio stop the repeats lining up into a single pulse
     * train and turn the delay into something closer to a small room. At 0 the
     * tap is resolved to 0 by ret_period() and this costs one compare. */
    if (s->dtap2)
        wet += dl[(w - (uint32_t)s->dtap2) & RET_DL_MASK] >> 1;

    /* The damper runs unconditionally rather than being skipped when DARK is
     * fully open. Skipping it would save one multiply and buy a filter state
     * that goes stale inside a live feedback loop and clicks when the knob
     * comes back down. */
    d = ret_lp(&st->dtone, wet, ret_dark_c(s->p[2]));

    if (s->p[4]) d = ret_dirt(d, s->p[4]);

    /* HOLD closes the loop at unity and stops admitting input, so the exact
     * phrase in the line circulates until it is released. Same semantics as
     * SPACE's FREEZE, and bounded for the same reason: what is already in the
     * line is int16, and recirculating it at unity cannot make it bigger. */
    if (hold) {
        into = wet;
    } else {
        int fb = s->p[1] > 252 ? 252 : s->p[1];
        into = ret_sat(in + (int32_t)(((int64_t)d * fb) >> 8));
    }

    dl[w] = (int16_t)ret_clip16s(into);
    st->dw = (w + 1u) & RET_DL_MASK;

    /* RAW OUT -- NO DC BLOCKER HERE, and that is a correction, not an
     * omission. There used to be one, on the grounds that a long feedback loop
     * is an integrator for DC. The loop is; the blocker was in the wrong place.
     *
     * ret_run()'s contract is one raw wet frame out, with level, fade, limiter
     * and peak all belonging to the bus. A DC blocker belongs there too, and it
     * matters WHICH copy it lands on. The bus feeds a return's output back
     * through the link matrix, so a filter applied here is a filter inside
     * whatever loop the user builds -- and a highpass inside a loop whose gain
     * has been ridden to unity is a negative feedback path with a 1,800-sample
     * lag in it. That is not a stability curiosity, it is an oscillator:
     * measured, a self-linked DELAY abandoned its 50 Hz repeat entirely and
     * settled into a full-scale wander at a few Hertz, which is silent, pins
     * the limiter, and is the worst thing you can hand a speaker.
     *
     * So the bus blocks DC on the copy you HEAR and feeds back the copy it has
     * BOUNDED. This type is left with no filter in the loop at all, and the
     * only DC it can integrate is what arrives at its input -- which comes
     * either from voices, already blocked one stage upstream, or from a link,
     * which is precisely the case that arms the bus's own blocker. */
    return wet;
}

/* ========================================================================= */
/*  THE DRIVE  (type 3)                                                      */
/* ========================================================================= */

/* Zero memory, five multiplies, and the thing you actually want sitting inside
 * a feedback loop. Parallel distortion -- a clean signal with a destroyed copy
 * of itself underneath it -- is a different sound from distortion in series,
 * and the instrument could not make it at all before this.
 *
 * p5 FEED is the reason there is no separate FEEDBACK effect type. A bare
 * self-link on the routing matrix is a one-sample comb, i.e. a fairly boring
 * high shelf. Feedback taken INSIDE a bounded nonlinearity is where the
 * screaming lives, and because the shaper's output is bounded for any input
 * whatsoever, that loop is stable at every setting by construction rather than
 * by a stability argument. */
static int32_t ret_drive_run(int slot, const RetSnap *s, int32_t in)
{
    RetState *st = &g_ret_st[slot];
    int32_t   x;

    /* HEAT: 1x to about 25x. Far more than dsp.c's DRIVE, because a return is
     * fed a send that may be 20 dB down and still has to be able to melt. */
    x = (int32_t)(((int64_t)in * (256 + (int64_t)s->p[0] * 24)) >> 8);

    /* BITE is pre-emphasis: a highpass BEFORE the shaper, so the top end is
     * what gets destroyed and the low end survives to carry weight. Without it
     * a hot drive on a full-range signal is mud, because the bass eats all the
     * gain and the harmonics it generates sit on top of everything.
     *
     * The coefficient is capped well below 256 on purpose: a one-pole that
     * tracks its input exactly outputs silence, and a knob whose top end is
     * silence is a bug, not a sound. */
    if (s->p[4]) {
        int c = 1 + (s->p[4] * 3) / 4;               /* 1..192, Q8 */
        st->hhp += (int32_t)(((int64_t)(x - st->hhp) * c) >> 8);
        x -= st->hhp;
    }

    /* BIAS pushes the waveform off centre so the shaper treats the two halves
     * differently, which is what generates even harmonics and what the ear
     * hears as growl rather than fizz.
     *
     * IT IS ZERO AT ZERO, and it used to be centred at 128 instead. That
     * looked like a bipolar control and cost nothing musically -- a symmetric
     * shaper turns +B and -B into the same harmonics with the even ones
     * inverted, so half the knob was a polarity switch nobody can hear. What
     * it cost was the invariant everything else in this file keeps: a slot
     * that has never held an effect carries all zeros, and under a centred
     * BIAS those zeros meant the offset slammed hard negative. A brand new
     * DRIVE therefore came up making a constant out of nothing and, once the
     * blocker downstream had learned that constant, took a quarter of a second
     * to give it back every time the input stopped. A waveshaper with no delay
     * line in it has no business having a tail at all.
     *
     * The reach is unchanged -- BIAS at 255 pushes as far off centre as the
     * old knob's ends did -- it just does it across the whole travel and
     * starts from centre. */
    if (s->p[1])
        x += (int32_t)((int64_t)s->p[1] * 48);

    if (s->p[5])
        x += (int32_t)(((int64_t)st->hprev * s->p[5]) >> 8);

    x = ret_shape(x, s->p[2]);
    st->hprev = x;

    /* AND THE BIAS COMES STRAIGHT BACK OUT, which is what makes the knob an
     * asymmetry control rather than a DC generator.
     *
     * Offsetting a waveform before a nonlinearity is the whole mechanism: the
     * shaper then treats the two halves differently and even harmonics appear.
     * None of that needs the offset ITSELF to survive to the output, and it
     * must not, because shape(bias) is a constant -- with no input at all a
     * biased DRIVE would otherwise emit that constant forever. That is DC, on
     * a return, with the bus's safety blocker disarmed because this slot has
     * no incoming feedback edge.
     *
     * Leaving it to the DC blocker below does not work and it is worth saying
     * why, because it looks like it should. That filter's corner is near 4 Hz
     * -- a time constant of about 1,800 samples, a quarter of a second at
     * 8 kHz -- so a step of several thousand counts is still thousands of
     * counts of offset a second later. A blocker is the right tool for DC that
     * ARRIVES; it is the wrong tool for DC you generated on purpose and know
     * the exact value of. Subtracting the value you know is exact, is free,
     * and leaves the blocker to do the job it is actually good at.
     *
     * hprev deliberately stores the value BEFORE the subtraction: FEED is a
     * loop around the shaper, and the thing that goes back round is what the
     * shaper produced. ret_period() solves the same recursion for its fixed
     * point, so silence in still means exactly silence out with FEED up. */
    x -= s->hbias;

    /* DARK, post. Without it the fold is all fizz: folding generates harmonics
     * far above anything musical in the source and they need somewhere to go. */
    x = ret_lp(&st->htone, x, ret_dark_c(s->p[3]));

    /* THE BLOCKER RUNS WHEN BIAS DOES, and that pairing is the whole rule.
     *
     * BIAS is the only thing in this stage that can make an offset. With it at
     * zero every shaper on the SHAPE knob -- saturate, hard clip, wavefold --
     * is an ODD function, and an odd function of the zero-mean signal a
     * DC-blocked voice bus delivers has zero mean too. There is nothing for a
     * blocker to remove. Push BIAS off centre and that stops being true in a
     * way no closed form covers: the constant part comes out exactly at the
     * subtraction above, and what is left is program-dependent offset, which
     * is what this filter is actually good at.
     *
     * Running it anyway, on a stage that has no offset, is not free and not
     * harmless. Its corner sits at 0.7 Hz at the 8 kHz the suite runs at, and
     * a highpass whose corner is within an octave or two of the programme
     * material does not remove DC from it -- it STORES the low end and hands
     * it back afterwards. Measured, on a signal whose fundamental is 4.5 Hz:
     * ten percent of full scale still coming out a second after the input
     * stopped, on an effect whose entire description is that it has no delay
     * line in it. A DRIVE that groans for a second after you close the send is
     * a bug report, and the filter causing it was not removing anything.
     *
     * So: DRIVE is memoryless when its knobs say it should be, and carries the
     * blocker exactly when it has earned one. The bus's own safety blocker is
     * still there underneath for the case this cannot see, a feedback edge
     * arriving from another return. */
    if (s->p[1]) x = dsp_dc(&st->dc, x);
    return x;
}

/* ========================================================================= */
/*  THE CHOIR  (type 4)                                                      */
/* ========================================================================= */

/* Four tuned resonator combs on quarters of the slot's delay line.
 *
 * This is the type that answers the choral half of what this instrument is
 * for. Nothing else in MORGUE makes noise PITCHED. A comb with its feedback up
 * near unity is a resonator: feed it broadband noise and it hands back the one
 * frequency whose period matches its length, plus that frequency's harmonics.
 * Four of them tuned to a ratio set turns a wall of static into a chord. Send
 * that into a CHAMBER and it is a cathedral, and the whole thing costs eight
 * multiplies and no new memory.
 *
 * 256 log-spaced periods in samples at 44.1 kHz, from 8 (about 5.5 kHz) up to
 * 8820 (5 Hz). A TABLE rather than an approximation, because 512 bytes is
 * nothing, because it is exact on every platform -- which a computed
 * logarithm would not be -- and because equal ratios per knob step is what
 * makes the knob feel musical instead of bunched up at one end. The short end
 * repeats a few values; that is inherent to quantising a log sweep onto
 * integer sample counts and it is inaudible at those periods. */
static const uint16_t RET_PERIOD[256] = {
        8,     8,     8,     9,     9,     9,     9,    10,
       10,    10,    11,    11,    11,    11,    12,    12,
       12,    13,    13,    13,    14,    14,    15,    15,
       15,    16,    16,    17,    17,    18,    18,    19,
       19,    20,    20,    21,    22,    22,    23,    23,
       24,    25,    25,    26,    27,    28,    28,    29,
       30,    31,    32,    32,    33,    34,    35,    36,
       37,    38,    39,    40,    42,    43,    44,    45,
       46,    48,    49,    50,    52,    53,    55,    56,
       58,    59,    61,    63,    65,    66,    68,    70,
       72,    74,    76,    78,    80,    83,    85,    87,
       90,    92,    95,    97,   100,   103,   106,   109,
      112,   115,   118,   121,   125,   128,   132,   136,
      139,   143,   147,   151,   155,   160,   164,   169,
      174,   178,   183,   188,   194,   199,   205,   210,
      216,   222,   228,   235,   241,   248,   255,   262,
      269,   277,   285,   292,   301,   309,   318,   326,
      335,   345,   354,   364,   374,   385,   396,   407,
      418,   430,   442,   454,   467,   480,   493,   507,
      521,   535,   550,   565,   581,   597,   614,   631,
      649,   667,   685,   704,   724,   744,   765,   786,
      808,   831,   854,   878,   902,   927,   953,   979,
     1007,  1035,  1064,  1093,  1124,  1155,  1187,  1220,
     1254,  1289,  1325,  1362,  1400,  1439,  1479,  1520,
     1563,  1606,  1651,  1697,  1744,  1793,  1842,  1894,
     1947,  2001,  2057,  2114,  2173,  2233,  2295,  2359,
     2425,  2493,  2562,  2633,  2707,  2782,  2860,  2939,
     3021,  3105,  3192,  3281,  3372,  3466,  3562,  3662,
     3764,  3868,  3976,  4087,  4201,  4318,  4438,  4562,
     4689,  4819,  4954,  5092,  5233,  5379,  5529,  5683,
     5841,  6004,  6171,  6343,  6520,  6701,  6888,  7080,
     7277,  7480,  7688,  7902,  8122,  8348,  8581,  8820,
};

/* Two frequency-ratio sets in Q8, blended by ODD.
 *
 * PIPE is the harmonic series 1:2:3:4 -- an organ rank, a vowel, something the
 * ear fuses into one pitched tone. BELL is deliberately inharmonic; those
 * ratios are close to the partials of a struck metal bar, which the ear
 * refuses to fuse and hears as a clang with a pitch centre. Sweeping between
 * them takes the same wall of noise from angelic to industrial without
 * touching anything else. */
static const int RET_RATIO_PIPE[4] = { 256, 512, 768, 1024 };
static const int RET_RATIO_BELL[4] = { 256, 384, 683,  998 };

static int32_t ret_choir_run(int slot, const RetSnap *s, int32_t in)
{
    RetState *st = &g_ret_st[slot];
    int16_t  *dl = g_ret_dl[slot];
    uint32_t  w  = st->qw;
    int       hold = s->p[5] > 127;
    int       damp = ret_dark_c(s->p[2]);
    int32_t   fb, out = 0;
    int       k;

    /* Resonance, Q15, capped at 32100/32768 = 0.980. Below unity in code, the
     * way dsp.c caps SPACE and the way the CHAMBER's own feedback tops out --
     * the type owes the bus internal stability at every setting, because a
     * type that self-oscillates from silence has no incoming link and so gets
     * no bus guard at all. p1 * 126 reaches 32130, which the cap trims. */
    fb = s->p[1] * 126;
    if (fb > 32100) fb = 32100;

    for (k = 0; k < s->qn; k++) {
        int16_t *q = dl + (size_t)k * RET_Q_LEN;
        int32_t  y = q[(w - (uint32_t)s->qlen[k]) & RET_Q_MASK];
        int32_t  d = ret_lp(&st->qdamp[k], y, damp);
        int32_t  into = hold ? d
                             : (in + (int32_t)(((int64_t)d * fb) >> 15));
        q[w & RET_Q_MASK] = (int16_t)ret_clip16s(into);
        out += y;
    }
    st->qw = (w + 1u) & RET_Q_MASK;

    /* Average, not sum. At SPREAD 0 the siblings collapse onto the fundamental
     * and only one comb runs, so dividing by the number that ran is what makes
     * the transition out of unison continuous in level instead of a 12 dB
     * step. qn is 1 or 4, both powers of two, so this is a shift either way --
     * and out is a sum of int16-range values, so the shift is on a value that
     * cannot have overflowed. */
    if (s->qn == 4) out >>= 2;

    /* Raw out, for exactly the reason the DELAY is: four resonators tuned near
     * unity are four feedback loops, and a highpass inside a feedback loop is
     * an oscillator waiting for the gain to reach one. The bus blocks DC on
     * the audible copy and feeds back the bounded one. */
    return out;
}

/* ========================================================================= */
/*  DISPATCH                                                                 */
/* ========================================================================= */

/* A SWITCH, AND IT MUST STAY A SWITCH. Session files carry raw type ids, and a
 * session written by a future build that has GATE in slot 3 must load here,
 * render as silence, and be written back out with that id intact. That means
 * an out-of-range id reaches this function every period of that session's
 * life. `default: return 0;` makes it silence; a function-pointer table indexed
 * by the same value would make it an out-of-bounds jump. Treat any table-based
 * dispatch added here as a defect, not a refactor. */
int32_t ret_run(int slot, const RetSnap *s, int32_t in)
{
    if (slot < 0 || slot >= BB_NRET) return 0;

    /* ONE SANITY CLAMP ON THE WAY IN, and it is what makes every arithmetic
     * claim below it provable rather than merely likely.
     *
     * `in` is a sum: up to eleven send sources plus up to eight link
     * contributions, each bounded near full scale, so the largest value the
     * bus can actually hand over is on the order of 600,000. DRIVE then
     * multiplies by up to 25x. 1<<24 is about 512x full scale -- two orders of
     * magnitude above anything the bus can produce, so no musical signal ever
     * touches this -- and it guarantees that HEAT's 25x, BITE's difference and
     * BIAS's offset all stay inside int32 by a wide margin.
     *
     * It cannot affect the CHAMBER's bit-exactness: a legacy session's send
     * sum is nine post-fader voices, at most ~295,000, which is fifty times
     * below this clamp. It never fires there and the arithmetic is unchanged. */
    if (in >  RET_IN_MAX) in =  RET_IN_MAX;
    if (in < -RET_IN_MAX) in = -RET_IN_MAX;

    switch (s->type) {
    case RET_CHAMBER: return ret_chamber_run(slot, s, in);
    case RET_DELAY:   return ret_delay_run  (slot, s, in);
    case RET_DRIVE:   return ret_drive_run  (slot, s, in);
    case RET_CHOIR:   return ret_choir_run  (slot, s, in);
    default:          return 0;
    }
}

/* ========================================================================= */
/*  PER-PERIOD RESOLUTION                                                    */
/* ========================================================================= */

/* Everything rate-dependent, tempo-dependent or otherwise expensive is
 * resolved here, once per period, so that ret_run() is pure arithmetic on
 * values it was handed. Every division in this file lives in this function.
 * engine.c's space_samples() and its old per-period chamber block are the
 * model; this is the same idea generalised to eight slots. */
void ret_period(int slot, RetSnap *s, int rate,
                uint32_t beat_len, uint32_t bar_len)
{
    int r44, i;

    (void)slot;   /* accepted for symmetry; no type needs per-slot geometry yet */

    /* Clamp on the way in as well as at the engine's snapshot. The engine
     * clamps because a UI control can be dragged anywhere; this clamps because
     * a session file is plain text that someone will hand-edit, and a
     * hand-edited SIZE of 400 must not index off the end of a tuning table. */
    s->level = ret_clampi(s->level, 0, 256);
    s->sync  = ret_clampi(s->sync,  0, 10);
    for (i = 0; i < BB_RET_NPARAM; i++)
        s->p[i] = ret_clampi(s->p[i], 0, 255);

    r44 = rate < RET_RATE_MIN ? 44100 : rate;
    s->rate = r44;

    /* Cleared for every type, so a slot that changes type cannot inherit a
     * live tap length from what it used to be. */
    s->vpre = s->dtap = s->dtap2 = 0;
    s->qn   = 0;
    s->hbias = 0;

    switch (s->type) {

    case RET_CHAMBER: {
        int64_t pre;
        /* Byte for byte the computation engine.c did per period, including the
         * clamp bounds. This is the arithmetic the golden hash pins. */
        for (i = 0; i < VERB_NCOMB; i++)
            s->vclen[i] = ret_clampi(
                (int)(((int64_t)VERB_CTUNE[i] * r44) / 44100), 8, VERB_CSIZE);
        for (i = 0; i < VERB_NAP; i++)
            s->valen[i] = ret_clampi(
                (int)(((int64_t)VERB_ATUNE[i] * r44) / 44100), 8, VERB_ASIZE);

        s->vfb     = 22000 + s->p[0] * 40;    /* Q15: .67 .. .983            */
        s->vdamp_c = 4096  + s->p[1] * 112;   /* loop lowpass: dark..bright  */

        /* HOLD takes the feedback to just under unity. Not TO unity: at unity
         * the loop gain never decays and the combs walk to the rails, which is
         * the same reason SPACE's feedback is capped. 32700/32768 rings for
         * minutes and still, eventually, dies. */
        if (s->p[3] > 127) s->vfb = 32700;

        /* PRE: 0..500 ms. Zero stays exactly zero so the whole predelay stage
         * is skipped, which is what keeps an old session bit-exact. */
        pre = ((int64_t)s->p[2] * 500 * r44) / (255 * 1000);
        if (pre > (int64_t)RET_PRE_MASK) pre = (int64_t)RET_PRE_MASK;
        s->vpre = (int)pre;
        break;
    }

    case RET_DELAY: {
        uint32_t tap;
        if (s->sync) {
            tap = ret_sync_samples(s->sync, beat_len, bar_len);
        } else {
            /* 20 + raw*20 ms, i.e. 20 ms to 5.12 s. Deliberately NOT
             * space_samples()' 20 + raw*3 ms: a voice insert wants
             * milliseconds so it can be tuned into a comb filter, a return
             * delay wants seconds so it can be an echo you hear as an event.
             * 5.12 s at 96 kHz is 491,520 samples, inside the line. */
            int64_t ms  = 20 + (int64_t)s->p[0] * 20;
            int64_t smp = (ms * r44) / 1000;
            if (smp > (int64_t)RET_DL_MASK) smp = (int64_t)RET_DL_MASK;
            tap = (uint32_t)(smp < 1 ? 1 : smp);
        }
        s->dtap = (int)tap;
        /* SPREAD's second tap sits at two thirds of the time. Two thirds
         * rather than a half because a half lands the two taps on the same
         * grid and just sounds like one delay twice as fast. */
        if (s->p[3]) {
            uint32_t t2 = tap * 2u / 3u;
            s->dtap2 = (int)(t2 < 1u ? 1u : t2);
        }
        break;
    }

    case RET_CHOIR: {
        /* PITCH RISES AS THE KNOB RISES, which needed the table read backwards.
         *
         * RET_PERIOD is a table of PERIODS -- entry 0 is eight samples and
         * entry 255 is 8,820 -- and period is the reciprocal of pitch, so
         * indexing it with the raw knob made PITCH turn the wrong way: up the
         * knob went, down the note went. That is worth fixing on the label
         * alone, and it fixes something else with it. At the bottom of the
         * knob the comb was EIGHT samples long, which at any rate this engine
         * runs at rescales to the four-sample floor -- a comb too short to
         * hold anything, so a CHOIR whose resonance knob was also down was not
         * a resonator at all, just a wire with a couple of samples of delay in
         * it. The other end of the same table is a comb long enough to be an
         * effect no matter where the other knobs sit, which is what a slot
         * that has never been touched should come up as. */
        int64_t base = ((int64_t)RET_PERIOD[255 - s->p[0]] * r44) / 44100;
        int spread = s->p[3], odd = s->p[4];
        if (base < 4) base = 4;
        if (base > (int64_t)RET_Q_MASK) base = (int64_t)RET_Q_MASK;

        for (i = 0; i < 4; i++) {
            /* Blend the two ratio sets, then blend the result back toward
             * unison by SPREAD. At SPREAD 0 every ratio is exactly 256, all
             * four combs are the same length, and there is no reason to run
             * more than one of them. */
            int set = RET_RATIO_PIPE[i]
                    + ((RET_RATIO_BELL[i] - RET_RATIO_PIPE[i]) * odd) / 255;
            int ratio = 256 + ((set - 256) * spread) / 255;
            int64_t len;
            if (ratio < 1) ratio = 1;
            /* Length is period divided by frequency ratio -- the runtime
             * division that would be illegal one function down the call
             * stack, which is precisely why it is up here. */
            len = (base * 256) / ratio;
            if (len < 4) len = 4;
            if (len > (int64_t)RET_Q_MASK) len = (int64_t)RET_Q_MASK;
            s->qlen[i] = (int)len;
        }
        s->qn = spread ? 4 : 1;
        break;
    }

    case RET_DRIVE: {
        /* DRIVE has no line geometry. What it does have is one constant worth
         * solving for up here: what the shaper emits when the input is zero.
         *
         * With no FEED that is just shape(bias). With FEED it is the fixed
         * point of v = shape(bias + v*p5/256), because the loop is closed
         * around the shaper, and the iteration below is the honest way to find
         * it -- the shaper is three different functions with two crossfades
         * between them and has no inverse worth writing down. It converges
         * because the loop gain p5/256 is strictly under one and the shaper is
         * bounded; 64 rounds is far more than the settings anyone uses need,
         * and the early-out means the common case (FEED at zero) costs one.
         *
         * This is exactly the kind of work ret_period() exists for: a division
         * and a loop, once per period, so that ret_run() is left with a single
         * subtraction. */
        int32_t bias = (int32_t)((int64_t)s->p[1] * 48);
        int32_t v = ret_shape(bias, s->p[2]);
        if (s->p[5]) {
            for (i = 0; i < 64; i++) {
                int32_t nv = ret_shape(bias + (int32_t)(((int64_t)v * s->p[5]) >> 8),
                                       s->p[2]);
                if (nv == v) break;
                v = nv;
            }
        }
        s->hbias = v;
        break;
    }

    default:
        /* An unknown id renders as silence, so there is nothing to resolve. */
        break;
    }
}

/* ========================================================================= */
/*  LIFECYCLE                                                                */
/* ========================================================================= */

unsigned ret_footprint_dl(int type, int rate)
{
    (void)rate;   /* the windows are power-of-two and rate-independent */
    switch (type) {
    case RET_CHAMBER: return RET_PRE_LEN;   /* only the predelay window   */
    case RET_DELAY:   return RET_DL_LEN;    /* the whole line             */
    case RET_CHOIR:   return RET_DL_LEN;    /* four quarters of it        */
    default:          return 0;             /* DRIVE, NONE, unknown ids   */
    }
}

unsigned ret_footprint_vb(int type)
{
    return type == RET_CHAMBER ? (unsigned)RET_VB_WORDS : 0u;
}

/* Each per-effect reset zeroes the WHOLE per-slot scratch, not just its own
 * fields. Only one type is ever live in a slot, the scratch is under 512
 * bytes, and a partial clear is how a stale filter state from the previous
 * effect survives a type change and turns up as a click on the first sample
 * of the next one. */
static void ret_clear_slot_state(int slot)
{
    memset(&g_ret_st[slot], 0, sizeof g_ret_st[slot]);
}

/* The one place either pool is cleared for a single slot.
 *
 * Every reset below routes through here so that the arena tracker is updated
 * on every path there is. A second memset added somewhere else "just for this
 * one case" is how the tracker starts lying, and a tracker that lies is worse
 * than no tracker at all, because the residue it leaves is silent.
 *
 * The clear spans the union of what the outgoing type dirtied and what the
 * incoming type will read, which is at most the larger of the two footprints
 * -- so the demand-zero argument in ret.h survives intact: a slot that has
 * only ever held a CHAMBER still touches 160 KB + 256 KB and never the
 * megabyte behind it. */
static void ret_prepare_arena(int slot, int type)
{
    /* The rate argument is ignored by both helpers -- every window in the
     * pools is a power of two chosen to cover the longest reach at any device
     * rate -- so there is nothing sensible to pass and nothing that could be
     * got wrong by passing it. */
    int was = g_ret_arena[slot];
    unsigned dl = ret_footprint_dl(was,  0), dl2 = ret_footprint_dl(type, 0);
    unsigned vb = ret_footprint_vb(was),     vb2 = ret_footprint_vb(type);

    if (dl2 > dl) dl = dl2;
    if (vb2 > vb) vb = vb2;

    ret_clear_slot_state(slot);
    if (dl) memset(g_ret_dl[slot], 0, (size_t)dl * sizeof g_ret_dl[slot][0]);
    if (vb) memset(g_ret_vb[slot], 0, (size_t)vb * sizeof g_ret_vb[slot][0]);

    g_ret_arena[slot] = type;
}

void ret_chamber_reset(int slot)
{
    if (slot < 0 || slot >= BB_NRET) return;
    ret_prepare_arena(slot, RET_CHAMBER);
}

void ret_delay_reset(int slot)
{
    if (slot < 0 || slot >= BB_NRET) return;
    ret_prepare_arena(slot, RET_DELAY);
}

void ret_drive_reset(int slot)
{
    if (slot < 0 || slot >= BB_NRET) return;
    ret_prepare_arena(slot, RET_DRIVE);   /* no lines of its own */
}

void ret_choir_reset(int slot)
{
    if (slot < 0 || slot >= BB_NRET) return;
    ret_prepare_arena(slot, RET_CHOIR);
}

void ret_reset(int slot, int type)
{
    if (slot < 0 || slot >= BB_NRET) return;
    switch (type) {
    case RET_CHAMBER: ret_chamber_reset(slot); break;
    case RET_DELAY:   ret_delay_reset(slot);   break;
    case RET_DRIVE:   ret_drive_reset(slot);   break;
    case RET_CHOIR:   ret_choir_reset(slot);   break;
    /* RET_NONE and any id from a newer build. Both render as silence, so
     * neither will read the arena -- but the OUTGOING type's residue still
     * has to go, or destroying a return leaves a megabyte of its tail behind
     * for whatever is created here next. */
    default:          ret_prepare_arena(slot, RET_NONE); break;
    }
}

void ret_init(void)
{
    /* The scratch only. The pools are BSS and already zero on the first run,
     * and on every run after that a slot is cleared when its type changes.
     * Clearing 9.25 MiB here would fault in every page of a session that uses
     * one return, and the regression suite calls bb_engine_init() seven
     * times. This omission is the whole memory argument; do not "fix" it. */
    memset(g_ret_st, 0, sizeof g_ret_st);
}

void ret_reset_all(void)
{
    /* The honest full reset, pools included, for tests that need a provably
     * pristine machine. NOT for bb_engine_init() -- see ret.h. */
    memset(g_ret_st, 0, sizeof g_ret_st);
    memset(g_ret_dl, 0, sizeof g_ret_dl);
    memset(g_ret_vb, 0, sizeof g_ret_vb);
    /* Every arena is now provably clean, so the tracker may honestly say so.
     * This is the only place it is allowed to be cleared without a memset
     * having actually happened first. */
    for (int r = 0; r < BB_NRET; r++) g_ret_arena[r] = RET_NONE;
}

/* ========================================================================= */
/*  METADATA                                                                 */
/* ========================================================================= */

const char * const ret_type_name[RET_NTYPE] = {
    "---", "CHAMBER", "DELAY", "DRIVE", "CHOIR"
};

/* Six words, and they mean the same thing on every type. That is worth more
 * than a precise name per knob: a user learns DARK / FEED / HOLD / SPREAD once
 * and can drive an effect they have never opened before. NULL means the knob
 * is unused and the UI greys it. */
const char * const ret_param_name[RET_NTYPE][BB_RET_NPARAM] = {
    /* RET_NONE    */ { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL },
    /* RET_CHAMBER */ { "SIZE", "DARK", "PRE",  "HOLD",   NULL,   NULL,   NULL, NULL },
    /* RET_DELAY   */ { "TIME", "FEED", "DARK", "SPREAD", "DIRT", "HOLD", NULL, NULL },
    /* RET_DRIVE   */ { "HEAT", "BIAS", "SHAPE","DARK",   "BITE", "FEED", NULL, NULL },
    /* RET_CHOIR   */ { "PITCH","FEED", "DARK", "SPREAD", "ODD",  "HOLD", NULL, NULL },
};

/* Defaults are chosen so that every type is audible and characteristic the
 * instant it is created. An effect whose defaults are inaudible reads as
 * broken, and the user's first act should be shaping a sound rather than
 * hunting for one.
 *
 * CHAMBER's are today's engine defaults exactly -- SIZE 172, DARK 96 -- and
 * PRE and HOLD are 0 because a nonzero default for either would change what an
 * old session sounds like. */
const unsigned char ret_param_def[RET_NTYPE][BB_RET_NPARAM] = {
    /* RET_NONE    */ {   0,   0,   0,   0,   0,   0, 0, 0 },
    /* RET_CHAMBER */ { 172,  96,   0,   0,   0,   0, 0, 0 },
    /* RET_DELAY   */ { 128, 190, 110,   0,   0,   0, 0, 0 },
    /* RET_DRIVE   */ { 150, 128,  40, 160,  60,   0, 0, 0 },
    /* RET_CHOIR   */ { 118, 210, 140,  40,   0,   0, 0, 0 },
};

/* Return level on create, 0..256.
 *
 * CHAMBER's entry is NOT used for slot 0: slot 0's level is bb.verb_level,
 * whose default is 0 and must stay 0 or every session that never opened the
 * reverb starts making one. This value is for a CHAMBER created in some other
 * slot, where silence on create would just look broken. */
const short ret_level_def[RET_NTYPE] = {
    0,      /* RET_NONE    */
    128,    /* RET_CHAMBER */
    160,    /* RET_DELAY   */
    128,    /* RET_DRIVE   */
    140,    /* RET_CHOIR   */
};
