/* ui.c -- ncurses front end: layers, editor, step grid, panel, scope.
 *
 * This thread is allowed to be slow. It mallocs, it writes files, it talks to
 * sockets, it runs the patch generator, it redraws a terminal 50 times a
 * second. None of that can disturb the audio thread, because the only things
 * the two share are atomics and two single-producer ring buffers.
 *
 * Every frame: drain the sample ring (sink_service), free programs the audio
 * thread has finished with (bb_reclaim), read a key, redraw.
 *
 * ---- the panel ----------------------------------------------------------
 *
 * The centre of the screen is one flat list of controls presented in three
 * columns, and every control in it -- rack slot, post-chain knob, tempo,
 * sample rate -- is reached the same way: move the cursor, press left/right.
 * That uniformity is deliberate. The previous layout had eight anonymous
 * knobs bumped with letter keys, thirteen controls TAB-ed through in a
 * ribbon, sample rate on bracket keys and gain on minus/equals, and there was
 * no way to know which of them was worth touching.
 *
 * Two ideas make the panel legible:
 *
 *   DETENTS. Values step along the ladder for their kind (knob.c), not along
 *   the integers, so every press changes the sound and the bar position means
 *   something. A shift knob has nineteen useful positions, not 256.
 *
 *   UNITS. Everything that has an honest physical value shows it -- Hz, ms,
 *   bits, percent. "SWEEP >>9 86Hz" tells you what will happen. "p0 137" does
 *   not.
 *
 * The bottom line always describes the control under the cursor, so the help
 * for what you are touching is on screen before you think to ask for it.
 */

#include "bytebeat.h"
#include "ui.h"
#include "audio.h"
#include "sink.h"
#include "gen.h"
#include "rack.h"
#include "knob.h"
#include "examples.h"

#include <ncurses.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <time.h>

/* ---- modes ------------------------------------------------------------- */
enum { M_NORMAL = 0, M_INSERT, M_SEQ, M_PERF };
static int mode_ui;

/* ---- editor state ------------------------------------------------------ */
static char  ed[BB_EXPR_MAX];
static int   ed_len, ed_cur, ed_scroll;
static ExprError last_err = { 1, 0, "" };

static int   seq_cur;            /* step cursor in SEQ mode          */
enum { SL_GATE = 0, SL_PITCH, SL_RATCHET, SL_PROB, SL_LOCK, SL_COUNT };
static int   seq_lane;
static int   seq_lock_target;
static int   euclid_k;           /* last euclidean density used      */
static int   cur_example;
static int   help_page = -1;     /* -1 = closed                      */
static int   quit;

static int   perf_cursor;
static int   motion_record;
static int   motion_layer = -1;
static int   motion_target = -1;

/* Voice history, per layer, so 'C' can walk back out of a bad move. */
static Voice    prev_voice[BB_NLAYER];
static int      has_prev[BB_NLAYER];
static unsigned last_seed[BB_NLAYER];

static char  status[240];
static int   status_ttl;
static char  warning[200];

static SCREEN *scr;
static FILE   *tty_out, *tty_in;

enum { C_FRAME = 1, C_VAL, C_ERR, C_SEL, C_WAVE, C_HOT, C_PITCH, C_TIME, C_SPACE };
static int use_color;

static uint32_t uirng = 0x9e3779b9u;

static uint32_t nextrand(void)
{
    uirng ^= uirng << 13;
    uirng ^= uirng >> 17;
    uirng ^= uirng << 5;
    return uirng;
}

/* ======================================================================== */
/*  helpers                                                                 */
/* ======================================================================== */

void ui_status(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(status, sizeof status, fmt, ap);
    va_end(ap);
    status_ttl = 110;
}

void ui_set_warning(const char *msg)
{
    snprintf(warning, sizeof warning, "%s", msg);
}

static int attr_for(int pair, int bold)
{
    int a = bold ? A_BOLD : 0;
    if (use_color) a |= COLOR_PAIR(pair);
    return a;
}

static int dim_attr(int pair)
{
    return (int)((unsigned)attr_for(pair, 0) | (unsigned)A_DIM);
}

static int rev_attr(int pair)
{
    return (int)((unsigned)attr_for(pair, 1) | (unsigned)A_REVERSE);
}

/* Draw a plain string. Never printw() with user data as the format. */
static void put(int y, int x, int a, const char *s)
{
    if (y < 0 || y >= LINES || x >= COLS || x < 0) return;
    attron(a);
    mvaddnstr(y, x, s, COLS - x);
    attroff(a);
}

static void putf(int y, int x, int a, const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    put(y, x, a, b);
}

static void clear_row(int y)
{
    if (y < 0 || y >= LINES) return;
    move(y, 0);
    clrtoeol();
}

static const char *mode_name(int m)
{
    return m == BB_BYTE ? "BYTE" : (m == BB_SIGNED ? "SIGN" : "WORD");
}

static const char *space_sync_name(int v)
{
    static const char *const NAME[] = {
        "free", "1/32", "1/16T", "1/16", "1/8T", "1/8",
        "1/4T", "1/4", "1/2", "1bar", "2bar"
    };
    return NAME[bb_clampi(v, 0, 10)];
}

static const char *loop_status_name(int v)
{
    switch (v) {
    case LOOP_ARMED:     return "ARMED";
    case LOOP_RECORDING: return "CAPTURE";
    case LOOP_PLAYING:   return "PLAY";
    default:             return "OFF";
    }
}

static int      focus(void)     { return atomic_load(&bb.focus); }
static Layer   *FL(void)        { return &bb.layer[focus()]; }
static Program *cur_prog(void)  { return atomic_load(&FL()->prog); }

static const CtlInfo *ctl_info(int i)
{
    return i < LCTL_COUNT ? &bb_lctl_info[i] : &bb_gctl_info[i - LCTL_COUNT];
}

static int ctl_get(int i)
{
    return i < LCTL_COUNT ? atomic_load(&FL()->ctl[i])
                          : atomic_load(&bb.gctl[i - LCTL_COUNT]);
}

static void ctl_set(int i, int v)
{
    const CtlInfo *ci = ctl_info(i);
    v = bb_clampi(v, ci->lo, ci->hi);
    if (i < LCTL_COUNT) atomic_store(&FL()->ctl[i], v);
    else                atomic_store(&bb.gctl[i - LCTL_COUNT], v);
}

/* Every control's bar is coloured by what it DOES, not by where it lives, so
 * the two pitch controls on opposite sides of the screen look alike. */
static int class_pair(int cls)
{
    switch (cls) {
    case KC_PITCH:  return C_PITCH;
    case KC_RATE:   return C_TIME;
    case KC_TIMBRE: return C_WAVE;
    case KC_CUTOFF: return C_SEL;
    case KC_SPACE:  return C_SPACE;
    default:        return C_VAL;
    }
}

/* ======================================================================== */
/*  the control list                                                        */
/* ======================================================================== */

enum { CG_VOICE = 0, CG_POST, CG_XPORT, CG_COUNT };

enum {
    CT_SOURCE = 0,   /* rack source chooser                       */
    CT_STAGES,       /* which optional stages are wrapped around  */
    CT_PARAM,        /* layer param[idx], stepped along a ladder  */
    CT_LCTL,         /* layer ctl[idx]                            */
    CT_GCTL,         /* global gctl[idx]                          */
    CT_MODE,         /* how the expression's int becomes a sample */
    CT_SEQON,        /* sequencer on/off                          */
    CT_RATE          /* sample rate; adjusting it retunes ALSA    */
};

/* Which directional move sweeps a control. Deliberately separate from the
 * ladder kind: DRIVE and CRUSH belong on the grit axis but must not inherit
 * the mask ladder, and the SPACE delay time is measured in milliseconds
 * without being something "faster" should shorten. */
enum { NX_NONE = 0, NX_TONE, NX_GRIT, NX_PITCH, NX_RATE, NX_COUNT };

typedef struct {
    char          label[12];    /* longest is "p2 cutoff" */
    const char   *hint;
    unsigned char type, idx, kind, group, axis;
} Ctrl;

#define CTRL_MAX 32
static Ctrl ctrls[CTRL_MAX];
static int  nctrl;
static int  grp_first[CG_COUNT], grp_count[CG_COUNT];
static int  cursor;              /* index into ctrls[]                */
static RackBuild focus_build;    /* rebuilt every frame for the focus */

/* Default axis for a knob, from what its kind means. Post-chain controls
 * override this, because "dirtier" has to reach DRIVE and CRUSH even though
 * neither is a mask. */
static int axis_for_kind(int kind)
{
    switch (knob_class(kind)) {
    case KC_PITCH:  return NX_PITCH;
    case KC_RATE:   return NX_RATE;
    case KC_TIMBRE: return NX_GRIT;
    case KC_CUTOFF: return NX_TONE;
    default:        return NX_NONE;
    }
}

static void ctrl_add(int group, int type, int idx, int kind, int axis,
                     const char *label, const char *hint)
{
    if (nctrl >= CTRL_MAX) return;
    Ctrl *c = &ctrls[nctrl++];
    snprintf(c->label, sizeof c->label, "%s", label);
    c->hint  = hint;
    c->type  = (unsigned char)type;
    c->idx   = (unsigned char)idx;
    c->kind  = (unsigned char)kind;
    c->group = (unsigned char)group;
    c->axis  = (unsigned char)(axis < 0 ? axis_for_kind(kind) : axis);
    grp_count[group]++;
}

/* Rebuilt every frame. It is a few dozen stores and it means the panel can
 * never disagree with the layer it is describing -- change the source and the
 * rows change with it, with no invalidation logic to get wrong. */
static void ctrls_rebuild(void)
{
    int f = focus();
    nctrl = 0;
    for (int i = 0; i < CG_COUNT; i++) { grp_first[i] = 0; grp_count[i] = 0; }

    rack_build(&bb_rack[f], &focus_build);

    /* --- VOICE ---------------------------------------------------------- */
    grp_first[CG_VOICE] = nctrl;
    if (bb_custom[f]) {
        /* Hand-written expression: the rack cannot describe it, so fall back
         * to the raw knobs -- but still laddered and labelled, using the role
         * the compiler inferred from the bytecode.
         *
         * SOURCE stays at the top even though nothing is driving it, because
         * it is the way back: pressing left or right on it picks a source and
         * re-racks the layer. A dedicated key for that would have to be one
         * the knob letters do not already own, and this is more discoverable
         * than any of the survivors would have been. */
        ctrl_add(CG_VOICE, CT_SOURCE, 0, KV_LINEAR, NX_NONE, "SOURCE",
                 "this layer is hand-written, so no source describes it. "
                 "left/right picks one and rewrites the expression (C undoes).");
        Program *pr = cur_prog();
        for (int i = 0; i < BB_NPARAM; i++) {
            int role = pr ? pr->role[i] : 0;
            int kind = knob_kind_for_role(role);
            char lab[12];
            snprintf(lab, sizeof lab, "p%d %s", i, knob_kind_name(kind));
            ctrl_add(CG_VOICE, CT_PARAM, i, kind, -1, lab,
                     "raw knob. its role is worked out from how the compiled "
                     "expression uses it; dimmed means the expression never "
                     "reads it.");
        }
    } else {
        ctrl_add(CG_VOICE, CT_SOURCE, 0, KV_LINEAR, NX_NONE, "SOURCE",
                 "what generates the raw signal. changing it rewrites the "
                 "expression and re-seeds the slots below.");
        ctrl_add(CG_VOICE, CT_STAGES, 0, KV_LINEAR, NX_NONE, "STAGES",
                 "optional stages wrapped around the source: BODY is a "
                 "lowpass, SPACE is a feedback delay.");
        for (int i = 0; i < focus_build.nslot; i++)
            ctrl_add(CG_VOICE, CT_PARAM, focus_build.slot[i].pidx,
                     focus_build.slot[i].kind, -1,
                     focus_build.slot[i].label, focus_build.slot[i].hint);
    }

    /* --- POST ----------------------------------------------------------- */
    grp_first[CG_POST] = nctrl;
    ctrl_add(CG_POST, CT_MODE, 0, KV_LINEAR, NX_NONE, "MODE",
             "how the expression's integer becomes a sample. BYTE is the "
             "classic 8-bit sound; WORD is full range and much cleaner.");
    ctrl_add(CG_POST, CT_LCTL, LCTL_LEVEL,   KV_AMOUNT, NX_NONE, "LEVEL",
             "how much of this layer reaches the master bus.");
    ctrl_add(CG_POST, CT_LCTL, LCTL_DRIVE,   KV_AMOUNT, NX_GRIT, "DRIVE",
             "gain into a hard clip. turns a thin signal into a wall.");
    ctrl_add(CG_POST, CT_LCTL, LCTL_TONE,    KV_CUT,    NX_TONE, "TONE",
             "lowpass over the whole layer, after DRIVE. the master darkness "
             "control.");
    ctrl_add(CG_POST, CT_LCTL, LCTL_CRUSH,   KV_AMOUNT, NX_GRIT, "CRUSH",
             "sample-and-hold. drops the effective sample rate and adds the "
             "aliasing that comes with it.");
    ctrl_add(CG_POST, CT_LCTL, LCTL_SPC_TIME, KV_TIME, NX_NONE, "SP-TIME",
             "free delay length. SP-SYNC replaces it with a clock division.");
    ctrl_add(CG_POST, CT_LCTL, LCTL_SPC_SYNC, KV_LINEAR, NX_NONE, "SP-SYNC",
             "tempo division for SPACE. zero is free milliseconds; the rest "
             "stay locked to the transport.");
    ctrl_add(CG_POST, CT_LCTL, LCTL_SPC_FB,  KV_AMOUNT, NX_NONE, "SP-FB",
             "how much of the delay feeds back. capped below unity so it "
             "cannot run away.");
    ctrl_add(CG_POST, CT_LCTL, LCTL_SPC_MIX, KV_AMOUNT, NX_NONE, "SP-MIX",
             "how much delay is mixed in. at zero the delay stage is off.");
    ctrl_add(CG_POST, CT_LCTL, LCTL_SPC_FREEZE, KV_LINEAR, NX_NONE, "FREEZE",
             "stop admitting new sound and recirculate the current SPACE "
             "contents at unity.");

    /* --- SHAPE / TRANSPORT ---------------------------------------------- */
    grp_first[CG_XPORT] = nctrl;
    ctrl_add(CG_XPORT, CT_SEQON, 0, KV_LINEAR, NX_NONE, "SEQ",
             "gate this layer from the step grid. v edits the pattern.");
    ctrl_add(CG_XPORT, CT_LCTL, LCTL_STEPS, KV_LINEAR, NX_NONE, "STEPS",
             "pattern length. 16 is one bar of 16ths; 7 or 5 will drift "
             "against the other layers.");
    ctrl_add(CG_XPORT, CT_LCTL, LCTL_DECAY, KV_DECAY, NX_RATE, "DECAY",
             "how fast each gate falls. this is what turns a tone into a hit "
             "-- short DECAY plus the noise source is a snare.");
    ctrl_add(CG_XPORT, CT_GCTL, LCTL_COUNT + GCTL_BPM,   KV_LINEAR, NX_NONE, "BPM",
             "tempo. drives bt/bl in expressions and the step grid.");
    ctrl_add(CG_XPORT, CT_GCTL, LCTL_COUNT + GCTL_BEATS, KV_LINEAR, NX_NONE, "BEATS",
             "beats per bar.");
    ctrl_add(CG_XPORT, CT_GCTL, LCTL_COUNT + GCTL_BARS,  KV_LINEAR, NX_NONE, "BARS",
             "bars per loop. sets ll, the period of k.");
    ctrl_add(CG_XPORT, CT_RATE, 0, KV_LINEAR, NX_NONE, "RATE",
             "sample rate. lowering it is a tone control -- everything gets "
             "darker and grittier at once.");
    ctrl_add(CG_XPORT, CT_GCTL, LCTL_COUNT + GCTL_ZOOM,  KV_LINEAR, NX_NONE, "ZOOM",
             "scope time-scale. no effect on the sound.");

    if (cursor >= nctrl) cursor = nctrl - 1;
    if (cursor < 0)      cursor = 0;
}

