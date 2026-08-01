/* main.c -- startup, shutdown, the program free list, and the session file.
 *
 * The interesting part of this file is bb_publish() / bb_reclaim(): the
 * mechanism by which a newly typed expression reaches the audio thread
 * without a lock and without the audio thread ever touching the allocator.
 * Everything else here is plumbing.
 */

#include "bytebeat.h"
#include "audio.h"
#include "sink.h"
#include "ui.h"
#include "rack.h"
#include "gen.h"
#include "examples.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <stdarg.h>

struct bb_state bb;

char bb_expr[BB_NLAYER][BB_EXPR_MAX];
Rack bb_rack[BB_NLAYER];
int  bb_custom[BB_NLAYER];

volatile sig_atomic_t bb_quit_signal;

/* ======================================================================== */
/*  Program publication and reclamation                                     */
/* ======================================================================== */

/* Retired programs waiting to be freed. Touched ONLY by the UI thread, so it
 * needs no synchronisation of its own. */
static Program *retire_head;

/* THE SWAP.
 *
 * The audio thread holds a pointer to a Program and dereferences it a few
 * thousand times per period. We want to replace it while that is happening.
 *
 * A mutex would work and is completely unacceptable: if the UI thread is
 * holding the lock when the audio thread wants it, the audio thread blocks,
 * misses its deadline, and you hear a gap. Worse, it could block on a thread
 * that has been preempted and is not running at all (priority inversion).
 *
 * So instead:
 *   - the new program is built somewhere the audio thread has never seen
 *   - one atomic exchange makes it visible
 *   - the old pointer is remembered, not freed
 *
 * The release semantics of the exchange guarantee that every byte written
 * into the new Program is visible to any thread that subsequently acquires
 * the pointer. The audio thread therefore either sees the whole old program
 * or the whole new one -- never a half-written mixture -- and it never waits
 * for anything.
 *
 * The cost of not locking is that we cannot free the old program here. The
 * audio thread might be three instructions into evaluating it. That is what
 * the retire list is for.
 */
int bb_publish(int layer, const char *src, ExprError *err)
{
    if (layer < 0 || layer >= BB_NLAYER) return 0;

    Program *np = malloc(sizeof *np);
    if (!np) {
        err->ok  = 0;
        err->col = 0;
        snprintf(err->msg, sizeof err->msg, "out of memory");
        return 0;
    }

    if (!expr_compile(src, np, err)) {
        free(np);
        return 0;            /* audio thread never learns this happened */
    }

    Program *old = atomic_exchange(&bb.layer[layer].prog, np);

    if (old) {
        /* Record which period was in flight when we swapped. See bb_reclaim.
         * This load must come AFTER the exchange -- reading the epoch first
         * would let us underestimate how far the audio thread has got. */
        old->retire_epoch = atomic_load(&bb.epoch);
        old->next = retire_head;
        retire_head = old;
    }
    return 1;
}

/* WHEN IS AN OLD PROGRAM DEAD?
 *
 * The audio thread's period loop is, in order:
 *
 *     epoch++            (sequentially consistent)
 *     prog = load(prog)  (sequentially consistent)
 *     ...render with prog...
 *
 * Say we published, then read epoch == E.
 *
 * The period that incremented epoch to E may have loaded `prog` before our
 * store, so it might still be using the old program.
 *
 * The period that increments epoch to E+1 comes after our epoch read in the
 * single total order that sequential consistency provides, and its load of
 * `prog` comes after that increment. So it is guaranteed to see the new
 * pointer.
 *
 * Periods run one after another on one thread, so when we observe epoch
 * >= E+2 the period numbered E+1 has already started, which means the period
 * numbered E has already finished. Nobody can be looking at the old program.
 * Free it.
 *
 * In wall-clock terms that is about two periods, i.e. ~20ms. Programs are
 * ~6KB; typing at ten keystrokes a second retires 60KB/s and reclaims all of
 * it two frames later.
 */
void bb_reclaim(void)
{
    unsigned long long now = atomic_load(&bb.epoch);
    Program **pp = &retire_head;
    while (*pp) {
        Program *p = *pp;
        if (now >= p->retire_epoch + 2) {
            *pp = p->next;
            free(p);
        } else {
            pp = &p->next;
        }
    }
}

static void free_all_programs(void)
{
    /* Called after the audio thread has been joined, so everything is safe. */
    Program *p = retire_head;
    while (p) { Program *n = p->next; free(p); p = n; }
    retire_head = NULL;

    for (int i = 0; i < BB_NLAYER; i++)
        free(atomic_exchange(&bb.layer[i].prog, NULL));
}

/* ======================================================================== */
/*  Session file                                                            */
/* ======================================================================== */

static char cfg_dir[512];
static char cfg_path[600];

static void cfg_paths(void)
{
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg) snprintf(cfg_dir, sizeof cfg_dir, "%s/bytebeat", xdg);
    else if (home)   snprintf(cfg_dir, sizeof cfg_dir, "%s/.config/bytebeat", home);
    else             snprintf(cfg_dir, sizeof cfg_dir, ".bytebeat");
    snprintf(cfg_path, sizeof cfg_path, "%s/session.conf", cfg_dir);
}

const char *bb_config_path(void) { return cfg_path; }

