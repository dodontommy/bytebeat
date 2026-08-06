/* bytebeat.h -- state shared between the UI thread and the audio thread.
 *
 * There are exactly two threads that matter in this program:
 *
 *   UI THREAD    (JUCE's message thread) -- drawing, parsing, file I/O,
 *                malloc/free. Allowed to be slow. Allowed to block. Runs at
 *                whatever priority the OS feels like.
 *
 *   AUDIO THREAD -- computes samples and hands them to the device callback.
 *                Has a hard deadline: if it does not deliver a block of
 *                samples before the sound card runs out, you get an audible
 *                gap (an xrun). It must therefore never malloc, never take a
 *                lock, and never call anything that can block.
 *
 * Every field below that both threads touch is an atomic. That is not
 * decoration: without it the compiler is entitled to cache a value in a
 * register forever, and the audio thread would never see your knob move.
 *
 * The rings (scope, sink) are single-producer / single-consumer, written by
 * audio and read by UI, with atomic cursors. No locks anywhere.
 *
 * The atomics are spelled BB_ATOMIC(T) rather than atomic_int / _Atomic(T)
 * because this header is read by BOTH halves of the program: the engine, which
 * is C11, and the JUCE GUI, which is C++17. `_Atomic` is a C keyword that C++
 * does not have -- Clang accepts it in C++ as an extension, which is the only
 * reason this file ever compiled, and MSVC and g++ do not. bb_atomic.h maps
 * BB_ATOMIC(T) to _Atomic(T) in C and std::atomic<T> in C++, and static-asserts
 * that the two agree on size, alignment and lock-freedom, because both threads
 * are reading the same bytes of `bb` through different declarations of it.
 *
 * The whole body of this header is inside ONE extern "C" block, and the
 * #includes are deliberately outside it. Both halves matter. The extern "C"
 * has to cover the DATA declarations (`bb`, `bb_expr`, `bb_rack`,
 * `bb_lctl_info`, ...) and not just the functions: on a mangling ABI like
 * MSVC's, a variable declared without it gets a C++-mangled name in the GUI
 * and a plain C name in the engine, and the two simply never meet at link
 * time. The #includes have to be outside it because a C++ standard header
 * dragged into a C-linkage region is ill-formed -- templates may not have C
 * language linkage.
 */
#ifndef BYTEBEAT_H
#define BYTEBEAT_H

#include <stdint.h>
#include <signal.h>
#include "bb_atomic.h"
#include "expr.h"
/* The return bus's DSP core owns BB_NRET, BB_RET_NPARAM, BB_RET_NAME and the
 * RET_* effect ids, because it is the half that cannot be compiled without
 * them (it sizes its pools from BB_NRET and switches on the ids). This header
 * takes them from there rather than the reverse; do NOT paste a second copy.
 * ret.h also carries DcState/dsp_dc and ret_limit, which the bus's safety
 * stage in engine.c uses. */
#include "ret.h"

