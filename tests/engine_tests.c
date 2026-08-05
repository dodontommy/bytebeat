/* engine_tests.c -- the regression suite, lifted out of the terminal driver.
 *
 * This is the whole of the `-T` self-test that used to live inside main.c,
 * moved out unchanged. It had to move: main.c is the ncurses front end, it
 * needs ALSA and a terminal to link, and it is being retired -- but the suite
 * inside it is the only thing in the tree that can say whether a port changed
 * the SOUND. Losing it to a platform migration would be losing the one
 * instrument that measures the migration.
 *
 * So this target links bbengine and nothing else. No ALSA, no ncurses, no
 * JUCE, no sound card, no terminal. That is not a convenience, it is the
 * contract engine.h has been advertising all along ("the regression suite can
 * exercise the full DSP, sequencer, phrase looper and session round-trip on
 * ANY platform"). This file is that promise being collected.
 *
 * Three things had to change on the way across, and only three:
 *
 *   1. main.c called audio_self_test(), which lives in audio.c behind ALSA.
 *      That function is a one-line forwarder to bb_engine_self_test() (see
 *      audio.c:411-417) -- the clock and phrase-loop invariants moved into
 *      the engine some time ago precisely so the GUI test runner could reach
 *      them. We call the engine entry point directly. Nothing is lost: the
 *      same checks run, against the same code, with ALSA no longer dragged
 *      in to reach them.
 *
 *   2. The two temp-directory fixtures used mkdtemp(3) on a hardcoded /tmp
 *      path. Neither exists on Windows -- there is no mkdtemp in the MSVC CRT
 *      and no /tmp on the filesystem -- so both now go through the portable
 *      helper below.
 *
 *   3. The saved-session scan compared a line against the literal
 *      "version 7\n". That comparison is a trap on any platform where a text
 *      -mode FILE writes CRLF: the file would be perfectly correct and the
 *      test would fail. It now compares the TEXT of the line, which is what
 *      it always meant.
 *
 * Every check that ran under the terminal driver runs here, in the same
 * order, with the same wording, so the printed pass count stays directly
 * comparable to the historical numbers. The handful of checks added for the
 * port are run last and counted separately for exactly that reason.
 */

/* The C library comes first: bb_platform.h declares functions in terms of
 * FILE and uint64_t, and this file must not be the reason it depends on
 * having been included after <stdio.h> somewhere else. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>

#include "bytebeat.h"
#include "engine.h"
#include "bb_platform.h"
#include "expr.h"
#include "rack.h"
#include "gen.h"

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <unistd.h>
#  include <sys/stat.h>
#endif

/* UTF-8 spelled out in hex escapes so this source file stays pure 7-bit
 * ASCII. MSVC reads a source file in the system ANSI codepage unless it is
 * told otherwise, so an o-with-diaeresis typed literally here would arrive at
 * the engine as ONE CP-1252 byte on Windows and as TWO UTF-8 bytes on macOS
 * and Linux -- the test would quietly be testing a different string on each
 * platform, which is the opposite of what a port regression test is for.
 * The escapes are kept isolated by string concatenation because a hex escape
 * in C consumes as many hex digits as follow it, and "\xC3\xB6ad" would not
 * mean what it looks like. */
#define U8_OE "\xC3\xB6"        /* U+00F6 LATIN SMALL LETTER O WITH DIAERESIS */

static const char PORT_DIR_TAG[]   = "p" U8_OE "rt tests";
static const char PORT_CLIP_NAME[] = "c" U8_OE "ld hall II";
static const char PORT_KIT_DIR[]   = "m" U8_OE "rgue kits";

/* ======================================================================== */
/*  Portable scaffolding                                                     */
/* ======================================================================== */

/* Where the system keeps scratch directories, as UTF-8, with no trailing
 * separator. GetTempPathW is the only correct answer on Windows: the
 * directory moves with the user profile, and on a machine whose account name
 * is not ASCII the ANSI variant would hand back a lossily-transcoded path
 * that does not open. Everywhere else TMPDIR-or-/tmp is the convention, and
 * the suite has always used /tmp. */
static int tmp_base(char *out, size_t n)
{
#if defined(_WIN32)
    wchar_t w[MAX_PATH + 2];
    DWORD len = GetTempPathW((DWORD)(sizeof w / sizeof w[0]), w);
    if (len == 0 || len >= sizeof w / sizeof w[0]) return -1;
    while (len > 0 && (w[len - 1] == L'\\' || w[len - 1] == L'/')) w[--len] = L'\0';
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)n, NULL, NULL) == 0)
        return -1;
    /* The engine joins session paths with '/', and every Win32 file API
     * accepts it, so normalise here and let one separator run through the
     * whole test rather than two. */
    for (char *p = out; *p; p++) if (*p == '\\') *p = '/';
    return 0;
#else
    const char *t = getenv("TMPDIR");
    if (!t || !*t) t = "/tmp";
    snprintf(out, n, "%s", t);
    size_t l = strlen(out);
    while (l > 1 && out[l - 1] == '/') out[--l] = '\0';
    return 0;
#endif
}

/* Create a fresh scratch directory named after `tag` and write its UTF-8 path
 * into `out`. Returns 0 on success.
 *
 * This replaces mkdtemp(3), which the MSVC CRT does not have. mkdtemp's real
 * guarantee is atomic uniqueness against a hostile racer; nothing here is
 * racing anything, so uniqueness comes from the process id, the wall clock
 * and a per-call counter, and creation goes through the engine's own
 * bb_mkdirs() so the UTF-8 path is interpreted the same way the session
 * writer will interpret it a few lines later. That last part matters more
 * than the naming: if bb_mkdirs and bb_fopen disagreed about an encoding,
 * these tests would be the place it shows up. */
static int tmp_dir_make(char *out, size_t n, const char *tag)
{
    static unsigned seq;
    char base[512];
    unsigned long pid;

    if (tmp_base(base, sizeof base) != 0) return -1;
#if defined(_WIN32)
    pid = (unsigned long)GetCurrentProcessId();
#else
    pid = (unsigned long)getpid();
#endif
    snprintf(out, n, "%s/morgue-%s-%lu-%lu-%u", base, tag, pid,
             (unsigned long)time(NULL), ++seq);
    return bb_mkdirs(out);
}

/* Remove an (empty) directory named by a UTF-8 path. Deliberately not part of
 * the bb_platform contract -- the engine never removes directories, only the
 * tests do, so the knowledge stays here. Failure is ignored for the same
 * reason main.c ignored rmdir's return: a scratch directory that will not go
 * away is not a reason to fail a test about audio. */
static void tmp_dir_remove(const char *utf8_path)
{
#if defined(_WIN32)
    wchar_t w[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1, w,
                            (int)(sizeof w / sizeof w[0])) == 0) return;
    RemoveDirectoryW(w);
#else
    rmdir(utf8_path);
#endif
}

/* Compare a line handed back by fgets against `want`, ignoring whatever line
 * terminator arrived.
 *
 * The suite used to test `!strcmp(line, "version 7\n")`. That is a fine test
 * of a file written on a system whose text streams end lines with a bare LF,
 * and a false alarm everywhere else: a session saved through a CRLF text
 * stream is a completely valid session that the loader reads back perfectly,
 * and the old comparison would have called it a failure. What the check is
 * actually about is the marker TEXT, so strip the terminator and compare
 * that. */
static int line_is(const char *line, const char *want)
{
    size_t n = strlen(line);
    while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
    return strlen(want) == n && strncmp(line, want, n) == 0;
}

static void set_defaults(void)
{
    bb_engine_set_defaults();
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
        "metal", "dust", "rumble", "feedback",
        /* the cold wing -- appended only, so saved src indices stay stable */
        "cold", "vapor", "hymn", "siren", "glass"
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
    test_expect(triggered_sources == 7, "rack exposes all seven triggered engines");

    /* the patch morgue: every curated voice names a real source and makes
     * sound with its shipped settings */
    for (int i = 0; i < rack_npatch(); i++) {
        const RackPatch *pt = rack_patch(i);
        int src = -1;
        for (int s = 0; s < rack_nsrc(); s++)
            if (!strcmp(rack_src_name(s), pt->src)) { src = s; break; }
        test_expect(src >= 0, "patch %s names a real source", pt->name);
        if (src < 0) continue;

        Voice v;
        memset(&v, 0, sizeof v);
        v.rack.src  = (unsigned char)src;
        v.rack.body = pt->body;
        v.rack.space = pt->space;
        v.rack.mode = (unsigned char)rack_src_mode(src);
        v.mode = rack_src_mode(src);
        RackBuild b;
        rack_build(&v.rack, &b);
        snprintf(v.expr, sizeof v.expr, "%s", b.expr);
        rack_seed_params(&b, v.p);
        for (int o = 0; o < pt->nset && o < RACK_PATCH_SET; o++)
            v.p[pt->set[o].idx % BB_NPARAM] = pt->set[o].val;
        test_expect(gen_measure(&v) > 0, "patch %s auditions above silence",
                    pt->name);
    }

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
    char root[720];
    int made = tmp_dir_make(root, sizeof root, "selftest") == 0;
    test_expect(made, "temporary session directory can be created");
    if (!made) return;

    bb_config_set_root(root);
    const char *cfg_path = bb_config_path();

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

    SamplerSlot *smp = &bb.sampler[3];
    atomic_store(&smp->on, 1);
    atomic_store(&smp->ctl[SMP_CTL_LEVEL], 251);
    atomic_store(&smp->ctl[SMP_CTL_CHOKE], 2);
    atomic_store(&smp->mute, 0);
    atomic_store(&smp->solo, 1);
    atomic_store(&smp->gate[0], SMP_GATE_ACCENT);
    atomic_store(&smp->gate[11], SMP_GATE_ON);
    atomic_store(&smp->pitch[5], -7);
    atomic_store(&smp->vel[11], 43);

    atomic_store(&bb.layer[2].send, 77);
    atomic_store(&bb.verb_size, 201);
    atomic_store(&bb.verb_tone, 55);
    atomic_store(&bb.verb_level, 99);
    atomic_store(&bb.smp_send, 31);

    test_expect(bb_config_save() == 0, "version 4 session can be saved");

    FILE *f = bb_fopen(cfg_path, "r");
    int saw_v4 = 0;
    if (f) {
        char line[128];
        while (fgets(line, sizeof line, f))
            if (line_is(line, "version 7")) saw_v4 = 1;
        fclose(f);
    }
    test_expect(saw_v4, "saved session carries the version 7 marker");

    set_defaults();
    memset(bb_expr, 0, sizeof bb_expr);
    test_expect(bb_config_load() == 1, "version 4 session can be loaded");
    ly = &bb.layer[2];
    test_expect(atomic_load(&bb.req_rate) == 48000 &&
                atomic_load(&bb.gain) == 201 && atomic_load(&bb.focus) == 2,
                "master state survives a session round-trip");
    test_expect(atomic_load(&bb.layer[2].send) == 77 &&
                atomic_load(&bb.verb_size) == 201 &&
                atomic_load(&bb.verb_tone) == 55 &&
                atomic_load(&bb.verb_level) == 99 &&
                atomic_load(&bb.smp_send) == 31,
                "chamber sends and return survive a session round-trip");
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

    smp = &bb.sampler[3];
    test_expect(atomic_load(&smp->on) == 1 &&
                atomic_load(&smp->ctl[SMP_CTL_LEVEL]) == 251 &&
                atomic_load(&smp->ctl[SMP_CTL_CHOKE]) == 2 &&
                atomic_load(&smp->mute) == 0 && atomic_load(&smp->solo) == 1 &&
                atomic_load(&smp->gate[0]) == SMP_GATE_ACCENT &&
                atomic_load(&smp->gate[11]) == SMP_GATE_ON &&
                atomic_load(&smp->pitch[5]) == -7 &&
                atomic_load(&smp->vel[11]) == 43,
                "step-sampler pattern and slot controls survive a round-trip");

    /* A short v3 control line must leave every appended v4 field at its
     * default. This is the compatibility path real existing sessions use.
     *
     * The fixture is written in BINARY mode with explicit newlines so the
     * bytes on disk are identical on every platform. A text-mode stream would
     * put CRLF in it on Windows and the fixture would stop being the fixture
     * the historical numbers were measured against -- CRLF handling gets its
     * own dedicated test further down, where it can be asserted rather than
     * stumbled into. */
    f = bb_fopen(cfg_path, "wb");
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

    bb_remove(bb_config_path());
    {
        /* bb_config_set_root() puts session.conf directly in `root`, so the
         * "<root>/bytebeat" subdirectory the suite has always swept here has
         * not existed for some time. Sweeping it still costs nothing and
         * keeps the cleanup identical to the historical suite for anyone
         * bisecting against an older session layout. */
        char sub[600];
        snprintf(sub, sizeof sub, "%s/bytebeat", root);
        tmp_dir_remove(sub);
    }
    tmp_dir_remove(root);
}

static int smp_full(int v, int vel)
{
    long amp = (long)(vel << 8);
    return (int)(((long long)v * amp) >> 16);
}

static void test_step_sampler(void)
{
    /* Deterministic drum test: BPM 60 at 44100 puts a 16th-step at 11025
     * frames. Each scenario re-defaults the engine so the internal clock
     * (k/tick) starts at zero. Sample buffers are freshly malloc'd for each
     * scenario because the engine takes ownership of them. */
    const int rate = 44100;
    const int N = 256;
    static int16_t out[12050];

    /* ---- basic one-shot: a single hit on step 0 plays exactly the loaded
     * sample, then stops, in sync with the master clock. ---------------- */
    bb_engine_set_defaults();
    bb_engine_init(rate);
    atomic_store(&bb.gctl[GCTL_BPM], 60);
    bb_engine_reset_loop();   /* fresh clock each scenario */
    atomic_store(&bb.gain, 256);

    int16_t *bufA = malloc(N * sizeof(int16_t));
    test_expect(bufA != NULL, "sampler test buffer allocates");
    if (bufA == NULL) return;
    for (int i = 0; i < N; i++) bufA[i] = (int16_t)(i * 100);

    test_expect(bb_engine_sampler_set(0, bufA, N, rate) == 1,
                "slot 0 accepts a published sample buffer");
    SamplerSlot *s0 = &bb.sampler[0];
    atomic_store(&s0->on, 1);
    atomic_store(&s0->ctl[SMP_CTL_LEVEL], 256);
    for (int i = 0; i < BB_STEPS; i++) atomic_store(&s0->vel[i], 255);

    bb_engine_render(out, 12000, 1);              /* prelude: level saturates */
    atomic_store(&s0->gate[2], SMP_GATE_ON);      /* arm the NEXT step (step 2) */
    bb_engine_render(out, 1000, 1);               /* still inside step 1      */
    test_expect(out[999] == 0, "no sample fires before its step boundary");

    bb_engine_render(out, 10050, 1);              /* crosses into step 2      */
    int fire = 22050 - 13000;                     /* local frame of the fire  */
    test_expect(out[fire - 1] == 0, "sample waits for the step boundary");
    int j = 96;                                    /* past the attack ramp    */
    test_expect(out[fire + j] == (int16_t)smp_full(bufA[j], 255),
                "one-shot sample plays at full velocity on its step");
    test_expect(out[fire + N] == 0,
                "one-shot sample stops at its end instead of looping");
    test_expect(atomic_load(&bb.seq_pos) >= 0,
                "playhead publishes while the step sampler is running");
    bb_engine_sampler_clear(0);
    bb_engine_sampler_reclaim();

    /* ---- drum choke: two slots in the same group, only the last-fired
     * survives (higher slot index fires second in the walk). -------------- */
    bb_engine_set_defaults();
    bb_engine_init(rate);
    atomic_store(&bb.gctl[GCTL_BPM], 60);
    bb_engine_reset_loop();   /* fresh clock each scenario */
    atomic_store(&bb.gain, 256);

    int16_t *bchk0 = malloc(N * sizeof(int16_t));
    int16_t *bchk1 = malloc(N * sizeof(int16_t));
    test_expect(bchk0 != NULL && bchk1 != NULL, "choke test buffers allocate");
    if (bchk0 == NULL || bchk1 == NULL) return;
    for (int i = 0; i < N; i++) { bchk0[i] = (int16_t)(1000 + i); bchk1[i] = (int16_t)(2000 + i); }
    test_expect(bb_engine_sampler_set(0, bchk0, N, rate) == 1 &&
                bb_engine_sampler_set(1, bchk1, N, rate) == 1,
                "two choked slots accept sample buffers");
    s0 = &bb.sampler[0];
    SamplerSlot *s1 = &bb.sampler[1];
    for (int s = 0; s < 2; s++) {
        SamplerSlot *sl = s ? s1 : s0;
        atomic_store(&sl->on, 1);
        atomic_store(&sl->ctl[SMP_CTL_LEVEL], 256);
        atomic_store(&sl->ctl[SMP_CTL_CHOKE], 1);
        for (int i = 0; i < BB_STEPS; i++) atomic_store(&sl->vel[i], 255);
    }
    bb_engine_render(out, 12000, 1);
    atomic_store(&s0->gate[2], SMP_GATE_ON);
    atomic_store(&s1->gate[2], SMP_GATE_ON);
    bb_engine_render(out, 1000, 1);
    bb_engine_render(out, 10050, 1);
    int chkFire = 22050 - 13000;
    test_expect(out[chkFire + j] == (int16_t)smp_full(2000 + j, 255),
                "choke keeps only the last-fired slot in the group");
    test_expect(out[chkFire + j] != (int16_t)smp_full(1000 + j, 255),
                "choked slot contributes no audio");
    bb_engine_sampler_clear(0);
    bb_engine_sampler_clear(1);
    bb_engine_sampler_reclaim();

    /* ---- per-step pitch: +12 semitones doubles the playback rate, so the
     * j-th output frame reads the 2j-th sample. --------------------------- */
    bb_engine_set_defaults();
    bb_engine_init(rate);
    atomic_store(&bb.gctl[GCTL_BPM], 60);
    bb_engine_reset_loop();   /* fresh clock each scenario */
    atomic_store(&bb.gain, 256);

    int16_t *bpitch = malloc(N * sizeof(int16_t));
    test_expect(bpitch != NULL, "pitch test buffer allocates");
    if (bpitch == NULL) return;
    for (int i = 0; i < N; i++) bpitch[i] = (int16_t)(i * 100);
    test_expect(bb_engine_sampler_set(0, bpitch, N, rate) == 1,
                "pitch test buffer publishes");
    s0 = &bb.sampler[0];
    atomic_store(&s0->on, 1);
    atomic_store(&s0->ctl[SMP_CTL_LEVEL], 256);
    atomic_store(&s0->pitch[2], 12);
    for (int i = 0; i < BB_STEPS; i++) atomic_store(&s0->vel[i], 255);
    bb_engine_render(out, 12000, 1);
    atomic_store(&s0->gate[2], SMP_GATE_ON);
    bb_engine_render(out, 1000, 1);
    bb_engine_render(out, 10050, 1);
    int pf = 22050 - 13000;
    test_expect(out[pf + 96] == (int16_t)smp_full(19200, 255) &&
                out[pf + 96] != (int16_t)smp_full(9600, 255),
                "per-step pitch sets the playback rate of a hit");
    bb_engine_sampler_clear(0);
    bb_engine_sampler_reclaim();
}

