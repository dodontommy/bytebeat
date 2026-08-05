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
 *
 * THE ENGINE IS ELSEWHERE
 *
 * Every sample of actual DSP now lives in engine.c (bb_engine_render). This
 * file is only the device plumbing: it opens ALSA, runs the deadline thread
 * that calls the engine, and hands the filled buffer to the card. The same
 * engine.c is what JUCE drives in the GUI -- one instrument, two doors.
 * ------------------------------------------------------------------------
 */

#include "bytebeat.h"
#include "audio.h"
#include "engine.h"
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

/* ======================================================================== */
/*  Device configuration                                                    */
/* ======================================================================== */

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

    /* RW_INTERLEAVED: we hand ALSA a normal array and it copies. */
    if ((rc = snd_pcm_hw_params_set_access(g_pcm, hw,
                                           SND_PCM_ACCESS_RW_INTERLEAVED)) < 0)
        FAIL("set_access", rc);

    /* We always feed the device signed 16-bit little-endian. */
    if ((rc = snd_pcm_hw_params_set_format(g_pcm, hw,
                                           SND_PCM_FORMAT_S16_LE)) < 0)
        FAIL("set_format S16_LE", rc);

    g_chans = 1;
    if (snd_pcm_hw_params_set_channels(g_pcm, hw, 1) < 0) {
        g_chans = 2;
        if ((rc = snd_pcm_hw_params_set_channels(g_pcm, hw, 2)) < 0)
            FAIL("set_channels", rc);
    }

    if ((rc = snd_pcm_hw_params_set_rate_near(g_pcm, hw, &rate, 0)) < 0)
        FAIL("set_rate_near", rc);

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

    /* Software params:
     *   start_threshold = buffer size -> the card does not start converting
     *     until we have filled the whole buffer once.
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

    /* Wire up the engine's private DSP state before anything renders. */
    bb_engine_init(rate);

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

    snd_pcm_drop(g_pcm);
    snd_pcm_hw_free(g_pcm);

    int rc = configure(rate, err, errsz);
    if (rc < 0) {
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

static int recover(snd_pcm_t *h, int e)
{
    if (e == -EPIPE) {
        atomic_fetch_add(&bb.xruns, 1);
        return snd_pcm_prepare(h);
    }
    if (e == -ESTRPIPE) {
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

static void *audio_thread(void *arg)
{
    (void)arg;

    /* Scratch buffer on the thread's own stack. Two channels' worth so the
     * stereo-fallback path fits. Touched immediately so the pages are
     * faulted in before anything time-critical happens. */
    int16_t buf[BB_MAX_PERIOD * 2];
    memset(buf, 0, sizeof buf);

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

        /* --- render the period and hand it to the hardware ---------------- */
        int rate = atomic_load(&bb.rate);
        int frames = (int)g_period;
        bb_engine_render(buf, frames, (int)g_chans);

        if (g_pcm) {
            /* The ONLY blocking call the audio thread is allowed to make. */
            snd_pcm_sframes_t n = snd_pcm_writei(g_pcm, buf, (snd_pcm_uframes_t)frames);
            if (n < 0) {
                recover(g_pcm, (int)n);
            } else if (n < frames) {
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
             * still advances in real time. Absolute-deadline sleeping means
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

    /* Try for real-time scheduling. Failing to get it must NEVER be fatal. */
    sp.sched_priority = 70;
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    pthread_attr_setschedparam(&attr, &sp);

    rc = pthread_create(&g_thread, &attr, audio_thread, NULL);
    if (rc != 0) {
        pthread_attr_destroy(&attr);
        pthread_attr_init(&attr);
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
    atomic_store(&g_park_req, 0);
    pthread_join(g_thread, NULL);
    g_thread_live = 0;
}

/* The old audio_self_test (clock/phrase-loop invariants) now lives in the
 * engine as bb_engine_self_test so the GUI test runner can reach it too.
 * Thin alias kept so the terminal binary's call site does not change. */
int audio_self_test(char *err, size_t errsz)
{
    return bb_engine_self_test(err, errsz);
}
