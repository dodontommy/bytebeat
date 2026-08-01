/* audio.c -- raw ALSA, and the one thread that has a deadline.
 *
 * ------------------------------------------------------------------------
 * HOW SOUND ACTUALLY LEAVES THIS PROGRAM
 *
 * The sound card owns a chunk of memory called the RING BUFFER. Hardware
 * walks it at a constant rate -- one frame every 1/sample_rate seconds --
 * converting each frame to a voltage and never, ever pausing. Our only job
 * is to keep writing new frames in ahead of that read pointer. If we fall
 * behind, the hardware reads whatever stale bytes are there and you hear a
 * click, a buzz, or a repeat of the last fragment. That is an XRUN (buffer
 * underrun on playback).
 *
 * The buffer is divided into PERIODS. The card raises an interrupt each time
 * it finishes one, and that interrupt is what wakes us up. So:
 *
 *   period size  = how much we hand over at a time
 *                = how often we get woken
 *                = OUR LATENCY. A keypress cannot affect sound that is
 *                  already sitting in the buffer, so the delay between a
 *                  knob turn and hearing it is roughly one period.
 *
 *   buffer size  = how much runway we have before an xrun
 *                = period_size * period_count
 *
 * These pull in opposite directions and that tension is the whole of
 * real-time audio: small periods mean responsive knobs and frequent
 * deadlines you can miss; large periods mean a sloppy instrument that never
 * glitches. We take 4 periods of ~10ms, which is about 40ms of runway and
 * ~10ms of latency -- tight enough to perform with, forgiving enough to
 * survive a laptop deciding to index something.
 * ------------------------------------------------------------------------
 */

#include "bytebeat.h"
#include "audio.h"
#include "dsp.h"

#include <alsa/asoundlib.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>

/* ---- device state (UI thread owns configuration, audio thread owns use) */
static snd_pcm_t         *g_pcm;
static char               g_dev[64] = "default";
static int                g_resample = 1;
static snd_pcm_uframes_t  g_period   = 512;
static snd_pcm_uframes_t  g_bufsize  = 2048;
static unsigned           g_chans    = 1;

static pthread_t          g_thread;
static int                g_thread_live;

/* ---- park handshake: the ONLY cross-thread control flow in the program */
static atomic_int         g_park_req;
static atomic_int         g_parked;

/* ---- DSP state owned exclusively by the audio thread -------------------
 * These are big and they are allocated statically. Not because static is
 * elegant, but because the audio thread must never call malloc, and
 * mlockall(MCL_CURRENT) pins the whole BSS into RAM in one go so none of it
 * can ever be paged out mid-period. */
/* Per-layer DSP state. Eight fully independent voices: each gets its own
 * expression delay line, its own filter and register state, and its own post
 * chain. That is ~8MB of statically allocated buffers, and it is the price of
 * layers that genuinely do not interfere with each other. Paid once at
 * startup, so the audio thread still never allocates. */
static int32_t   g_delay[BB_NLAYER][EXPR_DELAY_LEN];
static int32_t   g_space[BB_NLAYER][BB_SPACE_LEN];
static ExprCtx   g_ctx[BB_NLAYER];
static PostState g_post[BB_NLAYER];

/* Per-layer running state that must survive across periods. */
static uint64_t  g_vt[BB_NLAYER];        /* voice clock, Q32           */
static int32_t   g_env[BB_NLAYER];       /* gate envelope, Q16         */
static int       g_env_atk[BB_NLAYER];
static int32_t   g_env_peak[BB_NLAYER];
static int       g_last_step[BB_NLAYER];
static uint32_t  g_last_tick[BB_NLAYER];
static int       g_last_sub[BB_NLAYER];
static int       g_step_fire[BB_NLAYER];
static uint32_t  g_seq_rng[BB_NLAYER];
static int32_t   g_hit_age[BB_NLAYER];
static int32_t   g_hit_vel[BB_NLAYER];
static int32_t   g_lvl[BB_NLAYER];       /* smoothed mix level, Q16    */

/* Master phrase looper. The buffer and all cursors are owned by the audio
 * thread; the UI only touches the atomic controls in bb. */
static int16_t   g_loop_buf[BB_LOOP_LEN];
static uint32_t  g_loop_len;
static uint32_t  g_loop_write;
static uint32_t  g_loop_target;
static uint32_t  g_loop_slice_start;
static uint32_t  g_loop_pub_pos;
static uint64_t  g_loop_phase;            /* Q32 frames within slice */
static int32_t   g_loop_wet;              /* Q16 click-free crossfade */
static int       g_loop_state;
static int       g_loop_last_slice = 1;

/* Semitone -> playback-rate ratio in Q32 fixed point, index = semitones+12.
 *
 * Transposing bytebeat is unusual: there is no oscillator to retune. But
 * every frequency in the output is derived from `t`, so advancing `t` at
 * 2x speed moves EVERYTHING up an octave -- pitch, rhythm within the
 * expression, all of it. That is what the sequencer's pitch lane does, and
 * it works on any expression ever written without editing it.
 *
 * At offset 0 the ratio is exactly 1<<32, so the voice clock advances by
 * precisely one per sample and is bit-identical to plain `t`. Switching the
 * sequencer off therefore changes nothing at all. */
static const uint64_t PITCH_Q32[25] = {
    2147483648ULL, 2275179671ULL, 2410468894ULL, 2553802834ULL,
    2705659104ULL, 2866542664ULL, 3036987106ULL, 3217556019ULL,
    3408844448ULL, 3611480456ULL, 3826126808ULL, 4053482775ULL,
    4294967296ULL,                                     /* 0 semitones = 1.0 */
    4550359342ULL, 4820937789ULL, 5107665669ULL, 5411318208ULL,
    5733085329ULL, 6073974212ULL, 6435112038ULL, 6817688895ULL,
    7222960912ULL, 7652253616ULL, 8107818609ULL, 8589934592ULL,
};

/* ======================================================================== */
/*  Device configuration                                                    */
/* ======================================================================== */

/* Everything that touches snd_pcm_hw_params lives here. It is called from
 * the UI thread only: ALSA's parameter-negotiation code allocates, and
 * allocation on the audio thread is exactly what we are trying to avoid.
 *
 * snd_pcm_hw_params_alloca() is not a malloc -- it is alloca(), so the
 * params object lives on this function's stack frame and evaporates on
 * return. Worth knowing: it is the reason you must never let a
 * snd_pcm_hw_params_t* escape the function that allocated it. */
