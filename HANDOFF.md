# HANDOFF — MORGUE (current state, for the next session)

Written at the end of the first build pass. Everything here was verified
green on macOS (Apple Silicon, Xcode CLT) immediately before writing.

---

## TL;DR

A working 8-voice bytebeat instrument with a JUCE GUI and a terminal (TUI)
front end that both drive the SAME C audio engine. The GUI is fully
functional — every workspace wired to the engine, no painted fakes — with a
2,961-check regression suite guarding the engine. The FL-parity roadmap in
`DESIGN_SPEC.md` is underway: **R1 (the step sampler / Channel Rack) and
R2 v1 (the arrangement timeline) are built and verified**; automation draw
(R3) is next.

---

## Project state at handoff

| Thing | State |
|---|---|
| GUI build (`cmake --build build --target MORGUE`) | ✅ compiles, 0 warnings, runs, no crash (12 s soak + `--screenshot`) |
| Engine regression suite (`./bytebeat -T`) | ✅ all 2,835 checks pass (2,820 base + 15 R1 step-sampler checks) |
| WAV recording (transport REC → `~/MORGUE/*.wav`) | ✅ works |
| Sampler (GRAIN MASS: load/pitch/reverse/loop/pause) | ✅ works |
| Phrase looper (SURVIVOR: ARM/PLAY/CLEAR + mangling) | ✅ works (engine real looper) |
| Mixer (faders/mute/meter/master) | ✅ works |
| MIDI in (note→voice, CC1→p0) | ✅ works (single fixed CC, no learn yet) |
| Step sampler (STEPS tab: 8×16 drum grid) | ✅ R1 — built + verified |
| Arrangement/timeline compose (ARRANGE tab) | ✅ R2 v1 — built + verified (see R2 section) |
| Others | see roadmap R3..R9 in DESIGN_SPEC.md |