#ifdef __cplusplus
extern "C" {
#endif

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

/* ---- THE LOOP BANK --------------------------------------------------------
 * Six loopers. SLOT 0 IS THE MASTER PHRASE LOOPER (SURVIVOR): its seven knobs
 * and its status/frames/pos have NO storage in bb.loopn[0] -- they ARE
 * bb.loop_bars / loop_mix / loop_feedback / loop_overdub / loop_rate /
 * loop_reverse / loop_slice / loop_status / loop_frames / loop_pos. The alias
 * is unconditional; go through loop_ctl_load/store (C) or bb_engine_loop_ctl
 * (C++). NO shadow copies -- see the comment on `Return`.
 *
 * bb.loopn[0].cmd is the ONE field that is real rather than aliased, because
 * slot 0's legacy PLAY is a TOGGLE and the bank's is not. sat_command()
 * translates it into bb.loop_cmd on the audio thread, where the true
 * g_loop_state is visible; bb_engine_loop_command() still writes bb.loop_cmd
 * directly and is untouched.
 *
 * Slot 0's DSP is loop_process(), unchanged, at the same call site, with the
 * same 10-argument signature. THAT IS THE BIT-EXACTNESS ARGUMENT.
 *
 * Satellites 1..5 are ADDITIVE and share one 10 MiB BSS array. NOTHING may
 * ever memset it: not bb_engine_init() (the suite calls it many times per run;
 * the count is beside g_sat_buf in engine.c, where it does the arguing), not
 * CLEAR, not the loader. Every buffer read is `idx % g_sat_len[n]` and every
 * index below len was written during that capture, so residue is unreachable
 * by construction -- unlike the return pools, which had to learn it. */
#define BB_NLOOP          6
#define BB_LOOP_DEF_BARS  4        /* FOLLOW with no cycle yet established   */
#define BB_LOOP_SUM_CEIL  24576    /* -2.5 dBFS on the summed satellite bus  */

/* What a looper records. 0..9 mirror BB_RET_SRC_* exactly.
 *   LIVE -- the bus at the INPUT of the loop stage MINUS the arrangement:
 *           voices + sampler + returns and their tails, and NO looper, EVER.
 *           Exact, not a subtraction: no looper has run when it is taken.
 *           This is the default and it IS the feature.
 *   MASTER -- read-only sentinel reported by slot 0. Not selectable.
 * There is deliberately NO source that contains another looper's playback.
 * That is the trap: with one, looper 2 records looper 1, looper 1 is in the
 * mix twice, and that is the bug this feature exists to close. Sound-on-sound
 * within a layer is `overdub`; a bounce of everything is slot 0. */
enum {
    BB_LOOP_SRC_V0     = 0,
    BB_LOOP_SRC_LICKS  = BB_NLAYER,  /*  8 sampler bus (mix - premix)        */
    BB_LOOP_SRC_DRY,                 /*  9 voices+sampler, pre-return        */
    BB_LOOP_SRC_LIVE,                /* 10 DEFAULT for satellites            */
    BB_LOOP_SRC_MASTER,              /* 11 slot 0 only; read-only sentinel   */
    BB_LOOP_NSRC                     /* 12                                   */
};

/* Action in the low byte; LBC_HARD skips the quantum. HARD is honoured on
 * PLAY/STOP/CLEAR and IGNORED on ARM -- ARM only sets state ARMED and the
 * ARMED->RECORDING edge must land on a boundary or the loop is not a whole
 * number of bars. */
enum { LBC_NONE = 0, LBC_ARM, LBC_PLAY, LBC_STOP, LBC_CLEAR, LBC_ACTION = 0xff };
#define LBC_HARD 0x100

enum { L2C_SRC = 0, L2C_BARS, L2C_LEVEL, L2C_FEEDBACK, L2C_OVERDUB,
       L2C_RATE, L2C_REVERSE, L2C_SLICE, L2C_MUTE, L2C_LANE, L2C_COUNT };

typedef struct {
    BB_ATOMIC(int) cmd;         /* REAL for every slot incl. 0; see above     */
    BB_ATOMIC(int) pend;        /* audio->UI: latched cmd awaiting a boundary */
    BB_ATOMIC(int) status;      /* LOOP_*; slot 0 ALIASES bb.loop_status      */
    BB_ATOMIC(int) src;         /* BB_LOOP_SRC_*; slot 0 reads MASTER, w/o    */
    BB_ATOMIC(int) bars;        /* 0 = FOLLOW; slot 0 ALIASES bb.loop_bars    */
    BB_ATOMIC(int) level;       /* 0..256; slot 0 ALIASES bb.loop_mix         */
    BB_ATOMIC(int) feedback;    /* 0..256; slot 0 ALIASES bb.loop_feedback    */
    BB_ATOMIC(int) overdub;     /*         slot 0 ALIASES bb.loop_overdub     */
    BB_ATOMIC(int) rate;        /*         slot 0 ALIASES bb.loop_rate        */
    BB_ATOMIC(int) reverse;     /*         slot 0 ALIASES bb.loop_reverse     */
    BB_ATOMIC(int) slice;       /* 1,2,4,8,16; slot 0 ALIASES bb.loop_slice   */
    BB_ATOMIC(int) mute;        /* satellites only; slot 0 reads 0, w/o       */
    BB_ATOMIC(int) lane;        /* ARRANGE commit target. THE ENGINE NEVER
                                 * READS THIS -- it is UI state that rides
                                 * the session line, like bb_ret_name[].      */
    BB_ATOMIC(unsigned) frames; /* recorded length; slot 0 ALIASES loop_frames */
    BB_ATOMIC(unsigned) pos;    /* play position;   slot 0 ALIASES loop_pos    */
    BB_ATOMIC(int) barlen;      /* bar_len IN FRAMES AT CAPTURE. Drives the
                                 * ARRANGE handoff, PLAY re-phasing, and the
                                 * tempo-drift chip. 0 for slot 0.            */
    BB_ATOMIC(int) peak;        /* max-hold 0..32767; UI clears by exchange    */
} Looper;

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

/* What REC (and the raw TCP sink) capture off the master bus.
 *
 * BB_REC_MASTER is the whole finished bus -- voices, drums, chamber, phrase
 * looper AND the arrangement -- which is what you want when you are printing
 * a mix.
 *
 * BB_REC_LIVE omits the arrangement's clip playback. That is the overdub
 * case: loop a song you have already arranged, play over the top of it, and
 * record only what you played, so the backing does not get printed into the
 * take a second time and pile up on every pass.
 *
 * The distinction is exact rather than a subtraction after the fact, because
 * the clip sum is accumulated separately in the render loop and the master
 * gain stage is simply applied twice -- once with it, once without. The
 * reason that is even possible is an accident of ordering worth writing down:
 * clips are summed AFTER the chamber (engine.c, "R2 song playback"), so no
 * clip ever feeds the reverb, and dropping them cannot strand a wet tail of
 * material that is not in the recording. If a future change moves the clip
 * sum above the chamber, this guarantee dies with it. */
enum { BB_REC_MASTER = 0, BB_REC_LIVE };

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
    BB_ATOMIC(int)  on;                    /* slot audible?                  */
    BB_ATOMIC(int)  gate[BB_STEPS];        /* SMP_GATE_*                    */
    BB_ATOMIC(int)  pitch[BB_STEPS];       /* semitone offset, -12..+12     */
    BB_ATOMIC(int)  vel[BB_STEPS];         /* per-step velocity, 0..255     */
    BB_ATOMIC(int)  ctl[SMP_CTL_COUNT];    /* LEVEL 0..256, CHOKE 0..4      */
    BB_ATOMIC(int)  mute;                  /* 1 = silenced by hand          */
    BB_ATOMIC(int)  solo;                  /* 1 = only soloed slots sound   */

    /* Post-level abs peak of the slot's contribution to the master bus,
     * 0..32767. Audio thread max-holds it once per period; the UI reads
     * with atomic_exchange(&peak, 0) and applies its own display decay. */
    BB_ATOMIC(int)  peak;
} SamplerSlot;

