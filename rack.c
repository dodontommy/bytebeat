/* rack.c -- the source table and the renderer. See rack.h for the idea.
 *
 * Adding a source is adding one row to SRC[]. It needs: a name short enough
 * for the panel, a skeleton whose %d placeholders are filled left to right
 * with p-indices, a slot list saying what each placeholder is FOR, and one
 * line of honest description. Nothing else in the program has to change --
 * the generator, the panel, the ladders and the help all read this table.
 */

#include "rack.h"

#include <stdio.h>
#include <string.h>

/* Every skeleton below obeys C precedence, which the expression parser
 * implements exactly, so `t*p0^t*p1>>1` groups as `(t*p0)^((t*p1)>>1)` here
 * and in the VM alike. Parenthesise anyway where the intent is not obvious. */
typedef struct {
    const char *name;
    const char *fmt;
    const char *desc;
    int         nslot;
    RackSlot    slot[RACK_SRC_SLOTS];
    unsigned char mode;      /* the mode this source was voiced for */
    unsigned char triggered; /* needs tr/age/vel from the sequencer */
} Src;

static const Src SRC[] = {

{ "ramp", "t*p%d",
  "a plain sawtooth. the raw material -- everything else is this, bent.",
  1, {
    { "PITCH", "how fast the ramp wraps. this is the note you hear.",
      0, KV_MUL, 6 },
  }, BB_BYTE, 0 },

{ "pair", "t*(t>>p%d&t>>p%d)",
  "the classic. metallic and self-similar; it never quite repeats.",
  2, {
    { "SWEEP", "the slow one. sets how long the pattern takes to come round.",
      0, KV_SHIFT, 9 },
    { "GRAIN", "the fast one. sets the size of the grain the pattern is cut from.",
      0, KV_SHIFT, 12 },
  }, BB_WORD, 0 },

{ "fold", "(t*p%d^t*p%d>>1)",
  "two ramps beating against each other. thick, and slightly out of tune.",
  2, {
    { "PITCH",  "the fundamental.", 0, KV_MUL, 5 },
    { "DETUNE", "the second ramp. close to PITCH beats slowly; far is a chord.",
      0, KV_MUL, 7 },
  }, BB_WORD, 0 },

{ "gate", "t*((t>>p%d|t>>p%d)&p%d)",
  "rhythmic on its own -- the mask chops the ramp into bursts.",
  3, {
    { "SWEEP", "the long cycle.", 0, KV_SHIFT, 10 },
    { "GRAIN", "the short cycle.", 0, KV_SHIFT, 13 },
    { "WIDTH", "how much of the mask survives. small values leave gaps.",
      0, KV_MASK, 63 },
  }, BB_WORD, 0 },

/* Two ramps masked to a byte and multiplied -- an actual ring modulator, sum
 * and difference tones and all. The obvious `t*a & t>>b` is not one: its
 * amplitude is bounded by `t>>b`, so the patch is inaudible for the first
 * minute of a session and gradually gets louder, which is not a sound, it is
 * a fault. Masking both operands makes the level independent of how long the
 * instrument has been running. */
{ "ring", "((t*p%d&255)*(t*p%d&255))",
  "ring modulation: two ramps multiplied. clangorous, bell-like, inharmonic.",
  2, {
    { "PITCH", "the carrier.", 0, KV_MUL, 8 },
    { "RATIO", "the modulator. near PITCH it beats slowly; far away it is a "
               "chord of sum and difference tones.", 0, KV_MUL, 13 },
  }, BB_WORD, 0 },

{ "noise", "(r>>p%d)",
  "white noise. with a short decay this is a hi-hat; filtered, it is wind.",
  1, {
    { "LEVEL", "how many bits of noise survive. fewer bits is quieter and grittier.",
      0, KV_NOISE, 18 },
  }, BB_WORD, 0 },

{ "snare", "((r>>p%d)+(t*p%d>>1))",
  "noise over a tuned body. give it a short decay and it is a snare.",
  2, {
    { "NOISE", "the crack.", 0, KV_NOISE, 18 },
    { "BODY",  "the pitch underneath the crack.", 0, KV_MUL, 30 },
  }, BB_WORD, 0 },

{ "crackle", "((t&t>>p%d)*p%d)",
  "sparse and spitting. mostly silence with events happening in it.",
  2, {
    { "SWEEP", "how often an event happens.", 0, KV_SHIFT, 10 },
    { "PITCH", "what the events are pitched at.", 0, KV_MUL, 20 },
  }, BB_WORD, 0 },

{ "stack", "((t>>p%d)*(t>>p%d))",
  "two slow ramps multiplied. deep, lurching, full of subharmonics.",
  2, {
    { "SWEEP", "the slower ramp.", 0, KV_SHIFT, 7 },
    { "GRAIN", "the faster ramp.", 0, KV_SHIFT, 10 },
  }, BB_WORD, 0 },

{ "pulse", "((t%%(p%d+1))*(t>>p%d))",
  "a hard-edged pulse train, swept by a slow ramp. reedy and nasal.",
  2, {
    { "PITCH", "the pulse period. this is a real note.", 0, KV_PERIOD, 40 },
    { "SWEEP", "the ramp that sweeps it.", 0, KV_SHIFT, 12 },
  }, BB_WORD, 0 },

{ "bell", "(t*p%d^t>>p%d)",
  "struck metal. inharmonic and transient -- best with a short decay.",
  2, {
    { "PITCH", "the strike tone.", 0, KV_MUL, 12 },
    { "SWEEP", "the decay-shaped partial underneath it.", 0, KV_SHIFT, 9 },
  }, BB_WORD, 0 },

/* Triggered noise-synth engines. Unlike the continuous bytebeat sources
 * above, these restart from tr/age/vel, so every sequencer event has a real
 * attack and a repeatable internal phase. */
{ "thump", "bp(tr*vel*4096,p%d,p%d)",
  "an impulse into a low resonator. from kick drum to moving machinery.",
  2, {
    { "PITCH", "frequency of the resonating body.", 0, KV_RESON, 2 },
    { "RING",  "how long the body keeps moving after the strike.", 0, KV_Q, 244 },
  }, BB_WORD, 1 },

{ "burst", "hp(lp(r>>p%d,p%d),p%d)",
  "a triggered band of noise: hat, snare, steam valve or torn speaker.",
  3, {
    { "GRAIN", "how many noise bits survive.", 0, KV_NOISE, 18 },
    { "BODY",  "upper edge of the noise band.", 0, KV_CUT, 88 },
    { "AIR",   "lower edge; raise it for a thinner strike.", 0, KV_CUT, 8 },
  }, BB_WORD, 1 },

{ "metal", "bp(((((age*p%d)^(age*p%d))&65535)-32768),p%d,p%d)",
  "reset bitwise partials striking a resonator. repeatable, clangorous metal.",
  4, {
    { "PARTIAL", "first metallic partial.", 0, KV_MUL, 23 },
    { "RATIO",   "second partial; move it away for harsher intervals.", 0, KV_MUL, 31 },
    { "PITCH",   "frequency of the struck body.", 0, KV_RESON, 24 },
    { "RING",    "resonator persistence.", 0, KV_Q, 235 },
  }, BB_WORD, 1 },

{ "dust", "((r&255)<p%d?(r>>p%d):0)",
  "random microscopic impacts inside each gate, from grit to sandblast.",
  2, {
    { "DENSITY", "chance of a grain on each sample.", 0, KV_AMOUNT, 12 },
    { "GRAIN",   "size and level of each grain.", 0, KV_NOISE, 18 },
  }, BB_WORD, 1 },

{ "rumble", "lp((r>>p%d)+bp(tr*vel*512,p%d,p%d),p%d)",
  "low noise wrapped around a struck body. weight underneath a beat.",
  4, {
    { "NOISE", "depth of the moving floor.", 0, KV_NOISE, 21 },
    { "PITCH", "frequency of the impact body.", 0, KV_RESON, 3 },
    { "RING",  "how long the impact persists.", 0, KV_Q, 242 },
    { "DARK",  "roll everything into the floor.", 0, KV_CUT, 24 },
  }, BB_WORD, 1 },

{ "feedback", "bp((tr*vel*512)+(r>>p%d),p%d,p%d)",
  "a nearly unstable resonator fed by noise and fresh strikes.",
  3, {
    { "FEED",  "noise feeding the resonator; lower is stronger.", 0, KV_NOISE, 24 },
    { "PITCH", "frequency the loop wants to become.", 0, KV_RESON, 10 },
    { "RING",  "toward the top, the loop refuses to die.", 0, KV_Q, 250 },
  }, BB_WORD, 1 },

};
#define N_SRC ((int)(sizeof SRC / sizeof SRC[0]))

