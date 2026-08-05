/* bytebeat.h -- state shared between the UI thread and the audio thread.
 *
 * There are exactly two threads in this program:
 *
 *   UI THREAD    (also the main thread) -- ncurses, parsing, file I/O,
 *                network I/O, malloc/free. Allowed to be slow. Allowed to
 *                block. Runs at whatever priority the OS feels like.
 *
 *   AUDIO THREAD -- computes samples and hands them to ALSA. Has a hard
 *                deadline: if it does not deliver a period of samples before
 *                the sound card runs out, you get an audible gap (an xrun).
 *                It must therefore never malloc, never take a lock, and never
 *                call anything that can block except snd_pcm_writei().
 *
 * Every field below that both threads touch is an atomic. That is not
 * decoration: without it the compiler is entitled to cache a value in a
 * register forever, and the audio thread would never see your knob move.
 *
 * The rings (scope, sink) are single-producer / single-consumer, written by
 * audio and read by UI, with atomic cursors. No locks anywhere.
 */
#ifndef BYTEBEAT_H
#define BYTEBEAT_H

#include <stdint.h>
#include <stdatomic.h>
#include <signal.h>
#include "expr.h"

#define BB_NPARAM       8
#define BB_NLAYER       8
#define BB_EXPR_MAX     512

/* Sequencer grid. One step is a 16th note, so 16 steps is one bar in 4/4. */
#define BB_STEPS        16
enum { GATE_OFF = 0, GATE_ON, GATE_ACCENT };

/* Per-step parameter locks. A stored value of -1 means "use the live knob".
 * Locks cover all eight expression parameters plus the sound-shaping layer
 * controls; transport controls deliberately remain global. */
enum {
    LOCK_P0 = 0, LOCK_P1, LOCK_P2, LOCK_P3,
    LOCK_P4, LOCK_P5, LOCK_P6, LOCK_P7,
    LOCK_LEVEL, LOCK_DRIVE, LOCK_TONE, LOCK_CRUSH,
    LOCK_SPC_TIME, LOCK_SPC_FB, LOCK_SPC_MIX, LOCK_DECAY,
    BB_LOCK_COUNT
};

/* Scope ring. Power of two so the index wrap is one AND. */
#define BB_SCOPE_BITS   15
#define BB_SCOPE_LEN    (1u << BB_SCOPE_BITS)
#define BB_SCOPE_MASK   (BB_SCOPE_LEN - 1u)

/* Sink ring: audio thread pushes finished int16 samples, UI thread drains
 * them to the .wav file and/or the network socket. 2^20 samples is ~11s at
 * 96kHz -- absurdly more than the ~16ms the UI takes between polls, which is
 * exactly the point. The ring exists so that a slow disk or a stalled TCP
 * client can NEVER stall the audio thread. */
#define BB_SINK_BITS    20
#define BB_SINK_LEN     (1u << BB_SINK_BITS)
#define BB_SINK_MASK    (BB_SINK_LEN - 1u)

/* Per-layer post-chain feedback delay ("SPACE"). 2^18 = 5.9s at 44.1kHz.
 * Eight of these at 1MB each is 8MB, which is the price of every layer
 * having a genuinely independent effects chain. */
#define BB_SPACE_BITS   18
#define BB_SPACE_LEN    (1u << BB_SPACE_BITS)
#define BB_SPACE_MASK   (BB_SPACE_LEN - 1u)

/* Master phrase buffer: 10.9s at 96kHz and 23.8s at 44.1kHz. Capture is
 * specified in bars and clamped to this fixed allocation. */
#define BB_LOOP_BITS    20
#define BB_LOOP_LEN     (1u << BB_LOOP_BITS)
#define BB_LOOP_MASK    (BB_LOOP_LEN - 1u)

enum { LOOP_OFF = 0, LOOP_ARMED, LOOP_RECORDING, LOOP_PLAYING };
enum { LOOP_CMD_NONE = 0, LOOP_CMD_ARM, LOOP_CMD_PLAY, LOOP_CMD_CLEAR };
enum { LOOP_RATE_HALF = 0, LOOP_RATE_NORMAL, LOOP_RATE_DOUBLE };

