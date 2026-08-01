/* examples.h -- the starter bank.
 *
 * Each entry ships with its knobs, its output mode and its post-chain
 * settings, because a bytebeat expression without its parameter values is
 * like a patch sheet with no numbers on it. Pressing 'n' in the UI loads all
 * of it at once, so every one of these makes a sound immediately.
 *
 * The same list is reproduced in EXAMPLES.txt with longer prose.
 */
#ifndef EXAMPLES_H
#define EXAMPLES_H

typedef struct {
    const char *name;
    const char *desc;
    const char *expr;
    int mode;                 /* 0 BYTE, 1 SIGNED, 2 WORD                */
    int p[8];
    int drive, tone, crush;   /* post chain                              */
    int spc_time, spc_fb, spc_mix;
    int bpm, beats, bars;
} Example;

static const Example EXAMPLES[] = {

/* ---- classic bytebeat: bright, fast, memoryless ---------------------- */
{ "sierpinski",
  "Ramp gated by two shifted copies of itself; power-of-two shifts nest.",
  "t*(t>>p0&t>>p1)",
  0, {12,8,0,0,0,0,0,0}, 0,255,0, 100,0,0, 90,4,2 },

{ "crowd",
  "Viznut's classic. OR of two shifts picks the octave, &63 the timbre.",
  "t*((t>>p0|t>>p1)&p2&t>>4)",
  0, {12,8,63,0,0,0,0,0}, 0,255,0, 100,0,0, 90,4,2 },

{ "minimal",
  "The whole idea in five characters: a ramp ANDed with a slower ramp.",
  "t&t>>p0",
  0, {8,0,0,0,0,0,0,0}, 0,255,0, 100,0,0, 90,4,2 },

{ "modulo pitch",
  "Modulo resets the ramp every p0+1 samples, so p0 IS the pitch knob.",
  "(t%(p0+1))*(t>>p1)",
  0, {63,9,0,0,0,0,0,0}, 0,255,0, 100,0,0, 90,4,2 },

{ "ring metal",
  "Two AM'd tones multiplied: sum and difference tones, inharmonic, bell-like.",
  "(t*p0&t>>p1)*(t*p2&t>>p3)",
  0, {5,7,3,10,0,0,0,0}, 30,220,0, 100,0,0, 90,4,2 },

/* ---- death industrial: low, slow, filtered, with memory -------------- */
{ "two saws",
  "Detuned sawtooths XORed and lowpassed. p0/p1 one apart = slow beating.",
  "lp(t*p0^(t*p1>>1),p2)",
  2, {48,49,40,0,0,0,0,0}, 40,90,0, 140,150,60, 70,4,2 },

{ "rumble",
  "Square into a leaky integrator = triangle. The >>6 leak stops it drifting.",
  "lp(s0=s0-(s0>>6)+((t>>p0&1)?p1:-p1),p2)",
  2, {9,200,60,0,0,0,0,0}, 60,70,0, 90,120,40, 70,4,2 },

{ "hangar",
  "Output written back into the delay line and read a third of a second later.",
  "w(lp((t*p0^t>>p1)+(d(p2*64+1)>>1),p3))",
  2, {32,11,180,48,0,0,0,0}, 30,80,0, 200,200,90, 60,4,4 },

{ "beat gate",
  "bt/bl open a window at the top of each beat. Ritual pulse over a drone.",
  "(bt<bl>>p0)*((r>>p1)^lp(t*p2,p3))",
  2, {3,18,40,60,0,0,0,0}, 50,110,0, 160,180,80, 60,4,2 },

{ "loop + drift",
  "The k half repeats exactly every loop. The r half never repeats.",
  "lp(((k>>p0&k>>p1)*p2<<6)^(r>>p3),p4)",
  2, {9,12,60,18,100,0,0,0}, 40,100,20, 180,170,70, 66,4,2 },

{ "tape wobble",
  "A delay tap modulated by a slow ramp. Pitch slips like a stretched loop.",
  "w(lp(t*p0,p1))+d(p2*32+(t>>p3&511))",
  2, {36,50,200,7,0,0,0,0}, 30,75,0, 220,190,110, 60,4,4 },

{ "bar switch",
  "n counts bars. Every fourth bar the fundamental jumps. Slow structure.",
  "lp(t*((n&3)==3?p0:p1)^(t>>p2),p3)",
  2, {27,40,10,45,0,0,0,0}, 50,85,0, 170,160,70, 56,4,4 },

{ "collapse",
  "Three shifted ramps multiplied, wrapped to 16 bits, heavily filtered.",
  "lp(((t>>p0)*(t>>p1)*(t>>p2))&0xffff,p3)",
  2, {6,8,10,35,0,0,0,0}, 90,60,30, 150,200,100, 52,4,4 },

{ "static field",
  "Filtered noise, amplitude-modulated by a slow ramp: a bed that breathes.",
  "(lp(r>>p0,p1)*((t>>p2&p3)+1))>>3",
  2, {14,20,13,7,0,0,0,0}, 20,55,0, 240,210,120, 52,4,4 },
};

#define N_EXAMPLES ((int)(sizeof EXAMPLES / sizeof EXAMPLES[0]))

#endif /* EXAMPLES_H */
