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

    if (test_failures) {
        fprintf(stderr, "%d of %d checks failed\n", test_failures, test_checks);
        return 1;
    }
    printf("%d historical checks, %d port checks\n",
           historical, test_checks - historical);
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