/* ---- R2 arrangement timeline (the song) ---------------------------------
 * The song is scheduled in ABSOLUTE BARS against the same monotonic bar
 * counter the transport already publishes (bb.bar). Ten lanes: 0-7 mirror
 * the voices V01-V08, 8 is the LICKS sampler bus, 9 is FILE/MASS. Lanes are
 * organisational labels only -- any clip may sit on any lane. All clip
 * audio is mono int16, because the whole engine bus is mono.
 *
 * The edit model (ArrClip) and the clip-buffer lifetime API live in
 * engine.h; only the cross-thread atomics belong here, with everything
 * else both threads touch. */
#define ARR_MAX_CLIPS 96
#define ARR_LANES     10
#define ARR_NAME_MAX  48
#define ARR_PATH_MAX  512

/* Per-lane capture status, published by the audio thread in
 * bb.arr_rec_status exactly the way the phrase looper publishes LOOP_*. */
enum { ARR_REC_IDLE = 0, ARR_REC_ARMED, ARR_REC_RECORDING, ARR_REC_DONE };

/* ---- R1 step sampler (drum-machine / FL Channel-Rack equivalent) --------
 * 8 one-shot sample slots, each with a 16-step pattern on the engine's own
 * step clock. A slot fires on every step whose gate is set: the play
 * position resets to frame 0 (one-shot retrigger), the step's PITCH picks
 * the playback rate and its VELOCITY picks the amplitude. CHOKE groups
 * stop the other members of the group the moment any one of them fires.
 * The slots sum into the engine's master bus, so REC and SURVIVOR capture
 * them like any other voice. Pattern state lives here (with every other
 * cross-thread control); the sample audio itself is published through
 * bb_engine_sampler_set() and owns nothing in this struct. */
#define BB_SAMPLER 8
enum {
    SMP_GATE_OFF = 0, SMP_GATE_ON, SMP_GATE_ACCENT
};
enum {
    SMP_CTL_LEVEL = 0,   /* 0..256 mix level into the master bus */
    SMP_CTL_CHOKE,       /* 0 = none, 1..4 = drum-choke group   */
    SMP_CTL_COUNT
};

typedef struct {
    atomic_int  on;                    /* slot audible?                  */
    atomic_int  gate[BB_STEPS];        /* SMP_GATE_*                    */
    atomic_int  pitch[BB_STEPS];       /* semitone offset, -12..+12     */
    atomic_int  vel[BB_STEPS];         /* per-step velocity, 0..255     */
    atomic_int  ctl[SMP_CTL_COUNT];    /* LEVEL 0..256, CHOKE 0..4      */
    atomic_int  mute;                  /* 1 = silenced by hand          */
    atomic_int  solo;                  /* 1 = only soloed slots sound   */

    /* Post-level abs peak of the slot's contribution to the master bus,
     * 0..32767. Audio thread max-holds it once per period; the UI reads
     * with atomic_exchange(&peak, 0) and applies its own display decay. */
    atomic_int  peak;
} SamplerSlot;

/* Largest period we will ever ask ALSA for. The audio thread's scratch
 * buffer is this big and lives on its stack -- no malloc. */
#define BB_MAX_PERIOD   2048

#define BB_RATE_MIN     1000
#define BB_RATE_MAX     96000

enum { BB_BYTE = 0, BB_SIGNED, BB_WORD, BB_NMODE };

/* ---- controls ----------------------------------------------------------
 * Split in two because layers are fully independent voices: everything that
 * shapes a SOUND belongs to the layer, and only the things that must agree
 * across layers (the transport) are global. Getting this wrong is what makes
 * a multitimbral instrument frustrating -- if tempo were per-layer they
 * would drift apart, and if tone were global you could not put a dark drone
 * under a bright gated hit. */
enum {
    LCTL_LEVEL = 0,    /* mix level into the master bus, 0..256   */
    LCTL_DRIVE, LCTL_TONE, LCTL_CRUSH,
    LCTL_SPC_TIME, LCTL_SPC_FB, LCTL_SPC_MIX,
    LCTL_STEPS,        /* sequencer pattern length, 1..16          */
    LCTL_DECAY,        /* gate envelope; 0 = hold until next step  */
    LCTL_SPC_SYNC,     /* 0 = free milliseconds, 1..10 = division  */
    LCTL_SPC_FREEZE,   /* recirculate SPACE without new input      */
    LCTL_COUNT
};

enum {
    GCTL_BPM = 0, GCTL_BEATS, GCTL_BARS,
    GCTL_ZOOM,         /* scope time-scale; no audio effect        */
    GCTL_COUNT
};

/* The UI presents both as one TAB-able list: layer controls first, because
 * those are the ones you reach for while playing. */