/* ---- GRAIN MASS wells -----------------------------------------------------
 * Four FREE-RUNNING sample players. Where a step-sampler slot is fired by a
 * pattern and retriggers from frame 0, a well is a bed: you start it, it runs
 * (optionally looping, optionally backwards) until you stop it.
 *
 * These lived in JUCE until 2026-08-05, as SamplerVoice objects whose
 * mixInto() added into the device's float buffers AFTER bb_engine_render()
 * had already returned. Everything this instrument does with finished audio
 * reads bb.sink -- the WAV recorder, the master meter, the scope, and the
 * loop bank's capture of the pre-master bus -- so a well was audible and
 * nothing else: REC did not record it and SURVIVOR could not loop it. On a
 * workflow built out of layered loops that is the expensive kind of bug, and
 * R1's own notes predicted it in as many words: the step sampler's audio was
 * put in the engine "because REC and SURVIVOR capture the engine's master
 * bus; a JUCE-only mixer would sit outside the sink/looper". The wells were
 * that JUCE-only mixer. They are now summed inside the render loop beside the
 * LICKS bus, which is what makes all four true at once.
 *
 * Same split as the sampler above: the CONTROLS live here, the sample audio
 * is published through bb_engine_well_set() and owns nothing in this struct.
 * There is deliberately no `on` flag -- "is this well loaded" has exactly one
 * answer, the published buffer pointer, and a second flag beside it could
 * only ever disagree with it. */
#define BB_NWELL 4
enum {
    WELL_CTL_LEVEL = 0,  /* 0..256 mix level into the master bus, 256 = unity */
    WELL_CTL_PITCH,      /* semitone offset, -24..+24                         */
    WELL_CTL_COUNT
};

typedef struct {
    BB_ATOMIC(int)  play;      /* 1 = sounding. The AUDIO thread clears this   */
                               /*   when a non-looping well reaches its end,   */
                               /*   which is how the PLAY plate un-latches.    */
    BB_ATOMIC(int)  loop;      /* 1 = wrap at the end instead of stopping      */
    BB_ATOMIC(int)  reverse;   /* 1 = play backwards from the current point    */
    BB_ATOMIC(int)  arm;       /* bar-synced start pending (PLAY ALL)          */
    BB_ATOMIC(int)  ctl[WELL_CTL_COUNT];
    /* There is deliberately no `mute`. One was written, honoured by the render
     * loop and round-tripped through the session before anyone noticed that no
     * control anywhere could set it -- an engine value with no way in, which is
     * the same lie as a control with no way out, and persisted besides. LEVEL 0
     * silences a well today. When the console condenses and voices, sampler
     * slots, wells and returns share one strip grammar, mute arrives for all
     * four at once or not at all. */

    /* Play position as 0..65536 over the whole sample, published once per
     * period whether or not the well is sounding -- unlike the JUCE version,
     * which only ever published from inside its mix loop and so left a stale
     * value behind on stop. Cosmetic; torn reads do not matter. */
    BB_ATOMIC(int)  pos;

    /* Post-level abs peak, 0..32767, max-held once per period and read with
     * atomic_exchange(&peak, 0), exactly like SamplerSlot::peak above. */
    BB_ATOMIC(int)  peak;
} WellSlot;

/* ---- THE RETURN BUS -------------------------------------------------------
 * Eight pre-allocated return slots. bb_engine_render() never allocates, so
 * "create" and "destroy" are a TYPE CHANGE over a fixed array, executed by a
 * UI-thread quiesce handshake that reuses the same two-render-epoch proof as
 * bb_reclaim().
 *
 * Every return->return edge is delayed by exactly one sample, unconditionally,
 * including the diagonal. Three things fall out of that and all three are
 * load-bearing:
 *
 *   - slot index is acoustically invisible; renumbering returns is not an
 *     audio edit;
 *   - the order in which slots are processed cannot affect one output sample,
 *     so there is no evaluation-order question to get wrong;
 *   - the stability argument is one line: x[n+1] = f(A*x[n] + B*u[n]) where f
 *     ends in a hard-ceilinged limiter, therefore |x[n]| <= CEIL for every n,
 *     every matrix A, every patch, with no eigenvalue analysis.
 *
 * The cost of the uniform rule is 23us per hop at 44.1kHz.
 *
 * BB_NRET, BB_RET_NPARAM, BB_RET_NAME and the RET_* effect ids come from
 * ret.h, included at the top of this file. What lives HERE is everything the
 * two threads share about the ROUTING: the sources, the per-slot control
 * block, the send and link matrices, and the slot-0 alias. */