static void test_arrangement(void)
{
    /* R2 song timeline. Bar geometry chosen for cheap renders: 8kHz at
     * 240 BPM with 4 beats puts a beat at 2000 frames and a bar at 8000,
     * so bar B spans frames [B*8000, (B+1)*8000) of the scenario. Each
     * scenario re-defaults the engine so the internal clock starts at
     * zero, exactly like the step-sampler tests. */
    const int rate = 8000;
    const int N = 256;
    static int16_t aout[16050];
    static int16_t ref[16050];
    static int16_t dst[16050];
    static int16_t data[256];
    ArrClip clip[2];
    ArrClip got[ARR_MAX_CLIPS];

    for (int i = 0; i < N; i++) data[i] = (int16_t)(i * 100);

    /* ---- clip buffer lifecycle: create copies, accessors are NULL-safe */
    test_expect(bb_engine_clip_create(NULL, 8, rate) == NULL &&
                bb_engine_clip_create(data, 0, rate) == NULL &&
                bb_engine_clip_create(data, 8, 0) == NULL,
                "clip create rejects bad arguments");
    ArrClipBuf *ab = bb_engine_clip_create(data, N, rate);
    test_expect(ab != NULL, "clip create publishes a buffer");
    if (!ab) return;
    test_expect(bb_engine_clip_frames(ab) == (unsigned)N &&
                bb_engine_clip_data(ab) != NULL &&
                bb_engine_clip_data(ab) != data &&
                memcmp(bb_engine_clip_data(ab), data, sizeof data) == 0,
                "clip audio is an independent copy of the caller's frames");
    test_expect(bb_engine_clip_frames(NULL) == 0 &&
                bb_engine_clip_data(NULL) == NULL,
                "clip accessors tolerate NULL");

    /* ---- a one-shot clip fires exactly at its start bar, then ends --- */
    bb_engine_set_defaults();
    bb_engine_init(rate);
    atomic_store(&bb.gctl[GCTL_BPM], 240);
    bb_engine_reset_loop();
    atomic_store(&bb.gain, 256);
    atomic_store(&bb.layer[0].on, 0);

    memset(clip, 0, sizeof clip);
    clip[0].lane = 3;
    clip[0].start_bar = 2;
    clip[0].len_bars = 1;
    clip[0].loop = 0;
    clip[0].gain = 256;
    clip[0].audio = ab;
    snprintf(clip[0].name, ARR_NAME_MAX, "hit");
    test_expect(bb_engine_song_publish(clip, 1) == 0, "a one-clip song publishes");
    test_expect(bb_engine_song_publish(NULL, 1) == -1 &&
                bb_engine_song_publish(clip, ARR_MAX_CLIPS + 1) == -1 &&
                bb_engine_song_publish(clip, -1) == -1,
                "song publish rejects bad arguments");

    bb_engine_render(aout, 8000, 1);               /* bar 0 */
    bb_engine_render(aout, 8000, 1);               /* bar 1 */
    test_expect(aout[4000] == 0 && aout[7999] == 0,
                "a clip is silent before its start bar");
    bb_engine_render(aout, 1000, 1);               /* bar 2 begins here */
    test_expect(aout[1] == data[1] && aout[100] == data[100] &&
                aout[255] == data[255],
                "a clip fires exactly at its start bar at unity gain");
    test_expect(aout[256] == 0 && aout[999] == 0,
                "a one-shot clip goes silent after its audio ends");

    /* ---- loop wraps the audio inside the window; gain scales --------- */
    clip[0].start_bar = 4;
    clip[0].loop = 1;
    clip[0].gain = 128;
    test_expect(bb_engine_song_publish(clip, 1) == 0, "the song republishes live");
    bb_engine_render(aout, 7000, 1);               /* finish bar 2       */
    bb_engine_render(aout, 8000, 1);               /* bar 3, still early */
    test_expect(aout[0] == 0 && aout[7999] == 0,
                "a moved clip window leaves the old bars silent");
    bb_engine_render(aout, 600, 1);                /* bar 4 begins here  */
    int loop_ok = 1;
    for (int j = 0; j < 600; j++)
        if (aout[j] != (int16_t)((data[j % N] * 128) >> 8)) { loop_ok = 0; break; }
    test_expect(loop_ok, "a looped clip wraps its audio at half gain");

    /* ---- seek restarts a window -------------------------------------- */
    bb_engine_song_seek(4);
    bb_engine_render(aout, 300, 1);                /* top of bar 4 again */
    test_expect(atomic_load(&bb.bar) == 4,
                "seek lands the published bar counter on its target");
    int seek_ok = 1;
    for (int j = 0; j < 300; j++)
        if (aout[j] != (int16_t)((data[j % N] * 128) >> 8)) { seek_ok = 0; break; }
    test_expect(seek_ok, "seek restarts a clip window from its first frame");

    /* ---- a NULL-audio clip is a silent ghost; song_get returns meta -- */
    clip[0].audio = NULL;
    test_expect(bb_engine_song_publish(clip, 1) == 0, "a ghost clip publishes");
    bb_engine_render(aout, 200, 1);
    test_expect(aout[0] == 0 && aout[199] == 0, "a NULL-audio clip is silent");
    int ng = bb_engine_song_get(got, ARR_MAX_CLIPS);
    test_expect(ng == 1 && got[0].lane == 3 && got[0].start_bar == 4 &&
                got[0].len_bars == 1 && got[0].loop == 1 &&
                got[0].gain == 128 && got[0].audio == NULL &&
                !strcmp(got[0].name, "hit"),
                "song_get returns the published clip meta");
    bb_engine_clip_release(ab);

    /* ---- per-lane capture -------------------------------------------- */
    bb_engine_set_defaults();
    bb_engine_init(rate);
    atomic_store(&bb.gctl[GCTL_BPM], 240);
    bb_engine_reset_loop();
    atomic_store(&bb.gain, 256);
    atomic_store(&bb.layer[0].on, 0);
    test_expect(bb_engine_song_publish(NULL, 0) == 0, "an empty song publishes");

    /* A deterministic voice on lane 3: with every other source silent the
     * master output IS that lane's post-fader contribution, so the capture
     * buffer must match the rendered output bit for bit. */
    ExprError er;
    test_expect(bb_publish(3, "t*p0", &er), "capture reference voice compiles: %s", er.msg);
    atomic_store(&bb.layer[3].on, 1);
    atomic_store(&bb.layer[3].mode, BB_WORD);
    atomic_store(&bb.layer[3].ctl[LCTL_LEVEL], 256);
    atomic_store(&bb.layer[3].param[0], 37);

    bb_engine_render(ref, 12000, 1);               /* levels + gain settle */

    test_expect(bb_engine_arr_arm(9, 1, dst, 16000) == -1,
                "lane 9 (FILE/MASS) capture is refused");
    test_expect(bb_engine_arr_arm(3, 0, dst, 16000) == -1 &&
                bb_engine_arr_arm(3, 1, NULL, 16000) == -1 &&
                bb_engine_arr_arm(3, 1, dst, 0) == -1,
                "arm validates its arguments");
    test_expect(bb_engine_arr_arm(3, 2, dst, 16050) == 0 &&
                atomic_load(&bb.arr_rec_status) == ARR_REC_ARMED,
                "a voice lane arms and publishes ARMED");
    test_expect(bb_engine_arr_arm(3, 1, dst, 100) == -1,
                "a second arm is refused while a capture is in flight");
    bb_engine_arr_cancel();
    test_expect(atomic_load(&bb.arr_rec_status) == ARR_REC_IDLE,
                "cancel returns an armed capture to IDLE");
    test_expect(bb_engine_arr_arm(3, 2, dst, 16050) == 0,
                "a canceled capture can re-arm");

    bb_engine_render(aout, 4000, 1);               /* rest of bar 1      */
    test_expect(atomic_load(&bb.arr_rec_status) == ARR_REC_ARMED &&
                atomic_load(&bb.arr_rec_frames) == 0,
                "an armed capture waits for the bar boundary");
    bb_engine_render(ref, 16000, 1);               /* bars 2-3, captured */
    test_expect(atomic_load(&bb.arr_rec_status) == ARR_REC_RECORDING &&
                atomic_load(&bb.arr_rec_frames) == 16000,
                "capture starts at the bar boundary and reports progress");
    bb_engine_render(aout, 100, 1);                /* bar 4: closes it   */
    test_expect(atomic_load(&bb.arr_rec_status) == ARR_REC_DONE &&
                atomic_load(&bb.arr_rec_frames) == 16000,
                "capture publishes DONE with the whole-bar frame count");
    int cap_mismatch = 0, cap_energy = 0;
    for (int j = 0; j < 16000; j++) {
        if (dst[j] != ref[j]) cap_mismatch++;
        if (ref[j] != 0) cap_energy++;
    }
    test_expect(cap_mismatch == 0 && cap_energy > 1000,
                "captured frames equal the armed lane's contribution");

    test_expect(bb_engine_arr_arm(3, 4, dst, 500) == 0,
                "a fresh capture arms after DONE");
    bb_engine_render(aout, 8000, 1);               /* crosses into bar 5 */
    bb_engine_render(aout, 500, 1);
    test_expect(atomic_load(&bb.arr_rec_status) == ARR_REC_DONE &&
                atomic_load(&bb.arr_rec_frames) == 500,
                "capture stops when the destination runs out");
    atomic_store(&bb.layer[3].on, 0);

    /* ---- a publish mid-playback never hands a clip another clip's
     * window counter: deleting clip 0 while clip 1 sounds must leave the
     * survivor exactly where it was, not restarted and not teleported to
     * the deleted clip's offset. The ghost occupies index 0 with an older,
     * longer window, so inheriting its counter would silence the survivor
     * (offset far past its audio) -- the seamless-continue assertions
     * below fail on counters that are not re-keyed from the timeline. */
    {
        static int16_t big[12000];
        for (int i = 0; i < 12000; i++) big[i] = (int16_t)(1000 + i % 3000);
        ArrClipBuf *bb2 = bb_engine_clip_create(big, 12000, rate);
        test_expect(bb2 != NULL, "the survivor clip buffer publishes");

        bb_engine_set_defaults();
        bb_engine_init(rate);
        atomic_store(&bb.gctl[GCTL_BPM], 240);
        bb_engine_reset_loop();
        atomic_store(&bb.gain, 256);
        atomic_store(&bb.layer[0].on, 0);

        memset(clip, 0, sizeof clip);
        clip[0].lane = 0;                       /* silent ghost, bars 0-3 */
        clip[0].start_bar = 0;
        clip[0].len_bars = 4;
        clip[0].gain = 256;
        clip[0].audio = NULL;
        clip[1].lane = 1;                       /* the survivor, bars 1-2 */
        clip[1].start_bar = 1;
        clip[1].len_bars = 2;
        clip[1].gain = 256;
        clip[1].audio = bb2;
        test_expect(bb_engine_song_publish(clip, 2) == 0,
                    "the two-clip song publishes");

        bb_engine_render(aout, 8000, 1);        /* bar 0: ghost only     */
        bb_engine_render(aout, 8000, 1);        /* bar 1: survivor 0..7999 */
        bb_engine_render(aout, 500, 1);         /* into bar 2: 8000..8499 */
        test_expect(aout[499] == big[8499],
                    "the survivor sounds at its own offset before the edit");

        test_expect(bb_engine_song_publish(&clip[1], 1) == 0,
                    "deleting the ghost republishes");
        bb_engine_render(aout, 100, 1);
        test_expect(aout[0] == big[8500] && aout[99] == big[8599],
                    "a mid-playback edit keeps the surviving clip seamless");

        bb_engine_song_publish(NULL, 0);
        bb_engine_clip_release(bb2);
    }

    

/* ---- session v7: song meta round-trips, older sessions still load */
    char root[720];
    int made = tmp_dir_make(root, sizeof root, "arrtest") == 0;
    test_expect(made, "temporary arrangement session directory can be created");
    if (!made) return;
    bb_config_set_root(root);

    bb_engine_set_defaults();
    ArrClipBuf *ab2 = bb_engine_clip_create(data, 64, rate);
    memset(clip, 0, sizeof clip);
    clip[0].lane = 8;
    clip[0].start_bar = 12;
    clip[0].len_bars = 4;
    clip[0].loop = 1;
    clip[0].gain = 192;
    clip[0].audio = ab2;
    snprintf(clip[0].name, ARR_NAME_MAX, "cold hall II");
    snprintf(clip[0].path, ARR_PATH_MAX, "/tmp/morgue kits/cold hall.wav");
    clip[1].lane = 0;
    clip[1].start_bar = 0;
    clip[1].len_bars = 1;
    clip[1].loop = 0;
    clip[1].gain = 256;
    clip[1].audio = NULL;
    clip[1].name[0] = '\0';
    snprintf(clip[1].path, ARR_PATH_MAX, "/tmp/x.wav");
    test_expect(bb_engine_song_publish(clip, 2) == 0, "the session-test song publishes");
    test_expect(bb_config_save() == 0, "a version 7 session with a song can be saved");

    FILE *f = bb_fopen(bb_config_path(), "r");
    int aclip_lines = 0;
    if (f) {
        char line[1200];
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "aclip ", 6)) aclip_lines++;
        fclose(f);
    }
    test_expect(aclip_lines == 2, "the session carries one aclip line per clip");

    bb_engine_song_publish(NULL, 0);
    bb_engine_clip_release(ab2);
    bb_engine_set_defaults();
    test_expect(bb_config_load() == 1, "the version 7 session loads");
    ng = bb_engine_song_get(got, ARR_MAX_CLIPS);
    test_expect(ng == 2, "both clips return from the loaded session");
    test_expect(ng == 2 && got[0].lane == 8 && got[0].start_bar == 12 &&
                got[0].len_bars == 4 && got[0].loop == 1 &&
                got[0].gain == 192 && got[0].audio == NULL &&
                !strcmp(got[0].name, "cold hall II") &&
                !strcmp(got[0].path, "/tmp/morgue kits/cold hall.wav"),
                "clip meta with spaces in name and path survives the round-trip");
    test_expect(ng == 2 && got[1].lane == 0 && got[1].start_bar == 0 &&
                got[1].len_bars == 1 && got[1].loop == 0 &&
                got[1].gain == 256 && got[1].audio == NULL &&
                got[1].name[0] == '\0' && !strcmp(got[1].path, "/tmp/x.wav"),
                "an empty clip name survives the round-trip");

    /* A v6 session (no aclip lines) must still load, and must leave the
     * song empty -- the file is the whole truth about the timeline. Binary
     * mode for the same reason as the v3 fixture above: the bytes on disk
     * must be the same bytes on every platform. */
    f = bb_fopen(bb_config_path(), "wb");
    if (f) {
        fputs("version 6\n"
              "rate 22050\n"
              "verb 11 22 33 44\n", f);
        fclose(f);
    }
    test_expect(f != NULL, "v6 session fixture can be written");
    bb_engine_set_defaults();
    test_expect(bb_config_load() == 1, "a version 6 session remains loadable");
    test_expect(atomic_load(&bb.req_rate) == 22050 &&
                atomic_load(&bb.verb_size) == 11 &&
                atomic_load(&bb.smp_send) == 44,
                "v6 fields load unchanged next to the v7 song machinery");
    test_expect(bb_engine_song_get(got, ARR_MAX_CLIPS) == 0,
                "a pre-v7 session loads with an empty song");

    bb_remove(bb_config_path());
    {
        char sub[600];
        snprintf(sub, sizeof sub, "%s/bytebeat", root);
        tmp_dir_remove(sub);
    }
    tmp_dir_remove(root);

    /* leave nothing armed or published for whoever runs after us */
    bb_engine_arr_cancel();
    bb_engine_song_publish(NULL, 0);
    bb_engine_render(aout, 64, 1);
    bb_engine_render(aout, 64, 1);
    bb_engine_reclaim();
}

/* ======================================================================== */
/*  New: the checks the Windows port made necessary                          */
/* ======================================================================== */

/* ------------------------------------------------------------------------
 * bb.pos -- bar and step in one word.
 *
 * Reading bb.bar and bb.seq_pos as two loads is a torn read: the audio thread
 * publishes them as separate stores, so a caller can catch a bar from one side
 * of a boundary and a step from the other. It is a real defect that was
 * reported from playing the instrument -- the ARRANGE playhead appeared to
 * jump back a bar on every loop pass -- not a theoretical one.
 *
 * These checks pin the encoding and, more importantly, pin AGREEMENT: whatever
 * the packed word says must be what the two separate fields say, at every
 * point the engine publishes them.
 * ------------------------------------------------------------------------ */
static void test_packed_position(void)
{
    static int16_t out[2048];
    const int rate = 8000;

    bb_engine_set_defaults();
    test_expect((int)(unsigned)(atomic_load(&bb.pos) & 0xffffffffu) == -1,
                "defaults leave the packed position idle");

    bb_engine_init(rate);
    atomic_store(&bb.gctl[GCTL_BPM], 240);
    bb_engine_reset_loop();
    bb_engine_reset_t();
    atomic_store(&bb.layer[0].on, 1);
    atomic_store(&bb.layer[0].seq_on, 1);

    /* Walk a few bars, checking after every period that the packed word and
     * the two fields it mirrors have not drifted apart. 8000 frames at 240 BPM
     * is one bar, so 37 frames per period crosses plenty of boundaries at an
     * offset that does not divide evenly into anything. */
    int agree = 1, saw_step = 0, saw_bar = 0;
    unsigned last_bar = 0;
    for (int i = 0; i < 700 && agree; i++) {
        bb_engine_render(out, 37, 1);

        const unsigned long long pk = atomic_load(&bb.pos);
        const unsigned bar = (unsigned)(pk >> 32);
        const int      seq = (int)(unsigned)(pk & 0xffffffffu);

        if (bar != (unsigned)atomic_load(&bb.bar))  agree = 0;
        if (seq != atomic_load(&bb.seq_pos))        agree = 0;

        if (seq >= 0) saw_step = 1;
        if (bar != last_bar) { saw_bar = 1; last_bar = bar; }
    }
    test_expect(agree,  "the packed position always agrees with bar and seq_pos");
    test_expect(saw_step, "the packed position reported a live step");
    test_expect(saw_bar,  "the packed position crossed a bar boundary");

    /* The idle encoding must survive the round trip: -1 is not a step number,
     * and packing it through an unsigned must not turn it into 4294967295. */
    atomic_store(&bb.layer[0].on, 0);
    atomic_store(&bb.layer[0].seq_on, 0);
    for (int i = 0; i < 8; i++) bb_engine_render(out, 512, 1);
    const int idle_seq = (int)(unsigned)(atomic_load(&bb.pos) & 0xffffffffu);
    test_expect(idle_seq == atomic_load(&bb.seq_pos),
                "an idle step clock round-trips through the packed word");
}

/* Everything above this point is the suite as it was. Everything below is
 * new, and exists because the port moved two assumptions that had never been
 * questioned: that a path is a byte string the C library will hand straight
 * to the kernel, and that a text line ends with exactly one byte.
 *
 * Neither is true on Windows. The CRT's narrow fopen() interprets its path in
 * the active ANSI codepage, so a perfectly valid UTF-8 path either fails to
 * open or opens the WRONG FILE -- silently, and only for users whose name or
 * folders are not ASCII, which is the worst possible failure distribution.
 * And a text-mode stream writes CRLF, so a session saved on Windows and
 * carried to Linux has a carriage return sitting at the end of every line,
 * where it lands inside the last field the loader parses: the expression
 * text, the clip name, the clip path. A trailing '\r' in an expression is a
 * parse error; in a WAV path it is a file that does not exist.
 *
 * Both of those are silent corruptions of somebody's saved work, so both get
 * a test that fails loudly instead. */
static void test_port_paths(void)
{
    static ArrClip got[ARR_MAX_CLIPS];
    ArrClip clip;
    char root[720];
    int ng;

    /* ---- a session directory whose name carries a space AND a non-ASCII
     * character. The space catches anything that treats a path as
     * whitespace-delimited; the non-ASCII byte catches anything that
     * round-trips the path through a narrow codepage. Both together are what
     * a real Windows user's Documents folder looks like. ------------------ */
    int made = tmp_dir_make(root, sizeof root, PORT_DIR_TAG) == 0;
    test_expect(made,
                "a session directory whose name has a space and a non-ASCII "
                "character can be created");
    if (!made) return;

    bb_config_set_root(root);
    set_defaults();
    memset(bb_expr, 0, sizeof bb_expr);
    atomic_store(&bb.req_rate, 32000);
    atomic_store(&bb.gain, 77);
    snprintf(bb_expr[1], BB_EXPR_MAX, "t*p0&t>>3");
    bb_custom[1] = 1;

    /* A clip whose NAME and PATH both carry a space and a non-ASCII
     * character. Those are the two fields the session writer treats as free
     * text -- the name is length-prefixed in BYTES and the path runs to end
     * of line -- so they are exactly where a multi-byte character gets cut in
     * half or a space gets mistaken for a field separator. */
    memset(&clip, 0, sizeof clip);
    clip.lane      = 8;
    clip.start_bar = 3;
    clip.len_bars  = 2;
    clip.loop      = 1;
    clip.gain      = 200;
    clip.audio     = NULL;
    snprintf(clip.name, ARR_NAME_MAX, "%s", PORT_CLIP_NAME);
    snprintf(clip.path, ARR_PATH_MAX, "%s/%s/cold hall.wav", root, PORT_KIT_DIR);
    test_expect(bb_engine_song_publish(&clip, 1) == 0,
                "a song with a non-ASCII clip name and path publishes");
    test_expect(bb_config_save() == 0,
                "a session saves into a directory with a space and a "
                "non-ASCII character in its name");

    FILE *f = bb_fopen(bb_config_path(), "r");
    int saw_marker = 0, saw_aclip = 0;
    if (f) {
        char line[1400];
        while (fgets(line, sizeof line, f)) {
            if (line_is(line, "version 7")) saw_marker = 1;
            if (!strncmp(line, "aclip ", 6)) saw_aclip++;
        }
        fclose(f);
    }
    test_expect(f != NULL,
                "the session written under a non-ASCII path reopens by that "
                "same path");
    test_expect(saw_marker,
                "a session saved under a non-ASCII path carries the version 7 "
                "marker");
    test_expect(saw_aclip == 1,
                "a session saved under a non-ASCII path carries its aclip line");

    bb_engine_song_publish(NULL, 0);
    set_defaults();
    memset(bb_expr, 0, sizeof bb_expr);
    test_expect(bb_config_load() == 1,
                "a session under a non-ASCII path loads back");
    test_expect(atomic_load(&bb.req_rate) == 32000 &&
                atomic_load(&bb.gain) == 77,
                "master state survives a round-trip through a non-ASCII path");
    test_expect(!strcmp(bb_expr[1], "t*p0&t>>3"),
                "an expression survives a round-trip through a non-ASCII path");
    ng = bb_engine_song_get(got, ARR_MAX_CLIPS);
    test_expect(ng == 1, "the clip returns from the non-ASCII session");
    test_expect(ng == 1 && !strcmp(got[0].name, PORT_CLIP_NAME),
                "a non-ASCII clip name survives byte for byte");
    test_expect(ng == 1 && strstr(got[0].path, PORT_KIT_DIR) != NULL &&
                strstr(got[0].path, "cold hall.wav") != NULL,
                "a clip path with a space and a non-ASCII character survives "
                "byte for byte");
    test_expect(ng == 1 && got[0].lane == 8 && got[0].start_bar == 3 &&
                got[0].len_bars == 2 && got[0].loop == 1 && got[0].gain == 200,
                "the non-ASCII clip's numeric meta survives with it");

    /* ---- a session file written with CRLF line endings ------------------
     * Written in BINARY mode with explicit "\r\n" so the bytes on disk are
     * CRLF no matter which platform runs the test -- a text-mode "\n" would
     * be LF on POSIX and would prove nothing.
     *
     * What this bites on differs by platform, and both halves are worth
     * having. On POSIX the loader sees the '\r' and must strip it, or the
     * expression, the clip name and the clip path all come back with a
     * carriage return glued to the end. On Windows the CRT's text-mode read
     * translates it away before the loader ever sees it, so the test instead
     * asserts that the Windows read path really is doing that. Between them
     * they cover the case that actually happens to people: a session saved
     * on one and opened on the other. */
    {
        FILE *w = bb_fopen(bb_config_path(), "wb");
        test_expect(w != NULL, "a CRLF session fixture can be written");
        if (w) {
            fputs("# bytebeat session -- plain text, edit it if you like\r\n"
                  "version 7\r\n"
                  "rate 22050\r\n"
                  "gain 123\r\n"
                  "focus 4\r\n"
                  "verb 11 22 33 44\r\n"
                  "layer 4 on 1 mode 2 seq 1\r\n"
                  "expr 4 t*p0\r\n"
                  "rack 4 0 1 0 1\r\n"
                  "aclip 2 5 3 1 128 8:cold air /tmp/cold air.wav\r\n", w);
            fclose(w);
        }

        bb_engine_song_publish(NULL, 0);
        set_defaults();
        memset(bb_expr, 0, sizeof bb_expr);
        test_expect(bb_config_load() == 1, "a CRLF session loads");
        test_expect(atomic_load(&bb.req_rate) == 22050 &&
                    atomic_load(&bb.gain) == 123 &&
                    atomic_load(&bb.focus) == 4 &&
                    atomic_load(&bb.verb_size) == 11 &&
                    atomic_load(&bb.smp_send) == 44,
                    "numeric session fields survive CRLF line endings");
        test_expect(!strcmp(bb_expr[4], "t*p0"),
                    "an expression read from a CRLF session carries no stray "
                    "carriage return");
        test_expect(bb_rack[4].body == 1 && bb_rack[4].space == 0 &&
                    bb_custom[4] == 1,
                    "rack identity survives CRLF line endings");
        ng = bb_engine_song_get(got, ARR_MAX_CLIPS);
        test_expect(ng == 1, "the clip in a CRLF session loads");
        test_expect(ng == 1 && !strcmp(got[0].name, "cold air"),
                    "a clip name read from a CRLF session is intact");
        test_expect(ng == 1 && !strcmp(got[0].path, "/tmp/cold air.wav"),
                    "a clip path read from a CRLF session carries no stray "
                    "carriage return");
    }

    bb_remove(bb_config_path());
    tmp_dir_remove(root);

    /* leave nothing published for whoever runs after us */
    bb_engine_song_publish(NULL, 0);
    set_defaults();
    memset(bb_expr, 0, sizeof bb_expr);
    bb_engine_reclaim();
}