#define BB_CTL_TOTAL (LCTL_COUNT + GCTL_COUNT)

typedef struct {
    const char *name;
    int         lo, hi, step, coarse;
} CtlInfo;

extern const CtlInfo bb_lctl_info[LCTL_COUNT];
extern const CtlInfo bb_gctl_info[GCTL_COUNT];

/* ---- the shape of a voice ----------------------------------------------
 * Four bytes that decide what the expression LOOKS like. Everything
 * continuous lives in param[] instead, so turning a control is an atomic
 * store rather than a recompile. See rack.h for what these select.
 *
 * This lives here rather than in rack.h because it is session state that
 * main.c saves and loads, and because a layer has one whether or not the
 * rack is currently driving it. */
typedef struct {
    unsigned char src;    /* index into the source table */
    unsigned char body;   /* 1 = wrapped in lp()         */
    unsigned char space;  /* 1 = wrapped in a feedback delay */
    unsigned char mode;   /* BB_BYTE / BB_SIGNED / BB_WORD   */
} Rack;

/* ---- one voice --------------------------------------------------------- */
typedef struct {
    /* The UI thread compiles into a fresh Program and publishes it here.
     * The audio thread picks it up once per period. See bb_publish(). */
    _Atomic(Program *) prog;

    atomic_int  on;                     /* audible?                        */
    atomic_int  mode;                   /* BB_BYTE / BB_SIGNED / BB_WORD   */
    atomic_int  param[BB_NPARAM];       /* p0..p7, 0..255                  */
    atomic_int  ctl[LCTL_COUNT];

    atomic_int  seq_on;
    atomic_int  seq_gate[BB_STEPS];     /* GATE_OFF / ON / ACCENT          */
    atomic_int  seq_pitch[BB_STEPS];    /* semitone offset, -12..+12       */
    atomic_int  seq_ratchet[BB_STEPS];  /* retriggers in one step, 1..4    */
    atomic_int  seq_prob[BB_STEPS];     /* probability that step fires     */
    atomic_int  seq_lock[BB_LOCK_COUNT][BB_STEPS]; /* -1 = live knob       */
    atomic_uint motion_mask;            /* interpolated automation lanes   */

    /* MIDI / hardware hooks. The UI thread writes a one-shot impulse and the
     * audio thread consumes it on the next sample -- no lock, same pattern as
     * every other control. */
    atomic_int  mtrig;                  /* 1 = fire a hit, consumed once   */
    atomic_int  mvel;                   /* velocity 1..256 for that hit    */
    atomic_int  mtrans;                 /* semitone transpose, -24..+24    */

    /* Post-fader abs peak of this voice's contribution to the master mix,
     * 0..32767. Audio thread max-holds it once per period; the UI reads
     * with atomic_exchange(&peak, 0) and applies its own display decay. */
    atomic_int  peak;

    /* Send into the CHAMBER return bus (reverb), 0..255. Post-fader, so a
     * silent voice sends nothing and the mixer fader rides the send too. */
    atomic_int  send;
} Layer;

struct bb_state {
    Layer        layer[BB_NLAYER];
    atomic_int   focus;         /* layer the UI is editing               */

    /* 8 one-shot sample slots sequenced on the step clock (see above).   */
    SamplerSlot  sampler[BB_SAMPLER];

    /* Incremented by the audio thread at the top of every period. The UI
     * thread uses it to know when a retired Program can no longer be in
     * use. See bb_reclaim() in main.c for why this is sufficient. */
    atomic_ullong epoch;

    /* --- transport ------------------------------------------------------ */
    atomic_int   rate;          /* rate ALSA actually gave us            */
    atomic_int   req_rate;      /* rate the user asked for               */
    atomic_uint  t;             /* published sample counter (display)    */
    atomic_int   reset_t;
    atomic_int   reset_loop;
    atomic_uint  k;             /* published loop position               */
    atomic_uint  bar;
    atomic_int   seq_pos;       /* published playhead step, -1 if off    */

    /* --- master --------------------------------------------------------- */
    atomic_int   gctl[GCTL_COUNT];
    atomic_int   gain;          /* 0..256                                */
    atomic_int   mute;
    atomic_int   panic;
    atomic_int   bypass;        /* all post chains off                   */

