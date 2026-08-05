# Bytebeat

Bytebeat is a sample-free terminal instrument: an eight-layer bytebeat synth,
triggered noise drum machine, expressive step sequencer, effects rack, and
master phrase looper in one ncurses interface.

It still live-compiles ordinary bytebeat expressions. The groovebox is built
from that same language rather than hiding another synth behind the panel:
`thump`, `burst`, `metal`, `dust`, `rumble`, and `feedback` are expressions you
can open, rewrite, and break into something new.

## Build and run

On Debian or Ubuntu:

```sh
sudo apt install build-essential libasound2-dev libncurses-dev
make
make test
./bytebeat
```

Use `./bytebeat -d none` when there is no local sound card. The engine still
runs in real time and can record, stream, or feed raw PCM to another process.

The first launch opens a five-part noise groove immediately: struck low body,
backbeat, ratcheted metal, probabilistic dust, and a continuous bytebeat floor.
The session autosaves as editable text at
`~/.config/bytebeat/session.conf`.

## Play it

The normal panel edits the focused layer. Arrow keys move and adjust, `Tab`
changes column, and `<`/`>` make coarse moves.

| Key | Action |
|---|---|
| `1`–`8` | focus a layer |
| shifted `1`–`8` | toggle that layer |
| `p` / `P` | generate a new voice / mutate this voice |
| `[` `]` | darker / brighter |
| `{` `}` | cleaner / dirtier |
| `;` `'` | lower / higher |
| `:` `"` | slower / faster |
| `v` / `V` | open the sequencer / toggle it |
| `M` | record the selected sound control as a motion loop |
| `Z` | open the performance view and master phrase looper |
| `i` or `Enter` | edit the expression directly |
| `z` | start or finish a WAV recording |
| `?` | complete in-app help |

### Sequence a voice

Press `v`. The editor has five lanes:

- gate: off, hit, or accent;
- pitch: -12 to +12 semitones;
- ratchet: one to four sample-accurate retriggers per step;
- probability: one decision per main step;
- parameter lock: any `p0`–`p7`, level, drive, tone, crush, SPACE, or decay
  value stored on a step.

Move with left/right and change lane with `Tab`. Use up/down to change a cell,
`Space` to toggle it, `1`–`4` for a ratchet, `[`/`]` to choose a lock target,
and `l` to capture its live value. `m` changes the selected lock lane between
hard steps and interpolated motion. `e` fills a Euclidean rhythm, `r`
randomizes, `c` clears the current lane, and `x` clears the full pattern.

In the normal panel, select a VOICE or POST sound control and press `M`; turn
it while the transport runs, then press `M` again. The movement repeats over
the pattern as an interpolated parameter-lock lane.

### Loop and perform

Press `Z` for the eight-track performance view. It keeps patterns, levels,
clocked effects, and the master phrase controls visible at once.

| Key | Action |
|---|---|
| `r` | arm a 1–4 bar capture at the next bar boundary |
| `Space` | play or stop the captured phrase |
| `o` | toggle overdub |
| `f` | freeze the focused layer's SPACE delay |
| arrows / `<` `>` | choose and adjust phrase controls |
| `x` | clear the captured phrase |
| `Z` or `Esc` | return to the normal panel |

The phrase looper captures the finished pre-master bus, including every layer
and its effect tails. It supports dry/loop crossfade, overdub feedback,
half/normal/double speed, reverse, and `1/2` through `1/16` stutter slices.
Capture is held in RAM; its controls save with the session, while its audio is
deliberately ephemeral.

Each layer also has `SP-SYNC`, from `1/32` through two bars, and `FREEZE`,
which stops admitting new sound and recirculates the current delay phrase at
unity.

## The expression instrument

The usual bytebeat vocabulary remains intact: `t`, `k`, `n`, `bt`, `bl`,
`ll`, `sr`, noise `r`, knobs `p0`–`p7`, registers `s0`–`s3`, delay `d()` and
`w()`, plus `lp()` and `hp()` filters.

Triggered synthesis adds:

```text
tr          1 for exactly the first sample of each hit
age         samples since the latest hit
vel         148 for a normal hit, 256 for an accent
bp(x,f,q)   resonant band-pass body; f is pitch, q is ring time
```

For example, the shipped low drum is only this:

```text
bp(tr*vel*4096,p0,p1)
```

The gate envelope is before each layer's post chain, so echoes and frozen
SPACE tails continue after a hit closes. The master path is:

```text
expression -> gate -> drive -> tone -> crush -> SPACE -> layer level
           -> eight-layer sum -> phrase looper -> master gain -> output
```

## Headless and remote use

```sh
./bytebeat -E 'bp(tr*vel*4096,p0,p1)' -p 8,244 -n 200
./bytebeat -d none -s 9000
ffplay -nodisp -f s16le -ar 44100 -ac 1 -i tcp://HOST:9000
./bytebeat -O | ffplay -nodisp -f s16le -ar 44100 -ac 1 -
```

`-L` binds the stream to localhost, `-r` chooses a sample rate, and `-R`
disables ALSA software resampling for real low-rate aliasing. Run
`./bytebeat --help` for every command-line option.

## Verification and design notes

`make test` runs more than 2,800 headless checks: expression triggers and resonance,
all 17 rack engines in every stage combination, generator determinism and
audibility, Euclidean rhythms, clocked/frozen delay behavior, phrase capture,
bar alignment, playback rates, slices, reverse and overdub, and v3/v4 session
compatibility.

[NOTES.md](NOTES.md) is the implementation tour and design rationale.
[EXAMPLES.txt](EXAMPLES.txt) is the expression cookbook.
