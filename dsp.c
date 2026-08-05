/* dsp.c -- the post-expression chain. Integer only. Called once per sample
 * from the audio thread, so: no allocation, no division by a runtime value
 * in the hot path where a shift will do, no libm.
 */

#include "dsp.h"
#include <string.h>

int32_t dsp_clip16(int32_t x)
{
    if (x >  32767) return  32767;
    if (x < -32768) return -32768;
    return x;
}

void post_init(PostState *ps, int32_t *buf, uint32_t len_pow2)
{
    memset(ps, 0, sizeof *ps);
    ps->dl    = buf;
    ps->dlen  = len_pow2;
    ps->dmask = len_pow2 - 1u;
}

void post_reset(PostState *ps)
{
    ps->tone_st  = 0;
    ps->hold_val = 0;
    ps->hold_cnt = 0;
    ps->dc_x1    = 0;
    ps->dc_y1    = 0;
    if (ps->dl) memset(ps->dl, 0, ps->dlen * sizeof ps->dl[0]);
}

/* DC BLOCKER -- always on, deliberately not bypassable.
 *
 * BYTE mode maps the low 8 bits of the expression to a sample by subtracting
 * 128. That is correct for a signal that uses the whole byte, but plenty of
 * expressions do not: `t&t>>8` spends its early life producing values 0..31,
 * which after the mapping is a near-full-scale NEGATIVE constant with a
 * small wiggle on top. That is a DC offset, and DC is the worst thing you
 * can send to hardware -- it eats your headroom, it pushes a speaker cone
 * off centre and holds it there, and it is completely inaudible so you will
 * not notice until something is damaged or a recording is unusable.
 *
 *   y[n] = x[n] - x[n-1] + R*y[n-1]
 *
 * The differencing kills DC outright; R just below 1 restores everything
 * above a few Hz. R = 65500/65536 puts the corner near 4Hz at 44.1kHz --
 * far below anything you can hear, so nothing musical is touched.
 *
 * Kept out of the bypassable chain because "bypass" means "let me hear the
 * raw program", not "let me send DC to my mixer". */
#define DC_R 65500

int32_t dsp_dcblock(PostState *ps, int32_t x)
{
    /* The scaling shift goes through uint64_t. `x - dc_x1` is negative for
     * half of every waveform, and a signed left shift of a negative value is
     * undefined behaviour -- Clang only behaved because the Makefile passes
     * -fwrapv, and MSVC has no -fwrapv to pass. Shifting the unsigned bit
     * pattern and reinterpreting it produces the identical value on every
     * compiler; this is the same U()/S() trick expr.c plays with the VM's
     * arithmetic. Nothing about the filter changes. */
    int64_t y = (int64_t)((uint64_t)(int64_t)(x - ps->dc_x1) << 16)
              + ((ps->dc_y1 * DC_R) >> 16);
    ps->dc_x1 = x;
    ps->dc_y1 = y;
    return (int32_t)(y >> 16);
}

/* Three-segment saturator.
 *
 * A hard clip (just clamping at +/-32767) generates a lot of high harmonics
 * and reads as "fizz" or "digital". Compressing the region above a knee
 * instead means loud material gets denser rather than brighter, which is the
 * difference between a wall of noise and a wall of concrete.
 *
 *   |x| < KNEE          : untouched
 *   KNEE..KNEE+3*HEAD   : compressed 3:1
 *   beyond              : hard limit
 */
#define SAT_KNEE 18000

static inline int32_t saturate(int32_t x)
{
    if (x > SAT_KNEE) {
        x = SAT_KNEE + (x - SAT_KNEE) / 3;
    } else if (x < -SAT_KNEE) {
        x = -SAT_KNEE + (x + SAT_KNEE) / 3;
    }
    return dsp_clip16(x);
}

/* Same one-pole as expr.c's lp(), duplicated here rather than shared so that
 * dsp.c and expr.c stay independently readable. Q8 state, int64 multiply --
 * see the comment in expr.c for why the naive int32 version silently dies. */
static inline int32_t onepole(int64_t *st, int32_t x, int32_t c)
{
    /* Unsigned shift for the same reason as dsp_dcblock: x is a signed
     * sample, it is negative half the time, and `<<` on a negative signed
     * value is undefined. Same bits, defined everywhere. */
    int64_t target = (int64_t)((uint64_t)(int64_t)x << 8);
    *st += ((target - *st) * c) >> 8;
    return (int32_t)(*st >> 8);
}

int32_t post_process(PostState *ps, int32_t x, const PostParams *pp)
{
    if (pp->bypass) return x;

    /* --- DRIVE ----------------------------------------------------------
     * Multiply then saturate. The multiplier is (256 + drive*8)/256, so
     * drive=0 is unity and drive=255 is about 9x -- enough to slam any
     * bytebeat expression into the knee and keep it there. */
    if (pp->drive > 0) {
        int64_t g = 256 + (int64_t)pp->drive * 8;
        x = saturate((int32_t)(((int64_t)x * g) >> 8));
    }

    /* --- TONE -----------------------------------------------------------
     * The most important knob on the instrument. Bytebeat's structure lives
     * in its high-frequency edges; rolling those off leaves the periodicity
     * of the shifts audible as pitch and weight instead of as grit. */
    if (pp->tone < 255) {
        int c = pp->tone < 1 ? 1 : pp->tone;
        x = onepole(&ps->tone_st, x, c);
    }

    /* --- CRUSH ----------------------------------------------------------
     * Sample and hold. Holding each sample for N frames is decimation: it
     * folds everything above (sr/N)/2 back down as aliasing. Unlike actually
     * lowering the sample rate, it costs nothing, needs no device retune,
     * and can be swept continuously. */
    if (pp->crush > 0) {
        if (--ps->hold_cnt <= 0) {
            ps->hold_cnt = 1 + pp->crush / 4;
            ps->hold_val = x;
        }
        x = ps->hold_val;
    }

    /* --- SPACE ----------------------------------------------------------
     * A single feedback delay. Not a reverb: at short times it is a comb
     * filter that adds a resonant pitch, at long times it is an echo, and
     * with feedback near maximum it is a decaying smear that never quite
     * stops -- which is the sound that makes a static drone feel like it is
     * happening in a room the size of a hangar.
     *
     * Feedback is capped below 256 (unity) on purpose. At unity the loop
     * gain never decays and the delay line saturates into a howl within
     * seconds. 244/256 is about 0.95: long, but bounded. */
    if (ps->dl && (pp->spc_mix > 0 || pp->spc_freeze)) {
        uint32_t tap = (uint32_t)pp->spc_time;
        if (tap < 1) tap = 1;
        if (tap > ps->dmask) tap = ps->dmask;

        int32_t wet = ps->dl[(ps->dw - tap) & ps->dmask];

        int fb = pp->spc_fb;
        if (fb > 244) fb = 244;

        /* FREEZE closes a unity loop and stops admitting new input. Because
         * the read and write cursors remain `tap` samples apart, the exact
         * captured phrase circulates until freeze is released. Normal mode
         * retains the deliberately sub-unity feedback ceiling. */
        int32_t into = pp->spc_freeze
                     ? wet
                     : saturate(x + (int32_t)(((int64_t)wet * fb) >> 8));
        ps->dl[ps->dw] = into;
        ps->dw = (ps->dw + 1u) & ps->dmask;

        x = x + (int32_t)(((int64_t)wet * pp->spc_mix) >> 8);
        x = saturate(x);
    }

    return x;
}