/* ---- the two optional stages -------------------------------------------
 * BODY is a lowpass and SPACE is a feedback delay, and they are wrapped in
 * that order -- filter INSIDE the loop -- so each repeat is darker than the
 * last. Filtering outside the loop instead lets the feedback build up bright
 * and scream; inside, it decays into the floor, which is the sound you
 * actually want from a delay you are going to leave running.
 *
 * SPACE's delay time cannot be a bare knob: `d(p)` maxes out at 255 samples,
 * which is 6ms. Scaling it to `p*96+400` puts the range at 9ms..565ms, which
 * covers slapback through to a long smear, and keeps it live -- the tap moves
 * while you turn it, with no recompile.
 */
static const RackSlot SLOT_BODY =
    { "BODY",  "lowpass over everything above. this is the darkness control.",
      0, KV_CUT, 70 };

static const RackSlot SLOT_SPC_T =
    { "TIME",  "delay length, 9ms to 565ms. short is a room, long is a loop.",
      0, KV_TIME, 96 };

static const RackSlot SLOT_SPC_F =
    { "FEED",  "how much comes back. near the top it will run away -- that is allowed.",
      0, KV_AMOUNT, 150 };

/* ---- public ------------------------------------------------------------- */

int         rack_nsrc(void)            { return N_SRC; }
const char *rack_src_name(int i)       { return SRC[i % N_SRC].name; }
const char *rack_src_desc(int i)       { return SRC[i % N_SRC].desc; }
const char *rack_src_shape(int i)      { return SRC[i % N_SRC].fmt; }