**Working directory & how to run it (tonight):**
```
/tmp/morgue_build2/MORGUE.app/Contents/MacOS/MORGUE
```
(That's the verified build. For a fresh build see Build below.)

---

## Architecture — read this first

Two front ends, one engine:

- `engine.c` / `engine.h` — device-independent audio core. Owns the `bb`
  master state (bytebeat.h), the render loop (`bb_engine_render`), program
  publish/reclaim, session config, defaults, first-run groove, phrase looper.
  Hard real-time rules: `bb_engine_render` must never malloc/lock/block; all
  DSP state static; cross-thread UI = atomics only.
- `audio.c` — ALSA wrapper (TUI only). Its thread calls `bb_engine_render`.
- `app/` — JUCE GUI ("MORGUE"). Its device callback calls the same
  `bb_engine_render`, int16 interleaved, converted to float.
- `main.c`, `ui.c` — the ncurses TUI (Linux).

**The seam that matters:** the GUI never touches audio devices directly. The
`bb` master struct (bytebeat.h) is the shared state. GUI panels write engine
atomics; a 30 Hz `Timer` in MainComponent pulls state back (`*Panel::sync()`),
skipping controls the user is dragging. Read bytebeat.h top-to-bottom — every
field's comment explains which thread owns it.

Key GUI files (app/):
- `Main.cpp` — shell, tabbed workspaces, 30 Hz sync timer, REC wiring, help.
- `AudioEngine.{h,cpp}` — JUCE device manager + callback → engine,
  `SamplerVoice` (real-time try-lock mixer), `WavRecorder` (rings → WAV).
- `Panels.{h,cpp}` — every workspace: RACK, TIMELINE, GRAIN MASS, SURVIVOR,
  MIXER, HW/SYNC, TRANSPORT, SCOPE, STATUS, HELP.
- `Theme.{h,cpp}` — palette/type/motifs (the aesthetic spec).

## Build

```sh
cmake -S . -B build -G Ninja          # or "Unix Makefiles"
cmake --build build --target MORGUE
open build/MORGUE.app                 # macOS
```

- JUCE is vendored `third_party/JUCE` (pinned 8.0.15, detached HEAD).
- `bbengine` static lib = pure C11 engine sources (engine/expr/dsp/knob/
  rack/gen) compiled as C. `extern "C"` guards already in the C headers so
  C++ links cleanly.
- Linux needs JUCE system deps; TUI needs ALSA (`make` / `make test`, Linux
  only — ALSA does NOT install on macOS).

## Conventions (keep these)

1. Engine is real-time-safe. Add atomics to bytebeat.h, consume in engine.c,
   expose in engine.h.
2. UI pulls FROM engine via sync(); never store truth in the UI.
3. Knob/Fader/LabButton have `setValueQuiet`/`setToggleState` for sync, and
   `isUserDragging()` guards. Keep using them.
4. C11 + C++17. `-fwrapv` everywhere (bytebeat overflow IS the sound).
5. All panels custom-drawn per Theme. No JUCE LookAndFeel, no gradients, no
   rounded corners. Test with `--screenshot` (permission-free PNG of the UI).

## Known items / gotchas

- JUCE 8 API notes: `audioDeviceIOCallbackWithContext` (not plain
  `audioDeviceIOCallback`); `String::append` returns void; FileChooser uses
  `launchAsync` (modal loops disabled); Font uses `FontOptions`; tooltips =
  `SettableTooltipClient`.
- The GUI scratch buffer must be `nframes * nOut` (interleaved) — fixed a
  heap-corruption there; keep it that way.
- Voice pointers for the audio callback are held in a member array
  (not a stack array) — keep it that way.
- `morgue_render.png` is a scratch artifact; `--screenshot` writes to CWD.
- `.gitignore` covers `*.o`, `/bytebeat`, `*.wav` — `third_party/` and
  `app/`, `engine.c/h`, `AGENTS.md`, `DESIGN_SPEC.md` are new/untracked.

## R1 STEP SAMPLER — what was built tonight

The full drum-machine loop landed: **record/load a wav → sequence it into a
repeating rhythm**, routed into the master bus so REC and SURVIVOR capture it.

Engine (`bytebeat.h` + `engine.c` + `engine.h`):
- `bb.sampler[8]` — per-slot atomics: gate/pitch/velocity per step, LEVEL,
  CHOKE (0..4 group), MUTE, SOLO. SamplerSlot lives in bytebeat.h.
- Engine-side playback (audio-thread only, no alloc): `sampler_snapshot()`
  per period + `sampler_process()` per frame. A slot fires on the engine's
  own step clock (`tick = k/step_len`) when `gate[step]` is set — play
  position resets to frame 0 (one-shot), step pitch picks the Q32 playback
  rate (`PITCH_Q32`), step velocity picks the amplitude, CHOKE groups kill
  the other running members. Summed into `mix` before the master clip, the
  phrase looper and the sink ring → REC + SURVIVOR both see it.
- Sample memory is UI-owned, published lock-free like Programs:
  `bb_engine_sampler_set/clear/reclaim/loaded` (retire list, epoch+2).
- First run preloads a synthetic kick/snare/hat kit (`drum_kit_sample` +
  `sampler_demo_kit`) so the STEPS tab grooves before any file is loaded.
  (sinf/expf used — added `-lm` to the Makefile and a `m` link for non-APPLE
  in CMake.)
- Session config bumped to **version 5**: persists sampler pattern state
  (`sampler/sgate/spitch/svel` lines) and looper/layer state as before.
- `bb.seq_pos` now publishes the playhead when only the sampler is running.

GUI (`Panels.{h,cpp}`, `Main.cpp`):
- New **STEPS** workspace: 8 lane rows — left rail per slot (double-click to
  load a file, right-click to clear, M/S, LVL knob, CHOKE combo), 16-cell
  gate grid (off/on/accent + oxide playhead), and a PITCH/VELOCITY inspector
  for the focused slot (drag up/down to edit a step). Keys 1-8 focus a slot.
- `stepSampler.sync()` joined the 30 Hz timer with `isUserDragging` guards.

Verification: `cc ... && /tmp/bbtest/bytebeat -T` → all 2,835 checks pass;
GUI clean build (0 warnings), `--screenshot` of the STEPS tab and the default
tab both render, 12 s smoke run alive.

Notes / known R1 limits:
- The step-sampler audio path is engine-side (not the JUCE `SamplerVoice`)
  by design — REC/SURVIVOR only see the engine master bus. `SamplerVoice`
  still powers the GRAIN MASS wells unchanged.
- Slot sample *names* live only in the GUI (not persisted); patterns persist.
- "Per-step level" is per-step *velocity*; each slot also has a master LEVEL.
- No manual pad-trigger (drum-pad preview) or sampler mute-while-playing
  retrigger semantics yet — natural candidates for the next pass.

## THE SYNTH SIDE — CHAMBER + cold wing + specimens (added after R1)

Built to push the instrument toward late-Prurient territory (cold melodic
synth over the noise floor):

- **CHAMBER (RETURN A, live)** — engine master reverb bus in `engine.c`:
  integer Freeverb (8 damped combs + 4 allpasses, `verb_process`), summed
  into the mix pre-clip/looper/sink so REC and SURVIVOR capture the tail.
  Atomics: `bb.layer[L].send` (post-fader send), `bb.smp_send` (sampler
  bus), `bb.verb_size/tone/level/peak`. `verb_level 0` is bit-exact bypass.
  MIXER: send A knob live on V01–V08 + LICKS; RETURN A strip = live fader,
  SIZE/TONE knobs, meter. Session **version 6** persists all of it.
- **Cold wing** — 5 appended sources in `rack.c` (indices 17–21, append-only
  to keep saved src indices stable): `cold` (interval-saw pad), `vapor`
  (beat-breathing filter pad), `hymn` (pulse organ), `siren` (swept
  resonator whine), `glass` (tuned struck partial, triggered). All ride the
  semitone voice clock, so the sequencer PITCH lane plays them melodically.
- **Specimen synthesizer** — DIRECTED first (per the directability rule):
  `bb_engine_render_specimen_voice()` renders the focused layer's CURRENT
  voice — expression, params AND post chain — as a self-looping WAV
  (`SPC-VNN-XXXX.wav`, whole bars at tempo, drift + tape warble, tail
  crossfaded into head, private VM/PostState — safe while playing). The
  GROW button in the LOCKER footer uses it (falls back to the random
  cold-wing `bb_engine_render_specimen()` only when the layer is empty).
  Core is `specimen_core()` in engine.c; suite pins determinism, the post
  path and broken-expression rejection. Loop one in a GRAIN MASS well.
- Suite grew to **2,886 checks** (chamber impulse/decay/silence, specimen
  determinism/normalisation, per-source audibility, v6 round-trip). Also:
  SHIFT+1–8 / shift-click a voice plate toggles a layer on/off (TUI parity).
- TUI limitation: sends/chamber have no TUI controls yet (engine defaults
  keep it silent there; session.conf is hand-editable).

## GRAIN MASS — bar-synced starts + per-well LEVEL

- **PLAY ALL / STOP ALL** in the GRAIN MASS footer. PLAY ALL arms every
  loaded well (`SamplerVoice::armSyncStart`); the audio callback watches
  `bb.bar` transitions (`EngineCAPlayback::barSeen`) and fires all pending
  wells together, rewound, in the same buffer — so wells lock to the
  transport. Lamp on PLAY ALL = armed; click again cancels. STOP ALL is
  immediate. The rewind (`retrig`) is consumed inside mixInto under the
  same try-lock that guards `pos`.
- **LEVEL knob per well** (0–255, 128 = shipped level, `SamplerVoice.gain`).
  The old hidden 0.75× attenuation of unfocused wells is REMOVED — the
  LEVEL knob is the sole authority, so a well's loudness never changes
  because focus moved. Well levels are GUI-side (not in session.conf yet,
  same as well filenames).

## DIRECTED COMPOSING — patch morgue + voice design macros (RACK rework)

Tommy's standing requirement: sound design must be DIRECTABLE — never "type
a math equation", never a random-roll lottery. The RACK now leads with:

- **PATCH MORGUE** (top of the source column) — 16 curated named voices in
  `rack.c` (`RackPatch PATCH[]`, `rack_npatch`/`rack_patch`): source +
  params + envelope + post chain + chamber send, click-to-load onto the
  focused voice, audition on select. The suite verifies every patch names
  a real source and auditions above silence.
- **VOICE DESIGN** row (between editor and p-knobs) — five fixed perceptual
  macros: PITCH · MOTION · DIRT · DARK · ROOM. Each drives whichever knobs
  of the CURRENT voice carry that meaning — rack slot kinds (`KV_*`) when
  the rack built the expression, bytecode roles (`ROLE_*`) when custom —
  plus the post chain and chamber send. Macros are UI-side write-gestures
  around a base captured at voice load (`captureMacroBase`); the engine
  stays the single source of truth and the p-knobs visibly move.
- **SCULPT** — directed mutation replaces gambling: DARKER/BRIGHTER,
  CALMER/BUSIER, TIGHTER/HUGER nudge the matching macro axis ±26 with a
  little jitter; STEP BACK is a 32-deep undo of nudges. ROLL/MUTATE still
  exist in the voice strip for raw material.

All of it lives in `app/panels/RackPanel.{h,cpp}` (macro machinery near
`applyMacroAxis`). Suite: 2,918 checks.

## R2 ARRANGEMENT TIMELINE v1 — built + verified

The ARRANGE tab is now a live playlist: a 64-bar song of up to 96 clips on
10 lanes (V01–V08, LICKS, MASS — organisational labels; any clip sits on
any lane), scheduled in ABSOLUTE BARS against the engine's monotonic bar
counter. Clip audio is mono int16, played 1:1 at the device rate from its
window start — **BPM stretches nothing**; a tempo change moves the bar grid
under the audio.

Engine (`bytebeat.h` / `engine.h` / `engine.c`):
- Song = published snapshot (`ArrSong`, atomic-pointer swap, epoch+2 retire
  like Programs). `bb_engine_song_publish` copies the UI's `ArrClip` array
  and sanitizes (lane clamp, len≥1, gain 0..256); `bb_engine_song_get`
  copies it back for UI/session. Clip audio lives in opaque `ArrClipBuf`s
  (`bb_engine_clip_create/release/frames/data`) — UI-owned lifetime,
  release only AFTER publishing a song without the clip.
- Render loop: per-clip frames-into-window counters with edge-detected
  window entry; loop wraps the audio inside the window, non-loop goes
  silent after its last frame; `(data[idx]*gain)>>8` summed BEFORE the
  master clip / phrase looper / sink, so REC and SURVIVOR capture the song.
  Counters are per-clip-INDEX and survive a republish (live edits do not
  restart in-window clips); seek/reset_loop restarts everything.
- `bb_engine_song_seek(bar)`: pending `bb.arr_seek_bar`, consumed at the
  top of the next period exactly like reset_loop (bar_count=target, layer
  tick latches, sampler tick and clip counters re-armed).
- Per-lane capture `bb_engine_arr_arm(lane, bars, dst, cap)`: starts at
  the next bar boundary, copies the lane's post-fader pre-chamber
  contribution (voice con; lane 8 = sampler premix delta; lane 9 refused)
  into the UI-owned dst; status ARMED→RECORDING→DONE in
  `bb.arr_rec_status`, progress in `bb.arr_rec_frames`; bars counted by
  actual bar boundaries so mid-capture BPM changes cannot skew the count;
  CAS'd edges so `bb_engine_arr_cancel` always wins. After cancel treat
  dst like a retired Program (reuse only after one reclaim frame).
- Session **version 7**: song META persisted as `aclip` lines (lane,
  start, len, loop, gain, quoted name, path to end-of-line). Audio is NOT
  persisted; the loader always republishes the song from the file (empty
  for v6-and-earlier), and the GUI rehydrates by re-decoding each path.
  v6 and older sessions still load.

GUI (`app/panels/ArrangePanel.{h,cpp}`, wired in `Main.cpp` + Chrome):
- Spec-section-6 chrome intact (toolbar 30 / ruler 22 + 120 gutter / ten
  42px lanes / clip grammar / automation lane 96 / BLOOD_HOT playhead);
  64 bars viewed as two 32-bar pages that jump with the playhead.
- Live: SELECT/TRIM mode pair; ARM LANE (lamp on the lane head); CAPTURE
  + BARS stepper (1/2/4/8) records N bars of the armed lane, writes
  `CLIP-<LANE>-<NNNN>.wav` into ~/MORGUE, places the clip at the capture
  bar and refreshes the LOCKER; PLACE drops the selected LOCKER WAV on
  the focused lane at the playhead (JUCE decode, mono-mix, file rate);
  LOOP CLIP; ruler click = seek; clip select / move / re-lane / trim
  (6px edge zones) / right-click delete / double-click loop — every edit
  republishes (publish is cheap). Deleting releases audio only after the
  removing publish. Missing session files stay ghost clips (name intact).
- Main.cpp wiring: `arrange.getLockerSelection` / `arrange.onLockerRefresh`
  to the LOCKER (`Locker::selectedFile()` / `refresh()`), and
  `arrange.rehydrateFromSession()` called AFTER `bb_config_load()` (panel
  members construct before the ctor body — the ctor call is the real
  rehydrate). `arrange.sync()` rides the 30 Hz timer; the panel also
  self-drives an internal idempotent 30 Hz sync. Transport's ARRANGE
  context shows `SONG bbb:bb:s` from `bb.bar`/`bb.seq_pos`.
- Suite: **2,961 checks** (2,918 baseline untouched + 43 appended R2
  checks: clip fires exactly at its start bar, loop wrap, one-shot tail
  silence, gain, seek restart, per-lane capture with a deterministic
  layer, v7 round-trip, v6 back-compat). Plus a standalone ramp-clip
  harness verified bit-exact clip landing at the bar boundary, and an
  ASan two-thread publish/release/seek/arm stress ran clean.

R2 v1 limits / what remains:
- Automation draw (DRAW/SLIP tools, automation ARM CAPTURE) — R3; the
  chips stay planned-styled and the automation lane still mirrors the
  focused voice's motion data read-only.
- CONSOLIDATE (bounce a lane's clips into one WAV) — planned chrome only.
- LOCKER drag-and-drop onto lanes — R7; today placement is the PLACE
  plate at the playhead.
- Clip audio persists as file PATHS only; a moved/deleted WAV rehydrates
  as a silent ghost clip. No embedded-audio session format yet.
- Playback counters are per-clip-index: inserting/removing clips
  mid-play can hand an in-window clip a stale counter until its window
  re-enters (seek or transport restart clears it).

See `DESIGN_SPEC.md` (roadmap) and `AGENTS.md` (build/architecture) for the
full detail. `NOTES.md` is the original TUI implementation tour.
