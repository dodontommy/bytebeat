/* knob.h -- what a knob means, and where its detents are.
 *
 * The problem this file exists to solve:
 *
 *   `param[i]` is an integer 0..255 handed straight to the VM. But almost no
 *   use of a knob is linear in that number. `t>>p` masks p to 0..31, so a
 *   256-position knob addresses 32 distinct sounds in eight identical repeats.
 *   `t&p` is only musical when p is 2^n-1 -- eight useful values out of 256.
 *   `lp(x,p)` is logarithmic: p=1..10 covers three octaves, p=200..255 covers
 *   almost nothing.
 *
 *   So the raw value is a linear ramp across a space where the sound is a
 *   step function, and a UI that steps it by 1 or 16 is mostly walking through
 *   dead positions and occasionally falling off a cliff. That is what "I don't
 *   feel in control" actually is.
 *
 * The fix is a LADDER per kind: the ordered list of values that do something.
 * The UI steps along the ladder instead of along the integers, draws the bar
 * from the ladder position instead of from v/256, and prints the physical
 * quantity the value corresponds to. A shift knob then has 16 positions that
 * each change the sound, labelled with the time they actually take.
 *
 * Nothing here is in the audio path. `param[i]` stays a raw 0..255 int and the
 * VM never learns any of this; the ladder only decides which of those 256
 * values the UI is willing to stop on.
 */
#ifndef KNOB_H
#define KNOB_H

#include <stddef.h>

/* What a value is being used FOR. Hand-written expressions get a kind derived
 * from the role the compiler inferred (knob_kind_for_role); rack slots declare
 * their kind directly, because the rack knows what it built. */
enum {
    KV_LINEAR = 0,  /* unknown use: every value, coarse steps    */
    KV_SHIFT,       /* t>>p     octave/rate, masked to 0..31     */
    KV_MUL,         /* t*p      pitch, f = sr*p/256              */
    KV_MASK,        /* t&p      timbre, only 2^n-1 is musical    */
    KV_CUT,         /* lp(x,p)  cutoff, logarithmic              */
    KV_PERIOD,      /* t%p      pitch, f = sr/p                  */
    KV_NOISE,       /* r>>p     noise level in bits              */
    KV_AMOUNT,      /* +p -p    offset, linear but coarse        */
    KV_TIME,        /* the rack's SPACE tap, d(p*96+400)         */
    KV_DECAY,       /* gate envelope, k = 1 + p*p/108            */
    KV_RESON,       /* bp(x,p,q) resonator frequency coefficient  */
    KV_Q,           /* bp resonance, concentrated near self-ring  */
    KV_COUNT
};

/* Sample rate used for every label that quotes a frequency or a duration.
 * UI thread only; call it once a frame, it is a plain store. */
void knob_set_rate(int sr);

int         knob_kind_for_role(int role);
const char *knob_kind_name(int kind);

/* One-pole coefficient (1..255) -> approximate -3dB point in Hz. Exposed
 * because the post chain's TONE stage is the same filter as lp() and should
 * be labelled with the same number. */
int         knob_cutoff_hz(int c);

/* Which of the six meanings a kind carries, for colouring. Two controls that
 * share a colour do the same KIND of thing to the sound, wherever they live
 * on screen. */
enum { KC_LEVEL = 0, KC_PITCH, KC_RATE, KC_TIMBRE, KC_CUTOFF, KC_SPACE, KC_COUNT };
int         knob_class(int kind);

/* Ladder access. Detent indices run 0..knob_ndetent()-1. */
int knob_ndetent(int kind);
int knob_value(int kind, int detent);      /* detent -> raw 0..255           */
int knob_detent(int kind, int value);      /* raw -> nearest detent          */

/* Move `value` by `dir` detents (or `dir` coarse jumps if coarse != 0) and
 * return the new raw value, clamped to the ends of the ladder. */
int knob_step(int kind, int value, int dir, int coarse);

/* Bar fill 0..width, computed from LADDER POSITION rather than from v/256.
 * A shift knob at the top of its useful range reads full, not 5%. */
int knob_fill(int kind, int value, int width);

/* The stretch of the ladder the patch generator is allowed to roll in, as
 * detent indices. The ends of a ladder are reachable by hand but are a bad
 * place to land at random -- a cutoff of 1 is inaudible and a mask of 1 is
 * nearly silent, and a generator that hands you those is a generator you stop
 * trusting. Both ends inclusive. */
void knob_gen_range(int kind, int *lo, int *hi);

/* "  >>9" -- the value as it appears in the expression, 5 chars, right
 * aligned. Never longer than 5 visible characters. */
void knob_fmt_value(int kind, int value, char *buf, size_t n);

/* "2.9s" / "1.2k" / "6 bits" -- the physical quantity, or "" if the kind has
 * no honest unit. Never longer than 7 visible characters. */
void knob_fmt_unit(int kind, int value, char *buf, size_t n);

#endif /* KNOB_H */