static int configure(int want_rate, char *err, size_t errsz)
{
    snd_pcm_hw_params_t *hw;
    snd_pcm_sw_params_t *sw;
    int rc;
    unsigned rate = (unsigned)want_rate;

#define FAIL(what, code) do { \
        snprintf(err, errsz, "%s: %s", (what), snd_strerror(code)); \
        return -1; \
    } while (0)

    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_sw_params_alloca(&sw);

    if ((rc = snd_pcm_hw_params_any(g_pcm, hw)) < 0)
        FAIL("no configurations available", rc);

    /* Software resampling. Leaving it ON is what lets you ask a 48kHz-only
     * card for 1000Hz and get something. Turning it OFF (-R) makes the
     * device refuse rates it cannot do natively, which is the honest way to
     * hear genuine low-rate aliasing rather than ALSA's interpolation of it. */
    if ((rc = snd_pcm_hw_params_set_rate_resample(g_pcm, hw,
                                                  g_resample ? 1u : 0u)) < 0)
        FAIL("set_rate_resample", rc);

    /* RW_INTERLEAVED: we hand ALSA a normal array and it copies. The
     * alternative (MMAP) hands us a pointer into the card's ring so we write
     * samples directly where the hardware will read them -- faster, but it
     * makes the code about the DMA layout instead of about the instrument. */
    if ((rc = snd_pcm_hw_params_set_access(g_pcm, hw,
                                           SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
        FAIL("set_access", rc);

    /* We always feed the device signed 16-bit little-endian, regardless of
     * the instrument's BYTE/SIGNED/WORD mode. The mode changes how the
     * expression's int32 result is folded down to a sample value -- that is
     * a musical decision. What the wire format is is a hardware decision,
     * and S16_LE is the one format every card and every plugin supports. */
    if ((rc = snd_pcm_hw_params_set_format(g_pcm, hw,
                                           SND_PCM_FORMAT_S16_LE)) < 0)
        FAIL("set_format S16_LE", rc);

    /* Prefer mono; fall back to stereo and duplicate. Plenty of HDA codecs
     * and most PipeWire/Pulse ALSA bridges only expose stereo. */
    g_chans = 1;
    if (snd_pcm_hw_params_set_channels(g_pcm, hw, 1) < 0) {
        g_chans = 2;
        if ((rc = snd_pcm_hw_params_set_channels(g_pcm, hw, 2)) < 0)
            FAIL("set_channels", rc);
    }

    /* _near, not _exact: ask for what you want, accept what exists, and then
     * BELIEVE THE ANSWER. `rate` is modified in place. Using the requested
     * value afterwards instead of the granted one is the classic way to end
     * up with an instrument that is subtly out of tune with itself. */
    if ((rc = snd_pcm_hw_params_set_rate_near(g_pcm, hw, &rate, 0)) < 0)
        FAIL("set_rate_near", rc);

    /* Target ~10ms of latency, clamped so the audio thread's stack buffer is
     * always big enough. At 1000Hz that clamp means 64 frames = 64ms, which
     * is fine; nobody is playing fast at 1kHz. */
    snd_pcm_uframes_t period = rate / 100;
    if (period < 64)            period = 64;
    if (period > BB_MAX_PERIOD) period = BB_MAX_PERIOD;
    if ((rc = snd_pcm_hw_params_set_period_size_near(g_pcm, hw, &period, 0)) < 0)
        FAIL("set_period_size_near", rc);

    unsigned periods = 4;
    if ((rc = snd_pcm_hw_params_set_periods_near(g_pcm, hw, &periods, 0)) < 0)
        FAIL("set_periods_near", rc);

    if ((rc = snd_pcm_hw_params(g_pcm, hw)) < 0)
        FAIL("hw_params commit", rc);

    snd_pcm_hw_params_get_period_size(hw, &g_period, 0);
    snd_pcm_hw_params_get_buffer_size(hw, &g_bufsize);
    if (g_period > BB_MAX_PERIOD) g_period = BB_MAX_PERIOD;

    /* Software params.
     *   start_threshold = buffer size -> the card does not start converting
     *     until we have filled the whole buffer once. Starting on a
     *     half-empty buffer is a guaranteed immediate xrun.
     *   avail_min = period -> wake us when one period of space exists. */
    if ((rc = snd_pcm_sw_params_current(g_pcm, sw)) < 0)
        FAIL("sw_params_current", rc);
    if ((rc = snd_pcm_sw_params_set_start_threshold(g_pcm, sw, g_bufsize)) < 0)
        FAIL("set_start_threshold", rc);
    if ((rc = snd_pcm_sw_params_set_avail_min(g_pcm, sw, g_period)) < 0)
        FAIL("set_avail_min", rc);
    if ((rc = snd_pcm_sw_params(g_pcm, sw)) < 0)
        FAIL("sw_params commit", rc);

    if ((rc = snd_pcm_prepare(g_pcm)) < 0)
        FAIL("prepare", rc);

    atomic_store(&bb.rate, (int)rate);
    return 0;
#undef FAIL
}

int audio_open(const char *device, int rate, int allow_resample,
               char *err, size_t errsz)
{
    int rc;

    snprintf(g_dev, sizeof g_dev, "%s", device ? device : "default");
    g_resample = allow_resample;

    /* Wire up the audio thread's private DSP state. Done once, here, so the
     * audio thread never has to. */
    for (int L = 0; L < BB_NLAYER; L++) {
        memset(&g_ctx[L], 0, sizeof g_ctx[L]);
        g_ctx[L].dly = g_delay[L];
        /* Each layer's PRNG gets a different seed, otherwise two layers both
         * using `r` would produce identical noise and sum to a correlated
         * 6dB-louder version of one layer instead of a wider bed. */
        g_ctx[L].rng = 0x1234567u + (uint32_t)L * 0x9E3779B9u;
        post_init(&g_post[L], g_space[L], BB_SPACE_LEN);
        g_env[L]       = 65536;
        g_env_peak[L]  = 65536;
        g_last_step[L] = -1;
        g_last_tick[L] = UINT32_MAX;
        g_last_sub[L]  = -1;
        g_step_fire[L] = 0;
        g_seq_rng[L]   = 0xA341316Cu + (uint32_t)L * 0x9E3779B9u;
        g_hit_age[L]   = INT32_MAX;
        g_hit_vel[L]   = 256;
    }

    g_loop_len = g_loop_write = g_loop_target = 0;
    g_loop_slice_start = g_loop_pub_pos = 0;
    g_loop_phase = 0;
    g_loop_wet = 0;
    g_loop_state = LOOP_OFF;
    g_loop_last_slice = 1;
    atomic_store(&bb.loop_status, LOOP_OFF);
    atomic_store(&bb.loop_frames, 0);
    atomic_store(&bb.loop_pos, 0);

    if (strcmp(g_dev, "none") == 0) {
        /* Headless: no card at all. The audio thread will pace itself off
         * CLOCK_MONOTONIC and everything downstream (scope, .wav, TCP
         * stream) works exactly as it does with a device. */
        g_pcm = NULL;
        g_chans = 1;
        atomic_store(&bb.rate, bb_clampi(rate, BB_RATE_MIN, BB_RATE_MAX));
        g_period  = (snd_pcm_uframes_t)bb_clampi(rate / 100, 64, BB_MAX_PERIOD);
        g_bufsize = g_period * 4;
        return 0;
    }

    /* Blocking mode. snd_pcm_writei() will then sleep until there is room,
     * which is precisely the wakeup we want -- the sound card's own clock
     * becomes our scheduler and we never have to poll or sleep on a timer. */
    if ((rc = snd_pcm_open(&g_pcm, g_dev, SND_PCM_STREAM_PLAYBACK, 0)) < 0) {
        snprintf(err, errsz, "snd_pcm_open(\"%s\"): %s", g_dev, snd_strerror(rc));
        g_pcm = NULL;
        return -1;
    }

    if (configure(rate, err, errsz) < 0) {
        snd_pcm_close(g_pcm);
        g_pcm = NULL;
        return -1;
    }
    return 0;
}

void audio_close(void)
{
    if (g_pcm) {
        snd_pcm_drop(g_pcm);
        snd_pcm_close(g_pcm);
        g_pcm = NULL;
    }
    /* ALSA caches configuration in a global. Freeing it keeps valgrind quiet
     * and costs nothing. */
    snd_config_update_free_global();
}

const char *audio_device_name(void) { return g_dev; }
int audio_period_frames(void)       { return (int)g_period; }
int audio_buffer_frames(void)       { return (int)g_bufsize; }
int audio_channels(void)            { return (int)g_chans; }
int audio_have_device(void)         { return g_pcm != NULL; }

/* ======================================================================== */
/*  Park handshake                                                          */
/* ======================================================================== */

/* Retuning the PCM device means calling snd_pcm_hw_params again, and that
 * allocates. We refuse to do that on the audio thread, so instead the UI
 * thread asks the audio thread to stand still while it does the work.
 *
 * The audio thread checks g_park_req at the top of each period -- i.e. while
 * it is NOT inside snd_pcm_writei and NOT touching the handle -- sets
 * g_parked, and spins on a 200us sleep until released. Because that spin
 * only ever happens when there is no stream running, there is no deadline to
 * miss and the sleep is harmless.
 *
 * Note there is no mutex here. There does not need to be: exactly one thread
 * writes each flag, and the acquire/release ordering on the two atomics is
 * enough to guarantee the UI thread's device writes happen strictly between
 * the audio thread's last and next uses of the handle. */
static void park_sleep(long usec)
{
    struct timespec ts;
    ts.tv_sec  = 0;
    ts.tv_nsec = usec * 1000L;
    nanosleep(&ts, NULL);
}

static int audio_park(void)
{
    if (!g_thread_live) return 0;

    atomic_store_explicit(&g_park_req, 1, memory_order_release);

    /* Bounded wait. If the audio thread has died for some reason we must not
     * hang the UI forever -- 2 seconds is far longer than the longest
     * possible period (2048 frames at 1000Hz is 2.0s, so: exactly one worst
     * case period plus slack). */
    for (int i = 0; i < 4000; i++) {
        if (atomic_load_explicit(&g_parked, memory_order_acquire)) return 0;
        if (!atomic_load(&bb.running)) return 0;
        park_sleep(1000);
    }
    atomic_store(&g_park_req, 0);
    return -1;
}

static void audio_unpark(void)
{
    atomic_store_explicit(&g_park_req, 0, memory_order_release);
}

int audio_retune(int rate, char *err, size_t errsz)
{
    rate = bb_clampi(rate, BB_RATE_MIN, BB_RATE_MAX);
    atomic_store(&bb.req_rate, rate);

    if (!g_pcm) {                    /* no device: the rate is just a number */
        atomic_store(&bb.rate, rate);
        g_period  = (snd_pcm_uframes_t)bb_clampi(rate / 100, 64, BB_MAX_PERIOD);
        g_bufsize = g_period * 4;
        return 0;
    }

    int old = atomic_load(&bb.rate);

    if (audio_park() < 0) {
        snprintf(err, errsz, "audio thread did not park; rate unchanged");
        return -1;
    }

    /* drop, not drain: drain waits for the buffer to play out, which is the
     * wrong thing when the user is sweeping the rate knob. Throwing away the
     * queued audio is the responsive choice, and it is why a rate change
     * makes a small click rather than a lag. */
    snd_pcm_drop(g_pcm);
    snd_pcm_hw_free(g_pcm);

    int rc = configure(rate, err, errsz);
    if (rc < 0) {
        /* Put it back the way it was. If even that fails the stream is gone
         * and there is nothing honest left to do but say so. */
        char err2[128];
        if (configure(old, err2, sizeof err2) < 0) {
            snprintf(err, errsz, "retune failed AND restore failed: %s", err2);
            audio_unpark();
            return -1;
        }
    }
    audio_unpark();
    return rc;
}

/* ======================================================================== */
/*  xrun recovery                                                           */
/* ======================================================================== */

/* -EPIPE from writei means the hardware ran out of data while we were away:
 * an underrun. The stream is now in the XRUN state and will not accept
 * anything until it is prepared again. There is nothing to "fix" -- the gap
 * has already been heard. All we can do is count it and restart, which is
 * why the xrun counter on the display is a performance metric, not an error
 * log: if it climbs while you are sweeping a knob, your period is too small
 * or your expression is too expensive. */
static int recover(snd_pcm_t *h, int e)
{
    if (e == -EPIPE) {
        atomic_fetch_add(&bb.xruns, 1);
        return snd_pcm_prepare(h);
    }
    if (e == -ESTRPIPE) {
        /* The machine suspended. Wait for the device to come back. */
        int rc;
        while ((rc = snd_pcm_resume(h)) == -EAGAIN) park_sleep(10000);
        if (rc < 0) return snd_pcm_prepare(h);
        return 0;
    }
    return snd_pcm_prepare(h);
}

/* ======================================================================== */
/*  The audio thread                                                        */
/* ======================================================================== */

static inline void scope_push(int16_t v)
{
    unsigned w = atomic_load_explicit(&bb.scope_w, memory_order_relaxed);
    bb.scope[w & BB_SCOPE_MASK] = v;
    atomic_store_explicit(&bb.scope_w, w + 1u, memory_order_release);
}

static inline void sink_push(int16_t v)
{
    unsigned w = atomic_load_explicit(&bb.sink_w, memory_order_relaxed);
    bb.sink[w & BB_SINK_MASK] = v;
    atomic_store_explicit(&bb.sink_w, w + 1u, memory_order_release);
}

/* One layer's controls, snapshotted once per period. Taking a copy means
 * every sample in a buffer sees a consistent set of values, and it keeps the
 * inner loop reading plain ints instead of atomics. */
typedef struct {
    const Program *prog;
    int        on, mode, level;
    int        p[BB_NPARAM];
    PostParams pp;
    int        spc_time_raw, spc_sync;
    int        seq_on, steps;
    int        decay;
    int        gate[BB_STEPS];
    int        pitch[BB_STEPS];
    int        ratchet[BB_STEPS];
    int        prob[BB_STEPS];
    int        lock[BB_LOCK_COUNT][BB_STEPS];
    unsigned   motion_mask;
} LSnap;

static inline uint32_t seq_rand(int L)
{
    uint32_t x = g_seq_rng[L];
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    g_seq_rng[L] = x ? x : 0xA341316Cu;
    return g_seq_rng[L];
}

static inline int32_t decay_coeff(int raw)
{
    raw = bb_clampi(raw, 0, 255);
    return raw > 0 ? 1 + (raw * raw) / 108 : 0;
}

/* Clock divisions are relative to a quarter-note beat. Index zero retains
 * the old free-millisecond knob; the remaining values are 1/32, 1/16T,
 * 1/16, 1/8T, 1/8, 1/4T, 1/4, 1/2, one bar and two bars. */
static int space_samples(int raw, int sync, uint32_t beat_len,
                         uint32_t bar_len, int rate)
{
    uint64_t smp;
    switch (sync) {
    case 1:  smp = beat_len / 8u; break;
    case 2:  smp = beat_len / 6u; break;
    case 3:  smp = beat_len / 4u; break;
    case 4:  smp = beat_len / 3u; break;
    case 5:  smp = beat_len / 2u; break;
    case 6:  smp = (uint64_t)beat_len * 2u / 3u; break;
    case 7:  smp = beat_len; break;
    case 8:  smp = (uint64_t)beat_len * 2u; break;
    case 9:  smp = bar_len; break;
    case 10: smp = (uint64_t)bar_len * 2u; break;
    default: {
        int ms = 20 + bb_clampi(raw, 0, 255) * 3;
        smp = (uint64_t)ms * (uint64_t)(rate > 0 ? rate : 44100) / 1000u;
        break;
    }
    }
    if (smp < 1) smp = 1;
    if (smp > BB_SPACE_MASK) smp = BB_SPACE_MASK;
    return (int)smp;
}

/* Read one lock lane. Manually placed locks are steps; lanes marked as
 * motion interpolate to the next locked point across the 16th note. */
static inline int locked(const LSnap *sn, int target, int step, int base,
                         uint32_t in_step, uint32_t step_len)
{
    if (!sn->seq_on) return base;
    int a = sn->lock[target][step];
    if (a < 0) return base;
    if (!((sn->motion_mask >> target) & 1u) || step_len < 2) return a;

    int next = (step + 1) % sn->steps;
    int b = sn->lock[target][next];
    if (b < 0) b = a;
    return a + (int)(((int64_t)(b - a) * in_step) / step_len);
}

static void loop_state(int state)
{
    g_loop_state = state;
    atomic_store_explicit(&bb.loop_status, state, memory_order_relaxed);
}

static void loop_command(void)
{
    int cmd = atomic_exchange_explicit(&bb.loop_cmd, LOOP_CMD_NONE,
                                       memory_order_relaxed);
    if (cmd == LOOP_CMD_CLEAR) {
        g_loop_len = g_loop_write = g_loop_target = 0;
        g_loop_phase = 0;
        g_loop_wet = 0;
        loop_state(LOOP_OFF);
        atomic_store_explicit(&bb.loop_frames, 0, memory_order_relaxed);
    } else if (cmd == LOOP_CMD_ARM) {
        loop_state(LOOP_ARMED);
    } else if (cmd == LOOP_CMD_PLAY) {
        if (g_loop_state == LOOP_PLAYING) loop_state(LOOP_OFF);
        else if (g_loop_len)              loop_state(LOOP_PLAYING);
        else                              loop_state(LOOP_ARMED);
    }
}

static int32_t loop_process(int32_t live, uint32_t bar_pos, uint32_t bar_len,
                            int bars, int mix, int feedback, int overdub,
                            int rate_mode, int reverse, int slice)
{
    if (g_loop_state == LOOP_ARMED && bar_pos == 0) {
        uint64_t wanted = (uint64_t)bar_len * (uint64_t)bb_clampi(bars, 1, 4);
        if (wanted < 1) wanted = 1;
        if (wanted > BB_LOOP_LEN) wanted = BB_LOOP_LEN;
        g_loop_target = (uint32_t)wanted;
        g_loop_write = 0;
        g_loop_wet = 0;
        loop_state(LOOP_RECORDING);
    }

    if (g_loop_state == LOOP_RECORDING) {
        g_loop_buf[g_loop_write++] = (int16_t)dsp_clip16(live);
        if (g_loop_write >= g_loop_target) {
            g_loop_len = g_loop_write;
            g_loop_phase = 0;
            g_loop_slice_start = 0;
            g_loop_pub_pos = 0;
            g_loop_last_slice = 1;
            atomic_store_explicit(&bb.loop_frames, g_loop_len, memory_order_relaxed);
            loop_state(LOOP_PLAYING);
        }
        return live;
    }

    int32_t wet_target = g_loop_state == LOOP_PLAYING
                       ? bb_clampi(mix, 0, 256) << 8 : 0;
    if (g_loop_wet < wet_target) {
        g_loop_wet += 128; if (g_loop_wet > wet_target) g_loop_wet = wet_target;
    } else if (g_loop_wet > wet_target) {
        g_loop_wet -= 128; if (g_loop_wet < wet_target) g_loop_wet = wet_target;
    }
    if (!g_loop_len || (g_loop_state != LOOP_PLAYING && g_loop_wet == 0))
        return live;

    if (slice != 1 && slice != 2 && slice != 4 && slice != 8 && slice != 16)
        slice = 1;
    if (slice != g_loop_last_slice) {
        g_loop_slice_start = slice == 1 ? 0 : g_loop_pub_pos;
        g_loop_phase = 0;
        g_loop_last_slice = slice;
    }

    uint32_t slen = g_loop_len / (uint32_t)slice;
    if (slen < 1) slen = 1;
    uint32_t off = (uint32_t)(g_loop_phase >> 32) % slen;
    uint32_t rel = reverse ? (slen - 1u - off) : off;
    uint32_t idx = (g_loop_slice_start + rel) % g_loop_len;
    int32_t played = g_loop_buf[idx];
    g_loop_pub_pos = idx;

    if (g_loop_state == LOOP_PLAYING && overdub) {
        int32_t nw = live + (int32_t)(((int64_t)played *
                                      bb_clampi(feedback, 0, 256)) >> 8);
        g_loop_buf[idx] = (int16_t)dsp_clip16(nw);
    }

    uint64_t inc = rate_mode == LOOP_RATE_HALF ? (1ULL << 31)
                 : rate_mode == LOOP_RATE_DOUBLE ? (1ULL << 33)
                 : (1ULL << 32);
    g_loop_phase += inc;
    uint64_t span = (uint64_t)slen << 32;
    while (g_loop_phase >= span) g_loop_phase -= span;

    return (int32_t)(((int64_t)live * (65536 - g_loop_wet)
                    + (int64_t)played * g_loop_wet) >> 16);
}

int audio_self_test(char *err, size_t errsz)
{
#define AUDIO_TEST(ok, why) do { \
        if (!(ok)) { snprintf(err, errsz, "%s", (why)); return 0; } \
    } while (0)

    /* Every sync choice must remain inside the fixed SPACE allocation. */
    for (int i = 0; i <= 10; i++) {
        int n = space_samples(200, i, 22050, 88200, 44100);
        AUDIO_TEST(n >= 1 && n <= (int)BB_SPACE_MASK,
                   "clocked SPACE division escaped its buffer");
    }
    AUDIO_TEST(space_samples(0, 7, 12345, 49380, 44100) == 12345,
               "quarter-note SPACE division is not one beat");
    AUDIO_TEST(space_samples(0, 9, 12345, 49380, 44100) == 49380,
               "bar SPACE division is not one bar");

    /* Freeze must recirculate the tap and reject fresh input. */
    int32_t delay[16] = { 0 };
    PostState ps;
    PostParams pp;
    memset(&pp, 0, sizeof pp);
    pp.tone = 255;
    pp.spc_time = 4;
    pp.spc_mix = 256;
    pp.spc_freeze = 1;
    post_init(&ps, delay, 16);
    ps.dw = 4;
    delay[0] = 1234;
    (void)post_process(&ps, 9999, &pp);
    AUDIO_TEST(delay[4] == 1234,
               "frozen SPACE admitted fresh input instead of recirculating");

    memset(delay, 0, sizeof delay);
    post_init(&ps, delay, 16);
    ps.dw = 4;
    delay[0] = 1234;
    pp.spc_freeze = 0;
    (void)post_process(&ps, 9999, &pp);
    AUDIO_TEST(delay[4] == 9999,
               "live SPACE failed to admit fresh input after freeze release");

    /* Capture one tiny synthetic bar, then exercise normal, reverse and
     * overdub reads. This touches the real looper state machine. */
    memset(g_loop_buf, 0, 16 * sizeof g_loop_buf[0]);
    g_loop_len = g_loop_write = g_loop_target = 0;
    g_loop_slice_start = g_loop_pub_pos = 0;
    g_loop_phase = 0;
    g_loop_wet = 0;
    g_loop_last_slice = 1;
    loop_state(LOOP_ARMED);

    AUDIO_TEST(loop_process(777, 3, 8, 1, 256, 192, 0,
                            LOOP_RATE_NORMAL, 0, 1) == 777 &&
               g_loop_state == LOOP_ARMED && g_loop_write == 0,
               "armed phrase capture started away from a bar boundary");

    for (uint32_t i = 0; i < 8; i++) {
        int32_t out = loop_process(1000 + (int32_t)i, i, 8, 1, 256,
                                   192, 0, LOOP_RATE_NORMAL, 0, 1);
        AUDIO_TEST(out == 1000 + (int32_t)i,
                   "phrase capture altered the live bus");
    }
    AUDIO_TEST(g_loop_state == LOOP_PLAYING && g_loop_len == 8,
               "phrase capture did not enter playback at the requested bar");
    AUDIO_TEST(atomic_load(&bb.loop_frames) == 8,
               "phrase length was not published");

    g_loop_wet = 65536;
    g_loop_phase = 0;
    AUDIO_TEST(loop_process(0, 0, 8, 1, 256, 192, 0,
                            LOOP_RATE_NORMAL, 0, 1) == 1000,
               "forward phrase playback did not start at frame zero");

    g_loop_phase = 0;
    int32_t half0 = loop_process(0, 0, 8, 1, 256, 192, 0,
                                 LOOP_RATE_HALF, 0, 1);
    int32_t half1 = loop_process(0, 0, 8, 1, 256, 192, 0,
                                 LOOP_RATE_HALF, 0, 1);
    int32_t half2 = loop_process(0, 0, 8, 1, 256, 192, 0,
                                 LOOP_RATE_HALF, 0, 1);
    AUDIO_TEST(half0 == 1000 && half1 == 1000 && half2 == 1001,
               "half-speed phrase playback did not advance at 1/2x");

    g_loop_phase = 0;
    int32_t dbl0 = loop_process(0, 0, 8, 1, 256, 192, 0,
                                LOOP_RATE_DOUBLE, 0, 1);
    int32_t dbl1 = loop_process(0, 0, 8, 1, 256, 192, 0,
                                LOOP_RATE_DOUBLE, 0, 1);
    AUDIO_TEST(dbl0 == 1000 && dbl1 == 1002,
               "double-speed phrase playback did not advance at 2x");

    g_loop_phase = 0;
    g_loop_pub_pos = 0;
    g_loop_last_slice = 1;
    int32_t sliced[5];
    for (int i = 0; i < 5; i++)
        sliced[i] = loop_process(0, 0, 8, 1, 256, 192, 0,
                                 LOOP_RATE_NORMAL, 0, 2);
    AUDIO_TEST(sliced[0] == 1000 && sliced[1] == 1001 &&
               sliced[2] == 1002 && sliced[3] == 1003 && sliced[4] == 1000,
               "phrase slice did not repeat the selected fraction");

    g_loop_phase = 0;
    g_loop_last_slice = 1;
    g_loop_slice_start = 0;
    AUDIO_TEST(loop_process(0, 0, 8, 1, 256, 192, 0,
                            LOOP_RATE_NORMAL, 1, 1) == 1007,
               "reverse phrase playback did not start at the final frame");

    g_loop_phase = 0;
    g_loop_slice_start = 0;
    g_loop_last_slice = 1;
    AUDIO_TEST(loop_process(200, 0, 8, 1, 256, 128, 1,
                            LOOP_RATE_NORMAL, 0, 1) == 1000,
               "overdub changed the currently playing frame");
    AUDIO_TEST(g_loop_buf[0] == 700,
               "overdub feedback did not write live plus retained phrase");

    atomic_store(&bb.loop_cmd, LOOP_CMD_CLEAR);
    loop_command();
    AUDIO_TEST(g_loop_state == LOOP_OFF && g_loop_len == 0 &&
               atomic_load(&bb.loop_frames) == 0,
               "clear did not reset the phrase state");

    err[0] = '\0';
    return 1;
#undef AUDIO_TEST
}

static void *audio_thread(void *arg)
{
    (void)arg;

    /* Scratch buffer on the thread's own stack. Two channels' worth so the
     * stereo-fallback path fits. Touched immediately so the pages are
     * faulted in before anything time-critical happens. */
    int16_t buf[BB_MAX_PERIOD * 2];
    memset(buf, 0, sizeof buf);

    uint32_t t         = 0;      /* free-running sample counter        */
    uint32_t k         = 0;      /* loop position                      */
    uint32_t beat_pos  = 0;
    uint32_t bar_pos   = 0;
    uint32_t bar_count = 0;
    int32_t  gain_cur  = 0;      /* Q16 smoothed master gain           */

    LSnap    ls[BB_NLAYER];

    struct timespec next_wake;
    clock_gettime(CLOCK_MONOTONIC, &next_wake);

    while (atomic_load_explicit(&bb.running, memory_order_relaxed)) {

        /* --- park, if the UI wants the device to itself ------------------ */
        if (atomic_load_explicit(&g_park_req, memory_order_acquire)) {
            atomic_store_explicit(&g_parked, 1, memory_order_release);
            while (atomic_load_explicit(&g_park_req, memory_order_acquire) &&
                   atomic_load_explicit(&bb.running, memory_order_relaxed))
                park_sleep(200);
            atomic_store_explicit(&g_parked, 0, memory_order_release);
            clock_gettime(CLOCK_MONOTONIC, &next_wake);
            continue;
        }

        /* --- publish that a new period is starting -----------------------
         * Sequentially consistent, and the order between this and the
         * program loads below is load-bearing: the UI thread's proof that a
         * retired program is dead depends on "epoch bumped, THEN prog read"
         * holding in one global order both threads agree on. See
         * bb_reclaim() in main.c. */
        atomic_fetch_add(&bb.epoch, 1);

        /* --- snapshot the master ----------------------------------------- */
        int rate = atomic_load_explicit(&bb.rate,   memory_order_relaxed);
        int mute = atomic_load_explicit(&bb.mute,   memory_order_relaxed);
        int pan  = atomic_load_explicit(&bb.panic,  memory_order_relaxed);
        int gtgt = atomic_load_explicit(&bb.gain,   memory_order_relaxed);
        int byp  = atomic_load_explicit(&bb.bypass, memory_order_relaxed);

        int bpm   = atomic_load_explicit(&bb.gctl[GCTL_BPM],   memory_order_relaxed);
        int beats = atomic_load_explicit(&bb.gctl[GCTL_BEATS], memory_order_relaxed);
        int bars  = atomic_load_explicit(&bb.gctl[GCTL_BARS],  memory_order_relaxed);

        int lp_bars = atomic_load_explicit(&bb.loop_bars, memory_order_relaxed);
        int lp_mix  = atomic_load_explicit(&bb.loop_mix, memory_order_relaxed);
        int lp_fb   = atomic_load_explicit(&bb.loop_feedback, memory_order_relaxed);
        int lp_od   = atomic_load_explicit(&bb.loop_overdub, memory_order_relaxed);
        int lp_rate = atomic_load_explicit(&bb.loop_rate, memory_order_relaxed);
        int lp_rev  = atomic_load_explicit(&bb.loop_reverse, memory_order_relaxed);
        int lp_slice= atomic_load_explicit(&bb.loop_slice, memory_order_relaxed);
        loop_command();

        /* --- snapshot every layer ---------------------------------------- */
        int any_seq = 0;
        for (int L = 0; L < BB_NLAYER; L++) {
            Layer *ly = &bb.layer[L];
            LSnap *sn = &ls[L];

            sn->prog  = atomic_load(&ly->prog);
            sn->on    = atomic_load_explicit(&ly->on,   memory_order_relaxed);
            sn->mode  = atomic_load_explicit(&ly->mode, memory_order_relaxed);
            sn->level = atomic_load_explicit(&ly->ctl[LCTL_LEVEL], memory_order_relaxed);

            for (int i = 0; i < BB_NPARAM; i++)
                sn->p[i] = atomic_load_explicit(&ly->param[i], memory_order_relaxed);

            sn->pp.drive   = atomic_load_explicit(&ly->ctl[LCTL_DRIVE],   memory_order_relaxed);
            sn->pp.tone    = atomic_load_explicit(&ly->ctl[LCTL_TONE],    memory_order_relaxed);
            sn->pp.crush   = atomic_load_explicit(&ly->ctl[LCTL_CRUSH],   memory_order_relaxed);
            sn->pp.spc_fb  = atomic_load_explicit(&ly->ctl[LCTL_SPC_FB],  memory_order_relaxed);
            sn->pp.spc_mix = atomic_load_explicit(&ly->ctl[LCTL_SPC_MIX], memory_order_relaxed);
            sn->pp.spc_freeze = atomic_load_explicit(&ly->ctl[LCTL_SPC_FREEZE],
                                                      memory_order_relaxed);
            sn->pp.bypass  = byp;
            sn->spc_time_raw = atomic_load_explicit(&ly->ctl[LCTL_SPC_TIME],
                                                     memory_order_relaxed);
            sn->spc_sync = atomic_load_explicit(&ly->ctl[LCTL_SPC_SYNC],
                                                 memory_order_relaxed);

            sn->seq_on = atomic_load_explicit(&ly->seq_on, memory_order_relaxed);
            sn->steps  = bb_clampi(atomic_load_explicit(&ly->ctl[LCTL_STEPS],
                                                        memory_order_relaxed), 1, BB_STEPS);
            sn->decay = atomic_load_explicit(&ly->ctl[LCTL_DECAY], memory_order_relaxed);
            sn->motion_mask = atomic_load_explicit(&ly->motion_mask, memory_order_relaxed);

            for (int i = 0; i < BB_STEPS; i++) {
                sn->gate[i]  = atomic_load_explicit(&ly->seq_gate[i],  memory_order_relaxed);
                sn->pitch[i] = atomic_load_explicit(&ly->seq_pitch[i], memory_order_relaxed);
                sn->ratchet[i] = atomic_load_explicit(&ly->seq_ratchet[i], memory_order_relaxed);
                sn->prob[i] = atomic_load_explicit(&ly->seq_prob[i], memory_order_relaxed);
                for (int q = 0; q < BB_LOCK_COUNT; q++)
                    sn->lock[q][i] = atomic_load_explicit(&ly->seq_lock[q][i],
                                                          memory_order_relaxed);
            }
            if (sn->seq_on) any_seq = 1;
        }

        /* --- loop clock -------------------------------------------------- */
        uint32_t beat_len = (uint32_t)(((long)rate * 60L) / (bpm > 0 ? bpm : 90));
        if (beat_len < 1) beat_len = 1;
        uint32_t bar_len  = beat_len * (uint32_t)(beats > 0 ? beats : 4);
        uint32_t loop_len = bar_len  * (uint32_t)(bars  > 0 ? bars  : 1);
        if (loop_len < 1) loop_len = 1;
        uint32_t step_len = beat_len / 4;          /* one step = a 16th note */
        if (step_len < 1) step_len = 1;

        if (atomic_exchange_explicit(&bb.reset_t, 0, memory_order_relaxed)) {
            t = 0;
            for (int L = 0; L < BB_NLAYER; L++) {
                g_vt[L] = 0;
                g_hit_age[L] = INT32_MAX;
            }
        }
        if (atomic_exchange_explicit(&bb.reset_loop, 0, memory_order_relaxed)) {
            k = beat_pos = bar_pos = bar_count = 0;
            g_loop_phase = 0;
            for (int L = 0; L < BB_NLAYER; L++) {
                g_last_step[L] = -1;
                g_last_tick[L] = UINT32_MAX;
                g_last_sub[L] = -1;
            }
        }
        /* Tempo or rate changed under us -- fold the counters back into
         * range rather than letting k run off the end of the loop. */
        if (k        >= loop_len) k        %= loop_len;
        if (beat_pos >= beat_len) beat_pos %= beat_len;
        if (bar_pos  >= bar_len)  bar_pos  %= bar_len;

        /* ~2ms attack: fast enough to read as a hit, slow enough not to click */
        int32_t atk_inc = 65536 / (rate / 500 > 0 ? rate / 500 : 1);
        if (atk_inc < 1) atk_inc = 1;

        for (int L = 0; L < BB_NLAYER; L++) {
            g_ctx[L].sr = rate;
            g_ctx[L].bl = (int32_t)beat_len;
            g_ctx[L].ll = (int32_t)loop_len;
        }

        int target_gain = (mute || pan) ? 0 : (gtgt << 8);   /* 0..256 -> Q16 */
        int frames = (int)g_period;
        int clipped = 0;

        /* --- render ------------------------------------------------------ */
        struct timespec c0, c1;
        clock_gettime(CLOCK_MONOTONIC, &c0);

        for (int i = 0; i < frames; i++) {
            int32_t mix = 0;
            uint32_t tick = k / step_len;
            uint32_t in_step = k % step_len;

            for (int L = 0; L < BB_NLAYER; L++) {
                LSnap *sn = &ls[L];

                int step  = sn->seq_on ? (int)(tick % (uint32_t)sn->steps) : 0;
                int gate  = sn->seq_on ? sn->gate[step]  : GATE_ON;
                int semis = sn->seq_on ? sn->pitch[step] : 0;
                int level = locked(sn, LOCK_LEVEL, step, sn->level,
                                   in_step, step_len);

                /* Level is ramped rather than switched, so muting a layer is
                 * a fade and not a click. Once it has reached silence we can
                 * skip the layer's DSP entirely, which is where the CPU
                 * saving from having layers off actually comes from. */
                int32_t ltgt = sn->on ? (bb_clampi(level, 0, 256) << 8) : 0;
                if (g_lvl[L] < ltgt) {
                    g_lvl[L] += 32; if (g_lvl[L] > ltgt) g_lvl[L] = ltgt;
                } else if (g_lvl[L] > ltgt) {
                    g_lvl[L] -= 32; if (g_lvl[L] < ltgt) g_lvl[L] = ltgt;
                }
                if (g_lvl[L] == 0 || !sn->prog) continue;

                /* --- sequencer ---------------------------------------------
                 * Probability is decided exactly once at the main-step edge.
                 * Ratchets divide that step into equal sub-events; each one
                 * produces a one-sample `tr` and resets `age`. */
                int trigger = 0;
                if (sn->seq_on) {
                    if (tick != g_last_tick[L]) {
                        g_last_tick[L] = tick;
                        g_last_step[L] = step;
                        g_last_sub[L] = -1;
                        int prob = bb_clampi(sn->prob[step], 0, 100);
                        g_step_fire[L] = gate != GATE_OFF &&
                            (prob >= 100 || (int)(seq_rand(L) % 100u) < prob);
                    }

                    int rat = bb_clampi(sn->ratchet[step], 1, 4);
                    int sub = (int)(((uint64_t)in_step * (uint64_t)rat) / step_len);
                    if (sub >= rat) sub = rat - 1;
                    if (sub != g_last_sub[L]) {
                        g_last_sub[L] = sub;
                        if (g_step_fire[L]) {
                            trigger = 1;
                            g_env_peak[L] = (gate == GATE_ACCENT) ? 65536 : 38000;
                            g_env_atk[L]  = 1;
                            g_hit_age[L]  = 0;
                            g_hit_vel[L]  = (gate == GATE_ACCENT) ? 256 : 148;
                        }
                    }

                    int32_t dk = decay_coeff(locked(sn, LOCK_DECAY, step,
                                                    sn->decay, in_step, step_len));
                    if (g_env_atk[L]) {
                        g_env[L] += atk_inc;
                        if (g_env[L] >= g_env_peak[L]) {
                            g_env[L] = g_env_peak[L];
                            g_env_atk[L] = 0;
                        }
                    } else if (!g_step_fire[L]) {
                        g_env[L] -= (int32_t)(((int64_t)g_env[L] * 200) >> 16);
                    } else if (dk > 0) {
                        g_env[L] -= (int32_t)(((int64_t)g_env[L] * dk) >> 16);
                    }
                    if (g_env[L] < 0) g_env[L] = 0;
                } else {
                    g_env[L] = 65536;
                    g_env_atk[L] = 0;
                    g_hit_vel[L] = 256;
                }

                /* Voice clock. Advancing it faster transposes the entire
                 * expression, because every frequency in a bytebeat is
                 * derived from t. At 0 semitones this is exactly t. */
                g_vt[L] += PITCH_Q32[bb_clampi(semis, -12, 12) + 12];

                g_ctx[L].t  = (int32_t)(uint32_t)(g_vt[L] >> 32);
                g_ctx[L].k  = (int32_t)k;
                g_ctx[L].n  = (int32_t)bar_count;
                g_ctx[L].bt = (int32_t)beat_pos;
                g_ctx[L].tr = trigger;
                g_ctx[L].age = g_hit_age[L];
                g_ctx[L].vel = g_hit_vel[L];
                for (int p = 0; p < BB_NPARAM; p++)
                    g_ctx[L].p[p] = bb_clampi(locked(sn, LOCK_P0 + p, step,
                                                     sn->p[p], in_step, step_len),
                                              0, 255);

                int32_t v = expr_eval(sn->prog, &g_ctx[L]);
                if (g_hit_age[L] < INT32_MAX) g_hit_age[L]++;

                /* The delay write cursor advances exactly once per sample no
                 * matter how many times the expression called w(). That is
                 * what makes d(1) reliably mean "the previous sample". */
                g_ctx[L].dw = (g_ctx[L].dw + 1u) & EXPR_DELAY_MASK;

                /* --- output mode: how an arbitrary int32 becomes a sample ---
                 * BYTE   : keep 8 bits, read UNSIGNED. Silence is 128. This
                 *          is the formulation every bytebeat on the internet
                 *          assumes.
                 * SIGNED : the same 8 bits as two's complement. Silence is 0,
                 *          so the wrap points move -- same data, very
                 *          different sound.
                 * WORD   : keep 16 bits. Wider and smoother, far less of the
                 *          characteristic 8-bit crunch. */
                int32_t s;
                switch (sn->mode) {
                case BB_BYTE:   s = (int32_t)((v & 0xff) - 128) << 8;  break;
                case BB_SIGNED: s = (int32_t)(int8_t)(v & 0xff) << 8;  break;
                default:        s = (int32_t)(int16_t)(v & 0xffff);    break;
                }

                /* Gate BEFORE the post chain, so space/delay tails keep
                 * ringing after the gate shuts. Gating afterwards would chop
                 * the reverb off with the note, which sounds like a fault. */
                if (sn->seq_on) s = (int32_t)(((int64_t)s * g_env[L]) >> 16);

                PostParams pp = sn->pp;
                pp.drive = bb_clampi(locked(sn, LOCK_DRIVE, step, sn->pp.drive,
                                            in_step, step_len), 0, 255);
                pp.tone = bb_clampi(locked(sn, LOCK_TONE, step, sn->pp.tone,
                                           in_step, step_len), 1, 255);
                pp.crush = bb_clampi(locked(sn, LOCK_CRUSH, step, sn->pp.crush,
                                            in_step, step_len), 0, 255);
                pp.spc_fb = bb_clampi(locked(sn, LOCK_SPC_FB, step, sn->pp.spc_fb,
                                             in_step, step_len), 0, 255);
                pp.spc_mix = bb_clampi(locked(sn, LOCK_SPC_MIX, step, sn->pp.spc_mix,
                                              in_step, step_len), 0, 255);
                int spc_raw = bb_clampi(locked(sn, LOCK_SPC_TIME, step,
                                               sn->spc_time_raw, in_step, step_len),
                                        0, 255);
                pp.spc_time = space_samples(spc_raw, sn->spc_sync,
                                             beat_len, bar_len, rate);

                s = post_process(&g_post[L], s, &pp);

                /* DC removal per layer, even when the chain is bypassed --
                 * eight layers each with a small offset would otherwise sum
                 * into a large one. See dsp_dcblock(). */
                s = dsp_dcblock(&g_post[L], s);

                mix += (int32_t)(((int64_t)s * g_lvl[L]) >> 16);
            }

            if (mix > 32767 || mix < -32768) clipped = 1;
            mix = dsp_clip16(mix);

            /* The phrase looper sits on the finished pre-gain bus. Capture
             * therefore includes every layer and its tails, while master
             * gain/mute remain performance controls outside the recording. */
            mix = loop_process(mix, bar_pos, bar_len, lp_bars, lp_mix, lp_fb,
                               lp_od, lp_rate, lp_rev, lp_slice);

            /* Scope taps the summed bus PRE-gain and pre-mute, so you can
             * keep watching what you are building while it is silent. */
            scope_push((int16_t)mix);

            /* Master gain, ramped. A step change in gain is a step change in
             * the waveform, and a step is a click. 32 units per sample over a
             * 0..65536 range is a ~45ms full sweep at 44.1kHz: fast enough
             * that PANIC feels instant, slow enough that it does not thump. */
            if (gain_cur < target_gain) {
                gain_cur += 32; if (gain_cur > target_gain) gain_cur = target_gain;
            } else if (gain_cur > target_gain) {
                gain_cur -= 32; if (gain_cur < target_gain) gain_cur = target_gain;
            }
            int16_t o16 = (int16_t)dsp_clip16((int32_t)(((int64_t)mix * gain_cur) >> 16));

            sink_push(o16);

            if (g_chans == 2) { buf[i * 2] = o16; buf[i * 2 + 1] = o16; }
            else              { buf[i] = o16; }

            t++;
            k++;         if (k        >= loop_len) k = 0;
            beat_pos++;  if (beat_pos >= beat_len) beat_pos = 0;
            bar_pos++;   if (bar_pos  >= bar_len)  { bar_pos = 0; bar_count++; }
        }

        clock_gettime(CLOCK_MONOTONIC, &c1);
        long us = (c1.tv_sec - c0.tv_sec) * 1000000L
                + (c1.tv_nsec - c0.tv_nsec) / 1000L;
        atomic_store_explicit(&bb.cpu_us, (int)us, memory_order_relaxed);
        atomic_store_explicit(&bb.budget_us,
                              (int)((1000000L * frames) / (rate > 0 ? rate : 1)),
                              memory_order_relaxed);
        atomic_store_explicit(&bb.clipping, clipped, memory_order_relaxed);

        atomic_store_explicit(&bb.t,   t,         memory_order_relaxed);
        atomic_store_explicit(&bb.k,   k,         memory_order_relaxed);
        atomic_store_explicit(&bb.bar, bar_count, memory_order_relaxed);
        atomic_store_explicit(&bb.seq_pos,
                              any_seq ? (int)((k / step_len) % BB_STEPS) : -1,
                              memory_order_relaxed);
        atomic_store_explicit(&bb.loop_pos, g_loop_pub_pos, memory_order_relaxed);
        atomic_store_explicit(&bb.loop_frames, g_loop_len, memory_order_relaxed);

        /* --- hand it to the hardware ------------------------------------ */
        if (g_pcm) {
            /* The ONLY blocking call the audio thread is allowed to make. It
             * sleeps until the card has drained a period, which is what paces
             * this whole loop. */
            snd_pcm_sframes_t n = snd_pcm_writei(g_pcm, buf, (snd_pcm_uframes_t)frames);
            if (n < 0) {
                recover(g_pcm, (int)n);
            } else if (n < frames) {
                /* Short write: push the tail. Rare in blocking mode but legal,
                 * and silently dropping the rest would hole the waveform. */
                snd_pcm_sframes_t done = n;
                while (done < frames) {
                    snd_pcm_sframes_t m = snd_pcm_writei(
                        g_pcm, buf + done * g_chans,
                        (snd_pcm_uframes_t)(frames - done));
                    if (m < 0) { recover(g_pcm, (int)m); break; }
                    done += m;
                }
            }
        } else {
            /* No device: pace ourselves off the monotonic clock so that t
             * still advances in real time and a network listener receives
             * samples at the right speed. Absolute-deadline sleeping means
             * scheduling jitter does not accumulate into drift. */
            long ns = (long)((1000000000LL * frames) / (rate > 0 ? rate : 1));
            next_wake.tv_nsec += ns;
            while (next_wake.tv_nsec >= 1000000000L) {
                next_wake.tv_nsec -= 1000000000L;
                next_wake.tv_sec++;
            }
            clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next_wake, NULL);
        }
    }
    return NULL;
}