/* Send sources. 0..7 mirror the voices (post-fader `con`, exactly where the
 * CHAMBER send taps today). 8 is the LICKS sampler bus (mix - premix). 9 is
 * the DRY master tap: `mix` after voices, sampler and wells, BEFORE any return
 * output and before the arrangement. 10 is the previous frame's summed return
 * output -- one knob for global master feedback, the no-input-mixer row, well
 * defined because it is a frame old. 11 is the GRAIN MASS well bus, tapped the
 * same way LICKS is; a sample into the big chamber is most of what the wells
 * are for, and a well that could reach the master but no effect would have
 * been the only strip on the MIXER with no send at all.
 *
 * Index 11 previously carried a comment reserving it for BB_RET_SRC_LOOP,
 * which is worth reading as a small lesson: the sentence before it explains at
 * length why the phrase looper must NEVER be a send source, and then the next
 * sentence holds an id open for it anyway. Nothing implemented it, nothing
 * could have (BB_RET_NSRC was 11, so 11 was out of range and no session can
 * contain it), and reserving an id for a thing you have just argued is
 * forbidden is a dead control in comment form. The reservation is gone.
 *
 * The arrangement is NOT a source and must never become one. Clips are summed
 * AFTER the return bus; that ordering is what makes BB_REC_LIVE exact and it
 * is written down at the BB_REC_* enum above. The phrase looper is not a
 * source either -- it lives after dsp_clip16, and a hard clipper inside a
 * feedback loop is the full-scale-square-into-headphones failure the limiter
 * exists to prevent. */
enum {
    BB_RET_SRC_V0    = 0,
    BB_RET_SRC_LICKS = BB_NLAYER,      /* 8  */
    BB_RET_SRC_DRY,                    /* 9  */
    BB_RET_SRC_WET,                    /* 10 */
    BB_RET_SRC_MASS,                   /* 11 */
    BB_RET_NSRC                        /* 12 */
};

typedef struct {
    /* SLOT 0 IS THE CHAMBER, AND ITS STORAGE IS THE LEGACY ATOMICS.
     * bb.ret[0].level     is UNUSED -- slot 0's level IS bb.verb_level.
     * bb.ret[0].param[0]  is UNUSED -- slot 0's P0    IS bb.verb_size.
     * bb.ret[0].param[1]  is UNUSED -- slot 0's P1    IS bb.verb_tone.
     * bb.ret_send[s][0] for s < 9 is UNUSED -- those cells ARE
     *   bb.layer[s].send and bb.smp_send.
     * The alias is UNCONDITIONAL ON TYPE: bb.verb_size is simply where slot 0's
     * P0 lives whatever effect slot 0 holds. Always go through the
     * ret_*_load/ret_*_store accessors below (or the bb_engine_ret_* API),
     * which redirect. DO NOT add a shadow copy that is "kept in sync" -- the
     * two will drift, the session round-trip will pick the wrong one, and the
     * chamber's golden hash will fail for a reason that looks like DSP. This
     * alias is why the mixer's RETURN A strip, the `verb` session key and the
     * chamber round-trip checks in the suite need zero edits. */
    BB_ATOMIC(int) type;                 /* raw; may exceed RET_NTYPE          */
    BB_ATOMIC(int) level;                /* 0..256 into the master             */
    BB_ATOMIC(int) param[BB_RET_NPARAM]; /* 0..255                             */
    BB_ATOMIC(int) sync;                 /* 0 = free, 1..10 = step division,
                                          * same table as space_samples()      */
    BB_ATOMIC(int) mute;                 /* mixer mute; freezes the slot       */
    BB_ATOMIC(int) arm;                  /* ENGINE-INTERNAL handshake, 1 = run.
                                          * NOT persisted, NOT user visible.
                                          * The UI clears it to quiesce a slot
                                          * before a type change.              */
    BB_ATOMIC(int) quiet;                /* audio -> UI: fade reached 0 and the
                                          * effect was not run for a whole
                                          * period. Read only by the handshake */
    BB_ATOMIC(int) peak;                 /* max-hold, clamped 32767; UI clears
                                          * with atomic_exchange               */
    BB_ATOMIC(int) gr;                   /* limiter gain reduction, Q8:
                                          * 256 = none, 0 = fully closed       */
} Return;

