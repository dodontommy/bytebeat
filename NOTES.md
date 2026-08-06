# NOTES

Background for the bytebeat instrument. This is the "why" document; the code
comments cover the "why here". Read it in any order — the sections don't
depend on each other.

---

## 1. How PCM audio actually reaches hardware

A speaker is a coil in a magnetic field. Current through the coil moves the
cone. Move the cone in a pattern and you make pressure waves. That's it —
everything else is bookkeeping about *what number to send when*.

A DAC (digital-to-analog converter) on your sound card takes a number and
holds a corresponding voltage until you give it another one. It does this at
a fixed, crystal-locked rate. **Nothing** can make it wait. It is not polling
you; it is not asking; it is going to convert a number every 22.6
microseconds (at 44.1kHz) whether you have supplied one or not.

So the card owns a block of memory — the **ring buffer** — and walks it
forever, wrapping at the end:

```
      write pointer (us)                 read pointer (hardware)
             |                                    |
             v                                    v
  +----+----+----+----+----+----+----+----+----+----+----+----+
  |    |    | NEW| NEW|    |    |    |    |    | old| old| old|
  +----+----+----+----+----+----+----+----+----+----+----+----+
   \_________ period 0 ________/\_________ period 1 __________/
```

Our entire job is to stay ahead of that read pointer. The gap between the two
pointers is our safety margin, measured in time.

The buffer is divided into **periods**. When the hardware finishes a period
it raises an interrupt; the kernel wakes whoever was waiting on the device.
That wakeup is the heartbeat of the whole program:

```
snd_pcm_writei()  ->  blocks  ->  interrupt  ->  returns  ->  we compute
       ^                                                          |
       +----------------------------------------------------------+
```

