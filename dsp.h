/* dsp.h -- the fixed post-expression processing chain.
 *
 *   expr -> [ DRIVE ] -> [ TONE ] -> [ CRUSH ] -> [ SPACE ] -> gain -> out
 *
 * This is the part that decides whether the instrument sounds like a
 * chiptune or like a collapsing building. Raw bytebeat is bright, thin and
 * fast; every one of these four stages exists to push it down and back.
 *
 * All integer, no allocation, no branching on anything but knob values.
 */
#ifndef DSP_H
#define DSP_H

#include <stdint.h>

typedef struct {
    int drive;      /* 0..255  saturating pre-gain                      */
    int tone;       /* 1..255  lowpass cutoff, 255 = open               */
    int crush;      /* 0..255  sample-and-hold length                   */
    int spc_time;   /* delay length in SAMPLES (caller converts from ms) */
    int spc_fb;     /* 0..255  feedback amount                          */
    int spc_mix;    /* 0..255  wet level                                */
    int spc_freeze; /* recirculate the current delay contents at unity   */
    int bypass;
} PostParams;

typedef struct {
    int64_t   tone_st;    /* Q8 lowpass state              */
    int32_t   hold_val;   /* sample-and-hold latch         */
    int       hold_cnt;
    int32_t  *dl;         /* SPACE delay buffer, caller-owned */
    uint32_t  dlen;       /* power of two                  */
    uint32_t  dmask;
    uint32_t  dw;
    int32_t   dc_x1;      /* DC blocker: last input        */
    int64_t   dc_y1;      /* DC blocker: last output, Q16  */
} PostState;

void    post_init(PostState *ps, int32_t *buf, uint32_t len_pow2);
void    post_reset(PostState *ps);
int32_t post_process(PostState *ps, int32_t x, const PostParams *pp);

/* Always-on ~4Hz highpass applied after the chain and before master gain.
 * NOT bypassable -- see the comment in dsp.c. */
int32_t dsp_dcblock(PostState *ps, int32_t x);

/* Hard-limit to the int16 range. Exposed because the audio thread needs it
 * after applying master gain too. */
int32_t dsp_clip16(int32_t x);

#endif /* DSP_H */