/* Largest block we will ever accept from the audio device. The audio
 * thread's scratch buffer is this big and lives on its stack -- no malloc. */
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
 * engine.c saves and loads, and because a layer has one whether or not the
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
    BB_ATOMIC(Program *) prog;

    BB_ATOMIC(int)  on;                     /* audible?                        */
    BB_ATOMIC(int)  mode;                   /* BB_BYTE / BB_SIGNED / BB_WORD   */
    BB_ATOMIC(int)  param[BB_NPARAM];       /* p0..p7, 0..255                  */
    BB_ATOMIC(int)  ctl[LCTL_COUNT];

    BB_ATOMIC(int)  seq_on;
    BB_ATOMIC(int)  seq_gate[BB_STEPS];     /* GATE_OFF / ON / ACCENT          */
    BB_ATOMIC(int)  seq_pitch[BB_STEPS];    /* semitone offset, -12..+12       */
    BB_ATOMIC(int)  seq_ratchet[BB_STEPS];  /* retriggers in one step, 1..4    */
    BB_ATOMIC(int)  seq_prob[BB_STEPS];     /* probability that step fires     */
    BB_ATOMIC(int)  seq_lock[BB_LOCK_COUNT][BB_STEPS]; /* -1 = live knob       */
    BB_ATOMIC(unsigned) motion_mask;        /* interpolated automation lanes   */

    /* MIDI / hardware hooks. The UI thread writes a one-shot impulse and the
     * audio thread consumes it on the next sample -- no lock, same pattern as
     * every other control. */
    BB_ATOMIC(int)  mtrig;                  /* 1 = fire a hit, consumed once   */
    BB_ATOMIC(int)  mvel;                   /* velocity 1..256 for that hit    */
    BB_ATOMIC(int)  mtrans;                 /* semitone transpose, -24..+24    */

    /* Post-fader abs peak of this voice's contribution to the master mix,
     * 0..32767. Audio thread max-holds it once per period; the UI reads
     * with atomic_exchange(&peak, 0) and applies its own display decay. */
    BB_ATOMIC(int)  peak;

    /* Send into the CHAMBER return bus (reverb), 0..255. Post-fader, so a
     * silent voice sends nothing and the mixer fader rides the send too. */
    BB_ATOMIC(int)  send;
} Layer;

struct bb_state {
    Layer        layer[BB_NLAYER];
    BB_ATOMIC(int)   focus;         /* layer the UI is editing               */

    /* 8 one-shot sample slots sequenced on the step clock (see above).   */
    SamplerSlot  sampler[BB_SAMPLER];

    /* 4 free-running GRAIN MASS wells (see above).                       */
    WellSlot     well[BB_NWELL];

    /* Incremented by the audio thread at the top of every block. The UI
     * thread uses it to know when a retired Program can no longer be in use:
     * a Program retired during epoch N cannot still be referenced once the
     * audio thread has started epoch N+2, because the render that could have
     * been holding it has by then returned. See bb_reclaim() in engine.c. */
    BB_ATOMIC(unsigned long long) epoch;

    /* --- transport ------------------------------------------------------ */
    BB_ATOMIC(int)   rate;          /* rate the device actually gave us      */
    BB_ATOMIC(int)   req_rate;      /* rate the user asked for               */
    BB_ATOMIC(unsigned) t;          /* published sample counter (display)    */
    BB_ATOMIC(int)   reset_t;
    BB_ATOMIC(int)   reset_loop;
    BB_ATOMIC(unsigned) k;          /* published loop position               */
    BB_ATOMIC(unsigned) bar;
    BB_ATOMIC(int)   seq_pos;       /* published playhead step, -1 if off    */
    /* bar and seq_pos, packed into ONE word so the pair can be read without
     * tearing: bar in the high 32 bits, seq_pos in the low 32 as a signed
     * value so the idle -1 survives the round trip.
     *
     * Reading the two fields above separately is a torn read -- the audio
     * thread can have published the new bar but not yet the new step, and a
     * caller that combines them gets a position nearly a whole bar out, once
     * per bar, forever. That showed up as the ARRANGE playhead jumping
     * backwards on every loop pass. Anything that needs BOTH numbers must
     * read this; anything that needs only one can still read that one. */
    BB_ATOMIC(unsigned long long) pos;

    /* --- master --------------------------------------------------------- */
    BB_ATOMIC(int)   gctl[GCTL_COUNT];
    BB_ATOMIC(int)   gain;          /* 0..256                                */
    BB_ATOMIC(int)   mute;
    BB_ATOMIC(int)   panic;
    BB_ATOMIC(int)   bypass;        /* all post chains off                   */

    /* --- RETURN A: the CHAMBER (master reverb bus) ----------------------
     * Per-voice sends live in Layer.send; the step-sampler bus has one
     * shared send below. The wet return is summed into the master mix
     * BEFORE the clip, the phrase looper and the sink, so REC and
     * SURVIVOR capture the tail like everything else. verb_level 0 is
     * bit-exact bypass. */
    BB_ATOMIC(int)   verb_size;     /* decay length, 0..255                  */
    BB_ATOMIC(int)   verb_tone;     /* damping: 0 = dark cavern, 255 = bright */
    BB_ATOMIC(int)   verb_level;    /* return level into the master, 0..256  */
    BB_ATOMIC(int)   smp_send;      /* LICKS sampler-bus send, 0..255        */
    BB_ATOMIC(int)   verb_peak;     /* return abs peak for the RETURN A meter */