/* ======================================================================== */
/*  Driver                                                                   */
/* ======================================================================== */


/* ------------------------------------------------------------------------
 * Arrangement transport and record source.
 *
 * Two things the timeline could not do: stop, and stay out of the recording.
 * The second is the one that bit -- looping an arranged section and playing
 * over it printed the backing into the take on every pass, so each new layer
 * arrived with the previous ones already baked into it.
 *
 * The guarantee pinned here is stronger than "quieter": a BB_REC_LIVE take
 * must be SAMPLE-IDENTICAL to the take you would have got by muting the
 * timeline and recording normally. That is why the engine removes the clip
 * sum before the 16-bit clamp rather than subtracting clip audio from the
 * finished output afterwards -- dsp_clip16 is not linear, so an after-the-fact
 * subtraction would drift exactly when the bus is hottest.
 *
 * Every comparison renders the same number of frames on both sides, because
 * bb.gain ramps in over roughly 2048 frames; comparing two takes that started
 * at different points in that ramp would fail for reasons that have nothing
 * to do with the arrangement.
 * ------------------------------------------------------------------------ */
static void test_arr_transport_and_rec_src(void)
{
    const int rate = 8000;
    const int N = 256;
    static int16_t aout[9000];
    static int16_t a[1024], b[1024];
    static int16_t data[256];
    ArrClip clip[1];
    ExprError er;

    for (int i = 0; i < N; i++) data[i] = (int16_t)(i * 100);
    ArrClipBuf *ab = bb_engine_clip_create(data, N, rate);
    test_expect(ab != NULL, "transport test publishes a clip buffer");
    if (!ab) return;

    /* One looping clip from bar 0. `voice` adds a deterministic voice
     * underneath so there is live material to record on its own -- a layer
     * only sounds once a program has been published to it. */
    #define SCENARIO(voice)                                                 \
        do {                                                                \
            bb_engine_set_defaults();                                       \
            bb_engine_init(rate);                                           \
            atomic_store(&bb.gctl[GCTL_BPM], 240);                          \
            bb_engine_reset_loop();                                         \
            /* bb.t is a free-running static that survives set_defaults and  \
             * init by design (the transport must outlive the host           \
             * recreating its IO thread). Without this the voice starts at a \
             * different point in the sample clock in every scenario, and    \
             * two takes that should be identical are not. */                \
            bb_engine_reset_t();                                            \
            atomic_store(&bb.gain, 256);                                    \
            for (int L = 0; L < BB_NLAYER; L++)                             \
                atomic_store(&bb.layer[L].on, 0);                           \
            if (voice) {                                                    \
                bb_publish(0, "t*p0", &er);                                 \
                atomic_store(&bb.layer[0].on, 1);                           \
                atomic_store(&bb.layer[0].mode, BB_WORD);                   \
                atomic_store(&bb.layer[0].ctl[LCTL_LEVEL], 256);            \
                atomic_store(&bb.layer[0].param[0], 37);                    \
            }                                                               \
            memset(clip, 0, sizeof clip);                                   \
            clip[0].lane = 0;                                               \
            clip[0].start_bar = 0;                                          \
            clip[0].len_bars = 4;                                           \
            clip[0].loop = 1;                                               \
            clip[0].gain = 256;                                             \
            clip[0].audio = ab;                                             \
            snprintf(clip[0].name, ARR_NAME_MAX, "bed");                    \
            bb_engine_song_publish(clip, 1);                                \
        } while (0)

    /* ---- defaults preserve the old behaviour ------------------------- */
    bb_engine_set_defaults();
    test_expect(bb_engine_song_playing() == 1,
                "the timeline plays by default");
    test_expect(bb_engine_rec_src_get() == BB_REC_MASTER,
                "REC captures the whole bus by default");

    /* ---- the transport actually silences the timeline ---------------- */
    SCENARIO(0);
    bb_engine_render(aout, 1024, 1);
    int sounded = 0;
    for (int j = 0; j < 1024; j++) if (aout[j] != 0) { sounded = 1; break; }
    test_expect(sounded, "an arranged clip sounds while the timeline plays");

    SCENARIO(0);
    bb_engine_song_play(0);
    bb_engine_render(aout, 1024, 1);
    int stopped_silent = 1;
    for (int j = 0; j < 1024; j++) if (aout[j] != 0) { stopped_silent = 0; break; }
    test_expect(stopped_silent, "a stopped timeline puts no clip audio on the bus");
    test_expect(bb_engine_song_playing() == 0, "song_playing reports the stop");
    bb_engine_song_play(1);
    test_expect(bb_engine_song_playing() == 1, "song_playing reports the start");

    /* ---- stopping is a MUTE, not a pause -----------------------------
     * Stop for 600 frames then start, versus playing throughout: the same
     * 512-frame window must contain the same clip audio, because the per-clip
     * counters keep tracking the bar grid while stopped. If STOP had paused
     * those counters, the resumed take would lag by exactly those 600 frames. */
    SCENARIO(0);
    bb_engine_song_play(0);
    bb_engine_render(aout, 600, 1);
    bb_engine_song_play(1);
    bb_engine_render(a, 512, 1);

    SCENARIO(0);
    bb_engine_render(aout, 600, 1);
    bb_engine_render(b, 512, 1);

    int grid_ok = 1;
    for (int j = 0; j < 512; j++) if (a[j] != b[j]) { grid_ok = 0; break; }
    test_expect(grid_ok, "PLAY resumes on the bar grid, not where STOP left off");

    /* ---- BB_REC_LIVE: heard, but not printed ------------------------- */
    bb_engine_rec_src(BB_REC_LIVE);
    test_expect(bb_engine_rec_src_get() == BB_REC_LIVE, "rec source round-trips");
    bb_engine_rec_src(BB_REC_MASTER);
    test_expect(bb_engine_rec_src_get() == BB_REC_MASTER, "rec source resets");

    /* The take: timeline playing, recording LIVE. */
    SCENARIO(1);
    bb_engine_rec_src(BB_REC_LIVE);
    /* bb.gain ramps toward its target at 32 per frame and gain_cur is a
     * static that survives set_defaults, so two takes that render the same
     * number of frames can still sample different points on that ramp
     * depending on what ran before them. Settle it first -- both takes
     * pre-roll identically, so the clip position stays aligned too. */
    bb_engine_render(aout, 4096, 1);
    unsigned w0 = atomic_load(&bb.sink_w);
    bb_engine_render(aout, 1024, 1);
    test_expect(atomic_load(&bb.sink_w) - w0 == 1024u,
                "the sink advances one frame per rendered frame");
    for (unsigned j = 0; j < 1024; j++) a[j] = bb.sink[(w0 + j) & BB_SINK_MASK];

    int arrangement_heard = 0;
    for (int j = 0; j < 1024; j++)
        if (aout[j] != a[j]) { arrangement_heard = 1; break; }
    test_expect(arrangement_heard,
                "BB_REC_LIVE still sends the arrangement to the speakers");

    /* The reference: identical scenario, timeline muted, recorded normally. */
    SCENARIO(1);
    bb_engine_rec_src(BB_REC_MASTER);
    bb_engine_song_play(0);
    bb_engine_render(aout, 4096, 1);          /* same settle as the take */
    unsigned r0 = atomic_load(&bb.sink_w);
    bb_engine_render(aout, 1024, 1);
    for (unsigned j = 0; j < 1024; j++) b[j] = bb.sink[(r0 + j) & BB_SINK_MASK];

    int nonzero = 0;
    for (int j = 0; j < 1024; j++) if (b[j] != 0) { nonzero = 1; break; }
    test_expect(nonzero, "the reference take is not just silence");

    int identical = 1;
    for (int j = 0; j < 1024; j++) if (a[j] != b[j]) { identical = 0; break; }
    test_expect(identical,
                "a BB_REC_LIVE take is sample-identical to muting the timeline");

    /* ---- with the timeline stopped the two sources agree ------------- */
    SCENARIO(1);
    bb_engine_song_play(0);
    bb_engine_rec_src(BB_REC_LIVE);
    bb_engine_render(aout, 4096, 1);          /* settle the gain ramp */
    unsigned s0 = atomic_load(&bb.sink_w);
    bb_engine_render(aout, 512, 1);
    for (unsigned j = 0; j < 512; j++) a[j] = bb.sink[(s0 + j) & BB_SINK_MASK];

    SCENARIO(1);
    bb_engine_song_play(0);
    bb_engine_rec_src(BB_REC_MASTER);
    bb_engine_render(aout, 4096, 1);          /* settle the gain ramp */
    unsigned s1 = atomic_load(&bb.sink_w);
    bb_engine_render(aout, 512, 1);
    int agree = 1;
    for (unsigned j = 0; j < 512; j++)
        if (bb.sink[(s1 + j) & BB_SINK_MASK] != a[j]) { agree = 0; break; }
    test_expect(agree, "the two record sources agree while the timeline is stopped");

    #undef SCENARIO
    bb_engine_rec_src(BB_REC_MASTER);
    bb_engine_song_play(1);
    bb_engine_song_publish(NULL, 0);
    bb_engine_reclaim();
    bb_engine_reclaim();
    bb_engine_clip_release(ab);
}

/* ======================================================================== */
/*  The RETURN BUS                                                           */
/* ======================================================================== */

/* One CHAMBER became eight return slots, a send matrix and a link graph. The
 * single hardest thing to prove about that change is the thing nobody can
 * hear until it is too late: that a session which uses only the CHAMBER still
 * renders THE SAME SAMPLES it rendered before the bus existed.
 *
 * Everything else in this file compares two renders of the current binary
 * against each other. That is the right tool for "these two paths agree", and
 * it is completely blind to a change that moves BOTH sides -- which is exactly
 * what a refactor of the mix bus does. So the first test below is different in
 * kind: it hashes 4096 rendered frames of a fixed CHAMBER session and compares
 * them against a 64-bit constant that was captured from the binary BEFORE the
 * return bus was written, and committed on its own.
 *
 * THAT CONSTANT IS NEVER TO BE UPDATED. If it changes, the arithmetic of the
 * mix bus changed, and the correct response is to bisect until you find the
 * commit that moved it -- not to paste in the new number. The five ways the
 * bus can break bit-exactness silently are all detectable here and nowhere
 * else: a create/destroy fade that starts at 0 instead of unity, a safety
 * stage armed on a return that has no feedback edge, two reads of the same
 * send atomic at different instants, a wet-bus limiter armed unconditionally,
 * and a snapshot that forgets to clamp a hand-edited session's values.
 *
 * The scenario is deliberately boring and deliberately explicit. Two voices
 * with fixed expressions, two different send amounts, the sampler send open
 * over an empty sampler, the sequencer off, the timeline empty, and the two
 * things that survive bb_engine_set_defaults() -- the free-running sample
 * clock and the master gain ramp -- pinned by hand. Every one of those is
 * load-bearing: leave any of them to whatever ran earlier in the suite and the
 * hash stops being a property of the engine and starts being a property of the
 * test order.
 *
 * THE TWO-STAGE SETTLE BELOW IS NOT DECORATION, AND IT WAS NOT MY FIRST
 * ATTEMPT. Two pieces of render state outlive both bb_engine_set_defaults()
 * and bb_engine_init(): the per-voice level ramp g_lvl[] and the master gain
 * ramp gain_cur. Both walk toward their target at 32 per frame, so they are
 * settled by rendering. Everything ELSE -- and in particular the chamber's
 * comb and allpass lines -- is cleared by bb_engine_init(), and it has to be,
 * because at 8 kHz this reverb's tail is far longer than any settle worth
 * rendering: the state left behind by the ramp-in transient is still audible
 * thousands of frames later. Measured: settling the ramps alone gave three
 * DIFFERENT hashes depending on whether the test ran first, second, or after
 * the whole suite. Rendering to settle the ramps, then wiping the DSP state
 * with a second bb_engine_init(), then filling the chamber from a known-empty
 * state, gives one hash from any starting point. If you reorder this you will
 * get a constant that describes the suite instead of the engine. */
/* Captured on b9d8fbc -- the last commit before the return bus -- with MSVC
 * 19.44 at both /O2 and /Od, from three different positions in the suite, all
 * four agreeing. The render path is pure integer (the only float in engine.c
 * is the specimen synthesizer, which never runs here), so this number is a
 * property of the arithmetic and not of the compiler. */
#define CHAMBER_GOLDEN 0xd3be940ff58259ddULL

static void test_chamber_golden(void)
{
    static int16_t out[4096];
    ExprError er;

    bb_engine_set_defaults();
    bb_engine_init(8000);
    bb_engine_song_publish(NULL, 0);
    bb_engine_rec_src(BB_REC_MASTER);
    bb_engine_song_play(1);

    for (int L = 0; L < BB_NLAYER; L++) {
        atomic_store(&bb.layer[L].on, 0);
        atomic_store(&bb.layer[L].send, 0);
    }
    test_expect(bb_publish(0, "t*p0", &er),
                "golden-hash voice 0 compiles: %s", er.msg);
    test_expect(bb_publish(1, "t*p0&t>>3", &er),
                "golden-hash voice 1 compiles: %s", er.msg);
    for (int L = 0; L < 2; L++) {
        atomic_store(&bb.layer[L].on, 1);
        atomic_store(&bb.layer[L].mode, BB_WORD);
        atomic_store(&bb.layer[L].seq_on, 0);
        atomic_store(&bb.layer[L].ctl[LCTL_LEVEL], L == 0 ? 256 : 200);
    }
    atomic_store(&bb.layer[0].param[0], 37);
    atomic_store(&bb.layer[1].param[0], 91);
    atomic_store(&bb.gain, 256);

    atomic_store(&bb.verb_size,  172);
    atomic_store(&bb.verb_tone,   96);
    atomic_store(&bb.verb_level, 132);
    atomic_store(&bb.layer[0].send, 200);
    atomic_store(&bb.layer[1].send,  90);
    atomic_store(&bb.smp_send,       64);

    /* Stage one: settle the two ramps. 4096 frames is twice the 2048 a full
     * 0 -> 256 sweep needs at 32 per frame, so both are pinned at their
     * targets no matter where the previous test left them. */
    bb_engine_render(out, 4096, 1);

    /* Stage two: wipe every piece of DSP state that ISN'T one of those two
     * ramps -- the chamber's lines, the per-voice post chains and DC
     * blockers, the expression contexts, the envelopes -- and restart the
     * sample clock and the bar grid. From here the render is a pure function
     * of the controls set above. */
    bb_engine_init(8000);
    bb_engine_reset_t();
    bb_engine_reset_loop();

    /* Stage three: fill the chamber from empty, then hash the window after
     * it, where the reverb is developed and the only thing still moving is
     * the instrument. */
    bb_engine_render(out, 4096, 1);
    unsigned w0 = atomic_load(&bb.sink_w);
    bb_engine_render(out, 4096, 1);

    uint64_t h = 0xcbf29ce484222325ULL;
    for (unsigned j = 0; j < 4096; j++) {
        h ^= (uint64_t)(uint16_t)bb.sink[(w0 + j) & BB_SINK_MASK];
        h *= 0x100000001b3ULL;
    }
    test_expect(h == CHAMBER_GOLDEN,
                "the CHAMBER renders bit-identically to the pre-return-bus "
                "engine (got 0x%016llx, want 0x%016llx)",
                (unsigned long long)h, (unsigned long long)CHAMBER_GOLDEN);

    /* A hash is only worth what its input is worth: if the scenario ever
     * stops making sound, the constant above would still match a buffer of
     * silence and this test would pass forever while proving nothing. */
    int nonzero = 0;
    for (unsigned j = 0; j < 4096; j++)
        if (bb.sink[(w0 + j) & BB_SINK_MASK] != 0) { nonzero++; }
    test_expect(nonzero > 3000,
                "the golden-hash scenario is real audio, not silence");
    test_expect(atomic_load(&bb.verb_peak) > 0,
                "the golden-hash scenario actually runs the chamber");
}

/* Everything from here down needs the return bus itself. It is compiled only
 * once bytebeat.h defines BB_NRET, so this file builds -- and the golden hash
 * above still runs -- both before the bus lands and after. That is not a
 * courtesy to the build: the golden constant HAS to be captured and committed
 * on a binary that does not have the bus yet, and a test file that could not
 * compile against that binary could not have captured it. */
#if defined(BB_NRET)

/* The contract fixes the per-return ceiling at 24576 (-2.5 dBFS) and the
 * summed-wet ceiling at 30000. Taken as literals rather than pulled out of
 * ret.h so that a change to either shows up here as a failed bound rather
 * than as a test that silently re-derives whatever the engine now does. */
#ifndef BB_RET_CEIL
#  define BB_RET_CEIL      24576
#endif
#ifndef BB_RET_BUS_CEIL
#  define BB_RET_BUS_CEIL  30000
#endif

/* One switch per shipping effect type, so a type that slips to a later commit
 * costs one line here instead of a red suite. The contract ships four. */
#define RET_HAVE_CHAMBER 1
#define RET_HAVE_DELAY   1
#define RET_HAVE_DRIVE   1
#define RET_HAVE_CHOIR   1

enum { RET_SETTLE = 4096 };          /* frames: gain and level ramps at 32 */

static int16_t ret_out[8192];

static void ret_render(int frames)
{
    while (frames > 0) {
        int n = frames > (int)(sizeof ret_out / sizeof ret_out[0])
              ? (int)(sizeof ret_out / sizeof ret_out[0]) : frames;
        bb_engine_render(ret_out, n, 1);
        frames -= n;
    }
}

/* Capture the RECORD sink rather than the render buffer, for the same reason
 * test_arr_transport_and_rec_src does: the sink is what the instrument
 * committed to, and reading it keeps the comparison independent of how the
 * frames were chunked into render calls. */
static void ret_capture(int16_t *dst, int n)
{
    unsigned w0 = atomic_load(&bb.sink_w);
    ret_render(n);
    for (int j = 0; j < n; j++)
        dst[j] = bb.sink[(w0 + (unsigned)j) & BB_SINK_MASK];
}

static int ret_pending_any(void)
{
    for (int r = 0; r < BB_NRET; r++)
        if (bb_engine_ret_pending(r)) return 1;
    return 0;
}

/* Create and destroy are asynchronous: the render thread fades the slot out,
 * reports itself quiet, and two render epochs later the UI thread swaps the
 * type in. In this process there is no render thread, so the suite has to be
 * both threads -- render, then service, until nothing is pending. Returns 0
 * if it never landed, which is a deadlock in the handshake and is asserted on
 * once, in test_ret_lifecycle, rather than at every call site. */