/* ======================================================================== */
/*  reading a control                                                       */
/* ======================================================================== */

/* value text (what the control is set to) and unit text (what that means in
 * the physical world). Either may come back empty. */
static void ctrl_read(const Ctrl *c, char *val, size_t vn, char *unit, size_t un)
{
    int f = focus();
    val[0] = unit[0] = '\0';

    switch (c->type) {
    case CT_SOURCE:
        snprintf(val, vn, "%s", rack_src_name(bb_rack[f].src));
        snprintf(unit, un, "%d/%d", bb_rack[f].src + 1, rack_nsrc());
        break;

    case CT_STAGES: {
        int b = bb_rack[f].body, s = bb_rack[f].space;
        snprintf(val, vn, "%s", b && s ? "both" : b ? "body" : s ? "space" : "dry");
        break;
    }

    case CT_MODE:
        snprintf(val, vn, "%s", mode_name(atomic_load(&FL()->mode)));
        break;

    case CT_SEQON:
        snprintf(val, vn, "%s", atomic_load(&FL()->seq_on) ? "on" : "off");
        break;

    case CT_RATE: {
        int r = atomic_load(&bb.rate);
        snprintf(val, vn, "%d", r);
        snprintf(unit, un, "%dkHz", r / 1000);
        break;
    }

    case CT_PARAM: {
        int v = atomic_load(&FL()->param[c->idx]);
        knob_fmt_value(c->kind, v, val, vn);
        knob_fmt_unit(c->kind, v, unit, un);
        break;
    }

    case CT_LCTL: case CT_GCTL: {
        int v = ctl_get(c->idx);
        snprintf(val, vn, "%d", v);

        /* Each of these conversions is copied from the code that consumes
         * the value, so the number on screen is the number the DSP uses. */
        switch (c->idx) {
        case LCTL_LEVEL:    snprintf(unit, un, "%d%%", v * 100 / 256); break;
        case LCTL_DRIVE: {  /* gain is (256 + drive*8)/256 */
            int x10 = (256 + v * 8) * 10 / 256;
            snprintf(unit, un, "%d.%dx", x10 / 10, x10 % 10);
            break;
        }
        /* Same filter as lp(), so the same formatter -- if the two ever
         * disagreed one of them would be lying. */
        case LCTL_TONE: knob_fmt_unit(KV_CUT, v, unit, un); break;
        case LCTL_CRUSH: {  /* hold length is 1 + crush/4 samples */
            if (v == 0) { snprintf(unit, un, "off"); break; }
            int hz = atomic_load(&bb.rate) / (1 + v / 4);
            if (hz >= 1000) snprintf(unit, un, "%d.%dk", hz / 1000, (hz % 1000) / 100);
            else            snprintf(unit, un, "%dHz", hz);
            break;
        }
        case LCTL_SPC_TIME:
            if (atomic_load(&FL()->ctl[LCTL_SPC_SYNC]))
                snprintf(unit, un, "free only");
            else
                snprintf(unit, un, "%dms", 20 + v * 3);
            break;
        case LCTL_SPC_FB:   snprintf(unit, un, "%d%%", (v > 244 ? 244 : v) * 100 / 256); break;
        case LCTL_SPC_MIX:  snprintf(unit, un, v ? "%d%%" : "off", v * 100 / 256); break;
        case LCTL_STEPS:    snprintf(unit, un, "%d/16", v); break;

        case LCTL_DECAY: knob_fmt_unit(KV_DECAY, v, unit, un); break;
        case LCTL_SPC_SYNC:
            snprintf(val, vn, "%s", space_sync_name(v));
            snprintf(unit, un, v ? "clocked" : "ms");
            break;
        case LCTL_SPC_FREEZE:
            snprintf(val, vn, "%s", v ? "ON" : "off");
            snprintf(unit, un, v ? "held" : "live");
            break;

        /* BEATS reports the bar it makes and BARS the loop it makes, so the
         * two rows say different things and you can read the transport as
         * "this long, then this long" instead of the same number twice. */
        default: {
            int bpm = atomic_load(&bb.gctl[GCTL_BPM]);
            int be  = atomic_load(&bb.gctl[GCTL_BEATS]);
            int ba  = atomic_load(&bb.gctl[GCTL_BARS]);
            if (bpm <= 0) break;

            int ms = 0;
            if (c->idx == LCTL_COUNT + GCTL_BPM)        ms = 60000 / bpm;
            else if (c->idx == LCTL_COUNT + GCTL_BEATS) ms = 60000 * be / bpm;
            else if (c->idx == LCTL_COUNT + GCTL_BARS)  ms = 60000 * be * ba / bpm;
            else break;

            if (ms >= 1000) snprintf(unit, un, "%d.%ds", ms / 1000, (ms % 1000) / 100);
            else            snprintf(unit, un, "%dms", ms);
            break;
        }
        }
        break;
    }
    }
}

/* Bar fill for a control, 0..width. Controls with no meaningful continuum
 * (source, mode, on/off) get -1 and draw no bar at all. */
static int ctrl_fill(const Ctrl *c, int width)
{
    switch (c->type) {
    case CT_PARAM:
        return knob_fill(c->kind, atomic_load(&FL()->param[c->idx]), width);
    case CT_LCTL: case CT_GCTL: {
        const CtlInfo *ci = ctl_info(c->idx);
        int v = ctl_get(c->idx) - ci->lo, span = ci->hi - ci->lo;
        return span > 0 ? v * width / span : 0;
    }
    default:
        return -1;
    }
}

/* ======================================================================== */
/*  editor and voice application                                            */
/* ======================================================================== */

static void ed_set(const char *s)
{
    snprintf(ed, sizeof ed, "%s", s ? s : "");
    ed_len = (int)strlen(ed);
    ed_cur = ed_len;
    ed_scroll = 0;
}

/* Recompile and, if valid, publish to the focused layer. A failed compile
 * changes nothing -- the last good program keeps playing, which is the whole
 * reason live coding a running instrument is tolerable. */
static void ed_publish(void)
{
    int f = focus();
    snprintf(bb_expr[f], BB_EXPR_MAX, "%s", ed);
    bb_publish(f, ed, &last_err);
}

/* Typing detaches the layer from its rack: the text no longer corresponds to
 * any source, and pretending otherwise would mean the panel lies about what
 * the knobs do. */
static void ed_commit(void)
{
    bb_custom[focus()] = 1;
    ed_publish();
}

/* Re-render the expression from the rack and publish it. Called whenever a
 * choice that changes the SHAPE of the expression moves. */
static void rack_republish(int reseed)
{
    int f = focus();
    RackBuild b;
    rack_build(&bb_rack[f], &b);
    if (reseed) {
        int p[BB_NPARAM];
        for (int i = 0; i < BB_NPARAM; i++) p[i] = atomic_load(&FL()->param[i]);
        rack_seed_params(&b, p);
        for (int i = 0; i < BB_NPARAM; i++) atomic_store(&FL()->param[i], p[i]);
    }
    bb_custom[f] = 0;
    ed_set(b.expr);
    ed_publish();
}

static void voice_capture(Voice *v)
{
    int f = focus();
    Layer *l = FL();
    memset(v, 0, sizeof *v);
    v->rack   = bb_rack[f];
    v->custom = bb_custom[f];
    snprintf(v->expr, sizeof v->expr, "%s", ed);
    v->mode   = atomic_load(&l->mode);
    v->seq_on = atomic_load(&l->seq_on);
    v->seed   = last_seed[f];
    for (int i = 0; i < BB_NPARAM; i++)   v->p[i]     = atomic_load(&l->param[i]);
    for (int i = 0; i < LCTL_COUNT; i++)  v->ctl[i]   = atomic_load(&l->ctl[i]);
    for (int i = 0; i < BB_STEPS; i++)    v->gate[i]  = atomic_load(&l->seq_gate[i]);
    for (int i = 0; i < BB_STEPS; i++)    v->pitch[i] = atomic_load(&l->seq_pitch[i]);
    for (int i = 0; i < BB_STEPS; i++)    v->ratchet[i] = atomic_load(&l->seq_ratchet[i]);
    for (int i = 0; i < BB_STEPS; i++)    v->prob[i] = atomic_load(&l->seq_prob[i]);
    for (int k = 0; k < BB_LOCK_COUNT; k++)
        for (int i = 0; i < BB_STEPS; i++)
            v->lock[k][i] = atomic_load(&l->seq_lock[k][i]);
    v->motion_mask = atomic_load(&l->motion_mask);
}

static void voice_apply(const Voice *v)
{
    int f = focus();
    Layer *l = FL();
    bb_rack[f]   = v->rack;
    bb_custom[f] = v->custom;
    ed_set(v->expr);
    ed_publish();
    atomic_store(&l->mode, v->mode);
    atomic_store(&l->seq_on, v->seq_on);
    for (int i = 0; i < BB_NPARAM; i++)  atomic_store(&l->param[i], v->p[i]);
    for (int i = 0; i < LCTL_COUNT; i++) {
        const CtlInfo *ci = &bb_lctl_info[i];
        atomic_store(&l->ctl[i], bb_clampi(v->ctl[i], ci->lo, ci->hi));
    }
    for (int i = 0; i < BB_STEPS; i++) atomic_store(&l->seq_gate[i], v->gate[i]);
    for (int i = 0; i < BB_STEPS; i++) atomic_store(&l->seq_pitch[i], v->pitch[i]);
    for (int i = 0; i < BB_STEPS; i++) atomic_store(&l->seq_ratchet[i], bb_clampi(v->ratchet[i], 1, 4));
    for (int i = 0; i < BB_STEPS; i++) atomic_store(&l->seq_prob[i], bb_clampi(v->prob[i], 0, 100));
    for (int k = 0; k < BB_LOCK_COUNT; k++)
        for (int i = 0; i < BB_STEPS; i++)
            atomic_store(&l->seq_lock[k][i], bb_clampi(v->lock[k][i], -1, 256));
    atomic_store(&l->motion_mask, v->motion_mask & ((1u << BB_LOCK_COUNT) - 1u));
    atomic_store(&l->on, 1);
}

static void snapshot(void)
{
    int f = focus();
    voice_capture(&prev_voice[f]);
    has_prev[f] = 1;
}

static void focus_layer(int n)
{
    if (n < 0 || n >= BB_NLAYER) return;
    if (motion_record && n != motion_layer) motion_record = 0;
    snprintf(bb_expr[focus()], BB_EXPR_MAX, "%s", ed);
    atomic_store(&bb.focus, n);
    ed_set(bb_expr[n]);
    last_err.ok = 1;
    ui_status("layer %d%s", n + 1,
              atomic_load(&bb.layer[n].on) ? "" : "  (off -- shift+number to enable)");
}