    /* --- the return bus (slot 0 IS the CHAMBER above; see `Return`) ----- */
    Return ret[BB_NRET];

    /* 0..255. [s][0] for s < BB_RET_SRC_DRY is UNUSED -- those cells ARE
     * bb.layer[s].send and bb.smp_send. Go through ret_send_load/store. */
    BB_ATOMIC(int) ret_send[BB_RET_NSRC][BB_NRET];

    /* return -> return, 0..256 (256 = unity). EVERY entry is one frame
     * delayed, including ret_link[r][r], which is legal and is the
     * freeze / regeneration cell. No over-unity: it would only make the
     * runaway arrive faster at the same ceiling. */
    BB_ATOMIC(int) ret_link[BB_NRET][BB_NRET];

    BB_ATOMIC(int) ret_active;           /* live slot count, published/period  */

    /* --- master phrase looper ----------------------------------------- */
    BB_ATOMIC(int)   loop_cmd;       /* UI writes, audio thread consumes     */
    BB_ATOMIC(int)   loop_status;    /* LOOP_* published by audio thread     */
    BB_ATOMIC(int)   loop_bars;      /* capture length, 1..4 bars            */
    BB_ATOMIC(int)   loop_mix;       /* dry/loop crossfade, 0..256           */
    BB_ATOMIC(int)   loop_feedback;  /* retained audio while overdubbing     */
    BB_ATOMIC(int)   loop_overdub;
    BB_ATOMIC(int)   loop_rate;      /* half / normal / double               */
    BB_ATOMIC(int)   loop_reverse;
    BB_ATOMIC(int)   loop_slice;     /* 1,2,4,8,16: repeated fraction        */
    BB_ATOMIC(unsigned) loop_pos;
    BB_ATOMIC(unsigned) loop_frames;

    /* --- the loop bank (slot 0 IS the phrase looper above; see `Looper`) - */
    Looper loopn[BB_NLOOP];
    BB_ATOMIC(int) loop_cycle_bars; /* published mirror of g_sat_cyc_bars;
                                     * 0 = none. NOT PERSISTED -- no loop
                                     * audio is persisted either.            */
    BB_ATOMIC(int) loop_active;     /* live satellite count, published/period */

    /* --- R2 arrangement timeline ---------------------------------------
     * Command/status traffic for the song. The song itself (clip list +
     * audio) is published through bb_engine_song_publish() as one atomic
     * pointer swap, exactly like a Program -- it never lives in this
     * struct. */
    BB_ATOMIC(int)   arr_rec_status; /* ARR_REC_*, published by audio thread  */
    BB_ATOMIC(unsigned) arr_rec_frames; /* capture progress in frames, audio  *
                                  * thread writes, UI reads               */
    BB_ATOMIC(int)   arr_seek_bar;   /* pending seek target in absolute bars: *
                                  * UI writes, audio thread consumes at   *
                                  * the top of a period; -1 = none        */
    BB_ATOMIC(int)   arr_play;       /* 1 = the timeline sounds, 0 = it does  *
                                  * not. The song used to play whenever   *
                                  * the transport ran, with no way to     *
                                  * stop it short of deleting the clips.  *
                                  * Stopping HOLDS the per-clip counters   *
                                  * rather than resetting them, so PLAY    *
                                  * resumes where the bar grid now is --   *
                                  * seek is still the thing that restarts. */
    BB_ATOMIC(int)   rec_src;        /* BB_REC_* -- what REC and the TCP sink *
                                  * capture. MASTER is everything;        *
                                  * LIVE omits the arrangement so you can  *
                                  * loop a song and overdub against it     *
                                  * without printing it into the take     *
                                  * again. Exact, not approximate: clips   *
                                  * are summed AFTER the chamber, so       *
                                  * dropping them strands no reverb tail.  */

    /* --- telemetry ------------------------------------------------------ */
    BB_ATOMIC(int)   xruns;
    BB_ATOMIC(int)   cpu_us;
    BB_ATOMIC(int)   budget_us;
    BB_ATOMIC(int)   running;
    BB_ATOMIC(int)   clipping;      /* master bus hit the rails last period  */

    /* --- scope ring (audio writes, UI reads; a torn read here is a
     *     cosmetic glitch in an ASCII waveform, so no atomics on data) */
    int16_t      scope[BB_SCOPE_LEN];
    BB_ATOMIC(unsigned) scope_w;