static int ret_finish(void)
{
    /* The handshake needs one period to fade, one to report quiet and two
     * more epochs to prove nobody is inside the old type -- call it five.
     * The budget here is 256 periods, which is 65536 frames, eight seconds at
     * 8 kHz. That is enormous compared to what a working handshake needs and
     * small enough that a handshake which never lands fails in a moment
     * instead of grinding through half a million frames per slot. */
    for (int i = 0; i < 256; i++) {
        if (!ret_pending_any()) return 1;
        bb_engine_render(ret_out, 256, 1);
        bb_engine_ret_service();
    }
    return !ret_pending_any();
}

static int ret_set_type(int slot, int type)
{
    if (type == RET_NONE) bb_engine_ret_destroy(slot);
    else                  bb_engine_ret_create(slot, type);
    return ret_finish();
}

/* Read and clear a return's max-hold meter, which is the only view the suite
 * gets of a return's own output -- everything downstream of it is summed with
 * the dry bus. Clear before a window, read after it. */
static int ret_peak(int r)
{
    return atomic_exchange(&bb.ret[r].peak, 0);
}

/* A known-empty engine with a known-empty graph. Deliberately does not trust
 * bb_engine_set_defaults() to have cleared the matrix -- that is asserted
 * exactly once, in test_ret_defaults, and everywhere else the scenario is
 * made explicit so a failure there cannot cascade into every other test. */
static void ret_scene(int rate)
{
    bb_engine_set_defaults();
    bb_engine_init(rate);
    bb_engine_song_publish(NULL, 0);
    bb_engine_rec_src(BB_REC_MASTER);
    bb_engine_song_play(1);
    atomic_store(&bb.gain, 256);
    atomic_store(&bb.panic, 0);
    atomic_store(&bb.mute, 0);
    for (int L = 0; L < BB_NLAYER; L++) atomic_store(&bb.layer[L].on, 0);

    for (int r = 0; r < BB_NRET; r++) {
        bb_engine_ret_mute(r, 0);
        bb_engine_ret_sync(r, 0);
        bb_engine_ret_level(r, 0);
        for (int s = 0; s < BB_RET_NSRC; s++) bb_engine_ret_send(s, r, 0);
        for (int q = 0; q < BB_NRET; q++)     bb_engine_ret_link(q, r, 0);
        bb_ret_name[r][0] = '\0';
        atomic_exchange(&bb.ret[r].peak, 0);
    }
    atomic_store(&bb.verb_peak, 0);

    bb_engine_reset_t();
    bb_engine_reset_loop();
}

static void ret_voice(int L, const char *src, int p0, int level)
{
    ExprError er;
    bb_publish(L, src, &er);
    atomic_store(&bb.layer[L].on, 1);
    atomic_store(&bb.layer[L].mode, BB_WORD);
    atomic_store(&bb.layer[L].seq_on, 0);
    atomic_store(&bb.layer[L].ctl[LCTL_LEVEL], level);
    atomic_store(&bb.layer[L].param[0], p0);
}

/* ---- slot 0 is the CHAMBER, and its storage IS the legacy atomics -------
 *
 * bb.ret[0].level and bb.ret[0].param[0..1] are documented as UNUSED: slot
 * 0's level, SIZE and DARK live in bb.verb_level / verb_size / verb_tone, and
 * send column 0 lives in bb.layer[s].send / bb.smp_send. That alias is the
 * whole reason the mixer panel, the `verb` session key and the chamber checks
 * further up this file needed no edits -- and it is exactly the kind of thing
 * a later "cleanup" adds a shadow copy to. The moment there are two storages
 * they drift, and the drift shows up as a session that loads the wrong reverb
 * or as a golden hash that fails for a reason that looks like DSP. So: pin it
 * in both directions, through the accessors and through the raw atomics. */
static void test_ret_defaults(void)
{
    bb_engine_set_defaults();
    bb_engine_init(8000);

    test_expect(bb_engine_ret_type_get(0) == RET_CHAMBER,
                "slot 0 comes up as the CHAMBER");
    int empty = 0;
    for (int r = 1; r < BB_NRET; r++)
        empty += bb_engine_ret_type_get(r) == RET_NONE;
    test_expect(empty == BB_NRET - 1,
                "slots 1..7 come up empty");

    int sends = 0, links = 0;
    for (int s = 0; s < BB_RET_NSRC; s++)
        for (int r = 0; r < BB_NRET; r++)
            sends += bb_engine_ret_send_get(s, r) != 0;
    for (int a = 0; a < BB_NRET; a++)
        for (int b2 = 0; b2 < BB_NRET; b2++)
            links += bb_engine_ret_link_get(a, b2) != 0;
    test_expect(sends == 0 && links == 0,
                "a defaulted session has an empty routing graph");

    /* reads go through the alias */
    atomic_store(&bb.verb_level, 111);
    atomic_store(&bb.verb_size,  222);
    atomic_store(&bb.verb_tone,   33);
    atomic_store(&bb.layer[4].send, 44);
    atomic_store(&bb.smp_send,      55);
    test_expect(bb_engine_ret_level_get(0) == 111 &&
                bb_engine_ret_param_get(0, 0) == 222 &&
                bb_engine_ret_param_get(0, 1) == 33,
                "slot 0's level and first two params read the legacy atomics");
    test_expect(bb_engine_ret_send_get(BB_RET_SRC_V0 + 4, 0) == 44 &&
                bb_engine_ret_send_get(BB_RET_SRC_LICKS, 0) == 55,
                "send column 0 reads Layer.send and smp_send");

    /* writes go through it too */
    bb_engine_ret_level(0, 88);
    bb_engine_ret_param(0, 0, 77);
    bb_engine_ret_param(0, 1, 66);
    bb_engine_ret_send(BB_RET_SRC_V0 + 4, 0, 22);
    bb_engine_ret_send(BB_RET_SRC_LICKS, 0, 11);
    test_expect(atomic_load(&bb.verb_level) == 88 &&
                atomic_load(&bb.verb_size)  == 77 &&
                atomic_load(&bb.verb_tone)  == 66,
                "writing slot 0 writes the legacy atomics, not a shadow copy");
    test_expect(atomic_load(&bb.layer[4].send) == 22 &&
                atomic_load(&bb.smp_send) == 11,
                "writing send column 0 writes Layer.send and smp_send");

    /* and the aliased cells of the new arrays stay out of it entirely */
    test_expect(atomic_load(&bb.ret_send[BB_RET_SRC_V0 + 4][0]) == 0 &&
                atomic_load(&bb.ret_send[BB_RET_SRC_LICKS][0]) == 0,
                "the aliased send cells are left unused");

    bb_engine_set_defaults();
}

/* ---- create, destroy, and the promise that neither leaves a residue ----- */
static void test_ret_lifecycle(void)
{
    const int rate = 8000;
    ret_scene(rate);

    test_expect(bb_engine_ret_create(-1, RET_DELAY) == -1 &&
                bb_engine_ret_create(BB_NRET, RET_DELAY) == -1 &&
                bb_engine_ret_destroy(-1) == -1 &&
                bb_engine_ret_destroy(BB_NRET) == -1,
                "return lifecycle calls reject a bad slot");

    test_expect(bb_engine_ret_create(1, RET_DELAY) == 0,
                "a return can be created at runtime");
    test_expect(ret_finish(), "the create handshake completes");
    test_expect(bb_engine_ret_type_get(1) == RET_DELAY,
                "the created slot reports its type");

    bb_engine_ret_level(1, 256);
    bb_engine_ret_send(BB_RET_SRC_V0, 1, 255);
    ret_voice(0, "t*p0", 37, 256);
    ret_render(RET_SETTLE);
    ret_peak(1);
    ret_render(2048);
    test_expect(ret_peak(1) > 0, "a created return is audible");

    /* Destroy while it is ringing, then put a different effect in the same
     * slot: the first sample out of the new type must be silence. A stale
     * tail here means the arena was not cleared, which is the one failure a
     * "does it make sound" test would happily walk past. */
    test_expect(bb_engine_ret_destroy(1) == 0, "a return can be destroyed");
    test_expect(ret_finish(), "the destroy handshake completes");
    test_expect(bb_engine_ret_type_get(1) == RET_NONE,
                "a destroyed slot reports itself empty");

    atomic_store(&bb.layer[0].on, 0);
    bb_engine_ret_send(BB_RET_SRC_V0, 1, 0);
    test_expect(ret_set_type(1, RET_CHAMBER), "the slot takes a new type");
    bb_engine_ret_level(1, 256);
    ret_peak(1);
    ret_render(512);
    test_expect(ret_peak(1) == 0,
                "a re-created slot starts from silence, with no stale tail");

    ret_scene(rate);
}

/* ---- the bit-exact bypass, as an audio property ------------------------- */
static void test_ret_bypass(void)
{
    const int rate = 8000;
    static int16_t a[2048], b[2048];

    /* (1) Eight slots created and destroyed, then all empty, must render
     * bit-identically to a session that never touched them. This is the
     * contract's "disabled costs nothing" stated as sound rather than as
     * cycles: an empty slot is not visited, so it cannot contribute a sample
     * and it cannot advance a state. */
    ret_scene(rate);
    ret_voice(0, "t*p0", 37, 256);
    bb_engine_ret_level(0, 132);
    bb_engine_ret_send(BB_RET_SRC_V0, 0, 200);
    bb_engine_reset_t();
    bb_engine_reset_loop();
    ret_render(RET_SETTLE);
    ret_capture(a, 2048);

    ret_scene(rate);
    for (int r = 1; r < BB_NRET; r++) {
        ret_set_type(r, r & 1 ? RET_DELAY : RET_CHOIR);
        bb_engine_ret_level(r, 256);
    }
    ret_render(2048);                       /* let them all run for a while */
    for (int r = 1; r < BB_NRET; r++) {
        bb_engine_ret_level(r, 0);
        ret_set_type(r, RET_NONE);
    }
    ret_voice(0, "t*p0", 37, 256);
    bb_engine_ret_level(0, 132);
    bb_engine_ret_send(BB_RET_SRC_V0, 0, 200);
    bb_engine_reset_t();                    /* the churn advanced the clock */
    bb_engine_reset_loop();
    ret_render(RET_SETTLE);
    ret_capture(b, 2048);

    int churn_diff = 0;
    for (int j = 0; j < 2048; j++) if (a[j] != b[j]) churn_diff++;
    test_expect(churn_diff == 0,
                "creating and destroying every return leaves the mix bus "
                "bit-identical");

    /* (2) A closed return is not merely silent, it is FROZEN. Blast the sends
     * at a chamber whose level is 0, then open it: at 8 kHz the shortest comb
     * is 202 frames long, so if the state really did not advance, the first
     * 100 frames after opening are EXACTLY zero. If the closed return had
     * been running under the level multiply, the tail would arrive already at
     * full amplitude the instant the fader moved. */
    ret_scene(rate);
    ret_voice(0, "t*p0", 37, 256);
    bb_engine_ret_send(BB_RET_SRC_V0, 0, 255);
    bb_engine_ret_level(0, 0);
    ret_render(RET_SETTLE + 8192);
    ret_peak(0);
    atomic_store(&bb.verb_peak, 0);
    test_expect(ret_peak(0) == 0 && atomic_load(&bb.verb_peak) == 0,
                "a closed return meters nothing while it is closed");

    bb_engine_ret_level(0, 256);
    ret_render(100);
    test_expect(ret_peak(0) == 0,
                "a closed return did not advance its state while closed");
    ret_render(4096);
    test_expect(ret_peak(0) > 0, "the same return sounds once it is opened");

    /* (3) With the return closed, the sends are inaudible whatever they are
     * set to -- the bypass is the whole send path, not just the return
     * fader. */
    ret_scene(rate);
    ret_voice(0, "t*p0", 37, 256);
    bb_engine_ret_level(0, 0);
    for (int s = 0; s < BB_RET_NSRC; s++) bb_engine_ret_send(s, 0, 255);
    bb_engine_reset_t();
    bb_engine_reset_loop();
    ret_render(RET_SETTLE);
    ret_capture(a, 2048);

    ret_scene(rate);
    ret_voice(0, "t*p0", 37, 256);
    bb_engine_ret_level(0, 0);
    bb_engine_reset_t();
    bb_engine_reset_loop();
    ret_render(RET_SETTLE);
    ret_capture(b, 2048);

    int send_diff = 0;
    for (int j = 0; j < 2048; j++) if (a[j] != b[j]) send_diff++;
    test_expect(send_diff == 0,
                "sends into a closed return contribute nothing at all");

    ret_scene(rate);
}

/* ---- one section per effect type ---------------------------------------
 *
 * Three questions per type, and they are the three that catch everything
 * cheap: does it stay quiet when nothing is going in (uninitialised state, a
 * DC offset, self-oscillation from nothing), does it do SOMETHING when
 * something is (a type that is inaudible at its own defaults is a bug, not a
 * preference), and does it do the same thing twice (an uninitialised read or
 * a dependence on whatever ran before it). */
static const struct { int type; const char *name; int rings; } RET_KIND[] = {
#if RET_HAVE_CHAMBER
    { RET_CHAMBER, "CHAMBER", 1 },
#endif
#if RET_HAVE_DELAY
    { RET_DELAY,   "DELAY",   1 },
#endif
#if RET_HAVE_DRIVE
    { RET_DRIVE,   "DRIVE",   0 },
#endif
#if RET_HAVE_CHOIR
    { RET_CHOIR,   "CHOIR",   1 },
#endif
};

static void test_ret_types(void)
{
    const int rate = 8000;
    const int NKIND = (int)(sizeof RET_KIND / sizeof RET_KIND[0]);
    static int16_t a[1024], b[1024];

    test_expect(NKIND == 4, "all four shipping return types are present");

    for (int i = 0; i < NKIND; i++) {
        int type = RET_KIND[i].type;
        const char *nm = RET_KIND[i].name;

        /* --- silence in, silence out ---------------------------------- */
        ret_scene(rate);
        test_expect(ret_set_type(1, type), "%s can be created", nm);
        bb_engine_ret_level(1, 256);
        ret_render(2048);                 /* settle any DC the type makes */
        ret_peak(1);
        ret_render(2048);
        test_expect(ret_peak(1) == 0, "%s is silent on silence", nm);

        /* --- something in, something out ------------------------------- */
        ret_voice(0, "t*p0", 37, 256);
        bb_engine_ret_send(BB_RET_SRC_V0, 1, 255);
        ret_render(RET_SETTLE);
        ret_peak(1);
        ret_render(2048);
        test_expect(ret_peak(1) > 0, "%s responds to its send", nm);

        /* --- and it behaves like the kind of effect it says it is ------ */
        atomic_store(&bb.layer[0].on, 0);
        bb_engine_ret_send(BB_RET_SRC_V0, 1, 0);
        ret_render(64);                   /* one period for the snapshot   */
        ret_peak(1);
        ret_render(512);
        int tail = ret_peak(1);
        if (RET_KIND[i].rings)
            test_expect(tail > 0, "%s rings after its input stops", nm);
        else
            test_expect(tail == 0,
                        "%s stops when its input stops (it has no tail)", nm);

        /* --- deterministic across identical runs ----------------------- */
        for (int pass = 0; pass < 2; pass++) {
            ret_scene(rate);
            ret_set_type(1, type);
            bb_engine_ret_level(1, 256);
            ret_voice(0, "t*p0", 37, 256);
            bb_engine_ret_send(BB_RET_SRC_V0, 1, 255);
            bb_engine_reset_t();
            bb_engine_reset_loop();
            ret_render(RET_SETTLE);
            ret_capture(pass ? b : a, 1024);
        }
        int drift = 0, energy = 0;
        for (int j = 0; j < 1024; j++) {
            if (a[j] != b[j]) drift++;
            if (a[j] != 0) energy++;
        }
        test_expect(drift == 0 && energy > 0,
                    "%s renders identically on two identical runs", nm);
    }

    ret_scene(rate);
}

/* ---- the matrix: who feeds what ---------------------------------------- */
static void test_ret_routing(void)
{
    const int rate = 8000;
    ret_scene(rate);

    test_expect(ret_set_type(1, RET_CHAMBER) && ret_set_type(2, RET_CHAMBER) &&
                ret_set_type(3, RET_CHAMBER),
                "three returns can exist at once");
    for (int r = 1; r <= 3; r++) bb_engine_ret_level(r, 256);

    /* a send at zero contributes nothing, even with the voice at full tilt */
    ret_voice(0, "t*p0", 37, 256);
    ret_render(RET_SETTLE);
    ret_peak(1); ret_peak(2); ret_peak(3);
    ret_render(4096);
    test_expect(ret_peak(1) == 0 && ret_peak(2) == 0 && ret_peak(3) == 0,
                "a send at zero puts nothing into any return");

    /* a send at full feeds ONLY the return it names */
    bb_engine_ret_send(BB_RET_SRC_V0, 2, 255);
    ret_render(RET_SETTLE);
    ret_peak(1); ret_peak(2); ret_peak(3);
    ret_render(4096);
    int p1 = ret_peak(1), p2 = ret_peak(2), p3 = ret_peak(3);
    test_expect(p2 > 0, "a send at full feeds its return");
    test_expect(p1 == 0 && p3 == 0,
                "a send feeds the return it names and no other");

    /* A second source column is independent of the first.
     *
     * Zeroing a send does NOT silence the return it was feeding: return 2 is a
     * CHAMBER that has just been driven at full for 4096 frames, and a reverb
     * tail is precisely the thing that outlives its input. Measured, it was
     * still putting 10 counts into its meter a full RET_SETTLE later, which is
     * the tail decaying exactly as it should rather than a routing fault.
     *
     * So re-arm all three returns before measuring. Destroying and recreating
     * clears the slot's arena, which is what actually establishes the
     * precondition this assertion needs -- an empty return -- instead of
     * hoping a decay outruns the window. The assertion itself is unchanged. */
    bb_engine_ret_send(BB_RET_SRC_V0, 2, 0);
    for (int r = 1; r <= 3; r++) {
        ret_set_type(r, RET_NONE);
        ret_set_type(r, RET_CHAMBER);
        bb_engine_ret_level(r, 256);
    }
    ret_voice(5, "t*p0&t>>5", 90, 256);
    bb_engine_ret_send(BB_RET_SRC_V0 + 5, 3, 255);
    ret_render(RET_SETTLE);
    ret_peak(1); ret_peak(2); ret_peak(3);
    ret_render(4096);
    test_expect(ret_peak(3) > 0 && ret_peak(1) == 0 && ret_peak(2) == 0,
                "each voice's send row is routed independently");

    /* the DRY master tap: a source with no voice send of its own */
    ret_scene(rate);
    ret_set_type(4, RET_CHAMBER);
    bb_engine_ret_level(4, 256);
    ret_voice(0, "t*p0", 37, 256);
    ret_render(RET_SETTLE);
    ret_peak(4);
    ret_render(4096);
    test_expect(ret_peak(4) == 0, "the DRY row is off until it is opened");
    bb_engine_ret_send(BB_RET_SRC_DRY, 4, 255);
    ret_render(RET_SETTLE);
    ret_peak(4);
    ret_render(4096);
    test_expect(ret_peak(4) > 0, "the DRY master tap feeds a return");

    /* mute freezes a return, exactly like a level of zero */
    bb_engine_ret_mute(4, 1);
    test_expect(bb_engine_ret_mute_get(4) == 1, "return mute round-trips");
    ret_render(1024);
    ret_peak(4);
    ret_render(4096);
    test_expect(ret_peak(4) == 0, "a muted return contributes nothing");
    bb_engine_ret_mute(4, 0);

    /* the published live count is what STATUS renders */
    ret_scene(rate);
    ret_render(512);
    test_expect(atomic_load(&bb.ret_active) == 0,
                "an all-closed graph publishes no live returns");
    bb_engine_ret_level(0, 200);
    ret_set_type(2, RET_CHAMBER);
    bb_engine_ret_level(2, 200);
    ret_render(512);
    test_expect(atomic_load(&bb.ret_active) == 2,
                "the live-return count reaches STATUS");

    ret_scene(rate);
}

/* ---- links, feedback, and the reason any of this is safe ---------------
 *
 * Returns feeding returns is the point of the feature, not an edge case: it
 * is the no-input-mixer technique. So the test is not "does feedback work",
 * it is "when the user does the thing the feature exists for, does the
 * instrument stay inside the rails". That is a hearing-safety property and it
 * gets asserted like one -- on the peak, on the limiter's own published gain
 * reduction, and on the DC content, which is the one that a peak assertion
 * cannot see and the one that damages hardware. */
