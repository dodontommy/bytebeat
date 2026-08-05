# AGENTS.md — MORGUE

A full-optional-noise instrument: a JUCE GUI fronted onto the proven bytebeat
audio engine. Terminal instrument (TUI) still exists and shares the engine.

## Two front ends, one engine

- `engine.c` / `engine.h` — the device-independent audio core. Owns `bb`
  master state, the render loop (`bb_engine_render`), publish/reclaim, session
  I/O, defaults, first-run groove, phrase looper. **No ALSA, no ncurses.**
- `audio.c` — thin ALSA wrapper (TUI only). Its thread calls `bb_engine_render`.
- `app/` — the JUCE GUI ("MORGUE"). Links `bbengine` + JUCE modules; its device
  callback calls the same `bb_engine_render`.
- The 2,820-check regression suite runs against the engine on ANY platform
  (no ALSA needed): `./bytebeat -T`.

## Build

GUI (macOS shown; works same on Windows/Linux):

```sh
cmake -S . -B build -G Ninja
cmake --build build --target MORGUE
open build/MORGUE.app            # or run build/MORGUE manually
```

Requirements: CMake ≥3.20, a C11 + C++17 toolchain. JUCE is vendored in
`third_party/JUCE` (pinned 8.0.15). Nothing else needed on macOS/Win; Linux
needs the JUCE system deps (`libcurl4-openssl-dev libasound2-dev libx11-dev`
etc. per JUCE docs).

TUI (Linux-only, ALSA):

```sh
sudo apt install build-essential libasound2-dev libncurses-dev
make        # terminal bytebeat
make test   # 2,820 headless checks
```

There is no `make test` on macOS because ALSA cannot legally install there;
the suite still runs via the GUI-target build (`cargo`-less, just CMake).

## Architecture conventions

- The engine is C11 with hard real-time rules: `bb_engine_render` must never
  malloc/lock/block. All DSP state is static. Cross-thread UI is atomics only.
- The GUI is C++17. A 30 Hz `Timer` pulls state FROM the engine into the
  console (`*Panel::sync()`), skipping controls the user is dragging. The
  engine is the single source of truth.
- New engine features: add atomics to `bytebeat.h`, consume them in
  `engine.c`, expose an API in `engine.h`. Keep it real-time safe.

## GUI workspaces (all live)

- RACK: pick voice, source, ROLL/MUTATE voices, edit expression + ENTER to
  compile, knobs (roles inferred from the bytecode), 16-step sequencer.
- TRANSPORT: RUN/CUT, BPM/BEATS/BARS/GAIN, REC → `~/MORGUE/*.wav`.
- MIXER: faders (level) + mute per layer, live meters, master fader.
- SURVIVOR: the engine's real phrase looper — ARM/PLAY/CLEAR, mix/fb/od/
  rate/rev/slice.
- GRAIN MASS: load audio (double-click a well), play, pitch A/Z, reverse,
  loop. Mixed on top of the engine by `SamplerVoice` (real-time try-lock).
- HW/SYNC: MIDI input → notes trigger the focused voice, CC → p0.
- `?` opens the field manual; tooltips everywhere.

## Screenshot (permission-free self-render)

```sh
build/MORGUE.app/Contents/MacOS/MORGUE --screenshot
# writes morgue_render.png to the CWD
```