    /* --- sink ring: the audio thread's only outbound channel for finished
     *     samples. Everything that consumes it -- the recorder, the master
     *     meter, the scope -- keeps its OWN read cursor privately and reads
     *     back from sink_w, so no cursor lives here. That is deliberate: a
     *     consumer that stalls must not be able to stall or corrupt another,
     *     and a shared cursor is exactly how that would happen. */
    int16_t      sink[BB_SINK_LEN];
    BB_ATOMIC(unsigned) sink_w;
};

extern struct bb_state bb;

static inline int bb_clampi(int v, int lo, int hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ---- session state, owned by engine.c, edited by the GUI ---------------
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

/* Return names: UI-owned, engine-persisted, exactly like bb_expr[]. */
extern char bb_ret_name[BB_NRET][BB_RET_NAME];

#if !defined(__cplusplus)
/* ---- the slot-0 alias, in ONE place ------------------------------------
 * Slot 0's LEVEL / P0 / P1 and its send column have NO storage in bb.ret[0]
 * or bb.ret_send[][0]: they ARE bb.verb_level / verb_size / verb_tone and
 * bb.layer[s].send / bb.smp_send. Every read and write of a return knob --
 * the render snapshot, the session writer, the session loader, the public
 * API -- goes through these four pairs, and nothing else may touch the
 * aliased cells. There are NO shadow copies; see the comment on `Return`.
 *
 * C only. C++ callers (the GUI) use the bb_engine_ret_* functions in
 * engine.h, which are these bodies with range clamping in front. */
static inline int ret_send_load(int s, int r)
{
    if (r == 0) {
        if (s >= 0 && s < BB_NLAYER)
            return atomic_load_explicit(&bb.layer[s].send, memory_order_relaxed);
        if (s == BB_RET_SRC_LICKS)
            return atomic_load_explicit(&bb.smp_send, memory_order_relaxed);
    }
    return atomic_load_explicit(&bb.ret_send[s][r], memory_order_relaxed);
}

static inline void ret_send_store(int s, int r, int v)
{
    if (r == 0) {
        if (s >= 0 && s < BB_NLAYER) {
            atomic_store_explicit(&bb.layer[s].send, v, memory_order_relaxed);
            return;
        }
        if (s == BB_RET_SRC_LICKS) {
            atomic_store_explicit(&bb.smp_send, v, memory_order_relaxed);
            return;
        }
    }
    atomic_store_explicit(&bb.ret_send[s][r], v, memory_order_relaxed);
}

static inline int ret_level_load(int r)
{
    return r == 0 ? atomic_load_explicit(&bb.verb_level, memory_order_relaxed)
                  : atomic_load_explicit(&bb.ret[r].level, memory_order_relaxed);
}

static inline void ret_level_store(int r, int v)
{
    if (r == 0) atomic_store_explicit(&bb.verb_level, v, memory_order_relaxed);
    else        atomic_store_explicit(&bb.ret[r].level, v, memory_order_relaxed);
}

static inline int ret_param_load(int r, int p)
{
    if (r == 0 && p == 0)
        return atomic_load_explicit(&bb.verb_size, memory_order_relaxed);
    if (r == 0 && p == 1)
        return atomic_load_explicit(&bb.verb_tone, memory_order_relaxed);
    return atomic_load_explicit(&bb.ret[r].param[p], memory_order_relaxed);
}

static inline void ret_param_store(int r, int p, int v)
{
    if (r == 0 && p == 0)
        atomic_store_explicit(&bb.verb_size, v, memory_order_relaxed);
    else if (r == 0 && p == 1)
        atomic_store_explicit(&bb.verb_tone, v, memory_order_relaxed);
    else
        atomic_store_explicit(&bb.ret[r].param[p], v, memory_order_relaxed);
}

/* ---- the loop bank's slot-0 alias, in ONE indexed switch ------------------
 * Slot 0's nine aliased controls and its status/frames/pos have no storage in
 * bb.loopn[0]; they ARE the legacy bb.loop_* atomics. Nine named accessor
 * pairs is where the return bus's idiom stops paying, so this is one switch
 * and nothing else may touch the aliased cells. Every read and write -- the
 * render snapshot, the session writer, the session loader, the public API --
 * goes through here.
 *
 * SRC on slot 0 reads the read-only MASTER sentinel and drops stores; MUTE on
 * slot 0 reads 0 and drops stores; LANE has real storage on every slot. */
static inline int loop_ctl_load(int n, int c)
{
    if (n == 0) {
        switch (c) {
        case L2C_SRC:      return BB_LOOP_SRC_MASTER;
        case L2C_BARS:     return atomic_load_explicit(&bb.loop_bars, memory_order_relaxed);
        case L2C_LEVEL:    return atomic_load_explicit(&bb.loop_mix, memory_order_relaxed);
        case L2C_FEEDBACK: return atomic_load_explicit(&bb.loop_feedback, memory_order_relaxed);
        case L2C_OVERDUB:  return atomic_load_explicit(&bb.loop_overdub, memory_order_relaxed);
        case L2C_RATE:     return atomic_load_explicit(&bb.loop_rate, memory_order_relaxed);
        case L2C_REVERSE:  return atomic_load_explicit(&bb.loop_reverse, memory_order_relaxed);
        case L2C_SLICE:    return atomic_load_explicit(&bb.loop_slice, memory_order_relaxed);
        case L2C_MUTE:     return 0;
        default:           break;                       /* LANE: real storage */
        }
    }
    switch (c) {
    case L2C_SRC:      return atomic_load_explicit(&bb.loopn[n].src, memory_order_relaxed);
    case L2C_BARS:     return atomic_load_explicit(&bb.loopn[n].bars, memory_order_relaxed);
    case L2C_LEVEL:    return atomic_load_explicit(&bb.loopn[n].level, memory_order_relaxed);
    case L2C_FEEDBACK: return atomic_load_explicit(&bb.loopn[n].feedback, memory_order_relaxed);
    case L2C_OVERDUB:  return atomic_load_explicit(&bb.loopn[n].overdub, memory_order_relaxed);
    case L2C_RATE:     return atomic_load_explicit(&bb.loopn[n].rate, memory_order_relaxed);
    case L2C_REVERSE:  return atomic_load_explicit(&bb.loopn[n].reverse, memory_order_relaxed);
    case L2C_SLICE:    return atomic_load_explicit(&bb.loopn[n].slice, memory_order_relaxed);
    case L2C_MUTE:     return atomic_load_explicit(&bb.loopn[n].mute, memory_order_relaxed);
    case L2C_LANE:     return atomic_load_explicit(&bb.loopn[n].lane, memory_order_relaxed);
    default:           return 0;
    }
}

static inline void loop_ctl_store(int n, int c, int v)
{
    if (n == 0) {
        switch (c) {
        case L2C_SRC:      return;                       /* pinned to MASTER  */
        case L2C_MUTE:     return;                       /* slot 0 has none   */
        case L2C_BARS:     atomic_store_explicit(&bb.loop_bars, v, memory_order_relaxed); return;
        case L2C_LEVEL:    atomic_store_explicit(&bb.loop_mix, v, memory_order_relaxed); return;
        case L2C_FEEDBACK: atomic_store_explicit(&bb.loop_feedback, v, memory_order_relaxed); return;
        case L2C_OVERDUB:  atomic_store_explicit(&bb.loop_overdub, v, memory_order_relaxed); return;
        case L2C_RATE:     atomic_store_explicit(&bb.loop_rate, v, memory_order_relaxed); return;
        case L2C_REVERSE:  atomic_store_explicit(&bb.loop_reverse, v, memory_order_relaxed); return;
        case L2C_SLICE:    atomic_store_explicit(&bb.loop_slice, v, memory_order_relaxed); return;
        default:           break;                        /* LANE: real storage */
        }
    }
    switch (c) {
    case L2C_SRC:      atomic_store_explicit(&bb.loopn[n].src, v, memory_order_relaxed); break;
    case L2C_BARS:     atomic_store_explicit(&bb.loopn[n].bars, v, memory_order_relaxed); break;
    case L2C_LEVEL:    atomic_store_explicit(&bb.loopn[n].level, v, memory_order_relaxed); break;
    case L2C_FEEDBACK: atomic_store_explicit(&bb.loopn[n].feedback, v, memory_order_relaxed); break;
    case L2C_OVERDUB:  atomic_store_explicit(&bb.loopn[n].overdub, v, memory_order_relaxed); break;
    case L2C_RATE:     atomic_store_explicit(&bb.loopn[n].rate, v, memory_order_relaxed); break;
    case L2C_REVERSE:  atomic_store_explicit(&bb.loopn[n].reverse, v, memory_order_relaxed); break;
    case L2C_SLICE:    atomic_store_explicit(&bb.loopn[n].slice, v, memory_order_relaxed); break;
    case L2C_MUTE:     atomic_store_explicit(&bb.loopn[n].mute, v, memory_order_relaxed); break;
    case L2C_LANE:     atomic_store_explicit(&bb.loopn[n].lane, v, memory_order_relaxed); break;
    default:           break;
    }
}

static inline int loop_status_load(int n)
{
    return n == 0 ? atomic_load_explicit(&bb.loop_status, memory_order_relaxed)
                  : atomic_load_explicit(&bb.loopn[n].status, memory_order_relaxed);
}

static inline unsigned loop_frames_load(int n)
{
    return n == 0 ? atomic_load_explicit(&bb.loop_frames, memory_order_relaxed)
                  : atomic_load_explicit(&bb.loopn[n].frames, memory_order_relaxed);
}

static inline unsigned loop_pos_load(int n)
{
    return n == 0 ? atomic_load_explicit(&bb.loop_pos, memory_order_relaxed)
                  : atomic_load_explicit(&bb.loopn[n].pos, memory_order_relaxed);
}
#endif /* !__cplusplus */

/* Set by the SIGINT/SIGTERM handler so a kill still reaches the .wav
 * header patch-up on the way out. */
extern volatile sig_atomic_t bb_quit_signal;

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
}   /* extern "C" */
#endif

#endif /* BYTEBEAT_H */