/* ======================================================================== */

int audio_start(char *warn, size_t warnsz)
{
    pthread_attr_t attr;
    struct sched_param sp;
    int rc;

    warn[0] = '\0';
    atomic_store(&bb.running, 1);

    pthread_attr_init(&attr);

    /* Try for real-time scheduling.
     *
     * Under SCHED_FIFO this thread runs until it blocks -- no timeslice, no
     * preemption by ordinary work. That is what stops a browser or a
     * compile from stealing the CPU during a period and causing an xrun.
     *
     * It normally requires either root or an rtprio limit in
     * /etc/security/limits.conf. Failing to get it must NEVER be fatal:
     * a slightly glitchy instrument is infinitely better than no
     * instrument, so we fall back and say so on screen. */
    sp.sched_priority = 70;
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    pthread_attr_setschedparam(&attr, &sp);

    rc = pthread_create(&g_thread, &attr, audio_thread, NULL);
    if (rc != 0) {
        pthread_attr_destroy(&attr);
        pthread_attr_init(&attr);      /* plain inherited scheduling */
        rc = pthread_create(&g_thread, &attr, audio_thread, NULL);
        if (rc != 0) {
            snprintf(warn, warnsz, "cannot start audio thread: %s", strerror(rc));
            pthread_attr_destroy(&attr);
            atomic_store(&bb.running, 0);
            return -1;
        }
        snprintf(warn, warnsz,
                 "no realtime priority - run 'make caps' to fix. "
                 "Harmless otherwise; watch the xrun counter.");
    }
    pthread_attr_destroy(&attr);
    g_thread_live = 1;
    return 0;
}

void audio_stop(void)
{
    if (!g_thread_live) return;
    atomic_store(&bb.running, 0);
    atomic_store(&g_park_req, 0);      /* make sure a parked thread exits */
    pthread_join(g_thread, NULL);
    g_thread_live = 0;
}