#if RET_HAVE_DELAY
static void test_ret_feedback(void)
{
    const int rate = 8000;
    ret_scene(rate);

    test_expect(ret_set_type(1, RET_DELAY) && ret_set_type(2, RET_CHAMBER),
                "a two-node feedback graph can be built");
    bb_engine_ret_level(1, 256);
    bb_engine_ret_level(2, 0);          /* node 2 is heard only through 1 */

    /* A short, hot delay with a unity self-link. Loop gain is above one by
     * construction, so this WILL run away -- that is the point. */
    bb_engine_ret_param(1, 0, 0);       /* TIME: as short as it goes       */
    bb_engine_ret_param(1, 1, 200);     /* FEED: hot, but under unity      */
    bb_engine_ret_link(1, 1, 256);
    test_expect(bb_engine_ret_link_get(1, 1) == 256,
                "a return can be linked to itself");

    ret_voice(0, "t*p0", 37, 256);
    bb_engine_ret_send(BB_RET_SRC_V0, 1, 255);
    ret_render(RET_SETTLE);
    ret_render(8000);                   /* one second of full-tilt input   */
    atomic_store(&bb.layer[0].on, 0);
    bb_engine_ret_send(BB_RET_SRC_V0, 1, 0);
    /* Let the voice's own level ramp reach zero before measuring, so what
     * follows is the loop and nothing but the loop -- otherwise the dry
     * signal rides on top of the wet sum and the bus-ceiling assertion below
     * would be measuring the wrong thing. */
    ret_render(RET_SETTLE);

    int32_t rail = 0, worst = 0, gr_min = 256;
    int64_t sum = 0;
    long counted = 0;
    for (int blk = 0; blk < 80; blk++) { /* ten seconds with no input at all */
        ret_peak(1);
        bb_engine_render(ret_out, 1000, 1);
        int p = ret_peak(1);
        if (p > worst) worst = p;
        int gr = atomic_load(&bb.ret[1].gr);
        if (gr < gr_min) gr_min = gr;
        for (int j = 0; j < 1000; j++) {
            int32_t v = ret_out[j] < 0 ? -ret_out[j] : ret_out[j];
            if (v > rail) rail = v;
            if (blk >= 72) { sum += ret_out[j]; counted++; }
        }
    }

    test_expect(worst > 4096,
                "the feedback loop really does sustain with no input");
    test_expect(worst <= BB_RET_CEIL,
                "a runaway return stays under the return ceiling "
                "(peaked at %d, ceiling %d)", (int)worst, BB_RET_CEIL);
    test_expect(rail < 32767,
                "a runaway return never pins the master output "
                "(peaked at %d)", (int)rail);
    /* With the voices gone the master IS the summed wet bus, so this is the
     * wet-bus limiter being measured directly. Eight returns each held at
     * their own ceiling still sum to eight times unity, and the master's only
     * other protection is a hard clipper -- which in a feedback loop settles
     * into a full-scale square wave, the worst waveform at the worst level. */
    test_expect(rail <= BB_RET_BUS_CEIL,
                "the summed wet bus stays under its own ceiling "
                "(peaked at %d, ceiling %d)", (int)rail, BB_RET_BUS_CEIL);
    test_expect(gr_min < 256,
                "the limiter engages rather than letting the loop run away");
    long mean = counted ? (long)(sum / counted) : 0;
    test_expect(mean > -256 && mean < 256,
                "a pinned feedback loop is not sitting on a DC offset "
                "(mean %ld)", mean);

    /* PANIC has to BREAK the loop, not hide it. Before the return bus it
     * only zeroed the master gain, which with feedback in the graph would
     * mean releasing it dumps a fully saturated loop straight back at you. */
    atomic_store(&bb.panic, 1);
    ret_render(24000);                  /* three seconds, muted             */
    ret_peak(1);
    ret_render(4000);
    int after = ret_peak(1);
    atomic_store(&bb.panic, 0);
    test_expect(after < worst / 8,
                "PANIC decays a feedback loop instead of muting it "
                "(%d, was %d)", after, (int)worst);

    /* and the explicit escape hatch does what it says */
    bb_engine_ret_link(1, 1, 256);
    bb_engine_ret_link(2, 1, 128);
    bb_engine_ret_level(1, 256);
    bb_engine_ret_panic();
    int live_links = 0, live_levels = 0;
    for (int x = 0; x < BB_NRET; x++) {
        live_levels += bb_engine_ret_level_get(x) != 0;
        for (int y = 0; y < BB_NRET; y++)
            live_links += bb_engine_ret_link_get(x, y) != 0;
    }
    test_expect(live_links == 0 && live_levels == 0,
                "bb_engine_ret_panic zeroes every link and every return level");

    /* A link into a return that is NOT part of a cycle still carries audio:
     * the safety stage must not be a mute. */
    ret_scene(rate);
    ret_set_type(1, RET_CHAMBER);
    ret_set_type(2, RET_DELAY);
    bb_engine_ret_level(1, 0);
    bb_engine_ret_level(2, 256);
    ret_voice(0, "t*p0", 37, 256);
    bb_engine_ret_send(BB_RET_SRC_V0, 1, 255);
    ret_render(RET_SETTLE);
    ret_peak(2);
    ret_render(4096);
    test_expect(ret_peak(2) == 0, "an unlinked return hears nothing");
    bb_engine_ret_link(1, 2, 256);
    ret_render(RET_SETTLE);
    ret_peak(2);
    ret_render(4096);
    test_expect(ret_peak(2) > 0, "a return -> return link carries audio");

    ret_scene(rate);
}
#endif /* RET_HAVE_DELAY */

/* ---- persistence -------------------------------------------------------
 *
 * A routing graph that does not survive a save is a feature that does not
 * exist. Everything the mixer can edit has to come back: eight slot configs,
 * eight names, the whole send matrix and the whole link matrix -- and the
 * legacy `verb` and `send` keys have to keep meaning what they meant, since
 * slot 0 and send column 0 are stored in them. */
static void test_ret_session(void)
{
    char root[720];
    int made = tmp_dir_make(root, sizeof root, "rettest") == 0;
    test_expect(made, "temporary return-bus session directory can be created");
    if (!made) return;
    bb_config_set_root(root);

    ret_scene(8000);

    /* a graph with something of everything in it */
    ret_set_type(1, RET_DELAY);
    ret_set_type(3, RET_CHOIR);
    ret_set_type(5, RET_DRIVE);

    bb_engine_ret_level(0, 132);            /* aliased -> bb.verb_level     */
    bb_engine_ret_param(0, 0, 201);         /* aliased -> bb.verb_size      */
    bb_engine_ret_param(0, 1,  55);         /* aliased -> bb.verb_tone      */
    bb_engine_ret_param(0, 2,  90);         /* real storage                 */

    bb_engine_ret_level(1, 200);
    bb_engine_ret_mute(1, 1);
    bb_engine_ret_sync(1, 4);
    for (int p = 0; p < BB_RET_NPARAM; p++) bb_engine_ret_param(1, p, 10 + p * 7);

    bb_engine_ret_level(3, 90);
    for (int p = 0; p < BB_RET_NPARAM; p++) bb_engine_ret_param(3, p, 200 - p * 3);

    bb_engine_ret_level(5, 256);

    snprintf(bb_ret_name[1], BB_RET_NAME, "long hall");
    snprintf(bb_ret_name[3], BB_RET_NAME, "choir II");

    bb_engine_ret_send(BB_RET_SRC_V0 + 0, 1, 200);
    bb_engine_ret_send(BB_RET_SRC_V0 + 2, 3,  17);
    bb_engine_ret_send(BB_RET_SRC_LICKS,  5,  64);
    bb_engine_ret_send(BB_RET_SRC_DRY,    1, 255);
    bb_engine_ret_send(BB_RET_SRC_WET,    3, 128);
    bb_engine_ret_send(BB_RET_SRC_V0 + 4, 0,  77);   /* aliased -> layer 4  */
    bb_engine_ret_send(BB_RET_SRC_LICKS,  0,  31);   /* aliased -> smp_send */

    bb_engine_ret_link(1, 3, 256);
    bb_engine_ret_link(3, 3, 200);          /* the self-link is legal       */
    bb_engine_ret_link(5, 1,  33);

    test_expect(bb_config_save() == 0, "a session with a return graph saves");

    int ret_lines = 0, rsend_lines = 0, rlink_lines = 0, rname_lines = 0;
    int saw_verb = 0;
    FILE *f = bb_fopen(bb_config_path(), "r");
    if (f) {
        char line[1200];
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "ret ",   4)) ret_lines++;
            if (!strncmp(line, "rsend ", 6)) rsend_lines++;
            if (!strncmp(line, "rlink ", 6)) rlink_lines++;
            if (!strncmp(line, "rname ", 6)) rname_lines++;
            if (!strncmp(line, "verb ",  5)) saw_verb++;
        }
        fclose(f);
    }
    test_expect(f != NULL, "the return-bus session can be reopened");
    test_expect(ret_lines == BB_NRET,
                "the session carries one ret line per slot (%d)", ret_lines);
    test_expect(rsend_lines == 5,
                "only nonzero, non-aliased sends are written (%d lines)",
                rsend_lines);
    test_expect(rlink_lines == 3,
                "only nonzero links are written (%d lines)", rlink_lines);
    test_expect(rname_lines == 2,
                "only named returns are written (%d lines)", rname_lines);
    test_expect(saw_verb == 1,
                "the legacy verb line is still written for older binaries");

    /* wipe every trace of the graph, then load it back */
    ret_scene(8000);
    bb_engine_ret_param(0, 2, 0);
    test_expect(bb_engine_ret_type_get(1) == RET_NONE &&
                bb_engine_ret_level_get(1) == 0,
                "the graph really was wiped before the reload");
    test_expect(bb_config_load() == 1, "a session with a return graph loads");

    test_expect(bb_engine_ret_type_get(0) == RET_CHAMBER &&
                bb_engine_ret_type_get(1) == RET_DELAY &&
                bb_engine_ret_type_get(3) == RET_CHOIR &&
                bb_engine_ret_type_get(5) == RET_DRIVE,
                "return types survive a session round-trip");
    int still_empty = 0;
    for (int r = 0; r < BB_NRET; r++)
        if (r != 0 && r != 1 && r != 3 && r != 5)
            still_empty += bb_engine_ret_type_get(r) == RET_NONE;
    test_expect(still_empty == 4, "unused slots come back empty");

    test_expect(bb_engine_ret_level_get(0) == 132 &&
                atomic_load(&bb.verb_level) == 132 &&
                bb_engine_ret_param_get(0, 0) == 201 &&
                atomic_load(&bb.verb_size) == 201 &&
                bb_engine_ret_param_get(0, 1) == 55 &&
                atomic_load(&bb.verb_tone) == 55 &&
                bb_engine_ret_param_get(0, 2) == 90,
                "slot 0 round-trips through the legacy verb storage");
    test_expect(bb_engine_ret_level_get(1) == 200 &&
                bb_engine_ret_mute_get(1) == 1 &&
                bb_engine_ret_sync_get(1) == 4,
                "level, mute and sync survive a session round-trip");

    int pdiff = 0;
    for (int p = 0; p < BB_RET_NPARAM; p++) {
        if (bb_engine_ret_param_get(1, p) != 10 + p * 7) pdiff++;
        if (bb_engine_ret_param_get(3, p) != 200 - p * 3) pdiff++;
    }
    test_expect(pdiff == 0, "every effect parameter survives a round-trip");

    test_expect(!strcmp(bb_ret_name[1], "long hall") &&
                !strcmp(bb_ret_name[3], "choir II") &&
                bb_ret_name[2][0] == '\0',
                "return names, spaces and all, survive a round-trip");

    /* the whole matrix, cell by cell */
    int sdiff = 0, ldiff = 0;
    for (int s = 0; s < BB_RET_NSRC; s++) {
        for (int r = 0; r < BB_NRET; r++) {
            int want = 0;
            if (s == BB_RET_SRC_V0 + 0 && r == 1) want = 200;
            if (s == BB_RET_SRC_V0 + 2 && r == 3) want = 17;
            if (s == BB_RET_SRC_LICKS  && r == 5) want = 64;
            if (s == BB_RET_SRC_DRY    && r == 1) want = 255;
            if (s == BB_RET_SRC_WET    && r == 3) want = 128;
            if (s == BB_RET_SRC_V0 + 4 && r == 0) want = 77;
            if (s == BB_RET_SRC_LICKS  && r == 0) want = 31;
            if (bb_engine_ret_send_get(s, r) != want) sdiff++;
        }
    }
    for (int x = 0; x < BB_NRET; x++) {
        for (int y = 0; y < BB_NRET; y++) {
            int want = 0;
            if (x == 1 && y == 3) want = 256;
            if (x == 3 && y == 3) want = 200;
            if (x == 5 && y == 1) want = 33;
            if (bb_engine_ret_link_get(x, y) != want) ldiff++;
        }
    }
    test_expect(sdiff == 0,
                "all %d send cells survive a session round-trip",
                BB_RET_NSRC * BB_NRET);
    test_expect(ldiff == 0,
                "all %d link cells survive a session round-trip",
                BB_NRET * BB_NRET);
    test_expect(atomic_load(&bb.layer[4].send) == 77 &&
                atomic_load(&bb.smp_send) == 31,
                "the aliased send cells land on the legacy storage");

    /* ---- a session written before the return bus existed --------------
     * This is the case every existing session on disk is in: no ret, rsend,
     * rlink or rname lines at all. It must load to a graph that reproduces
     * the old audio exactly -- slot 0 a CHAMBER carrying the verb line's
     * values, everything else empty, matrix zero. Binary mode so the bytes
     * are the same on every platform, like the v3 and v6 fixtures above. */
    f = bb_fopen(bb_config_path(), "wb");
    if (f) {
        fputs("version 7\n"
              "rate 22050\n"
              "gain 180\n"
              "verb 11 22 33 44\n"
              "layer 0 on 1 mode 2 seq 0\n"
              "expr 0 t*p0\n", f);
        fclose(f);
    }
    test_expect(f != NULL, "a pre-return-bus session fixture can be written");
    ret_scene(8000);
    test_expect(bb_config_load() == 1, "a pre-return-bus session still loads");
    test_expect(bb_engine_ret_type_get(0) == RET_CHAMBER &&
                bb_engine_ret_level_get(0) == 33 &&
                bb_engine_ret_param_get(0, 0) == 11 &&
                bb_engine_ret_param_get(0, 1) == 22 &&
                atomic_load(&bb.smp_send) == 44,
                "an old session's verb line still lands on slot 0");
    int old_empty = 0, old_graph = 0;
    for (int r = 1; r < BB_NRET; r++) old_empty += bb_engine_ret_type_get(r) == RET_NONE;
    for (int s = 0; s < BB_RET_NSRC; s++)
        for (int r = 0; r < BB_NRET; r++)
            old_graph += bb_engine_ret_send_get(s, r) != 0;
    for (int x = 0; x < BB_NRET; x++)
        for (int y = 0; y < BB_NRET; y++)
            old_graph += bb_engine_ret_link_get(x, y) != 0;
    /* The graph is not EMPTY, and it must not be: the old `verb` line's fourth
     * field is the sampler bus send, and the assertion directly above this one
     * requires it to survive as 44. The loader mirrors it into the matrix as
     * SRC_SMP -> slot 0, which is the only way a pre-return-bus session still
     * sounds the same -- bb.smp_send is no longer read by the render loop at
     * all (it is written on load, written on save, and cleared on defaults),
     * so the matrix is the single source of truth and there is no double
     * count. Expecting a literally empty graph contradicted the line above it.
     *
     * What actually matters is that the migration invents nothing else: one
     * entry, from the sampler row, into slot 0, carrying exactly the value the
     * old file held. */
    const int smp_row = bb_engine_ret_send_get(BB_RET_SRC_LICKS, 0);
    test_expect(old_empty == BB_NRET - 1 && old_graph == 1 && smp_row == 44,
                "an old session opens with one CHAMBER and only its sampler send");

    /* ---- a session written by a NEWER build ----------------------------
     * Effect ids 5..9 are reserved. A slot carrying one of them was written
     * by a build that has an effect this one does not, and the only safe
     * behaviour is to render it as silence and write the number back out
     * untouched -- clamping it would silently rewrite somebody's patch, and
     * indexing a dispatch table with it would be a jump into nowhere. */
    f = bb_fopen(bb_config_path(), "wb");
    if (f) {
        fputs("version 7\n"
              "rate 8000\n"
              "verb 172 96 0 0\n"
              "ret 2 47 100 0 0 11 12 13 14 15 16 17 18\n"
              "rsend 9 2 255\n", f);
        fclose(f);
    }
    test_expect(f != NULL, "a newer-build session fixture can be written");
    ret_scene(8000);
    test_expect(bb_config_load() == 1, "a session from a newer build loads");
    test_expect(bb_engine_ret_type_get(2) == 47,
                "an unknown effect id is preserved, not clamped");
    /* and it is silent WITH A SIGNAL POINTED AT IT -- the fixture opens the
     * DRY row into slot 2, so this is the dispatch's `default: return 0`
     * being exercised, not just an absence of input. */
    ret_voice(0, "t*p0", 37, 256);
    ret_render(2048);
    ret_peak(2);
    ret_render(2048);
    test_expect(ret_peak(2) == 0, "an unknown effect id renders as silence");
    atomic_store(&bb.layer[0].on, 0);
    test_expect(bb_config_save() == 0, "a session with an unknown id saves");
    int saw_unknown = 0;
    f = bb_fopen(bb_config_path(), "r");
    if (f) {
        char line[1200];
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "ret 2 47 ", 9)) saw_unknown = 1;
        fclose(f);
    }
    test_expect(saw_unknown,
                "an unknown effect id survives load and save unchanged");

    /* ---- CRLF, for the same reason the arrangement has a CRLF test ----- */
    f = bb_fopen(bb_config_path(), "wb");
    if (f) {
        fputs("version 7\r\n"
              "rate 8000\r\n"
              "verb 172 96 0 0\r\n"
              "ret 1 2 190 0 3 5 6 7 8 9 10 11 12\r\n"
              "rsend 9 1 240\r\n"
              "rlink 1 1 128\r\n"
              "rname 1 9:cold organ\r\n", f);
        fclose(f);
    }
    test_expect(f != NULL, "a CRLF return-bus fixture can be written");
    ret_scene(8000);
    test_expect(bb_config_load() == 1, "a CRLF return-bus session loads");
    test_expect(bb_engine_ret_type_get(1) == RET_DELAY &&
                bb_engine_ret_level_get(1) == 190 &&
                bb_engine_ret_sync_get(1) == 3 &&
                bb_engine_ret_param_get(1, 0) == 5,
                "return fields survive CRLF line endings");
    test_expect(bb_engine_ret_send_get(BB_RET_SRC_DRY, 1) == 240 &&
                bb_engine_ret_link_get(1, 1) == 128,
                "the matrix survives CRLF line endings");
    test_expect(!strcmp(bb_ret_name[1], "cold organ"),
                "a return name read from a CRLF session carries no stray "
                "carriage return");

    bb_remove(bb_config_path());
    tmp_dir_remove(root);

    ret_scene(8000);
    bb_engine_set_defaults();
    bb_engine_reclaim();
}

#endif /* BB_NRET */

static void test_return_bus(void)
{
    test_chamber_golden();
#if defined(BB_NRET)
    test_ret_defaults();
    test_ret_lifecycle();
    test_ret_bypass();
    test_ret_types();
    test_ret_routing();
#if RET_HAVE_DELAY
    test_ret_feedback();
#endif
    test_ret_session();

    /* leave the engine the way an empty session would */
    bb_engine_set_defaults();
    bb_engine_init(44100);
    bb_engine_reclaim();
#endif
}

/* ======================================================================== */
/*  THE LOOP BANK                                                            */
/* ======================================================================== */

/* SURVIVOR was ONE looper, hardwired to the finished master bus. That is a
 * bounce, not a multitrack: capture a loop and everything you play afterwards
 * is recorded on top of a bus that already contains it, so every layer
 * arrives with the previous ones baked into it. That is what was reported
 * from playing the instrument, and the bank is the answer to it -- slot 0 IS
 * SURVIVOR, unchanged, and slots 1..5 are additive satellites that record
 * BB_LOOP_SRC_LIVE: the bus at the INPUT of the loop stage, which contains no
 * looper's playback at all, because no looper has run when it is taken.
 *
 * Three kinds of check live here, in this order.
 *
 *  1. BIT-EXACTNESS, asserted as SAMPLE VALUES and not as a hash of two
 *     renders of the same binary. Comparing two renders is the right tool for
 *     "these two paths agree" and it is completely blind to a change that
 *     moves BOTH sides -- which is precisely what a restructuring of the loop
 *     stage does. So test_loop_survivor_exact() re-derives loop_process()'s
 *     documented arithmetic in the test -- the +/-128 wet ramp, the
 *     crossfade, the playhead -- frame by frame against a reference render
 *     taken with the looper idle, and names the first frame that disagrees.
 *     If SURVIVOR moves, changes shape, or acquires a neighbour that leaks
 *     into it, this fails loudly and specifically.
 *
 *  2. THE CENTRAL GUARANTEE: layering. Record loop A, play it, record loop B,
 *     and B must not contain A. It is asserted the only way that means
 *     anything: B's captured buffer is compared BYTE FOR BYTE between a run
 *     where A is PLAYING and a run where A has been CLEARED, with a second
 *     assertion that the master output really did differ between those two
 *     runs -- otherwise the first one could pass while proving nothing.
 *
 *  3. Everything else that has to hold for this to be usable on stage: whole
 *     bars or nothing, the bar/cycle quantum, an independent playhead per
 *     slot, a mute that does not freeze it, an empty bank that costs exactly
 *     nothing, and a session that comes back.
 *
 * The three traps this file has already learned twice each all apply, and all
 * three are closed in lb_scene()/lb_restart() rather than per test:
 *   - bb.t is a free-running static that survives set_defaults and init, so
 *     every scenario calls bb_engine_reset_t().
 *   - gain_cur and the per-voice level ramps walk at 32/frame and survive
 *     set_defaults, so bb.gain is stored BEFORE bb_engine_init() (which snaps
 *     the master ramp to it) and bb_engine_reset_t() snaps g_lvl[]. No
 *     comparison here is ever sampling two different points of a fade.
 *   - a reverb or delay TAIL outlives its input, so these scenes open no
 *     return at all rather than hoping a decay outruns the window.
 */
#if defined(BB_NLOOP)

/* 8 kHz at 240 BPM with 4 beats puts a beat at 2000 frames and a BAR at 8000,
 * which is the same geometry the arrangement tests use. Every capture below
 * is a whole number of these. */