    /* --- RETURN A: the CHAMBER (master reverb bus) ----------------------
     * Per-voice sends live in Layer.send; the step-sampler bus has one
     * shared send below. The wet return is summed into the master mix
     * BEFORE the clip, the phrase looper and the sink, so REC and
     * SURVIVOR capture the tail like everything else. verb_level 0 is
     * bit-exact bypass. */
    atomic_int   verb_size;     /* decay length, 0..255                  */
    atomic_int   verb_tone;     /* damping: 0 = dark cavern, 255 = bright */
    atomic_int   verb_level;    /* return level into the master, 0..256  */
    atomic_int   smp_send;      /* LICKS sampler-bus send, 0..255        */
    atomic_int   verb_peak;     /* return abs peak for the RETURN A meter */

    /* --- master phrase looper ----------------------------------------- */
    atomic_int   loop_cmd;       /* UI writes, audio thread consumes     */
    atomic_int   loop_status;    /* LOOP_* published by audio thread     */
    atomic_int   loop_bars;      /* capture length, 1..4 bars            */
    atomic_int   loop_mix;       /* dry/loop crossfade, 0..256           */
    atomic_int   loop_feedback;  /* retained audio while overdubbing     */
    atomic_int   loop_overdub;
    atomic_int   loop_rate;      /* half / normal / double               */
    atomic_int   loop_reverse;
    atomic_int   loop_slice;     /* 1,2,4,8,16: repeated fraction        */
    atomic_uint  loop_pos;
    atomic_uint  loop_frames;

    /* --- R2 arrangement timeline ---------------------------------------
     * Command/status traffic for the song. The song itself (clip list +
     * audio) is published through bb_engine_song_publish() as one atomic
     * pointer swap, exactly like a Program -- it never lives in this
     * struct. */
    atomic_int   arr_rec_status; /* ARR_REC_*, published by audio thread  */
    atomic_uint  arr_rec_frames; /* capture progress in frames, audio     *
                                  * thread writes, UI reads               */
    atomic_int   arr_seek_bar;   /* pending seek target in absolute bars: *
                                  * UI writes, audio thread consumes at   *
                                  * the top of a period; -1 = none        */

    /* --- telemetry ------------------------------------------------------ */
    atomic_int   xruns;
    atomic_int   cpu_us;
    atomic_int   budget_us;
    atomic_int   running;
    atomic_int   clipping;      /* master bus hit the rails last period  */

    /* --- scope ring (audio writes, UI reads; a torn read here is a
     *     cosmetic glitch in an ASCII waveform, so no atomics on data) */
    int16_t      scope[BB_SCOPE_LEN];
    atomic_uint  scope_w;

    /* --- sink ring: two independent read cursors so a stalled network
     *     client cannot corrupt or stall the .wav being written */
    int16_t      sink[BB_SINK_LEN];
    atomic_uint  sink_w;
    atomic_uint  file_r;
    atomic_uint  net_r;
    atomic_int   sink_lost;
};

extern struct bb_state bb;

static inline int bb_clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ---- session state, owned by main.c, edited by ui.c --------------------
 *
 * `bb_expr` is always the truth -- it is what got compiled and what is
 * playing. `bb_rack` is the structured description that PRODUCED it, and
 * `bb_custom` says whether that description is still accurate.
 *
 * The moment you hand-edit the expression the rack cannot describe it any
 * more, so the layer is marked custom and the panel switches to showing raw
 * knobs with inferred roles. Choosing a source again clears the flag and
 * overwrites the text. One direction only: parsing arbitrary expressions back
 * into racks would be a research project, and guessing wrong would silently
 * destroy someone's patch. */
extern char bb_expr[BB_NLAYER][BB_EXPR_MAX];
extern Rack bb_rack[BB_NLAYER];
extern int  bb_custom[BB_NLAYER];

/* Set by the SIGINT/SIGTERM handler so a kill still reaches the .wav
 * header patch-up on the way out. */
extern volatile sig_atomic_t bb_quit_signal;

#ifdef __cplusplus
extern "C" {
#endif

/* Compile `src` and, if it compiles, hand it to the given layer. Returns 1
 * on success; on failure that layer keeps playing whatever it had, which is
 * why a syntax error never interrupts the sound. */
int  bb_publish(int layer, const char *src, ExprError *err);

/* Free retired programs the audio thread can no longer be looking at.
 * UI thread only; call once per frame. */
void bb_reclaim(void);

int         bb_config_save(void);
int         bb_config_load(void);
const char *bb_config_path(void);

#ifdef __cplusplus
}
#endif

#endif /* BYTEBEAT_H */
