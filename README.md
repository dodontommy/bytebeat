# MORGUE

An instrument for dark noise, ambience and terror: an eight-voice bytebeat
synthesizer with a drum machine, granular wells, a master phrase looper, a
cavernous reverb bus, and a song timeline — behind a cold, clinical,
autopsy-archive interface. Everything you hear is a live-compiled integer
expression; the panel just puts hands on it.

![RACK — the voice station](docs/morgue_rack.png)

Two front ends drive **one realtime C engine**: this JUCE desktop app
(macOS) and the original [ncurses terminal instrument](docs/TUI.md) (Linux).
The engine renders the same samples for both, and a **2,966-check headless
regression suite** exercises the whole thing — DSP, sequencer, looper,
timeline, sessions — on any machine with a C compiler, no sound card
required.

## The console

| Workspace | What it does |
|---|---|
| **RACK** | The voice station. 16 curated patches, 22 expression generators, a live bytebeat editor, knobs with roles inferred from the compiled bytecode, per-voice post chain, 16-step sequencer with pitch/ratchet/probability/parameter-lock lanes. |
| **ARRANGE** | The song timeline. Record any voice or the drum bus into bar-aligned clips, drag/trim/loop them across 64 bars, place WAVs from the locker, seek by clicking the ruler. |
| **GRAIN LICKS** | The drum machine: 8 sample slots × 16 steps with per-step pitch and velocity, choke groups, mute/solo. A synthetic kit is preloaded so it grooves before you touch anything. |
| **GRAIN MASS** | Four sample wells: load anything, pitch it ±24 semitones, reverse it, loop it. PLAY ALL starts every well together on the next bar. |
| **SURVIVOR** | The master phrase looper, live: capture the finished bus at a bar boundary, then overdub, feed back, halve, reverse and stutter it. |
| **MIXER** | Faders, mutes and meters for every voice, plus **RETURN A — the CHAMBER**: a master reverb bus with per-voice sends. |
| **HW/SYNC** | MIDI in: notes trigger and re-pitch the focused voice, CC1 rides p0. |
| **EXPORT** | Stem rendering (planned — the sheet is real, the render is not yet). |

Press `?` anywhere for the field manual.

![ARRANGE — the song timeline](docs/morgue_arrange.png)

## Directed sound design

The instrument refuses to make you choose between writing math and pulling a
slot-machine lever:

- **PATCH MORGUE** — sixteen named, known-good voices (CONCRETE FLOOR,
  COLD ROOM, DISTRICT ALARM, GLASS AUTOPSY…). Click one; it sounds.
- **VOICE DESIGN** — five macros that always mean the same thing — PITCH ·
  MOTION · DIRT · DARK · ROOM — driving whichever knobs of the current
  voice carry that meaning, worked out from the compiled bytecode.
- **SCULPT** — mutation with a direction: DARKER / BRIGHTER, CALMER /
  BUSIER, TIGHTER / HUGER, with STEP BACK to undo. Nudges, never gambles.
- **GROW** — renders the focused voice as a self-looping specimen WAV
  (whole bars at your tempo, slow parameter drift, a few cents of tape
  warble) straight into the locker, ready for a well or a timeline lane.

And when you do want the math: the expression editor is right there, compiles
on RETURN with no glitch, and a failed compile keeps the old program running.

![MIXER — twelve strips and the CHAMBER](docs/morgue_mixer.png)

## Build

macOS (the GUI):

```sh
git clone --recursive https://github.com/dodontommy/bytebeat.git
cd bytebeat
cmake -S . -B build -G Ninja
cmake --build build --target MORGUE
open build/MORGUE.app
```

Requires CMake ≥ 3.20 and a C11/C++17 toolchain (Xcode CLT). JUCE 8.0.15 is
pinned as a submodule; the IBM Plex fonts ship embedded (OFL). If you cloned
without `--recursive`, run `git submodule update --init`.

Linux (the terminal instrument):

```sh
sudo apt install build-essential libasound2-dev libncurses-dev
make        # the TUI
make test   # the regression suite
```

See [docs/TUI.md](docs/TUI.md) for the terminal instrument's full manual,
including headless evaluation and streaming raw PCM over TCP.

## The engine

`engine.c` is the instrument: the render loop, the voices, the step sampler,
the looper, the chamber, the timeline and the session file, with hard
realtime rules — the audio thread never allocates, never locks, never
blocks. Every knob is an atomic; programs, samples, clips and songs are
published to the audio thread by single atomic pointer swaps and reclaimed
only after two render epochs. Integer overflow is not a bug here;
`-fwrapv` everywhere, because the overflow **is** the sound.

The signal path, per voice:

```text
expression -> gate -> drive -> tone -> crush -> SPACE -> level
           -> eight-voice sum + drums + clips -> CHAMBER return
           -> phrase looper -> master gain -> output
```

Sessions autosave as editable plain text at `~/MORGUE/session.conf`.
Recordings, captured clips and grown specimens land beside it, in the
locker.

## Documents

- [docs/TUI.md](docs/TUI.md) — the terminal instrument
- [DESIGN_SPEC.md](DESIGN_SPEC.md) — product spec and the R1–R9 roadmap
- [design_handoff_morgue_gui/](design_handoff_morgue_gui/) — the pixel spec this GUI implements
- [HANDOFF.md](HANDOFF.md) — engineering state, for whoever builds next
- [NOTES.md](NOTES.md) — implementation tour · [EXAMPLES.txt](EXAMPLES.txt) — expression cookbook
