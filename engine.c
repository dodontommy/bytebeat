/* engine.c -- the device-independent audio core.
 *
 * Extracted from the terminal instrument's audio thread so a GUI (or anything
 * else) can make the same sound without committing to ALSA. The single rule
 * that used to live in audio.c still lives here and is still the whole game:
 *
 *   ONLY bb_engine_render() runs on the audio thread. It must never malloc,
 *   never lock, and never block. Everything the audio thread needs is a
 *   compile-time allocation in this file, exactly as it was before.
 *
 * The audio-device of the day is whoever calls bb_engine_render():
 *   - the terminal instrument's ALSA thread (thin, see audio.c)
 *   - JUCE's device callback (see the GUI)
 *   - the regression suite (see bb_engine_self_test)
 *
 * The shared ownership of `bb` (bytebeat.h) is what lets the render loop and
 * the session machinery agree on everything: layers, transport, looper,
 * telemetry. main.c no longer owns them; this file does.
 */

#include "bytebeat.h"
#include "bb_platform.h"
#include "dsp.h"
#include "engine.h"
#include "rack.h"
#include "gen.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <math.h>

/* The only OS-specific include left in this file, and only because the
 * flush-to-medium call in bb_config_save() needs a file descriptor out of a
 * FILE*. Everything else the platform is asked for goes through
 * bb_platform.h; see the comment on cfg_flush_to_disk(). */
#if defined(_WIN32)
#  include <io.h>       /* _fileno, _commit */
#else
#  include <unistd.h>   /* fileno, fsync    */
#  if defined(__APPLE__)
#    include <fcntl.h>  /* F_FULLFSYNC      */
#  endif
#endif

/* ---- session state (owned here, declared in bytebeat.h) ------------------ */
struct bb_state bb;

char bb_expr[BB_NLAYER][BB_EXPR_MAX];
Rack bb_rack[BB_NLAYER];
int  bb_custom[BB_NLAYER];
char bb_ret_name[BB_NRET][BB_RET_NAME];

volatile sig_atomic_t bb_quit_signal;

/* ---- control metadata ----------------------------------------------------
 * The ladder tables that drive every knob's range and step size. Owned here
 * so the TUI, the GUI and the session loader all agree on the same controls.
 * (These used to live in the TUI; the config loader needs them, and it is
 * not a UI concern.) */
const CtlInfo bb_lctl_info[LCTL_COUNT] = {
    { "LEVEL",   0, 256, 8, 32 },
    { "DRIVE",   0, 255, 4, 32 },
    { "TONE",    1, 255, 4, 32 },
    { "CRUSH",   0, 255, 2, 16 },
    { "SP-TIME", 0, 255, 4, 32 },
    { "SP-FB",   0, 255, 4, 32 },
    { "SP-MIX",  0, 255, 4, 32 },
    { "STEPS",   1,  16, 1,  4 },
    { "DECAY",   0, 255, 4, 32 },
    { "SP-SYNC", 0,  10, 1,  2 },
    { "FREEZE",  0,   1, 1,  1 },
};

const CtlInfo bb_gctl_info[GCTL_COUNT] = {
    { "BPM",    30, 240, 1, 10 },
    { "BEATS",   1,  16, 1,  4 },
    { "BARS",    1,  16, 1,  4 },
    { "ZOOM",    1, 256, 2, 16 },
};

/* ---- program publication and reclamation ---------------------------------
 * Moved verbatim from main.c -- see bb_publish/bb_reclaim there for the full
 * reasoning about the lock-free atomic swap and the retire list. */
static Program *retire_head;

static void free_all_sampler_data(void);   /* defined with the step sampler */
static void arr_clip_reclaim(void);        /* defined with the R2 song      */
static void free_all_arr_clips(void);
static void arr_song_reclaim(void);
static void free_all_arr_songs(void);

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
        return 0;
    }

    Program *old = atomic_exchange(&bb.layer[layer].prog, np);

    if (old) {
        old->retire_epoch = atomic_load(&bb.epoch);
        old->next = retire_head;
        retire_head = old;
    }
    return 1;
}

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
    Program *p = retire_head;
    while (p) { Program *n = p->next; free(p); p = n; }
    retire_head = NULL;

    for (int i = 0; i < BB_NLAYER; i++)
        free(atomic_exchange(&bb.layer[i].prog, NULL));
}

int bb_engine_publish(int layer, const char *src, ExprError *err)
{ return bb_publish(layer, src, err); }

void bb_engine_reclaim(void) { bb_reclaim(); bb_engine_sampler_reclaim(); arr_clip_reclaim(); arr_song_reclaim(); bb_engine_ret_service(); }

void bb_engine_shutdown(void) { free_all_programs(); free_all_sampler_data(); free_all_arr_clips(); free_all_arr_songs(); }

/* ======================================================================== */
/*  R1 step sampler: sample pool + playback state                            */
/* ======================================================================== */

/* The pattern and per-slot controls live in bb.sampler[] (bytebeat.h). The
 * sample WAV data itself is owned by the UI thread and published to the
 * audio thread exactly like a Program: a lock-free atomic swap, read once
 * per period, reclaimed once it is at least two epochs old.
 *
 * All of the playback state below belongs to the audio thread alone: the
 * play position (Q32 so pitch ratio joins are exact), the per-hit playback
 * rate, a run flag, and a velocity/level envelope smoothed the same way the
 * voice mix levels are. */
/* One immutable header per published sample, swapped through a SINGLE atomic
 * pointer exactly like a Program. The audio thread reads the pointer once and
 * takes data/n/rate from the same struct, so it can never see a new buffer
 * with a stale length (or vice versa). The retire linkage is embedded in the
 * header itself, so unpublishing never needs a side allocation -- there is no
 * failure path that could free a buffer the render thread still reads. */
typedef struct SmpBuf {
    int16_t *data;                  /* caller's malloc'd mono frames        */
    int      n;                     /* frame count, > 0                     */
    int      rate;                  /* sample rate of data, > 0             */
    unsigned long long retire_epoch;
    struct SmpBuf *next;            /* retire list, embedded like Program's */
} SmpBuf;

static _Atomic(SmpBuf *) g_smp_buf[BB_SAMPLER];
static SmpBuf *smp_retire_head;

static void smp_retire(SmpBuf *b)
{
    if (!b) return;
    b->retire_epoch = atomic_load(&bb.epoch);
    b->next = smp_retire_head;
    smp_retire_head = b;
}

int bb_engine_sampler_set(int slot, int16_t *mono, int n, int rate)
{
    if (slot < 0 || slot >= BB_SAMPLER || !mono || n <= 0 || rate <= 0) {
        if (mono) free(mono);
        return 0;
    }
    SmpBuf *nb = malloc(sizeof *nb);
    if (!nb) { free(mono); return 0; }   /* never published, safe to free */
    nb->data = mono;
    nb->n    = n;
    nb->rate = rate;
    nb->retire_epoch = 0;
    nb->next = NULL;
    SmpBuf *old = atomic_exchange(&g_smp_buf[slot], nb);
    if (old) smp_retire(old);
    return 1;
}

void bb_engine_sampler_clear(int slot)
{
    if (slot < 0 || slot >= BB_SAMPLER) return;
    SmpBuf *old = atomic_exchange(&g_smp_buf[slot], NULL);
    if (old) smp_retire(old);
}

void bb_engine_sampler_reclaim(void)
{
    unsigned long long now = atomic_load(&bb.epoch);
    SmpBuf **pp = &smp_retire_head;
    while (*pp) {
        SmpBuf *b = *pp;
        if (now >= b->retire_epoch + 2) {
            *pp = b->next;
            free(b->data);
            free(b);
        } else {
            pp = &b->next;
        }
    }
}

int bb_engine_sampler_loaded(int slot)
{
    if (slot < 0 || slot >= BB_SAMPLER) return 0;
    return atomic_load(&g_smp_buf[slot]) != NULL;   /* n > 0 guaranteed */
}

static void free_all_sampler_data(void)
{
    SmpBuf *b = smp_retire_head;
    while (b) { SmpBuf *nx = b->next; free(b->data); free(b); b = nx; }
    smp_retire_head = NULL;
    for (int s = 0; s < BB_SAMPLER; s++) {
        SmpBuf *p = atomic_exchange(&g_smp_buf[s], NULL);
        if (p) { free(p->data); free(p); }
    }
}

/* ======================================================================== */
/*  R2 arrangement timeline: clip buffers + song publication                  */
/* ======================================================================== */

/* One immutable audio buffer per clip, following the SmpBuf discipline to
 * the letter: created whole by the UI thread, handed to the render thread
 * only through pointers inside a published song snapshot, retired (never
 * freed) by the UI, and reclaimed on the normal bb_engine_reclaim() path
 * once at least two epochs old. Unlike SmpBuf the frames are COPIED on
 * create -- clip audio typically comes straight out of a decoder's scratch
 * buffer or a capture array the UI wants to keep reusing.
 *
 * The retire linkage is embedded in the header, so releasing a clip never
 * needs a side allocation: there is no failure path that could free frames
 * the render thread still reads. */
struct ArrClipBuf {
    int16_t *data;                  /* our copy of the mono frames          */
    unsigned n;                     /* frame count, > 0                     */
    int      rate;                  /* sample rate of data, > 0             */
    unsigned long long retire_epoch;
    struct ArrClipBuf *next;        /* retire list, embedded like Program's */
};

static ArrClipBuf *arr_clip_retire_head;

ArrClipBuf *bb_engine_clip_create(const int16_t *data, unsigned n, int rate)
{
    if (!data || n == 0 || rate <= 0) return NULL;
    ArrClipBuf *b = malloc(sizeof *b);
    if (!b) return NULL;
    b->data = malloc((size_t)n * sizeof *b->data);
    if (!b->data) { free(b); return NULL; }
    memcpy(b->data, data, (size_t)n * sizeof *b->data);
    b->n    = n;
    b->rate = rate;
    b->retire_epoch = 0;
    b->next = NULL;
    return b;
}

void bb_engine_clip_release(ArrClipBuf *b)
{
    if (!b) return;
    b->retire_epoch = atomic_load(&bb.epoch);
    b->next = arr_clip_retire_head;
    arr_clip_retire_head = b;
}

unsigned bb_engine_clip_frames(const ArrClipBuf *b)
{
    return b ? b->n : 0;
}

const int16_t *bb_engine_clip_data(const ArrClipBuf *b)
{
    return b ? b->data : NULL;
}

/* UI thread, from bb_engine_reclaim() -- same proof as bb_reclaim(). */
static void arr_clip_reclaim(void)
{
    unsigned long long now = atomic_load(&bb.epoch);
    ArrClipBuf **pp = &arr_clip_retire_head;
    while (*pp) {
        ArrClipBuf *b = *pp;
        if (now >= b->retire_epoch + 2) {
            *pp = b->next;
            free(b->data);
            free(b);
        } else {
            pp = &b->next;
        }
    }
}

/* Only after the render thread has stopped (bb_engine_shutdown). Clips the
 * UI still holds in its edit model are the UI's to release; this frees only
 * what was already retired. */
static void free_all_arr_clips(void)
{
    ArrClipBuf *b = arr_clip_retire_head;
    while (b) { ArrClipBuf *nx = b->next; free(b->data); free(b); b = nx; }
    arr_clip_retire_head = NULL;
}

/* ---- song publication ----------------------------------------------------
 * The song is an immutable snapshot of the whole clip list, swapped through
 * ONE atomic pointer exactly like a Program: the render thread reads the
 * pointer once per period and takes every clip's window, gain and audio
 * pointer from the same struct, so it can never see a half-edited song.
 * Publishing retires the OLD snapshot epoch+2; the ArrClipBufs the clips
 * point at are never freed here -- their lifetime belongs to the UI via
 * bb_engine_clip_release(), which follows the same epoch discipline, so a
 * song snapshot can never outlive the audio it points into as long as the
 * UI releases a buffer only after unpublishing every clip that uses it. */
typedef struct ArrSong {
    int     nclips;
    ArrClip clip[ARR_MAX_CLIPS];
    unsigned long long retire_epoch;
    struct ArrSong *next;               /* retire list, embedded like Program's */
} ArrSong;

static _Atomic(ArrSong *) g_song;
static ArrSong *arr_song_retire_head;

int bb_engine_song_publish(const ArrClip *clips, int nclips)
{
    if (nclips < 0 || nclips > ARR_MAX_CLIPS || (nclips > 0 && !clips))
        return -1;

    ArrSong *ns = malloc(sizeof *ns);
    if (!ns) return -1;                  /* old song keeps playing */

    ns->nclips = nclips;
    for (int i = 0; i < nclips; i++) {
        ArrClip *d = &ns->clip[i];
        *d = clips[i];
        /* Sanitize on the way in so the render thread never range-checks:
         * lane/gain clamped, a window is always at least one bar, and the
         * strings are guaranteed NUL-terminated whatever the caller sent. */
        d->lane = bb_clampi(d->lane, 0, ARR_LANES - 1);
        if (d->len_bars < 1) d->len_bars = 1;
        d->loop = !!d->loop;
        d->gain = bb_clampi(d->gain, 0, 256);
        d->name[ARR_NAME_MAX - 1] = '\0';
        d->path[ARR_PATH_MAX - 1] = '\0';
    }
    ns->retire_epoch = 0;
    ns->next = NULL;

    ArrSong *old = atomic_exchange(&g_song, ns);
    if (old) {
        old->retire_epoch = atomic_load(&bb.epoch);
        old->next = arr_song_retire_head;
        arr_song_retire_head = old;
    }
    return 0;
}

int bb_engine_song_get(ArrClip *out, int max)
{
    /* UI thread only, and only the UI thread ever swaps g_song, so the
     * loaded snapshot cannot be retired out from under this copy. */
    const ArrSong *s = atomic_load(&g_song);
    if (!s || !out || max <= 0) return 0;
    int n = s->nclips < max ? s->nclips : max;
    memcpy(out, s->clip, (size_t)n * sizeof *out);
    return n;
}

void bb_engine_song_seek(int bar)
{
    atomic_store(&bb.arr_seek_bar, bar < 0 ? -1 : bar);
}

void bb_engine_song_play(int on)
{
    atomic_store(&bb.arr_play, on ? 1 : 0);
}

int bb_engine_song_playing(void)
{
    return atomic_load(&bb.arr_play);
}

void bb_engine_rec_src(int src)
{
    atomic_store(&bb.rec_src,
                 src == BB_REC_LIVE ? BB_REC_LIVE : BB_REC_MASTER);
}

int bb_engine_rec_src_get(void)
{
    return atomic_load(&bb.rec_src);
}

/* UI thread, from bb_engine_reclaim() -- same proof as bb_reclaim(). */
static void arr_song_reclaim(void)
{
    unsigned long long now = atomic_load(&bb.epoch);
    ArrSong **pp = &arr_song_retire_head;
    while (*pp) {
        ArrSong *s = *pp;
        if (now >= s->retire_epoch + 2) {
            *pp = s->next;
            free(s);
        } else {
            pp = &s->next;
        }
    }
}

static void free_all_arr_songs(void)   /* render thread stopped (shutdown) */
{
    ArrSong *s = arr_song_retire_head;
    while (s) { ArrSong *nx = s->next; free(s); s = nx; }
    arr_song_retire_head = NULL;
    free(atomic_exchange(&g_song, NULL));
}

/* ---- per-lane capture -----------------------------------------------------
 * The UI parks the request in these atomics and publishes ARR_REC_ARMED;
 * the render loop latches them while armed, starts copying at the next bar
 * boundary, and walks the status to RECORDING and DONE with single-shot
 * compare-exchanges so a UI cancel (a plain store of IDLE) always wins the
 * race instead of being overwritten. `dst` is UI-owned and preallocated;
 * the engine never frees it. NOTE for the owner of `dst`: after a cancel
 * the render thread may still be inside the period that writes it -- treat
 * the buffer like a retired Program and reuse/free it only two epochs
 * later (one bb_engine_reclaim() UI frame is comfortably enough). */
static _Atomic(int16_t *) g_arr_rec_dst;
static atomic_uint        g_arr_rec_cap;
static atomic_int         g_arr_rec_lane;
static atomic_int         g_arr_rec_bars;

int bb_engine_arr_arm(int lane, int bars, int16_t *dst, unsigned cap)
{
    /* Lane 9 (FILE/MASS) has no engine-side bus to tap -- refused. */
    if (lane < 0 || lane > 8 || bars < 1 || !dst || cap == 0)
        return -1;
    int st = atomic_load(&bb.arr_rec_status);
    if (st == ARR_REC_ARMED || st == ARR_REC_RECORDING)
        return -1;                      /* a capture is already in flight */

    atomic_store(&g_arr_rec_dst,  dst);
    atomic_store(&g_arr_rec_cap,  cap);
    atomic_store(&g_arr_rec_lane, lane);
    atomic_store(&g_arr_rec_bars, bars);
    atomic_store(&bb.arr_rec_frames, 0);
    /* seq_cst: the parameter stores above are visible before ARMED is. */
    atomic_store(&bb.arr_rec_status, ARR_REC_ARMED);
    return 0;
}

void bb_engine_arr_cancel(void)
{
    atomic_store(&bb.arr_rec_status, ARR_REC_IDLE);
}

/* ---- DSP state owned exclusively by the audio thread ---------------------
 * Big and static on purpose: the render thread never allocates, and the
 * whole BSS can be mlocked in one go so none of it is ever paged out mid
 * period. Eight fully independent voices cost ~8MB of buffers; that is the
 * price of layers that do not interfere with each other. */
static int32_t   g_delay[BB_NLAYER][EXPR_DELAY_LEN];
static int32_t   g_space[BB_NLAYER][BB_SPACE_LEN];
static ExprCtx   g_ctx[BB_NLAYER];
static PostState g_post[BB_NLAYER];

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

/* The master gain ramp. File scope rather than a static inside
 * bb_engine_render(), which is where it used to live, purely so that
 * bb_engine_init() can reach it: see the ramp-settling block there. Its
 * lifetime and its ownership are unchanged -- audio thread, one instance,
 * survives a device change -- and the render loop still drives it. */
static int32_t   g_gain_cur;

/* Step sampler playback state -- audio thread only (see below). */
static const SmpBuf *g_smp_cur[BB_SAMPLER]; /* last published buf seen   */
static int16_t  *g_smp_ds[BB_SAMPLER];   /* snapshot of the sample ptr   */
static uint32_t  g_smp_dlen[BB_SAMPLER]; /* snapshot sample length       */
static uint32_t  g_smp_drate[BB_SAMPLER];
static int       g_smp_sn[BB_SAMPLER];   /* snapshot: slot audible       */
static int       g_smp_mute[BB_SAMPLER];
static int       g_smp_solo[BB_SAMPLER];
static int       g_smp_choke[BB_SAMPLER];
static int       g_smp_gate[BB_SAMPLER][BB_STEPS];
static int       g_smp_pitch[BB_SAMPLER][BB_STEPS];
static int       g_smp_vel[BB_SAMPLER][BB_STEPS];
static int32_t   g_smp_vtgt[BB_SAMPLER]; /* Q16 level target this period */

