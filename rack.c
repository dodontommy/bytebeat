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

/* The cold wing. Sustained, tunable sources for the synth side of the
 * instrument: pads, organs, sirens, bells. All of them ride the semitone
 * voice clock, so the sequencer's per-step PITCH lane plays them as
 * melodies; all of them are voiced to sit in front of the CHAMBER send. */

{ "cold", "lp(((t*p%d&511)+(t*p%d&511))*24,p%d)",
  "two cold saws a fixed interval apart. the pad -- sequence it in semitones.",
  3, {
    { "PITCH", "the lower saw. this is the root.", 0, KV_MUL, 4 },
    { "FIFTH", "the upper saw. 3:2 of PITCH is a fifth; nearby is a beating "
               "cluster.", 0, KV_MUL, 6 },
    { "DARK",  "lowpass over the pair. the winter control.", 0, KV_CUT, 56 },
  }, BB_WORD, 0 },

{ "vapor", "lp(((t*p%d&511)+(t*p%d&511))*24,"
           "(bt*320/bl>160?320-bt*320/bl:bt*320/bl)+p%d)",
  "the saw pair under a filter that breathes with the beat.",
  3, {
    { "PITCH",  "the lower saw.", 0, KV_MUL, 4 },
    { "THIRD",  "the upper saw; near 5:4 it turns minor daylight grey.",
      0, KV_MUL, 5 },
    { "BREATH", "how far the filter opens at the top of each breath.",
      0, KV_CUT, 20 },
  }, BB_WORD, 0 },

{ "hymn", "((t*p%d&255)>p%d?9000:-9000)+((t*p%d&255)>127?4500:-4500)",
  "hollow pulse organ, one register above another. WIDTH thins it to a reed.",
  3, {
    { "PITCH",  "the fundamental register.", 0, KV_MUL, 4 },
    { "WIDTH",  "pulse width. 127 is hollow and square; low is a reed.",
      0, KV_MASK, 127 },
    { "QUINT",  "the upper register. 2x PITCH is an octave organ stop.",
      0, KV_MUL, 8 },
  }, BB_WORD, 0 },

{ "siren", "bp((r>>p%d),(bt*p%d/bl)+p%d,250)",
  "a starved resonator dragged across each beat. the whine, the alarm.",
  3, {
    { "FEED",  "noise feeding the whine; lower is stronger.", 0, KV_NOISE, 20 },
    { "SWEEP", "how far the pitch is dragged over one beat.", 0, KV_AMOUNT, 18 },
    { "PITCH", "where the sweep starts.", 0, KV_RESON, 6 },
  }, BB_WORD, 0 },

{ "glass", "bp((((age*p%d)&2047)-1024),p%d,p%d)",
  "a single tuned partial striking a long resonator. cold bell.",
  3, {
    { "PARTIAL", "the strike tone; move it against PITCH for shades of "
                 "inharmonic.", 0, KV_MUL, 9 },
    { "PITCH",   "frequency of the ringing body.", 0, KV_RESON, 16 },
    { "RING",    "how long the glass keeps sounding.", 0, KV_Q, 246 },
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

/* ---- the patch morgue ----------------------------------------------------
 * Sixteen voices that are SOUNDS, not starting points. The first five are
 * the proven first-run groove voices under their working names; the rest
 * are curated settings of the remaining sources, cold wing included.
 * Params not overridden here come from the source's own seed values, which
 * the suite verifies audible for every source. */
static const RackPatch PATCH[] = {
    { "CONCRETE FLOOR",  "thump",    0,0,1, 198, 55,105, 0,  120,150, 40,  40, 0, {{0,0}} },
    { "BACKROOM SNARE",  "burst",    0,0,1, 228, 70,175, 8,  100,130, 35,  30, 0, {{0,0}} },
    { "AUTOPSY BELL",    "metal",    1,0,1, 242, 45,150, 20,  80,120, 25,  96, 0, {{0,0}} },
    { "SAND IN VENTS",   "dust",     1,1,1, 172, 35, 90, 28, 160,190, 75,  70, 0, {{0,0}} },
    { "BASEMENT TONE",   "fold",     1,1,0,   0, 40, 65, 0,  190,175, 80, 120, 0, {{0,0}} },
    { "COLD ROOM",       "cold",     0,1,0,   0,  0,255, 0,  180,170, 60, 140, 0, {{0,0}} },
    { "GREY DAYLIGHT",   "vapor",    0,1,0,   0,  0,255, 0,  170,150, 45, 120, 0, {{0,0}} },
    { "BLACK ORGAN",     "hymn",     1,0,0,   0, 30,140, 0,  100,  0,  0,  90, 0, {{0,0}} },
    { "DISTRICT ALARM",  "siren",    0,1,0,   0, 20,220, 0,  200,180, 55, 110, 0, {{0,0}} },
    { "GLASS AUTOPSY",   "glass",    0,0,1, 250,  0,255, 0,  100,  0,  0, 130, 0, {{0,0}} },
    { "MEAT LOCKER HUM", "stack",    1,0,0,   0, 25, 90, 0,  100,  0,  0,  60, 0, {{0,0}} },
    { "TAPE WIND",       "noise",    1,1,0,   0,  0, 60, 0,  190,190, 90,  80, 1, {{0,16}} },
    { "WIRE MOTHER",     "feedback", 0,0,1,   0,  0,255, 0,  100,  0,  0, 100, 0, {{0,0}} },
    { "CELLAR PULSE",    "pulse",    1,0,1, 120, 45,120, 0,  100,  0,  0,  40, 0, {{0,0}} },
    { "RUST CHIME",      "ring",     1,0,1, 210,  0,130, 0,  100,  0,  0,  90, 0, {{0,0}} },
    { "FLUORESCENT ROT", "crackle",  0,1,0,   0,  0,200, 40, 140,170, 70,  70, 0, {{0,0}} },
};
#define N_PATCH ((int)(sizeof PATCH / sizeof PATCH[0]))

int              rack_npatch(void)   { return N_PATCH; }
const RackPatch *rack_patch(int i)   { return &PATCH[((i % N_PATCH) + N_PATCH) % N_PATCH]; }

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
