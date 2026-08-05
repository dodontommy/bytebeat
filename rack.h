/* rack.h -- a voice described as parts, which compiles down to an expression.
 *
 * The expression language is the truth, but it is a bad control surface: it
 * offers no way to ask for "the same idea, brighter", and eight knobs called
 * p0..p7 tell you nothing about what they will do. Meanwhile gen.c already
 * knew the answer -- it carried a table of expression skeletons that are known
 * to work, with each slot tagged by what KIND of value belongs in it. That
 * table was only reachable by rolling dice.
 *
 * A Rack is that table made navigable. You pick a SOURCE by name, and the
 * slots it exposes are named for what they do to the sound. Optional BODY and
 * SPACE stages wrap it. rack_build() renders the whole thing to expression
 * text, which the UI shows live: turn GRAIN, watch p1 change in the source
 * line, hear the difference. That is the point -- the rack is not a wrapper
 * that hides the language, it is a labelled view of it.
 *
 * Continuous values are NOT stored here. Every one of them lives in the
 * layer's param[] array as a raw 0..255 int, exactly as before, so turning a
 * rack slot is a plain atomic store that the audio thread picks up on the next
 * sample -- no recompile, no glitch. The Rack only records the choices that
 * change the SHAPE of the expression, because those are the ones that do
 * require a recompile.
 */
#ifndef RACK_H
#define RACK_H

#ifdef __cplusplus
extern "C" {
#endif

#include "bytebeat.h"
#include "knob.h"

#define RACK_SRC_SLOTS  5    /* most slots any one source exposes  */
#define RACK_MAX_SLOTS  8    /* source slots + BODY + SPACE(2)     */

/* `Rack` itself is declared in bytebeat.h -- it is session state that main.c
 * persists, and a layer carries one whether or not the rack is driving it. */

/* One labelled control, as presented to the player. */
typedef struct {
    const char   *label;  /* "GRAIN"                                */
    const char   *hint;   /* one line, for the help overlay         */
    unsigned char pidx;   /* which p it writes to                   */
    unsigned char kind;   /* KV_* -- picks the ladder and the unit  */
    unsigned char init;   /* value to use when this slot appears    */
} RackSlot;

typedef struct {
    char     expr[BB_EXPR_MAX];
    RackSlot slot[RACK_MAX_SLOTS];
    int      nslot;
} RackBuild;

/* ---- the patch morgue ---------------------------------------------------
 * A curated bank of named, known-good voices: a source plus the settings
 * that make it a SOUND (params, post chain, envelope, chamber send).
 * Composing starts from one of these, not from a blank layer or a random
 * roll. Data only -- the front end applies it through its normal paths, so
 * every value lands in the same atomics a hand on a knob would reach. */
#define RACK_PATCH_SET 4
typedef struct {
    const char *name;         /* evidence-tag label, caps          */
    const char *src;          /* source-table name                 */
    unsigned char body, space, seq;
    unsigned char decay;      /* LCTL_DECAY; 0 = hold              */
    unsigned char drive, tone, crush;
    unsigned char spc_t, spc_fb, spc_mix;
    unsigned char send;       /* CHAMBER send                      */
    unsigned char nset;       /* sparse param overrides over seeds */
    struct { unsigned char idx, val; } set[RACK_PATCH_SET];
} RackPatch;

int              rack_npatch(void);
const RackPatch *rack_patch(int i);

/* A rack that makes a sound the moment it is applied. */
void rack_default(Rack *r);

/* Render `r` to expression text plus the list of controls it exposes.
 * Deterministic and allocation-free; safe to call every frame. */
void rack_build(const Rack *r, RackBuild *out);

/* Write each slot's starting value into p[]. Used when the source changes:
 * the old p0 meant "sweep in octaves" and the new p0 means "pitch in Hz", so
 * carrying the number across would be meaningless. */
void rack_seed_params(const RackBuild *b, int *p);

/* Is `p` a value this rack would ever ask for? Used to dim knobs the current
 * shape cannot reach. */
int  rack_slot_for_param(const RackBuild *b, int pidx);

int         rack_nsrc(void);
const char *rack_src_name(int i);
const char *rack_src_desc(int i);
const char *rack_src_shape(int i);   /* skeleton, %d where a knob goes */

/* The same skeleton with its placeholders resolved to p0..p4 -- what the
 * expression line will actually read once this source is selected. */
void        rack_src_shape_text(int i, char *buf, size_t n);

/* The output mode this source was voiced for. Changing source adopts it,
 * because `ramp` wants BYTE (it IS a byte counter) and `pair` wants WORD. */
int         rack_src_mode(int i);
int         rack_src_triggered(int i);

#ifdef __cplusplus
}
#endif

#endif /* RACK_H */