static void do_generate(int mutate)
{
    int f = focus();
    Voice v;
    unsigned seed;

    snapshot();

    if (mutate && last_seed[f]) seed = gen_mutate(last_seed[f], nextrand(), &v);
    else                        seed = gen_roll(nextrand(), &v);

    last_seed[f] = seed;
    voice_apply(&v);
    ui_status("layer %d  %s  %s  seed %u  level %d%%   (P mutate, C undo)",
              f + 1, mutate ? "mutated" : "generated",
              rack_src_name(v.rack.src), seed, v.level);
}

static void do_undo(void)
{
    int f = focus();
    if (!has_prev[f]) { ui_status("nothing to undo on this layer"); return; }
    Voice v = prev_voice[f];
    snapshot();
    last_seed[f] = v.seed;
    voice_apply(&v);
    ui_status("layer %d reverted", f + 1);
}

void ui_load_example(int i)
{
    if (N_EXAMPLES <= 0) return;
    while (i < 0) i += N_EXAMPLES;
    i %= N_EXAMPLES;
    cur_example = i;
    const Example *e = &EXAMPLES[i];

    Voice v;
    memset(&v, 0, sizeof v);
    /* Examples are hand-written expressions -- that is the whole point of
     * them -- so they land as custom and the panel shows raw knobs. */
    v.custom = 1;
    rack_default(&v.rack);
    snprintf(v.expr, sizeof v.expr, "%s", e->expr);
    v.mode = e->mode;
    for (int j = 0; j < BB_NPARAM; j++) v.p[j] = e->p[j];
    v.ctl[LCTL_LEVEL]    = 200;
    v.ctl[LCTL_DRIVE]    = e->drive;
    v.ctl[LCTL_TONE]     = e->tone;
    v.ctl[LCTL_CRUSH]    = e->crush;
    v.ctl[LCTL_SPC_TIME] = e->spc_time;
    v.ctl[LCTL_SPC_FB]   = e->spc_fb;
    v.ctl[LCTL_SPC_MIX]  = e->spc_mix;
    v.ctl[LCTL_STEPS]    = 16;
    v.ctl[LCTL_SPC_SYNC] = 0;
    v.ctl[LCTL_SPC_FREEZE] = 0;
    for (int st = 0; st < BB_STEPS; st++) {
        v.ratchet[st] = 1;
        v.prob[st] = 100;
        for (int k = 0; k < BB_LOCK_COUNT; k++) v.lock[k][st] = -1;
    }

    snapshot();
    voice_apply(&v);
    atomic_store(&bb.gctl[GCTL_BPM],   e->bpm);
    atomic_store(&bb.gctl[GCTL_BEATS], e->beats);
    atomic_store(&bb.gctl[GCTL_BARS],  e->bars);
    ui_status("[%d/%d] %s -- %s", i + 1, N_EXAMPLES, e->name, e->desc);
}

/* ======================================================================== */
/*  adjusting                                                               */
/* ======================================================================== */

static void retune(int newrate)
{
    char err[200];
    newrate = bb_clampi(newrate, BB_RATE_MIN, BB_RATE_MAX);
    if (audio_retune(newrate, err, sizeof err) < 0) ui_status("%s", err);
    else ui_status("sample rate %d", atomic_load(&bb.rate));
}

static void ctrl_adjust(int ci, int dir, int coarse)
{
    if (ci < 0 || ci >= nctrl) return;
    const Ctrl *c = &ctrls[ci];
    int f = focus();

    switch (c->type) {

    case CT_SOURCE: {
        int n = rack_nsrc();
        /* Leaving a hand-written expression throws it away, so make sure
         * there is something for C to come back to. Cycling between sources
         * is not destructive in the same way and does not need it. */
        int leaving_custom = bb_custom[f];
        if (leaving_custom) snapshot();
        else                bb_rack[f].src = (unsigned char)((bb_rack[f].src + n + dir) % n);

        bb_rack[f].mode = (unsigned char)rack_src_mode(bb_rack[f].src);
        atomic_store(&FL()->mode, bb_rack[f].mode);
        rack_republish(1);
        if (rack_src_triggered(bb_rack[f].src)) {
            int any = 0;
            for (int i = 0; i < BB_STEPS; i++)
                if (atomic_load(&FL()->seq_gate[i])) any = 1;
            if (!any) {
                int gate[BB_STEPS];
                gen_euclid(16, 4, gate);
                for (int i = 0; i < BB_STEPS; i++)
                    atomic_store(&FL()->seq_gate[i], gate[i]);
            }
            atomic_store(&FL()->seq_on, 1);
            if (atomic_load(&FL()->ctl[LCTL_DECAY]) == 0)
                atomic_store(&FL()->ctl[LCTL_DECAY], 172);
        }
        ui_status("%s -- %s%s", rack_src_name(bb_rack[f].src),
                  rack_src_desc(bb_rack[f].src),
                  leaving_custom ? "   (C undoes)" : "");
        break;
    }

    /* Four combinations in a fixed order, so left/right walks dry -> body ->
     * space -> both and back rather than toggling two things at once. */
    case CT_STAGES: {
        int cur = (bb_rack[f].body ? 1 : 0) | (bb_rack[f].space ? 2 : 0);
        cur = (cur + 4 + dir) % 4;
        bb_rack[f].body  = (unsigned char)(cur & 1);
        bb_rack[f].space = (unsigned char)((cur >> 1) & 1);
        rack_republish(1);
        break;
    }

    case CT_MODE: {
        int m = (atomic_load(&FL()->mode) + BB_NMODE + dir) % BB_NMODE;
        atomic_store(&FL()->mode, m);
        if (!bb_custom[f]) bb_rack[f].mode = (unsigned char)m;
        break;
    }

    case CT_SEQON: {
        int on = !atomic_load(&FL()->seq_on);
        atomic_store(&FL()->seq_on, on);
        if (on && ctl_get(LCTL_DECAY) == 0) ctl_set(LCTL_DECAY, 90);
        break;
    }

    /* Sample rate moves geometrically: a fixed step would be a rounding
     * error at 96k and a total transformation at 2k. */
    case CT_RATE: {
        int r = atomic_load(&bb.req_rate);
        int step = coarse ? r : r / 16 + 1;
        retune(dir > 0 ? r + step : r - step);
        break;
    }

    case CT_PARAM: {
        int v = atomic_load(&FL()->param[c->idx]);
        atomic_store(&FL()->param[c->idx], knob_step(c->kind, v, dir, coarse));
        break;
    }

    /* TONE and DECAY are as logarithmic in their effect as any knob in the
     * VOICE column, so they step along a ladder too. The rest -- level, mix,
     * tempo, step count -- really are linear in what they do, and a fixed
     * step is the honest treatment. */
    case CT_LCTL: case CT_GCTL: {
        const CtlInfo *in = ctl_info(c->idx);
        int v = ctl_get(c->idx);
        if (c->kind == KV_CUT || c->kind == KV_DECAY)
            v = knob_step(c->kind, v, dir, coarse);
        else
            v += dir * (coarse ? in->coarse : in->step);
        ctl_set(c->idx, v);
        break;
    }
    }
}

/* ---- directional moves --------------------------------------------------
 * The generator can only teleport: press p and you are somewhere else with no
 * path back to where you were. These are the opposite -- one small, audible,
 * reversible step along a named axis, applied to whatever controls in the
 * current voice happen to serve that axis.
 *
 * Working over KINDS rather than over specific knobs is what makes this work
 * for hand-written expressions too: an expression whose p3 the compiler
 * classified as a cutoff gets moved by "darker" without the rack knowing
 * anything about it.
 */
static const char *nudge_name(int axis, int dir)
{
    switch (axis) {
    case NX_TONE:  return dir > 0 ? "brighter" : "darker";
    case NX_GRIT:  return dir > 0 ? "dirtier"  : "cleaner";
    case NX_PITCH: return dir > 0 ? "higher"   : "lower";
    default:       return dir > 0 ? "faster"   : "slower";
    }
}

static void do_nudge(int axis, int dir)
{
    int moved = 0;
    char touched[96] = "";

    for (int i = 0; i < nctrl; i++) {
        const Ctrl *c = &ctrls[i];
        if (c->axis != axis) continue;

        /* A bigger shift means a SLOWER modulator, so that one ladder runs
         * backwards against the axis. Getting this the wrong way round makes
         * "faster" audibly do the opposite, which is worse than not having
         * the key at all. */
        int d = (c->kind == KV_SHIFT) ? -dir : dir;

        ctrl_adjust(i, d, 0);
        moved++;
        if (strlen(touched) + strlen(c->label) + 2 < sizeof touched) {
            if (touched[0]) strcat(touched, " ");
            strcat(touched, c->label);
        }
    }

    if (!moved) ui_status("%s -- nothing in this voice responds to that",
                          nudge_name(axis, dir));
    else        ui_status("%s: %s", nudge_name(axis, dir), touched);
}

/* ======================================================================== */
/*  parameter locks and motion recording                                    */
/* ======================================================================== */

static const int LOCK_LCTL[] = {
    LCTL_LEVEL, LCTL_DRIVE, LCTL_TONE, LCTL_CRUSH,
    LCTL_SPC_TIME, LCTL_SPC_FB, LCTL_SPC_MIX, LCTL_DECAY
};
static const char *const LOCK_CTL_NAME[] = {
    "LEVEL", "DRIVE", "TONE", "CRUSH", "SP-TIME", "SP-FB", "SP-MIX", "DECAY"
};

static int lock_target_for_ctrl(const Ctrl *c)
{
    if (!c) return -1;
    if (c->type == CT_PARAM) return c->idx;
    if (c->type == CT_LCTL) {
        for (int i = 0; i < (int)(sizeof LOCK_LCTL / sizeof LOCK_LCTL[0]); i++)
            if (c->idx == LOCK_LCTL[i]) return LOCK_LEVEL + i;
    }
    return -1;
}

static int lock_kind(int layer, int target)
{
    if (target < BB_NPARAM) {
        if (!bb_custom[layer]) {
            RackBuild b;
            rack_build(&bb_rack[layer], &b);
            int si = rack_slot_for_param(&b, target);
            if (si >= 0) return b.slot[si].kind;
        }
        Program *pr = atomic_load(&bb.layer[layer].prog);
        return knob_kind_for_role(pr ? pr->role[target] : ROLE_NONE);
    }
    switch (LOCK_LCTL[target - LOCK_LEVEL]) {
    case LCTL_TONE:     return KV_CUT;
    case LCTL_SPC_TIME: return KV_TIME;
    case LCTL_DECAY:    return KV_DECAY;
    default:            return KV_AMOUNT;
    }
}

static void lock_name(int layer, int target, char *buf, size_t n)
{
    if (target < BB_NPARAM) {
        if (!bb_custom[layer]) {
            RackBuild b;
            rack_build(&bb_rack[layer], &b);
            int si = rack_slot_for_param(&b, target);
            if (si >= 0) {
                snprintf(buf, n, "%s", b.slot[si].label);
                return;
            }
        }
        snprintf(buf, n, "p%d", target);
        return;
    }
    snprintf(buf, n, "%s", LOCK_CTL_NAME[target - LOCK_LEVEL]);
}

static int lock_current(int layer, int target)
{
    Layer *l = &bb.layer[layer];
    if (target < BB_NPARAM) return atomic_load(&l->param[target]);
    return atomic_load(&l->ctl[LOCK_LCTL[target - LOCK_LEVEL]]);
}

static int lock_step_value(int layer, int target, int value, int dir, int coarse)
{
    if (value < 0) value = lock_current(layer, target);
    int kind = lock_kind(layer, target);
    if (target < BB_NPARAM || kind == KV_CUT || kind == KV_DECAY || kind == KV_TIME)
        return knob_step(kind, value, dir, coarse);

    const CtlInfo *ci = &bb_lctl_info[LOCK_LCTL[target - LOCK_LEVEL]];
    return bb_clampi(value + dir * (coarse ? ci->coarse : ci->step), ci->lo, ci->hi);
}

static void motion_toggle(void)
{
    if (motion_record) {
        motion_record = 0;
        ui_status("motion loop written");
        return;
    }
    int target = lock_target_for_ctrl(cursor >= 0 && cursor < nctrl ? &ctrls[cursor] : NULL);
    if (target < 0) {
        ui_status("select a VOICE or POST sound control before recording motion");
        return;
    }
    if (!atomic_load(&FL()->seq_on)) atomic_store(&FL()->seq_on, 1);
    motion_layer = focus();
    motion_target = target;
    motion_record = 1;
    atomic_fetch_or(&bb.layer[motion_layer].motion_mask, 1u << motion_target);
    char name[24];
    lock_name(motion_layer, motion_target, name, sizeof name);
    ui_status("MOTION REC L%d %s -- turn it, M stops", motion_layer + 1, name);
}

static void motion_service(void)
{
    if (!motion_record || motion_layer < 0 || motion_target < 0) return;
    int step = atomic_load(&bb.seq_pos);
    if (step < 0) return;
    int steps = bb_clampi(atomic_load(&bb.layer[motion_layer].ctl[LCTL_STEPS]),
                          1, BB_STEPS);
    step %= steps;
    atomic_store(&bb.layer[motion_layer].seq_lock[motion_target][step],
                 lock_current(motion_layer, motion_target));
}