static uint64_t  g_smp_pos[BB_SAMPLER];  /* play position, Q32           */
static int64_t   g_smp_inc[BB_SAMPLER];  /* Q32 frames per output frame  */
static int       g_smp_run[BB_SAMPLER];  /* currently sounding           */
static int32_t   g_smp_tgt[BB_SAMPLER];  /* Q16 velocity amp target      */
static int32_t   g_smp_amp[BB_SAMPLER];  /* Q16 current amp              */
static int32_t   g_smp_vol[BB_SAMPLER];  /* Q16 smoothed level           */
static int32_t   g_smp_pk[BB_SAMPLER];   /* abs peak this period (meters) */
static uint32_t  g_smp_tick = UINT32_MAX;

/* R2 song playback state -- audio thread only. One frames-into-window
 * counter and one in-window flag per clip INDEX: the counter resets when
 * the transport enters the clip's window (edge-detected on membership) and
 * runs 1:1 at the device rate from there, so tempo changes move the bar
 * grid under the audio instead of resampling it. */
static uint32_t g_arr_ctr[ARR_MAX_CLIPS];   /* frames into the window       */
static uint8_t  g_arr_in[ARR_MAX_CLIPS];    /* membership edge detector     */

/* R2 per-lane capture state -- audio thread only (armed via the atomics
 * defined with bb_engine_arr_arm above). */
static int16_t *g_cap_dst;                  /* UI-owned destination         */
static unsigned g_cap_cap;                  /* dst capacity in frames       */
static unsigned g_cap_pos;                  /* frames written so far        */
static int      g_cap_lane;                 /* 0-7 voice con, 8 sampler bus */
static int      g_cap_left;                 /* whole bars still to capture  */
static int      g_cap_run;                  /* 1 once the boundary fired    */

/* Master phrase looper. Buffer and cursors belong to the audio thread; the
 * UI only touches the atomic controls in bb. */
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

/* ---- THE RETURN BUS -------------------------------------------------------
 * RETURN A -- the CHAMBER -- is now slot 0 of an eight-slot return bus. The
 * reverb itself did not move an instruction: ret.c holds the identical comb
 * and allpass loops, reindexed off a carved block instead of five file
 * statics, and slot 0's SIZE / DARK / LEVEL and send column are still
 * bb.verb_size / verb_tone / verb_level and bb.layer[].send / bb.smp_send.
 * See the comment on `Return` in bytebeat.h for why the alias is
 * unconditional and why there is no shadow copy of it anywhere.
 *
 * These two forwarders exist so the chamber block in the regression suite
 * compiles and passes UNEDITED, with its hard-coded fb 28880, damp 14848 and
 * line lengths. If that block ever needs new numbers, the chamber's
 * arithmetic moved and the whole bit-exactness argument is already broken --
 * stop and bisect rather than adjusting the test. */
static void verb_reset(void) { ret_chamber_reset(0); }

static int32_t verb_process(int32_t in, int32_t fb, int32_t damp,
                            const int *clen, const int *alen)
{
    return ret_chamber_process(0, in, fb, damp, clen, alen);
}

/* ---- the bus's own state, audio thread only ------------------------------
 * The effects' state lives in ret.c. What lives here is everything the GRAPH
 * needs: the one-sample delay that makes every link edge well defined, the
 * create/destroy fade, the safety stage, and the meters.
 *
 * g_ret_prev[] is the z^-1 on every return->return edge. It is what turns a
 * cycle from an algebraic loop into a difference equation, so the slots may
 * be walked in any order and the slot NUMBER never reaches the ears. */
static int32_t g_ret_prev[BB_NRET];   /* previous frame's raw output, z^-1   */
static int32_t g_ret_cur [BB_NRET];   /* this frame's, before the copy back  */
static int32_t g_ret_wprev;           /* previous frame's summed wet bus     */
static int32_t g_ret_fade[BB_NRET];   /* Q16 create/destroy ramp             */
static int32_t g_ret_pk  [BB_NRET];   /* post-level abs peak this period     */

/* The safety stage. Armed per slot, and STICKY: once a slot has ever had an
 * incoming feedback edge it keeps its DC blocker and limiter until its type
 * changes. A non-sticky rule would engage and disengage a filter every time a
 * link knob crossed zero, which is a real, small, audible discontinuity
 * mid-performance that would be reported as a bug. Sticky costs one bit and
 * removes it entirely.
 *
 * It stays EXACTLY FALSE for a legacy session: a chamber fed only by voice
 * sends has never had an incoming link, so nothing is armed, not one extra
 * arithmetic operation runs, and the golden hash is safe. */
static int     g_ret_sticky[BB_NRET];
static DcState g_ret_dc[BB_NRET];     /* the ~4Hz highpass, ret.h/dsp.c      */
static int32_t g_ret_lg[BB_NRET];     /* per-return limiter gain, Q16        */
static int32_t g_ret_buslg;           /* the summed-wet limiter's gain, Q16  */

/* THE SUMMED WET BUS GETS ITS OWN DC BLOCKER, AND IT IS THE LAST THING IN THE
 * CHAIN ON PURPOSE.
 *
 * The per-return safety stage ends in an unconditional clamp to the return
 * ceiling, and it has to: the bound on the whole feedback graph is that clamp,
 * and a filter is not allowed to be the last word on amplitude. But a clamp on
 * an asymmetric waveform MANUFACTURES DC -- which is the one thing the per-slot
 * blocker cannot clean up, because it ran a line earlier. A pinned loop is
 * exactly the case where the clamp fires on every sample, so exactly the case
 * where it manufactures the most: measured, a self-linked delay held at the
 * ceiling parked the master a couple of thousand counts off centre, inaudible
 * and permanent, while every peak assertion in the suite stayed green.
 *
 * So the summed bus is blocked after everything that clamps it. Armed by the
 * same bus_hot flag as the wet limiter, so a session with no feedback anywhere
 * never executes it and the CHAMBER's golden hash is untouched. */
static DcState g_ret_busdc;

/* ---- create / destroy: the quiesce handshake -----------------------------
 * The arenas are statically allocated and never move, so there is nothing to
 * pointer-swap: the correct primitive is an epoch-gated quiesce, not an
 * atomic exchange. Fade the slot's output to zero, wait for the render thread
 * to report it stopped running the effect, wait two more render epochs -- the
 * same proof bb_reclaim() uses -- and only then clear the arena and arm the
 * new type. Knob changes never come near this path. */
enum { RQ_IDLE = 0, RQ_FADE, RQ_WAIT, RQ_SWAP };
static struct {
    int st, want;
    unsigned long long e0;      /* epoch latched on entry to RQ_WAIT        */
    unsigned long long t0;      /* bb_now_us() when the current state began */
    unsigned long long ew;      /* epoch when the current state began       */
} g_ret_req[BB_NRET];

/* Semitone -> playback-rate ratio in Q32; see audio.c's comment (index
 * = semitones + 12, offset 12 is exactly 1<<32). */
static const uint64_t PITCH_Q32[25] = {
    2147483648ULL, 2275179671ULL, 2410468894ULL, 2553802834ULL,
    2705659104ULL, 2866542664ULL, 3036987106ULL, 3217556019ULL,
    3408844448ULL, 3611480456ULL, 3826126808ULL, 4053482775ULL,
    4294967296ULL,
    4550359342ULL, 4820937789ULL, 5107665669ULL, 5411318208ULL,
    5733085329ULL, 6073974212ULL, 6435112038ULL, 6817688895ULL,
    7222960912ULL, 7652253616ULL, 8107818609ULL, 8589934592ULL,
};

/* ---- small ring pub/sub helpers ------------------------------------------ */
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

/* ---- one layer's controls, snapshotted once per period -------------------- */
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
    /* NO `send` FIELD, DELIBERATELY. The voice sends are part of the routing
     * matrix and are read ONCE per period by ret_snapshot(), through the
     * slot-0 alias. A second read here would let the voice loop and the
     * routing snapshot see bb.layer[L].send at two different instants and
     * disagree by one knob step -- which is not a crash, not a warning, and
     * exactly the kind of thing that breaks a bit-exactness proof silently. */
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

/* ---- the routing matrix, snapshotted once per period ----------------------
 *
 * Everything the frame loop needs about the graph, resolved into PACKED EDGE
 * LISTS. Packed rather than scanned, for one reason that matters more than
 * speed: with a legacy session -- one CHAMBER, no links, no DRY or WET row --
 * every list but the voice sends is empty and the voice list holds exactly the
 * terms the old `if (sn->send) verb_in += ...` produced, in the same order.
 * The legacy path therefore executes the same instruction sequence it always
 * did, which is what makes the bit-exactness claim inspectable rather than
 * merely tested.
 *
 * A dead slot is not branched over, it is not VISITED: rb.live[] holds only
 * the slots that run. That is also what preserves verb_level 0 as a bit-exact
 * bypass rather than a merely silent one -- an absent slot's effect is not
 * called, so its comb and allpass state does not advance, exactly as
 * `if (v_level > 0)` used to arrange. */
typedef struct {
    int nlive, live[BB_NRET];        /* packed live slots, ascending          */
    int ran[BB_NRET];                /* per slot: was it run this period      */
    int bus_hot;                     /* the graph contains feedback at all    */

    /* per-source edge lists; amounts are pre-clamped */
    int nv[BB_NLAYER];
    int vr[BB_NLAYER][BB_NRET], va[BB_NLAYER][BB_NRET];   /* voices  0..255 */
    int nl, lr[BB_NRET], la[BB_NRET];                     /* LICKS   0..255 */
    int nd, dr[BB_NRET], da[BB_NRET];                     /* DRY     0..255 */
    int nw, wr[BB_NRET], wa[BB_NRET];                     /* WET     0..255 */
    int nk, kt[BB_NRET * BB_NRET],                        /* links   0..256 */
            kf[BB_NRET * BB_NRET],
            ka[BB_NRET * BB_NRET];

    RetSnap s[BB_NRET];
} RetBus;

static void ret_snapshot(RetBus *rb, int rate, uint32_t beat_len,
                         uint32_t bar_len, int panicked)
{
    int lvl[BB_NRET];

    rb->nlive   = 0;
    rb->bus_hot = 0;

    for (int r = 0; r < BB_NRET; r++) {
        /* acquire on type and arm: the swap side releases them after it has
         * zeroed the arena, so the cleared memory is visible to the first
         * ret_run() that sees the new type. */
        int type = atomic_load_explicit(&bb.ret[r].type, memory_order_acquire);
        int arm  = atomic_load_explicit(&bb.ret[r].arm,  memory_order_acquire);
        int mute = atomic_load_explicit(&bb.ret[r].mute, memory_order_relaxed);

        int valid = type > RET_NONE && type < RET_NTYPE;
        lvl[r] = mute ? 0 : bb_clampi(ret_level_load(r), 0, 256);

        /* `feeds` keeps a slot alive when its LEVEL is down but its output
         * still reaches something -- a reverb at zero return level driving a
         * gate is a real patch. `raw_hot` is the safety arming test: does
         * anything a frame old arrive at this slot. Both are read from the
         * true matrix even while panicked, so that a held PANIC lets loops
         * DECAY instead of freezing them for the release. */
        int feeds = 0, raw_hot = 0;
        for (int q = 0; q < BB_NRET; q++) {
            if (atomic_load_explicit(&bb.ret_link[r][q], memory_order_relaxed))
                feeds = 1;
            if (atomic_load_explicit(&bb.ret_link[q][r], memory_order_relaxed))
                raw_hot = 1;
            if (atomic_load_explicit(&bb.ret_send[BB_RET_SRC_WET][q],
                                     memory_order_relaxed))
                feeds = 1;
        }
        if (atomic_load_explicit(&bb.ret_send[BB_RET_SRC_WET][r],
                                 memory_order_relaxed))
            raw_hot = 1;

        g_ret_sticky[r] |= raw_hot;
        rb->s[r].hot = g_ret_sticky[r];
        if (g_ret_sticky[r]) rb->bus_hot = 1;

        /* LIVENESS, and the `lvl > 0` term in it IS the bit-exact bypass. A
         * non-live slot is absent from rb.live[], so its effect is NOT CALLED
         * and its comb and allpass state does not advance -- which is exactly
         * what `if (v_level > 0)` used to arrange, and is why closing the
         * return is bit-exact rather than merely silent. Mute therefore also
         * FREEZES a slot's tail, the same semantics verb_level 0 has always
         * had; that is documented on the mute button.
         *
         * The fade term is deliberately gated on `!arm`. The fade exists for
         * the create/destroy handshake -- it keeps a slot running while its
         * tail rings out before its type is swapped -- and NOT for the level
         * knob. Without the gate, a slot sitting at its initial fade of 65536
         * would be live at level 0 forever: ret_active would over-report, the
         * effect would run under a zero multiply, and the bypass would stop
         * being bit-exact in the one way a "does it make sound" test cannot
         * see. */
        int run_wanted = arm && (lvl[r] > 0 || feeds);
        int live = valid && (run_wanted || (!arm && g_ret_fade[r] > 0));
        rb->ran[r] = live;
        if (!live) {
            /* A slot that stops running must stop contributing to the link
             * matrix too, or its last output sits in g_ret_prev forever and
             * every link out of it becomes a DC source. */
            g_ret_prev[r] = 0;
            g_ret_cur[r]  = 0;
            /* An EMPTY slot has no tail to fade, so its ramp is snapped to the
             * target rather than left stale -- otherwise the quiesce handshake
             * waits forever for a fade that nothing is driving, and create()
             * on an empty slot never lands.
             *
             * A slot that holds a real effect keeps whatever the ramp left it,
             * which is what gives a freshly created return a genuine fade-in
             * while a closed CHAMBER, whose fade has never moved off 65536,
             * re-opens instantly exactly as it always did. */
            if (!valid) g_ret_fade[r] = arm ? 65536 : 0;
            continue;
        }
        rb->live[rb->nlive++] = r;

        /* IN fields, read through the slot-0 alias -- this is the ONLY place
         * the render thread reads a return knob. */
        RetSnap *s = &rb->s[r];
        s->type  = type;
        s->level = lvl[r];
        s->sync  = bb_clampi(atomic_load_explicit(&bb.ret[r].sync,
                                                  memory_order_relaxed), 0, 10);
        for (int p = 0; p < BB_RET_NPARAM; p++)
            s->p[p] = bb_clampi(ret_param_load(r, p), 0, 255);

        ret_period(r, s, rate, beat_len, bar_len);

        /* Bus state, after ret_period so it cannot be clobbered by it. The
         * level is re-clamped for the same reason: a hand-edited session must
         * not be able to push a multiply out of range. */
        s->level    = lvl[r];
        s->hot      = g_ret_sticky[r];
        s->fade_tgt = arm ? 65536 : 0;
    }

    /* ---- the edge lists ---------------------------------------------------
     * Only LIVE targets get edges, because rin[] is zeroed (and read) only for
     * live slots. A link from a dead source is dropped rather than carried at
     * zero: g_ret_prev is zero there anyway, so the two are identical and the
     * shorter list is the cheaper one.
     *
     * PANIC zeroes every amount. Not the master gain -- that is already
     * handled -- but the matrix itself, so a feedback network decays while you
     * are muted instead of being handed back to you saturated the moment you
     * let go. */
    for (int L = 0; L < BB_NLAYER; L++) {
        rb->nv[L] = 0;
        for (int j = 0; j < rb->nlive; j++) {
            int r = rb->live[j];
            int a = panicked ? 0 : bb_clampi(ret_send_load(L, r), 0, 255);
            if (a) { rb->vr[L][rb->nv[L]] = r; rb->va[L][rb->nv[L]] = a; rb->nv[L]++; }
        }
    }

    rb->nl = rb->nd = rb->nw = rb->nk = 0;
    for (int j = 0; j < rb->nlive; j++) {
        int r = rb->live[j], a;

        a = panicked ? 0 : bb_clampi(ret_send_load(BB_RET_SRC_LICKS, r), 0, 255);
        if (a) { rb->lr[rb->nl] = r; rb->la[rb->nl] = a; rb->nl++; }

        a = panicked ? 0 : bb_clampi(ret_send_load(BB_RET_SRC_DRY, r), 0, 255);
        if (a) { rb->dr[rb->nd] = r; rb->da[rb->nd] = a; rb->nd++; }

        a = panicked ? 0 : bb_clampi(ret_send_load(BB_RET_SRC_WET, r), 0, 255);
        if (a) { rb->wr[rb->nw] = r; rb->wa[rb->nw] = a; rb->nw++; }
    }

    for (int jt = 0; jt < rb->nlive; jt++) {
        int to = rb->live[jt];
        for (int jf = 0; jf < rb->nlive; jf++) {
            int from = rb->live[jf];
            int a = panicked ? 0
                  : bb_clampi(atomic_load_explicit(&bb.ret_link[from][to],
                                                   memory_order_relaxed), 0, 256);
            if (a) {
                rb->kt[rb->nk] = to;
                rb->kf[rb->nk] = from;
                rb->ka[rb->nk] = a;
                rb->nk++;
            }
        }
    }
}

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

/* ---- master phrase looper ------------------------------------------------ */
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

void bb_engine_loop_command(int cmd)
{
    atomic_store(&bb.loop_cmd, cmd);
}

void bb_engine_note_on(int layer, int midi_note, int velocity)
{
    if (layer < 0 || layer >= BB_NLAYER) return;
    /* Center on key 60 (middle C) and ask for a whole tone of range. */
    int semis = (midi_note - 60);
    if (semis < -24) semis = -24;
    if (semis > 24)  semis = 24;
    atomic_store(&bb.layer[layer].mtrans, semis);
    atomic_store(&bb.layer[layer].mvel, bb_clampi(velocity * 2, 1, 256));
    atomic_store(&bb.layer[layer].mtrig, 1);
    atomic_store(&bb.layer[layer].on, 1);
}

void bb_engine_note_off(int layer)
{
    if (layer < 0 || layer >= BB_NLAYER) return;
    atomic_store(&bb.layer[layer].mtrans, 0);
}

void bb_engine_key_transpose(int layer, int semitones)
{
    if (layer < 0 || layer >= BB_NLAYER) return;
    atomic_store(&bb.layer[layer].mtrans, bb_clampi(semitones, -24, 24));
}

/* CC maps to a layer's live expression parameter, scaled to 0..255. The
 * mapping is deliberately simple at this stage -- p0 under your hand is
 * better than a hidden matrix. Only the advertised CC 001 (mod wheel) -> p0
 * mapping is live; anything else (sustain pedal, bank select, channel
 * volume spam) must not stomp the patch. */