This is why the terminal instrument's ALSA thread had no timer, no sleep and
no frame counter driving it: the only blocking call at the bottom of its loop
was `snd_pcm_writei`, and it blocked *on purpose*. That thread went out with
the rest of the terminal front end, but the shape survived the port -- JUCE's
device callback chops whatever buffer the host hands it into blocks that fit
the interleaved scratch it already owns (sized on the message thread from the
device's advertised buffer, so the callback never reaches the allocator) and
calls `bb_engine_render()` once per block, so the sound card's own crystal is
still the only scheduler in this program.

### Period size is latency

```
period size = how many frames we hand over at once
            = how long between our wakeups
            = HOW STALE THE SOUND IS
```

When you turn a knob, the samples already sitting in the ring buffer have
already been computed. They cannot be changed. So the soonest your knob can
possibly be heard is after the buffer drains down to where we are writing.

```
buffer size  = period size x period count
             = how much runway before disaster
```

Two knobs, pulling opposite ways:

| period | latency | xrun risk |
|--------|---------|-----------|
| tiny (64 frames, 1.5ms) | instant response | you must never be late, ever |
| huge (4096 frames, 93ms) | mushy, laggy | survives almost anything |

The terminal instrument picked `rate/100` (≈10ms) with 4 periods -- ~40ms of
runway, ~10ms of latency -- and printed the figure it actually got as `lat`.
The JUCE app picks nothing: the device declares its rate and buffer size,
`audioDeviceAboutToStart` sizes the interleaved scratch from that number, and
the tradeoff above is settled in the OS audio control panel rather than by us.

### What an xrun actually is

The hardware reached a part of the buffer we had not written yet. It played
whatever bytes were there — stale audio from one buffer ago, or zeroes.
You hear a click, a tick, or a stutter.

The damage is *already done* by the time we find out. ALSA told the terminal
instrument by returning `-EPIPE` from `snd_pcm_writei`, leaving the stream in
the XRUN state and refusing everything until `snd_pcm_prepare()` restarted it;
counting that and restarting was the whole of its `recover()`.

**An xrun count is a performance instrument, not an error log.** If it climbs
while you sweep a knob, something in your render path got slow. If it climbs
at rest, the machine is too busy or the period is too small. Worth knowing
that nobody currently measures it: `bb.xruns` is still declared in
`bytebeat.h` and nothing has written it since the ALSA thread was deleted.

### Why the audio thread can't malloc

`malloc` can:

- take a lock inside the allocator (another thread may hold it)
- call `mmap` and enter the kernel
- trigger a page fault when you first touch the memory
- walk a free list of unpredictable length

Any of those can take longer than the ~10ms we have. The deadline is not
"usually met" — it is met or you hear it. And the failure isn't a crash you
can debug; it's a click, once, in the middle of a take.

The same argument rules out:

- **locks** — if the UI thread holds it, we wait for the UI thread, which
  might be blocked on a terminal write. Worse is *priority inversion*: a
  low-priority thread holds the lock, gets preempted, and our high-priority
  thread waits on a thread that isn't even running.
- **file I/O** — `fwrite` can block on the page cache, the disk, dm-crypt…
- **sockets** — `send` blocks when the receiving window closes
- **anything in libc that might allocate** — including, annoyingly, some of
  ALSA's own configuration calls

That last one is why the terminal instrument's sample-rate change had to be a
whole dance (§6).

`mlockall(MCL_CURRENT | MCL_FUTURE)` pins every page into RAM so nothing can
be paged out and faulted back in mid-period. `MCL_CURRENT` covers everything
mapped now (the per-layer delay lines and the 2MB sample ring, both in BSS);
`MCL_FUTURE` covers anything mapped later, i.e. the Programs we `malloc`
while live coding.

`SCHED_FIFO` asks the kernel to run this thread until it blocks, rather than
giving it a timeslice like everything else. Both usually need permission --
`setcap cap_sys_nice,cap_ipc_lock=eip` on the binary, or

```
@audio   -  rtprio  95
@audio   -  memlock unlimited
```

in `/etc/security/limits.conf`. The terminal instrument asked for both at
startup, treated either refusal as non-fatal and said so on screen, on the
grounds that a slightly glitchy instrument beats no instrument.

**Neither call is anywhere in the tree now.** They were Linux-only and
went out with the ALSA thread; the JUCE app takes whatever priority the host
gives its device callback and does not pin memory. Anything that goes back to
owning its own audio thread wants both again, which is why the reasoning is
written down here rather than in the file that used to make the calls.

---

## 2. Sample rate and bit depth, physically

### Sample rate

The number of times per second the DAC is handed a new value.

The **Nyquist–Shannon** result: a sampled signal can only represent
frequencies below half the sample rate. At 44100 Hz, the ceiling is 22050 Hz,
which is roughly the top of human hearing. That is where 44.1kHz came from —
not elegance, but the recording format (it fits an NTSC video frame, which is
what early digital masters were stored on).

What happens above Nyquist is the interesting part. It doesn't disappear; it
**folds back**:

```
  true frequency:  0    5k   10k  15k  20k | 25k   30k
  what you hear:   0    5k   10k  15k  20k | 19k   14k     (sr = 44.1k)
                                            ^
                                        Nyquist — the mirror
```

That folding is **aliasing**, and for this instrument it is a feature. A
bitwise expression generates enormous amounts of energy above Nyquist. All of
it folds back down into the audible band at frequencies that have no harmonic
relationship to the original. That is a large part of why bytebeat sounds
metallic and inharmonic rather than like a synthesizer.

**Which is why low sample rates are a feature here.** Drop to 8000 Hz and the
mirror sits at 4000 Hz; nearly everything folds, several times, and the sound
becomes gritty and detuned in a way you cannot get any other way.

There were two ways to exploit that in this program, and one of them is left:

- The terminal instrument retuned the actual PCM device from the keyboard,
  and its `-R` flag disabled ALSA's resampler -- without that, asking a
  48kHz-only card for 1000 Hz got you ALSA politely interpolating, which
  smooths away the aliasing you were trying to hear, so `-R` snapped the
  device to the nearest rate it genuinely supported and the readout said
  `48000(want 1000)`. Both went with that front end. The engine's rate is now
  whatever the audio device reports once, at
  `bb_engine_init(dev->getCurrentSampleRate())`, and nothing moves it after.
- The **CRUSH** knob does it in software with no device change at all: hold
  each sample for N frames and you have decimated to `sr/N` without touching
  the hardware. Sweepable, unlike the real rate, and now the only one of the
  two you can reach.

### Bit depth

How many distinct voltage levels the DAC can produce.

- 8-bit: 256 levels
- 16-bit: 65536 levels

The gap between adjacent levels is the smallest error you can make when
representing a waveform, and that error is **quantisation noise**. Each extra
bit halves it, which is about 6 dB:

```
dynamic range ≈ 6.02 x bits  dB
    8-bit  ->  ~48 dB     audibly grainy, and that graininess is the sound
   16-bit  ->  ~96 dB     CD quality, noise floor below hearing
```

The instrument's three output modes are about which bits of the expression's
32-bit result become the sample:

| mode | mapping | character |
|------|---------|-----------|
| `BYTE` | `(v & 0xff) - 128`, unsigned 8-bit | the classic. Silence is 128. Every bytebeat on the internet assumes this. |
| `SIGNED` | `(int8_t)(v & 0xff)` | same 8 bits, two's complement. Silence is 0, so the *wrap points move* — same expression, different sound. |
| `WORD` | `(int16_t)(v & 0xffff)` | 16 bits. Smoother, wider, much less crunch. Where the drone patches live. |

The engine renders int16 regardless. The mode is a musical decision about how
to fold an int32 down; the wire format is a hardware fact. The terminal
instrument handed those int16s straight to ALSA as `S16_LE`, the one format
every card and plugin supports; the JUCE callback scales them by 1/32768 into
the float buffers the device asked for. The fold is the same either way.

**Watch the wrap in WORD mode.** `(int16_t)(v & 0xffff)` *wraps*, it does not
clip. An expression that exceeds ±32767 doesn't get louder, it tears. That's
occasionally useful and usually not what you meant.

### DC offset — the one that bites

`BYTE` mode maps the low 8 bits by subtracting 128. Correct when the
expression uses the whole byte. But `t&t>>8` spends its early life producing
values 0..31, and 0..31 minus 128 is a near-full-scale **negative constant**
with a small wiggle on top.

That is a DC offset. It is inaudible, it eats your headroom, and it holds a
speaker cone off-centre. You will not notice until a recording is unusable or
something is damaged.

So there is a DC blocker after the post chain, always on, deliberately not
bypassable:

```
y[n] = x[n] - x[n-1] + R*y[n-1]        R = 65500/65536
```

The differencing kills DC outright; `R` just below 1 puts everything back
above ~4 Hz. The post-chain bypass -- `bb.bypass`, which the terminal
instrument put on `B` and which no GUI control currently writes -- skips the
*chain*, not this: "let me hear the raw program" is a reasonable request,
"let me send DC to my mixer" is not.

---

## 3. Why bitwise operators produce music

This is the heart of it. Bytebeat is strange because a handful of operators
on a counter produce rhythm, pitch, and structure, all at once, with no
oscillator anywhere.

### `t` is already a sawtooth

`t` counts 0, 1, 2, 3, … If you keep only the low 8 bits (`t & 0xff`) you get
0…255, 0…255, 0…255 — a **sawtooth wave** with a period of exactly 256
samples. At 44100 Hz that is 172 Hz. You have an oscillator and you did not
write one.

Every mask is a sawtooth. `t & 0xfff` is a sawtooth 16 times slower, 4 octaves
down. **Masks are pitch.**

### `>>` is octaves, exactly

`t >> 1` counts half as fast. `t >> 2`, a quarter. Shifting right by `n`
divides frequency by `2^n` — and a factor of two in frequency is precisely one
octave.

This is the crucial structural fact: **every shift amount is exactly an
octave apart, so every term in a bytebeat expression is automatically in tune
with every other term.** You cannot write a sour note with `>>`. Sweep `p0` in
`t>>p0` and you are transposing in perfect octaves. That's why the parameter
knobs feel musical even though nothing here knows what a note is.

### Combining is where structure appears

Now the operators:

**AND** — `a & b` is 1 only where both are 1. Since `t>>8` changes 256× more
slowly than `t`, `t & (t>>8)` is a fast ramp *gated* by a slow one. The slow
term acts as an envelope. **`&` is amplitude modulation and rhythm.**

```
t & t>>8
  t     : /|/|/|/|/|/|/|/|/|/|/|/|   fast ramp (pitch)
  t>>8  : /            |             slow ramp (envelope)
  result: .|.|/|/|/|/|/|.|.|         gated, pulsing
```

**OR** — sets bits, so it *adds* harmonics rather than removing them.
`t>>12 | t>>8` is two octaves sounding together.

**XOR** — the interesting one. `a ^ b` toggles, so it produces *difference*
patterns. XOR of two periodic signals with periods P and Q repeats at
lcm(P, Q). With power-of-two periods that lcm is just the larger one, which
keeps things consonant. Mix in a non-power-of-two (via `*` or `%`) and the
lcm explodes — you get patterns that take thousands of samples to repeat.
That is where bytebeat's famous "it evolves for minutes" behaviour comes
from.

**Multiply** — `t * (something)` is frequency modulation: the rate at which
the ramp climbs is now itself varying. This is what makes the classic
formulas sing rather than click.

**Modulo** — `t % 100` is a sawtooth of period exactly 100 samples, which is
*not* a power of two. `%` is how you escape the octave lattice and get
arbitrary pitch. In the `modulo pitch` example, `p0` is literally a pitch
knob.

### Self-similarity

The canonical:

```
t * (t>>12 | t>>8) & 63 & t>>4
```

- `t>>12` and `t>>8` are the same ramp 4 octaves apart
- `|` sounds them together
- `t *` frequency-modulates by that
- `& 63` masks to 6 bits — a fast sawtooth carrier
- `& t>>4` gates the whole thing with a mid-speed ramp

Every term is a power-of-two-scaled copy of the same counter. The result is
self-similar: the pattern at bar level looks like the pattern at note level
looks like the pattern at waveform level. It is a Sierpinski triangle you can
hear. Nobody composed it; it falls out of binary counting.

### Why this alone isn't death industrial

Everything above is **memoryless**: output is a pure function `f(t)`. Pure
functions of a counter are bright, fast and brittle. They buzz. They cannot
sustain, decay, or resonate, because sustain *is* memory.

So the language has state (§4), and there is a post chain (§6). The single
most important control is **TONE**: bytebeat's structure lives in its
high-frequency edges, and rolling those off leaves the periodicity audible as
*weight and pitch* rather than as grit. Measured on white noise, taking TONE
from 255 to 1 moves the spectral centroid from 11.0 kHz to 3.8 kHz and drops
energy above 5 kHz from 77% to 25%.

---

## 4. The expression language

### Identifiers

| | |
|---|---|
| `t` | free-running sample counter, wraps at 2^32 |
| `sr` | current sample rate |
| `k` | **loop position**, 0 … `ll`-1, wraps |
| `n` | bar counter, monotonic |
| `bt` | position within the current beat |
| `bl` | beat length in samples |
| `ll` | loop length in samples |
| `tr` | 1 for exactly the first sample of a sequencer hit |
| `age` | samples since the latest hit |
| `vel` | hit velocity: 148 normal, 256 accent |
| `p0`..`p7` | knobs, 0..255 |
| `r` | xorshift PRNG |
| `s0`..`s3` | registers that persist across samples |

`k` is the loop. Write a phrase against `k` and it repeats **exactly**, every
loop, forever. Write the same phrase against `t` and it never repeats. Put
both in one expression and you have a fixed figure with something drifting
over it — which is the `loop + drift` example, and the thing you asked for:

```
lp(((k>>p0 & k>>p1) * p2 << 6) ^ (r >> p3), p4)
    \_______________________/     \______/
      repeats every loop           never repeats
```

Loop length comes from the `bpm`, `beats` and `bars` controls:
`ll = (sr*60/bpm) * beats * bars`. `bb_engine_reset_loop()` restarts it; the
terminal instrument put that on `K` and nothing in the GUI calls it yet.

### `r` advances on every read

`r` is xorshift32 — three shifts, three XORs, one word of state, no multiply:

```c
x ^= x << 13;  x ^= x >> 17;  x ^= x << 5;
```

It advances *every time the opcode executes*, so `r^r` is noise, not zero.
That's deliberate for a noise instrument. If you want one noise value used
twice in a sample, latch it: `s0 = r` then use `s0`.

### State

| | |
|---|---|
| `d` | delay line, one sample ago (= `d(1)`) |
| `d(n)` | delay line, n samples ago (clamped 1 … 131071, ~3.0s at 44.1kHz) |
| `w(x)` | write x into the delay line, returns x |
| `lp(x,c)` | one-pole lowpass, `c` 1..255, low = dark |
| `hp(x,c)` | one-pole highpass |
| `bp(x,f,q)` | resonant bandpass; `f` sets pitch and high `q` rings longer |
| `s0 = x` | assign a register, returns x |

The delay write cursor advances **once per sample**, not once per `w()`. So
several `w()` calls in one expression all target the same slot (last wins),
and `d(1)` reliably means "the previous sample" however you wrote it.

`d` and `w` together are feedback, and feedback is what makes a drone sound
like it is happening in a space:

```
w( lp( (t*p0 ^ t>>p1) + (d(p2*64+1) >> 1), p3 ) )
```

reads a third of a second ago at half level, mixes it with new material,
filters, and writes it back. That is the `hangar` patch.

Each textual `lp()`/`hp()`/`bp()` call site gets its own filter memory, so two
`lp()`s in one expression are two independent filters. State lives in the
audio thread's `ExprCtx`, **not** in the Program — so when you edit the
expression, the delay line and filters keep their contents and the sound
morphs instead of restarting.

`tr`, `age` and `vel` turn the expression VM into a triggered synthesizer
without introducing samples or a second hidden engine. A kick is an impulse
into a state-variable resonator:

```
bp(tr*vel*4096,p0,p1)
```

Ratchets produce another one-sample `tr` and reset `age`; probability is
decided once for the containing step. Continuous bytebeat sources can ignore
all three identifiers and behave exactly as they did before.

### Integer semantics, and why each choice was made

- **All arithmetic is int32, done internally in `uint32_t`.** Signed overflow
  is undefined behaviour in C, and bytebeat overflows on nearly every sample —
  that overflow *is* the instrument. Unsigned wrapping is defined, so the VM
  computes there and reinterprets. (`-fwrapv` is belt and braces where it
  exists: CMake passes it on GCC, Clang and Apple Clang only. MSVC has no
  equivalent, which is exactly why nothing may depend on one.)
- **Divide/modulo by zero yields 0.** A live expression *will* divide by zero
  the moment a knob crosses a value. A `SIGFPE` mid-set kills the instrument.
  `INT32_MIN / -1` also traps on x86 and is special-cased for the same reason.
- **Shift counts are masked to 0..31.** Shifting an int32 by ≥32 is undefined
  and knobs go to 255. Masking is what JavaScript does — which matters,
  because every classic bytebeat formula on the internet was written against
  JS semantics. Paste one in and it sounds the way its author heard it.
  Right shift stays signed (arithmetic), also matching JS.

Verified: all 14 shipped examples plus a battery of hostile inputs
(`t<<255`, `-2147483648/-1`, `d(999999999)`, …) run clean under UBSan.

---

## 5. Recursive descent, and why precedence is free

A parser's job: turn `1 | 2 ^ 3 & 4` into a tree that respects C's rules.

The usual first instinct is a precedence table and a loop. You don't need
one. **Precedence is the shape of the grammar.**

Write one function per precedence level, loosest binding at the top, and have
each one call the next-tighter level for its operands:

```
assign  := S '=' assign | ternary
ternary := lor ('?' assign ':' assign)?
lor     := land ('||' land)*
land    := bor  ('&&' bor )*
bor     := bxor ('|'  bxor)*      <- loosest bitwise
bxor    := band ('^'  band)*
band    := equal('&'  equal)*
equal   := rel  (('=='|'!=') rel)*
rel     := shift(('<'|'>'|'<='|'>=') shift)*
shift   := add  (('<<'|'>>') add)*
add     := mul  (('+'|'-') mul)*
mul     := unary(('*'|'/'|'%') unary)*
unary   := ('-'|'~'|'!'|'+') unary | primary
primary := NUMBER | ident | call | '(' assign ')'
```

Every one of those is four lines in `expr.c`:

```c
static void p_bor(P *p)
{
    p_bxor(p);                                  /* left operand */
    while (accept(p, TK_OR)) {                  /* our operator? */
        p_bxor(p);                              /* right operand */
        emit(p, OP_OR, 0, -1);
    }
}
```

**Why precedence falls out.** `p_bor` can *only* obtain operands by calling
`p_bxor`. So when parsing `1 | 2 ^ 3`, the call to `p_bxor` for the right
operand consumes `2 ^ 3` **entirely** before returning — a `^` physically
cannot escape upward past a `|`. The tighter operator is deeper in the call
stack, and depth *is* precedence. There is no table to get wrong.

**Why left-associativity falls out.** The `while` loop emits as it goes, so
`100-10-1` emits `100 10 -` then `1 -` = 89. Right-associativity (`=`, `?:`)
is written as recursion instead of iteration — the function calls *itself*
for the right operand.

**Why parentheses need no special case.** `primary` calls all the way back to
the top on `(`. Recursion handles nesting for free.

### Flat bytecode, not an AST

Walking a tree per sample means chasing pointers all over the heap 44,100
times a second. Instead the parser emits **postfix** directly — because in
`p_bor` above, `emit` happens *after* both operands are parsed, which is the
definition of postfix.

```
t*(t>>12|t>>8)&63

  OP_T                    stack: [t]
  OP_T                           [t, t]
  OP_CONST 12                    [t, t, 12]
  OP_SHR                         [t, t>>12]
  OP_T                           [t, t>>12, t]
  OP_CONST 8                     [t, t>>12, t, 8]
  OP_SHR                         [t, t>>12, t>>8]
  OP_OR                          [t, (t>>12|t>>8)]
  OP_MUL                         [t*(...)]
  OP_CONST 63                    [t*(...), 63]
  OP_AND                         [result]
```

One flat array, walked front to back, dispatched through a `switch`. No
pointer chasing, no recursion, cache-friendly.

The emitter tracks the stack effect of every instruction as it goes, so the
maximum depth is known at **compile** time. That's why `expr_eval` has no
bounds checks in its inner loop — `expr_compile` already proved they can't be
needed.

`&&`, `||` and `?:` need real control flow (they short-circuit), so there are
three jump opcodes. The only fiddly part is rewinding the modelled stack depth
before emitting the second arm of a branch, since a linear walk would
otherwise double-count arms that are alternatives, not sequence.

---

## 6. The lock-free program swap

The problem: the audio thread is dereferencing a `Program` thousands of times
per period, and we want to replace it, from another thread, without ever
making the audio thread wait.

A mutex is correct and unacceptable — see §1.

What we do instead:

```
UI THREAD                              AUDIO THREAD
---------                              ------------
malloc a fresh Program                 (every period:)
compile into it                          epoch++
   |                                      prog = load(bb.prog)
   | (audio thread has never                |
   |  seen this pointer, so                 | render 441 samples
   |  writing it is race-free)              |
   v
atomic_exchange(&bb.prog, new) --------> (next period picks it up)
   |
   v
push old onto retire list
(NOT free — audio may be mid-evaluation)
```

One atomic exchange. The audio thread never waits for anything. A failed
compile publishes nothing at all, which is why a syntax error can't interrupt
the sound — verified in the terminal editor: typing ` & ((((` and leaving the
field left the audio playing.

### Why the old program can't be freed immediately

The audio thread might be three instructions into evaluating it. Freeing it
is a use-after-free that will manifest as a crash or a burst of noise, at
random, hours later. So:

**When is a retired program provably dead?** The audio thread's loop is:

```
epoch++            (sequentially consistent)
prog = load(prog)  (sequentially consistent)
...render...
```

Say we publish, then read `epoch == E`.

- The period that incremented epoch to `E` may have loaded `prog` *before*
  our store. It might still be using the old program.
- The period that increments epoch to `E+1` comes after our epoch read in the
  single total order sequential consistency provides, and its load of `prog`
  comes after that increment. It is **guaranteed** to see the new pointer.
- Periods run one after another on one thread. So when we observe
  `epoch >= E+2`, period `E+1` has started, therefore period `E` has
  **finished**. Nobody can be looking at the old program.

Hence `bb_reclaim()` frees anything with `now >= retire_epoch + 2`. In
wall-clock terms that's about two periods, ~20ms.

This is why those two operations are `seq_cst` and not merely
acquire/release: the proof needs both threads to agree on one global order of
"epoch bumped" vs "prog read". On x86 it costs nothing extra — the
`fetch_add` is a locked instruction either way, and it runs once per ~10ms.

**Verified:** 1440 program publishes (one per keystroke) plus slot/example
churn produced **+128 kB** of RSS growth. A broken retire list would have
leaked ~9 MB. ThreadSanitizer reported **zero** races over the same run.

### Retuning the device: the park handshake

The park handshake lived in the ALSA thread and went out with it. `park_req`
and `parked` are not in the tree any more, and the JUCE app takes the device's
rate once at startup rather than renegotiating it live. The handshake is
written down here because the transferable part is the argument -- what makes
it legitimate for a real-time thread to sleep at all -- and because anything
that ever wants a live rate change will have to make that argument again.

Changing sample rate means calling `snd_pcm_hw_params` again — and ALSA's
parameter negotiation **allocates**. We refuse to do that on the audio
thread. But the UI thread can't touch the PCM handle while the audio thread
is using it either.

So the audio thread is asked to stand still:

```
UI                                  AUDIO
--                                  -----
park_req = 1
wait for parked  <---------------   (top of period, not inside writei)
                                    parked = 1
snd_pcm_drop                        spin: sleep 200us while park_req
snd_pcm_hw_free                       "
reconfigure (allocates - fine,        "
 we're on the UI thread)              "
park_req = 0     ---------------->  parked = 0, resume
```

The audio thread does sleep here, which technically violates "never block".
It is safe because **there is no stream running, so there is no deadline to
miss**. That is the honest justification, and it is why the rule is about
deadlines rather than about sleeping.

The UI's wait is bounded (2s) so a dead audio thread can't hang the
interface, and a failed reconfiguration attempts to restore the previous
rate before giving up.

**Verified:** 200 fine rate steps, 48 octave jumps, and driving to both the
1000 Hz floor and 96000 Hz ceiling — no crash, audio still flowing, 0 xruns.

### Recording uses the same trick

The audio thread cannot write a file or a socket. So it writes int16s into a
preallocated ring and bumps an index; the UI thread drains it on a timer. The
ring holds ~11s at 96kHz and ~22s at 48kHz, against a 30 Hz message-thread
poll — the margin is the point. If the UI is catastrophically late the ring
laps, `WavRecorder::service()` skips to the oldest still-valid sample and
counts the lap rather than splicing garbage, and **the instrument never
stutters**.

Every consumer of that ring keeps its **own read cursor**, privately; no read
cursor is stored in `bb` at all. That is what stopped a stalled TCP listener
on bad wifi from corrupting or delaying the `.wav` you were recording, and it
is still what keeps `WavRecorder` and the master meters in `Chrome.cpp` and
`MixerPanel.cpp` out of each other's way now that the TCP sink is gone.

---

## 7. Listening from another machine

Everything in this section was `sink.c`, driven by the terminal front end's
`-d`, `-s`, `-L` and `-O` flags. `sink.c` is deleted, so there is no listener,
no HTTP page, no `/stream.wav` and no stdout PCM today; the JUCE app plays to a
local audio device and writes `.wav` files. Read the instructions below as
description rather than as something you can run. The probe design is kept
because one port serving both browsers and `nc` -- by waiting ~300ms to see
whether the client speaks HTTP -- is the part worth having if remote listening
is ever built again.

The instrument runs on the Linux box; the sound comes out of your laptop.

**`-d none` means no sound card, deliberately.** If you start with it and
nothing connects to the stream, you will hear nothing, and that is working as
designed. The `listen` line in the UI shows the URL and says whether anything
is actually connected.

### Easiest: open the URL in a browser

```sh
# on the linux box
./bytebeat -d none -s 9000
```

The UI prints something like `listen  http://100.68.127.104:9000`. Open that
on your laptop. Nothing to install.

Three routes on the one port:

| request | response |
|---------|----------|
| `GET /` | an HTML page with an `<audio>` player and copy-paste commands |
| `GET /stream.wav` | `audio/wav`, 44-byte header, then endless PCM |
| *(sends nothing)* | bare headerless PCM, for `nc` and friends |

`/` has to return a **page**, not the audio. A bare `audio/wav` with no
`Content-Length` makes browsers *download* an endlessly growing file instead
of playing it. An `<audio>` element pointed at `/stream.wav` plays in place
and gives you a transport control.

Because `/stream.wav` carries a real WAV header, every normal player works
from the URL alone — no format flags to get right:

```sh
ffplay -nodisp -autoexit http://100.68.127.104:9000/stream.wav
mpv --no-video        http://100.68.127.104:9000/stream.wav
vlc                   http://100.68.127.104:9000/stream.wav
```

**Those run on your laptop, not on the box.** The laptop is the thing with
speakers; it pulls bytes over the network and plays them locally. The box
only serves.

The address is taken from `SSH_CONNECTION` — specifically the address on this
box that your ssh client connected to — so it is provably reachable from
wherever you are sitting, because you are already using it. Tailscale, LAN,
VPN, whatever. Falls back to the first non-loopback interface.

How it works: a freshly accepted client is *probed*. Browsers send
`GET / HTTP/1.1` immediately, so we answer with HTTP headers plus a 44-byte
WAV header (length fields `0xffffffff`, the convention for "this never
ends"). Raw listeners like `nc` send nothing at all, so after ~300ms of
silence we assume raw and stream bare PCM. One port, both kinds of client, no
flag to remember.

### Also works: raw PCM over TCP

```sh
# on the mac / windows machine
ffplay -nodisp -f s16le -ar 44100 -ac 1 -i tcp://thebox:9000

# or with sox
nc thebox 9000 | play -t raw -r 44100 -e signed -b 16 -c 1 -

# or vlc
vlc --demux=rawaud --rawaud-channels 1 --rawaud-samplerate 44100 tcp://thebox:9000
```

Through your existing ssh session, no extra ports exposed:

```sh
ssh -L 9000:localhost:9000 thebox
# then, in another local terminal:
nc localhost 9000 | ffplay -nodisp -f s16le -ar 44100 -ac 1 -
```

Use `-L` on bytebeat to bind the listener to 127.0.0.1 only, which is what
you want with the ssh tunnel.

Or as a single command:

```sh
ssh -t thebox '/home/you/bytebeat/bytebeat -d none -O' | ffplay -f s16le -ar 44100 -ac 1 -
```

`-O` sends raw PCM to stdout and moves the TUI to `/dev/tty` so the two don't
collide.

**Set `-ar` to match.** The stream carries no header, so if you retune with
`[` `]` the player keeps its original rate and the pitch shifts. That is
sometimes useful and usually confusing.

The socket is non-blocking and drops rather than backing up, so a laptop
that goes to sleep can't affect the instrument. Reconnecting just works —
newest connection wins.

### The other route: PulseAudio / PipeWire over ssh

If you already run a sound server on the laptop, you can send ALSA's output
there instead. Needs `libasound2-plugins` on the box and a running server on
the laptop.

```sh
ssh -R 24713:localhost:4713 thebox
# on the box:
export PULSE_SERVER=tcp:localhost:24713
./bytebeat -d pulse
```

Higher latency and more moving parts than the TCP route, but it mixes with
your other audio.

---

## 8. Porting to a microcontroller

Say a Cortex-M4 at 100 MHz with an I2S DAC — a Teensy, an STM32, a Daisy.
This design is deliberately close already: fixed-size buffers, integer-only,
no allocation in the audio path.

**What comes across unchanged**

`expr.c` almost entirely. The VM is a switch over int32 ops with a
fixed-size stack — that's exactly what you want on an MCU. The post chain in
`dsp.c` too.

**What has to change**

1. **ALSA → a DMA double-buffer.** No PCM handle, no `writei`. You give the
   I2S peripheral two buffers and it raises an interrupt at each half:

   ```c
   void DMA1_Stream_IRQHandler(void) {
       int16_t *half = half_transfer ? buf : buf + N/2;
       for (int i = 0; i < N/2; i++) half[i] = render_one_sample();
   }
   ```

   Same period/latency tradeoff from §1, same xrun (here: "I didn't finish
   before the DMA wrapped"), just no library in between. Conceptually this is
   *simpler* than ALSA — you are talking to the DAC directly.

2. **No threads, no `stdatomic`.** One interrupt and one main loop. The swap
   still works but the mechanism changes: an interrupt cannot be preempted by
   `main()`, so publishing becomes a single aligned 32-bit pointer store
   (atomic on ARM by construction) with a `__DMB()` before it. Reclamation is
   easier — set a flag in the ISR, and `main()` frees when it sees the flag
   change twice. The epoch argument in §6 carries over verbatim.

3. **Static allocation everywhere.** No `malloc` at all. A fixed pool of two
   or three `Program` structs, ping-ponged. `EXPR_CODE_MAX 768` at 8 bytes is
   6 KB per program — comfortable in 128 KB of SRAM, not in 20 KB.

4. **The delay line is the real constraint.** `EXPR_DELAY_LEN` is 2^17 int32
   = **512 KB**, per layer. (The 2^18 buffer is `BB_SPACE_LEN`, the post-chain
   SPACE line; the two are separate and are easy to confuse.) No MCU has either
   in SRAM. Options: drop to 2^13 (32 KB, 0.19s at 44.1 kHz), store int16
   instead of int32 (halves it), or put it in external SDRAM/PSRAM if you have
   it (Daisy has 64 MB). Same for the 2 MB sink ring —
   on an MCU you'd stream to an SD card in small blocks instead.

5. **`mlockall` and `SCHED_FIFO` disappear.** There's no virtual memory and no
   scheduler. You get hard real-time for free; that's the compensation for
   everything else being harder.

6. **The `int64` multiplies in `onepole` cost real cycles.** M4 has a
   single-cycle 32×32 multiply but 64-bit needs several. Either use the DSP
   intrinsic (`SMULL`/`SMLAL`, which is what the compiler should emit anyway),
   or reduce the Q8 state to Q4 and accept coarser filters.

7. **The UI goes away**, or becomes a small OLED plus real potentiometers on
   the ADC — which honestly is the version worth building. `p0..p7` were
   designed as knobs the whole time; wiring them to actual knobs is the point.

**Rough budget.** At 48 kHz you have ~2080 cycles per sample at 100 MHz. A
30-instruction expression through a switch VM is maybe 300–600 cycles, and
the post chain another 100. Comfortable. Double the sample rate or write a
200-instruction expression and it stops being comfortable — at which point
you'd thread the bytecode (computed goto / function-pointer table) rather
than switch-dispatch.

---

## 8a. Making sounds without writing expressions

Most of the mechanisms described here are live. The ladders, the rack, the
generator and the sequencer are `knob.c`, `rack.c`, `gen.c` and the sequencer,
looper and knob-role code in `engine.c`, and the JUCE panels drive every one of
them. One is not: the per-control `axis` tagging described under *Directional
moves* existed only in `ui.c` and went out with it, there is no axis mechanism
anywhere in the tree today, and RACK's SCULPT is a different thing: it nudges
the five VOICE DESIGN macros. The axis argument is kept because it is what
SCULPT would need in order to move a hand-written expression the way the
terminal version could.

What is retired throughout is the front end. The single-key bindings named
below -- `p`, `v`, `M`, `Z`, `[`/`]` -- were the terminal instrument's, and the
`VOICE`/`POST`/`SHAPE` columns they refer to are the terminal panel drawn in
§9, not anything on screen today. The layer digits under *Layers* are the
exception: those two bindings are live in the JUCE app. Read the rest as a
record of which gestures turned out to be worth having. The design arguments
underneath them are why this section exists and none of those have changed.

The expression language is the truth of this instrument, but it is a bad
control surface. It offers no way to ask for "the same idea, but darker", and
eight knobs called `p0..p7` tell you nothing about what they will do. Four
mechanisms sit on top of it, none of which require touching the editor.

### Layers

Eight fully independent voices, summed. Each has its own expression, its own
`p0..p7`, its own output mode, its own drive/tone/crush/space chain, its own
sequencer pattern, and its own level. The only things shared are the
transport (bpm/beats/bars), the master gain, and the output device.

That independence is what lets you put a dark sub drone under a bright gated
hit under a filtered noise bed -- which is the entire architecture of the
genre. The cost is eight copies of the expression and SPACE state, plus the
sink and master-phrase rings: about 18MB of static memory. That figure covers
the per-layer state and those two rings only -- the loop bank's five satellite
buffers, `BB_LOOP_LEN` int16 apiece, are another 10MB of BSS beside it. None of
it is allocated on the audio thread. The expression VM is cheap; it was never
going to be the bottleneck.

`1`-`8` focuses a layer -- panel, editor and step grid all follow it.
`shift`+number toggles a layer on or off without moving focus, so you can drop
the beat out while still tweaking the drone. Muting is a ramp, not a switch --
`g_lvl[]` moves 32 Q16 steps per sample, so about 46ms at 44.1kHz -- and once a
layer has reached silence the render loop skips its DSP entirely.

### Ladders, or: why a knob used to feel like nothing was happening

A knob is an integer 0..255 handed straight to the VM. Almost nothing in this
instrument is linear in that number:

- `t>>p` is masked to `0..31` by the VM, so **256 knob positions address 32
  distinct sounds, in eight identical repeats.** Of those 32 only about a
  dozen are musical: below 2 the modulator is above the sample rate, above 20
  it is slower than a bar.
- `t&p` produces a clean partial only when `p` is `2^n-1`. That is **eight
  useful values out of 256**; everything else is a lopsided mask.
- `lp(x,p)` is logarithmic. `p` 1..10 covers three octaves of cutoff and
  200..255 covers almost nothing.
- The gate envelope is worse than either: `decay_k = 1 + p*p/108` and the fall
  time goes as `1/k`, so `p`=4 and `p`=10 are both a ten-second decay while
  `p`=200 and `p`=210 are 25ms and 23ms.

So the old UI was a linear ramp across a space where the sound is a step
function, stepping by 1 or 16 through mostly dead positions and occasionally
falling off a cliff. That is what "I don't feel in control" actually is; it
was never a matter of not understanding the maths.

`knob.c` fixes it with a **ladder** per kind: the ordered list of values that
do something. The UI steps along the ladder instead of along the integers,
draws the bar from the ladder position rather than from `v/256`, and prints
the physical quantity the value corresponds to. `p0 137` becomes
`SWEEP >>9 86Hz`.

| kind | rungs | unit shown |
|---|---|---|
| `SHIFT`  | 19 | step rate of the modulator, 11kHz down to 23s |
| `MUL`    | 57 | `sr*p/256` in Hz, folded if it aliases |
| `MASK`   | 8  | how many bits survive |
| `CUT`    | 36 | one-pole -3dB point, `open` past Nyquist |
| `PERIOD` | 35 | `sr/p` in Hz |
| `NOISE`  | 13 | bits of noise reaching the output |
| `DECAY`  | 21 | 60dB fall time, 5.1s down to 17ms |

None of this is in the audio path. `param[i]` is still a raw 0..255 int and
the VM never learns any of it; the ladder only decides which of those 256
values the UI is willing to stop on. Two rules keep the numbers honest: every
conversion is copied from the code that consumes the value, and anything
without an honest unit shows none rather than a plausible invention.

Oscillator frequencies past Nyquist are printed **folded**, marked with `~`.
Aliasing is half of why this instrument sounds the way it does, so the answer
is not to forbid it but to say what you will actually hear -- `*233` at 44.1k
reads `~4.1k`, not `40kHz`.

### The rack

`gen.c` always contained the vocabulary for a good UI: a table of expression
skeletons known to work, each slot tagged with what kind of value belongs in
it. It was only reachable by rolling dice. `rack.c` is that table made
navigable.

A voice is a **source** -- twenty-two of them, named for what they sound like --
optionally wrapped in **BODY** (a lowpass) and **SPACE** (a feedback delay).
The slots the source exposes are named for what they do: `SWEEP`, `GRAIN`,
`PITCH`, `WIDTH`, `RATIO`. `rack_build()` renders the lot to expression text,
which the panel shows live on line 3: turn `GRAIN`, watch `p1` change in the
expression, hear the difference. The rack is not a wrapper that hides the
language, it is a labelled view of it.

The original eleven are continuous bytebeat sources. Six appended engines
are hit-oriented: `thump`, `burst`, `metal`, `dust`, `rumble`, and `feedback`.
Choosing one enables a sequencer and seeds a four-pulse rhythm when the layer
was empty. They still compile to visible expressions; "triggered engine" is
an input convention (`tr`/`age`/`vel` plus the gate envelope), not a closed
synth hidden alongside the VM. Five more make the cold wing -- `cold`,
`vapor`, `hymn`, `siren` and `glass` -- tuned sources that ride the semitone
voice clock, so the sequencer's pitch lane plays them as melodies. `glass` is
struck as well as tuned: it reads `age`, so it carries the triggered flag too,
which is why the suite pins seven triggered engines rather than six.
Twenty-two sources in all.

Only the choices that change the *shape* of the expression live in the `Rack`
struct -- four bytes: source, body, space, mode. Every continuous value stays
in `param[]` exactly as before, so turning a slot is a plain atomic store the
audio thread picks up on the next sample: no recompile, no glitch. Only
changing source or stages recompiles.

`SPACE`'s delay time cannot be a bare knob, because `d(p)` maxes out at 255
samples, which is 6ms. It is rendered as `d(p*96+400)`, putting the range at
9ms..565ms and keeping the tap live while you turn it.

Typing into the editor **detaches** the layer from its rack: the text no
longer corresponds to any source, and pretending otherwise would put wrong
labels on the panel. The layer is marked `custom`, the VOICE column falls back
to raw `p0..p7` with roles inferred from the bytecode, and the `SOURCE` row
becomes the way back. One direction only -- parsing arbitrary expressions
back into racks would be a research project, and guessing wrong would
silently destroy someone's patch.

### Directional moves

The generator can only teleport: press `p` and you are somewhere else, with no
path back to where you were. The bracket keys are the opposite -- one small,
audible, reversible step along a named axis:

```
[ ]   darker / brighter     every cutoff in the voice, plus TONE
{ }   cleaner / dirtier     masks, plus DRIVE and CRUSH
; '   lower / higher        every pitch in the voice
: "   slower / faster       every modulator rate, plus gate DECAY
```

Each control carries an *axis* separately from its ladder kind, because
`DRIVE` belongs on the grit axis without inheriting the mask ladder, and the
`SPACE` delay time is measured in milliseconds without being something
"faster" should shorten. Working over axes rather than named knobs means this
works on hand-written expressions too: an expression whose `p3` the compiler
classified as a cutoff gets moved by "darker" without the rack knowing
anything about it.

One sign flip matters. A **bigger** shift is a **slower** modulator, so that
one ladder runs backwards against its axis. Getting it the wrong way round
makes "faster" audibly do the opposite, which is worse than not having the
key.

### The generator

`p` rolls a new voice into the focused layer. `P` mutates it -- same source
and stages, new numbers. `x` re-rolls just the slot values. `C` steps back.

Random bytebeat is almost always *silence*: one stray `& 0` or `>> 31`
flattens the whole expression. A dice button that produces nothing four times
in five is worse than no button. Three things fix it:

- **Structured choice.** The candidate is a `Rack`, not a string, so the
  structure always works. This used to be a private table in `gen.c`; sharing
  it with the panel means what the dice can reach and what your hands can
  reach are, by construction, the same set.
- **Ladder windows.** Within a kind the roll is confined to the stretch worth
  landing on. The extremes stay reachable by hand -- a cutoff of 1 is
  inaudible, and a generator that hands you one is a generator you stop
  trusting.
- **Audition.** The candidate is compiled and actually run through the VM,
  offline, before you hear it. Measure the RMS after DC blocking; if it is
  silent or slammed into the rails, discard it and try the next seed.

The acceptance contract is explicit: a generated voice must audition between
6% and 85% RMS. Trigger-oriented rolls always arrive with a rhythm; some hits
also receive ratchets and reduced probability. `morgue-tests` (run it through
`ctest --preset <your-preset>`) compiles every source with every BODY/SPACE
combination, confirms all twenty-two source defaults are audible, and checks
deterministic generated seeds against that level contract.

The audition measures **three windows**, at `t` = 0, 2^18 and 2^21, and takes
the loudest. Measuring only from `t`=0 quietly biased the whole generator: a
modulator written `t>>12` has only reached 3 by the end of a 14000-sample
window, so any patch whose structure lives in the high bits of a slow shift
looked like silence and got thrown away -- it had not started yet. That bug
made one of the original sources statistically unreachable, and it is the sort
of thing you find by sweeping the parameter space rather than by listening.

That sweep is worth keeping as a habit: it also caught that `t*a & t>>b` is
not a ring modulator at all. Its amplitude is bounded by `t>>b`, so it is
inaudible for the first minute of a session and slowly gets louder. That is
not a sound, it is a fault. Masking both operands -- `(t*a&255)*(t*b&255)` --
makes the level independent of how long the instrument has been running, and
is an actual ring modulator, sum and difference tones and all.

Rolls are seeded, and the seed round-trips exactly: hand `gen_roll()` the same
32-bit number and you get the same voice back. That is a property of the engine
that no front end currently surfaces -- the seed the roll returns is discarded
at the call site, it is in no field of `bb` and in no session file, and the
status line shows bar, step, CPU and clip instead. Anything that wants "find
that sound again" has to start by keeping the number.

### The sequencer

`SEQ` in the SHAPE column turns the gate on for the focused layer; `v` opens
the pattern editor.

Sixteen steps of a 16th-note grid, so 16 steps is one bar in 4/4. The editor
has five lanes:

- **gate** -- off, hit, or accent;
- **pitch** -- -12..+12 semitones;
- **ratchet** -- one to four sample-accurate retriggers inside the step;
- **probability** -- 0..100%, decided once for the main step so a ratchet is
  either wholly present or wholly absent;
- **parameter lock** -- one of sixteen targets: `p0`..`p7`, level, drive,
  tone, crush, SPACE time/feedback/mix, or decay.

The gate multiplies the layer's output *before* its post chain, so delay and
space tails keep ringing after the gate shuts -- gating after the chain would
chop the reverb off with the note, which sounds like a fault rather than a
choice. `DECAY` sets the envelope: 0 holds the gate open until the next step
(a plain on/off gate), higher values make it percussive. **`DECAY` plus the
`burst` or `dust` source is how you get a noise hit on purpose** rather than
by accident, which is why it has a ladder in decay time rather than in knob
units. Accents also raise `vel`, so an expression can respond before the gate
envelope is applied.

**Pitch on a bytebeat is unusual** -- there is no oscillator to retune. But
every frequency in the output derives from `t`, so advancing `t` faster moves
everything up together. Each layer therefore has its own voice clock in Q32
fixed point, stepped by a semitone ratio each sample. At offset 0 the ratio
is exactly `1<<32`, so the clock is bit-identical to plain `t` and switching
the sequencer off changes nothing at all. This works on any expression ever
written, without editing it.

In the pattern editor, `Tab` moves between lanes, `[`/`]` chooses a lock
target, and `l` captures its current live value on the selected step. A lock
lane normally jumps. Pressing `m` marks it as motion and linearly interpolates
to the next locked point. `e` fills a Euclidean rhythm -- k pulses spread as
evenly as possible over n steps. Two numbers gets you a surprising fraction
of the world's traditional rhythms, which is a lot of mileage for one key.

### Motion recording

The lock lanes are also the automation format. In the normal panel, put the
cursor on any VOICE slot or POST sound control and press `M`. While recording,
the UI writes its live value into the transport's current step and marks that
lane for interpolation. Press `M` again and the sixteen-step gesture loops.
The audio thread only reads the resulting atomic grid; curses timing and knob
movement never enter the real-time path.

### Clocked and frozen SPACE

Each layer's post-chain feedback delay can keep its free millisecond time or
lock to `1/32`, `1/16T`, `1/16`, `1/8T`, `1/8`, `1/4T`, `1/4`, `1/2`, one
bar, or two bars. The larger 2^18-sample buffer holds the longest possible
division when it fits and clamps safely at extreme slow-tempo/high-rate
combinations.

`FREEZE` stops admitting the layer signal and writes the current delayed tap
back at unity. Read and write cursors continue moving, so the captured delay
phrase circulates rather than becoming one held sample. Releasing freeze
restores the ordinary bounded-feedback path.

### Master phrase looper and performance view

`Z` replaces the editing panel with an eight-track performance overview:
engine name, gate pattern, ratchets/probability, level, clocked SPACE and
freeze state all remain visible. Its master looper sits after the eight-layer
sum and before master gain, so it captures voices and their effect tails but
mute and master gain remain playable outside the recording.

`r` arms capture for the next exact bar boundary. It records one to four bars
into a fixed 2^20-frame buffer, then enters playback automatically with a
short crossfade. Controls cover dry/loop mix, overdub feedback, half/normal/
double speed, forward/reverse, and repeating `1/2`, `1/4`, `1/8`, or `1/16`
slices. `Space` starts or stops, `o` toggles overdub, and `x` clears.

The phrase audio is intentionally volatile: the session file -- version 7, the
number `engine.c` writes today -- stores all looper controls, while the actual
capture disappears when the process exits. WAV recording remains the durable
way to keep a performance.

### Knob roles

An expression only reads some of its knobs, and the ones it does read are
doing specific jobs. After every compile a small dataflow pass re-walks the
bytecode with a stack that carries *provenance* instead of values: when a
binary op consumes a slot that came from `pN`, that operator names the knob's
role. Feeding `>>` makes it an octave control; feeding `lp()` a filter
cutoff; feeding `%` a period.

This started as a label. It now does real work: on a `custom` layer the role
is what **chooses the ladder and the unit**, so a hand-written expression gets
the same detented, unit-labelled knobs as a racked one, with no help from the
author. Knobs the expression never mentions are dimmed.

Provenance deliberately survives `+`/`-` with a constant, so the very common
`t % (p0+1)` idiom still reports `p0` as a period control rather than an
anonymous offset. One role maps deliberately *away* from its obvious ladder:
`d(p)` is a tap in raw samples, and the `TIME` ladder's unit assumes the
rack's `p*96+400` scaling, so a hand-written delay knob shows a bare number.
Better that than a confidently wrong millisecond count.

---

## 9. The panel

This whole section is `ui.c`, and `ui.c` is deleted. There is no terminal
panel, no scope, no `?` pages and no keymap in the tree, and none of the keys
below do anything today; the JUCE app in `app/` is the only front end now. It
stays here in full because it is the only surviving statement of what the
layout was FOR: the uniform cursor, the colour-by-meaning rule, and the bottom
line that describes the control under the cursor. The session-file paragraph at
the end describes version 4, which was current when it was written -- `engine.c`
writes `version 7` now.

The centre of the screen is one flat list of controls in three columns, and
every control in it -- rack slot, post-chain knob, tempo, sample rate -- is
reached the same way: move the cursor, press left or right.

```
 VOICE ------------------  POST -------------------  SHAPE ------------------
SOURCE    pair 2/17        MODE      WORD            SEQ        on
STAGES    body             LEVEL      150 58%  ####  STEPS       16 16/16
SWEEP      >>9 86Hz  ####  DRIVE       70 3.1x ###   DECAY      140 56ms  ###
GRAIN     >>12 92ms  ####  TONE       130 4.9k ####  BPM         68 882ms
BODY        70 2.2k  ####  CRUSH       20 7.3k #     BEATS        4 3.5s
                           SP-TIME    120 380ms ###  BARS         2 7.0s
                           SP-FB      140 54%  ###   RATE     44100 44kHz
                           SP-MIX      80 31%  ##    ZOOM        32
                           SP-SYNC    1/4
                           FREEZE     off
```

**VOICE** is the expression. **POST** is the fixed chain applied after it,
per layer. **SHAPE** is the sequencer and the transport.

The uniformity is deliberate. What this replaced was eight anonymous knobs
bumped with letter keys, thirteen more TAB-ed through in a ribbon, sample rate
on bracket keys and gain on minus/equals -- with no way to tell which of them
was worth touching.

Bars are coloured by **what a control does, not where it lives**, so the two
pitch controls on opposite sides of the screen look alike:

```
magenta  pitch    cyan  rate     green  timbre
yellow   cutoff   blue  space    white  level
```

The directional keys follow those meanings exactly: `]` moves every yellow
control up, `;` moves every magenta one down. Blue is left alone, which is
why speeding a patch up does not also shorten its reverb.

The expression on line 3 has every `pN` coloured by the same scheme, which is
what stops the language being opaque: the panel and the text are visibly the
same thing.

Controls that cannot currently do anything are dimmed -- `STEPS`/`DECAY` with
the sequencer off, `SP-TIME`/`SP-FB` with the wet mix at zero, and any knob
the expression never reads.

**The bottom line always describes the control under the cursor.** That is the
real help system: an explanation of the thing you are touching, on screen,
before you think to ask for it. `?` opens five pages behind it -- keys, panel
rationale, the sequencer and looper, the source list with each skeleton, and
the language reference.

### Keys

**NORMAL**

```
MOVING AND ADJUSTING
  up / down      move the cursor through the panel
  left / right   adjust by one detent      < >   coarse, an eighth of a ladder
  , .            same as left/right, for terminals without them
  TAB / S-TAB    jump to the next / previous column
  q/a w/s e/d r/f t/g y/h u/j o/l   the VOICE knobs directly, no cursor
                                    (uppercase for a coarse step)

STEERING A SOUND YOU ALREADY HAVE
  [ ]            darker / brighter        { }   cleaner / dirtier
  ; '            lower / higher           : "   slower / faster

FINDING ONE FROM NOTHING
  p              roll a new voice (always audible -- it is auditioned)
  P              mutate: same source and stages, new numbers
  x              re-roll this source's slots, keeping the source
  C              undo, one step, per layer
  n / b          step through the built-in examples

LAYERS
  1-8            focus a layer
  ! @ # $ % ^ & *  toggle that layer on/off   (shift + number)
  L              solo the focused layer / all on
  D              clear the focused layer

EDITING AND OUTPUT
  i or Enter     edit the expression by hand (the SOURCE row takes you back)
  v  V           edit the step pattern / turn the sequencer on
  M              record the selected VOICE/POST control as a motion loop
  Z              performance view and master phrase looper
  m              output mode of this layer (BYTE/SIGNED/WORD)
  B              bypass all post chains
  0              reset t                 K     restart the loop
  - =            master gain             _ +   coarse
  SPACE mute     `  PANIC (silence, stream stays up)
  z              start/stop .wav recording
  ^S save now    ?  help                 X or ^C  quit (session autosaves)
```

**SEQ** (`v` to enter)

```
  left / right   move the step cursor    TAB / S-TAB  change lane
  up / down      adjust the selected gate/pitch/ratchet/probability/lock
  SPACE          toggle or cycle the selected cell
  1 2 3 4        set this step's ratchet count
  [ ]            choose parameter-lock target    l  capture current value
  m              hard steps / interpolated motion for this lock lane
  c              clear selected lane              x  clear the whole pattern
  e              Euclidean fill                   r  random pattern
  Esc or v       back to NORMAL
```

**PERFORM** (`Z` to enter)

```
  up / down      choose phrase control    left / right, < >  adjust
  r              arm capture at next bar boundary
  SPACE          play / stop              o  overdub
  f              freeze focused layer SPACE
  1-8            focus layer              shift+number  toggle layer
  x              clear phrase             z  record WAV
  Esc or Z       back to NORMAL            X  quit
```

**INSERT** (`i` to enter)

```
  Esc            back to NORMAL, keeping the edit
  Enter          compile and back to NORMAL
  ^A ^E          start / end of line     ^K kill to end    ^U clear
  arrows, Home, End, Backspace, Delete
```

The expression recompiles on **every keystroke**. Half-typed expressions fail
to parse constantly and that is fine -- a failure publishes nothing, so the
last good program keeps playing while you type.

The whole session -- eight layers with their expressions, racks, knobs,
chains, modes, five sequencer lanes, motion masks, SPACE modes and looper
controls -- autosaves to `~/.config/bytebeat/session.conf` on exit. Plain
text, version 4, edit it if you like. The captured phrase audio is not saved.
A version 2 file (before racks existed) still loads every layer as `custom`;
version 3 receives safe defaults for every appended v4 field.

## 10. Command line

Every flag below was parsed by `main.c`, which is deleted, along with the ALSA
and TCP code most of them configured. The JUCE app takes no arguments at all.
One thing survived: the regression suite still accepts `-T` for muscle memory,
but the binary is `morgue-tests` and the documented way to run it is
`ctest --preset <preset>` after `cmake --build --preset <preset>`.

```
-d DEV     ALSA device. "default" (via PulseAudio/PipeWire), "plughw:0,0"
           (direct, with format conversion), "hw:0,0" (raw), or "none".
-r RATE    initial sample rate, 1000..96000
-R         no ALSA resampling — snap to a rate the hardware really does
-s PORT    serve raw s16le mono over TCP
-L         bind that to 127.0.0.1 only
-O         raw s16le to stdout; TUI moves to /dev/tty
-e EXPR    start with this expression
-E EXPR    evaluate headlessly and print decimal samples (no audio, no UI)
-n N       how many samples -E prints
-p LIST    set p0..p7, e.g. -p 12,8,63
-T         run the headless regression suite and exit
```

`-E` was how you checked a patch without a sound card -- compile the
expression, evaluate it headlessly, print decimal samples:

```sh
bytebeat -E 't*(t>>p0&t>>p1)' -p 12,8 -n 20
```

Nothing replaces it. The nearest thing left is `morgue-tests`, which runs the
whole engine with no device present, but it runs its own fixtures rather than
an expression you hand it.

---

## 11. Files

```
expr.c    lexer, recursive-descent parser, bytecode, VM       — read alone
dsp.c     post chain: drive, tone, crush, space, DC blocker   — read alone
gen.c     procedural patch generator with offline audition    — read alone
knob.c    ladders and units: where a knob's detents are       — read alone
rack.c    the source table; renders a voice to expression text
ret.c     the return bus: four effects in eight slots, no atomics, no `bb`
engine.c  the instrument: render loop, voices, sequencer, sampler, loop bank,
          return bus, arrangement, program reclamation, session file
bb_platform.c  the five calls engine.c is allowed to make to an OS
bytebeat.h     every field both threads touch, and why each one is an atomic
examples.h     the starter bank. Nothing compiles it today; it is kept
          because it is the only place the per-example bpm/beats/bars live
tests/engine_tests.c   the regression suite, lifted out of main.c; links the
          engine and nothing else -- no JUCE, no ALSA, no sound card
app/      the JUCE front end, and the only front end. AudioEngine.cpp owns the
          device callback and the WAV recorder; the panels are in app/panels/
```

Four files that used to be on this list are gone: `audio.c` (the ALSA
thread), `sink.c` (WAV writing and HTTP/TCP/stdout streaming), `ui.c` (the
ncurses panel) and `main.c` (startup, reclamation, sessions and the `-T`
suite). Most of what they held had already moved before they were deleted:
the mixer, sequencer, SPACE and looper went from `audio.c` into `engine.c`,
reclamation and the session file went from `main.c` the same way, and the
suite is now `tests/engine_tests.c`. What actually went with them is the ALSA
plumbing, the socket plumbing, and the terminal panel itself.

`expr.c` has no dependency on anything else in the project. That is
deliberate — it should be readable on its own, and it is the file worth
reading first.