/* ======================================================================== */
/*  sequencer editing                                                       */
/* ======================================================================== */

static void seq_clear(void)
{
    Layer *l = FL();
    for (int i = 0; i < BB_STEPS; i++) {
        atomic_store(&l->seq_gate[i], GATE_OFF);
        atomic_store(&l->seq_pitch[i], 0);
        atomic_store(&l->seq_ratchet[i], 1);
        atomic_store(&l->seq_prob[i], 100);
        for (int k = 0; k < BB_LOCK_COUNT; k++)
            atomic_store(&l->seq_lock[k][i], -1);
    }
    atomic_store(&l->motion_mask, 0);
}

static void seq_random(void)
{
    Layer *l = FL();
    int n = ctl_get(LCTL_STEPS);
    for (int i = 0; i < BB_STEPS; i++) {
        int on = (i < n) && ((nextrand() % 100u) < 38u);
        atomic_store(&l->seq_gate[i],
                     on ? (((nextrand() % 100u) < 25u) ? GATE_ACCENT : GATE_ON)
                        : GATE_OFF);
        atomic_store(&l->seq_ratchet[i],
                     on && (nextrand() % 100u) < 15u ? 2 + (int)(nextrand() % 3u) : 1);
        atomic_store(&l->seq_prob[i],
                     on && (nextrand() % 100u) < 18u ? 55 + (int)(nextrand() % 46u) : 100);
    }
    atomic_store(&l->seq_gate[0], GATE_ACCENT);   /* always land on the one */
}

static void seq_euclid(int pulses)
{
    int gate[BB_STEPS];
    gen_euclid(ctl_get(LCTL_STEPS), pulses, gate);
    for (int i = 0; i < BB_STEPS; i++) atomic_store(&FL()->seq_gate[i], gate[i]);
}

/* ======================================================================== */
/*  drawing                                                                 */
/* ======================================================================== */

static void draw_title(void)
{
    int f = focus();
    putf(0, 1, attr_for(C_FRAME, 1), "bytebeat");
    putf(0, 11, attr_for(C_SEL, 1), "L%d", f + 1);
    putf(0, 14, attr_for(C_HOT, 1), "%-9s",
         bb_custom[f] ? "custom" : rack_src_name(bb_rack[f].src));

    const char *mn = mode_ui == M_INSERT ? "INSERT"
                   : mode_ui == M_SEQ    ? "SEQ" : "NORMAL";
    if (mode_ui == M_PERF) mn = "PERFORM";
    char right[200];
    int  st = sink_net_state();
    int lst = atomic_load(&bb.loop_status);
    snprintf(right, sizeof right, "%s%s%s%s%s%s%s%s%s [%s] ",
             atomic_load(&bb.clipping) ? "CLIP " : "",
             atomic_load(&bb.panic) ? "PANIC " : "",
             atomic_load(&bb.mute)  ? "MUTE "  : "",
             atomic_load(&FL()->ctl[LCTL_SPC_FREEZE]) ? "FREEZE " : "",
             motion_record ? "MOTION " : "",
             lst != LOOP_OFF ? loop_status_name(lst) : "",
             lst != LOOP_OFF ? " " : "",
             sink_rec_active()      ? "REC "   : "",
             st == 2 ? "NET " : (st == 1 ? "net " : ""),
             mn);
    int x = COLS - (int)strlen(right);
    if (x < 24) x = 24;
    put(0, x, attr_for(atomic_load(&bb.panic) ? C_ERR : C_SEL, 1), right);

    char line[512];
    int  w = COLS < (int)sizeof line - 1 ? COLS : (int)sizeof line - 1;
    memset(line, '-', (size_t)w);
    line[w] = '\0';
    put(1, 0, dim_attr(C_FRAME), line);
}

/* The expression, with every pN coloured by what that knob does. Turning
 * GRAIN and watching p1 light up in the text is how the language stops being
 * opaque -- the panel and the expression are visibly the same thing. */
static void draw_expr(int y)
{
    char label[16];
    snprintf(label, sizeof label, "L%d > ", focus() + 1);
    int lx = (int)strlen(label);
    int w  = COLS - lx - 1;
    if (w < 8) w = 8;

    if (ed_cur < ed_scroll)         ed_scroll = ed_cur;
    if (ed_cur > ed_scroll + w - 1) ed_scroll = ed_cur - w + 1;
    if (ed_scroll < 0)              ed_scroll = 0;

    put(y, 0, attr_for(C_SEL, 1), label);
    clear_row(y);
    put(y, 0, attr_for(C_SEL, 1), label);

    int n = ed_len - ed_scroll;
    if (n < 0) n = 0;
    if (n > w) n = w;

    for (int i = 0; i < n; i++) {
        char ch = ed[ed_scroll + i];
        int a = attr_for(C_VAL, 1);

        /* A pN reference, and not part of a longer identifier. */
        int at = ed_scroll + i;
        if (ch == 'p' && at + 1 < ed_len && ed[at + 1] >= '0' && ed[at + 1] <= '7' &&
            (at == 0 || !((ed[at-1] >= 'a' && ed[at-1] <= 'z') ||
                          (ed[at-1] >= '0' && ed[at-1] <= '9')))) {
            int pi = ed[at + 1] - '0';
            int kind = KV_LINEAR;
            for (int j = 0; j < nctrl; j++)
                if (ctrls[j].type == CT_PARAM && ctrls[j].idx == pi &&
                    ctrls[j].group == CG_VOICE) { kind = ctrls[j].kind; break; }
            a = attr_for(class_pair(knob_class(kind)), 1) | A_UNDERLINE;
        } else if (at > 0 && ed[at-1] == 'p' && ch >= '0' && ch <= '7' &&
                   (at < 2 || !((ed[at-2] >= 'a' && ed[at-2] <= 'z') ||
                                (ed[at-2] >= '0' && ed[at-2] <= '9')))) {
            int kind = KV_LINEAR;
            for (int j = 0; j < nctrl; j++)
                if (ctrls[j].type == CT_PARAM && ctrls[j].idx == ch - '0' &&
                    ctrls[j].group == CG_VOICE) { kind = ctrls[j].kind; break; }
            a = attr_for(class_pair(knob_class(kind)), 1) | A_UNDERLINE;
        }

        char s[2] = { ch, '\0' };
        put(y, lx + i, a, s);
    }
}

static void draw_status(int y)
{
    clear_row(y);

    if (!last_err.ok) {
        int lx = 5;
        int col = last_err.col - ed_scroll;
        if (col >= 0 && lx + col < COLS - 1)
            put(y, lx + col, attr_for(C_ERR, 1), "^");
        put(y, 0, attr_for(C_ERR, 1), "err");
        int mx = lx + (col > 0 ? col : 0) + 2;
        if (mx > COLS - 24) mx = COLS - 24;
        if (mx < 5) mx = 5;
        putf(y, mx, attr_for(C_ERR, 0), "%s", last_err.msg);
    } else if (status_ttl > 0) {
        put(y, 0, attr_for(C_SEL, 0), status);
    } else if (warning[0]) {
        putf(y, 0, attr_for(C_ERR, 0), "! %s", warning);
    } else {
        put(y, 0, dim_attr(C_FRAME), "ok");
    }
}

static void draw_readout(int y)
{
    int rate = atomic_load(&bb.rate);
    int req  = atomic_load(&bb.req_rate);
    int cpu  = atomic_load(&bb.cpu_us);
    int bud  = atomic_load(&bb.budget_us);
    int pct  = bud > 0 ? (cpu * 100) / bud : 0;

    char rbuf[48];
    if (rate == req) snprintf(rbuf, sizeof rbuf, "%d", rate);
    else             snprintf(rbuf, sizeof rbuf, "%d(want %d)", rate, req);

    /* Built into a buffer rather than printed straight out, because the
     * stream address goes after it and its length varies with the rate --
     * "sr 15319(want 1000)" is eleven columns longer than "sr 44100". */
    char left[200];
    int n = snprintf(left, sizeof left,
                     "t %-9u sr %-12s gain %3d  xrun %-3d cpu %3d%%  lat %dms",
                     (unsigned)atomic_load(&bb.t), rbuf,
                     atomic_load(&bb.gain), atomic_load(&bb.xruns), pct,
                     rate > 0 ? (audio_period_frames() * 1000) / rate : 0);
    put(y, 0, attr_for(C_VAL, 0), left);

    /* The stream address, when there is one, rather than a whole row telling
     * you there is not. */
    int st = sink_net_state();
    if (!st) return;

    const char *url = sink_stream_url();
    if (!url || !url[0]) url = "stdout (raw s16le)";

    int x = n + 3;
    if (x + 7 + (int)strlen(url) >= COLS) return;   /* no room; the title
                                                     * bar still shows NET */
    put(y, x, dim_attr(C_FRAME), "listen ");
    put(y, x + 7, attr_for(st == 2 ? C_SEL : C_HOT, 1), url);
}

static void draw_layers(int y)
{
    put(y, 0, dim_attr(C_FRAME), "layers");
    int x = 7;
    for (int i = 0; i < BB_NLAYER; i++) {
        int on    = atomic_load(&bb.layer[i].on);
        int empty = bb_expr[i][0] == '\0';
        int foc   = (i == focus());

        char cell[16];
        snprintf(cell, sizeof cell, " %d%s ", i + 1,
                 empty ? "-" : (on ? "*" : "o"));

        int a = foc ? rev_attr(C_SEL)
                    : (empty ? dim_attr(C_FRAME)
                             : (on ? attr_for(C_HOT, 1) : dim_attr(C_VAL)));
        put(y, x, a, cell);
        x += (int)strlen(cell);
    }

    /* Name each non-empty layer's source, so the strip says what the set is
     * made of rather than just how many parts it has. */
    x += 2;
    for (int i = 0; i < BB_NLAYER && x < COLS - 10; i++) {
        if (bb_expr[i][0] == '\0') continue;
        const char *nm = bb_custom[i] ? "custom" : rack_src_name(bb_rack[i].src);
        int a = (i == focus()) ? attr_for(C_SEL, 1)
              : atomic_load(&bb.layer[i].on) ? dim_attr(C_VAL) : dim_attr(C_FRAME);
        putf(y, x, a, "%d:%s", i + 1, nm);
        x += 2 + (int)strlen(nm) + 1;
    }
}

/* The step grid for the focused layer. Three columns per step: a beat marker
 * every four, then the gate character, then a space. */
static void draw_seq(int y)
{
    Layer *l = FL();
    int on   = atomic_load(&l->seq_on);
    int n    = ctl_get(LCTL_STEPS);
    int head = atomic_load(&bb.seq_pos);
    int rows = mode_ui == M_SEQ ? 5 : 2;

    for (int r = 0; r < rows; r++) clear_row(y + r);
    put(y, 0, on ? attr_for(C_FRAME, 0) : dim_attr(C_FRAME), "seq");
    put(y + 1, 0, dim_attr(C_FRAME), "pit");

    char lname[24] = "lock";
    if (mode_ui == M_SEQ) {
        put(y + 2, 0, dim_attr(C_FRAME), "rat");
        put(y + 3, 0, dim_attr(C_FRAME), "prb");
        lock_name(focus(), seq_lock_target, lname, sizeof lname);
        putf(y + 4, 0, dim_attr(C_FRAME), "%-4.4s", lname);
    }

    if (!on) {
        put(y, 5, dim_attr(C_FRAME),
            "off -- SEQ in the right-hand column turns it on, v edits the pattern");
        return;
    }

    for (int st = 0; st < BB_STEPS; st++) {
        int x = 5 + st * 3;
        if (x + 2 >= COLS) break;

        int g   = atomic_load(&l->seq_gate[st]);
        int pit = atomic_load(&l->seq_pitch[st]);
        int live = st < n;
        int ish = (st == head % (n > 0 ? n : 1)) && live;

        if (st % 4 == 0) put(y, x, dim_attr(C_FRAME), "|");

        char c[2];
        c[0] = (g == GATE_ACCENT) ? 'X' : (g == GATE_ON ? 'x' : '.');
        c[1] = '\0';

        int a;
        if (mode_ui == M_SEQ && seq_lane == SL_GATE && st == seq_cur)
                        a = rev_attr(C_SEL);
        else if (!live) a = dim_attr(C_FRAME);
        else if (ish)   a = rev_attr(C_HOT);
        else if (g)     a = attr_for(C_HOT, 1);
        else            a = dim_attr(C_VAL);
        put(y, x + 1, a, c);

        char pb[8];
        if (!live)      snprintf(pb, sizeof pb, "  ");
        else if (pit)   snprintf(pb, sizeof pb, "%+d", pit);
        else            snprintf(pb, sizeof pb, " .");
        int pa = (mode_ui == M_SEQ && seq_lane == SL_PITCH && st == seq_cur)
               ? rev_attr(C_SEL)
               : (pit ? attr_for(C_PITCH, 0) : dim_attr(C_FRAME));
        put(y + 1, x, pa, pb);

        if (mode_ui == M_SEQ) {
            int rat = bb_clampi(atomic_load(&l->seq_ratchet[st]), 1, 4);
            int prob = bb_clampi(atomic_load(&l->seq_prob[st]), 0, 100);
            int lv = atomic_load(&l->seq_lock[seq_lock_target][st]);
            unsigned mm = atomic_load(&l->motion_mask);
            char rb[4], qb[4], lb[3];
            snprintf(rb, sizeof rb, "x%d", rat);
            if (prob >= 100) snprintf(qb, sizeof qb, "--");
            else             snprintf(qb, sizeof qb, "%02d", prob);
            snprintf(lb, sizeof lb, "%c", lv < 0 ? '.' : ((mm >> seq_lock_target) & 1u) ? '~' : 'o');

            int ra = (seq_lane == SL_RATCHET && st == seq_cur) ? rev_attr(C_SEL)
                   : (rat > 1 ? attr_for(C_TIME, 1) : dim_attr(C_FRAME));
            int qa = (seq_lane == SL_PROB && st == seq_cur) ? rev_attr(C_SEL)
                   : (prob < 100 ? attr_for(C_WAVE, 1) : dim_attr(C_FRAME));
            int la = (seq_lane == SL_LOCK && st == seq_cur) ? rev_attr(C_SEL)
                   : (lv >= 0 ? attr_for(C_SEL, 1) : dim_attr(C_FRAME));
            put(y + 2, x, ra, rb);
            put(y + 3, x, qa, qb);
            put(y + 4, x + 1, la, lb);
        }
    }
}

