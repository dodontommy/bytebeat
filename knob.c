/* knob.c -- ladders and units. See knob.h for why this exists at all.
 *
 * Every ladder below is the answer to one question: "as this number goes up,
 * at which values does the sound actually change?" Everything between two
 * rungs is either inaudible or a duplicate, so the UI never stops there.
 */

#include "knob.h"
#include "expr.h"

#include <stdio.h>
#include <string.h>
#include <stdint.h>

static int g_sr = 44100;

void knob_set_rate(int sr) { g_sr = sr > 0 ? sr : 44100; }

/* ---- ladders -----------------------------------------------------------
 * SHIFT: `t>>p` is masked to 0..31 by the VM, so 32 is the whole space and
 *   only the low end of it is musical -- above 20 the modulator steps slower
 *   than a bar. Below 2 it is above the sample rate.
 * MUL: `t*p` wraps its low byte every 256/p samples, so pitch is LINEAR in p
 *   and the ear is logarithmic. The rungs thin out as p grows so that one
 *   detent is roughly one musical step all the way up.
 * MASK: `t&p` only produces a clean partial when p is 2^n-1. Every other
 *   value is a lopsided mask that sounds like a mistake, because it is one.
 * CUT / PERIOD: geometric, because both are logarithmic in the ear.
 * NOISE: `r>>p` -- below 16 the low 16 bits are still full scale, so those
 *   values are all the same sound. The ladder starts where it starts changing.
 */
static const unsigned char L_SHIFT[]  = { 2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20 };
static const unsigned char L_MASK[]   = { 1,3,7,15,31,63,127,255 };
static const unsigned char L_NOISE[]  = { 16,17,18,19,20,21,22,23,24,25,26,27,28 };

static const unsigned char L_MUL[]    = {
    1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,
    24,26,28,30,32,34,36,39,42,45,48,52,56,60,64,68,73,78,84,90,
    96,103,110,118,126,135,145,155,166,178,190,204,218,233,250
};

static const unsigned char L_CUT[]    = {
    1,2,3,4,5,6,7,8,9,10,11,12,14,16,18,21,24,27,31,35,40,45,
    52,59,67,77,88,100,114,130,148,169,192,219,249,255
};

static const unsigned char L_PERIOD[] = {
    2,3,4,5,6,7,8,9,10,11,12,14,16,18,21,24,27,31,35,40,45,52,
    59,67,77,88,100,114,130,148,169,192,219,249,255
};

/* bp()'s state-variable coefficient. Frequency is close to sr*f/(512*pi)
 * over the useful range, so geometric rungs feel like musical intervals. */
static const unsigned char L_RESON[] = {
    1,2,3,4,5,6,7,8,9,10,12,14,16,18,21,24,28,32,37,43,50,58,
    67,78,90,104,120,128
};
static const unsigned char L_Q[] = {
    64,96,128,152,176,192,208,220,228,236,242,246,250,253,255
};

/* The gate envelope is the worst offender of the lot. decay_k = 1 + p*p/108
 * and the 60dB time goes as 1/k, so the knob is reciprocal-quadratic in the
 * thing you care about: p=4 and p=10 are both a ten-second fall, while p=200
 * and p=210 are 25ms and 23ms. Below 11 the integer division floors k to 1
 * and every value is identical.
 *
 * These rungs are the smallest p for each k on a 1.32 geometric walk, which
 * makes one press a constant RATIO of decay time -- 5.1s, 3.4s, 2.6s ... 21ms,
 * 17ms -- all the way down. 0 is kept as its own rung because it does not mean
 * "very long", it means hold the gate open until the next step. */
static const unsigned char L_DECAY[] = {
    0, 11, 15, 18, 24, 28, 33, 39, 46, 53, 62,
    72, 84, 97, 112, 129, 149, 172, 198, 228, 255
};

/* LINEAR and AMOUNT get a generated 0,8,16..248,255 ladder: coarse enough
 * that one keypress is audible, fine enough to land anywhere. */
static unsigned char L_COARSE[33];

static const struct { const unsigned char *v; int n; } LADDER[KV_COUNT] = {
    [KV_LINEAR] = { L_COARSE, 33 },
    [KV_SHIFT]  = { L_SHIFT,  (int)(sizeof L_SHIFT)  },
    [KV_MUL]    = { L_MUL,    (int)(sizeof L_MUL)    },
    [KV_MASK]   = { L_MASK,   (int)(sizeof L_MASK)   },
    [KV_CUT]    = { L_CUT,    (int)(sizeof L_CUT)    },
    [KV_PERIOD] = { L_PERIOD, (int)(sizeof L_PERIOD) },
    [KV_NOISE]  = { L_NOISE,  (int)(sizeof L_NOISE)  },
    [KV_AMOUNT] = { L_COARSE, 33 },
    [KV_TIME]   = { L_COARSE, 33 },
    [KV_DECAY]  = { L_DECAY,  (int)(sizeof L_DECAY) },
    [KV_RESON]  = { L_RESON,  (int)(sizeof L_RESON) },
    [KV_Q]      = { L_Q,      (int)(sizeof L_Q) },
};

