/* gen.h -- procedural patch generator, and the voice snapshot it produces.
 *
 * Writing bytebeat expressions is a skill. Choosing a source by name is not.
 * This file exists so you can find sounds by rolling dice instead of by
 * typing, and still end up somewhere usable every single time -- and, since
 * the generator now rolls RACKS rather than raw text, so that whatever it
 * hands you is something you can then steer by hand. That second part is the
 * one that matters: a random patch you cannot adjust is a slot machine.
 *
 * The key property is unchanged: a roll is AUDITIONED before you ever hear
 * it. The candidate is compiled and run through the VM offline, and if it
 * turns out silent, DC, or clipped into mush it is discarded and the next seed
 * is tried. "Press a key, get a sound" is only useful if it is actually true.
 */
#ifndef GEN_H
#define GEN_H

#include "bytebeat.h"
#include "rack.h"

/* Everything about one layer's sound, in a form that can be copied, stored
 * and restored in one assignment. Used for generation, for the undo history,
 * and for loading examples -- all three are "replace this voice wholesale",
 * so all three want the same struct. */
typedef struct {
    Rack     rack;
    int      custom;              /* 1 = expr is hand-written, rack is stale */
    char     expr[BB_EXPR_MAX];
    int      mode;
    int      p[BB_NPARAM];
    int      ctl[LCTL_COUNT];
    int      seq_on;
    int      gate[BB_STEPS];
    int      pitch[BB_STEPS];
    int      ratchet[BB_STEPS];
    int      prob[BB_STEPS];
    int      lock[BB_LOCK_COUNT][BB_STEPS];
    unsigned motion_mask;
    unsigned seed;
    int      level;               /* measured RMS as a percentage of full scale */
} Voice;

/* Roll a fresh patch. `seed` picks both the structure and the values.
 * Returns the seed that was actually accepted (which may not be the one you
 * passed, if the first candidates failed the audition). Deterministic:
 * gen_roll(s, &v) always yields the same voice for the same s. */
unsigned gen_roll(unsigned seed, Voice *out);

/* Keep the source and the stages, re-roll the values. Small change, same
 * family -- the difference between "another idea" and "the same idea, moved". */
unsigned gen_mutate(unsigned seed, unsigned salt, Voice *out);

/* Euclidean rhythm: spread `pulses` as evenly as possible over `n` steps.
 * Bjorklund's algorithm gives you most of the world's traditional rhythms
 * from two numbers, which is a lot of musical mileage for one keypress.
 * Writes GATE_OFF/ON/ACCENT into gate[0..BB_STEPS-1]. */
void gen_euclid(int n, int pulses, int *gate);

/* Measure a voice offline and return its RMS as a percentage of full scale.
 * Exposed because the UI wants to report the level of hand-edited patches
 * too, not just generated ones. */
int  gen_measure(const Voice *v);

#endif /* GEN_H */
