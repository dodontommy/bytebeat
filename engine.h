/* engine.h -- the device-independent audio core.
 *
 * This is the part of the program that makes sound, with every notion of
 * WHERE the sound goes removed from it. The terminal instrument and the GUI
 * both link this file; the only difference between them is who calls
 * bb_engine_render() -- ALSA used to, JUCE does now.
 *
 * Owning all of these here also means the regression suite can exercise the
 * full DSP, sequencer, phrase looper and session round-trip on ANY platform,
 * no sound card required. That is the deal: audio devices come and go, but
 * the instrument is this file.
 */
#ifndef ENGINE_H
#define ENGINE_H

#include "bytebeat.h"

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
#include <cstdint>
extern "C" {
#endif

/* Run the fixed startup wiring once, before any thread starts: per-layer
 * expression delays, space buffers, post chains, the phrase looper, and the
 * published sample rate. Called by whoever owns the audio thread. */
void bb_engine_init(int rate);

/* Render one period: `frames` frames of MONO audio into `out`, duplicated
 * across `channels`. This is the every-sample hot path -- it must never
 * malloc, never lock, and never block. JUCE's device callback calls it; the
 * ALSA thread calls it; the self-test calls it. */
void bb_engine_render(int16_t *out, int frames, int channels);

/* Reset the free-running sample counter and per-layer voice state. */
void bb_engine_reset_t(void);

/* Reset the loop position / phrase looper. */
void bb_engine_reset_loop(void);

/* Master phrase looper commands (UI thread -> engine). */
void bb_engine_loop_command(int cmd);

/* Read-only view of the master phrase-loop buffer (BB_LOOP_LEN samples) for
 * the SURVIVOR waveform and LOOP OUT meter. UI-thread reads race the audio
 * thread's writes; torn reads are cosmetic exactly like bb.scope. */
const int16_t *bb_engine_loop_buffer(void);

/* MIDI / hardware entry points. All UI-thread safe. */
void bb_engine_note_on(int layer, int midi_note, int velocity);   /* fire a hit + set pitch */
void bb_engine_note_off(int layer);                               /* release key / reset pitch */
void bb_engine_key_transpose(int layer, int semitones);           /* -24..+24 */
void bb_engine_cc(int layer, int cc, int value);                  /* 0..127 -> mapped layer control */

/* ---- R1 step sampler sample pool -----------------------------------------
 * The UI thread loads WAV data into a slot; the audio thread plays it under
 * the slot's pattern state (bb.sampler[], owned by whoever edits the grid).
 * `mono` must be malloc'd by the caller: on success this function takes
 * ownership and frees it later (so call it with a fresh allocation, and do
 * NOT touch the buffer afterwards). Call bb_engine_reclaim() (or
 * bb_engine_sampler_reclaim()) once per UI frame so retired buffers get
 * freed. Render-thread safe to call anytime; all UI-thread safe. */
int  bb_engine_sampler_set(int slot, int16_t *mono, int n, int rate);
void bb_engine_sampler_clear(int slot);
void bb_engine_sampler_reclaim(void);   /* UI thread, per frame */
int  bb_engine_sampler_loaded(int slot);

/* Factory kit audio into any still-empty slot 0-2, touching no pattern or
 * level state. Used after a session restore: session.conf persists sampler
 * patterns but not sample memory. */
void bb_engine_demo_kit_samples(void);

/* ---- R2 arrangement timeline (the song) -----------------------------------
 * The song is a flat list of clips scheduled in ABSOLUTE BARS against the
 * same monotonic bar counter the transport publishes (bb.bar). Ten lanes:
 * 0-7 mirror the voices V01-V08, 8 is the LICKS sampler bus, 9 is FILE/MASS.
 * Lanes are organisational labels only -- any clip may sit on any lane.
 * Clip audio is mono int16, because the whole engine bus is mono.
 *
 * Ownership is split exactly like the step-sampler pool:
 *
 *   - Clip AUDIO lives in ArrClipBuf, an opaque immutable buffer created and
 *     released by the UI thread and read by the render thread. Release never
 *     frees: it retires, and the memory is reclaimed on the normal per-frame
 *     bb_engine_reclaim() path once it is at least two epochs old, so the
 *     render thread can never be left reading freed frames.
 *
 *   - The SONG (the clip list) is an immutable snapshot published through
 *     one atomic pointer swap, exactly like a Program. The engine copies the
 *     ArrClip array on publish and retires the OLD song struct epoch+2; it
 *     never frees ArrClipBufs on publish -- their lifetime belongs to the UI
 *     via bb_engine_clip_release().
 *
 * Tempo does not stretch anything: a clip's audio always plays 1:1 at the
 * device rate from its window start, so a BPM change moves the bar grid
 * under the audio rather than resampling it. All calls below are UI-thread
 * only unless noted. */
typedef struct ArrClipBuf ArrClipBuf;   /* opaque published audio buffer */

typedef struct {                        /* one clip in the edit model    */
    int lane;                            /* 0..ARR_LANES-1               */
    unsigned start_bar, len_bars;        /* placement window, len >= 1    */
    int loop;                            /* 1 = repeat audio inside win  */
    int gain;                            /* 0..256, 256 = unity          */
    ArrClipBuf *audio;                   /* may be NULL (silent ghost)    */
    char name[ARR_NAME_MAX];
    char path[ARR_PATH_MAX];             /* source WAV for persistence    */
} ArrClip;

/* Copy `n` mono frames at `rate` into a fresh immutable clip buffer. The
 * caller keeps ownership of `data` (unlike bb_engine_sampler_set, this
 * COPIES). Returns NULL on bad arguments or out of memory. */
ArrClipBuf *bb_engine_clip_create(const int16_t *data, unsigned n, int rate);

/* Retire a clip buffer. Safe while the render thread may still be reading
 * it: the memory is freed by bb_engine_reclaim() once two epochs have
 * passed. NULL is tolerated. Do not touch `b` after this call. */
void        bb_engine_clip_release(ArrClipBuf *b);

/* Frame count / raw frames of a clip buffer, for UI waveform drawing and
 * placement math. NULL-safe (0 / NULL). The frames are immutable for the
 * buffer's whole life, so the UI may read them without ceremony. */
unsigned       bb_engine_clip_frames(const ArrClipBuf *b);
const int16_t *bb_engine_clip_data(const ArrClipBuf *b);

/* Publish the song: copies the `nclips` ArrClips and atomically swaps them
 * in as the render thread's new snapshot; the OLD song struct is retired
 * epoch+2. Clip audio lifetime stays with the UI (see above) -- the engine
 * never frees ArrClipBufs on publish. Returns 0, or -1 on bad arguments or
 * out of memory (the old song keeps playing, like a failed bb_publish). */
int  bb_engine_song_publish(const ArrClip *clips, int nclips);

/* Copy the current song's clip meta into `out` (at most `max` entries) for
 * the UI and the session writer. Returns the count. */
int  bb_engine_song_get(ArrClip *out, int max);

/* Request a transport seek to an absolute bar: the render loop consumes it
 * at the top of the next period, setting the bar counter, bar position and
 * loop counter the way reset_loop does, and rewinding every clip window. */
void bb_engine_song_seek(int bar);

/* The arrangement's own transport, independent of the master RUN.
 *
 * Stopping is a mute, not a pause: the per-clip window counters keep tracking
 * the bar grid while stopped, so PLAY drops in wherever the song has got to
 * rather than resuming from where you stopped. bb_engine_song_seek() remains
 * the way to restart a song from a given bar. Safe from any thread. */
void bb_engine_song_play(int on);
int  bb_engine_song_playing(void);

/* What REC and the raw TCP sink capture: BB_REC_MASTER (everything) or
 * BB_REC_LIVE (everything except the arrangement's clip playback).
 *
 * BB_REC_LIVE is the overdub case -- loop an arranged section, play over it,
 * and print only the new layer instead of stacking the backing again on every
 * pass. It is exact rather than an approximation, because the clip sum is
 * kept apart in the render loop and the master gain stage is applied to the
 * mix twice, once with it and once without, BEFORE the 16-bit clamp. */
void bb_engine_rec_src(int src);
int  bb_engine_rec_src_get(void);

/* Arm per-lane capture: at the NEXT bar boundary the render loop starts
 * copying the post-fader mono contribution of `lane` (voice lanes: the
 * voice's summed contribution; lane 8: the sampler-bus premix; lane 9:
 * refused, returns -1) into `dst`, one int16 per frame, for `bars` whole
 * bars or until `cap` frames run out. `dst` is UI-owned and preallocated --
 * the engine never frees it, and it must outlive the capture. Progress is
 * published in bb.arr_rec_status (ARR_REC_ARMED -> RECORDING -> DONE) and
 * bb.arr_rec_frames. Returns 0, or -1 if the request is invalid or a
 * capture is already in flight. */
int  bb_engine_arr_arm(int lane, int bars, int16_t *dst, unsigned cap);

/* Abandon an armed or running capture; status returns to ARR_REC_IDLE. */
void bb_engine_arr_cancel(void);

/* ---- THE RETURN BUS -------------------------------------------------------
 * Eight pre-allocated return slots (bb.ret[], bytebeat.h), an 11 x 8 send
 * matrix and an 8 x 8 return->return link matrix. bb_engine_render() may not
 * allocate, so "create" and "destroy" are a TYPE CHANGE over the fixed array,
 * not an allocation: the DSP arenas exist from load and never move.
 *
 * Every return->return edge is delayed by exactly one sample, including the
 * diagonal. Slot index is therefore acoustically invisible, the processing
 * order cannot affect one output sample, and the stability proof is one line:
 * every cycle passes through ret_limit()'s hard ceiling, so the state is
 * bounded for every matrix and every patch with no graph analysis at all.
 *
 * SLOT 0 IS THE CHAMBER. Its LEVEL / P0 / P1 and its send column have no
 * storage of their own -- they ARE bb.verb_level / verb_size / verb_tone and
 * bb.layer[s].send / bb.smp_send. Everything below redirects, so every
 * existing caller of those atomics keeps working unchanged, and a session with
 * one CHAMBER and an empty matrix renders bit-identically to the engine that
 * had no return bus. See the comment on `Return` in bytebeat.h. */

/* Lifecycle. ASYNCHRONOUS by necessity: the slot's output is faded out over
 * ~46 ms, then two render epochs must pass (the same proof bb_reclaim() uses)
 * before its arena may be cleared and the new type armed. Drive it by calling
 * bb_engine_ret_service() once per UI frame -- bb_engine_reclaim() already
 * does. bb_engine_ret_pending() is 1 until it lands, roughly 80-110 ms.
 * Returns 0, or -1 on a bad slot or type. UI THREAD ONLY. */
int  bb_engine_ret_create (int slot, int type);
int  bb_engine_ret_destroy(int slot);
int  bb_engine_ret_pending(int slot);
void bb_engine_ret_service(void);

/* Bulk-edit bracket for a session load or a preset recall: quiesce every slot,
 * write whatever you like, release. The quiesce IS the hold -- there is no
 * flag with a stale-forever failure mode. The wait is bounded: if the render
 * thread is not running (startup, before the audio device exists) both calls
 * return promptly. UI THREAD ONLY. */
void bb_engine_ret_quiesce_all(void);
void bb_engine_ret_release_all(void);

/* Knobs. One atomic store each, effective next period, safe from any thread.
 * Slot 0 redirects LEVEL / P0 / P1 to bb.verb_level / verb_size / verb_tone.
 * Values are clamped here AND again in the render snapshot. */
void bb_engine_ret_level(int slot, int v);          /* 0..256           */
int  bb_engine_ret_level_get(int slot);
void bb_engine_ret_param(int slot, int p, int v);   /* p 0..7, v 0..255 */
int  bb_engine_ret_param_get(int slot, int p);
void bb_engine_ret_sync(int slot, int v);           /* 0..10            */
int  bb_engine_ret_sync_get(int slot);
void bb_engine_ret_mute(int slot, int on);
int  bb_engine_ret_mute_get(int slot);
int  bb_engine_ret_type_get(int slot);

/* The matrix. src 0..BB_RET_NSRC-1 (0-7 voices, 8 LICKS, 9 DRY master, 10 the
 * previous frame's summed wet -- the no-input-mixer row). Sends into slot 0
 * from src 0..8 redirect to bb.layer[src].send / bb.smp_send. Sends are
 * 0..255, links 0..256. `from == to` is legal and is the freeze /
 * regeneration cell: one frame of the effect wrapped around itself. */
void bb_engine_ret_send(int src, int slot, int amt);
int  bb_engine_ret_send_get(int src, int slot);
void bb_engine_ret_link(int from, int to, int amt);
int  bb_engine_ret_link_get(int from, int to);

/* Kill the feedback, keep the voices: zero every link and every return level
 * in one call, for a bindable panic that is narrower than bb.panic.
 *
 * Note that bb.panic ALREADY does something stronger for as long as it is
 * held: while panicked the render thread reads the whole matrix as zero, so
 * loops DECAY behind the mute instead of coming back saturated when you
 * release it. That is a deliberate behaviour change -- PANIC used to leave the
 * reverb tail ringing behind the mute and no longer does. */
void bb_engine_ret_panic(void);

/* Load the default session shape: all layers off except the floor, an honest
 * starting point for a blank slate rather than the groove. */
void bb_engine_set_defaults(void);

/* First run: put the five-part noise groove on the table (struck low body,
 * backbeat, ratcheted metal, probabilistic dust, continuous bytebeat floor).
 * The GUI boots into this so it makes sound before you touch anything. */
void bb_engine_first_run(void);

/* Synthesize a self-looping drone specimen into `dir` as SPC-XXXX.wav:
 * `bars` whole bars at the current transport tempo, tail crossfaded into
 * the head so a looping GRAIN MASS well plays it seamlessly. Uses a private
 * compiler/VM context -- UI THREAD ONLY, safe while the instrument plays.
 * Deterministic in (seed, tempo, rate). Returns 0 and the created filename
 * in `out` on success, -1 on failure. */
int  bb_engine_render_specimen(const char *dir, unsigned seed,
                               int bars, char *out, size_t outsz);

/* The DIRECTED specimen: renders the given layer's CURRENT voice --
 * expression, parameters and post chain -- as a self-looping drone
 * (SPC-VNN-XXXX.wav), with slow parameter drift around the values you set
 * and a few cents of tape warble. Same geometry/looping/safety contract as
 * bb_engine_render_specimen. UI THREAD ONLY. */
int  bb_engine_render_specimen_voice(const char *dir, unsigned seed, int bars,
                                     int layer, char *out, size_t outsz);

/* Compile `src` and publish it to a layer; see bytebeat.h for the contract. */
int  bb_engine_publish(int layer, const char *src, ExprError *err);

/* Reclaim retired programs the render thread can no longer be using.
 * Call once per UI frame (it is almost always a no-op). */
void bb_engine_reclaim(void);

/* Free every program, including the currently published ones. Call only after
 * the render thread has stopped, or you are freeing memory that is in use. */
void bb_engine_shutdown(void);

/* ---- session configuration (declared in bytebeat.h) --------------------- */
int  bb_config_set_root(const char *dir);   /* tests redirect the config dir */

/* The audio/clock/phrase-loop invariants, same as the old audio_self_test.
 * Returns 1 if every check passed, else 0 with a message in `err`. */
int  bb_engine_self_test(char *err, size_t errsz);

#ifdef __cplusplus
}
#endif

#endif /* ENGINE_H */