enum { LB_RATE = 8000, LB_BAR = 8000, LB_CAP = 2 * LB_BAR };

static int16_t lb_a[LB_CAP], lb_b[LB_CAP], lb_c[LB_CAP], lb_d[LB_CAP];
static int16_t lb_scratch[4096];

static int32_t lb_clip16(int32_t v)
{
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return v;
}

static void lb_render(int frames)
{
    while (frames > 0) {
        int n = frames > (int)(sizeof lb_scratch / sizeof lb_scratch[0])
              ? (int)(sizeof lb_scratch / sizeof lb_scratch[0]) : frames;
        bb_engine_render(lb_scratch, n, 1);
        frames -= n;
    }
}

/* One render call, so the whole window is one period: the exact-sample checks
 * below read bb.clipping afterwards and want it to describe the window they
 * just measured, and a HARD command issued before the call lands at its top. */
static void lb_render_into(int16_t *dst, int frames)
{
    bb_engine_render(dst, frames, 1);
}

/* A known-empty engine with a known-empty graph, at a known tempo. The master
 * gain is stored BEFORE bb_engine_init() on purpose -- init snaps g_gain_cur
 * to whatever bb.gain says, so from here the master ramp is already at unity
 * and o16 == the bus, which is what makes the sample-exact assertions below
 * possible at all. */
static void lb_scene_at(int rate, int bpm, int beats)
{
    bb_engine_set_defaults();
    atomic_store(&bb.gain, 256);
    bb_engine_init(rate);
    bb_engine_song_publish(NULL, 0);
    bb_engine_rec_src(BB_REC_MASTER);
    bb_engine_song_play(1);
    atomic_store(&bb.gctl[GCTL_BPM],   bpm);
    atomic_store(&bb.gctl[GCTL_BEATS], beats);
    atomic_store(&bb.panic, 0);
    atomic_store(&bb.mute, 0);
    atomic_store(&bb.smp_send, 0);
    for (int L = 0; L < BB_NLAYER; L++) {
        atomic_store(&bb.layer[L].on, 0);
        atomic_store(&bb.layer[L].send, 0);
    }
#if defined(BB_NRET)
    for (int r = 0; r < BB_NRET; r++) {
        bb_engine_ret_mute(r, 0);
        bb_engine_ret_level(r, 0);
        for (int s = 0; s < BB_RET_NSRC; s++) bb_engine_ret_send(s, r, 0);
        for (int q = 0; q < BB_NRET; q++)     bb_engine_ret_link(q, r, 0);
    }
#endif
    bb_engine_reset_t();
    bb_engine_reset_loop();
}

static void lb_scene(void) { lb_scene_at(LB_RATE, 240, 4); }

/* Both resets are consumed at the top of the next render, so a voice
 * published after lb_scene() is still snapped rather than ramped. */
static void lb_restart(void)
{
    bb_engine_reset_t();
    bb_engine_reset_loop();
}

/* Level 96 rather than 256: every exact-sample assertion below depends on the
 * bus never reaching the 16-bit rail (dsp_clip16 is not linear, and a clamped
 * frame would make a prediction that is arithmetically right compare unequal).
 * The scenarios assert bb.clipping == 0 rather than trusting that. */
static void lb_voice(int L, const char *src, int p0, int level)
{
    ExprError er;
    bb_publish(L, src, &er);
    atomic_store(&bb.layer[L].on, 1);
    atomic_store(&bb.layer[L].mode, BB_WORD);
    atomic_store(&bb.layer[L].seq_on, 0);
    atomic_store(&bb.layer[L].ctl[LCTL_LEVEL], level);
    atomic_store(&bb.layer[L].param[0], p0);
}

/* Copy a slot's recorded frames out from under the audio thread. Returns the
 * length, 0 if the slot is empty or longer than the caller's buffer. */
static unsigned lb_copy(int slot, int16_t *dst, unsigned max)
{
    unsigned len = 0;
    const int16_t *buf = bb_engine_loop_slot_buffer(slot, &len);
    if (!buf || !len || len > max) return 0;
    memcpy(dst, buf, len * sizeof *dst);
    return len;
}

/* Compare a slot's buffer against `want` without copying it. */
static int lb_buf_is(int slot, const int16_t *want, unsigned n)
{
    unsigned len = 0;
    const int16_t *buf = bb_engine_loop_slot_buffer(slot, &len);
    return buf && len == n && memcmp(buf, want, n * sizeof *want) == 0;
}

static void lb_arm(int slot, int bars, int level)
{
    bb_engine_loop_ctl(slot, L2C_BARS,  bars);
    bb_engine_loop_ctl(slot, L2C_LEVEL, level);
    bb_engine_loop_cmd(slot, LBC_ARM);
}

/* ---- slot 0 IS SURVIVOR, and its storage IS the legacy atomics ----------
 *
 * Same shape as test_ret_defaults, and for the same reason: the moment there
 * are two storages for the phrase looper's seven knobs they drift, and the
 * drift shows up as a session that loads the wrong loop or as a golden hash
 * that fails for a reason that looks like DSP. Pinned in both directions,
 * through the accessors and through the raw atomics. */
static void test_loop_defaults(void)
{
    bb_engine_set_defaults();
    bb_engine_init(LB_RATE);

    test_expect(BB_NLOOP == 6, "the loop bank is six slots");

    atomic_store(&bb.loop_bars, 3);
    atomic_store(&bb.loop_mix, 137);
    atomic_store(&bb.loop_feedback, 91);
    atomic_store(&bb.loop_overdub, 1);
    atomic_store(&bb.loop_rate, LOOP_RATE_DOUBLE);
    atomic_store(&bb.loop_reverse, 1);
    atomic_store(&bb.loop_slice, 8);
    test_expect(bb_engine_loop_ctl_get(0, L2C_BARS)     == 3 &&
                bb_engine_loop_ctl_get(0, L2C_LEVEL)    == 137 &&
                bb_engine_loop_ctl_get(0, L2C_FEEDBACK) == 91 &&
                bb_engine_loop_ctl_get(0, L2C_OVERDUB)  == 1 &&
                bb_engine_loop_ctl_get(0, L2C_RATE)     == LOOP_RATE_DOUBLE &&
                bb_engine_loop_ctl_get(0, L2C_REVERSE)  == 1 &&
                bb_engine_loop_ctl_get(0, L2C_SLICE)    == 8,
                "slot 0's controls read the legacy phrase-looper atomics");

    bb_engine_loop_ctl(0, L2C_BARS, 2);
    bb_engine_loop_ctl(0, L2C_LEVEL, 200);
    bb_engine_loop_ctl(0, L2C_FEEDBACK, 64);
    bb_engine_loop_ctl(0, L2C_OVERDUB, 0);
    bb_engine_loop_ctl(0, L2C_RATE, LOOP_RATE_HALF);
    bb_engine_loop_ctl(0, L2C_REVERSE, 0);
    bb_engine_loop_ctl(0, L2C_SLICE, 4);
    test_expect(atomic_load(&bb.loop_bars) == 2 &&
                atomic_load(&bb.loop_mix) == 200 &&
                atomic_load(&bb.loop_feedback) == 64 &&
                atomic_load(&bb.loop_overdub) == 0 &&
                atomic_load(&bb.loop_rate) == LOOP_RATE_HALF &&
                atomic_load(&bb.loop_reverse) == 0 &&
                atomic_load(&bb.loop_slice) == 4,
                "writing slot 0 writes the legacy atomics, not a shadow copy");

    /* slot 0's source is the whole bit-exactness argument, so it is not a
     * control at all: it reports the read-only MASTER sentinel internally and
     * the public setter refuses both it and MUTE. */
    bb_engine_loop_ctl(0, L2C_SRC, BB_LOOP_SRC_LIVE);
    bb_engine_loop_ctl(0, L2C_MUTE, 1);
    test_expect(loop_ctl_load(0, L2C_SRC) == BB_LOOP_SRC_MASTER &&
                loop_ctl_load(0, L2C_MUTE) == 0,
                "slot 0 reports the MASTER source and no mute, whatever is "
                "written to it");
    test_expect(bb_engine_loop_ctl_get(0, L2C_SRC) == -1 &&
                bb_engine_loop_ctl_get(0, L2C_MUTE) == -1,
                "the public control API refuses slot 0's source and mute "
                "outright rather than pretending they are knobs");
    test_expect(atomic_load(&bb.loopn[0].src) == 0 &&
                atomic_load(&bb.loopn[0].mute) == 0,
                "slot 0's aliased cells are left unused");

    /* the satellites' shipped defaults */
    bb_engine_set_defaults();
    int def_ok = 1;
    for (int n = 1; n < BB_NLOOP; n++) {
        def_ok &= bb_engine_loop_ctl_get(n, L2C_SRC)      == BB_LOOP_SRC_LIVE;
        def_ok &= bb_engine_loop_ctl_get(n, L2C_BARS)     == 0;
        def_ok &= bb_engine_loop_ctl_get(n, L2C_LEVEL)    == 200;
        def_ok &= bb_engine_loop_ctl_get(n, L2C_FEEDBACK) == 160;
        def_ok &= bb_engine_loop_ctl_get(n, L2C_OVERDUB)  == 0;
        def_ok &= bb_engine_loop_ctl_get(n, L2C_RATE)     == LOOP_RATE_NORMAL;
        def_ok &= bb_engine_loop_ctl_get(n, L2C_REVERSE)  == 0;
        def_ok &= bb_engine_loop_ctl_get(n, L2C_SLICE)    == 1;
        def_ok &= bb_engine_loop_ctl_get(n, L2C_MUTE)     == 0;
        def_ok &= bb_engine_loop_ctl_get(n, L2C_LANE)     == n;
        def_ok &= bb_engine_loop_status(n) == LOOP_OFF;
        def_ok &= bb_engine_loop_frames(n) == 0u;
        def_ok &= bb_engine_loop_pending(n) == 0;
    }
    test_expect(def_ok,
                "every satellite comes up on LIVE, FOLLOW, empty and silent");
    test_expect(bb_engine_loop_cycle_bars() == 0,
                "a defaulted session has established no cycle");

    /* A hand-edited session must never reach the DSP raw. Clamped in the
     * setter AND again in the render snapshot; this is the setter. */
    bb_engine_loop_ctl(1, L2C_LEVEL, 9999);
    int lvl_hi = bb_engine_loop_ctl_get(1, L2C_LEVEL);
    bb_engine_loop_ctl(1, L2C_LEVEL, -5);
    int lvl_lo = bb_engine_loop_ctl_get(1, L2C_LEVEL);
    bb_engine_loop_ctl(1, L2C_SLICE, 7);
    int slice = bb_engine_loop_ctl_get(1, L2C_SLICE);
    bb_engine_loop_ctl(1, L2C_SRC, 999);
    int src = bb_engine_loop_ctl_get(1, L2C_SRC);
    bb_engine_loop_ctl(1, L2C_BARS, 99);
    int bars = bb_engine_loop_ctl_get(1, L2C_BARS);
    bb_engine_loop_ctl(0, L2C_BARS, 99);
    int s0bars = bb_engine_loop_ctl_get(0, L2C_BARS);
    bb_engine_loop_ctl(1, L2C_LANE, 999);
    int lane = bb_engine_loop_ctl_get(1, L2C_LANE);
    test_expect(lvl_hi == 256 && lvl_lo == 0 && slice == 1 &&
                src == BB_LOOP_SRC_LIVE && bars == 8 && s0bars == 4 &&
                lane == ARR_LANES - 1,
                "out-of-range control values are clamped by the setter");

    /* out-of-range slots are inert rather than fatal */
    bb_engine_loop_ctl(-1, L2C_LEVEL, 100);
    bb_engine_loop_ctl(BB_NLOOP, L2C_LEVEL, 100);
    bb_engine_loop_cmd(-1, LBC_ARM);
    bb_engine_loop_cmd(BB_NLOOP, LBC_ARM);
    unsigned junk = 7;
    test_expect(bb_engine_loop_frames(-1) == 0u &&
                bb_engine_loop_frames(BB_NLOOP) == 0u &&
                bb_engine_loop_status(BB_NLOOP) == LOOP_OFF &&
                bb_engine_loop_ctl_get(BB_NLOOP, L2C_LEVEL) == -1 &&
                bb_engine_loop_slot_buffer(BB_NLOOP, &junk) == NULL &&
                junk == 0u,
                "a slot index outside the bank is inert");

    bb_engine_set_defaults();
}

/* ---- SURVIVOR, sample for sample --------------------------------------
 *
 * The bank's whole claim is that slot 0 did not move: same buffer, same
 * statics, same loop_process(), same call site, same arguments. This is that
 * claim as an audio property, and it is deliberately NOT a two-render
 * comparison -- it predicts every sample of a playback bar from first
 * principles.
 *
 * With the master gain settled at 256, g_gain_cur is exactly 65536 and
 * o16 == the bus, so the render buffer IS loop_process()'s output. The
 * reference run gives `live` for every frame (nothing downstream of the loop
 * stage feeds back, so the bus is identical with the looper idle or not); the
 * recorded buffer gives `played`; the wet ramp is +128 per frame from 0 to
 * mix<<8. Every frame of the playback bar is therefore known in advance. */
static void test_loop_survivor_exact(void)
{
    /* the reference: two bars, SURVIVOR never armed */
    lb_scene();
    lb_voice(0, "t*p0", 37, 96);
    lb_restart();
    lb_render_into(lb_a, LB_BAR);                 /* bar 0 */
    lb_render_into(lb_b, LB_BAR);                 /* bar 1 */

    int quiet = 0;
    for (int j = 0; j < LB_BAR; j++) if (lb_a[j] != 0) { quiet = 1; break; }
    test_expect(quiet, "the SURVIVOR scenario is real audio, not silence");

    /* the take: identical, with a one-bar capture from the downbeat */
    lb_scene();
    lb_voice(0, "t*p0", 37, 96);
    lb_restart();
    atomic_store(&bb.loop_bars, 1);
    atomic_store(&bb.loop_mix, 256);
    atomic_store(&bb.loop_feedback, 0);
    atomic_store(&bb.loop_overdub, 0);
    atomic_store(&bb.loop_rate, LOOP_RATE_NORMAL);
    atomic_store(&bb.loop_reverse, 0);
    atomic_store(&bb.loop_slice, 1);
    bb_engine_loop_cmd(0, LBC_ARM);

    lb_render_into(lb_c, LB_BAR);                 /* bar 0: the capture */
    test_expect(atomic_load(&bb.clipping) == 0,
                "the SURVIVOR scenario never reaches the 16-bit rail");
    test_expect(memcmp(lb_c, lb_a, LB_BAR * sizeof lb_a[0]) == 0,
                "a RECORDING phrase looper passes the bus through untouched");
    test_expect(bb_engine_loop_status(0) == LOOP_PLAYING &&
                bb_engine_loop_frames(0) == (unsigned)LB_BAR,
                "a one-bar capture ends exactly one bar later, PLAYING");

    unsigned len = 0;
    const int16_t *legacy = bb_engine_loop_buffer();
    const int16_t *slot0  = bb_engine_loop_slot_buffer(0, &len);
    test_expect(slot0 == legacy && len == (unsigned)LB_BAR,
                "slot 0's buffer IS the phrase looper's buffer");
    test_expect(memcmp(legacy, lb_a, LB_BAR * sizeof lb_a[0]) == 0,
                "the phrase looper records the master bus sample for sample");

    memcpy(lb_d, legacy, LB_BAR * sizeof lb_d[0]);   /* `played` */

    lb_render_into(lb_c, LB_BAR);                 /* bar 1: the playback */
    test_expect(atomic_load(&bb.clipping) == 0,
                "the SURVIVOR playback bar never reaches the 16-bit rail");

    int bad = -1, moved = 0;
    for (int f = 0; f < LB_BAR && bad < 0; f++) {
        int32_t wet = 128 * (f + 1);
        if (wet > 65536) wet = 65536;
        int16_t want = (int16_t)lb_clip16((int32_t)(
                           ((int64_t)lb_b[f] * (65536 - wet) +
                            (int64_t)lb_d[f] * wet) >> 16));
        if (lb_c[f] != want) bad = f;
        if (lb_c[f] != lb_b[f]) moved++;
    }
    test_expect(bad < 0,
                "SURVIVOR renders its documented +/-128 wet ramp and crossfade "
                "sample for sample (first mismatch at frame %d)", bad);
    test_expect(moved > LB_BAR / 2,
                "the SURVIVOR playback bar is audibly the loop, not the dry bus");
    test_expect(memcmp(legacy, lb_d, LB_BAR * sizeof lb_d[0]) == 0,
                "a phrase looper with overdub off never writes to its buffer");

    /* the legacy entry point still drives slot 0, and PLAY is still a TOGGLE
     * on it -- that asymmetry is the seam the bank's translation lives on */
    bb_engine_loop_command(LOOP_CMD_PLAY);
    lb_render(64);
    test_expect(bb_engine_loop_status(0) == LOOP_OFF,
                "the legacy PLAY command still toggles slot 0 off");
    bb_engine_loop_command(LOOP_CMD_PLAY);
    lb_render(64);
    test_expect(bb_engine_loop_status(0) == LOOP_PLAYING,
                "the legacy PLAY command still toggles slot 0 back on");

    /* the bank's PLAY is NOT a toggle: pressing it twice leaves it playing */
    bb_engine_loop_cmd(0, LBC_PLAY);
    lb_render(64);
    test_expect(bb_engine_loop_status(0) == LOOP_PLAYING,
                "the bank's PLAY on slot 0 is idempotent, not a toggle");
    bb_engine_loop_cmd(0, LBC_STOP);
    lb_render(64);
    test_expect(bb_engine_loop_status(0) == LOOP_OFF &&
                bb_engine_loop_frames(0) == (unsigned)LB_BAR,
                "the bank's STOP on slot 0 stops it and keeps its buffer");
    bb_engine_loop_cmd(0, LBC_CLEAR);
    lb_render(64);
    test_expect(bb_engine_loop_status(0) == LOOP_OFF &&
                bb_engine_loop_frames(0) == 0u,
                "the bank's CLEAR empties slot 0");
}

/* ---- an idle bank is a bit-exact bypass, and costs nothing --------------
 *
 * The satellite block is gated behind `if (sb.nlive)` and its taps behind the
 * want flags, so with nothing live not one added line executes. Stated as
 * sound: record, play and clear all five satellites, and the bar after they
 * are gone must be bit-identical to the same bar of a run that never touched
 * them. Both halves render exactly the same number of frames with the same
 * voice running throughout, so the only difference between them is the bank. */
static void test_loop_bypass(void)
{
    int peaked = 0, active_live = 0, active_idle = -1;

    for (int pass = 0; pass < 2; pass++) {
        lb_scene();
        lb_voice(0, "t*p0", 37, 96);
        lb_restart();

        if (pass) for (int n = 1; n < BB_NLOOP; n++) lb_arm(n, 1, 200);
        lb_render(LB_BAR);                        /* bar 0: five captures */
        lb_render(LB_BAR);                        /* bar 1: five playbacks */

        if (pass) {
            active_live = atomic_load(&bb.loop_active);
            peaked = 1;
            for (int n = 1; n < BB_NLOOP; n++)
                peaked &= bb_engine_loop_peak(n) > 0 &&
                          bb_engine_loop_frames(n) == (unsigned)LB_BAR &&
                          bb_engine_loop_status(n) == LOOP_PLAYING;
            for (int n = 1; n < BB_NLOOP; n++)
                bb_engine_loop_cmd(n, LBC_CLEAR | LBC_HARD);
        }
        lb_render(LB_BAR);                        /* bar 2: the bank empties */
        if (pass) active_idle = atomic_load(&bb.loop_active);
        lb_render_into(pass ? lb_b : lb_a, LB_BAR);   /* bar 3: measured */
    }

    test_expect(peaked && active_live == BB_NLOOP - 1,
                "all five satellites recorded and played back audibly");
    test_expect(active_idle == 0,
                "a cleared bank publishes no live satellites");
    test_expect(memcmp(lb_a, lb_b, LB_BAR * sizeof lb_a[0]) == 0,
                "recording, playing and clearing every satellite leaves the "
                "mix bus bit-identical");

    int cleared = 1;
    for (int n = 1; n < BB_NLOOP; n++)
        cleared &= bb_engine_loop_frames(n) == 0u &&
                   bb_engine_loop_status(n) == LOOP_OFF &&
                   bb_engine_loop_barlen(n) == 0;
    test_expect(cleared, "CLEAR empties a satellite and forgets its geometry");
    test_expect(bb_engine_loop_cycle_bars() == 0,
                "the cycle is collected once every satellite is empty");

    /* A STOPPED satellite is not merely silent, it is FROZEN -- the same
     * property test_ret_bypass pins on a closed return, and for the same
     * reason: a slot that is not live is not visited, so its playhead cannot
     * advance and it cannot come back out of sync with the others. */
    lb_scene();
    lb_voice(0, "t*p0", 37, 96);
    lb_restart();
    lb_arm(1, 1, 200);
    lb_render(LB_BAR);
    bb_engine_loop_cmd(1, LBC_STOP | LBC_HARD);
    lb_render(LB_BAR);                       /* the level ramp reaches zero */
    unsigned frozen = bb_engine_loop_pos(1);
    /* The meter is max-HOLD and is cleared by its reader, so the ramp-down's
     * peak is still latched here. Clear it, then measure the window that
     * matters -- the same discipline ret_peak() imposes. */
    (void)bb_engine_loop_peak(1);
    lb_render(LB_BAR);
    test_expect(bb_engine_loop_pos(1) == frozen &&
                bb_engine_loop_frames(1) == (unsigned)LB_BAR,
                "a stopped satellite freezes its playhead and keeps its audio");
    test_expect(bb_engine_loop_peak(1) == 0,
                "a stopped satellite meters nothing");
}