/* The skeleton with its placeholders filled in the way rack_build() would
 * fill them, so the help page shows the text you will actually get rather
 * than a printf format string. */
void rack_src_shape_text(int i, char *buf, size_t n)
{
    snprintf(buf, n, SRC[i % N_SRC].fmt, 0, 1, 2, 3, 4);
}
int         rack_src_mode(int i)       { return SRC[i % N_SRC].mode; }
int         rack_src_triggered(int i)  { return SRC[i % N_SRC].triggered; }

void rack_default(Rack *r)
{
    r->src   = 1;          /* pair -- the sound bytebeat is known for */
    r->body  = 1;
    r->space = 0;
    r->mode  = BB_WORD;
}

void rack_build(const Rack *r, RackBuild *out)
{
    const Src *s = &SRC[r->src % N_SRC];
    int knob = 0;

    out->nslot = 0;

    /* Source slots take p0 upward, in the order they appear in the skeleton,
     * so the panel reads top to bottom in the same order as the text. */
    int idx[RACK_SRC_SLOTS] = { 0, 0, 0, 0, 0 };
    for (int i = 0; i < s->nslot; i++) {
        idx[i] = knob++;
        out->slot[out->nslot]       = s->slot[i];
        out->slot[out->nslot].pidx  = (unsigned char)idx[i];
        out->nslot++;
    }

    char core[256];
    snprintf(core, sizeof core, s->fmt,
             idx[0], idx[1], idx[2], idx[3], idx[4]);

    /* BODY and SPACE claim their slots next. There is always room: the
     * biggest source uses five of eight and the two stages want three. */
    int cut = -1, dt = -1, fb = -1;
    if (r->body && knob < BB_NPARAM) {
        cut = knob++;
        out->slot[out->nslot]      = SLOT_BODY;
        out->slot[out->nslot].pidx = (unsigned char)cut;
        out->nslot++;
    }
    if (r->space && knob + 1 < BB_NPARAM) {
        dt = knob++;
        out->slot[out->nslot]      = SLOT_SPC_T;
        out->slot[out->nslot].pidx = (unsigned char)dt;
        out->nslot++;
        fb = knob++;
        out->slot[out->nslot]      = SLOT_SPC_F;
        out->slot[out->nslot].pidx = (unsigned char)fb;
        out->nslot++;
    }

    if (fb >= 0 && cut >= 0)
        snprintf(out->expr, sizeof out->expr,
                 "w(lp(%s+(d(p%d*96+400)*p%d>>8),p%d))", core, dt, fb, cut);
    else if (fb >= 0)
        snprintf(out->expr, sizeof out->expr,
                 "w(%s+(d(p%d*96+400)*p%d>>8))", core, dt, fb);
    else if (cut >= 0)
        snprintf(out->expr, sizeof out->expr, "lp(%s,p%d)", core, cut);
    else
        snprintf(out->expr, sizeof out->expr, "%s", core);
}

void rack_seed_params(const RackBuild *b, int *p)
{
    for (int i = 0; i < b->nslot; i++)
        p[b->slot[i].pidx] = b->slot[i].init;
}

int rack_slot_for_param(const RackBuild *b, int pidx)
{
    for (int i = 0; i < b->nslot; i++)
        if (b->slot[i].pidx == pidx) return i;
    return -1;
}