/* ---- the panel ---------------------------------------------------------- */

static const char *GROUP_NAME[CG_COUNT] = { "VOICE", "POST", "SHAPE" };

static void draw_panel(int y0, int rows)
{
    /* label 9 + value 5 + unit 6 + three separators = 23 fixed columns; the
     * bar gets what is left, and disappears entirely on a narrow terminal
     * rather than pushing the numbers off the edge. */
    int colw = COLS / CG_COUNT;
    if (colw < 22) colw = 22;
    int barw = colw - 24;
    if (barw < 0)  barw = 0;
    if (barw > 16) barw = 16;

    int f = focus();

    /* header */
    for (int g = 0; g < CG_COUNT; g++) {
        int x = g * colw;
        if (x + 8 >= COLS) break;
        char head[128];
        int  hl = colw - 2;
        if (hl > (int)sizeof head - 1) hl = (int)sizeof head - 1;
        memset(head, '-', (size_t)hl);
        head[hl] = '\0';
        put(y0, x, dim_attr(C_FRAME), head);
        putf(y0, x, attr_for(C_FRAME, 1), " %s ", GROUP_NAME[g]);
    }

    /* Custom layers get told, on the panel itself, why they are not looking
     * at named slots and how to get back. */
    if (bb_custom[f]) {
        int x = (int)strlen(" VOICE ") + 1;
        if (x + 24 < colw) putf(y0, x, dim_attr(C_ERR), "hand-written");
    }

    for (int i = 0; i < nctrl; i++) {
        const Ctrl *c = &ctrls[i];
        int row = i - grp_first[c->group];
        if (row >= rows) continue;

        int x = c->group * colw;
        int y = y0 + 1 + row;
        if (y >= LINES || x + 18 >= COLS) continue;

        char val[24], unit[24];
        ctrl_read(c, val, sizeof val, unit, sizeof unit);

        int sel = (i == cursor);
        int cls = knob_class(c->kind);

        /* Dim what cannot currently do anything. A knob the expression never
         * mentions and a delay time with the mix at zero should not look
         * identical to one that is live. */
        int inert = 0;
        if (c->type == CT_PARAM && bb_custom[f]) {
            Program *pr = cur_prog();
            inert = !(pr && ((pr->used_p >> c->idx) & 1u));
        }
        if (c->type == CT_LCTL && c->idx == LCTL_SPC_TIME)
            inert = ctl_get(LCTL_SPC_MIX) == 0 || ctl_get(LCTL_SPC_SYNC) != 0;
        if (c->type == CT_LCTL && c->idx == LCTL_SPC_FB)
            inert = ctl_get(LCTL_SPC_MIX) == 0 || ctl_get(LCTL_SPC_FREEZE) != 0;
        if (c->type == CT_LCTL &&
            (c->idx == LCTL_SPC_SYNC || c->idx == LCTL_SPC_FREEZE))
            inert = ctl_get(LCTL_SPC_MIX) == 0;
        if ((c->idx == LCTL_STEPS || c->idx == LCTL_DECAY) && c->type == CT_LCTL)
            inert = !atomic_load(&FL()->seq_on);

        int la = sel ? rev_attr(C_SEL) : (inert ? dim_attr(C_FRAME) : attr_for(C_FRAME, 0));
        int va = inert ? dim_attr(C_FRAME) : attr_for(C_VAL, 1);
        int ua = inert ? dim_attr(C_FRAME) : dim_attr(C_VAL);

        putf(y, x, la, "%-9.9s", c->label);

        int fill = ctrl_fill(c, barw);

        if (fill < 0) {
            /* No bar means the value is a word, not a quantity. Words read
             * left-aligned, and they get the value and unit columns both --
             * otherwise "crackle" arrives as "crack". */
            char joined[64];
            snprintf(joined, sizeof joined, "%s%s%s", val, unit[0] ? " " : "", unit);
            putf(y, x + 10, va, "%-12.12s", joined);
        } else {
            putf(y, x + 10, va, "%5.5s",  val);
            putf(y, x + 16, ua, "%-6.6s", unit);
        }

        if (fill >= 0 && barw > 0) {
            char bar[24];
            for (int j = 0; j < barw; j++) bar[j] = (j < fill) ? '#' : '-';
            bar[barw] = '\0';
            int ba = inert ? dim_attr(C_FRAME) : attr_for(class_pair(cls), 1);
            /* Draw the empty part dim and the filled part hot, in two passes,
             * so the track is visible without competing with the level. */
            put(y, x + 23, dim_attr(C_FRAME), bar);
            char fb[24];
            int nf = fill > barw ? barw : (fill < 0 ? 0 : fill);
            memset(fb, '#', (size_t)nf);
            fb[nf] = '\0';
            if (nf) put(y, x + 23, ba, fb);
        }
    }
}