/* ---- whole bars, on the downbeat, or not at all ------------------------
 *
 * A loop that is not a whole number of bars is what makes a committed clip
 * drift against the grid, which is half of what was reported. So: the ARM
 * waits for the boundary however far into the bar it was pressed, the capture
 * is exactly the requested number of bars, and the frames it holds are the
 * bus it heard, sample for sample, starting on the downbeat. */
static void test_loop_capture(void)
{
    lb_scene();
    lb_voice(0, "t*p0", 37, 96);
    lb_restart();

    lb_render(1000);
    bb_engine_loop_ctl(1, L2C_BARS, 2);
    bb_engine_loop_cmd(1, LBC_ARM);
    lb_render(1000);
    test_expect(bb_engine_loop_status(1) == LOOP_OFF &&
                bb_engine_loop_pending(1) == LBC_ARM &&
                bb_engine_loop_frames(1) == 0u,
                "an ARM pressed mid-bar waits, and says so");
    lb_render(LB_BAR - 2000);                 /* up to the boundary, not over */
    test_expect(bb_engine_loop_status(1) == LOOP_OFF,
                "an ARM has still not fired one frame before the downbeat");

    lb_render_into(lb_a, 2 * LB_BAR);         /* bars 1-2: the capture */
    test_expect(bb_engine_loop_status(1) == LOOP_PLAYING,
                "the capture completes into PLAYING");
    test_expect(bb_engine_loop_frames(1) == (unsigned)(2 * LB_BAR) &&
                bb_engine_loop_barlen(1) == LB_BAR,
                "a two-bar capture is exactly two bars long and remembers the "
                "bar it was recorded at");
    test_expect(bb_engine_loop_frames(1) % (unsigned)bb_engine_loop_barlen(1) == 0u,
                "a capture is a whole number of bars");
    test_expect(lb_buf_is(1, lb_a, 2 * LB_BAR),
                "a satellite records the live bus sample for sample, starting "
                "exactly on the downbeat");
    test_expect(bb_engine_loop_cycle_bars() == 2,
                "the first satellite capture establishes the cycle");

    /* FOLLOW takes the cycle's length, and its ARM waits for a CYCLE edge
     * rather than merely the next bar. WHERE that edge falls is the very
     * arithmetic under test, so this does not hard-code a bar number -- it
     * lets the capture land and then asks the question that matters. */
    bb_engine_loop_ctl(2, L2C_BARS, 0);
    bb_engine_loop_ctl(2, L2C_LEVEL, 200);
    bb_engine_loop_cmd(2, LBC_ARM);
    int waited = 0;
    while (bb_engine_loop_status(2) != LOOP_PLAYING && waited < 8) {
        lb_render(LB_BAR);                    /* whole bars: alignment kept */
        waited++;
    }
    test_expect(bb_engine_loop_status(2) == LOOP_PLAYING &&
                bb_engine_loop_frames(2) == (unsigned)(2 * LB_BAR),
                "a FOLLOW satellite takes the cycle's length");
    test_expect(waited > 0 && waited <= 4,
                "the FOLLOW capture landed within a cycle or two (%d bars)",
                waited);

    /* THE POINT OF THE CYCLE, and the reason it is not simply the bar: two
     * layers recorded a cycle apart must share a DOWNBEAT. Both loops are the
     * same length, so if they do, their published positions agree at every
     * frame -- and the first frame of the new layer's playback is frame 0 of
     * both. If the cycle's origin is off by a bar this is where it shows up,
     * and it shows up in use as the second layer sitting a bar out for the
     * rest of the session. */
    lb_render(1);
    unsigned p1 = bb_engine_loop_pos(1), p2 = bb_engine_loop_pos(2);
    test_expect(p1 == 0u && p2 == 0u,
                "two layers recorded a cycle apart cross zero on the same "
                "frame (layer 1 at %u, layer 2 at %u)", p1, p2);

    /* HARD IS IGNORED ON ARM, and that is a decision rather than an
     * oversight: an ARM only sets the state to ARMED, and the ARMED ->
     * RECORDING edge still has to land on a boundary or the loop is not a
     * whole number of bars. So honouring HARD here would buy nothing and
     * cost the grid. Pressed mid-bar, the slot must not even latch ARMED. */
    lb_render(1500);
    bb_engine_loop_ctl(4, L2C_BARS, 0);
    bb_engine_loop_cmd(4, LBC_ARM | LBC_HARD);
    lb_render(64);
    test_expect(bb_engine_loop_status(4) == LOOP_OFF &&
                bb_engine_loop_pending(4) == LBC_ARM,
                "ARM ignores HARD: it waits for a boundary instead of latching "
                "mid-bar");
    bb_engine_loop_cmd(4, LBC_CLEAR | LBC_HARD);
    lb_render(64);

    /* A bar longer than the buffer cannot be recorded as whole bars, so the
     * ARM is REFUSED and says so, rather than quietly keeping a fraction.
     * 30 BPM with 16 beats at 48 kHz is a 1,536,000-frame bar. */
    lb_scene_at(48000, 30, 16);
    lb_restart();
    bb_engine_loop_ctl(1, L2C_BARS, 1);
    bb_engine_loop_cmd(1, LBC_ARM);
    lb_render(256);
    test_expect(bb_engine_loop_status(1) == LOOP_OFF &&
                bb_engine_loop_frames(1) == 0u &&
                bb_engine_loop_pending(1) == 0,
                "a bar longer than the loop buffer refuses the ARM outright");

    /* And a request that merely OVERFLOWS the buffer is clamped to whole
     * bars: 30 BPM with 8 beats at 48 kHz is a 768,000-frame bar, so four
     * bars will not fit and exactly one does. */
    lb_scene_at(48000, 30, 8);
    lb_restart();
    bb_engine_loop_ctl(1, L2C_BARS, 4);
    bb_engine_loop_cmd(1, LBC_ARM);
    lb_render(768000 + 256);
    unsigned got = bb_engine_loop_frames(1);
    int bl = bb_engine_loop_barlen(1);
    test_expect(got == 768000u && bl == 768000,
                "an oversized request is clamped to the whole bars that fit "
                "(%u frames, bar %d)", got, bl);
    test_expect(got <= BB_LOOP_LEN && bl > 0 && got % (unsigned)bl == 0u,
                "the clamped capture is still a whole number of bars");
}

/* ---- THE CENTRAL GUARANTEE: a layer records your hands, not the bank ----
 *
 * "I recorded an overdubbed sample and placed it in arrange" is downstream of
 * this: with one master looper, everything you play after the first pass is
 * recorded on top of a bus that already contains it. The satellites record
 * BB_LOOP_SRC_LIVE, which is the bus at the INPUT of the loop stage -- not a
 * subtraction, an ordering: no looper has run when it is taken.
 *
 * Muting layer A would NOT test this (a muted layer changes the master, not
 * LIVE), so the two runs differ by A being PLAYING versus CLEARED, and the
 * assertion is byte equality of B's captured buffer. The companion assertion
 * -- that the MASTER differed between those runs -- is what stops this from
 * passing vacuously if the bank ever stops making sound at all. */
static void test_loop_layering(void)
{
    unsigned len[2] = { 0, 0 };

    for (int pass = 0; pass < 2; pass++) {
        lb_scene();
        lb_voice(0, "t*p0", 37, 96);
        lb_restart();

        lb_arm(1, 1, 256);                    /* A: one bar from the downbeat */
        lb_render(LB_BAR);                    /* bar 0: A captures */

        if (pass) bb_engine_loop_cmd(1, LBC_CLEAR | LBC_HARD);
        lb_arm(2, 1, 256);                    /* B: the very next bar */
        lb_render_into(pass ? lb_b : lb_a, LB_BAR);   /* bar 1 */

        len[pass] = lb_copy(2, pass ? lb_d : lb_c, LB_CAP);
    }

    test_expect(len[0] == (unsigned)LB_BAR && len[1] == len[0],
                "both runs captured a one-bar layer");
    int loud = 0;
    for (int j = 0; j < LB_BAR; j++) if (lb_c[j] != 0) { loud = 1; break; }
    test_expect(loud, "the captured layer is not silence");

    int master_diff = 0;
    for (int j = 0; j < LB_BAR; j++) if (lb_a[j] != lb_b[j]) master_diff++;
    test_expect(master_diff > LB_BAR / 4,
                "the layer that was playing really was in the mix (%d frames "
                "of %d differ)", master_diff, LB_BAR);

    test_expect(memcmp(lb_c, lb_d, LB_BAR * sizeof lb_c[0]) == 0,
                "a satellite records the live bus, NOT the loop bank: the same "
                "layer is captured byte for byte whether the previous layer is "
                "playing or cleared");

    /* and what it captured is exactly the bus of the run with no looper in
     * it -- the strongest form of the same statement */
    test_expect(memcmp(lb_c, lb_b, LB_BAR * sizeof lb_c[0]) == 0,
                "the captured layer IS the live bus, sample for sample");
}

/* ---- overdub accumulates, feedback decays, clear empties ---------------
 *
 * All three are pinned as arithmetic rather than as loudness. The slot is
 * MUTED throughout so its own playback contributes exactly nothing to the
 * master, which leaves the render buffer equal to the source the overdub is
 * writing -- and lets every write be predicted exactly:
 *
 *     buf'[i] = clip16(live[i] + ((buf[i] * feedback) >> 8))
 *
 * Muting does not stop the write, and it must not: `hold` is not a concept
 * here, and a muted layer keeps its playhead. */
static void test_loop_overdub(void)
{
    lb_scene();
    lb_voice(0, "t*p0", 37, 96);
    lb_restart();

    lb_arm(1, 1, 200);
    lb_render_into(lb_a, LB_BAR);                 /* bar 0: the capture */
    test_expect(bb_engine_loop_frames(1) == (unsigned)LB_BAR &&
                lb_buf_is(1, lb_a, LB_BAR),
                "the overdub scenario starts from a known capture");

    bb_engine_loop_ctl(1, L2C_MUTE, 1);
    lb_render(LB_BAR);                            /* bar 1: the mute settles */
    test_expect(lb_buf_is(1, lb_a, LB_BAR),
                "a playing satellite with overdub off never writes");

    /* pass A: overdub on, feedback 0 -- the loop becomes the live bus */
    bb_engine_loop_ctl(1, L2C_OVERDUB, 1);
    bb_engine_loop_ctl(1, L2C_FEEDBACK, 0);
    lb_render_into(lb_b, LB_BAR);                 /* bar 2 */
    test_expect(atomic_load(&bb.clipping) == 0,
                "the overdub scenario never reaches the 16-bit rail");
    test_expect(lb_buf_is(1, lb_b, LB_BAR),
                "overdub at feedback 0 replaces the loop with what it hears");

    /* pass B: feedback 160 -- the pass already in the buffer is scaled by
     * 160/256 and the new one is summed on top of it */
    memcpy(lb_c, lb_b, LB_BAR * sizeof lb_c[0]);  /* what is in the buffer */
    bb_engine_loop_ctl(1, L2C_FEEDBACK, 160);
    lb_render_into(lb_d, LB_BAR);                 /* bar 3: the live bus */
    test_expect(atomic_load(&bb.clipping) == 0,
                "the feedback pass never reaches the 16-bit rail");
    for (int j = 0; j < LB_BAR; j++)
        lb_a[j] = (int16_t)lb_clip16(lb_d[j] +
                      (int32_t)(((int64_t)lb_c[j] * 160) >> 8));
    test_expect(lb_buf_is(1, lb_a, LB_BAR),
                "overdub sums the live bus into the loop with the retained "
                "audio scaled by FEEDBACK, sample for sample");
    int grew = 0;
    for (int j = 0; j < LB_BAR; j++) if (lb_a[j] != lb_d[j]) grew++;
    test_expect(grew > LB_BAR / 4,
                "the overdub pass really accumulated rather than overwriting");

    /* Now silence the input so the decay is the loop and nothing but the
     * loop. A layer whose level ramp has reached zero is SKIPPED entirely
     * (engine.c's `if (g_lvl[L] == 0 || !sn->prog) continue;`), so with no
     * return open, no sampler and no arrangement the bus is EXACTLY zero --
     * which is what lets the decay be asserted as arithmetic instead of as a
     * ratio somebody has to argue about. */
    bb_engine_loop_ctl(1, L2C_OVERDUB, 0);
    atomic_store(&bb.layer[0].on, 0);
    lb_render(LB_BAR);                            /* bar 4: the level ramps out */
    lb_render_into(lb_b, LB_BAR);                 /* bar 5: measured silence */
    int silent = 1;
    for (int j = 0; j < LB_BAR; j++) if (lb_b[j] != 0) { silent = 0; break; }
    test_expect(silent, "the decay measurement starts from an exactly silent bus");

    unsigned before = lb_copy(1, lb_c, LB_CAP);
    bb_engine_loop_ctl(1, L2C_OVERDUB, 1);
    bb_engine_loop_ctl(1, L2C_FEEDBACK, 128);
    lb_render(LB_BAR);                            /* bar 6: one decay pass */
    for (int j = 0; j < LB_BAR; j++)
        lb_a[j] = (int16_t)(((int32_t)lb_c[j] * 128) >> 8);
    test_expect(before == (unsigned)LB_BAR && lb_buf_is(1, lb_a, LB_BAR),
                "feedback below unity decays the loop by exactly its ratio "
                "on every pass");
    long e0 = 0, e1 = 0;
    for (int j = 0; j < LB_BAR; j++) {
        e0 += lb_c[j] < 0 ? -lb_c[j] : lb_c[j];
        e1 += lb_a[j] < 0 ? -lb_a[j] : lb_a[j];
    }
    test_expect(e1 < e0 && e1 > 0,
                "the decayed pass is quieter than the one before it, and is "
                "still there (%ld -> %ld)", e0, e1);

    bb_engine_loop_ctl(1, L2C_FEEDBACK, 0);
    lb_render(LB_BAR);                            /* bar 7: erased by silence */
    unsigned after = lb_copy(1, lb_b, LB_CAP);
    int erased = after == (unsigned)LB_BAR;
    for (int j = 0; j < LB_BAR && erased; j++) if (lb_b[j] != 0) erased = 0;
    test_expect(erased,
                "overdub at feedback 0 over silence erases the loop exactly");
    test_expect(bb_engine_loop_frames(1) == (unsigned)LB_BAR,
                "an erased loop is still a loop -- CLEAR is the eraser, not "
                "overdub");

    bb_engine_loop_cmd(1, LBC_CLEAR | LBC_HARD);
    lb_render(64);
    test_expect(bb_engine_loop_frames(1) == 0u &&
                bb_engine_loop_status(1) == LOOP_OFF &&
                bb_engine_loop_barlen(1) == 0,
                "CLEAR empties the slot");
}

/* ---- six independent loopers, not one looper with six faders -----------
 *
 * Every satellite carries its own buffer, state, playhead and controls, and
 * touching one must not disturb another. The playhead assertions are exact
 * rather than "it moved": with slice 1, rate NORMAL and reverse off the
 * published position advances by exactly one frame per rendered frame,
 * modulo the loop length, so a frozen or re-seeded playhead is caught to the
 * sample -- which matters because a layer that comes back one frame out of
 * phase never comes back into it. */
static void test_loop_slots(void)
{
    lb_scene();
    lb_voice(0, "t*p0", 37, 96);
    lb_restart();

    lb_arm(1, 1, 200);
    lb_render(LB_BAR);                       /* bar 0: slot 1 captures */
    lb_arm(3, 1, 200);
    lb_render(LB_BAR);                       /* bar 1: slot 3 captures */

    test_expect(bb_engine_loop_status(1) == LOOP_PLAYING &&
                bb_engine_loop_status(3) == LOOP_PLAYING &&
                bb_engine_loop_frames(1) == (unsigned)LB_BAR &&
                bb_engine_loop_frames(3) == (unsigned)LB_BAR &&
                bb_engine_loop_frames(2) == 0u,
                "two slots hold their own captures and the third stays empty");

    unsigned p1 = bb_engine_loop_pos(1), p3 = bb_engine_loop_pos(3);
    lb_arm(5, 1, 200);
    lb_render(3000);                         /* slot 5 is recording meanwhile */
    test_expect(bb_engine_loop_status(5) == LOOP_RECORDING,
                "the third slot armed on the downbeat and is recording");
    test_expect(bb_engine_loop_status(1) == LOOP_PLAYING &&
                bb_engine_loop_status(3) == LOOP_PLAYING &&
                bb_engine_loop_frames(1) == (unsigned)LB_BAR &&
                bb_engine_loop_frames(3) == (unsigned)LB_BAR,
                "arming one slot does not disturb another's state");
    test_expect((bb_engine_loop_pos(1) + LB_BAR - p1) % LB_BAR == 3000u &&
                (bb_engine_loop_pos(3) + LB_BAR - p3) % LB_BAR == 3000u,
                "each slot's playhead advances one frame per frame, "
                "independently");

    /* MUTE IS NOT A FREEZE. This is the single most-used control on a
     * multitrack looper, and if it stopped the playhead the layer would come
     * back permanently out of sync with every other layer. */
    bb_engine_loop_ctl(1, L2C_MUTE, 1);
    lb_render(1000);                         /* the level ramp reaches zero */
    (void)bb_engine_loop_peak(1);
    p1 = bb_engine_loop_pos(1);
    lb_render(3000);
    test_expect(bb_engine_loop_peak(1) == 0,
                "a muted satellite contributes nothing");
    test_expect((bb_engine_loop_pos(1) + LB_BAR - p1) % LB_BAR == 3000u,
                "a muted satellite keeps its playhead running to the sample");
    bb_engine_loop_ctl(1, L2C_MUTE, 0);
    lb_render(2000);
    test_expect(bb_engine_loop_peak(1) > 0,
                "unmuting brings the same satellite straight back");

    /* clearing one leaves the others exactly where they were */
    p3 = bb_engine_loop_pos(3);
    bb_engine_loop_cmd(1, LBC_CLEAR | LBC_HARD);
    lb_render(1500);
    test_expect(bb_engine_loop_frames(1) == 0u &&
                bb_engine_loop_status(1) == LOOP_OFF,
                "CLEAR empties the slot it names");
    test_expect(bb_engine_loop_status(3) == LOOP_PLAYING &&
                bb_engine_loop_frames(3) == (unsigned)LB_BAR &&
                (bb_engine_loop_pos(3) + LB_BAR - p3) % LB_BAR == 1500u,
                "clearing one slot does not disturb another's audio or "
                "playhead");
    test_expect(bb_engine_loop_cycle_bars() == 1,
                "the cycle survives while any satellite still holds audio");
}

/* ---- the quantum: bar for STOP and PLAY, cycle for ARM and CLEAR -------
 *
 * Quantised is the default because layers that do not share a downbeat are
 * the whole problem. HARD is the escape hatch, and it is honest about what it
 * costs: it lands at the next PERIOD boundary, up to one audio buffer late,
 * because the alternative is reading an atomic per frame. */
static void test_loop_quantum(void)
{
    lb_scene();
    lb_voice(0, "t*p0", 37, 96);
    lb_restart();

    lb_arm(1, 1, 200);
    lb_render(LB_BAR);                       /* recorded; playing; cycle = 1 */
    test_expect(bb_engine_loop_status(1) == LOOP_PLAYING,
                "the quantum scenario starts from a playing layer");

    lb_render(2000);
    bb_engine_loop_cmd(1, LBC_STOP);
    lb_render(1000);
    test_expect(bb_engine_loop_status(1) == LOOP_PLAYING &&
                bb_engine_loop_pending(1) == LBC_STOP,
                "a quantised STOP waits for the bar and publishes that it is "
                "waiting");
    lb_render(LB_BAR - 3000);
    test_expect(bb_engine_loop_status(1) == LOOP_PLAYING,
                "the STOP has still not landed one frame before the bar");
    lb_render(1);
    test_expect(bb_engine_loop_status(1) == LOOP_OFF &&
                bb_engine_loop_pending(1) == 0,
                "the STOP lands on the downbeat, and clears its own pending "
                "flag");

    /* HARD skips the wait -- one period, not one bar */
    lb_render(1000);
    bb_engine_loop_cmd(1, LBC_PLAY | LBC_HARD);
    lb_render(64);
    test_expect(bb_engine_loop_status(1) == LOOP_PLAYING &&
                bb_engine_loop_pending(1) == 0,
                "a HARD PLAY lands within one period, mid-bar");
    bb_engine_loop_cmd(1, LBC_STOP | LBC_HARD);
    lb_render(64);
    test_expect(bb_engine_loop_status(1) == LOOP_OFF,
                "a HARD STOP lands within one period, mid-bar");

    /* an empty slot arms instead of playing nothing */
    bb_engine_loop_cmd(4, LBC_PLAY | LBC_HARD);
    lb_render(64);
    test_expect(bb_engine_loop_status(4) == LOOP_ARMED,
                "PLAY on an empty slot arms it");
    bb_engine_loop_cmd(4, LBC_CLEAR | LBC_HARD);
    lb_render(64);
    test_expect(bb_engine_loop_status(4) == LOOP_OFF,
                "CLEAR takes an armed slot back out of the way");

    /* ---- PLAY re-phases, it does not rewind ----------------------------
     * A layer brought back has to drop in WHERE THE TRANSPORT IS, not at its
     * own frame 0 -- otherwise stopping and starting a layer mid-piece leaves
     * it a bar out for the rest of the session, which is the same defect as
     * arming on the bar instead of the cycle, arriving by the other door.
     * It needs a loop longer than one bar to be visible at all, so: a two-bar
     * layer, stopped, and brought back on the downbeat of the SECOND bar of
     * its own cycle. It must resume at frame LB_BAR, not at frame 0. */
    lb_scene();
    lb_voice(0, "t*p0", 37, 96);
    lb_restart();
    lb_arm(1, 2, 200);
    lb_render(2 * LB_BAR);                   /* bars 0-1 captured */
    test_expect(bb_engine_loop_frames(1) == (unsigned)(2 * LB_BAR),
                "the re-phase scenario starts from a two-bar layer");
    bb_engine_loop_cmd(1, LBC_STOP | LBC_HARD);
    lb_render(LB_BAR);                       /* bar 2 passes, stopped */
    bb_engine_loop_cmd(1, LBC_PLAY);         /* quantised: bar 3's downbeat */
    lb_render(1);
    unsigned pos = bb_engine_loop_pos(1);
    test_expect(pos == (unsigned)LB_BAR,
                "PLAY re-phases a returning layer to the transport instead of "
                "rewinding it (pos %u, want %u)", pos, (unsigned)LB_BAR);
}