static void mkdirs(const char *path)
{
    char tmp[600];
    snprintf(tmp, sizeof tmp, "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

int bb_config_save(void)
{
    mkdirs(cfg_dir);
    FILE *f = fopen(cfg_path, "w");
    if (!f) return -1;

    /* Version tag: the control layout changed when slots became layers, and
     * silently reading an old file would drop values into the wrong knobs.
     * Version 3 adds the rack -- the structured description of each voice.
     * A version 2 file has expressions but no racks, so every layer in one is
     * loaded as `custom`: the text is all we have, and inventing a rack to go
     * with it would put wrong labels on the panel. Version 4 adds expressive
     * sequence lanes, clocked/frozen SPACE, and master-looper settings. */
    fprintf(f, "# bytebeat session -- plain text, edit it if you like\n");
    fprintf(f, "version 4\n");
    fprintf(f, "rate %d\n", atomic_load(&bb.req_rate));
    fprintf(f, "gain %d\n", atomic_load(&bb.gain));
    fprintf(f, "focus %d\n", atomic_load(&bb.focus));
    fprintf(f, "looper %d %d %d %d %d %d %d\n",
            atomic_load(&bb.loop_bars), atomic_load(&bb.loop_mix),
            atomic_load(&bb.loop_feedback), atomic_load(&bb.loop_overdub),
            atomic_load(&bb.loop_rate), atomic_load(&bb.loop_reverse),
            atomic_load(&bb.loop_slice));

    fprintf(f, "gctl");
    for (int i = 0; i < GCTL_COUNT; i++) fprintf(f, " %d", atomic_load(&bb.gctl[i]));
    fprintf(f, "\n");

    for (int L = 0; L < BB_NLAYER; L++) {
        Layer *ly = &bb.layer[L];
        fprintf(f, "layer %d on %d mode %d seq %d\n", L,
                atomic_load(&ly->on), atomic_load(&ly->mode),
                atomic_load(&ly->seq_on));

        fprintf(f, "param %d", L);
        for (int i = 0; i < BB_NPARAM; i++) fprintf(f, " %d", atomic_load(&ly->param[i]));
        fprintf(f, "\n");

        fprintf(f, "lctl %d", L);
        for (int i = 0; i < LCTL_COUNT; i++) fprintf(f, " %d", atomic_load(&ly->ctl[i]));
        fprintf(f, "\n");

        fprintf(f, "gate %d", L);
        for (int i = 0; i < BB_STEPS; i++) fprintf(f, " %d", atomic_load(&ly->seq_gate[i]));
        fprintf(f, "\n");

        fprintf(f, "pitch %d", L);
        for (int i = 0; i < BB_STEPS; i++) fprintf(f, " %d", atomic_load(&ly->seq_pitch[i]));
        fprintf(f, "\n");

        fprintf(f, "ratchet %d", L);
        for (int i = 0; i < BB_STEPS; i++) fprintf(f, " %d", atomic_load(&ly->seq_ratchet[i]));
        fprintf(f, "\n");

        fprintf(f, "prob %d", L);
        for (int i = 0; i < BB_STEPS; i++) fprintf(f, " %d", atomic_load(&ly->seq_prob[i]));
        fprintf(f, "\n");

        fprintf(f, "motion %d %u\n", L, atomic_load(&ly->motion_mask));
        for (int k = 0; k < BB_LOCK_COUNT; k++) {
            fprintf(f, "lock %d %d", L, k);
            for (int i = 0; i < BB_STEPS; i++)
                fprintf(f, " %d", atomic_load(&ly->seq_lock[k][i]));
            fprintf(f, "\n");
        }

        /* After `expr`, because loading an expression marks the layer custom
         * and this line is what takes it back off again. */
        fprintf(f, "expr %d %s\n", L, bb_expr[L]);
        fprintf(f, "rack %d %d %d %d %d\n", L, bb_rack[L].src,
                bb_rack[L].body, bb_rack[L].space, bb_custom[L]);
    }

    fclose(f);
    return 0;
}

/* Read a whitespace-separated run of ints into a set of atomics. */
static void read_ints(const char *p, atomic_int *dst, int n, int lo, int hi)
{
    for (int i = 0; i < n; i++) {
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p) return;
        atomic_store(&dst[i], bb_clampi((int)v, lo, hi));
        p = end;
    }
}

/* Returns 1 if a session file was read. */
int bb_config_load(void)
{
    FILE *f = fopen(cfg_path, "r");
    if (!f) return 0;

    char line[BB_EXPR_MAX + 96];
    int  version = 1;

    while (fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (line[0] == '#' || line[0] == '\0') continue;

        int v, L;

        if (sscanf(line, "version %d", &v) == 1)      { version = v; continue; }
        if (version < 2) continue;                    /* pre-layers: ignore */

        if (sscanf(line, "rate %d", &v) == 1) {
            atomic_store(&bb.req_rate, bb_clampi(v, BB_RATE_MIN, BB_RATE_MAX));
        } else if (sscanf(line, "gain %d", &v) == 1) {
            atomic_store(&bb.gain, bb_clampi(v, 0, 256));
        } else if (sscanf(line, "focus %d", &v) == 1) {
            atomic_store(&bb.focus, bb_clampi(v, 0, BB_NLAYER - 1));
        } else if (!strncmp(line, "looper ", 7)) {
            int bars, mix, fb, od, rt, rev, slice;
            if (sscanf(line, "looper %d %d %d %d %d %d %d",
                       &bars, &mix, &fb, &od, &rt, &rev, &slice) == 7) {
                atomic_store(&bb.loop_bars, bb_clampi(bars, 1, 4));
                atomic_store(&bb.loop_mix, bb_clampi(mix, 0, 256));
                atomic_store(&bb.loop_feedback, bb_clampi(fb, 0, 256));
                atomic_store(&bb.loop_overdub, !!od);
                atomic_store(&bb.loop_rate, bb_clampi(rt, LOOP_RATE_HALF, LOOP_RATE_DOUBLE));
                atomic_store(&bb.loop_reverse, !!rev);
                if (slice != 1 && slice != 2 && slice != 4 && slice != 8 && slice != 16)
                    slice = 1;
                atomic_store(&bb.loop_slice, slice);
            }
        } else if (!strncmp(line, "gctl ", 5)) {
            read_ints(line + 5, bb.gctl, GCTL_COUNT, -100000, 100000);
            for (int i = 0; i < GCTL_COUNT; i++)
                atomic_store(&bb.gctl[i], bb_clampi(atomic_load(&bb.gctl[i]),
                             bb_gctl_info[i].lo, bb_gctl_info[i].hi));
        } else if (!strncmp(line, "layer ", 6)) {
            int on, md, sq;
            if (sscanf(line, "layer %d on %d mode %d seq %d", &L, &on, &md, &sq) == 4
                && L >= 0 && L < BB_NLAYER) {
                atomic_store(&bb.layer[L].on, !!on);
                atomic_store(&bb.layer[L].mode, bb_clampi(md, 0, BB_NMODE - 1));
                atomic_store(&bb.layer[L].seq_on, !!sq);
            }
        } else if (!strncmp(line, "param ", 6)) {
            const char *p = line + 6;
            L = (int)strtol(p, (char **)&p, 10);
            if (L >= 0 && L < BB_NLAYER)
                read_ints(p, bb.layer[L].param, BB_NPARAM, 0, 255);
        } else if (!strncmp(line, "lctl ", 5)) {
            const char *p = line + 5;
            L = (int)strtol(p, (char **)&p, 10);
            if (L >= 0 && L < BB_NLAYER) {
                read_ints(p, bb.layer[L].ctl, LCTL_COUNT, -100000, 100000);
                for (int i = 0; i < LCTL_COUNT; i++)
                    atomic_store(&bb.layer[L].ctl[i],
                        bb_clampi(atomic_load(&bb.layer[L].ctl[i]),
                                  bb_lctl_info[i].lo, bb_lctl_info[i].hi));
            }
        } else if (!strncmp(line, "gate ", 5)) {
            const char *p = line + 5;
            L = (int)strtol(p, (char **)&p, 10);
            if (L >= 0 && L < BB_NLAYER)
                read_ints(p, bb.layer[L].seq_gate, BB_STEPS, 0, 2);
        } else if (!strncmp(line, "pitch ", 6)) {
            const char *p = line + 6;
            L = (int)strtol(p, (char **)&p, 10);
            if (L >= 0 && L < BB_NLAYER)
                read_ints(p, bb.layer[L].seq_pitch, BB_STEPS, -12, 12);
        } else if (!strncmp(line, "ratchet ", 8)) {
            const char *p = line + 8;
            L = (int)strtol(p, (char **)&p, 10);
            if (L >= 0 && L < BB_NLAYER)
                read_ints(p, bb.layer[L].seq_ratchet, BB_STEPS, 1, 4);
        } else if (!strncmp(line, "prob ", 5)) {
            const char *p = line + 5;
            L = (int)strtol(p, (char **)&p, 10);
            if (L >= 0 && L < BB_NLAYER)
                read_ints(p, bb.layer[L].seq_prob, BB_STEPS, 0, 100);
        } else if (!strncmp(line, "motion ", 7)) {
            unsigned mask;
            if (sscanf(line, "motion %d %u", &L, &mask) == 2 &&
                L >= 0 && L < BB_NLAYER)
                atomic_store(&bb.layer[L].motion_mask,
                             mask & ((1u << BB_LOCK_COUNT) - 1u));
        } else if (!strncmp(line, "lock ", 5)) {
            const char *p = line + 5;
            int target;
            L = (int)strtol(p, (char **)&p, 10);
            target = (int)strtol(p, (char **)&p, 10);
            if (L >= 0 && L < BB_NLAYER && target >= 0 && target < BB_LOCK_COUNT)
                read_ints(p, bb.layer[L].seq_lock[target], BB_STEPS, -1, 256);
        } else if (!strncmp(line, "expr ", 5)) {
            const char *p = line + 5;
            L = (int)strtol(p, (char **)&p, 10);
            while (*p == ' ') p++;
            if (L >= 0 && L < BB_NLAYER) {
                size_t sl = strlen(p);
                if (sl >= BB_EXPR_MAX) sl = BB_EXPR_MAX - 1;
                memcpy(bb_expr[L], p, sl);
                bb_expr[L][sl] = '\0';
                /* Assume the worst until a `rack` line says otherwise. A
                 * version 2 file never has one, so its layers stay custom --
                 * which is the truth about them. */
                bb_custom[L] = bb_expr[L][0] != '\0';
            }
        } else if (!strncmp(line, "rack ", 5)) {
            int src, body, space, cust;
            if (sscanf(line, "rack %d %d %d %d %d",
                       &L, &src, &body, &space, &cust) == 5 &&
                L >= 0 && L < BB_NLAYER) {
                bb_rack[L].src   = (unsigned char)bb_clampi(src, 0, rack_nsrc() - 1);
                bb_rack[L].body  = (unsigned char)!!body;
                bb_rack[L].space = (unsigned char)!!space;
                bb_rack[L].mode  = (unsigned char)atomic_load(&bb.layer[L].mode);
                bb_custom[L]     = !!cust;
            }
        }
    }
    fclose(f);
    return version >= 2;
}

/* ======================================================================== */
/*  Startup                                                                 */
/* ======================================================================== */

static void on_signal(int sig) { (void)sig; bb_quit_signal = 1; }

static void set_defaults(void)
{
    memset(bb_expr, 0, sizeof bb_expr);
    atomic_store(&bb.rate,     44100);
    atomic_store(&bb.req_rate, 44100);
    atomic_store(&bb.gain,     180);
    atomic_store(&bb.focus,    0);

    atomic_store(&bb.loop_status,   LOOP_OFF);
    atomic_store(&bb.loop_cmd,      LOOP_CMD_NONE);
    atomic_store(&bb.loop_bars,     1);
    atomic_store(&bb.loop_mix,      256);
    atomic_store(&bb.loop_feedback, 192);
    atomic_store(&bb.loop_overdub,  0);
    atomic_store(&bb.loop_rate,     LOOP_RATE_NORMAL);
    atomic_store(&bb.loop_reverse,  0);
    atomic_store(&bb.loop_slice,    1);
    atomic_store(&bb.loop_pos,      0);
    atomic_store(&bb.loop_frames,   0);

    atomic_store(&bb.mute,      0);
    atomic_store(&bb.panic,     0);
    atomic_store(&bb.bypass,    0);
    atomic_store(&bb.reset_t,   0);
    atomic_store(&bb.reset_loop, 0);
    atomic_store(&bb.seq_pos,  -1);

    atomic_store(&bb.gctl[GCTL_BPM],   90);
    atomic_store(&bb.gctl[GCTL_BEATS],  4);
    atomic_store(&bb.gctl[GCTL_BARS],   2);
    atomic_store(&bb.gctl[GCTL_ZOOM],  32);

    for (int L = 0; L < BB_NLAYER; L++) {
        Layer *ly = &bb.layer[L];
        rack_default(&bb_rack[L]);
        bb_custom[L] = 0;
        atomic_store(&ly->on,   L == 0);
        atomic_store(&ly->mode, BB_WORD);
        atomic_store(&ly->seq_on, 0);
        for (int p = 0; p < BB_NPARAM; p++)
            atomic_store(&ly->param[p], 0);
        atomic_store(&ly->ctl[LCTL_LEVEL],    L == 0 ? 200 : 140);
        atomic_store(&ly->ctl[LCTL_DRIVE],    0);
        atomic_store(&ly->ctl[LCTL_TONE],     255);
        atomic_store(&ly->ctl[LCTL_CRUSH],    0);
        atomic_store(&ly->ctl[LCTL_SPC_TIME], 100);
        atomic_store(&ly->ctl[LCTL_SPC_FB],   0);
        atomic_store(&ly->ctl[LCTL_SPC_MIX],  0);
        atomic_store(&ly->ctl[LCTL_STEPS],    16);
        atomic_store(&ly->ctl[LCTL_DECAY],    90);
        atomic_store(&ly->ctl[LCTL_SPC_SYNC], 0);
        atomic_store(&ly->ctl[LCTL_SPC_FREEZE], 0);
        atomic_store(&ly->motion_mask, 0);
        for (int i = 0; i < BB_STEPS; i++) {
            atomic_store(&ly->seq_gate[i], GATE_OFF);
            atomic_store(&ly->seq_pitch[i], 0);
            atomic_store(&ly->seq_ratchet[i], 1);
            atomic_store(&ly->seq_prob[i], 100);
            for (int k = 0; k < BB_LOCK_COUNT; k++)
                atomic_store(&ly->seq_lock[k][i], -1);
        }
    }
}

static void usage(const char *argv0)
{
    printf(
"usage: %s [options]\n"
"\n"
"  -d DEV     ALSA device (default \"default\"; \"none\" = no sound card,\n"
"             the engine free-runs and you listen over the stream)\n"
"  -r RATE    initial sample rate, %d..%d\n"
"  -R         disable ALSA's software resampling. The device then snaps to\n"
"             the nearest rate the hardware really supports and the readout\n"
"             shows \"48000(want 1000)\" so you know. Without -R, ALSA happily\n"
"             accepts 1000Hz and resamples it, which smooths away exactly the\n"
"             aliasing that makes low-rate bytebeat sound the way it does.\n"
"  -s PORT    serve raw s16le mono PCM over TCP on PORT\n"
"  -L         bind that stream to 127.0.0.1 only (use with ssh -L)\n"
"  -O         write raw s16le PCM to stdout; the TUI moves to /dev/tty\n"
"  -e EXPR    start with this expression\n"
"  -E EXPR    evaluate EXPR headlessly, print decimal samples, exit\n"
"  -n N       how many samples -E should print (default 256)\n"
"  -p LIST    set p0..p7, e.g. -p 12,8,63   (also applies to -E)\n"
"  -T         run the headless regression suite and exit\n"
"  -h         this message\n"
"\n"
"listening from another machine:\n"
"  %s -d none -s 9000\n"
"  ffplay -nodisp -f s16le -ar 44100 -ac 1 -i tcp://THISBOX:9000\n"
"  # or over your existing ssh session:\n"
"  ssh -L 9000:localhost:9000 thisbox     then    nc localhost 9000 | ffplay -f s16le -ar 44100 -ac 1 -\n"
"\n", argv0, BB_RATE_MIN, BB_RATE_MAX, argv0);
}

/* Headless evaluation. Useful for checking the parser without a sound card,
 * and for eyeballing what an expression actually produces. */
static int eval_mode(const char *src, int count)
{
    static Program pr;
    static int32_t delay[EXPR_DELAY_LEN];
    ExprError err;

    if (!expr_compile(src, &pr, &err)) {
        fprintf(stderr, "parse error at column %d: %s\n", err.col, err.msg);
        fprintf(stderr, "  %s\n  %*s^\n", src, err.col, "");
        return 1;
    }

    ExprCtx c;
    memset(&c, 0, sizeof c);
    c.dly = delay;
    c.rng = 0x1234567u;
    c.sr  = atomic_load(&bb.rate);
    c.bl  = c.sr / 2;
    c.ll  = c.sr * 2;
    for (int i = 0; i < BB_NPARAM; i++) c.p[i] = atomic_load(&bb.layer[0].param[i]);

    for (int i = 0; i < count; i++) {
        c.t  = i;
        c.k  = i % (c.ll ? c.ll : 1);
        c.bt = i % (c.bl ? c.bl : 1);
        c.n  = c.bl ? i / (c.bl * 4) : 0;
        c.tr = (i == 0);
        c.age = i;
        c.vel = 256;
        printf("%d\n", expr_eval(&pr, &c));
        c.dw = (c.dw + 1u) & EXPR_DELAY_MASK;
    }
    return 0;
}

/* ======================================================================== */
/*  Built-in regression suite                                               */
/* ======================================================================== */

static int test_checks;
static int test_failures;

static void test_expect(int ok, const char *fmt, ...)
{
    test_checks++;
    if (ok) return;

    test_failures++;
    fputs("not ok - ", stderr);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void test_expression_vm(void)
{
    static int32_t dly[EXPR_DELAY_LEN];
    Program pr;
    ExprError er;
    ExprCtx c;

    test_expect(expr_compile("tr*100000+age+vel", &pr, &er),
                "trigger expression compiles: %s", er.msg);
    test_expect(pr.uses_trigger, "trigger expression advertises its inputs");

    memset(&c, 0, sizeof c);
    c.dly = dly;
    c.rng = 0x1234567u;
    c.tr = 1;
    c.vel = 256;
    test_expect(expr_eval(&pr, &c) == 100256,
                "tr, age and vel evaluate with the documented values");

    test_expect(expr_compile("bp(tr*vel*4096,p0,p1)", &pr, &er),
                "resonator expression compiles: %s", er.msg);
    test_expect(pr.uses_trigger && pr.role[0] == ROLE_RESON &&
                pr.role[1] == ROLE_Q,
                "bp inputs infer resonator and resonance controls");

    memset(dly, 0, sizeof dly);
    memset(&c, 0, sizeof c);
    c.dly = dly;
    c.rng = 0x1234567u;
    c.p[0] = 20;
    c.p[1] = 244;
    int tail = 0;
    for (int i = 0; i < 2048; i++) {
        c.tr = i == 0;
        c.age = i;
        c.vel = 256;
        int32_t v = expr_eval(&pr, &c);
        if (i > 8 && v != 0) tail++;
    }
    test_expect(tail > 1000, "bp rings after a one-sample trigger");
    test_expect(!expr_compile("bp(1,2)", &pr, &er),
                "bp rejects an incomplete argument list");
}

static void test_rack_and_generator(void)
{
    Program pr;
    ExprError er;
    int audible = 0;
    int triggered_sources = 0;
    static const char *const expected_source[] = {
        "ramp", "pair", "fold", "gate", "ring", "noise", "snare",
        "crackle", "stack", "pulse", "bell", "thump", "burst",
        "metal", "dust", "rumble", "feedback"
    };

    test_expect(rack_nsrc() == (int)(sizeof expected_source / sizeof expected_source[0]),
                "rack source count matches the stable session index table");

    for (int src = 0; src < rack_nsrc(); src++) {
        test_expect(!strcmp(rack_src_name(src), expected_source[src]),
                    "rack source index %d remains %s", src, expected_source[src]);
        triggered_sources += rack_src_triggered(src) != 0;
        for (int body = 0; body <= 1; body++) {
            for (int space = 0; space <= 1; space++) {
                Rack r = { (unsigned char)src, (unsigned char)body,
                           (unsigned char)space,
                           (unsigned char)rack_src_mode(src) };
                RackBuild b;
                rack_build(&r, &b);
                test_expect(b.nslot > 0 && b.nslot <= BB_NPARAM,
                            "rack source %s has a valid control count",
                            rack_src_name(src));
                test_expect(expr_compile(b.expr, &pr, &er),
                            "rack source %s compiles with body=%d space=%d: %s",
                            rack_src_name(src), body, space, er.msg);
            }
        }

        Voice v;
        memset(&v, 0, sizeof v);
        v.rack.src = (unsigned char)src;
        v.rack.mode = (unsigned char)rack_src_mode(src);
        v.mode = rack_src_mode(src);
        RackBuild b;
        rack_build(&v.rack, &b);
        snprintf(v.expr, sizeof v.expr, "%s", b.expr);
        rack_seed_params(&b, v.p);
        int level = gen_measure(&v);
        test_expect(level > 0, "rack source %s auditions above silence",
                    rack_src_name(src));
        if (level > 0) audible++;
    }
    test_expect(audible == rack_nsrc(), "all %d rack sources are audible",
                rack_nsrc());
    test_expect(triggered_sources == 6, "rack exposes all six triggered engines");

    for (int n = 1; n <= BB_STEPS; n++) {
        for (int pulses = 0; pulses <= n; pulses++) {
            int gate[BB_STEPS];
            gen_euclid(n, pulses, gate);
            int count = 0;
            for (int i = 0; i < n; i++) count += gate[i] != GATE_OFF;
            test_expect(count == pulses,
                        "euclidean %d/%d contains exactly %d pulses",
                        pulses, n, pulses);
            test_expect(pulses == 0 || gate[0] == GATE_ACCENT,
                        "euclidean %d/%d starts on the downbeat", pulses, n);
        }
    }

    static const unsigned seeds[] = {
        1u, 0x12345678u, 0xdeadbeefu, 0x0101ffffu,
        0x7fffffffu, 0x80000000u, 0xc001d00du, 0xffffffffu
    };
    int saw_triggered_roll = 0;
    int saw_articulated_roll = 0;
    for (size_t i = 0; i < sizeof seeds / sizeof seeds[0]; i++) {
        Voice a, b;
        unsigned sa = gen_roll(seeds[i], &a);
        unsigned sb = gen_roll(seeds[i], &b);
        test_expect(sa == sb && memcmp(&a, &b, sizeof a) == 0,
                    "generator seed %u round-trips exactly", seeds[i]);
        test_expect(a.level >= 6 && a.level <= 85,
                    "generator seed %u passes the audible-level contract",
                    seeds[i]);
        if (rack_src_triggered(a.rack.src) && a.seq_on) saw_triggered_roll = 1;
        for (int s = 0; s < BB_STEPS; s++) {
            test_expect(a.ratchet[s] >= 1 && a.ratchet[s] <= 4,
                        "generated ratchet is in range");
            test_expect(a.prob[s] >= 0 && a.prob[s] <= 100,
                        "generated probability is in range");
            if (a.ratchet[s] > 1 || a.prob[s] < 100) saw_articulated_roll = 1;
            for (int k = 0; k < BB_LOCK_COUNT; k++)
                test_expect(a.lock[k][s] >= -1 && a.lock[k][s] <= 256,
                            "generated lock is in range");
        }
    }
    test_expect(saw_triggered_roll,
                "generator seeds include a sequenced triggered engine");
    test_expect(saw_articulated_roll,
                "generator seeds exercise ratchet or probability articulation");
}

static void test_session_roundtrip(void)
{
    char tmp[] = "/tmp/bytebeat-selftest.XXXXXX";
    char *root = mkdtemp(tmp);
    test_expect(root != NULL, "temporary session directory can be created");
    if (!root) return;

    snprintf(cfg_dir, sizeof cfg_dir, "%s/bytebeat", root);
    snprintf(cfg_path, sizeof cfg_path, "%s/session.conf", cfg_dir);

    set_defaults();
    memset(bb_expr, 0, sizeof bb_expr);
    atomic_store(&bb.req_rate, 48000);
    atomic_store(&bb.gain, 201);
    atomic_store(&bb.focus, 2);
    atomic_store(&bb.loop_bars, 4);
    atomic_store(&bb.loop_mix, 137);
    atomic_store(&bb.loop_feedback, 91);
    atomic_store(&bb.loop_overdub, 1);
    atomic_store(&bb.loop_rate, LOOP_RATE_DOUBLE);
    atomic_store(&bb.loop_reverse, 1);
    atomic_store(&bb.loop_slice, 8);
    Layer *ly = &bb.layer[2];
    atomic_store(&ly->on, 1);
    atomic_store(&ly->mode, BB_SIGNED);
    atomic_store(&ly->seq_on, 1);
    atomic_store(&ly->param[1], 222);
    atomic_store(&ly->ctl[LCTL_SPC_SYNC], 10);
    atomic_store(&ly->ctl[LCTL_SPC_FREEZE], 1);
    atomic_store(&ly->seq_gate[3], GATE_ACCENT);
    atomic_store(&ly->seq_pitch[3], -7);
    atomic_store(&ly->seq_ratchet[3], 4);
    atomic_store(&ly->seq_prob[3], 37);
    atomic_store(&ly->seq_lock[LOCK_TONE][3], 123);
    atomic_store(&ly->motion_mask, 1u << LOCK_TONE);
    snprintf(bb_expr[2], BB_EXPR_MAX, "bp(tr*vel*4096,p0,p1)");
    bb_rack[2].src = (unsigned char)(rack_nsrc() - 1);
    bb_rack[2].body = 1;
    bb_rack[2].space = 1;
    bb_custom[2] = 0;

    test_expect(bb_config_save() == 0, "version 4 session can be saved");

    FILE *f = fopen(cfg_path, "r");
    int saw_v4 = 0;
    if (f) {
        char line[128];
        while (fgets(line, sizeof line, f))
            if (!strcmp(line, "version 4\n")) saw_v4 = 1;
        fclose(f);
    }
    test_expect(saw_v4, "saved session carries the version 4 marker");

    set_defaults();
    memset(bb_expr, 0, sizeof bb_expr);
    test_expect(bb_config_load() == 1, "version 4 session can be loaded");
    ly = &bb.layer[2];
    test_expect(atomic_load(&bb.req_rate) == 48000 &&
                atomic_load(&bb.gain) == 201 && atomic_load(&bb.focus) == 2,
                "master state survives a session round-trip");
    test_expect(atomic_load(&bb.loop_bars) == 4 &&
                atomic_load(&bb.loop_mix) == 137 &&
                atomic_load(&bb.loop_feedback) == 91 &&
                atomic_load(&bb.loop_overdub) == 1 &&
                atomic_load(&bb.loop_rate) == LOOP_RATE_DOUBLE &&
                atomic_load(&bb.loop_reverse) == 1 &&
                atomic_load(&bb.loop_slice) == 8,
                "phrase-looper controls survive a session round-trip");
    test_expect(atomic_load(&ly->param[1]) == 222 &&
                atomic_load(&ly->ctl[LCTL_SPC_SYNC]) == 10 &&
                atomic_load(&ly->ctl[LCTL_SPC_FREEZE]) == 1,
                "voice and SPACE controls survive a session round-trip");
    test_expect(atomic_load(&ly->seq_gate[3]) == GATE_ACCENT &&
                atomic_load(&ly->seq_pitch[3]) == -7 &&
                atomic_load(&ly->seq_ratchet[3]) == 4 &&
                atomic_load(&ly->seq_prob[3]) == 37 &&
                atomic_load(&ly->seq_lock[LOCK_TONE][3]) == 123 &&
                atomic_load(&ly->motion_mask) == (1u << LOCK_TONE),
                "all expressive sequencer lanes survive a session round-trip");
    test_expect(!strcmp(bb_expr[2], "bp(tr*vel*4096,p0,p1)") &&
                bb_rack[2].src == rack_nsrc() - 1 && bb_rack[2].body &&
                bb_rack[2].space && !bb_custom[2],
                "expression and rack identity survive a session round-trip");

    /* A short v3 control line must leave every appended v4 field at its
     * default. This is the compatibility path real existing sessions use. */
    f = fopen(cfg_path, "w");
    if (f) {
        fputs("version 3\n"
              "rate 22050\n"
              "layer 0 on 1 mode 2 seq 1\n"
              "lctl 0 111 22 133 44 155 66 77 8 99\n"
              "gate 0 2 0 1 0 0 0 0 0 0 0 0 0 0 0 0 0\n"
              "pitch 0 -3 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n"
              "expr 0 t*p0\n"
              "rack 0 0 0 0 0\n", f);
        fclose(f);
    }
    test_expect(f != NULL, "legacy session fixture can be written");
    set_defaults();
    memset(bb_expr, 0, sizeof bb_expr);
    test_expect(bb_config_load() == 1, "version 3 session remains loadable");
    ly = &bb.layer[0];
    test_expect(atomic_load(&ly->ctl[LCTL_LEVEL]) == 111 &&
                atomic_load(&ly->ctl[LCTL_DECAY]) == 99 &&
                atomic_load(&ly->ctl[LCTL_SPC_SYNC]) == 0 &&
                atomic_load(&ly->ctl[LCTL_SPC_FREEZE]) == 0,
                "v3 controls load without shifting the appended v4 controls");
    test_expect(atomic_load(&ly->seq_ratchet[0]) == 1 &&
                atomic_load(&ly->seq_prob[0]) == 100 &&
                atomic_load(&ly->seq_lock[LOCK_P0][0]) == -1,
                "v3 sessions receive safe expressive-lane defaults");

    unlink(cfg_path);
    rmdir(cfg_dir);
    rmdir(root);
}

static int self_test_mode(void)
{
    test_checks = test_failures = 0;
    puts("bytebeat self-test");

    test_expression_vm();
    test_rack_and_generator();

    char err[160];
    test_expect(audio_self_test(err, sizeof err),
                "audio clock/phrase invariants: %s", err);
    test_session_roundtrip();

    if (test_failures) {
        fprintf(stderr, "%d of %d checks failed\n", test_failures, test_checks);
        return 1;
    }
    printf("all %d checks passed (%d sources, session v3/v4)\n",
           test_checks, rack_nsrc());
    return 0;
}

int main(int argc, char **argv)
{
    const char *dev       = "default";
    const char *start_exp = NULL;
    const char *eval_exp  = NULL;
    const char *param_list = NULL;
    int rate       = 0;
    int resample   = 1;
    int port       = 0;
    int local_only = 0;
    int to_stdout  = 0;
    int eval_n     = 256;
    int self_test  = 0;

    set_defaults();
    cfg_paths();

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if      (!strcmp(a, "-d") && i + 1 < argc) dev = argv[++i];
        else if (!strcmp(a, "-r") && i + 1 < argc) rate = atoi(argv[++i]);
        else if (!strcmp(a, "-R"))                 resample = 0;
        else if (!strcmp(a, "-s") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(a, "-L"))                 local_only = 1;
        else if (!strcmp(a, "-O"))                 to_stdout = 1;
        else if (!strcmp(a, "-e") && i + 1 < argc) start_exp = argv[++i];
        else if (!strcmp(a, "-E") && i + 1 < argc) eval_exp = argv[++i];
        else if (!strcmp(a, "-n") && i + 1 < argc) eval_n = atoi(argv[++i]);
        else if (!strcmp(a, "-p") && i + 1 < argc) param_list = argv[++i];
        else if (!strcmp(a, "-T") || !strcmp(a, "--self-test")) self_test = 1;
        else if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(argv[0]); return 0; }
        else { fprintf(stderr, "unknown option: %s\n", a); usage(argv[0]); return 2; }
    }

    if (self_test) return self_test_mode();

    int had_config = bb_config_load();
    if (rate > 0) atomic_store(&bb.req_rate, bb_clampi(rate, BB_RATE_MIN, BB_RATE_MAX));

    if (param_list) {
        const char *p = param_list;
        for (int i = 0; i < BB_NPARAM && *p; i++) {
            char *end;
            long v = strtol(p, &end, 10);
            if (end == p) break;
            atomic_store(&bb.layer[0].param[i], bb_clampi((int)v, 0, 255));
            p = end;
            while (*p == ',' || *p == ' ') p++;
        }
    }

    if (eval_exp) return eval_mode(eval_exp, eval_n > 0 ? eval_n : 256);

    /* A broken pipe on the PCM stream must not kill the process -- the whole
     * point of the sink is that a disappearing listener is a non-event. */
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);

    char warn[256] = "";
    char err[256]  = "";

    /* LOCK EVERYTHING INTO RAM.
     *
     * A page fault is a trip into the kernel and possibly to disk. On the UI
     * thread that is invisible. On the audio thread, in the middle of a
     * period, it is an xrun. mlockall(MCL_CURRENT) pins everything mapped
     * right now -- including the 1MB delay line and the 2MB sample ring in
     * BSS -- and MCL_FUTURE pins anything mapped later, which covers the
     * Programs we malloc while live coding.
     *
     * It needs CAP_IPC_LOCK or a generous RLIMIT_MEMLOCK. Failing is not
     * fatal; it just means occasional glitches under memory pressure. */
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        snprintf(warn, sizeof warn,
                 "mlockall failed (%s) - run 'make caps' to fix", strerror(errno));

    if (sink_init(port, local_only, to_stdout, err, sizeof err) < 0) {
        fprintf(stderr, "stream: %s\n", err);
        return 1;
    }

    if (audio_open(dev, atomic_load(&bb.req_rate), resample, err, sizeof err) < 0) {
        fprintf(stderr, "audio: %s\n", err);
        fprintf(stderr,
                "\nTry:  %s -d none -s 9000   (no card; listen over the network)\n"
                "or:   %s -d plughw:0,0\n", argv[0], argv[0]);
        sink_close();
        return 1;
    }

    /* Publish something before the audio thread starts so it never sees a
     * NULL program. */
    /* Publish something to every layer before the audio thread starts so it
     * never sees a NULL program. */
    ExprError e0;
    for (int L = 0; L < BB_NLAYER; L++)
        if (!bb_publish(L, bb_expr[L][0] ? bb_expr[L] : "0", &e0))
            bb_publish(L, "0", &e0);

    char awarn[256] = "";
    if (audio_start(awarn, sizeof awarn) < 0) {
        fprintf(stderr, "audio: %s\n", awarn);
        audio_close();
        sink_close();
        return 1;
    }
    char allwarn[560];
    snprintf(allwarn, sizeof allwarn, "%s%s%s",
             warn, (warn[0] && awarn[0]) ? " | " : "", awarn);

    if (ui_init(to_stdout) < 0) {
        fprintf(stderr, "cannot initialise terminal\n");
        audio_stop();
        audio_close();
        sink_close();
        return 1;
    }

    if (allwarn[0]) ui_set_warning(allwarn);

    if (start_exp) {
        /* An expression from the command line is by definition hand-written,
         * so no rack describes it. */
        snprintf(bb_expr[0], BB_EXPR_MAX, "%s", start_exp);
        bb_custom[0] = 1;
        atomic_store(&bb.layer[0].on, 1);
    } else if (!had_config) {
        /* First run: put a complete noise groove on the table rather than an
         * empty prompt -- triggered low body, backbeat, ratcheted metal,
         * probabilistic dust and a continuous bytebeat floor. */
        ui_first_run();
    }

    ui_run();

    ui_shutdown();

    audio_stop();
    sink_close();
    bb_config_save();
    free_all_programs();
    audio_close();

    printf("session saved to %s\n", cfg_path);
    return 0;
}
