/* sink.h -- where finished samples go besides the sound card.
 *
 * Two consumers, both driven from the UI thread, both fed from the same
 * lock-free ring the audio thread writes into:
 *
 *   1. a .wav file (44-byte header written by hand, no libsndfile)
 *   2. a raw s16le stream over TCP or stdout, so you can listen on a
 *      laptop while the instrument runs on a headless box over ssh
 *
 * Each consumer has its OWN read cursor into the ring. That is deliberate:
 * a stalled network client must never be able to corrupt or delay the file
 * you are recording, and vice versa.
 */
#ifndef SINK_H
#define SINK_H

#include <stddef.h>

/* port <= 0 disables TCP. use_stdout writes raw s16le to fd 1. */
int  sink_init(int port, int local_only, int use_stdout, char *err, size_t errsz);
void sink_close(void);

/* Drain the ring. Call this every UI frame -- it is the only thing keeping
 * the ring from lapping. */
void sink_service(void);

/* Recording. sink_rec_start() writes the header immediately and patches the
 * two length fields on stop, which is why an interrupted recording still
 * needs stopping cleanly to be playable. */
int         sink_rec_start(int rate, char *path_out, size_t pathsz,
                           char *err, size_t errsz);
void        sink_rec_stop(void);
int         sink_rec_active(void);
unsigned    sink_rec_frames(void);
const char *sink_rec_path(void);

/* Network status for the display: 0 off, 1 waiting, 2 streaming. */
int         sink_net_state(void);
const char *sink_net_desc(void);

/* A URL you can actually paste into a browser, e.g.
 * "http://100.68.127.104:9000". Empty string when streaming is disabled.
 * Derived from SSH_CONNECTION when available -- see discover_url(). */
const char *sink_stream_url(void);

#endif /* SINK_H */