void bb_engine_cc(int layer, int cc, int value)
{
    if (layer < 0 || layer >= BB_NLAYER) return;
    if (cc != 1) return;
    atomic_store(&bb.layer[layer].param[0],
                 bb_clampi(value * 2, 0, 255));
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

/* ======================================================================== */
/*  Step sampler (R1)                                                        */
/* ======================================================================== */

/* Snapshot the pattern controls once per period, the same way layers are
 * snapshotted. Runs on the audio thread; never allocates. */
static void sampler_snapshot(void)
{
    for (int s = 0; s < BB_SAMPLER; s++) {
        const SamplerSlot *sl = &bb.sampler[s];
        const SmpBuf *buf = atomic_load(&g_smp_buf[s]);
        if (buf != g_smp_cur[s]) {
            /* The UI swapped (or cleared) this slot's sample mid-flight.
             * Stop the voice so a running play position from the old
             * buffer is never applied to the new one. */
            g_smp_cur[s] = buf;
            g_smp_run[s] = 0;
            g_smp_pos[s] = 0;
        }
        if (buf) {
            g_smp_ds[s]    = buf->data;
            g_smp_dlen[s]  = (uint32_t)buf->n;
            g_smp_drate[s] = (uint32_t)buf->rate;
        } else {
            g_smp_ds[s]    = NULL;
            g_smp_dlen[s]  = 0;
            g_smp_drate[s] = 0;
        }
        g_smp_sn[s]    = atomic_load_explicit(&sl->on, memory_order_relaxed);
        g_smp_mute[s]  = atomic_load_explicit(&sl->mute, memory_order_relaxed);
        g_smp_solo[s]  = atomic_load_explicit(&sl->solo, memory_order_relaxed);
        g_smp_choke[s] = bb_clampi(atomic_load_explicit(&sl->ctl[SMP_CTL_CHOKE],
                                                       memory_order_relaxed), 0, 4);
        for (int i = 0; i < BB_STEPS; i++) {
            g_smp_gate[s][i]  = atomic_load_explicit(&sl->gate[i], memory_order_relaxed);
            g_smp_pitch[s][i] = atomic_load_explicit(&sl->pitch[i], memory_order_relaxed);
            g_smp_vel[s][i]   = atomic_load_explicit(&sl->vel[i], memory_order_relaxed);
        }
        g_smp_vtgt[s] = (g_smp_sn[s] && !g_smp_mute[s])
            ? (bb_clampi(atomic_load_explicit(&sl->ctl[SMP_CTL_LEVEL],
                                              memory_order_relaxed), 0, 256) << 8)
            : 0;
    }
}

/* True when any armed slot carries at least one set gate: the playhead is
 * then meaningful even with no sequenced synth voice. */
static int sampler_any_gate(void)
{
    for (int s = 0; s < BB_SAMPLER; s++) {
        if (!g_smp_sn[s]) continue;
        for (int i = 0; i < BB_STEPS; i++)
            if (g_smp_gate[s][i] != SMP_GATE_OFF) return 1;
    }
    return 0;
}

/* One output frame of the step sampler: fire slots at the step boundary,
 * then advance every sounding slot and sum it into the master mix. */
static inline void sampler_process(int32_t *mix, uint32_t tick, int rate, int32_t atk_inc)
{
    int solo = 0;
    for (int s = 0; s < BB_SAMPLER; s++) if (g_smp_solo[s]) { solo = 1; break; }

    if (tick != g_smp_tick) {
        g_smp_tick = tick;
        for (int s = 0; s < BB_SAMPLER; s++) {
            if (!g_smp_sn[s] || g_smp_mute[s]) continue;
            if (solo && !g_smp_solo[s]) continue;
            int st = (int)(tick % BB_STEPS);
            if (g_smp_gate[s][st] == SMP_GATE_OFF) continue;
            if (!g_smp_ds[s] || g_smp_dlen[s] == 0) continue;

            int pc  = bb_clampi(g_smp_pitch[s][st], -12, 12);
            int vel = bb_clampi(g_smp_vel[s][st], 0, 255);
            g_smp_pos[s] = 0;
            g_smp_run[s] = 1;
            g_smp_tgt[s] = vel << 8;
            g_smp_amp[s] = 64;
            uint64_t fr    = g_smp_drate[s] > 0 ? g_smp_drate[s] : 44100u;
            uint64_t orate = rate > 0 ? (uint32_t)rate : 44100u;
            g_smp_inc[s] = (int64_t)((PITCH_Q32[pc + 12] * fr) / orate);
            if (g_smp_inc[s] < 1) g_smp_inc[s] = 1;

            int grp = g_smp_choke[s];
            if (grp != 0)
                for (int t = 0; t < BB_SAMPLER; t++)
                    if (t != s && g_smp_run[t] && g_smp_choke[t] == grp)
                        g_smp_run[t] = 0;
        }
    }

    for (int s = 0; s < BB_SAMPLER; s++) {
        if (g_smp_vol[s] < g_smp_vtgt[s]) {
            g_smp_vol[s] += 32; if (g_smp_vol[s] > g_smp_vtgt[s]) g_smp_vol[s] = g_smp_vtgt[s];
        } else if (g_smp_vol[s] > g_smp_vtgt[s]) {
            g_smp_vol[s] -= 32; if (g_smp_vol[s] < g_smp_vtgt[s]) g_smp_vol[s] = g_smp_vtgt[s];
        }

        if (!g_smp_run[s] || !g_smp_ds[s]) continue;
        if (!g_smp_sn[s] || g_smp_mute[s] || g_smp_vol[s] == 0) continue;
        if (solo && !g_smp_solo[s]) continue;

        uint32_t idx = (uint32_t)(g_smp_pos[s] >> 32);
        if (idx >= g_smp_dlen[s]) { g_smp_run[s] = 0; continue; }
        int16_t v = g_smp_ds[s][idx];
        g_smp_pos[s] += (uint64_t)g_smp_inc[s];
        if (g_smp_amp[s] < g_smp_tgt[s]) {
            g_smp_amp[s] += atk_inc;
            if (g_smp_amp[s] > g_smp_tgt[s]) g_smp_amp[s] = g_smp_tgt[s];
        }
        int32_t sel = (int32_t)(((int64_t)g_smp_amp[s] * g_smp_vol[s]) >> 16);
        int32_t con = (int32_t)(((int64_t)v * sel) >> 16);
        *mix += con;
        if (con < 0) con = -con;
        if (con > g_smp_pk[s]) g_smp_pk[s] = con;
    }
}

/* ======================================================================== */
/*  THE RETURN BUS: the public API (UI thread)                               */
/* ======================================================================== */

static int ret_slot_ok(int s) { return s >= 0 && s < BB_NRET; }

/* ---- knobs. One atomic store each, effective next period ------------------
 * Every one of these goes through the slot-0 alias in bytebeat.h, so writing
 * return 0's LEVEL writes bb.verb_level and nothing else. There is no second
 * copy to keep in sync. */
void bb_engine_ret_level(int slot, int v)
{ if (ret_slot_ok(slot)) ret_level_store(slot, bb_clampi(v, 0, 256)); }

int bb_engine_ret_level_get(int slot)
{ return ret_slot_ok(slot) ? bb_clampi(ret_level_load(slot), 0, 256) : 0; }

void bb_engine_ret_param(int slot, int p, int v)
{
    if (ret_slot_ok(slot) && p >= 0 && p < BB_RET_NPARAM)
        ret_param_store(slot, p, bb_clampi(v, 0, 255));
}

int bb_engine_ret_param_get(int slot, int p)
{
    return (ret_slot_ok(slot) && p >= 0 && p < BB_RET_NPARAM)
         ? bb_clampi(ret_param_load(slot, p), 0, 255) : 0;
}

void bb_engine_ret_sync(int slot, int v)
{
    if (ret_slot_ok(slot))
        atomic_store_explicit(&bb.ret[slot].sync, bb_clampi(v, 0, 10),
                              memory_order_relaxed);
}

int bb_engine_ret_sync_get(int slot)
{
    return ret_slot_ok(slot)
         ? atomic_load_explicit(&bb.ret[slot].sync, memory_order_relaxed) : 0;
}

void bb_engine_ret_mute(int slot, int on)
{
    if (ret_slot_ok(slot))
        atomic_store_explicit(&bb.ret[slot].mute, on ? 1 : 0,
                              memory_order_relaxed);
}

int bb_engine_ret_mute_get(int slot)
{
    return ret_slot_ok(slot)
         ? atomic_load_explicit(&bb.ret[slot].mute, memory_order_relaxed) : 0;
}

int bb_engine_ret_type_get(int slot)
{
    return ret_slot_ok(slot)
         ? atomic_load_explicit(&bb.ret[slot].type, memory_order_relaxed)
         : RET_NONE;
}

/* ---- the matrix ---------------------------------------------------------- */
void bb_engine_ret_send(int src, int slot, int amt)
{
    if (src >= 0 && src < BB_RET_NSRC && ret_slot_ok(slot))
        ret_send_store(src, slot, bb_clampi(amt, 0, 255));
}

int bb_engine_ret_send_get(int src, int slot)
{
    return (src >= 0 && src < BB_RET_NSRC && ret_slot_ok(slot))
         ? bb_clampi(ret_send_load(src, slot), 0, 255) : 0;
}

void bb_engine_ret_link(int from, int to, int amt)
{
    if (ret_slot_ok(from) && ret_slot_ok(to))
        atomic_store_explicit(&bb.ret_link[from][to], bb_clampi(amt, 0, 256),
                              memory_order_relaxed);
}

int bb_engine_ret_link_get(int from, int to)
{
    return (ret_slot_ok(from) && ret_slot_ok(to))
         ? atomic_load_explicit(&bb.ret_link[from][to], memory_order_relaxed) : 0;
}

void bb_engine_ret_panic(void)
{
    for (int a = 0; a < BB_NRET; a++) {
        ret_level_store(a, 0);
        for (int b = 0; b < BB_NRET; b++)
            atomic_store_explicit(&bb.ret_link[a][b], 0, memory_order_relaxed);
    }
}

/* ---- create / destroy ----------------------------------------------------
 * How long to wait on a render thread that may not be there. bb.epoch is
 * incremented at the top of EVERY period, so "the epoch has not moved for
 * several periods" is the only honest liveness signal available. bb.budget_us
 * is that period's length in microseconds, published by the render loop, so
 * the window scales with the actual buffer size instead of guessing at it --
 * and a 4096-frame period at 8 kHz does not get the same 2 ms grace as a
 * 64-frame period at 96 kHz. */
static unsigned long long ret_wait_budget_us(void)
{
    int b = atomic_load_explicit(&bb.budget_us, memory_order_relaxed);
    long long us = (long long)(b > 0 ? b : 0) * 3 + 2000;
    if (us < 2000)   us = 2000;
    if (us > 250000) us = 250000;
    return (unsigned long long)us;
}

/* Clear everything the BUS owns for one slot. The effect's own state and
 * arena belong to ret_reset(). */
static void ret_bus_clear(int r)
{
    g_ret_sticky[r] = 0;
    g_ret_dc[r].x1  = 0;
    g_ret_dc[r].y1  = 0;
    g_ret_lg[r]     = BB_RET_GAIN_UNITY;
    g_ret_prev[r]   = 0;
    g_ret_cur[r]    = 0;
}

static void ret_service_slot(int r)
{
    if (g_ret_req[r].st == RQ_IDLE) return;

    unsigned long long ep  = atomic_load(&bb.epoch);
    unsigned long long now = bb_now_us();

    /* Any epoch progress re-arms the clock, so `stalled` means precisely
     * "the render thread has not started a period in a while" -- not "this
     * request is taking a while", which would be a race against a slow but
     * perfectly healthy device. */
    if (ep != g_ret_req[r].ew) { g_ret_req[r].ew = ep; g_ret_req[r].t0 = now; }
    int stalled = (now - g_ret_req[r].t0) > ret_wait_budget_us();

    switch (g_ret_req[r].st) {
    case RQ_FADE:
        /* Waiting for the render thread to ramp the slot to silence and stop
         * calling its effect. If it is not running at all, the fade will never
         * complete on its own -- and nothing can be inside ret_run() either,
         * so there is nothing to wait for. */
        if (atomic_load_explicit(&bb.ret[r].quiet, memory_order_relaxed)) {
            g_ret_req[r].st = RQ_WAIT;
            g_ret_req[r].e0 = ep;
        } else if (stalled) {
            g_ret_fade[r]   = 0;
            g_ret_req[r].st = RQ_SWAP;
        }
        break;

    case RQ_WAIT:
        /* Two full render epochs: the same proof bb_reclaim() uses, applied
         * to STATE rather than to memory. After it, the render thread cannot
         * still be inside a call that saw the old type. */
        if (ep >= g_ret_req[r].e0 + 2 || stalled) g_ret_req[r].st = RQ_SWAP;
        break;

    default:
        break;
    }

    if (g_ret_req[r].st == RQ_SWAP) {
        int want = g_ret_req[r].want;

        /* DESTROYING CLEARS THE ARENA TOO, and the comment that used to sit
         * here said the opposite: that the arena would be cleared on the next
         * create, so there was no point walking a megabyte to make silence
         * quieter. That reasoning holds only if a create clears everything the
         * previous type dirtied, and it does not -- ret_reset() sizes its clear
         * by footprint, and a CHAMBER's footprint is a quarter of a DELAY's.
         * Destroy a DELAY, create a CHAMBER, and three quarters of a megabyte
         * of the delay's audio is still in the pool. It is inaudible under the
         * CHAMBER and it comes straight back the moment anything with a longer
         * reach is created in that slot.
         *
         * ret_reset() now clears the union of the outgoing and the incoming
         * footprint, so the honest thing is to call it on every swap and let it
         * decide. Destroying a slot that held nothing still costs nothing. */
        ret_reset(r, want);

        /* THE TYPE'S DEFAULT KNOB VALUES ARE NOT STAMPED HERE, DELIBERATELY.
         * bb_config_load() also drives type changes, and a create that wrote
         * ret_param_def would stamp over the parameters it just read out of the
         * session file. The UI applies them on an explicit user create instead
         * -- see MixerPanel::typeMenu, which says the same thing from the other
         * side. Every effect therefore has to behave itself at ANY parameter
         * setting, including the all-zero one a slot that has never held an
         * effect carries; that is a property of the types, and it is where it
         * belongs. */

        ret_bus_clear(r);
        /* release, paired with the render snapshot's acquire loads, so the
         * zeroed arena is visible before the first ret_run() of the new type */
        atomic_store_explicit(&bb.ret[r].type, want, memory_order_release);
        atomic_store_explicit(&bb.ret[r].arm,  1,    memory_order_release);
        g_ret_req[r].st = RQ_IDLE;
    }
}

void bb_engine_ret_service(void)
{
    for (int r = 0; r < BB_NRET; r++) ret_service_slot(r);
}

int bb_engine_ret_create(int slot, int type)
{
    if (!ret_slot_ok(slot)) return -1;
    if (type < RET_NONE || type >= RET_NTYPE) return -1;

    g_ret_req[slot].want = type;
    g_ret_req[slot].st   = RQ_FADE;
    g_ret_req[slot].ew   = atomic_load(&bb.epoch);
    g_ret_req[slot].t0   = bb_now_us();
    atomic_store_explicit(&bb.ret[slot].quiet, 0, memory_order_relaxed);
    atomic_store_explicit(&bb.ret[slot].arm,   0, memory_order_relaxed);
    return 0;
}

int bb_engine_ret_destroy(int slot)
{
    return bb_engine_ret_create(slot, RET_NONE);
}

int bb_engine_ret_pending(int slot)
{
    return ret_slot_ok(slot) && g_ret_req[slot].st != RQ_IDLE;
}

/* ---- the bulk-edit bracket ------------------------------------------------
 * There is no bb.ret_hold flag: the quiesce IS the hold, and unlike a flag it
 * has no stale-forever failure mode. Both calls are UI thread only. */
void bb_engine_ret_quiesce_all(void)
{
    for (int r = 0; r < BB_NRET; r++) {
        atomic_store_explicit(&bb.ret[r].arm,   0, memory_order_relaxed);
        atomic_store_explicit(&bb.ret[r].quiet, 0, memory_order_relaxed);
    }

    /* The render thread has never run -- startup, before the audio device
     * exists, and every offline caller including the regression suite. There
     * is provably nothing inside ret_run(), so there is nothing to wait for. */
    unsigned long long e0 = atomic_load(&bb.epoch);
    if (e0 == 0) return;

    unsigned long long t0 = bb_now_us(), last_t = t0, last_ep = e0;
    for (;;) {
        unsigned long long ep = atomic_load(&bb.epoch), now = bb_now_us();
        int all = 1;
        for (int r = 0; r < BB_NRET; r++)
            if (!atomic_load_explicit(&bb.ret[r].quiet, memory_order_relaxed)) {
                all = 0;
                break;
            }
        if (all && ep >= e0 + 2) break;
        if (ep != last_ep) { last_ep = ep; last_t = now; }
        else if (now - last_t > ret_wait_budget_us()) break;  /* not running */
        if (now - t0 > 2000000ull) break;   /* hard ceiling; never hang a load */
    }
}

void bb_engine_ret_release_all(void)
{
    for (int r = 0; r < BB_NRET; r++)
        if (g_ret_req[r].st == RQ_IDLE)   /* a pending create owns its own arm */
            atomic_store_explicit(&bb.ret[r].arm, 1, memory_order_release);
}

/* ======================================================================== */
/*  Startup wiring                                                           */
/* ======================================================================== */

void bb_engine_init(int rate)
{
    atomic_store(&bb.rate, bb_clampi(rate, BB_RATE_MIN, BB_RATE_MAX));

    for (int L = 0; L < BB_NLAYER; L++) {
        memset(&g_ctx[L], 0, sizeof g_ctx[L]);
        g_ctx[L].dly = g_delay[L];
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

        /* THE LEVEL RAMP IS SETTLED, NOT ZEROED, AND THE DIFFERENCE IS THE
         * WHOLE POINT.
         *
         * g_lvl[] and the master gain ramp used to be the only two pieces of
         * render state that survived bb_engine_init() untouched. That made
         * bb_engine_init() a reset that did not reset, and it showed up as the
         * thing a reset is supposed to prevent: two runs of an identical
         * scenario, both starting with bb_engine_init(), rendering DIFFERENT
         * samples -- because whatever ran before left the ramp at a different
         * point and the ramp then took a different path into the reverb, whose
         * tail at 8 kHz is far longer than any settle worth rendering.
         *
         * Zeroing them instead would be wrong in the other direction: a ramp
         * forced to 0 and then allowed to walk back up at 32 per frame is a
         * 46 ms fade-in that the engine never used to have, and the CHAMBER's
         * golden hash would move. SNAPPING each ramp to the target its own
         * controls already ask for is the reset that changes nothing about a
         * settled engine and everything about an unsettled one: the golden
         * test settles both ramps before it calls this, so the value it finds
         * here is the value it already had, and the hash does not move.
         *
         * The target is spelled exactly as the render loop spells it, because
         * two expressions for one target is how they drift apart. */
        {
            int on  = atomic_load(&bb.layer[L].on);
            int lvl = bb_clampi(atomic_load(&bb.layer[L].ctl[LCTL_LEVEL]), 0, 256);
            g_lvl[L] = on ? (int32_t)((uint32_t)lvl << 8) : 0;
        }
    }

    /* The master gain ramp, snapped for the same reason and against the same
     * two mutes the render loop honours. The clamp is belt and braces the
     * render loop does not bother with -- bb.gain is published 0..256 by
     * everything that writes it -- and is identical for every value the fader
     * can actually hold. */
    {
        int silent = atomic_load(&bb.mute) || atomic_load(&bb.panic);
        int gtgt   = bb_clampi(atomic_load(&bb.gain), 0, 256);
        g_gain_cur = silent ? 0 : (int32_t)((uint32_t)gtgt << 8);
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

    g_smp_tick = UINT32_MAX;
    for (int s = 0; s < BB_SAMPLER; s++) {
        g_smp_cur[s]  = NULL;
        g_smp_pos[s]  = 0;
        g_smp_inc[s]  = (int64_t)1 << 32;
        g_smp_run[s]  = 0;
        g_smp_tgt[s]  = 0;
        g_smp_amp[s]  = 0;
        g_smp_vol[s]  = 0;
    }

    /* R2 song traffic: -1 means "no pending seek" -- the zero the BSS gives
     * us would read as a request to jump to bar 0. */
    atomic_store(&bb.arr_rec_status, ARR_REC_IDLE);
    atomic_store(&bb.arr_rec_frames, 0);
    atomic_store(&bb.arr_seek_bar, -1);
    memset(g_arr_ctr, 0, sizeof g_arr_ctr);
    memset(g_arr_in,  0, sizeof g_arr_in);
    g_cap_run = 0;
    g_cap_pos = 0;

    /* --- the return bus ---------------------------------------------------
     * ret_reset() clears only the arena the slot's CURRENT type actually
     * touches, which is why this is affordable: the regression suite calls
     * bb_engine_init() seven times, and clearing the 9.25 MiB of pools each
     * time would fault in every page of a session that uses one return. See
     * ret.h's pool comment -- ret_init() deliberately clears nothing.
     *
     * g_ret_fade MUST START AT 65536, not ramp up from 0. If it starts at 0
     * the first ~46 ms of every session differ from the pre-return-bus
     * engine, the chamber's golden hash fails, and it fails for a reason that
     * looks exactly like a DSP bug. */
    ret_init();
    for (int r = 0; r < BB_NRET; r++) {
        int t = atomic_load(&bb.ret[r].type);
        ret_reset(r, t);
        atomic_store(&bb.ret[r].type, r == 0 ? RET_CHAMBER : RET_NONE);
        atomic_store(&bb.ret[r].arm,   1);
        atomic_store(&bb.ret[r].quiet, 0);
        atomic_store(&bb.ret[r].gr,    256);
        g_ret_fade[r]   = 65536;
        g_ret_prev[r]   = 0;
        g_ret_cur[r]    = 0;
        g_ret_pk[r]     = 0;
        g_ret_lg[r]     = BB_RET_GAIN_UNITY;
        g_ret_sticky[r] = 0;
        g_ret_dc[r].x1  = 0;
        g_ret_dc[r].y1  = 0;
        g_ret_req[r].st = RQ_IDLE;
    }
    g_ret_wprev = 0;
    g_ret_buslg = BB_RET_GAIN_UNITY;
    g_ret_busdc.x1 = 0;
    g_ret_busdc.y1 = 0;
    atomic_store(&bb.ret_active, 0);

    verb_reset();               /* == the old chamber reset, on slot 0 */
}

void bb_engine_reset_t(void)      { atomic_store(&bb.reset_t, 1); }
void bb_engine_reset_loop(void)   { atomic_store(&bb.reset_loop, 1); }

/* ======================================================================== */
/*  The render loop                                                          */
/* ======================================================================== */

void bb_engine_render(int16_t *out, int frames, int channels)
{
    if (out == NULL || frames <= 0) return;
    if (channels < 1) channels = 1;
    if (channels > 8) channels = 8;

    /* --- publish that a new period is starting ---------------------------
     * Sequentially consistent, and order between this and the program loads
     * below is load-bearing; see bb_reclaim() for the proof. */
    atomic_fetch_add(&bb.epoch, 1);

    /* --- snapshot the master --------------------------------------------- */
    int rate = atomic_load_explicit(&bb.rate, memory_order_relaxed);
    int mute = atomic_load_explicit(&bb.mute, memory_order_relaxed);
    int pan  = atomic_load_explicit(&bb.panic, memory_order_relaxed);
    int gtgt = atomic_load_explicit(&bb.gain, memory_order_relaxed);
    int byp  = atomic_load_explicit(&bb.bypass, memory_order_relaxed);

    int bpm   = atomic_load_explicit(&bb.gctl[GCTL_BPM], memory_order_relaxed);
    int beats = atomic_load_explicit(&bb.gctl[GCTL_BEATS], memory_order_relaxed);
    int bars  = atomic_load_explicit(&bb.gctl[GCTL_BARS], memory_order_relaxed);

    int lp_bars = atomic_load_explicit(&bb.loop_bars, memory_order_relaxed);
    int lp_mix  = atomic_load_explicit(&bb.loop_mix, memory_order_relaxed);
    int lp_fb   = atomic_load_explicit(&bb.loop_feedback, memory_order_relaxed);
    int lp_od   = atomic_load_explicit(&bb.loop_overdub, memory_order_relaxed);
    int lp_rate = atomic_load_explicit(&bb.loop_rate, memory_order_relaxed);
    int lp_rev  = atomic_load_explicit(&bb.loop_reverse, memory_order_relaxed);
    int lp_slice= atomic_load_explicit(&bb.loop_slice, memory_order_relaxed);
    loop_command();

    /* --- snapshot every layer --------------------------------------------
     * Plain static like every other piece of render state: the engine is
     * single-renderer by contract (exactly two threads, see bytebeat.h), and
     * on Darwin a __thread variable's first access mallocs the TLV block on
     * the audio thread -- a violation of the never-malloc rule. */
    static LSnap ls[BB_NLAYER];
    int any_seq = 0;
    for (int L = 0; L < BB_NLAYER; L++) {
        Layer *ly = &bb.layer[L];
        LSnap *sn = &ls[L];

        sn->prog  = atomic_load(&ly->prog);
        sn->on    = atomic_load_explicit(&ly->on, memory_order_relaxed);
        sn->mode  = atomic_load_explicit(&ly->mode, memory_order_relaxed);
        sn->level = atomic_load_explicit(&ly->ctl[LCTL_LEVEL], memory_order_relaxed);

        for (int i = 0; i < BB_NPARAM; i++)
            sn->p[i] = atomic_load_explicit(&ly->param[i], memory_order_relaxed);

        sn->pp.drive = atomic_load_explicit(&ly->ctl[LCTL_DRIVE], memory_order_relaxed);
        sn->pp.tone  = atomic_load_explicit(&ly->ctl[LCTL_TONE],  memory_order_relaxed);
        sn->pp.crush = atomic_load_explicit(&ly->ctl[LCTL_CRUSH], memory_order_relaxed);
        sn->pp.spc_fb  = atomic_load_explicit(&ly->ctl[LCTL_SPC_FB],   memory_order_relaxed);
        sn->pp.spc_mix = atomic_load_explicit(&ly->ctl[LCTL_SPC_MIX],  memory_order_relaxed);
        sn->pp.spc_freeze = atomic_load_explicit(&ly->ctl[LCTL_SPC_FREEZE],
                                                  memory_order_relaxed);
        sn->pp.bypass = byp;
        sn->spc_time_raw = atomic_load_explicit(&ly->ctl[LCTL_SPC_TIME],
                                                 memory_order_relaxed);
        sn->spc_sync = atomic_load_explicit(&ly->ctl[LCTL_SPC_SYNC],
                                             memory_order_relaxed);

        sn->seq_on = atomic_load_explicit(&ly->seq_on, memory_order_relaxed);
        sn->steps = bb_clampi(atomic_load_explicit(&ly->ctl[LCTL_STEPS],
                                                   memory_order_relaxed), 1, BB_STEPS);
        sn->decay = atomic_load_explicit(&ly->ctl[LCTL_DECAY], memory_order_relaxed);
        sn->motion_mask = atomic_load_explicit(&ly->motion_mask, memory_order_relaxed);

        for (int i = 0; i < BB_STEPS; i++) {
            sn->gate[i]  = atomic_load_explicit(&ly->seq_gate[i],   memory_order_relaxed);
            sn->pitch[i] = atomic_load_explicit(&ly->seq_pitch[i],  memory_order_relaxed);
            sn->ratchet[i] = atomic_load_explicit(&ly->seq_ratchet[i], memory_order_relaxed);
            sn->prob[i] = atomic_load_explicit(&ly->seq_prob[i], memory_order_relaxed);
            for (int q = 0; q < BB_LOCK_COUNT; q++)
                sn->lock[q][i] = atomic_load_explicit(&ly->seq_lock[q][i],
                                                      memory_order_relaxed);
        }
        if (sn->seq_on) any_seq = 1;
    }

    sampler_snapshot();
    int sampler_clock = sampler_any_gate();

    /* --- snapshot the song ------------------------------------------------
     * One pointer load per period, like a Program: every clip this period
     * plays comes from the same immutable snapshot. */
    const ArrSong *sng = atomic_load(&g_song);

    /* --- per-lane capture traffic ----------------------------------------
     * ARMED re-latches the request every period until the bar boundary
     * fires (a re-arm after a cancel must pick up the NEW destination);
     * anything that is not ARMED/RECORDING -- IDLE from a cancel, DONE from
     * ourselves -- stops the copy. The ARMED->RECORDING and
     * RECORDING->DONE edges below are single-shot compare-exchanges so a
     * concurrent cancel is never overwritten. */
    /* Snapshot the arrangement transport and the record source once per
     * period rather than per frame: they are UI-driven, a period is at most a
     * few milliseconds, and a value that changes underneath the render loop
     * mid-buffer would put a discontinuity in the middle of a block. */
    const int arr_on  = atomic_load_explicit(&bb.arr_play, memory_order_relaxed);
    const int rec_src = atomic_load_explicit(&bb.rec_src,  memory_order_relaxed);

    int cap_st = atomic_load(&bb.arr_rec_status);
    if (cap_st == ARR_REC_ARMED) {
        g_cap_dst  = atomic_load(&g_arr_rec_dst);
        g_cap_cap  = atomic_load(&g_arr_rec_cap);
        g_cap_lane = atomic_load(&g_arr_rec_lane);
        g_cap_left = atomic_load(&g_arr_rec_bars);
        g_cap_pos  = 0;
        g_cap_run  = 0;
    } else if (cap_st != ARR_REC_RECORDING) {
        g_cap_run = 0;
    }
    int cap_on = cap_st == ARR_REC_ARMED || cap_st == ARR_REC_RECORDING;

    /* --- loop clock ------------------------------------------------------- */
    uint32_t beat_len = (uint32_t)(((long)rate * 60L) / (bpm > 0 ? bpm : 90));
    if (beat_len < 1) beat_len = 1;
    uint32_t bar_len  = beat_len * (uint32_t)(beats > 0 ? beats : 4);
    uint32_t loop_len = bar_len  * (uint32_t)(bars  > 0 ? bars  : 1);
    if (loop_len < 1) loop_len = 1;
    uint32_t step_len = beat_len / 4;
    if (step_len < 1) step_len = 1;

    /* Plain static, not __thread: the transport must survive the host
     * recreating its IO thread (device/route changes), and TLS access
     * lazily allocates on Darwin. Resets are explicit user actions only
     * (bb_engine_reset_t / bb_engine_reset_loop). */
    static uint32_t t = 0, k = 0, beat_pos = 0, bar_pos = 0, bar_count = 0;

    if (atomic_exchange_explicit(&bb.reset_t, 0, memory_order_relaxed)) {
        t = 0;
        for (int L = 0; L < BB_NLAYER; L++) {
            g_vt[L] = 0;
            g_hit_age[L] = INT32_MAX;

            /* AND THE LEVEL RAMP LANDS ON ITS TARGET, for the same reason the
             * phase accumulator lands on zero: this is the point where the
             * instrument is told to start again from a defined state, and a
             * ramp caught halfway through a fade is not one.
             *
             * It is not tidiness. g_lvl[] is smoothing state that survives
             * bb_engine_init() -- deliberately, because the CHAMBER's golden
             * hash was captured on a binary where it did -- so without this it
             * is a channel from whatever ran BEFORE the restart into what the
             * restart produces. And it does not fade out: a voice ramping in
             * from wherever the last scene left the fader feeds the reverb a
             * different envelope for the first fifty milliseconds, and a
             * reverb tail at 8 kHz is still carrying that difference thousands
             * of frames later. Two renders of an identical scenario, both
             * started with a reset, came out different samples because of it.
             *
             * Snapping rather than zeroing is what keeps the golden hash where
             * it is: the target is read from the controls that are already
             * set, so an engine that was ALREADY settled -- which is what the
             * hash's two-stage settle arranges -- finds the value it already
             * had and nothing moves. */
            {
                int on  = atomic_load_explicit(&bb.layer[L].on,
                                               memory_order_relaxed);
                int lvl = bb_clampi(atomic_load_explicit(
                              &bb.layer[L].ctl[LCTL_LEVEL],
                              memory_order_relaxed), 0, 256);
                g_lvl[L] = on ? (int32_t)((uint32_t)lvl << 8) : 0;
            }
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
        /* The step sampler's edge detector must re-fire step 0 exactly like
         * the synth layers' just did, even when the restart lands inside the
         * first 16th (tick would still equal g_smp_tick otherwise). */
        g_smp_tick = UINT32_MAX;
        /* Restarting the transport restarts every clip window too: a clip
         * whose window contains both the old and the new position would
         * otherwise keep its stale frames-into-window counter. */
        memset(g_arr_in,  0, sizeof g_arr_in);
        memset(g_arr_ctr, 0, sizeof g_arr_ctr);
    }

    /* A pending song seek is consumed exactly like reset_loop, except the
     * bar counter lands on the requested bar instead of zero. Every edge
     * detector that keys off the loop clock is re-armed -- the layer tick
     * latches, the step sampler's tick, and each clip's window state -- so
     * step 0 of the target bar fires and in-window clips restart their
     * audio from the window they just re-entered. */
    {
        int sk = atomic_exchange_explicit(&bb.arr_seek_bar, -1,
                                          memory_order_relaxed);
        if (sk >= 0) {
            k = beat_pos = bar_pos = 0;
            bar_count = (uint32_t)sk;
            g_loop_phase = 0;
            for (int L = 0; L < BB_NLAYER; L++) {
                g_last_step[L] = -1;
                g_last_tick[L] = UINT32_MAX;
                g_last_sub[L] = -1;
            }
            g_smp_tick = UINT32_MAX;
            memset(g_arr_in,  0, sizeof g_arr_in);
            memset(g_arr_ctr, 0, sizeof g_arr_ctr);
        }
    }
    if (k        >= loop_len) k        %= loop_len;
    if (beat_pos >= beat_len) beat_pos %= beat_len;
    if (bar_pos  >= bar_len)  bar_pos  %= bar_len;

    /* A NEW song snapshot invalidates the per-INDEX window counters: after
     * a delete or reorder, index c can describe a different clip than the
     * one whose counter lives in g_arr_ctr[c], and a surviving clip would
     * jump mid-audio. Re-key every counter from the timeline instead --
     * frames since the window opened, as if the clip had been there all
     * along -- so an edit mid-playback never restarts or teleports audio.
     * (The reset/seek branches above still zero the counters wholesale;
     * that is their documented restart-at-entry behaviour and this block
     * only runs when the SNAPSHOT changed.) */
    {
        static const ArrSong *g_arr_snap;
        if (sng != g_arr_snap) {
            memset(g_arr_in,  0, sizeof g_arr_in);
            memset(g_arr_ctr, 0, sizeof g_arr_ctr);
            if (sng) {
                for (int c = 0; c < sng->nclips; c++) {
                    const ArrClip *cp = &sng->clip[c];
                    if ((uint64_t)bar_count >= (uint64_t)cp->start_bar &&
                        (uint64_t)bar_count <  (uint64_t)cp->start_bar
                                             + (uint64_t)cp->len_bars) {
                        uint64_t off = (uint64_t)(bar_count - cp->start_bar)
                                         * (uint64_t)bar_len
                                     + (uint64_t)bar_pos;
                        g_arr_in[c]  = 1;
                        g_arr_ctr[c] = off > 0xffffffffu ? 0xffffffffu
                                                         : (uint32_t)off;
                    }
                }
            }
            g_arr_snap = sng;
        }
    }

    int32_t atk_inc = 65536 / (rate / 500 > 0 ? rate / 500 : 1);
    if (atk_inc < 1) atk_inc = 1;

    for (int L = 0; L < BB_NLAYER; L++) {
        g_ctx[L].sr = rate;
        g_ctx[L].bl = (int32_t)beat_len;
        g_ctx[L].ll = (int32_t)loop_len;
    }

    /* gtgt is published 0..256 by everything that writes bb.gain, but it is
     * read here raw, and a signed left shift of a negative value is undefined
     * -- so shift the bit pattern, not the number. Identical result for every
     * value the master fader can actually hold. */
    int target_gain = (mute || pan) ? 0 : (int)((uint32_t)gtgt << 8);
    int clipped = 0;
    int32_t layer_pk[BB_NLAYER] = { 0 };   /* per-voice abs peak (meters) */

    /* --- snapshot the RETURN BUS -----------------------------------------
     * Plain static, not __thread, for the same Darwin TLV reason as LSnap: a
     * __thread variable's first touch mallocs its block on the audio thread.
     *
     * This is where the CHAMBER's per-period block used to be, and slot 0 is
     * still the CHAMBER: with no other returns enabled and an empty matrix,
     * rb.live[] is {0}, rb.nv[L] is 1 exactly when bb.layer[L].send is
     * nonzero, and every other edge list is empty -- so the frame loop below
     * reduces term for term and branch for branch to the code it replaced.
     * verb_level 0 still means slot 0 is not live, is not visited, and its
     * reverb state does not advance: a bit-exact bypass, not a silent one. */
    static RetBus rb;
    ret_snapshot(&rb, rate, beat_len, bar_len, pan);

    /* bb_now_us() is a user-mode counter read on every platform: no syscall,
     * no allocation, no lock. Safe here; see bb_platform.h. */
    uint64_t c0 = bb_now_us();

    for (int i = 0; i < frames; i++) {
        int32_t mix = 0;
        int32_t rin[BB_NRET];     /* this frame's input to each live return */
        for (int j = 0; j < rb.nlive; j++) rin[rb.live[j]] = 0;
        int32_t cap_smp = 0;      /* armed lane's post-fader contribution */
        int32_t clip_sum = 0;     /* this frame's arrangement playback, kept
                                   * apart so REC can leave it out (BB_REC_LIVE) */
        uint32_t tick = k / step_len;
        uint32_t in_step = k % step_len;

        for (int L = 0; L < BB_NLAYER; L++) {
            LSnap *sn = &ls[L];

            int step  = sn->seq_on ? (int)(tick % (uint32_t)sn->steps) : 0;
            int gate  = sn->seq_on ? sn->gate[step]  : GATE_ON;
            int semis = sn->seq_on ? sn->pitch[step] : 0;

            /* MIDI / hardware transpose rides on top of whatever the
             * sequencer asks for, so a keyboard note can re-pitch a voice
             * without touching its pattern. */
            int mtrans = atomic_load_explicit(&bb.layer[L].mtrans,
                                              memory_order_relaxed);
            if (mtrans != 0) semis += bb_clampi(mtrans, -24, 24);

            int level = locked(sn, LOCK_LEVEL, step, sn->level,
                               in_step, step_len);

            int32_t ltgt = sn->on ? (bb_clampi(level, 0, 256) << 8) : 0;
            if (g_lvl[L] < ltgt) {
                g_lvl[L] += 32; if (g_lvl[L] > ltgt) g_lvl[L] = ltgt;
            } else if (g_lvl[L] > ltgt) {
                g_lvl[L] -= 32; if (g_lvl[L] < ltgt) g_lvl[L] = ltgt;
            }
            if (g_lvl[L] == 0 || !sn->prog) continue;

            int trigger = 0;

            /* A one-shot MIDI trigger fires like a hit, independent of the
             * sequencer: arm the envelope peak and clear the impulse flag. */
            if (atomic_exchange_explicit(&bb.layer[L].mtrig, 0,
                                          memory_order_relaxed)) {
                trigger = 1;
                g_env_peak[L] = 65536;
                g_env_atk[L]  = 1;
                g_hit_age[L]  = 0;
                g_hit_vel[L]  = bb_clampi(atomic_load_explicit(&bb.layer[L].mvel,
                                                                memory_order_relaxed),
                                          1, 256);
                g_env[L] = 0;
            }

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

            g_ctx[L].dw = (g_ctx[L].dw + 1u) & EXPR_DELAY_MASK;

            /* The shifts go through uint32_t. `(v & 0xff) - 128` is negative
             * for the whole bottom half of BYTE mode and a signed left shift
             * of a negative value is undefined behaviour -- Clang only made it
             * behave because the Makefile passes -fwrapv, and MSVC has no
             * -fwrapv to pass. Shifting the unsigned bit pattern and
             * reinterpreting it is the same bits on every compiler, which is
             * exactly the trick expr.c's U()/S() already play with the VM's
             * arithmetic. Not one sample changes. */
            int32_t s;
            switch (sn->mode) {
            case BB_BYTE:   s = (int32_t)((uint32_t)((v & 0xff) - 128) << 8); break;
            case BB_SIGNED: s = (int32_t)((uint32_t)(int8_t)(v & 0xff) << 8); break;
            default:        s = (int32_t)(int16_t)(v & 0xffff);               break;
            }

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
            s = dsp_dcblock(&g_post[L], s);

            int32_t con = (int32_t)(((int64_t)s * g_lvl[L]) >> 16);
            mix += con;
            if (cap_on && g_cap_lane == L)
                cap_smp += con;   /* voice-lane capture tap, pre-abs */
            /* Post-fader sends, exactly where the single CHAMBER send tapped.
             * With one return and one nonzero send this is the identical
             * multiply-shift-add on the identical value, in the identical L
             * order -- rb.va[L][0] IS bb.layer[L].send, read once. */
            for (int e = 0; e < rb.nv[L]; e++)
                rin[rb.vr[L][e]] += (int32_t)(((int64_t)con * rb.va[L][e]) >> 8);
            if (con < 0) con = -con;
            if (con > layer_pk[L]) layer_pk[L] = con;
        }

        {
            int32_t premix = mix;
            sampler_process(&mix, tick, rate, atk_inc);
            int32_t sbus = mix - premix;
            for (int e = 0; e < rb.nl; e++)
                rin[rb.lr[e]] += (int32_t)(((int64_t)sbus * rb.la[e]) >> 8);
            if (cap_on && g_cap_lane == 8)
                cap_smp += mix - premix;   /* LICKS-bus capture tap */
        }

        /* --- THE RETURN BUS -------------------------------------------------
         * Sits exactly where the single chamber sat: after the sampler bus,
         * BEFORE the arrangement clips, the master clamp, the phrase looper
         * and the sink. Clips are still summed after this, so no clip ever
         * feeds a return and the BB_REC_LIVE guarantee (bytebeat.h) survives
         * intact.
         *
         * Every return->return edge is one frame old, so the loop below may be
         * walked in any order; ascending index is chosen only so the peak and
         * limiter bookkeeping is deterministic. */
        if (rb.nlive) {
            for (int e = 0; e < rb.nd; e++)
                rin[rb.dr[e]] += (int32_t)(((int64_t)mix * rb.da[e]) >> 8);
            for (int e = 0; e < rb.nw; e++)
                rin[rb.wr[e]] += (int32_t)(((int64_t)g_ret_wprev * rb.wa[e]) >> 8);
            for (int e = 0; e < rb.nk; e++)
                rin[rb.kt[e]] += (int32_t)(((int64_t)g_ret_prev[rb.kf[e]]
                                            * rb.ka[e]) >> 8);

            int32_t wsum = 0;
            for (int j = 0; j < rb.nlive; j++) {
                int r = rb.live[j];
                int32_t y = ret_run(r, &rb.s[r], rin[r]);

                /* Safety, armed only where a feedback edge exists. A chamber
                 * fed by voices alone runs raw -- that is what keeps a legacy
                 * session bit-identical, and it is safe because a return with
                 * no incoming edge cannot be part of a cycle.
                 *
                 * The DC blocker matters more than the limiter here and is the
                 * less obvious of the two: a delayed loop with gain g is a
                 * one-pole at DC with gain 1/(1-g), so at g = 0.99 that is
                 * 100x at DC -- inaudible, rail-eating and speaker-damaging.
                 * A feedback patch's first symptom is an offset, not a howl.
                 *
                 * The limiter runs BEFORE the value that feeds back is stored,
                 * or the bound does not hold on the state. */
                /* THE LOOP GETS THE BOUNDED COPY; THE EARS GET THE FILTERED
                 * ONE. `loop` is what the link matrix carries back round a
                 * frame later; `y` is what reaches the wet sum.
                 *
                 * They part company because the two jobs want opposite things.
                 * What closes the loop must be BOUNDED, and nothing else --
                 * the limiter's hard ceiling is the entire stability proof for
                 * the graph, and it holds for any matrix the user can build
                 * because it is a clamp and not an argument. What reaches the
                 * ears must be DC-FREE. Trying to do both to one value is what
                 * put a highpass inside the user's feedback path, and a
                 * highpass in a loop whose gain has been ridden to unity is a
                 * negative feedback path with an 1,800-sample lag: it
                 * oscillates, at a few Hertz, at full amplitude, silently.
                 * Measured on a self-linked DELAY, the audible 50 Hz repeat
                 * disappeared completely and what was left was a full-scale
                 * ramp under a Hertz -- limiter pinned, meters lit, nothing to
                 * hear, and a speaker cone being walked off centre and held
                 * there. Every peak assertion in the suite stayed green
                 * through it, which is why the check that caught it measures
                 * the MEAN.
                 *
                 * So the filter comes out of the loop and stays on the output.
                 * The clamp after it is still unconditional: dsp_dc has unity
                 * passband gain but its transient response can overshoot, and
                 * it is not allowed to be the last word on amplitude. */
                int32_t loop = y;
                if (rb.s[r].hot) {
                    loop = ret_limit(&g_ret_lg[r], y, BB_RET_CEIL);
                    y    = dsp_dc(&g_ret_dc[r], loop);
                    if (y >  BB_RET_CEIL) y =  BB_RET_CEIL;
                    if (y < -BB_RET_CEIL) y = -BB_RET_CEIL;
                }

                /* Create/destroy fade. Same +/-32 ramp idiom and same Q16
                 * scale as g_lvl above: ~2048 frames, ~46 ms. It sits at
                 * exactly 65536 for a slot armed since bb_engine_init(), so
                 * the multiply is skipped and the arithmetic is the one it
                 * replaced. Both copies ride it, or a slot being faded out
                 * would go quiet while still driving the graph at full. */
                int32_t f = g_ret_fade[r], tg = rb.s[r].fade_tgt;
                if      (f < tg) { f += 32; if (f > tg) f = tg; }
                else if (f > tg) { f -= 32; if (f < tg) f = tg; }
                g_ret_fade[r] = f;
                if (f != 65536) {
                    y    = (int32_t)(((int64_t)y    * f) >> 16);
                    loop = (int32_t)(((int64_t)loop * f) >> 16);
                }

                g_ret_cur[r] = loop;
                int32_t w = (int32_t)(((int64_t)y * rb.s[r].level) >> 8);
                wsum += w;
                int32_t aw = w < 0 ? -w : w;
                if (aw > g_ret_pk[r]) g_ret_pk[r] = aw;
            }

            /* THE WET-BUS LIMITER. Eight returns each individually bounded at
             * -2.5 dBFS still sum to 8x unity, and the master's only
             * protection is dsp_clip16 below -- a pinned master clipper is the
             * same square-wave-into-headphones problem one stage later. Armed
             * ONLY when the graph contains feedback at all, so a legacy
             * session never executes it. */
            int32_t wloop = wsum;
            if (rb.bus_hot) {
                wloop = ret_limit(&g_ret_buslg, wsum, BB_RET_BUS_CEIL);

                /* And then the offset every clamp upstream of here left
                 * behind -- the per-return ceilings' as much as this one's. A
                 * clamp on an asymmetric waveform manufactures DC, a pinned
                 * loop is the case where the clamps fire on every sample, and
                 * the per-slot blockers all ran before those clamps did. This
                 * is the last stage that can clean up after them, so it is
                 * where the cleaning happens. See g_ret_busdc.
                 *
                 * Split from `wloop` for exactly the reason the per-return
                 * stage above is split: the WET row hands this sum back to the
                 * returns a frame later, so it is a feedback path, and a
                 * highpass in a feedback path is an oscillator rather than a
                 * filter. What goes back round is the BOUNDED sum; what goes
                 * to the master is the blocked one.
                 *
                 * The re-clamp keeps the ceiling a guarantee rather than an
                 * expectation. It fires far more rarely than the ones it is
                 * cleaning up after, because what reaches it is centred. */
                wsum = dsp_dc(&g_ret_busdc, wloop);
                if (wsum >  BB_RET_BUS_CEIL) wsum =  BB_RET_BUS_CEIL;
                if (wsum < -BB_RET_BUS_CEIL) wsum = -BB_RET_BUS_CEIL;
            }

            mix += wsum;
            g_ret_wprev = wloop;
            for (int j = 0; j < rb.nlive; j++)
                { int r = rb.live[j]; g_ret_prev[r] = g_ret_cur[r]; }
        } else {
            /* No live returns means no wet bus. Clearing it here stops a
             * stale sum from a graph that has since been closed sitting in
             * the WET row as a DC offset when one is next opened. */
            g_ret_wprev = 0;
        }

        /* --- R2 song playback --------------------------------------------
         * A clip sounds while bar_count sits inside its window; window
         * entry is edge-detected and resets the frames-into-window counter,
         * which then runs 1:1 at the device rate (BPM stretches nothing).
         * loop repeats the audio inside the window; otherwise the clip goes
         * silent after its last frame but the counter keeps walking so a
         * re-entry is what restarts it. Summed here, BEFORE the master clip,
         * the phrase looper and the sink, so REC and SURVIVOR capture the
         * song like everything else. NULL audio is a silent ghost. */
        if (sng) {
            for (int c = 0; c < sng->nclips; c++) {
                const ArrClip *cp = &sng->clip[c];
                if ((uint64_t)bar_count < (uint64_t)cp->start_bar ||
                    (uint64_t)bar_count >= (uint64_t)cp->start_bar
                                         + (uint64_t)cp->len_bars) {
                    g_arr_in[c] = 0;
                    continue;
                }
                if (!g_arr_in[c]) { g_arr_in[c] = 1; g_arr_ctr[c] = 0; }
                const ArrClipBuf *ab = cp->audio;
                if (ab) {
                    uint32_t idx = cp->loop ? g_arr_ctr[c] % ab->n
                                            : g_arr_ctr[c];
                    if (idx < ab->n)
                        clip_sum += (int32_t)(((int64_t)ab->data[idx]
                                               * cp->gain) >> 8);
                }
                g_arr_ctr[c]++;
            }
        }

        /* The clip sum is accumulated whether or not the timeline is playing,
         * and only its ADDITION is gated. That is deliberate: the per-clip
         * counters and their window-entry edges stay locked to the bar grid
         * while stopped, so pressing PLAY drops you in at the position the
         * song has reached rather than wherever you left off. It behaves like
         * unmuting a track, not like releasing a pause -- which is the right
         * model for looping a section and playing over it. bb_engine_song_seek()
         * is still the thing that restarts a song from the top.
         *
         * Zeroing it when stopped also means the two REC sources agree while
         * the timeline is silent, instead of differing by an inaudible amount. */
        if (!arr_on)
            clip_sum = 0;
        mix += clip_sum;

        /* --- per-lane capture --------------------------------------------
         * Waits armed until the top of a bar, then copies the tapped lane
         * contribution (collected into cap_smp above -- clips do NOT feed
         * it) one int16 per frame, for the requested whole bars or until
         * the destination runs out. Both status edges are CAS'd against
         * the expected state so a UI cancel always wins. */
        if (cap_on) {
            if (!g_cap_run && bar_pos == 0) {
                int expect = ARR_REC_ARMED;
                if (atomic_compare_exchange_strong(&bb.arr_rec_status,
                                                   &expect,
                                                   ARR_REC_RECORDING))
                    g_cap_run = 1;
                else
                    cap_on = 0;               /* canceled under us */
            }
            if (g_cap_run) {
                int done = 0;
                if (bar_pos == 0 && g_cap_pos > 0 && --g_cap_left <= 0)
                    done = 1;
                if (!done && g_cap_pos >= g_cap_cap)
                    done = 1;
                if (done) {
                    int expect = ARR_REC_RECORDING;
                    atomic_compare_exchange_strong(&bb.arr_rec_status,
                                                   &expect, ARR_REC_DONE);
                    atomic_store_explicit(&bb.arr_rec_frames, g_cap_pos,
                                          memory_order_relaxed);
                    g_cap_run = 0;
                    cap_on = 0;
                } else {
                    g_cap_dst[g_cap_pos++] = (int16_t)dsp_clip16(cap_smp);
                }
            }
        }

        /* The record path forks HERE, before the master clamp -- not later,
         * next to sink_push, where it would read more naturally.
         *
         * dsp_clip16 is not linear. Once the bus has been clamped, the clip
         * audio can no longer be taken back out of it exactly, and the error
         * shows up precisely when the mix is hottest, which on this
         * instrument is most of the time. Forking early costs one extra
         * clamp per frame and makes the guarantee hold at every level
         * instead of only the quiet ones. */
        int32_t dry = (rec_src == BB_REC_LIVE) ? mix - clip_sum : mix;

        if (mix > 32767 || mix < -32768) clipped = 1;
        mix = dsp_clip16(mix);

        int32_t pre_loop = mix;
        mix = loop_process(mix, bar_pos, bar_len, lp_bars, lp_mix, lp_fb,
                           lp_od, lp_rate, lp_rev, lp_slice);

        /* The phrase looper is stateful and can only be run once per frame,
         * so the record path takes its contribution as a delta rather than a
         * second pass. With SURVIVOR idle that delta is exactly zero, which
         * is the case the sample-identity check pins. */
        dry = (rec_src == BB_REC_LIVE) ? dsp_clip16(dry) + (mix - pre_loop)
                                       : mix;

        scope_push((int16_t)mix);

        if (g_gain_cur < target_gain) {
            g_gain_cur += 32; if (g_gain_cur > target_gain) g_gain_cur = target_gain;
        } else if (g_gain_cur > target_gain) {
            g_gain_cur -= 32; if (g_gain_cur < target_gain) g_gain_cur = target_gain;
        }
        int16_t o16 = (int16_t)dsp_clip16((int32_t)(((int64_t)mix * g_gain_cur) >> 16));

        /* What goes to the recorder is not always what goes to the speakers.
         * Under BB_REC_LIVE the arrangement is heard but not printed, so you
         * can loop an arranged section, play over the top of it, and capture
         * only the new layer instead of stacking the backing again on every
         * pass. `dry` was forked off above, before the master clamp. */
        int16_t rec16 = (rec_src == BB_REC_LIVE)
                      ? (int16_t)dsp_clip16(
                            (int32_t)(((int64_t)dry * g_gain_cur) >> 16))
                      : o16;

        sink_push(rec16);

        for (int c = 0; c < channels; c++)
            out[i * channels + c] = o16;

        t++;
        k++;         if (k        >= loop_len) k = 0;
        beat_pos++;  if (beat_pos >= beat_len) beat_pos = 0;
        bar_pos++;   if (bar_pos  >= bar_len)  { bar_pos = 0; bar_count++; }
    }

    /* Both readings come from the same monotonic counter, so the subtraction
     * cannot go negative and the unsigned arithmetic is exact. The clamp only
     * fires if this period took over half an hour, which would mean the render
     * thread was suspended mid-callback -- publish the ceiling rather than a
     * wrapped-around number that would read as "instant". */
    uint64_t c1 = bb_now_us();
    uint64_t us = c1 - c0;
    atomic_store_explicit(&bb.cpu_us,
                          (int)(us > (uint64_t)INT32_MAX
                                    ? INT32_MAX : (int32_t)us),
                          memory_order_relaxed);
    /* 1000000LL, not 1000000L: `long` is 32 bits on Windows, and at 4096
     * frames a period 1000000L * frames overflows it and publishes a negative
     * budget, which makes the CPU meter read as permanently over. */
    atomic_store_explicit(&bb.budget_us,
                          (int)((1000000LL * frames) / (rate > 0 ? rate : 1)),
                          memory_order_relaxed);
    atomic_store_explicit(&bb.clipping, clipped, memory_order_relaxed);

    atomic_store_explicit(&bb.t,   t,         memory_order_relaxed);
    atomic_store_explicit(&bb.k,   k,         memory_order_relaxed);
    atomic_store_explicit(&bb.bar, bar_count, memory_order_relaxed);
    atomic_store_explicit(&bb.seq_pos,
                          (any_seq || sampler_clock)
                              ? (int)((k / step_len) % BB_STEPS) : -1,
                          memory_order_relaxed);
    atomic_store_explicit(&bb.loop_pos, g_loop_pub_pos, memory_order_relaxed);
    atomic_store_explicit(&bb.loop_frames, g_loop_len, memory_order_relaxed);
    if (g_cap_run)
        atomic_store_explicit(&bb.arr_rec_frames, g_cap_pos,
                              memory_order_relaxed);

    /* Publish the per-voice and per-slot meter peaks: max-hold so a 30 Hz
     * UI never misses a transient between periods. The UI reads with
     * atomic_exchange(&peak, 0); a race with that clear only costs one
     * cosmetic meter frame. Plain load+store -- no CAS, no wait. */
    for (int L = 0; L < BB_NLAYER; L++) {
        if (layer_pk[L] > atomic_load_explicit(&bb.layer[L].peak,
                                               memory_order_relaxed))
            atomic_store_explicit(&bb.layer[L].peak, layer_pk[L],
                                  memory_order_relaxed);
    }
    for (int s = 0; s < BB_SAMPLER; s++) {
        if (g_smp_pk[s] > atomic_load_explicit(&bb.sampler[s].peak,
                                               memory_order_relaxed))
            atomic_store_explicit(&bb.sampler[s].peak, g_smp_pk[s],
                                  memory_order_relaxed);
        g_smp_pk[s] = 0;
    }
    /* Return-bus telemetry. Slot 0 additionally publishes to bb.verb_peak so
     * the existing RETURN A strip keeps its meter with no migration, and
     * bb.ret_active lets STATUS render "CPU 34% - RET 5/8" so a rising meter
     * has a legible cause. There is deliberately no per-bus microsecond
     * number: timing inside the frame loop would mean a timer read per frame,
     * and the bus is interleaved with the voice sum so it cannot be bracketed.
     * The whole bus already sits inside the cpu_us region measured above. */
    for (int r = 0; r < BB_NRET; r++) {
        int32_t pk = g_ret_pk[r]; if (pk > 32767) pk = 32767;
        if (pk > atomic_load_explicit(&bb.ret[r].peak, memory_order_relaxed))
            atomic_store_explicit(&bb.ret[r].peak, pk, memory_order_relaxed);
        g_ret_pk[r] = 0;
        atomic_store_explicit(&bb.ret[r].gr,
            (int)(((int64_t)g_ret_lg[r] * 256) >> 16), memory_order_relaxed);
        if (r == 0) {
            if (pk > atomic_load_explicit(&bb.verb_peak, memory_order_relaxed))
                atomic_store_explicit(&bb.verb_peak, pk, memory_order_relaxed);
        }
        /* The quiesce handshake's only signal: this slot faded to zero AND
         * was not run for a whole period, so the render thread is provably
         * out of its effect. */
        atomic_store_explicit(&bb.ret[r].quiet,
            (g_ret_fade[r] == 0 && !rb.ran[r]) ? 1 : 0, memory_order_relaxed);
    }
    atomic_store_explicit(&bb.ret_active, rb.nlive, memory_order_relaxed);
}

/* Read-only view of the master phrase-loop buffer for the SURVIVOR waveform
 * and LOOP OUT meter. UI-thread reads race the audio thread's writes; torn
 * reads are cosmetic exactly like bb.scope. */
const int16_t *bb_engine_loop_buffer(void)
{
    return g_loop_buf;
}

/* ======================================================================== */
/*  Specimen synthesizer                                                     */
/*                                                                           */
/*  Renders a self-looping drone to a WAV using the cold-wing sources and a  */
/*  PRIVATE compiler/VM context: nothing here touches the live render state, */
/*  so the UI thread can grow specimens while the instrument plays. The      */
/*  length is whole bars at the current transport tempo and the tail is      */
/*  crossfaded into the head, so a GRAIN MASS well with LOOP on plays it     */
/*  seamlessly and in musical time.                                          */
/* ======================================================================== */

static uint32_t spc_rand(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    return *s = x;
}

/* The core generator, deterministic in (seed, geometry, voice). Renders the
 * GIVEN expression with the GIVEN parameters -- slow parameter drift around
 * those values, a few cents of tape warble, optionally the voice's own post
 * chain -- then folds the surplus tail into the head so the loop point is
 * seamless. Fills dst with total frames. Returns the pre-normalisation peak
 * (0 = something failed, caller aborts). This is the DIRECTED path: the
 * specimen is the voice you designed, breathing. */
static int32_t specimen_core(int16_t *dst, uint32_t total, uint32_t xf,
                             uint32_t beat_len, int rate, unsigned seed,
                             const char *expr_text,
                             const int seedp[BB_NPARAM], int mode,
                             const PostParams *post)
{
    uint32_t rng = seed ? seed : 0x5eedu;

    Program *pr  = malloc(sizeof *pr);
    int32_t *dly = malloc(EXPR_DELAY_LEN * sizeof *dly);
    int32_t *acc = malloc((total + xf) * sizeof *acc);
    int32_t *spc = post ? malloc(BB_SPACE_LEN * sizeof *spc) : NULL;
    if (!pr || !dly || !acc || (post && !spc)) {
        free(pr); free(dly); free(acc); free(spc);
        return 0;
    }

    ExprError err;
    if (!expr_text || !*expr_text || !expr_compile(expr_text, pr, &err)) {
        free(pr); free(dly); free(acc); free(spc);
        return 0;
    }

    /* Wander targets per knob, approached exponentially around the voice's
     * OWN values: the drone moves without ever leaving the sound you made.
     * Q16 current values. */
    int32_t pq[BB_NPARAM], ptgt[BB_NPARAM];
    for (int i = 0; i < BB_NPARAM; i++) {
        pq[i]   = bb_clampi(seedp[i], 0, 255) << 16;
        ptgt[i] = pq[i];
    }

    memset(dly, 0, EXPR_DELAY_LEN * sizeof *dly);
    ExprCtx cx;
    memset(&cx, 0, sizeof cx);
    cx.dly = dly;
    cx.rng = 0x1234567u ^ seed;
    if (cx.rng == 0) cx.rng = 1;
    cx.sr  = rate;
    cx.bl  = (int32_t)beat_len;
    cx.ll  = (int32_t)total;
    cx.vel = 256;
    cx.age = INT32_MAX / 2;

    PostState ps;
    if (post) {
        memset(spc, 0, BB_SPACE_LEN * sizeof *spc);
        post_init(&ps, spc, BB_SPACE_LEN);
    }

    /* Tape warble: the voice clock drifts a few cents around unity. */
    uint64_t vt = 0;
    int64_t  inc  = (int64_t)1 << 32;
    int64_t  itgt = inc;
    const uint32_t seg = beat_len * 2u;   /* new wander targets per half-bar */

    for (uint32_t i = 0; i < total + xf; i++) {
        if (seg && i % seg == 0) {
            for (int q = 0; q < BB_NPARAM; q++)
                ptgt[q] = bb_clampi((pq[q] >> 16)
                                    + (int)(spc_rand(&rng) % 21u) - 10,
                                    1, 255) << 16;
            itgt = ((int64_t)1 << 32)
                 + (((int64_t)1 << 32) / 4096)
                   * ((int)(spc_rand(&rng) % 33u) - 16) / 4;
        }
        for (int q = 0; q < BB_NPARAM; q++) {
            pq[q] += (ptgt[q] - pq[q]) >> 13;
            cx.p[q] = pq[q] >> 16;
        }
        inc += (itgt - inc) >> 14;
        vt  += (uint64_t)inc;

        cx.t  = (int32_t)(uint32_t)(vt >> 32);
        cx.k  = (int32_t)(i % total);
        cx.bt = (int32_t)(i % beat_len);
        cx.n  = (int32_t)(i / (beat_len * 4u ? beat_len * 4u : 1u));

        int32_t v = expr_eval(pr, &cx);
        cx.dw = (cx.dw + 1u) & EXPR_DELAY_MASK;

        /* Same unsigned-shift reinterpretation as the render loop -- see the
         * comment there. A specimen must be bit-identical to the live voice
         * or it is not a specimen of anything. */
        int32_t s;
        switch (mode) {
        case BB_BYTE:   s = (int32_t)((uint32_t)((v & 0xff) - 128) << 8); break;
        case BB_SIGNED: s = (int32_t)((uint32_t)(int8_t)(v & 0xff) << 8); break;
        default:        s = (int32_t)(int16_t)(v & 0xffff);               break;
        }

        if (post) {
            s = post_process(&ps, s, post);
            s = dsp_dcblock(&ps, s);
        }
        acc[i] = s;
    }

    /* Fold the surplus tail into the head so the loop point is seamless. */
    for (uint32_t i = 0; i < xf && xf > 0; i++) {
        int64_t w = ((int64_t)i << 16) / xf;
        acc[i] = (int32_t)((acc[i] * w + acc[total + i] * ((1 << 16) - w)) >> 16);
    }

    int32_t peak = 0;
    for (uint32_t i = 0; i < total; i++) {
        int32_t a = acc[i] < 0 ? -acc[i] : acc[i];
        if (a > peak) peak = a;
    }
    if (peak > 0) {
        /* Normalise to about -2.5 dBFS: loud enough to sit in a well,
         * enough headroom not to fight the master. */
        int64_t g = ((int64_t)24576 << 16) / peak;
        for (uint32_t i = 0; i < total; i++)
            dst[i] = (int16_t)dsp_clip16((int32_t)(((int64_t)acc[i] * g) >> 16));
    }

    free(pr); free(dly); free(acc); free(spc);
    return peak;
}

/* The UNDIRECTED wrapper: picks a cold-wing source at random and seeds its
 * own params. Kept for the regression suite and as raw material. */
static int32_t specimen_synth(int16_t *dst, uint32_t total, uint32_t xf,
                              uint32_t beat_len, int rate, unsigned seed)
{
    static const char *const WING[4] = { "cold", "vapor", "siren", "hymn" };
    uint32_t rng = seed ? seed : 0x5eedu;

    int src = -1;
    const char *pick = WING[spc_rand(&rng) & 3u];
    for (int i = 0; i < rack_nsrc(); i++)
        if (!strcmp(rack_src_name(i), pick)) { src = i; break; }
    if (src < 0) return 0;

    Rack r;
    r.src   = (unsigned char)src;
    r.body  = 0;                      /* the wing sources carry their own lp */
    r.space = 1;                      /* the smear is the point              */
    r.mode  = (unsigned char)rack_src_mode(src);

    RackBuild b;
    rack_build(&r, &b);

    int p[BB_NPARAM] = { 0 };
    rack_seed_params(&b, p);
    for (int i = 0; i < BB_NPARAM; i++)
        p[i] = bb_clampi(p[i] + (int)(spc_rand(&rng) % 9u) - 4, 1, 255);

    return specimen_core(dst, total, xf, beat_len, rate, rng,
                         b.expr, p, r.mode, NULL);
}

/* Shared geometry: whole bars at the current transport tempo. */
static void spc_geometry(int bars, int *rate, uint32_t *beat_len,
                         uint32_t *total, uint32_t *xf)
{
    *rate = bb_clampi(atomic_load(&bb.rate), BB_RATE_MIN, BB_RATE_MAX);
    int bpm = atomic_load(&bb.gctl[GCTL_BPM]);
    int bts = atomic_load(&bb.gctl[GCTL_BEATS]);
    if (bpm < 20) bpm = 68;
    if (bts < 1)  bts = 4;

    *beat_len = (uint32_t)(((long)*rate * 60L) / bpm);
    if (*beat_len < 1) *beat_len = 1;
    uint32_t bar_len = *beat_len * (uint32_t)bts;
    bars = bb_clampi(bars, 1, 8);

    *total = bar_len * (uint32_t)bars;
    if (*total > (uint32_t)*rate * 60u) *total = (uint32_t)*rate * 60u;
    *xf = bar_len / 2u;
    if (*xf > *total / 4u) *xf = *total / 4u;
}

/* Join a directory and a leaf name with the platform's separator.
 *
 * Every path in this file used to be built with a hardcoded '/'. That still
 * WORKS on Windows -- the Win32 API has accepted forward slashes forever --
 * but the roots arrive from the GUI as "C:\Users\<name>\MORGUE", so the result
 * was a path that changed direction halfway through. It shows up in error
 * messages and in the session file, and anyone who pastes one into a shell
 * gets to find out which half of it their shell believes.
 *
 * A trailing separator on `dir` is trimmed so joining never doubles one. On
 * Unix only '/' counts as a separator -- a backslash is a perfectly legal
 * character in a filename there, and eating it would rename someone's folder
 * out from under them. */
static void path_join(char *dst, size_t dstsz,
                      const char *dir, const char *leaf)
{
    size_t n = strlen(dir);
#if defined(_WIN32)
    while (n > 0 && (dir[n - 1] == '/' || dir[n - 1] == '\\')) n--;
#else
    while (n > 0 && dir[n - 1] == '/') n--;
#endif
    snprintf(dst, dstsz, "%.*s%s%s", (int)n, dir, BB_PATH_SEP, leaf);
}

static int spc_write_wav(const char *path, const int16_t *buf,
                         uint32_t total, int rate)
{
    FILE *f = bb_fopen(path, "wb");
    if (!f) return -1;

    uint32_t dlen = total * 2u;
    unsigned char h[44] = { 'R','I','F','F', 0,0,0,0, 'W','A','V','E',
                            'f','m','t',' ', 16,0,0,0, 1,0, 1,0,
                            0,0,0,0, 0,0,0,0, 2,0, 16,0,
                            'd','a','t','a', 0,0,0,0 };
    uint32_t riff = 36u + dlen, brate = (uint32_t)rate * 2u;
    h[4]  = (unsigned char)riff;  h[5]  = (unsigned char)(riff >> 8);
    h[6]  = (unsigned char)(riff >> 16); h[7] = (unsigned char)(riff >> 24);
    h[24] = (unsigned char)rate;  h[25] = (unsigned char)(rate >> 8);
    h[26] = (unsigned char)(rate >> 16); h[27] = (unsigned char)(rate >> 24);
    h[28] = (unsigned char)brate; h[29] = (unsigned char)(brate >> 8);
    h[30] = (unsigned char)(brate >> 16); h[31] = (unsigned char)(brate >> 24);
    h[40] = (unsigned char)dlen;  h[41] = (unsigned char)(dlen >> 8);
    h[42] = (unsigned char)(dlen >> 16); h[43] = (unsigned char)(dlen >> 24);

    int ok = fwrite(h, 1, 44, f) == 44
          && fwrite(buf, 2, total, f) == total;
    ok = ok && !ferror(f);
    ok = (fclose(f) == 0) && ok;
    if (!ok) bb_remove(path);
    return ok ? 0 : -1;
}

int bb_engine_render_specimen(const char *dir, unsigned seed,
                              int bars, char *out, size_t outsz)
{
    if (!dir || !*dir) return -1;

    int rate; uint32_t beat_len, total, xf;
    spc_geometry(bars, &rate, &beat_len, &total, &xf);

    int16_t *buf = malloc(total * sizeof *buf);
    if (!buf) return -1;

    if (specimen_synth(buf, total, xf, beat_len, rate, seed) <= 0) {
        free(buf);
        return -1;
    }

    char name[64], path[720];
    snprintf(name, sizeof name, "SPC-%04X.wav", seed & 0xFFFFu);
    path_join(path, sizeof path, dir, name);

    int rc = spc_write_wav(path, buf, total, rate);
    free(buf);
    if (rc != 0) return -1;
    if (out && outsz) snprintf(out, outsz, "%s", name);
    return 0;
}

int bb_engine_render_specimen_voice(const char *dir, unsigned seed, int bars,
                                    int layer, char *out, size_t outsz)
{
    if (!dir || !*dir) return -1;
    layer = bb_clampi(layer, 0, BB_NLAYER - 1);
    if (bb_expr[layer][0] == '\0') return -1;

    int rate; uint32_t beat_len, total, xf;
    spc_geometry(bars, &rate, &beat_len, &total, &xf);

    Layer *l = &bb.layer[layer];
    int p[BB_NPARAM];
    for (int i = 0; i < BB_NPARAM; i++)
        p[i] = bb_clampi(atomic_load(&l->param[i]), 0, 255);
    int mode = bb_clampi(atomic_load(&l->mode), 0, BB_NMODE - 1);

    /* the voice's own dirt rides along -- a specimen of THIS sound */
    int bts = atomic_load(&bb.gctl[GCTL_BEATS]);
    if (bts < 1) bts = 4;
    PostParams pp;
    memset(&pp, 0, sizeof pp);
    pp.drive = bb_clampi(atomic_load(&l->ctl[LCTL_DRIVE]), 0, 255);
    pp.tone  = bb_clampi(atomic_load(&l->ctl[LCTL_TONE]),  1, 255);
    pp.crush = bb_clampi(atomic_load(&l->ctl[LCTL_CRUSH]), 0, 255);
    pp.spc_fb  = bb_clampi(atomic_load(&l->ctl[LCTL_SPC_FB]),  0, 255);
    pp.spc_mix = bb_clampi(atomic_load(&l->ctl[LCTL_SPC_MIX]), 0, 255);
    pp.spc_time = space_samples(
        bb_clampi(atomic_load(&l->ctl[LCTL_SPC_TIME]), 0, 255),
        atomic_load(&l->ctl[LCTL_SPC_SYNC]),
        beat_len, beat_len * (uint32_t)bts, rate);

    int16_t *buf = malloc(total * sizeof *buf);
    if (!buf) return -1;

    if (specimen_core(buf, total, xf, beat_len, rate, seed,
                      bb_expr[layer], p, mode, &pp) <= 0) {
        free(buf);
        return -1;
    }

    char name[64], path[720];
    snprintf(name, sizeof name, "SPC-V%02d-%04X.wav",
             layer + 1, seed & 0xFFFFu);
    path_join(path, sizeof path, dir, name);

    int rc = spc_write_wav(path, buf, total, rate);
    free(buf);
    if (rc != 0) return -1;
    if (out && outsz) snprintf(out, outsz, "%s", name);
    return 0;
}

/* ======================================================================== */
/*  Session configuration                                                    */
/* ======================================================================== */

static char cfg_dir[512];
static char cfg_path[600];

int bb_config_set_root(const char *dir)
{
    if (!dir) return -1;
    snprintf(cfg_dir, sizeof cfg_dir, "%s", dir);
    path_join(cfg_path, sizeof cfg_path, cfg_dir, "session.conf");
    return 0;
}

/* The fallback root, used only when nobody called bb_config_set_root() first.
 * The GUI always does (app/Main.cpp, before anything reads a path), so in
 * practice this is the headless path: the regression suite, and whatever
 * replaces the TUI. It still has to be RIGHT, because the day it is wrong is
 * the day someone's session lands in a directory nobody thinks to look in. */
static void cfg_paths(void)
{
#if defined(_WIN32)
    /* Windows has neither $HOME nor XDG. USERPROFILE first, because that is
     * where the GUI puts its root -- a headless run and a GUI run should find
     * the SAME session rather than quietly keeping two. LOCALAPPDATA and
     * APPDATA are the conventional places after that (LOCALAPPDATA first: a
     * session is machine-local state, and roaming it across a domain login
     * would drag the WAV paths it references somewhere they do not exist). */
    const char *profile = getenv("USERPROFILE");
    const char *local   = getenv("LOCALAPPDATA");
    const char *appdata = getenv("APPDATA");
    if      (profile && *profile) path_join(cfg_dir, sizeof cfg_dir, profile, "MORGUE");
    else if (local   && *local)   path_join(cfg_dir, sizeof cfg_dir, local,   "MORGUE");
    else if (appdata && *appdata) path_join(cfg_dir, sizeof cfg_dir, appdata, "MORGUE");
    else                          snprintf(cfg_dir, sizeof cfg_dir, "MORGUE");
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg) snprintf(cfg_dir, sizeof cfg_dir, "%s/bytebeat", xdg);
    else if (home)   snprintf(cfg_dir, sizeof cfg_dir, "%s/.config/bytebeat", home);
    else             snprintf(cfg_dir, sizeof cfg_dir, ".bytebeat");
#endif
    path_join(cfg_path, sizeof cfg_path, cfg_dir, "session.conf");
}

const char *bb_config_path(void)
{
    if (cfg_path[0] == '\0') cfg_paths();
    return cfg_path;
}

/* Push a stream all the way to the medium.
 *
 * fclose() is NOT this. fclose() flushes the C library's buffer into the
 * operating system's page cache and returns; the bytes may sit there for
 * seconds. If the machine loses power in that window the directory entry
 * created by the replace below can be on disk while the contents are not, and
 * what comes back is a session file the filesystem swears is complete and that
 * is in fact zero-length. That is the failure mode this whole write-then-
 * replace dance exists to prevent, so it is worth the extra call.
 *
 * Returns 0 if the data is on the device. The file is left open; the caller
 * still has to fclose() it. */
static int cfg_flush_to_disk(FILE *f)
{
    if (fflush(f) != 0) return -1;

#if defined(_WIN32)
    int fd = _fileno(f);
    if (fd < 0) return -1;
    return _commit(fd) == 0 ? 0 : -1;
#elif defined(__APPLE__)
    int fd = fileno(f);
    if (fd < 0) return -1;
    /* On Darwin, fsync() only promises the drive has been TOLD; the drive is
     * free to keep the bytes in its own write cache. F_FULLFSYNC is the call
     * that waits for the medium. Some filesystems (network mounts, a few
     * FUSE ones) return ENOTSUP for it, so fall back rather than fail the
     * save over something the user cannot fix. */
    if (fcntl(fd, F_FULLFSYNC, 0) != -1) return 0;
    return fsync(fd) == 0 ? 0 : -1;
#else
    int fd = fileno(f);
    if (fd < 0) return -1;
    return fsync(fd) == 0 ? 0 : -1;
#endif
}

int bb_config_save(void)
{
    if (cfg_path[0] == '\0') cfg_paths();
    /* Best effort, exactly as before: if the directory cannot be made, the
     * open below fails and reports it properly. */
    (void)bb_mkdirs(cfg_dir);

    /* Write to a temporary sibling and replace the live file only once
     * everything is on disk. fopen(cfg_path, "w") would truncate the only good
     * copy the instant it opens; if the disk then fills, the session is gone
     * even though we would report success.
     *
     * "On disk" is meant literally -- see cfg_flush_to_disk() above, and
     * bb_replace_atomic(), which asks the OS to commit the directory change
     * too. The one thing still left to the filesystem's own ordering is the
     * PARENT directory's entry; every journalling filesystem this program will
     * meet orders that behind the rename, and prying it open would mean
     * opening the directory as a file purely to fsync it. */
    char tmp_path[608];
    snprintf(tmp_path, sizeof tmp_path, "%s.tmp", cfg_path);
    FILE *f = bb_fopen(tmp_path, "w");
    if (!f) return -1;

    fprintf(f, "# bytebeat session -- plain text, edit it if you like\n");
    fprintf(f, "version 7\n");
    fprintf(f, "rate %d\n", atomic_load(&bb.req_rate));
    fprintf(f, "gain %d\n", atomic_load(&bb.gain));
    fprintf(f, "focus %d\n", atomic_load(&bb.focus));
    /* Deliberately NOT a format-version bump. The loader skips keys it does
     * not recognise (see the unknown-line fallthrough below), so an older
     * build opens a session carrying these and simply ignores them, and a
     * newer build opens an older session and keeps the defaults. Both
     * directions are safe, which a version bump would not have been: an old
     * binary that failed to parse a new version would still autosave on quit
     * and write the whole song back out without them. */
    fprintf(f, "arrplay %d\n", atomic_load(&bb.arr_play));
    fprintf(f, "recsrc %d\n",  atomic_load(&bb.rec_src));
    fprintf(f, "looper %d %d %d %d %d %d %d\n",
            atomic_load(&bb.loop_bars), atomic_load(&bb.loop_mix),
            atomic_load(&bb.loop_feedback), atomic_load(&bb.loop_overdub),
            atomic_load(&bb.loop_rate), atomic_load(&bb.loop_reverse),
            atomic_load(&bb.loop_slice));

    fprintf(f, "gctl");
    for (int i = 0; i < GCTL_COUNT; i++) fprintf(f, " %d", atomic_load(&bb.gctl[i]));
    fprintf(f, "\n");

    /* ---- the return bus, and still NOT a format-version bump ------------
     * The reasoning above applies unchanged and more strongly here: a session
     * now carries a routing GRAPH, and an old binary that rejected a bumped
     * version would still autosave on quit and write the graph away. The
     * unknown-key fallthrough in the loader makes both directions safe.
     *
     *   ret   <i> <type> <level> <mute> <sync> <p0>..<p7>
     *   rsend <src> <slot> <amount>
     *   rlink <from> <to> <amount>
     *   rname <slot> <len>:<name>
     *
     * `ret` is eight lines, always, fixed arity, and TYPE IS WRITTEN VERBATIM
     * -- including an id from a newer build that this one renders as silence.
     * Clamping it would silently rewrite someone's session on a downgrade.
     *
     * `rsend`/`rlink` carry nonzero entries only; a full matrix would be 152
     * lines of zeros. Sends from a voice or the sampler INTO SLOT 0 are never
     * emitted here -- those cells are the aliased storage the existing `send`
     * lines and the `verb` line's 4th field already own -- but they are
     * ACCEPTED on read, so a hand edit does the obvious thing.
     *
     * `verb` is unchanged and still written, but MOVED to after this block.
     * It and `ret 0` write the same storage so they always agree; emitting it
     * last means a hand-edit to the familiar key wins, which is what someone
     * editing the file expects, and an old binary that reads only `verb` is
     * still correct. */
    for (int r = 0; r < BB_NRET; r++) {
        fprintf(f, "ret %d %d %d %d %d", r,
                atomic_load(&bb.ret[r].type), ret_level_load(r),
                atomic_load(&bb.ret[r].mute), atomic_load(&bb.ret[r].sync));
        for (int p = 0; p < BB_RET_NPARAM; p++)
            fprintf(f, " %d", ret_param_load(r, p));
        fprintf(f, "\n");
    }
    for (int s = 0; s < BB_RET_NSRC; s++)
        for (int r = 0; r < BB_NRET; r++) {
            if (r == 0 && s < BB_RET_SRC_DRY) continue;   /* aliased cells */
            int amt = atomic_load(&bb.ret_send[s][r]);
            if (amt) fprintf(f, "rsend %d %d %d\n", s, r, amt);
        }
    for (int a = 0; a < BB_NRET; a++)
        for (int b = 0; b < BB_NRET; b++) {
            int amt = atomic_load(&bb.ret_link[a][b]);
            if (amt) fprintf(f, "rlink %d %d %d\n", a, b, amt);
        }
    /* Length-prefixed exactly like an aclip name, so names may contain
     * spaces: <len> decimal bytes follow the ':' verbatim to end of line. */
    for (int r = 0; r < BB_NRET; r++)
        if (bb_ret_name[r][0])
            fprintf(f, "rname %d %d:%s\n", r,
                    (int)strlen(bb_ret_name[r]), bb_ret_name[r]);

    fprintf(f, "verb %d %d %d %d\n",
            atomic_load(&bb.verb_size), atomic_load(&bb.verb_tone),
            atomic_load(&bb.verb_level), atomic_load(&bb.smp_send));

    for (int L = 0; L < BB_NLAYER; L++) {
        Layer *ly = &bb.layer[L];
        fprintf(f, "layer %d on %d mode %d seq %d\n", L,
                atomic_load(&ly->on), atomic_load(&ly->mode),
                atomic_load(&ly->seq_on));
        fprintf(f, "send %d %d\n", L, atomic_load(&ly->send));

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

        fprintf(f, "expr %d %s\n", L, bb_expr[L]);
        fprintf(f, "rack %d %d %d %d %d\n", L, bb_rack[L].src,
                bb_rack[L].body, bb_rack[L].space, bb_custom[L]);
    }

    for (int s = 0; s < BB_SAMPLER; s++) {
        SamplerSlot *sl = &bb.sampler[s];
        fprintf(f, "sampler %d on %d lvl %d chk %d mute %d solo %d\n", s,
                atomic_load(&sl->on), atomic_load(&sl->ctl[SMP_CTL_LEVEL]),
                atomic_load(&sl->ctl[SMP_CTL_CHOKE]),
                atomic_load(&sl->mute), atomic_load(&sl->solo));
        fprintf(f, "sgate %d", s);
        for (int i = 0; i < BB_STEPS; i++) fprintf(f, " %d", atomic_load(&sl->gate[i]));
        fprintf(f, "\n");
        fprintf(f, "spitch %d", s);
        for (int i = 0; i < BB_STEPS; i++) fprintf(f, " %d", atomic_load(&sl->pitch[i]));
        fprintf(f, "\n");
        fprintf(f, "svel %d", s);
        for (int i = 0; i < BB_STEPS; i++) fprintf(f, " %d", atomic_load(&sl->vel[i]));
        fprintf(f, "\n");
    }

    /* R2 song meta (version 7), one line per clip:
     *
     *   aclip <lane> <start> <len> <loop> <gain> <namelen>:<name> <path>
     *
     * <lane> 0..ARR_LANES-1, <start>/<len> in absolute bars (len >= 1),
     * <loop> 0/1, <gain> 0..256. The name is length-prefixed -- <namelen>
     * decimal bytes follow the ':' verbatim, so names may contain spaces --
     * then ONE separating space, then the source-WAV path, which runs to
     * end of line (paths may contain spaces; neither field may contain a
     * newline). Audio is deliberately NOT persisted: the GUI re-decodes
     * each path at startup and republishes the song. */
    {
        ArrClip clips[ARR_MAX_CLIPS];
        int nclip = bb_engine_song_get(clips, ARR_MAX_CLIPS);
        for (int c = 0; c < nclip; c++)
            fprintf(f, "aclip %d %u %u %d %d %d:%s %s\n",
                    clips[c].lane, clips[c].start_bar, clips[c].len_bars,
                    clips[c].loop ? 1 : 0, clips[c].gain,
                    (int)strlen(clips[c].name), clips[c].name, clips[c].path);
    }

    /* ferror catches fprintf failures above; cfg_flush_to_disk forces the
     * buffered writes out and is where ENOSPC/EDQUOT actually surfaces;
     * fclose is the last chance to hear about either. Only a tmp file that
     * is verifiably on the medium may replace the previous good session.
     *
     * bb_replace_atomic rather than rename(): Windows' rename() REFUSES when
     * the destination exists, so every save after the first one silently did
     * nothing and left a .tmp behind. */
    int bad = ferror(f);
    if (!bad && cfg_flush_to_disk(f) != 0) bad = 1;
    if (fclose(f) != 0) bad = 1;
    if (bad || bb_replace_atomic(tmp_path, cfg_path) != 0) {
        bb_remove(tmp_path);
        return -1;
    }
    return 0;
}

/* Song meta collected while parsing (audio always NULL -- the GUI
 * re-decodes each path and republishes with real ArrClipBufs). Static so
 * bb_config_load never carries 55KB on its stack. UI thread only. */
static ArrClip cfg_clips[ARR_MAX_CLIPS];
static int     cfg_nclips;

/* Reset the return bus to what a session that says nothing about it means:
 * slot 0 is the CHAMBER, slots 1-7 are empty, the matrix is zero.
 *
 * This has to happen BEFORE the parse loop, because `rsend`/`rlink` emit
 * nonzero entries only -- an omitted zero cannot clear a stale nonzero, and
 * without this a second load would inherit the first session's links.
 *
 * It deliberately does NOT touch the ALIASED cells: bb.layer[s].send,
 * bb.smp_send, bb.verb_level, bb.verb_size and bb.verb_tone are owned by the
 * `send` and `verb` lines and by bb_engine_set_defaults(), and v4 and v6
 * sessions round-trip through exactly those keys. Clearing them here would
 * quietly delete the sends out of every session written before the bus
 * existed. */
static void ret_config_reset(void)
{
    for (int s = 0; s < BB_RET_NSRC; s++)
        for (int r = 0; r < BB_NRET; r++) {
            if (r == 0 && s < BB_RET_SRC_DRY) continue;   /* aliased */
            atomic_store(&bb.ret_send[s][r], 0);
        }
    for (int a = 0; a < BB_NRET; a++)
        for (int b = 0; b < BB_NRET; b++)
            atomic_store(&bb.ret_link[a][b], 0);

    for (int r = 0; r < BB_NRET; r++) {
        int t = (r == 0) ? RET_CHAMBER : RET_NONE;
        atomic_store(&bb.ret[r].type, t);
        atomic_store(&bb.ret[r].mute, 0);
        atomic_store(&bb.ret[r].sync, 0);
        /* `arm` is NOT touched. This runs INSIDE bb_config_load's quiesce
         * bracket, and re-arming here would let the render thread back into
         * an effect whose type the parse is about to change underneath it --
         * defeating the one thing the bracket exists to guarantee.
         * bb_engine_ret_release_all() re-arms, once, at the end. */
        for (int p = 0; p < BB_RET_NPARAM; p++) {
            if (r == 0 && p < 2) continue;             /* aliased */
            ret_param_store(r, p, ret_param_def[t][p]);
        }
        if (r != 0) ret_level_store(r, 0);             /* slot 0's is aliased */
        bb_ret_name[r][0] = '\0';
    }
}

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

int bb_config_load(void)
{
    if (cfg_path[0] == '\0') cfg_paths();
    FILE *f = bb_fopen(cfg_path, "r");
    if (!f) return 0;

    /* Big enough for the longest legal line: an aclip carrying a full
     * ARR_NAME_MAX name and ARR_PATH_MAX path on top of its numbers. */
    /* Quiesce every return slot for the whole parse. A type change is a
     * direct atomic store below, which is safe ONLY because of this: the
     * render thread is provably out of every effect, so nothing can be inside
     * a call that saw the old type when its arena is cleared. Released
     * immediately before the single remaining return, which is why this
     * function must keep exactly two exits -- the `return 0` above (before
     * this call) and the one at the bottom. */
    bb_engine_ret_quiesce_all();
    ret_config_reset();
    int ret_base_type[BB_NRET];
    for (int r = 0; r < BB_NRET; r++)
        ret_base_type[r] = atomic_load(&bb.ret[r].type);

    char line[BB_EXPR_MAX + ARR_NAME_MAX + ARR_PATH_MAX + 96];
    int  version = 1;

    cfg_nclips = 0;

    while (fgets(line, sizeof line, f)) {
        /* Strip the WHOLE line terminator, CR as well as LF, and do it here
         * rather than per-field.
         *
         * A session written on Windows has CRLF line endings (the writer opens
         * the file in text mode, and git's autocrlf will do it to a checkout
         * regardless), and sessions travel: this is a file people copy between
         * a laptop and a studio machine. The numeric fields survive a stray
         * CR because strtol and sscanf stop at it, which is exactly why this
         * was easy to miss -- but three fields are strings that run to the end
         * of the line: an `expr` (the expression itself), and an `aclip`'s name
         * and source-WAV path. Each of those would silently acquire a trailing
         * carriage return, and a clip path with an invisible control character
         * on the end simply does not open. So: one strip, at the boundary,
         * covering every field that will ever be added below. */
        size_t ln = strlen(line);
        while (ln > 0 && (line[ln - 1] == '\n' || line[ln - 1] == '\r'))
            line[--ln] = '\0';
        if (line[0] == '#' || line[0] == '\0') continue;

        int v, L;

        if (sscanf(line, "version %d", &v) == 1)      { version = v; continue; }
        if (version < 2) continue;

        if (sscanf(line, "rate %d", &v) == 1) {
            atomic_store(&bb.req_rate, bb_clampi(v, BB_RATE_MIN, BB_RATE_MAX));
        } else if (sscanf(line, "gain %d", &v) == 1) {
            atomic_store(&bb.gain, bb_clampi(v, 0, 256));
        } else if (sscanf(line, "focus %d", &v) == 1) {
            atomic_store(&bb.focus, bb_clampi(v, 0, BB_NLAYER - 1));
        } else if (sscanf(line, "arrplay %d", &v) == 1) {
            atomic_store(&bb.arr_play, v ? 1 : 0);
        } else if (sscanf(line, "recsrc %d", &v) == 1) {
            atomic_store(&bb.rec_src, bb_clampi(v, BB_REC_MASTER, BB_REC_LIVE));
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
        } else if (!strncmp(line, "verb ", 5)) {
            int sz, tn, lv, ss;
            if (sscanf(line, "verb %d %d %d %d", &sz, &tn, &lv, &ss) == 4) {
                atomic_store(&bb.verb_size,  bb_clampi(sz, 0, 255));
                atomic_store(&bb.verb_tone,  bb_clampi(tn, 0, 255));
                atomic_store(&bb.verb_level, bb_clampi(lv, 0, 256));
                atomic_store(&bb.smp_send,   bb_clampi(ss, 0, 255));
            }
        } else if (!strncmp(line, "ret ", 4)) {
            /* ret <i> <type> <level> <mute> <sync> <p0>..<p7> -- fixed arity,
             * thirteen numbers. A short line is skipped whole rather than
             * half-applied. */
            const char *p = line + 4;
            char *end;
            int i = (int)strtol(p, &end, 10);
            if (end != p && i >= 0 && i < BB_NRET) {
                int fld[12], n = 0;
                p = end;
                for (; n < 12; n++) {
                    long fv = strtol(p, &end, 10);
                    if (end == p) break;
                    fld[n] = (int)fv;
                    p = end;
                }
                if (n == 12) {
                    /* TYPE IS NOT CLAMPED. An id this build does not know is a
                     * slot written by a newer one: it renders as silence and
                     * is written back out unchanged, so a downgrade does not
                     * silently rewrite someone's session. Only nonsense is
                     * rejected. Safe because ret_run() dispatches on a switch
                     * with `default: return 0;` and never a pointer table. */
                    atomic_store(&bb.ret[i].type,
                                 (fld[0] < 0 || fld[0] > 4096) ? RET_NONE : fld[0]);
                    ret_level_store(i, bb_clampi(fld[1], 0, 256));
                    atomic_store(&bb.ret[i].mute, fld[2] ? 1 : 0);
                    atomic_store(&bb.ret[i].sync, bb_clampi(fld[3], 0, 10));
                    for (int q = 0; q < BB_RET_NPARAM; q++)
                        ret_param_store(i, q, bb_clampi(fld[4 + q], 0, 255));
                }
            }
        } else if (!strncmp(line, "rsend ", 6)) {
            int src, slot, amt;
            if (sscanf(line, "rsend %d %d %d", &src, &slot, &amt) == 3 &&
                src >= 0 && src < BB_RET_NSRC && slot >= 0 && slot < BB_NRET)
                ret_send_store(src, slot, bb_clampi(amt, 0, 255));
        } else if (!strncmp(line, "rlink ", 6)) {
            int from, to, amt;
            if (sscanf(line, "rlink %d %d %d", &from, &to, &amt) == 3 &&
                from >= 0 && from < BB_NRET && to >= 0 && to < BB_NRET)
                atomic_store(&bb.ret_link[from][to], bb_clampi(amt, 0, 256));
        } else if (!strncmp(line, "rname ", 6)) {
            /* rname <slot> <len>:<name>
             *
             * Length-prefixed like an aclip name so that names may contain
             * spaces, and the CR has already been stripped at the top of the
             * loop, so a session copied off a Windows machine does not grow an
             * invisible control character on the end of every name.
             *
             * The name runs to END OF LINE and the prefix is a bound, not a
             * cut. Nothing follows the name on this line -- unlike an aclip,
             * where a path comes after it and the count is the only way to
             * find the boundary -- so honouring a hand-written count that
             * disagrees with the text would truncate a name for no reader's
             * benefit. The count is still written, so the field stays
             * self-describing if anything is ever appended after it. */
            int slot, nlen, pos = -1;
            if (sscanf(line, "rname %d %d:%n", &slot, &nlen, &pos) == 2 &&
                pos > 0 && slot >= 0 && slot < BB_NRET && nlen >= 0) {
                const char *nm = line + pos;
                size_t nl2 = strlen(nm);
                if (nl2 >= BB_RET_NAME) nl2 = BB_RET_NAME - 1;
                memcpy(bb_ret_name[slot], nm, nl2);
                bb_ret_name[slot][nl2] = '\0';
            }
        } else if (!strncmp(line, "send ", 5)) {
            int sv;
            if (sscanf(line, "send %d %d", &L, &sv) == 2
                && L >= 0 && L < BB_NLAYER)
                atomic_store(&bb.layer[L].send, bb_clampi(sv, 0, 255));
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
        } else if (!strncmp(line, "sampler ", 8)) {
            int on, lvl, chk, mute, solo;
            if (sscanf(line, "sampler %d on %d lvl %d chk %d mute %d solo %d",
                       &L, &on, &lvl, &chk, &mute, &solo) == 6 &&
                L >= 0 && L < BB_SAMPLER) {
                SamplerSlot *sl = &bb.sampler[L];
                atomic_store(&sl->on, !!on);
                atomic_store(&sl->ctl[SMP_CTL_LEVEL], bb_clampi(lvl, 0, 256));
                atomic_store(&sl->ctl[SMP_CTL_CHOKE], bb_clampi(chk, 0, 4));
                atomic_store(&sl->mute, !!mute);
                atomic_store(&sl->solo, !!solo);
            }
        } else if (!strncmp(line, "sgate ", 6)) {
            const char *p = line + 6;
            L = (int)strtol(p, (char **)&p, 10);
            if (L >= 0 && L < BB_SAMPLER)
                read_ints(p, bb.sampler[L].gate, BB_STEPS, 0, 2);
        } else if (!strncmp(line, "spitch ", 7)) {
            const char *p = line + 7;
            L = (int)strtol(p, (char **)&p, 10);
            if (L >= 0 && L < BB_SAMPLER)
                read_ints(p, bb.sampler[L].pitch, BB_STEPS, -12, 12);
        } else if (!strncmp(line, "svel ", 5)) {
            const char *p = line + 5;
            L = (int)strtol(p, (char **)&p, 10);
            if (L >= 0 && L < BB_SAMPLER)
                read_ints(p, bb.sampler[L].vel, BB_STEPS, 0, 255);
        } else if (!strncmp(line, "aclip ", 6)) {
            /* R2 song meta (v7) -- format documented at the writer. The
             * name is length-prefixed (it may contain spaces); the path is
             * last and runs to end of line. A malformed line is skipped
             * whole, the same way unknown lines always have been. */
            int lane, loop, gain, nlen, pos = -1;
            unsigned start, lenb;
            if (sscanf(line, "aclip %d %u %u %d %d %d:%n",
                       &lane, &start, &lenb, &loop, &gain, &nlen, &pos) == 6
                && pos > 0 && nlen >= 0 && nlen < ARR_NAME_MAX
                && cfg_nclips < ARR_MAX_CLIPS) {
                const char *nm = line + pos;
                if ((int)strlen(nm) >= nlen + 1 && nm[nlen] == ' ') {
                    ArrClip *c = &cfg_clips[cfg_nclips];
                    memset(c, 0, sizeof *c);
                    c->lane      = bb_clampi(lane, 0, ARR_LANES - 1);
                    c->start_bar = start;
                    c->len_bars  = lenb ? lenb : 1;
                    c->loop      = !!loop;
                    c->gain      = bb_clampi(gain, 0, 256);
                    memcpy(c->name, nm, (size_t)nlen);
                    c->name[nlen] = '\0';
                    snprintf(c->path, ARR_PATH_MAX, "%s", nm + nlen + 1);
                    c->audio = NULL;
                    cfg_nclips++;
                }
            }
        }
    }
    fclose(f);

    /* The session is the whole truth about the song: publish exactly what
     * it carried, which for v6-and-earlier sessions (no aclip lines) is an
     * empty song. Audio is NULL throughout; the GUI rehydrates from each
     * clip's path and republishes. */
    bb_engine_song_publish(cfg_clips, cfg_nclips);

    /* Any slot whose type actually changed gets its arena and its bus state
     * cleared, so a loaded session never starts with the previous session's
     * tail ringing inside a different effect. A slot whose type did NOT change
     * is left alone -- which is why loading a session over a running CHAMBER
     * keeps its reverb tail, exactly as it always has. */
    for (int r = 0; r < BB_NRET; r++) {
        int t = atomic_load(&bb.ret[r].type);
        if (t != ret_base_type[r]) {
            ret_reset(r, t);
            ret_bus_clear(r);
        }
    }
    bb_engine_ret_release_all();

    return version >= 2;
}

/* ======================================================================== */
/*  Defaults and the first-run groove                                        */
/* ======================================================================== */

void bb_engine_set_defaults(void)
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

    atomic_store(&bb.arr_rec_status, ARR_REC_IDLE);
    atomic_store(&bb.arr_rec_frames, 0);
    atomic_store(&bb.arr_seek_bar,  -1);
    /* The timeline plays by default -- that is what it did before it had a
     * transport of its own, and a session opening silent would read as a bug.
     * REC defaults to the whole bus for the same reason: the new behaviour is
     * opt-in, so nothing anyone already relies on changes underneath them. */
    atomic_store(&bb.arr_play,       1);
    atomic_store(&bb.rec_src,        BB_REC_MASTER);

    atomic_store(&bb.gctl[GCTL_BPM],   90);
    atomic_store(&bb.gctl[GCTL_BEATS],  4);
    atomic_store(&bb.gctl[GCTL_BARS],   2);
    atomic_store(&bb.gctl[GCTL_ZOOM],  32);

    /* CHAMBER: a long dark room, closed by default -- verb_level 0 is a
     * bit-exact bypass, so old sessions sound untouched until the return
     * fader is raised. */
    atomic_store(&bb.verb_size,  172);
    atomic_store(&bb.verb_tone,   96);
    atomic_store(&bb.verb_level,   0);
    atomic_store(&bb.smp_send,     0);
    atomic_store(&bb.verb_peak,    0);

    /* The rest of the return bus: slot 0 stays the CHAMBER, slots 1-7 are
     * empty and the matrix is zero, which is exactly the graph that renders
     * bit-identically to the engine that had no return bus. Runs AFTER the
     * verb_* stores above and skips the aliased cells, so the two agree
     * instead of racing to write the same three atomics. */
    ret_config_reset();
    for (int r = 0; r < BB_NRET; r++) {
        atomic_store(&bb.ret[r].peak,  0);
        atomic_store(&bb.ret[r].gr,    256);
        atomic_store(&bb.ret[r].quiet, 0);
        /* Unlike the loader, this IS the place to re-arm: a default session
         * has no pending create to respect, and any request still in flight
         * refers to a graph that no longer exists. */
        atomic_store(&bb.ret[r].arm,   1);
        g_ret_req[r].st = RQ_IDLE;
    }
    atomic_store(&bb.ret_active, 0);

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
        atomic_store(&ly->send, 0);
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

    for (int s = 0; s < BB_SAMPLER; s++) {
        SamplerSlot *sl = &bb.sampler[s];
        atomic_store(&sl->on, 0);
        atomic_store(&sl->ctl[SMP_CTL_LEVEL], 220);
        atomic_store(&sl->ctl[SMP_CTL_CHOKE], 0);
        atomic_store(&sl->mute, 0);
        atomic_store(&sl->solo, 0);
        for (int i = 0; i < BB_STEPS; i++) {
            atomic_store(&sl->gate[i], SMP_GATE_OFF);
            atomic_store(&sl->pitch[i], 0);
            atomic_store(&sl->vel[i], 200);
        }
    }
}

static void rack_apply(int L, int src, int body, int space)
{
    bb_rack[L].src   = (unsigned char)src;
    bb_rack[L].body  = (unsigned char)body;
    bb_rack[L].space = (unsigned char)space;
    bb_rack[L].mode  = (unsigned char)rack_src_mode(src);
    bb_custom[L]     = 0;

    Layer *l = &bb.layer[L];
    atomic_store(&l->mode, bb_rack[L].mode);

    RackBuild b;
    rack_build(&bb_rack[L], &b);
    int params[BB_NPARAM] = { 0 };
    rack_seed_params(&b, params);
    for (int p = 0; p < BB_NPARAM; p++)
        atomic_store(&l->param[p], params[p]);

    ExprError er;
    snprintf(bb_expr[L], BB_EXPR_MAX, "%s", b.expr);
    if (!bb_publish(L, bb_expr[L], &er))
        bb_publish(L, "0", &er);
}

/* Synthesize a tiny stock kit for the step sampler: kick (pitch-fall sine),
 * snare (noise burst) and hat (short crackle). UI thread only, so malloc
 * and sinf are fine here -- this never runs on the audio thread. */
static int16_t *drum_kit_sample(int kind, uint32_t *len_out)
{
    const uint32_t rate = 44100u;
    uint32_t len = (kind == 2) ? rate / 12u
                 : (kind == 1) ? rate / 6u
                 : rate / 4u;
    if (len < 32) len = 32;
    int16_t *b = malloc(len * sizeof(int16_t));
    if (!b) return NULL;
    uint32_t rng = 0x9E3779B9u * (uint32_t)(kind + 1);
    float prev = 0.f;
    for (uint32_t i = 0; i < len; i++) {
        float t = (float)i / (float)rate;
        float v = 0.f;
        if (kind == 0) {
            float f = 160.f - 130.f * (t / 0.25f);
            if (f < 34.f) f = 34.f;
            v = sinf(2.f * 3.14159265f * f * t);
            v *= (float)exp(-22.0 * (double)t);
        } else if (kind == 1) {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            v = (float)(int32_t)(rng & 0xffffu) / 16384.f - 1.f;
            v = v - 0.45f * prev;              /* crude high-pass */
            prev = v;
            v *= (float)exp(-26.0 * (double)t);
        } else {
            rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
            v = (float)(int32_t)(rng & 0xffffu) / 16384.f - 1.f;
            v = v - 0.6f * prev;
            prev = v;
            v *= (float)exp(-110.0 * (double)t);
        }
        int32_t sv = (int32_t)(v * 31000.f);
        if (sv > 32767) sv = 32767;
        if (sv < -32768) sv = -32768;
        b[i] = (int16_t)sv;
    }
    *len_out = len;
    return b;
}

/* Factory kit audio into any still-empty slot 0-2, touching no pattern or
 * level state. Sample memory is not persisted in session.conf, so a
 * restored session calls this to put real audio back under its saved
 * sampler patterns. */
void bb_engine_demo_kit_samples(void)
{
    for (int s = 0; s < 3; s++) {
        if (bb_engine_sampler_loaded(s)) continue;
        uint32_t len = 0;
        int16_t *b = drum_kit_sample(s, &len);
        if (!b) continue;
        bb_engine_sampler_set(s, b, (int)len, 44100);
    }
}

/* Preload the first three sampler slots with the stock kit and a beat so
 * the STEP workspace makes sound before a single file is loaded, exactly
 * like the rack groove does. */
static void sampler_demo_kit(void)
{
    bb_engine_demo_kit_samples();
    for (int s = 0; s < 3; s++) {
        SamplerSlot *sl = &bb.sampler[s];
        atomic_store(&sl->on, 1);
        atomic_store(&sl->ctl[SMP_CTL_LEVEL], s == 2 ? 190 : 230);
    }

    static const int kick_steps[] = { 0, 4, 8, 12 };
    static const int snare_steps[] = { 4, 12 };
    for (int i = 0; i < 4; i++) {
        atomic_store(&bb.sampler[0].gate[kick_steps[i]],
                     i == 0 ? SMP_GATE_ACCENT : SMP_GATE_ON);
        atomic_store(&bb.sampler[0].pitch[kick_steps[i]], -(2 * i));
    }
    for (int i = 0; i < 2; i++)
        atomic_store(&bb.sampler[1].gate[snare_steps[i]],
                     i == 0 ? SMP_GATE_ACCENT : SMP_GATE_ON);
    for (int st = 0; st < BB_STEPS; st += 2)
        atomic_store(&bb.sampler[2].gate[st], SMP_GATE_ON);
}

void bb_engine_first_run(void)
{
    static const struct {
        const char *src;
        int body, space, seq, steps, decay, level, drive, tone, crush;
        int spc_t, spc_fb, spc_mix, euclid;
    } GROOVE[5] = {
        { "thump", 0, 0, 1, 16, 198, 210, 55, 105,  0, 120, 150,  40, 4 },
        { "burst", 0, 0, 1, 16, 228, 155, 70, 175,  8, 100, 130,  35, 0 },
        { "metal", 1, 0, 1, 16, 242, 105, 45, 150, 20,  80, 120,  25, 8 },
        { "dust",  1, 1, 1, 15, 172,  80, 35,  90, 28, 160, 190,  75, 5 },
        { "fold",  1, 1, 0, 16,   0,  85, 40,  65,  0, 190, 175,  80, 0 },
    };

    for (int L = 0; L < 5; L++) {
        atomic_store(&bb.focus, L);
        Layer *l = &bb.layer[L];

        int src = 0;
        for (int i = 0; i < rack_nsrc(); i++)
            if (!strcmp(rack_src_name(i), GROOVE[L].src)) { src = i; break; }

        rack_apply(L, src, GROOVE[L].body, GROOVE[L].space);

        atomic_store(&l->ctl[LCTL_LEVEL],    GROOVE[L].level);
        atomic_store(&l->ctl[LCTL_DRIVE],    GROOVE[L].drive);
        atomic_store(&l->ctl[LCTL_TONE],     GROOVE[L].tone);
        atomic_store(&l->ctl[LCTL_CRUSH],    GROOVE[L].crush);
        atomic_store(&l->ctl[LCTL_SPC_TIME], GROOVE[L].spc_t);
        atomic_store(&l->ctl[LCTL_SPC_FB],   GROOVE[L].spc_fb);
        atomic_store(&l->ctl[LCTL_SPC_MIX],  GROOVE[L].spc_mix);
        atomic_store(&l->ctl[LCTL_STEPS],    GROOVE[L].steps);
        atomic_store(&l->ctl[LCTL_DECAY],    GROOVE[L].decay);
        atomic_store(&l->ctl[LCTL_SPC_SYNC], GROOVE[L].space ? 7 : 0);
        atomic_store(&l->ctl[LCTL_SPC_FREEZE], 0);
        atomic_store(&l->seq_on,             GROOVE[L].seq);
        atomic_store(&l->on, 1);
        if (GROOVE[L].euclid) {
            int gate[BB_STEPS];
            gen_euclid(BB_STEPS, GROOVE[L].euclid, gate);
            for (int st = 0; st < BB_STEPS; st++)
                atomic_store(&l->seq_gate[st], gate[st]);
        }
    }

    for (int st = 0; st < BB_STEPS; st++) atomic_store(&bb.layer[1].seq_gate[st], GATE_OFF);
    atomic_store(&bb.layer[1].seq_gate[4], GATE_ACCENT);
    atomic_store(&bb.layer[1].seq_gate[12], GATE_ON);
    atomic_store(&bb.layer[2].seq_ratchet[14], 3);
    atomic_store(&bb.layer[3].seq_prob[7], 55);
    atomic_store(&bb.layer[3].seq_prob[10], 70);

    atomic_store(&bb.gctl[GCTL_BPM], 68);
    atomic_store(&bb.focus, 0);

    /* Open the chamber a crack: the metallic hits and the drone get a send
     * so the first-run groove has a back wall, without washing the kit. */
    atomic_store(&bb.layer[2].send, 96);
    atomic_store(&bb.layer[3].send, 70);
    atomic_store(&bb.layer[4].send, 120);
    atomic_store(&bb.verb_level, 132);

    sampler_demo_kit();
}

/* ======================================================================== */
/*  Engine self-test: audio clock / phrase loop invariants                   */
/* ======================================================================== */

int bb_engine_self_test(char *err, size_t errsz)
{
#define AUDIO_TEST(ok, why) do { \
        if (!(ok)) { snprintf(err, errsz, "%s", (why)); return 0; } \
    } while (0)

    for (int i = 0; i <= 10; i++) {
        int n = space_samples(200, i, 22050, 88200, 44100);
        AUDIO_TEST(n >= 1 && n <= (int)BB_SPACE_MASK,
                   "clocked SPACE division escaped its buffer");
    }
    AUDIO_TEST(space_samples(0, 7, 12345, 49380, 44100) == 12345,
               "quarter-note SPACE division is not one beat");
    AUDIO_TEST(space_samples(0, 9, 12345, 49380, 44100) == 49380,
               "bar SPACE division is not one bar");

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

    /* --- the CHAMBER ------------------------------------------------------ */
    {
        static const int clen[VERB_NCOMB] =
            { 1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617 };
        static const int alen[VERB_NAP] = { 556, 441, 341, 225 };

        verb_reset();
        int32_t early = 0, late = 0, tail = 0;
        for (int i = 0; i < 20000; i++) {
            int32_t w = verb_process(i == 0 ? 20000 : 0, 28880, 14848,
                                     clen, alen);
            if (w < 0) w = -w;
            if (i < 4000)                     { if (w > early) early = w; }
            else if (i < 8000)                { if (w > late)  late  = w; }
            else if (i >= 16000)              { if (w > tail)  tail  = w; }
        }
        AUDIO_TEST(early > 0,
                   "the chamber returned nothing for an impulse");
        AUDIO_TEST(tail < early,
                   "the chamber tail did not decay");
        AUDIO_TEST(late > 0 && tail >= 0,
                   "the chamber tail died unnaturally fast for its size");

        verb_reset();
        int32_t silent = 0;
        for (int i = 0; i < 2000; i++) {
            int32_t w = verb_process(0, 28880, 14848, clen, alen);
            if (w != 0) silent = 1;
        }
        AUDIO_TEST(silent == 0,
                   "the chamber made sound out of silence");
        verb_reset();
    }

    /* --- the specimen synthesizer ---------------------------------------- */
    {
        enum { SPC_N = 8000, SPC_XF = 500 };
        int16_t *a = malloc(SPC_N * sizeof *a);
        int16_t *b2 = malloc(SPC_N * sizeof *b2);
        AUDIO_TEST(a && b2, "specimen self-test allocation failed");

        int32_t pk1 = specimen_synth(a,  SPC_N, SPC_XF, 2000, 8000, 0xC0FFEEu);
        int32_t pk2 = specimen_synth(b2, SPC_N, SPC_XF, 2000, 8000, 0xC0FFEEu);
        int32_t pk3 = specimen_synth(b2, SPC_N, SPC_XF, 2000, 8000, 0xC0FFEEu);
        AUDIO_TEST(pk1 > 0, "specimen synth produced silence");
        AUDIO_TEST(pk1 == pk2 && pk2 == pk3,
                   "specimen synth is not deterministic in its seed");

        int32_t loud = 0;
        for (int i = 0; i < SPC_N; i++) {
            int32_t v = a[i] < 0 ? -a[i] : a[i];
            if (v > loud) loud = v;
        }
        AUDIO_TEST(loud > 8000 && loud <= 32767,
                   "specimen normalisation missed its window");

        pk2 = specimen_synth(b2, SPC_N, SPC_XF, 2000, 8000, 0xDEAD01u);
        AUDIO_TEST(pk2 > 0, "specimen synth failed on a second seed");

        /* the DIRECTED core: a given voice in, that voice out */
        static const int vp[BB_NPARAM] = { 4, 6, 56, 0, 0, 0, 0, 0 };
        const char *vx = "lp(((t*p0&511)+(t*p1&511))*24,p2)";
        int32_t d1 = specimen_core(a,  SPC_N, SPC_XF, 2000, 8000,
                                   0xFACE01u, vx, vp, BB_WORD, NULL);
        int32_t d2 = specimen_core(b2, SPC_N, SPC_XF, 2000, 8000,
                                   0xFACE01u, vx, vp, BB_WORD, NULL);
        AUDIO_TEST(d1 > 0 && d1 == d2,
                   "directed specimen core is not deterministic");

        PostParams vpp;
        memset(&vpp, 0, sizeof vpp);
        vpp.tone = 200; vpp.spc_time = 500;
        vpp.spc_fb = 120; vpp.spc_mix = 128;
        AUDIO_TEST(specimen_core(b2, SPC_N, SPC_XF, 2000, 8000, 0xFACE01u,
                                 vx, vp, BB_WORD, &vpp) > 0,
                   "directed specimen through the post chain fell silent");

        AUDIO_TEST(specimen_core(b2, SPC_N, SPC_XF, 2000, 8000, 0xFACE01u,
                                 "((", vp, BB_WORD, NULL) == 0,
                   "directed specimen accepted a broken expression");

        free(a); free(b2);
    }

    err[0] = '\0';
    return 1;
#undef AUDIO_TEST
}