/* ---- the transport owns every playhead, and PANIC stops the lot -------- */
static void test_loop_transport(void)
{
    lb_scene();
    lb_voice(0, "t*p0", 37, 96);
    lb_restart();

    lb_arm(1, 1, 200); lb_render(LB_BAR);
    lb_arm(2, 1, 200); lb_render(LB_BAR);
    lb_arm(3, 1, 200); lb_render(LB_BAR);
    atomic_store(&bb.loop_bars, 1);
    bb_engine_loop_cmd(0, LBC_ARM);
    lb_render(LB_BAR);                       /* slot 0 bounces the bank */

    lb_render(2345);                         /* land somewhere off the grid */
    int off_grid = bb_engine_loop_pos(1) != 0u;

    bb_engine_reset_loop();
    lb_render(1);
    test_expect(off_grid &&
                bb_engine_loop_pos(1) == 0u && bb_engine_loop_pos(2) == 0u &&
                bb_engine_loop_pos(3) == 0u,
                "restarting the transport restarts every satellite's playhead");
    test_expect(bb_engine_loop_status(1) == LOOP_PLAYING &&
                bb_engine_loop_frames(1) == (unsigned)LB_BAR,
                "a transport restart keeps the audio it restarted");

    int playing = 0;
    for (int n = 0; n < BB_NLOOP; n++)
        playing += bb_engine_loop_status(n) == LOOP_PLAYING;
    test_expect(playing == 4,
                "four loopers -- the master and three layers -- are running");

    bb_engine_loop_panic();
    lb_render(64);
    int stopped = 0, kept = 0;
    for (int n = 0; n < BB_NLOOP; n++) {
        stopped += bb_engine_loop_status(n) == LOOP_OFF;
        kept    += bb_engine_loop_frames(n) != 0u;
    }
    test_expect(stopped == BB_NLOOP,
                "PANIC hard-stops all six loopers, slot 0 included");
    test_expect(kept == 4,
                "PANIC stops the loopers without erasing them");
}

/* ---- the bridge to ARRANGE --------------------------------------------
 *
 * A finished loop has to become a clip, or the jam never becomes a piece.
 * The commit is blocking and bounded, and it turns the slot's overdub OFF --
 * with overdub off the buffer is static and the copy is a plain memcpy, which
 * is the whole reason there is no hold handshake here. The last render before
 * each call is deliberately SHORT: the commit's stall bound scales with the
 * published period length, so a 64-frame period keeps the wait in
 * milliseconds when (as here) there is no render thread to move the epoch. */
static void test_loop_commit(void)
{
    unsigned bars = 99, len = 0;

    lb_scene();
    lb_voice(0, "t*p0", 37, 96);
    lb_restart();

    lb_render(64);
    test_expect(bb_engine_loop_clip(2, &bars) == NULL && bars == 0u,
                "an empty slot commits nothing");

    /* The ARM was pressed 64 frames into bar 0, so the capture starts on bar
     * 1's downbeat: render out the rest of this bar and half of the next. */
    lb_arm(2, 2, 200);
    lb_render(LB_BAR - 64 + LB_BAR / 2);
    lb_render(64);
    bars = 99;
    test_expect(bb_engine_loop_status(2) == LOOP_RECORDING &&
                bb_engine_loop_clip(2, &bars) == NULL && bars == 0u,
                "a recording slot commits nothing");

    lb_render(2 * LB_BAR);                   /* let the capture finish */
    bb_engine_loop_ctl(2, L2C_OVERDUB, 1);
    lb_render(LB_BAR);                       /* overdubbing while we commit */

    lb_render(64);
    bars = 99;
    ArrClipBuf *cb = bb_engine_loop_clip(2, &bars);
    test_expect(cb != NULL, "a finished loop commits to a clip buffer");
    if (!cb) return;

    len = bb_engine_clip_frames(cb);
    test_expect(len == (unsigned)(2 * LB_BAR) && bars == 2u,
                "the committed clip carries its recorded length in frames "
                "(%u) and in BARS (%u)", len, bars);
    test_expect(bb_engine_loop_ctl_get(2, L2C_OVERDUB) == 0,
                "committing a loop turns its overdub off -- the freeze is "
                "stated, not hidden");
    test_expect(lb_buf_is(2, bb_engine_clip_data(cb), len),
                "the committed clip is the slot's buffer, sample for sample");

    /* and it stays that way: with overdub off the buffer no longer moves */
    memcpy(lb_a, bb_engine_clip_data(cb), 2 * LB_BAR * sizeof lb_a[0]);
    lb_render(LB_BAR);
    test_expect(lb_buf_is(2, lb_a, 2 * LB_BAR) &&
                memcmp(bb_engine_clip_data(cb), lb_a,
                       2 * LB_BAR * sizeof lb_a[0]) == 0,
                "a committed loop is frozen, and the clip is an independent "
                "copy of it");

    bb_engine_clip_release(cb);
    bb_engine_reclaim();
    bb_engine_reclaim();

    bars = 99;
    test_expect(bb_engine_loop_clip(0, &bars) == NULL && bars == 0u,
                "slot 0 is the bounce, not a clip source");
}

/* ---- persistence, with no version bump --------------------------------
 *
 * Six fixed-arity `loopn` lines, written BEFORE the legacy `looper` line so a
 * hand edit to the familiar key still wins -- the same rule `verb` follows
 * over `ret 0`. Slot 0's fields are written through the alias, so the two
 * lines can never disagree, and a session that predates the bank still loads
 * to exactly the instrument it described. */
static void test_loop_session(void)
{
    static const int SRC[BB_NLOOP]   = { 0, BB_LOOP_SRC_LICKS, BB_LOOP_SRC_DRY,
                                         BB_LOOP_SRC_LIVE, BB_LOOP_SRC_V0 + 2,
                                         BB_LOOP_SRC_LIVE };
    static const int SLICE[BB_NLOOP] = { 8, 2, 4, 16, 1, 8 };
    char root[720];
    int made = tmp_dir_make(root, sizeof root, "loopbank") == 0;
    test_expect(made, "temporary loop-bank session directory can be created");
    if (!made) return;
    bb_config_set_root(root);

    lb_scene();

    /* slot 0 through the alias, and five satellites all different */
    bb_engine_loop_ctl(0, L2C_BARS, 3);
    bb_engine_loop_ctl(0, L2C_LEVEL, 137);
    bb_engine_loop_ctl(0, L2C_FEEDBACK, 91);
    bb_engine_loop_ctl(0, L2C_OVERDUB, 1);
    bb_engine_loop_ctl(0, L2C_RATE, LOOP_RATE_DOUBLE);
    bb_engine_loop_ctl(0, L2C_REVERSE, 1);
    bb_engine_loop_ctl(0, L2C_SLICE, SLICE[0]);
    bb_engine_loop_ctl(0, L2C_LANE, 7);
    for (int n = 1; n < BB_NLOOP; n++) {
        bb_engine_loop_ctl(n, L2C_SRC,      SRC[n]);
        bb_engine_loop_ctl(n, L2C_BARS,     n);
        bb_engine_loop_ctl(n, L2C_LEVEL,    100 + n * 10);
        bb_engine_loop_ctl(n, L2C_FEEDBACK, 50 + n);
        bb_engine_loop_ctl(n, L2C_OVERDUB,  n & 1);
        bb_engine_loop_ctl(n, L2C_RATE,     n % 3);
        bb_engine_loop_ctl(n, L2C_REVERSE,  !(n & 1));
        bb_engine_loop_ctl(n, L2C_SLICE,    SLICE[n]);
        bb_engine_loop_ctl(n, L2C_MUTE,     n == 4);
        bb_engine_loop_ctl(n, L2C_LANE,     (n * 2) % ARR_LANES);
    }

    test_expect(bb_config_save() == 0, "a session with a loop bank saves");

    int loopn_lines = 0, looper_lines = 0, order_ok = 0, agree = 0;
    int seen_loopn = 0;
    FILE *f = bb_fopen(bb_config_path(), "r");
    if (f) {
        char line[1200];
        int l0[11], lp[7];
        int have0 = 0, havep = 0;
        while (fgets(line, sizeof line, f)) {
            if (!strncmp(line, "loopn ", 6)) {
                loopn_lines++;
                seen_loopn = 1;
                /* Parse into a scratch row and copy it out only for slot 0.
                 * sscanf writes its arguments before the `n == 0` that used to
                 * guard it is ever evaluated, so every later loopn line
                 * overwrote l0 and by end of file it held slot 5 -- this then
                 * compared the wrong slot's knobs against the legacy line. */
                int n, row[11];
                if (sscanf(line, "loopn %d %d %d %d %d %d %d %d %d %d %d",
                           &n, &row[1], &row[2], &row[3], &row[4], &row[5],
                           &row[6], &row[7], &row[8], &row[9], &row[10]) == 11
                    && n == 0) {
                    for (int q = 1; q <= 10; q++) l0[q] = row[q];
                    have0 = 1;
                }
            }
            if (!strncmp(line, "looper ", 7)) {
                looper_lines++;
                /* the legacy key is emitted AFTER the bank, so a hand edit to
                 * it wins -- exactly the rule `verb` follows over `ret 0` */
                if (seen_loopn) order_ok = 1;
                if (sscanf(line, "looper %d %d %d %d %d %d %d",
                           &lp[0], &lp[1], &lp[2], &lp[3], &lp[4], &lp[5],
                           &lp[6]) == 7)
                    havep = 1;
            }
        }
        fclose(f);
        if (have0 && havep)
            agree = l0[2] == lp[0] && l0[3] == lp[1] && l0[4] == lp[2] &&
                    l0[5] == lp[3] && l0[6] == lp[4] && l0[7] == lp[5] &&
                    l0[8] == lp[6];
    }
    test_expect(f != NULL, "the loop-bank session can be reopened");
    test_expect(loopn_lines == BB_NLOOP,
                "the session carries one loopn line per slot (%d)", loopn_lines);
    test_expect(looper_lines == 1,
                "the legacy looper line is still written for older binaries");
    test_expect(order_ok,
                "the legacy looper line is written after the bank, so a hand "
                "edit to it wins");
    test_expect(agree,
                "loopn 0 and looper can never disagree -- they are the same "
                "storage");

    /* wipe and reload */
    lb_scene();
    test_expect(bb_engine_loop_ctl_get(1, L2C_LEVEL) == 200 &&
                bb_engine_loop_ctl_get(0, L2C_SLICE) != SLICE[0],
                "the bank really was wiped before the reload");
    test_expect(bb_config_load() == 1, "a session with a loop bank loads");

    test_expect(bb_engine_loop_ctl_get(0, L2C_BARS)     == 3 &&
                bb_engine_loop_ctl_get(0, L2C_LEVEL)    == 137 &&
                bb_engine_loop_ctl_get(0, L2C_FEEDBACK) == 91 &&
                bb_engine_loop_ctl_get(0, L2C_OVERDUB)  == 1 &&
                bb_engine_loop_ctl_get(0, L2C_RATE)     == LOOP_RATE_DOUBLE &&
                bb_engine_loop_ctl_get(0, L2C_REVERSE)  == 1 &&
                bb_engine_loop_ctl_get(0, L2C_SLICE)    == SLICE[0] &&
                bb_engine_loop_ctl_get(0, L2C_LANE)     == 7,
                "slot 0 round-trips through the legacy phrase-looper storage");
    test_expect(atomic_load(&bb.loop_bars) == 3 &&
                atomic_load(&bb.loop_mix) == 137 &&
                atomic_load(&bb.loop_slice) == SLICE[0],
                "and lands on the legacy atomics, not on a shadow copy");

    int diff = 0;
    for (int n = 1; n < BB_NLOOP; n++) {
        diff += bb_engine_loop_ctl_get(n, L2C_SRC)      != SRC[n];
        diff += bb_engine_loop_ctl_get(n, L2C_BARS)     != n;
        diff += bb_engine_loop_ctl_get(n, L2C_LEVEL)    != 100 + n * 10;
        diff += bb_engine_loop_ctl_get(n, L2C_FEEDBACK) != 50 + n;
        diff += bb_engine_loop_ctl_get(n, L2C_OVERDUB)  != (n & 1);
        diff += bb_engine_loop_ctl_get(n, L2C_RATE)     != n % 3;
        diff += bb_engine_loop_ctl_get(n, L2C_REVERSE)  != !(n & 1);
        diff += bb_engine_loop_ctl_get(n, L2C_SLICE)    != SLICE[n];
        diff += bb_engine_loop_ctl_get(n, L2C_MUTE)     != (n == 4);
        diff += bb_engine_loop_ctl_get(n, L2C_LANE)     != (n * 2) % ARR_LANES;
    }
    test_expect(diff == 0,
                "every satellite's ten controls survive a session round-trip");
    test_expect(bb_engine_loop_frames(1) == 0u,
                "no loop AUDIO is persisted, and none is invented on load");

    /* ---- a session written before the loop bank existed -----------------
     * This is the case every session on disk is in: a `looper` line and no
     * `loopn` lines at all. Slot 0 has to come back exactly as that line
     * describes it, and the satellites have to be at their defaults rather
     * than carrying the previous song's routing. Binary mode so the bytes are
     * identical on every platform, like the v3 and pre-return-bus fixtures. */
    f = bb_fopen(bb_config_path(), "wb");
    if (f) {
        fputs("version 7\n"
              "rate 22050\n"
              "gain 180\n"
              "looper 2 199 88 1 0 1 16\n"
              "layer 0 on 1 mode 2 seq 0\n"
              "expr 0 t*p0\n", f);
        fclose(f);
    }
    test_expect(f != NULL, "a pre-loop-bank session fixture can be written");
    lb_scene();
    bb_engine_loop_ctl(3, L2C_LEVEL, 11);        /* stale state to be reset */
    bb_engine_loop_ctl(3, L2C_SRC, BB_LOOP_SRC_DRY);
    test_expect(bb_config_load() == 1, "a pre-loop-bank session still loads");
    test_expect(atomic_load(&bb.loop_bars) == 2 &&
                atomic_load(&bb.loop_mix) == 199 &&
                atomic_load(&bb.loop_feedback) == 88 &&
                atomic_load(&bb.loop_overdub) == 1 &&
                atomic_load(&bb.loop_rate) == LOOP_RATE_HALF &&
                atomic_load(&bb.loop_reverse) == 1 &&
                atomic_load(&bb.loop_slice) == 16,
                "an old session's looper line still lands on slot 0");
    int fresh = 1;
    for (int n = 1; n < BB_NLOOP; n++)
        fresh &= bb_engine_loop_ctl_get(n, L2C_SRC)   == BB_LOOP_SRC_LIVE &&
                 bb_engine_loop_ctl_get(n, L2C_BARS)  == 0 &&
                 bb_engine_loop_ctl_get(n, L2C_LEVEL) == 200 &&
                 bb_engine_loop_ctl_get(n, L2C_LANE)  == n;
    test_expect(fresh,
                "a session with no loopn lines resets the satellites instead "
                "of inheriting the last song's");

    /* ---- a hand-edited session, and a truncated line -------------------- */
    f = bb_fopen(bb_config_path(), "wb");
    if (f) {
        fputs("version 7\n"
              "rate 8000\n"
              "loopn 1 10 3 999 -5 1 9 1 7 1 99\n"
              "loopn 2 10\n"
              "loopn 0 11 9 100 100 0 0 0 1 1 2\r\n", f);
        fclose(f);
    }
    test_expect(f != NULL, "a hand-edited loop-bank fixture can be written");
    lb_scene();
    test_expect(bb_config_load() == 1, "a hand-edited loop-bank session loads");
    test_expect(bb_engine_loop_ctl_get(1, L2C_LEVEL)    == 256 &&
                bb_engine_loop_ctl_get(1, L2C_FEEDBACK) == 0 &&
                bb_engine_loop_ctl_get(1, L2C_RATE)     == LOOP_RATE_DOUBLE &&
                bb_engine_loop_ctl_get(1, L2C_SLICE)    == 1 &&
                bb_engine_loop_ctl_get(1, L2C_LANE)     == ARR_LANES - 1 &&
                bb_engine_loop_ctl_get(1, L2C_BARS)     == 3,
                "a hand-edited session is clamped on the way in, not in the DSP");
    test_expect(bb_engine_loop_ctl_get(2, L2C_LEVEL) == 200 &&
                bb_engine_loop_ctl_get(2, L2C_SRC) == BB_LOOP_SRC_LIVE,
                "a short loopn line is skipped whole rather than half-applied");
    test_expect(loop_ctl_load(0, L2C_SRC) == BB_LOOP_SRC_MASTER &&
                loop_ctl_load(0, L2C_MUTE) == 0 &&
                atomic_load(&bb.loop_bars) == 4 &&
                atomic_load(&bb.loop_slice) == 1,
                "loopn 0 cannot give slot 0 a source or a mute, and its bars "
                "are clamped to the phrase looper's range");
    test_expect(bb_engine_loop_ctl_get(0, L2C_LANE) == 2,
                "slot 0's ARRANGE lane survives CRLF line endings");

    bb_remove(bb_config_path());
    tmp_dir_remove(root);

    bb_engine_set_defaults();
    bb_engine_reclaim();
}

#endif /* BB_NLOOP */

static void test_loop_bank(void)
{
#if defined(BB_NLOOP)
    test_loop_defaults();
    test_loop_survivor_exact();
    test_loop_bypass();
    test_loop_capture();
    test_loop_layering();
    test_loop_overdub();
    test_loop_slots();
    test_loop_quantum();
    test_loop_transport();
    test_loop_commit();
    test_loop_session();

    /* leave the engine the way an empty session would */
    bb_engine_set_defaults();
    bb_engine_init(44100);
    bb_engine_reclaim();
#endif
}

static int self_test_mode(void)
{
    test_checks = test_failures = 0;
    puts("bytebeat self-test");

    test_expression_vm();
    test_rack_and_generator();

    /* main.c called audio_self_test() here. That function (audio.c:414) does
     * nothing but forward to bb_engine_self_test(): the clock and phrase-loop
     * invariants moved into the engine so the GUI test runner could reach
     * them without ALSA. Calling the engine entry point directly runs exactly
     * the same checks and keeps this target free of any sound-card library,
     * which is the whole reason it exists. */
    char err[160] = "";
    test_expect(bb_engine_self_test(err, sizeof err),
                "audio clock/phrase invariants: %s", err);
    test_step_sampler();
    test_session_roundtrip();
    test_arrangement();

    /* Everything above this line is the suite exactly as it ran under the
     * terminal driver, in the same order with the same wording, so its count
     * is directly comparable to the historical numbers. Everything below it
     * is new and exists only because of the port -- reported separately so a
     * changed total never has to be guessed at. */
    int historical = test_checks;
    test_port_paths();
    test_packed_position();
    test_arr_transport_and_rec_src();

    /* The return bus is counted separately again, and for the same reason:
     * the golden CHAMBER hash inside it is the one check whose whole job is
     * to notice a change nobody meant to make, so it must never be able to
     * hide inside a total that moved for an unrelated reason. */
    int port = test_checks - historical;
    test_return_bus();

    /* And the loop bank is counted separately again, for the third time and
     * the same reason: the sample-exact SURVIVOR check inside it exists to
     * notice a change nobody meant to make, so it must never be able to hide
     * inside a total that moved for an unrelated reason. */
    int retbus = test_checks - historical - port;
    test_loop_bank();

    if (test_failures) {
        fprintf(stderr, "%d of %d checks failed\n", test_failures, test_checks);
        return 1;
    }
    printf("%d historical checks, %d port checks, %d return-bus checks, "
           "%d loop-bank checks\n",
           historical, port, retbus,
           test_checks - historical - port - retbus);
    printf("all %d checks passed (%d sources, session v3/v4)\n",
           test_checks, rack_nsrc());
    return 0;
}

int main(int argc, char **argv)
{
    /* The suite takes no arguments; -T is accepted so muscle memory and any
     * existing CI invocation carried over from `bytebeat -T` keeps working. */
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-T") || !strcmp(argv[i], "--self-test")) continue;
        fprintf(stderr, "usage: %s [-T]\n", argv[0]);
        return 2;
    }
    return self_test_mode();
}