/* Built once on first use. A constructor would need C11 attributes or an
 * init call from main; a lazy check costs one compare and keeps this file
 * free of startup order questions. */
static void ensure_coarse(void)
{
    if (L_COARSE[32]) return;
    for (int i = 0; i < 32; i++) L_COARSE[i] = (unsigned char)(i * 8);
    L_COARSE[32] = 255;
}

int knob_ndetent(int kind)
{
    ensure_coarse();
    if (kind < 0 || kind >= KV_COUNT) kind = KV_LINEAR;
    return LADDER[kind].n;
}

int knob_value(int kind, int detent)
{
    ensure_coarse();
    if (kind < 0 || kind >= KV_COUNT) kind = KV_LINEAR;
    int n = LADDER[kind].n;
    if (detent < 0) detent = 0;
    if (detent >= n) detent = n - 1;
    return LADDER[kind].v[detent];
}

/* Nearest rung, not floor: land on a patch whose p0 is 70 and the cutoff
 * ladder should say 67, not 59. */
int knob_detent(int kind, int value)
{
    ensure_coarse();
    if (kind < 0 || kind >= KV_COUNT) kind = KV_LINEAR;
    const unsigned char *v = LADDER[kind].v;
    int n = LADDER[kind].n, best = 0, bd = 1 << 30;

    for (int i = 0; i < n; i++) {
        int d = value - (int)v[i];
        if (d < 0) d = -d;
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}

int knob_step(int kind, int value, int dir, int coarse)
{
    int n = knob_ndetent(kind);
    int d = knob_detent(kind, value);

    /* A coarse jump is an eighth of the ladder, so every kind takes the same
     * number of coarse presses to cross regardless of how many rungs it has. */
    int mult = coarse ? (n / 8 > 1 ? n / 8 : 2) : 1;
    d += dir * mult;

    if (d < 0) d = 0;
    if (d >= n) d = n - 1;
    return knob_value(kind, d);
}

int knob_fill(int kind, int value, int width)
{
    int n = knob_ndetent(kind);
    if (n < 2) return width;
    int d = knob_detent(kind, value);
    return (d * width + (n - 1) / 2) / (n - 1);
}

/* Detent windows for the generator, chosen by ear rather than by symmetry.
 * SHIFT stops at 13 because a modulator slower than a couple of seconds
 * sounds like nothing is happening for the first bar. CUT stops short of the
 * top because a fully open lowpass is the same as no lowpass, and the whole
 * reason the stage is there is to take the edge off. */
static const struct { short lo, hi; } GENRANGE[KV_COUNT] = {
    [KV_LINEAR] = {  4, 28 },
    [KV_SHIFT]  = {  2, 13 },
    [KV_MUL]    = {  1, 41 },
    [KV_MASK]   = {  1,  7 },
    [KV_CUT]    = {  8, 31 },
    [KV_PERIOD] = {  4, 26 },
    /* Past four rungs down, `r>>p` is just quieter noise, and quieter is what
     * LEVEL is for. The generator stays where the source still has body. */
    [KV_NOISE]  = {  0,  4 },
    [KV_AMOUNT] = {  8, 28 },
    [KV_TIME]   = {  4, 28 },
    [KV_DECAY]  = {  6, 18 },
    [KV_RESON]  = {  2, 21 },
    [KV_Q]      = {  6, 13 },
};

void knob_gen_range(int kind, int *lo, int *hi)
{
    int n = knob_ndetent(kind);
    if (kind < 0 || kind >= KV_COUNT) kind = KV_LINEAR;

    int a = GENRANGE[kind].lo, b = GENRANGE[kind].hi;
    if (b > n - 1) b = n - 1;
    if (a > b)     a = b;
    *lo = a;
    *hi = b;
}

int knob_kind_for_role(int role)
{
    switch (role) {
    case ROLE_SHIFT:  return KV_SHIFT;
    case ROLE_MUL:    return KV_MUL;
    case ROLE_MASK:   return KV_MASK;
    case ROLE_CUT:    return KV_CUT;
    case ROLE_PERIOD: return KV_PERIOD;
    /* Deliberately NOT KV_TIME: a hand-written `d(p)` is a tap in raw
     * samples, and KV_TIME's unit assumes the rack's scaling. Better a bare
     * number than a confidently wrong millisecond count. */
    case ROLE_DELAY:  return KV_LINEAR;
    case ROLE_LEVEL:  return KV_AMOUNT;
    case ROLE_RESON:  return KV_RESON;
    case ROLE_Q:      return KV_Q;
    default:          return KV_LINEAR;
    }
}

const char *knob_kind_name(int kind)
{
    switch (kind) {
    case KV_SHIFT:  return "rate";
    case KV_MUL:    return "pitch";
    case KV_MASK:   return "timbre";
    case KV_CUT:    return "cutoff";
    case KV_PERIOD: return "pitch";
    case KV_NOISE:  return "noise";
    case KV_AMOUNT: return "amount";
    case KV_TIME:   return "time";
    case KV_RESON:  return "pitch";
    case KV_Q:      return "ring";
    default:        return "misc";
    }
}

/* ---- units -------------------------------------------------------------- */

/* -ln(1 - c/256) * 1000, sampled every 8 and at the very top. This is the
 * only transcendental the project needs and libm is not linked, so it is a
 * table. Interpolating between rungs is more than accurate enough for a
 * number whose job is to tell you roughly where the filter is. */
static const short LN1M[33] = {
       0,   32,   65,   98,  134,  170,  208,  247,  288,  330,  375,
     421,  470,  521,  575,  633,  693,  758,  827,  901,  981, 1068,
    1163, 1269, 1386, 1520, 1674, 1856, 2079, 2367, 2773, 3466, 5545
};

/* One-pole `y += (x-y)*c/256` has RC-equivalent cutoff -ln(1-c/256)*sr/2pi.
 * 6283 is 2pi scaled by the same 1000 the table carries. */
int knob_class(int kind)
{
    switch (kind) {
    case KV_MUL: case KV_PERIOD: case KV_RESON: return KC_PITCH;
    case KV_SHIFT: case KV_DECAY: return KC_RATE;
    case KV_MASK: case KV_NOISE:  return KC_TIMBRE;
    case KV_Q:                     return KC_TIMBRE;
    case KV_CUT:                  return KC_CUTOFF;
    /* An effect time, not a modulation rate. Kept apart so that "faster"
     * speeds up the sound without also shortening the reverb. */
    case KV_TIME:                 return KC_SPACE;
    default:                      return KC_LEVEL;
    }
}

static int cutoff_hz(int c)
{
    if (c < 1) c = 1;
    if (c > 255) c = 255;
    int i = c / 8, f = c % 8;
    if (i >= 32) return (int)((int64_t)g_sr * LN1M[32] / 6283);
    int v = LN1M[i] + (LN1M[i + 1] - LN1M[i]) * f / 8;
    return (int)((int64_t)g_sr * v / 6283);
}

int knob_cutoff_hz(int c) { return cutoff_hz(c); }

static void fmt_hz(int hz, char *buf, size_t n)
{
    if (hz >= 10000)     snprintf(buf, n, "%dkHz", hz / 1000);
    else if (hz >= 1000) snprintf(buf, n, "%d.%dk", hz / 1000, (hz % 1000) / 100);
    else if (hz >= 1)    snprintf(buf, n, "%dHz", hz);
    else                 snprintf(buf, n, "sub");
}

/* An oscillator asked for a frequency above Nyquist does not produce it -- it
 * produces the reflection, at a pitch that has nothing to do with the number
 * you set. Aliasing is half of why this instrument sounds the way it does, so
 * the answer is not to forbid it but to print what you will actually hear,
 * marked with a ~ so it is clear the knob and the ear have parted company. */
static void fmt_osc_hz(int hz, char *buf, size_t n)
{
    int ny = g_sr / 2;
    if (hz <= ny) { fmt_hz(hz, buf, n); return; }

    int m = hz % g_sr;
    if (m > ny) m = g_sr - m;

    char t[16];
    fmt_hz(m, t, sizeof t);
    snprintf(buf, n, "~%s", t);
}

/* Anything faster than about 20Hz is heard as a pitch and anything slower is
 * heard as a movement, so the unit switches at exactly the place your ear
 * does. Printing 0.04Hz for a 24-second sweep would be arithmetically correct
 * and completely useless. */
static void fmt_period_us(int64_t us, char *buf, size_t n)
{
    if (us <= 0) { snprintf(buf, n, "-"); return; }
    if (us < 50000) { fmt_hz((int)(1000000 / us), buf, n); return; }
    if (us < 1000000) { snprintf(buf, n, "%dms", (int)(us / 1000)); return; }
    if (us < 10000000) {
        snprintf(buf, n, "%d.%ds", (int)(us / 1000000), (int)(us / 100000) % 10);
        return;
    }
    snprintf(buf, n, "%ds", (int)(us / 1000000));
}

void knob_fmt_value(int kind, int value, char *buf, size_t n)
{
    switch (kind) {
    case KV_SHIFT: case KV_NOISE: snprintf(buf, n, ">>%d", value); break;
    case KV_MUL:                  snprintf(buf, n, "*%d",  value); break;
    case KV_MASK:                 snprintf(buf, n, "&%d",  value); break;
    case KV_PERIOD:               snprintf(buf, n, "%%%d", value); break;
    default:                      snprintf(buf, n, "%d",   value); break;
    }
}

void knob_fmt_unit(int kind, int value, char *buf, size_t n)
{
    switch (kind) {

    /* `t>>p` steps once every 2^p samples. That step rate is the thing you
     * hear: at p=4 it is a 2.7kHz tone, at p=16 it is a slow sweep. */
    case KV_SHIFT:
        fmt_period_us(((int64_t)1 << value) * 1000000 / g_sr, buf, n);
        break;

    /* `t*p` wraps its low byte every 256/p samples. */
    case KV_MUL:
        fmt_osc_hz(value > 0 ? (int)((int64_t)g_sr * value / 256) : 0, buf, n);
        break;

    /* `t%p` repeats every p samples. */
    case KV_PERIOD:
        fmt_osc_hz(value > 0 ? g_sr / value : 0, buf, n);
        break;

    case KV_RESON:
        /* Small-angle approximation of the Chamberlin state-variable
         * resonator used by bp(). It stays within a semitone over the rack's
         * useful range and, importantly, reports the pitch in the direction
         * the ear hears it move. */
        if (value < 1) value = 1;
        if (value > 128) value = 128;
        fmt_hz((int)((int64_t)g_sr * value / 1608), buf, n);
        break;

    case KV_Q:
        snprintf(buf, n, "%d%%", value * 100 / 255);
        break;

    /* Past Nyquist a lowpass is not filtering anything, and saying "37kHz"
     * invites you to keep turning a knob that has already stopped working. */
    case KV_CUT: {
        int hz = cutoff_hz(value);
        if (hz >= g_sr / 2) snprintf(buf, n, "open");
        else                fmt_hz(hz, buf, n);
        break;
    }

    /* The rack's SPACE delay tap, which rack.c renders as `d(p*96+400)`.
     * That scaling is the only thing this kind is used for, and the constants
     * are repeated here rather than plumbed through because a wrong number on
     * screen is a smaller problem than an indirection nobody can follow. */
    case KV_TIME: {
        int ms = (int)((int64_t)(value * 96 + 400) * 1000 / g_sr);
        if (ms >= 1000) snprintf(buf, n, "%d.%ds", ms / 1000, (ms % 1000) / 100);
        else            snprintf(buf, n, "%dms", ms);
        break;
    }

    /* The gate envelope falls by (env*k)>>16 per sample, so it loses 60dB in
     * ln(1000)*65536/k samples; 452709000 is that constant times 1000, to get
     * milliseconds without leaving integers. Both the decay_k formula and this
     * one are copied from the audio thread. */
    case KV_DECAY: {
        if (value == 0) { snprintf(buf, n, "hold"); break; }
        int k  = 1 + (value * value) / 108;
        int ms = (int)(452709000LL / ((int64_t)k * g_sr));
        if (ms >= 1000) snprintf(buf, n, "%d.%ds", ms / 1000, (ms % 1000) / 100);
        else            snprintf(buf, n, "%dms", ms);
        break;
    }

    /* 2^n-1 is n set bits, and n bits is how many steps the mask leaves. */
    case KV_MASK: {
        int bits = 0;
        for (int v = value; v; v >>= 1) bits++;
        snprintf(buf, n, "%d bit", bits);
        break;
    }

    /* `r>>p` keeps 32-p bits, but only the low 16 survive the output stage,
     * so anything at or below 16 is already full scale. */
    case KV_NOISE: {
        int bits = 32 - value;
        if (bits > 16) bits = 16;
        snprintf(buf, n, "%d bit", bits);
        break;
    }

    case KV_AMOUNT:
        snprintf(buf, n, "%d%%", value * 100 / 255);
        break;

    default:
        buf[0] = '\0';
        break;
    }
}