static void draw_scope(int y0, int h)
{
    if (h < 3) return;

    int cols = COLS;
    if (cols > 500) cols = 500;

    int zoom = atomic_load(&bb.gctl[GCTL_ZOOM]);
    int maxz = (int)(BB_SCOPE_LEN / (unsigned)(cols > 0 ? cols : 1));
    if (maxz < 1) maxz = 1;
    if (zoom > maxz) zoom = maxz;
    if (zoom < 1) zoom = 1;

    unsigned w = atomic_load_explicit(&bb.scope_w, memory_order_acquire);

    static char rowbuf[512];
    static signed char top[512], bot[512];

    for (int cx = 0; cx < cols; cx++) {
        unsigned end = w - (unsigned)((cols - cx) * zoom);
        int mn = 32767, mx = -32768;
        for (int j = 0; j < zoom; j++) {
            int v = bb.scope[(end + (unsigned)j) & BB_SCOPE_MASK];
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        int mid = h / 2, half = h / 2;
        if (half < 1) half = 1;
        int rt = mid - (mx * half) / 32768;
        int rb = mid - (mn * half) / 32768;
        if (rt < 0) rt = 0;
        if (rb > h - 1) rb = h - 1;
        if (rt > h - 1) rt = h - 1;
        if (rb < 0) rb = 0;
        if (rt > rb) { int s = rt; rt = rb; rb = s; }
        top[cx] = (signed char)rt;
        bot[cx] = (signed char)rb;
    }

    int mid = h / 2;
    int clip = atomic_load(&bb.clipping);
    for (int ry = 0; ry < h; ry++) {
        for (int cx = 0; cx < cols; cx++)
            rowbuf[cx] = (ry >= top[cx] && ry <= bot[cx]) ? '|'
                       : (ry == mid ? '-' : ' ');
        rowbuf[cols] = '\0';
        put(y0 + ry, 0, ry == mid ? dim_attr(C_FRAME)
                                  : attr_for(clip ? C_ERR : C_WAVE, 1), rowbuf);
    }
}

enum { PF_BARS = 0, PF_MIX, PF_FEEDBACK, PF_RATE, PF_DIRECTION,
       PF_SLICE, PF_OVERDUB, PF_COUNT };
static const char *const PERF_NAME[PF_COUNT] = {
    "BARS", "MIX", "FEEDBACK", "RATE", "DIRECTION", "SLICE", "OVERDUB"
};

static void perf_value(int idx, char *buf, size_t n)
{
    switch (idx) {
    case PF_BARS: snprintf(buf, n, "%d", atomic_load(&bb.loop_bars)); break;
    case PF_MIX: snprintf(buf, n, "%d%%", atomic_load(&bb.loop_mix) * 100 / 256); break;
    case PF_FEEDBACK: snprintf(buf, n, "%d%%", atomic_load(&bb.loop_feedback) * 100 / 256); break;
    case PF_RATE: {
        int r = atomic_load(&bb.loop_rate);
        snprintf(buf, n, "%s", r == LOOP_RATE_HALF ? "1/2x" : r == LOOP_RATE_DOUBLE ? "2x" : "1x");
        break;
    }
    case PF_DIRECTION: snprintf(buf, n, "%s", atomic_load(&bb.loop_reverse) ? "reverse" : "forward"); break;
    case PF_SLICE: snprintf(buf, n, "1/%d", atomic_load(&bb.loop_slice)); break;
    case PF_OVERDUB: snprintf(buf, n, "%s", atomic_load(&bb.loop_overdub) ? "ON" : "off"); break;
    default: buf[0] = '\0'; break;
    }
}

static void draw_performance(int y)
{
    int head = atomic_load(&bb.seq_pos);
    put(y, 0, attr_for(C_FRAME, 1), "TRACK  ENGINE      PATTERN                            LEVEL  FX");

    for (int L = 0; L < BB_NLAYER; L++) {
        Layer *l = &bb.layer[L];
        int row = y + 1 + L;
        int on = atomic_load(&l->on);
        int foc = L == focus();
        const char *src = bb_expr[L][0] == '\0' ? "empty"
                        : bb_custom[L] ? "custom" : rack_src_name(bb_rack[L].src);
        putf(row, 0, foc ? rev_attr(C_SEL) : (on ? attr_for(C_HOT, 1) : dim_attr(C_FRAME)),
             "%d%c", L + 1, on ? '*' : 'o');
        putf(row, 5, on ? attr_for(C_VAL, 1) : dim_attr(C_VAL), "%-10.10s", src);

        int steps = bb_clampi(atomic_load(&l->ctl[LCTL_STEPS]), 1, BB_STEPS);
        for (int st = 0; st < BB_STEPS; st++) {
            int x = 17 + st * 2;
            if (x + 1 >= COLS) break;
            int g = atomic_load(&l->seq_gate[st]);
            int rat = bb_clampi(atomic_load(&l->seq_ratchet[st]), 1, 4);
            int prob = bb_clampi(atomic_load(&l->seq_prob[st]), 0, 100);
            char cell[3] = {
                g == GATE_ACCENT ? 'X' : g == GATE_ON ? 'x' : '.',
                rat > 1 ? (char)('0' + rat) : prob < 100 ? '?' : ' ', '\0'
            };
            int live = st < steps;
            int play = live && atomic_load(&l->seq_on) && st == head % steps;
            int a = !live ? dim_attr(C_FRAME)
                  : play ? rev_attr(C_HOT)
                  : g ? attr_for(C_HOT, 1) : dim_attr(C_VAL);
            put(row, x, a, cell);
        }
        putf(row, 51, on ? attr_for(C_VAL, 0) : dim_attr(C_VAL), "%3d%%",
             atomic_load(&l->ctl[LCTL_LEVEL]) * 100 / 256);
        char fx[32];
        int sy = atomic_load(&l->ctl[LCTL_SPC_SYNC]);
        snprintf(fx, sizeof fx, "%s%s%s",
                 atomic_load(&l->ctl[LCTL_SPC_MIX]) ? "space " : "",
                 sy ? space_sync_name(sy) : "",
                 atomic_load(&l->ctl[LCTL_SPC_FREEZE]) ? " FREEZE" : "");
        put(row, 58, atomic_load(&l->ctl[LCTL_SPC_FREEZE])
                       ? attr_for(C_SPACE, 1) : dim_attr(C_SPACE), fx);
    }

    int ly = y + 10;
    int state = atomic_load(&bb.loop_status);
    unsigned len = atomic_load(&bb.loop_frames);
    unsigned pos = atomic_load(&bb.loop_pos);
    int pct = len ? (int)((uint64_t)pos * 100u / len) : 0;
    putf(ly, 0, attr_for(C_FRAME, 1),
         "MASTER PHRASE  %-7s  %u frames  %3d%%", loop_status_name(state), len, pct);

    int colw = COLS / 4;
    if (colw < 18) colw = 18;
    for (int i = 0; i < PF_COUNT; i++) {
        int row = ly + 2 + i / 4;
        int x = (i % 4) * colw;
        char value[24];
        perf_value(i, value, sizeof value);
        int a = i == perf_cursor ? rev_attr(C_SEL) : attr_for(C_VAL, 1);
        putf(row, x, a, "%-9s %-8s", PERF_NAME[i], value);
    }
    put(ly + 5, 0, dim_attr(C_FRAME),
        "r arm at next bar   SPACE play/stop   o overdub   f layer freeze   x clear");
}

/* The bottom two rows: what the cursor is on, then how to drive it. The first
 * of those is the real help system -- an explanation of the thing you are
 * touching, on screen, before you think to ask for it. */
static void draw_footer(int y, int rows)
{
    clear_row(y);
    if (rows > 1) clear_row(y + 1);

    if (mode_ui == M_INSERT) {
        put(y, 0, attr_for(C_SEL, 1),
            "INSERT   Esc/Enter back   ^A/^E home/end   ^K kill   ^U clear");
        if (rows > 1)
            put(y + 1, 0, dim_attr(C_FRAME),
                "  recompiles as you type -- if it does not parse, the last good "
                "program keeps playing");
        return;
    }
    if (mode_ui == M_SEQ) {
        char name[24];
        lock_name(focus(), seq_lock_target, name, sizeof name);
        putf(y, 0, attr_for(C_SEL, 1),
             "SEQ %-7s  <-/-> step  TAB lane  up/down adjust  SPACE toggle  lock:%s",
             seq_lane == SL_GATE ? "GATE" : seq_lane == SL_PITCH ? "PITCH" :
             seq_lane == SL_RATCHET ? "RATCHET" : seq_lane == SL_PROB ? "PROB" : "LOCK",
             name);
        if (rows > 1) put(y + 1, 0, dim_attr(C_FRAME),
            "[] lock target  l capture  m step/motion  e euclid  r random  x clear all  Esc back");
        return;
    }
    if (mode_ui == M_PERF) {
        put(y, 0, attr_for(C_SEL, 1),
            "PERFORM  arrows looper controls  r arm  SPACE play/stop  o overdub  f freeze  Z/Esc back");
        if (rows > 1) put(y + 1, 0, dim_attr(C_FRAME),
            "         1-8 focus  shift+number toggle layer  x clear phrase  z write WAV  X quit");
        return;
    }

    if (cursor >= 0 && cursor < nctrl) {
        const Ctrl *c = &ctrls[cursor];
        putf(y, 0, attr_for(C_SEL, 1), "%-8s", c->label);
        put(y, 9, attr_for(C_VAL, 0), c->hint ? c->hint : "");
    }
    if (rows > 1)
        put(y + 1, 0, dim_attr(C_FRAME),
            "arrows move/adjust  <> coarse  TAB group   p roll  P mutate  C undo   "
            "M motion  Z perform  ? help");
}

/* ---- help --------------------------------------------------------------- */

static void help_keys(int x, int y, int h);
static void help_panel(int x, int y, int h);
static void help_sources(int x, int y, int h);
static void help_groove(int x, int y, int h);
static void help_language(int x, int y, int h);

static const struct { const char *title; void (*fn)(int, int, int); } HELP_PAGE[] = {
    { "KEYS",                 help_keys     },
    { "THE PANEL",            help_panel    },
    { "SEQUENCER + LOOPER",   help_groove   },
    { "SOURCES",              help_sources  },
    { "THE EXPRESSION LANGUAGE", help_language },
};
#define N_HELP ((int)(sizeof HELP_PAGE / sizeof HELP_PAGE[0]))

static int hy, hymax, hx;

static void hput(const char *s)
{
    if (hy < hymax) put(hy++, hx, attr_for(C_VAL, 0), s);
}

static void hhead(const char *s)
{
    if (hy < hymax) put(hy++, hx, attr_for(C_SEL, 1), s);
}

static void help_keys(int x, int y, int h)
{
    hx = x; hy = y; hymax = y + h;
    hhead("MOVING AND ADJUSTING");
    hput("  up/down       move the cursor through the panel");
    hput("  left/right    adjust it by one detent      <  >   by eight");
    hput("  , .           same as left/right, for terminals without them");
    hput("  TAB  S-TAB    jump to the next / previous column");
    hput("  qa ws ed rf   the eight VOICE rows, directly -- no cursor needed");
    hput("  tg yh uj ol   (uppercase for a coarse step)");
    hput("");
    hhead("STEERING A SOUND YOU ALREADY HAVE");
    hput("  [  ]          darker / brighter        every cutoff in the voice");
    hput("  {  }          cleaner / dirtier        masks, drive, crush");
    hput("  ;  '          lower / higher           every pitch in the voice");
    hput("  :  \"          slower / faster          every rate in the voice");
    hput("");
    hhead("FINDING ONE FROM NOTHING");
    hput("  p             roll a new voice (always audible -- it is auditioned)");
    hput("  P             mutate: same source and stages, new numbers");
    hput("  C             undo, one step, per layer");
    hput("  x             re-roll this source's slots, keeping the source");
    hput("  n / b         step through the built-in examples");
    hput("");
    hhead("LAYERS AND TRANSPORT");
    hput("  1-8           focus a layer      !@#$%^&*  toggle it on/off");
    hput("  L  D          solo / clear the focused layer");
    hput("  i or Enter    edit the expression by hand (SOURCE takes you back)");
    hput("  v  V          edit the step pattern / turn the sequencer on");
    hput("  M             record the selected knob as a repeating motion lane");
    hput("  Z             performance view and master phrase looper");
    hput("  m             cycle output mode      B  bypass all post chains");
    hput("  SPACE mute    ` PANIC    z record    ^S save session    X quit");
    hput("  - = _ +       master gain            0 reset t    K restart loop");
}

static void help_groove(int x, int y, int h)
{
    hx = x; hy = y; hymax = y + h;
    hhead("TRIGGERED NOISE ENGINES");
    hput("  thump, burst, metal, dust, rumble and feedback are actual struck");
    hput("  voices. The sequencer sends tr (one sample), age (time since hit)");
    hput("  and vel (normal/accent) into the expression, where bp() supplies a");
    hput("  resonating body. They are expressions: i still opens the truth.");
    hput("");
    hhead("STEP EDITOR -- v");
    hput("  left/right step     TAB/S-TAB lane     up/down change the cell");
    hput("  lanes: gate, pitch, ratchet x1..x4, probability, parameter lock");
    hput("  [ ] choose lock target    l captures its live value at this step");
    hput("  m changes that lock lane between hard steps and smooth motion");
    hput("  c clears this lane; x clears the pattern; e fills Euclidean rhythm");
    hput("");
    hhead("MOTION -- M IN THE NORMAL PANEL");
    hput("  Select any VOICE knob or POST sound control and press M. For as");
    hput("  long as recording is lit, the current value is written against the");
    hput("  16-step transport. Press M again and it repeats with interpolation.");
    hput("");
    hhead("SPACE AND MASTER PHRASE -- Z");
    hput("  SP-SYNC locks each layer delay from 1/32 through two bars. FREEZE");
    hput("  closes that delay at unity without admitting new sound.");
    hput("  In PERFORM: r arms bar-aligned capture; SPACE plays/stops; o overdubs;");
    hput("  arrows set bars, dry/loop mix, feedback, half/normal/double speed,");
    hput("  reverse and 1/2..1/16 stutter slices. x clears the captured phrase.");
}

static void help_panel(int x, int y, int h)
{
    hx = x; hy = y; hymax = y + h;
    hhead("WHY THE NUMBERS LOOK LIKE THAT");
    hput("  A knob is 0..255, but almost nothing in this instrument is linear");
    hput("  in that number. `t>>p` masks p to 0..31, so 256 positions address");
    hput("  32 sounds in eight identical repeats. `t&p` is only musical when p");
    hput("  is 2^n-1 -- eight useful values. `lp(x,p)` is logarithmic.");
    hput("");
    hput("  So the panel does not step through the integers. Each control has a");
    hput("  LADDER of values that actually do something, and left/right moves");
    hput("  one rung. Every press changes the sound; the bar shows where you");
    hput("  are on the ladder, not v/256; and the unit column tells you what");
    hput("  the value means in Hz, ms, bits or percent.");
    hput("");
    hhead("THE THREE COLUMNS");
    hput("  VOICE   the expression. SOURCE picks the shape, STAGES wraps");
    hput("          optional BODY (lowpass) and SPACE (feedback delay) around");
    hput("          it, and the rows below are its named slots. Whatever you");
    hput("          turn here appears in the expression on line 3.");
    hput("  POST    a fixed chain applied after the expression, per layer:");
    hput("          drive -> tone -> crush -> space -> level. SPACE can follow");
    hput("          milliseconds or the transport, and FREEZE closes its loop.");
    hput("  SHAPE   the sequencer and transport. DECAY articulates each trigger;");
    hput("          v opens ratchets, probability and parameter-lock lanes.");
    hput("");
    hhead("BAR COLOURS -- what a control does, not where it lives");
    hput("  magenta  pitch     how high it sounds");
    hput("  cyan     rate      how fast it moves, including gate DECAY");
    hput("  green    timbre    which partials survive");
    hput("  yellow   cutoff    how dark it is");
    hput("  blue     space     delay times -- effect, not modulation");
    hput("  white    level     how much of it there is");
    hput("");
    hput("  The directional keys follow those meanings: ] moves every yellow");
    hput("  control up, ; moves every magenta one down, and so on. Blue is");
    hput("  left alone, so speeding a patch up does not shorten its reverb.");
}

static void help_sources(int x, int y, int h)
{
    hx = x; hy = y; hymax = y + h;
    hhead("SOURCES -- left/right on the SOURCE row cycles these");
    int cur = bb_custom[focus()] ? -1 : bb_rack[focus()].src;

    /* One source per row is deliberate: all seventeen, including the struck
     * engines appended after the classics, fit in an ordinary 80x24 terminal.
     * The selected source's prose remains at the bottom when there is room. */
    for (int i = 0; i < rack_nsrc() && hy < hymax; i++) {
        char b[240], shape[160];
        rack_src_shape_text(i, shape, sizeof shape);

        snprintf(b, sizeof b, "  %-9s %s", rack_src_name(i), shape);
        put(hy++, hx, i == cur ? attr_for(C_SEL, 1) : attr_for(C_VAL, 0), b);
    }
    if (cur >= 0 && hy < hymax) {
        char b[240];
        snprintf(b, sizeof b, "  %s: %s", rack_src_name(cur), rack_src_desc(cur));
        put(hy++, hx, dim_attr(C_FRAME), b);
    }
}

static void help_language(int x, int y, int h)
{
    hx = x; hy = y; hymax = y + h;
    hhead("EDIT THE TEXT DIRECTLY WITH i -- THE PANEL FOLLOWS AS BEST IT CAN");
    hput("  Typing detaches the layer from its rack: the text is then the only");
    hput("  truth, and the VOICE column falls back to raw p0..p7, with the role");
    hput("  of each worked out from the compiled bytecode -- so the ladders and");
    hput("  the units keep working on expressions nobody planned for.");
    hput("");
    hput("  To go back, move to the SOURCE row and press left or right. That");
    hput("  discards the text and rebuilds it from a source; C undoes it.");
    hput("");
    int n;
    const char *const *hl = expr_help_lines(&n);
    for (int i = 0; i < n && hy < hymax; i++) hput(hl[i]);
}

/* Help takes the whole screen. A centred box looked tidier in the abstract
 * and was unreadable in practice: the panel showing down both margins put
 * columns of unrelated numbers either side of every line of prose. */
static void draw_help(void)
{
    for (int y = 0; y < LINES; y++) clear_row(y);

    int pg = help_page % N_HELP;

    char rule[512];
    int  w = COLS < (int)sizeof rule - 1 ? COLS : (int)sizeof rule - 1;
    memset(rule, '-', (size_t)w);
    rule[w] = '\0';
    put(1, 0, dim_attr(C_FRAME), rule);

    putf(0, 2, attr_for(C_HOT, 1), "%s", HELP_PAGE[pg].title);
    for (int i = 0; i < N_HELP; i++) {
        int x = COLS - 34 + i * 2;
        if (x > 0) put(0, x, i == pg ? rev_attr(C_SEL) : dim_attr(C_FRAME),
                       i == pg ? "*" : ".");
    }
    if (COLS > 32)
        put(0, COLS - 22, dim_attr(C_FRAME), "<- ->    any key exits");

    HELP_PAGE[pg].fn(2, 3, LINES - 4);
}

/* ---- frame -------------------------------------------------------------- */

static void draw(void)
{
    erase();
    knob_set_rate(atomic_load(&bb.rate));
    ctrls_rebuild();

    int foot = (LINES >= 24) ? 2 : 1;

    draw_title();
    if (mode_ui == M_PERF) {
        draw_status(2);
        draw_readout(3);
        draw_performance(5);
        draw_footer(LINES - foot, foot);
        if (help_page >= 0) draw_help();
        curs_set(0);
        refresh();
        return;
    }

    draw_expr(2);
    draw_status(3);
    draw_readout(4);
    draw_layers(5);
    draw_seq(6);

    /* The panel gets as many rows as its longest column wants, and gives back
     * whatever it does not need to the scope. */
    int want = 0;
    for (int g = 0; g < CG_COUNT; g++) if (grp_count[g] > want) want = grp_count[g];

    int panel_y = 6 + (mode_ui == M_SEQ ? 5 : 2);
    int avail   = LINES - foot - panel_y - 1;
    int rows    = want < avail ? want : avail;
    if (rows < 1) rows = 1;

    draw_panel(panel_y, rows);

    int scope_y = panel_y + rows + 2;
    int scope_h = LINES - foot - scope_y;
    if (scope_h >= 3) draw_scope(scope_y, scope_h);

    draw_footer(LINES - foot, foot);

    if (help_page >= 0) draw_help();

    if (mode_ui == M_INSERT && help_page < 0) {
        move(2, 5 + (ed_cur - ed_scroll));
        curs_set(1);
    } else {
        curs_set(0);
    }
    refresh();
}

/* ======================================================================== */
/*  input                                                                   */
/* ======================================================================== */

/* The VOICE column's knobs, reachable without the cursor. This is the
 * performance interface: both hands on the home row, no navigation.
 *
 * They address the SLOTS only -- SOURCE and STAGES are skipped, because those
 * rewrite the expression and are not something you want under a key you are
 * hammering mid-set. So q/a is always the first thing that shapes the sound,
 * whether the layer is racked or custom. */
static const struct { int dn, up, DN, UP; } KNOB[8] = {
    { 'q','a','Q','A' }, { 'w','s','W','S' },
    { 'e','d','E','D' }, { 'r','f','R','F' },
    { 't','g','T','G' }, { 'y','h','Y','H' },
    { 'u','j','U','J' }, { 'o','l','O','L' },
};

/* shift+1..8 on a US layout */
static const char LAYER_TOGGLE[BB_NLAYER] = { '!','@','#','$','%','^','&','*' };

static void cursor_move(int d)
{
    if (nctrl <= 0) return;
    cursor = (cursor + nctrl + d) % nctrl;
}

static void cursor_group(int d)
{
    int g = ctrls[cursor].group;
    g = (g + CG_COUNT + d) % CG_COUNT;
    cursor = grp_first[g];
}

static void toggle_record(void)
{
    if (sink_rec_active()) {
        unsigned fr = sink_rec_frames();
        int rate = atomic_load(&bb.rate);
        char path[256];
        snprintf(path, sizeof path, "%s", sink_rec_path());
        sink_rec_stop();
        ui_status("wrote %s  %u frames  %.1fs", path, fr,
                  rate > 0 ? (double)fr / rate : 0.0);
    } else {
        char path[256], err[200];
        if (sink_rec_start(atomic_load(&bb.rate), path, sizeof path,
                           err, sizeof err) < 0)
            ui_status("%s", err);
        else
            ui_status("recording -> %s", path);
    }
}

/* Re-roll the slot values of the current source without changing the source.
 * "Same instrument, different setting" -- the smallest useful random move. */
static void reroll_slots(void)
{
    int f = focus();
    if (bb_custom[f]) {
        for (int i = 0; i < BB_NPARAM; i++)
            atomic_store(&FL()->param[i], (int)(nextrand() & 255u));
        ui_status("knobs randomised");
        return;
    }
    snapshot();
    for (int i = 0; i < focus_build.nslot; i++) {
        int lo, hi;
        knob_gen_range(focus_build.slot[i].kind, &lo, &hi);
        int d = lo + (int)(nextrand() % (unsigned)(hi - lo + 1));
        atomic_store(&FL()->param[focus_build.slot[i].pidx],
                     knob_value(focus_build.slot[i].kind, d));
    }
    ui_status("%s re-rolled  (C undo)", rack_src_name(bb_rack[f].src));
}

static void key_normal(int c)
{
    /* Direct access to the VOICE column's knobs. */
    int slot[8], nslot = 0;
    for (int i = grp_first[CG_VOICE];
         i < grp_first[CG_VOICE] + grp_count[CG_VOICE] && nslot < 8; i++)
        if (ctrls[i].type == CT_PARAM) slot[nslot++] = i;

    for (int i = 0; i < nslot; i++) {
        if (c == KNOB[i].dn) { ctrl_adjust(slot[i], -1, 0); return; }
        if (c == KNOB[i].up) { ctrl_adjust(slot[i], +1, 0); return; }
        if (c == KNOB[i].DN) { ctrl_adjust(slot[i], -1, 1); return; }
        if (c == KNOB[i].UP) { ctrl_adjust(slot[i], +1, 1); return; }
    }

    if (c >= '1' && c <= '8') { focus_layer(c - '1'); return; }

    for (int i = 0; i < BB_NLAYER; i++) {
        if (c == LAYER_TOGGLE[i]) {
            int on = !atomic_load(&bb.layer[i].on);
            atomic_store(&bb.layer[i].on, on);
            ui_status("layer %d %s", i + 1, on ? "ON" : "off");
            return;
        }
    }

    switch (c) {
    case KEY_UP:    cursor_move(-1); return;
    case KEY_DOWN:  cursor_move(+1); return;
    case KEY_LEFT:  ctrl_adjust(cursor, -1, 0); return;
    case KEY_RIGHT: ctrl_adjust(cursor, +1, 0); return;
    case ',':       ctrl_adjust(cursor, -1, 0); return;
    case '.':       ctrl_adjust(cursor, +1, 0); return;
    case '<':       ctrl_adjust(cursor, -1, 1); return;
    case '>':       ctrl_adjust(cursor, +1, 1); return;
    case '\t':      cursor_group(+1); return;
    case KEY_BTAB:  cursor_group(-1); return;

    /* directional moves */
    case '[':  do_nudge(NX_TONE,  -1); return;
    case ']':  do_nudge(NX_TONE,  +1); return;
    case '{':  do_nudge(NX_GRIT,  -1); return;
    case '}':  do_nudge(NX_GRIT,  +1); return;
    case ';':  do_nudge(NX_PITCH, -1); return;
    case '\'': do_nudge(NX_PITCH, +1); return;
    case ':':  do_nudge(NX_RATE,  -1); return;
    case '"':  do_nudge(NX_RATE,  +1); return;

    case 'i': case '\n': case '\r': case KEY_ENTER:
        mode_ui = M_INSERT;
        return;

    case 'v':
        mode_ui = M_SEQ;
        ui_status("SEQ: space toggles, e fills, Esc exits");
        return;

    case 'V': {
        int on = !atomic_load(&FL()->seq_on);
        atomic_store(&FL()->seq_on, on);
        if (on && ctl_get(LCTL_DECAY) == 0) ctl_set(LCTL_DECAY, 90);
        ui_status("layer %d sequencer %s", focus() + 1, on ? "ON" : "off");
        return;
    }

    case 'M': motion_toggle(); return;

    case 'Z':
        mode_ui = M_PERF;
        ui_status("PERFORM: r captures at the next bar, SPACE plays, arrows shape the loop");
        return;

    case 'p': do_generate(0); return;
    case 'P': do_generate(1); return;
    case 'C': do_undo();      return;
    case 'x': reroll_slots(); return;

    case 'L': {
        int f = focus(), others = 0;
        for (int i = 0; i < BB_NLAYER; i++)
            if (i != f && atomic_load(&bb.layer[i].on)) others = 1;
        for (int i = 0; i < BB_NLAYER; i++)
            atomic_store(&bb.layer[i].on, others ? (i == f) : (bb_expr[i][0] != '\0'));
        atomic_store(&bb.layer[f].on, 1);
        ui_status(others ? "solo layer %d" : "all layers on", f + 1);
        return;
    }

    case 'D':
        snapshot();
        ed_set("");
        ed_commit();
        atomic_store(&FL()->on, 0);
        seq_clear();
        ui_status("layer %d cleared  (C undo)", focus() + 1);
        return;

    case ' ':
        atomic_store(&bb.mute, !atomic_load(&bb.mute));
        return;

    case '`':
        atomic_store(&bb.panic, !atomic_load(&bb.panic));
        ui_status(atomic_load(&bb.panic) ? "PANIC -- silenced, stream intact"
                                         : "panic cleared");
        return;

    case 'm': {
        int m = (atomic_load(&FL()->mode) + 1) % BB_NMODE;
        atomic_store(&FL()->mode, m);
        if (!bb_custom[focus()]) bb_rack[focus()].mode = (unsigned char)m;
        ui_status("layer %d mode %s", focus() + 1, mode_name(m));
        return;
    }

    case '0': atomic_store(&bb.reset_t, 1);    ui_status("t reset"); return;
    case 'K': atomic_store(&bb.reset_loop, 1); ui_status("loop restarted"); return;

    case 'c': snapshot(); ed_set("t"); ed_commit(); return;

    case 'n': ui_load_example(cur_example + 1); return;
    case 'b': ui_load_example(cur_example - 1); return;

    case 'B':
        atomic_store(&bb.bypass, !atomic_load(&bb.bypass));
        ui_status(atomic_load(&bb.bypass) ? "post chains BYPASSED"
                                          : "post chains active");
        return;

    case '-': atomic_store(&bb.gain, bb_clampi(atomic_load(&bb.gain) -  8, 0, 256)); return;
    case '=': atomic_store(&bb.gain, bb_clampi(atomic_load(&bb.gain) +  8, 0, 256)); return;
    case '_': atomic_store(&bb.gain, bb_clampi(atomic_load(&bb.gain) - 32, 0, 256)); return;
    case '+': atomic_store(&bb.gain, bb_clampi(atomic_load(&bb.gain) + 32, 0, 256)); return;

    case 'z': toggle_record(); return;

    case 19:
        if (bb_config_save() == 0) ui_status("saved %s", bb_config_path());
        else                       ui_status("could not save session");
        return;

    case '?': help_page = 0; return;
    case 'X': case 3: quit = 1; return;
    default: return;
    }
}

static void key_seq(int c)
{
    Layer *l = FL();
    int n = ctl_get(LCTL_STEPS);

#define CUR_ATOM(field) (&l->field[seq_cur])

    switch (c) {
    case 27: case 'v': mode_ui = M_NORMAL; return;

    case KEY_LEFT:  seq_cur = (seq_cur + BB_STEPS - 1) % BB_STEPS; return;
    case KEY_RIGHT: seq_cur = (seq_cur + 1) % BB_STEPS; return;
    case '\t':      seq_lane = (seq_lane + 1) % SL_COUNT; return;
    case KEY_BTAB:  seq_lane = (seq_lane + SL_COUNT - 1) % SL_COUNT; return;
    case '[':       seq_lock_target = (seq_lock_target + BB_LOCK_COUNT - 1) % BB_LOCK_COUNT; return;
    case ']':       seq_lock_target = (seq_lock_target + 1) % BB_LOCK_COUNT; return;

    case ' ': {
        if (seq_lane == SL_GATE) {
            int g = (atomic_load(CUR_ATOM(seq_gate)) + 1) % 3;
            atomic_store(CUR_ATOM(seq_gate), g);
        } else if (seq_lane == SL_PITCH) {
            atomic_store(CUR_ATOM(seq_pitch), 0);
        } else if (seq_lane == SL_RATCHET) {
            atomic_store(CUR_ATOM(seq_ratchet),
                         atomic_load(CUR_ATOM(seq_ratchet)) % 4 + 1);
        } else if (seq_lane == SL_PROB) {
            atomic_store(CUR_ATOM(seq_prob),
                         atomic_load(CUR_ATOM(seq_prob)) == 100 ? 50 : 100);
        } else {
            atomic_int *a = &l->seq_lock[seq_lock_target][seq_cur];
            atomic_store(a, atomic_load(a) < 0 ? lock_current(focus(), seq_lock_target) : -1);
        }
        atomic_store(&l->seq_on, 1);
        return;
    }

    case KEY_UP: case KEY_DOWN: case '<': case '>': {
        int dir = (c == KEY_UP || c == '>') ? 1 : -1;
        int coarse = c == '<' || c == '>';
        if (seq_lane == SL_GATE) {
            int g = atomic_load(CUR_ATOM(seq_gate));
            atomic_store(CUR_ATOM(seq_gate), (g + 3 + dir) % 3);
        } else if (seq_lane == SL_PITCH) {
            int d = coarse ? 12 : 1;
            atomic_store(CUR_ATOM(seq_pitch),
                         bb_clampi(atomic_load(CUR_ATOM(seq_pitch)) + dir * d, -12, 12));
        } else if (seq_lane == SL_RATCHET) {
            atomic_store(CUR_ATOM(seq_ratchet),
                         bb_clampi(atomic_load(CUR_ATOM(seq_ratchet)) + dir, 1, 4));
        } else if (seq_lane == SL_PROB) {
            int d = coarse ? 25 : 5;
            atomic_store(CUR_ATOM(seq_prob),
                         bb_clampi(atomic_load(CUR_ATOM(seq_prob)) + dir * d, 0, 100));
        } else {
            atomic_int *a = &l->seq_lock[seq_lock_target][seq_cur];
            atomic_store(a, lock_step_value(focus(), seq_lock_target,
                                            atomic_load(a), dir, coarse));
        }
        atomic_store(&l->seq_on, 1);
        return;
    }

    case '0':
        if (seq_lane == SL_GATE) atomic_store(CUR_ATOM(seq_gate), GATE_OFF);
        else if (seq_lane == SL_PITCH) atomic_store(CUR_ATOM(seq_pitch), 0);
        else if (seq_lane == SL_RATCHET) atomic_store(CUR_ATOM(seq_ratchet), 1);
        else if (seq_lane == SL_PROB) atomic_store(CUR_ATOM(seq_prob), 100);
        else atomic_store(&l->seq_lock[seq_lock_target][seq_cur], -1);
        return;

    case '1': case '2': case '3': case '4':
        atomic_store(CUR_ATOM(seq_ratchet), c - '0');
        seq_lane = SL_RATCHET;
        atomic_store(&l->seq_on, 1);
        return;

    case 'l':
        atomic_store(&l->seq_lock[seq_lock_target][seq_cur],
                     lock_current(focus(), seq_lock_target));
        seq_lane = SL_LOCK;
        atomic_store(&l->seq_on, 1);
        return;

    case 'm': {
        unsigned bit = 1u << seq_lock_target;
        unsigned mask = atomic_load(&l->motion_mask) ^ bit;
        atomic_store(&l->motion_mask, mask);
        if ((mask & bit) && atomic_load(&l->seq_lock[seq_lock_target][seq_cur]) < 0)
            atomic_store(&l->seq_lock[seq_lock_target][seq_cur],
                         lock_current(focus(), seq_lock_target));
        ui_status("%s interpolation for this lock lane", (mask & bit) ? "motion" : "step");
        return;
    }

    case 'c':
        for (int st = 0; st < BB_STEPS; st++) {
            if (seq_lane == SL_GATE) atomic_store(&l->seq_gate[st], GATE_OFF);
            else if (seq_lane == SL_PITCH) atomic_store(&l->seq_pitch[st], 0);
            else if (seq_lane == SL_RATCHET) atomic_store(&l->seq_ratchet[st], 1);
            else if (seq_lane == SL_PROB) atomic_store(&l->seq_prob[st], 100);
            else atomic_store(&l->seq_lock[seq_lock_target][st], -1);
        }
        if (seq_lane == SL_LOCK) atomic_fetch_and(&l->motion_mask, ~(1u << seq_lock_target));
        ui_status("lane cleared");
        return;

    case 'x': seq_clear(); ui_status("pattern cleared"); return;
    case 'r': seq_random(); atomic_store(&l->seq_on, 1); ui_status("random pattern"); return;

    case 'e':
        euclid_k = euclid_k % (n > 0 ? n : 1) + 1;
        seq_euclid(euclid_k);
        atomic_store(&l->seq_on, 1);
        ui_status("euclid %d/%d", euclid_k, n);
        return;

    case 'X': case 3: quit = 1; return;
    default: return;
    }
#undef CUR_ATOM
}

static void perf_adjust(int idx, int dir, int coarse)
{
    switch (idx) {
    case PF_BARS:
        atomic_store(&bb.loop_bars,
                     bb_clampi(atomic_load(&bb.loop_bars) + dir, 1, 4));
        break;
    case PF_MIX:
        atomic_store(&bb.loop_mix,
                     bb_clampi(atomic_load(&bb.loop_mix) + dir * (coarse ? 32 : 8), 0, 256));
        break;
    case PF_FEEDBACK:
        atomic_store(&bb.loop_feedback,
                     bb_clampi(atomic_load(&bb.loop_feedback) + dir * (coarse ? 32 : 8), 0, 256));
        break;
    case PF_RATE:
        atomic_store(&bb.loop_rate,
                     bb_clampi(atomic_load(&bb.loop_rate) + dir,
                               LOOP_RATE_HALF, LOOP_RATE_DOUBLE));
        break;
    case PF_DIRECTION:
        atomic_store(&bb.loop_reverse, !atomic_load(&bb.loop_reverse));
        break;
    case PF_SLICE: {
        static const int V[] = { 1, 2, 4, 8, 16 };
        int cur = atomic_load(&bb.loop_slice), at = 0;
        for (int i = 0; i < 5; i++) if (V[i] == cur) at = i;
        at = bb_clampi(at + dir * (coarse ? 2 : 1), 0, 4);
        atomic_store(&bb.loop_slice, V[at]);
        break;
    }
    case PF_OVERDUB:
        atomic_store(&bb.loop_overdub, !atomic_load(&bb.loop_overdub));
        break;
    }
}

static void key_perf(int c)
{
    if (c >= '1' && c <= '8') { focus_layer(c - '1'); return; }
    for (int i = 0; i < BB_NLAYER; i++) if (c == LAYER_TOGGLE[i]) {
        atomic_store(&bb.layer[i].on, !atomic_load(&bb.layer[i].on));
        return;
    }

    switch (c) {
    case 27: case 'Z': mode_ui = M_NORMAL; return;
    case KEY_UP:   perf_cursor = (perf_cursor + PF_COUNT - 1) % PF_COUNT; return;
    case KEY_DOWN: perf_cursor = (perf_cursor + 1) % PF_COUNT; return;
    case KEY_LEFT:  perf_adjust(perf_cursor, -1, 0); return;
    case KEY_RIGHT: perf_adjust(perf_cursor, +1, 0); return;
    case '<': perf_adjust(perf_cursor, -1, 1); return;
    case '>': perf_adjust(perf_cursor, +1, 1); return;
    case 'r':
        atomic_store(&bb.loop_cmd, LOOP_CMD_ARM);
        ui_status("phrase capture armed for the next bar");
        return;
    case ' ':
        atomic_store(&bb.loop_cmd, LOOP_CMD_PLAY);
        return;
    case 'o':
        atomic_store(&bb.loop_overdub, !atomic_load(&bb.loop_overdub));
        ui_status("overdub %s", atomic_load(&bb.loop_overdub) ? "ON" : "off");
        return;
    case 'f':
        atomic_store(&FL()->ctl[LCTL_SPC_FREEZE],
                     !atomic_load(&FL()->ctl[LCTL_SPC_FREEZE]));
        ui_status("layer %d SPACE %s", focus() + 1,
                  atomic_load(&FL()->ctl[LCTL_SPC_FREEZE]) ? "FROZEN" : "live");
        return;
    case 'x':
        atomic_store(&bb.loop_cmd, LOOP_CMD_CLEAR);
        ui_status("master phrase cleared");
        return;
    case 'z': toggle_record(); return;
    case 'K': atomic_store(&bb.reset_loop, 1); return;
    case '?': help_page = 0; return;
    case 'X': case 3: quit = 1; return;
    default: return;
    }
}

static void key_insert(int c)
{
    switch (c) {
    case 27: mode_ui = M_NORMAL; ed_commit(); return;
    case '\n': case '\r': case KEY_ENTER: mode_ui = M_NORMAL; ed_commit(); return;

    case KEY_LEFT:  if (ed_cur > 0)      ed_cur--; return;
    case KEY_RIGHT: if (ed_cur < ed_len) ed_cur++; return;
    case KEY_HOME:  case 1: ed_cur = 0;      return;
    case KEY_END:   case 5: ed_cur = ed_len; return;

    case 11: ed_len = ed_cur; ed[ed_len] = '\0'; ed_commit(); return;
    case 21: ed_len = ed_cur = 0; ed[0] = '\0'; ed_commit(); return;

    case KEY_BACKSPACE: case 127: case 8:
        if (ed_cur > 0) {
            memmove(ed + ed_cur - 1, ed + ed_cur, (size_t)(ed_len - ed_cur + 1));
            ed_cur--; ed_len--; ed_commit();
        }
        return;

    case KEY_DC:
        if (ed_cur < ed_len) {
            memmove(ed + ed_cur, ed + ed_cur + 1, (size_t)(ed_len - ed_cur));
            ed_len--; ed_commit();
        }
        return;

    default: break;
    }

    if (c >= 32 && c < 127 && ed_len < BB_EXPR_MAX - 1) {
        memmove(ed + ed_cur + 1, ed + ed_cur, (size_t)(ed_len - ed_cur + 1));
        ed[ed_cur++] = (char)c;
        ed_len++;
        ed_commit();
    }
}

/* ======================================================================== */

int ui_init(int use_tty)
{
    uirng ^= (uint32_t)time(NULL);

    if (use_tty) {
        tty_out = fopen("/dev/tty", "w");
        tty_in  = fopen("/dev/tty", "r");
        if (!tty_out || !tty_in) return -1;
        scr = newterm(NULL, tty_out, tty_in);
        if (!scr) return -1;
        set_term(scr);
    } else {
        if (!initscr()) return -1;
    }

    raw();
    noecho();
    nonl();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(20);

#ifdef NCURSES_VERSION
    set_escdelay(25);
#endif

    if (has_colors()) {
        start_color();
        use_default_colors();
        init_pair(C_FRAME, COLOR_CYAN,    -1);
        init_pair(C_VAL,   COLOR_WHITE,   -1);
        init_pair(C_ERR,   COLOR_RED,     -1);
        init_pair(C_SEL,   COLOR_YELLOW,  -1);
        init_pair(C_WAVE,  COLOR_GREEN,   -1);
        init_pair(C_HOT,   COLOR_MAGENTA, -1);
        init_pair(C_PITCH, COLOR_MAGENTA, -1);
        init_pair(C_TIME,  COLOR_CYAN,    -1);
        init_pair(C_SPACE, COLOR_BLUE,    -1);
        use_color = 1;
    }
    return 0;
}

void ui_shutdown(void)
{
    curs_set(1);
    endwin();
    if (scr) delscreen(scr);
    if (tty_out) fclose(tty_out);
    if (tty_in)  fclose(tty_in);
}

void ui_run(void)
{
    ed_set(bb_expr[focus()]);
    if (ed_len) ed_publish();
    ctrls_rebuild();

    while (!quit && !bb_quit_signal) {
        /* Housekeeping first: if we are late getting here the ring is filling
         * up, and draining it matters more than the frame rate. */
        sink_service();
        bb_reclaim();
        motion_service();

        int c = getch();
        if (c != ERR) {
            warning[0] = '\0';

            if (help_page >= 0) {
                /* Paging stays inside help; anything else closes it. */
                if (c == KEY_RIGHT || c == ' ' || c == '\t')
                    help_page = (help_page + 1) % N_HELP;
                else if (c == KEY_LEFT || c == KEY_BTAB)
                    help_page = (help_page + N_HELP - 1) % N_HELP;
                else
                    help_page = -1;
            }
            else if (c == KEY_RESIZE)     { /* draw() recomputes */ }
            else if (mode_ui == M_INSERT) key_insert(c);
            else if (mode_ui == M_SEQ)    key_seq(c);
            else if (mode_ui == M_PERF)   key_perf(c);
            else                          key_normal(c);
        }

        if (status_ttl > 0) status_ttl--;
        draw();
    }
}

/* First launch. An empty prompt teaches nothing; five racked layers show
 * the full groovebox in one screen and make a sound immediately. Each one
 * is built the same way you would build it by hand -- pick a source, choose
 * stages, set the slots -- so the opening screen is a worked example. */
void ui_first_run(void)
{
    static const struct {
        const char *src;
        int body, space, seq, steps, decay, level, drive, tone, crush;
        int spc_t, spc_fb, spc_mix, euclid;
    } START[5] = {
        /* recognizable anchors, synthesized from impacts and noise */
        { "thump", 0, 0, 1, 16, 198, 210, 55, 105,  0, 120, 150,  40, 4 },
        { "burst", 0, 0, 1, 16, 228, 155, 70, 175,  8, 100, 130,  35, 0 },
        { "metal", 1, 0, 1, 16, 242, 105, 45, 150, 20,  80, 120,  25, 8 },
        { "dust",  1, 1, 1, 15, 172,  80, 35,  90, 28, 160, 190,  75, 5 },
        /* the original instrument remains a continuous floor       */
        { "fold",  1, 1, 0, 16,   0,  85, 40,  65,  0, 190, 175,  80, 0 },
    };

    for (int L = 0; L < 5; L++) {
        atomic_store(&bb.focus, L);
        Layer *l = &bb.layer[L];

        int src = 0;
        for (int i = 0; i < rack_nsrc(); i++)
            if (!strcmp(rack_src_name(i), START[L].src)) { src = i; break; }

        bb_rack[L].src   = (unsigned char)src;
        bb_rack[L].body  = (unsigned char)START[L].body;
        bb_rack[L].space = (unsigned char)START[L].space;
        bb_rack[L].mode  = (unsigned char)rack_src_mode(src);
        bb_custom[L]     = 0;
        atomic_store(&l->mode, bb_rack[L].mode);

        rack_republish(1);

        atomic_store(&l->ctl[LCTL_LEVEL],    START[L].level);
        atomic_store(&l->ctl[LCTL_DRIVE],    START[L].drive);
        atomic_store(&l->ctl[LCTL_TONE],     START[L].tone);
        atomic_store(&l->ctl[LCTL_CRUSH],    START[L].crush);
        atomic_store(&l->ctl[LCTL_SPC_TIME], START[L].spc_t);
        atomic_store(&l->ctl[LCTL_SPC_FB],   START[L].spc_fb);
        atomic_store(&l->ctl[LCTL_SPC_MIX],  START[L].spc_mix);
        atomic_store(&l->ctl[LCTL_STEPS],    START[L].steps);
        atomic_store(&l->ctl[LCTL_DECAY],    START[L].decay);
        atomic_store(&l->ctl[LCTL_SPC_SYNC], START[L].space ? 7 : 0);
        atomic_store(&l->ctl[LCTL_SPC_FREEZE], 0);
        atomic_store(&l->seq_on,             START[L].seq);
        atomic_store(&l->on, 1);
        if (START[L].euclid) seq_euclid(START[L].euclid);
    }

    /* Backbeat rather than E(2,16), and one obvious performance flourish. */
    for (int st = 0; st < BB_STEPS; st++) atomic_store(&bb.layer[1].seq_gate[st], GATE_OFF);
    atomic_store(&bb.layer[1].seq_gate[4], GATE_ACCENT);
    atomic_store(&bb.layer[1].seq_gate[12], GATE_ON);
    atomic_store(&bb.layer[2].seq_ratchet[14], 3);
    atomic_store(&bb.layer[3].seq_prob[7], 55);
    atomic_store(&bb.layer[3].seq_prob[10], 70);

    atomic_store(&bb.gctl[GCTL_BPM], 68);
    atomic_store(&bb.focus, 0);
    ed_set(bb_expr[0]);
    ui_status("noise groove loaded. v edits hits, M records motion, Z opens performance");
}
