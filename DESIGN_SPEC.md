# MORGUE — Product & Feature Specification (for Design)

Instrument for dark noise, experimentalism, ambience and avant-garde music.
Aesthetic brief: Atrax Morgue, Prurient, Genocide Organ — cold clinical
autopsy-archive starkness, cryptic occult-industrial dread, brutalist
signal-processing machinery. Subtly terrifying, death-obsessed, NOT corny.

---

## SYSTEM LAYOUT (window skeleton)

A single desktop window. Zones:

```
┌────────────────────────────────────────────────────────────┐
│  LOCKER (left sidebar)     │  MAIN STAGE (tabbed panels)    │
│                           │   RACK / ARRANGE / GRAIN MASS  │
│                           │   SURVIVOR / MIXER / HW-SYNC   │
│───────────────────────────┼────────────────────────────────│
│  SCOPE (right column)     │                                │
│  live waveform            │                                │
├───────────────────────────┴────────────────────────────────┤
│  TRANSPORT bar (RUN CUT REC INFO  BPM BEATS BARS GAIN)      │
│  STATUS bar (bar# step# CPU% CLIP  "press ? for map")       │
└────────────────────────────────────────────────────────────┘
```

- `?` key or INFO button opens a full-screen FIELD MANUAL overlay mapping
  every panel and listing the workflow.
- Tooltips on every interactive element.
- All controls reflect live engine state (30 Hz sync); controls being
  dragged are left alone so turns never fight.

---

## 1. LOCKER (browser / evidence locker)

- Lists `~/MORGUE/` contents: recorded WAVs and project patches.
- Header band, evidence-tag serial numbers, registration-cross corner marks.
- Behaves like a specimen archive.

## 2. RACK (the voice station — the heart)

The instrument is an 8-layer bytebeat synth. Each layer (VOICE 01–08) holds:

- **Layer strip**: 1–8 selector buttons; clicking selects the active voice.
- **Source selector**: 17 synthesis sources by name (thump, burst, metal,
  dust, fold, ...), each a bytebeat expression with known-good sound.
- **ROLL / MUTATE buttons**: ROLL generates a random voice; MUTATE tweaks the
  current one. Always-audible-before-you-hear-it auditioning.
- **BODY / SPACE toggles**: wrap the source in filtering / feedback-delay
  stages.
- **Expression editor**: the actual bytebeat source text; type + ENTER to
  live-compile with no glitch. Cursor stays in the box for fast iteration.
- **8 expression knobs (p0–p7)**: each carries a role LABEL inferred from the
  bytecode (PITCH, FILTER, RESON, GRAIN, LEVEL...) or "UNUSED" when the
  expression doesn't use it. Values 0–255.
- **Post chain (6 knobs)**: DRIVE → TONE → CRUSH → SPACE (time/feedback/mix)
  — per-voice dirt between the voice and the mixer.
- **16-step sequencer** (1 bar of 16ths): click cells to cycle
  off → hit → accent.

## 3. ARRANGE (multitrack timeline — display today, recording/arrangement next)

- 8 lanes (one per voice), live playhead, bar ruler.
- Current state: live-position canvas. Multitrack recording + clip
  arrangement is the next milestone.

## 4. GRAIN MASS (sampler / granular / tape mangling)

- 4 sample slots (wells).
- **Double-click a well** to load WAV/AIFF/MP3/OGG/FLAC from disk.
- Per-slot playback: PLAY/STOP, PITCH (A/Z), REVERSE (R), LOOP (O).
- Samples mix on top of the engine output in real time.
- Vision: granular slicing, tape-eraser textures, cross-modulation with the
  engine voices.

## 5. SURVIVOR (performance / live DJ view)

The engine's REAL master phrase looper, surfaced live:

- **ARM** — arms capture of 1 bar at the next bar boundary.
- **PLAY** — loops the captured master bus.
- **CLEAR** — wipe the loop.
- **Loop knobs**: MIX (dry/loop crossfade), FB (feedback/overdub
  retention), OD (overdub), HALF (half/normal/double speed), REV (reverse),
  SLICE (1/2..1/16 stutter).
- Captures the finished pre-master bus including every layer and its tails.

## 6. MIXER (levels + routing)

- 8 channel faders (voice level 0–256) + MASTER fader.
- Mute button per channel.
- Live peak meter per channel + master.
- Vision: sends, per-channel FX insert slots, panning, group buses.

## 7. HW/SYNC (MIDI / hardware control)

- MIDI input device picker + ENABLE toggle.
- MIDI notes → trigger the focused voice (velocity-sensitive, transpose by
  note).
- MIDI CC → live expression parameter p0 of the focused voice.
- Vision: full mod-matrix-style CC mapping, MIDI clock sync, footswitch.

## 8. TRANSPORT (master performance controls)

- RUN — master on/mute.
- CUT — instant master silence (panic).
- REC — records the master output to `~/MORGUE/<timestamp>.wav` (real WAV).
- BPM / BEATS (per bar) / BARS (loop length) / GAIN knobs — live transport,
  sequence-time modulation.
- Vision: song position timeline scrub, metronome, tap tempo.

## 9. SCOPE

- Live oscilloscope of the master output bus (pre-gain, pre-mute), 30 Hz.
- Centre-zero nudge line; rendered in ash/bone.

## 10. STATUS bar

- Current bar number, current step.
- CPU load vs budget (%); amber near-red, blood-red when overloaded.
- CLIP indicator when the master bus hits the rails.
- Persistent hint: "PRESS ? FOR A MAP OF THIS CONSOLE".

## 11. FIELD MANUAL (help overlay)

- Full-surface map of every zone, one-line description each.
- The golden rules (e.g. "if it makes no sound, press RUN or release CUT").
- Key reference: `?` = map, `RETURN` = compile, drag knobs side-to-side.

---

## ENGINE CAPABILITIES (under the hood — for the design team to know what lives)

- 8 independent real-time voices, bytebeat expression VM (hot-patchable,
  no-glitch source swap on ENTER).
- Per-voice DSP: gate envelope, DRIVE/TONE/CRUSH, SPACE feedback delay,
  DC-block.
- Sequencer: gate/pitch/ratchet/probability + 16 parameter-lock lanes with
  interpolated "motion" automation.
- Master phrase looper (the SURVIVOR engine).
- Random voice generation (+ mutation), Euclidean rhythm generator.
- Session save/load (autosaving project text).
- 2,820 headless regression checks.
- Sample-rate independent; recordings land as standard WAV.

---

## DESIGN LANGUAGE (recap for the designer)

- **Palette**: matte black ground, raised panel grey-black, recessed "socket"
  wells; INK = ash-white/bone for text; the ONLY accent = dried-blood red
  (armed/live/danger); rusted oxide for secondary/send, amber for tape/warn.
- **Type**: monospace only. Caps + letterspacing for labels, dense mono for
  data. Stencil-weight for section/serial headers.
- **Motifs**: hairline rules (no gradients, no soft shadows, no rounded
  corners), registration crosses at corners, evidence-tag serials
  (e.g. "N.72-0418") on headers, illuminated dot on armed switches.
- Knobs = engraved dial face with one cut + numeric readout; faders = trough
  with blood fill; cells = flat toggle plates.
- Voice of the UI: institutional, clinical, deadpan. Lab paperwork, not
  goth merch.

---

## STATUS LEGEND

- **LIVE** — fully wired and working.
- **PARTIAL** — works but with limits.
- **CANVAS** — display only today; next milestone.
- **PLANNED** — specified here, not yet built.

LOCKER: live (list) · RACK: live · ARRANGE: canvas · GRAIN MASS: live (load/
play/pitch/reverse/loop) · STEPS: live (8 slots × 16 steps, per-step pitch/
velocity, level, choke groups, mute/solo) · SURVIVOR: live · MIXER: live
(fader/mute/meter) · HW/SYNC: live (note+CC) · TRANSPORT: live · SCOPE: live ·
STATUS: live · FIELD MANUAL: live.

---

# ROADMAP — FL-PARITY FEATURES (specified, to be built)

Target: the parts of FL Studio / Ableton-style DAW parity that matter for
dense noise, dark ambient, tape mangling and improvisation. Everything below
is a spec for the next build sessions. Order matters: each item builds on the
one before it.

---

## R1. STEP SAMPLER — "GRAIN LICKS" (drum machine / Channel Rack equivalent) — LIVE

The single most-requested workflow: **record a wav, then sequence it into a
repeating rhythm** — like FL's Channel Rack.

- A new STEPS workspace: the sample-sequencer grid (8 lanes, left rail per
  slot, gate grid, and a pitch/velocity inspector for the focused slot).
- **8 sample slots**, each like a RACK voice. A slot holds ONE loaded sample
  plus a 16-step pattern.
- **Load** per slot (file chooser, double-click a lane; same formats as GRAIN
  MASS). A stock kick/snare/hat kit is preloaded on first run like the rack
  groove.
- **16 step cells per slot** — click a cell and the sample fires on that
  step, in sync with the global BPM grid (the engine's own step clock).
- **Per-slot controls**: per-step pitch (semitone playback rate — a kick can
  "fall"), per-step velocity, slot level, and one-shot retrigger (pos reset
  to 0 on every hit) with **drum-choke groups** (firing a slot kills the
  other slots in the same group).
- **Mute / solo** per slot, like the mixer.
- Renders into the same master bus so REC captures it, SURVIVOR can loop it.
- Engine-side step sampler reuses the engine clock (`bb.seq_pos`); sample
  playback reuses the SamplerVoice reset-pos-on-trigger approach, now inside
  `engine.c` (`g_smp_*`) so the audio lands in the master bus and the
  regression suite can verify it headlessly.

Implementation notes (deviations from the original sketch, decided while
building):
- The audio path lives in the engine (not the JUCE `SamplerVoice` class),
  because REC and SURVIVOR capture the engine's master bus — a JUCE-only
  mixer would sit outside the sink/looper. `SamplerVoice` still powers the
  GRAIN MASS wells unchanged.
- Session "version 5" now persists slot patterns/levels/choke/mute/solo (not
  the sample audio itself).
- Regression coverage added: one-shot timing/amplitudes, one-shot end, choke
  member exclusion, per-step pitch doubling, playhead publish, session
  round-trip (~15 checks; suite is 2,835).

Design: slot = vertical strip (sample name, mute/solo, level, choke, 16
cells) matching RACK + MIXER visual grammar; inspector rows for pitch/vel.

Why it comes first: it's the missing "make beats" loop, and ARRANGE
(R2) needs per-clip audio that this produces.

## R2. ARRANGEMENT TIMELINE — "MORGUE PLAYLIST" (song composition) — PLANNED

Current ARRANGE is a live-position canvas. Turn it into a real timeline.

- **Multi-bar ruler** (1..64+ bars), position in bars/beats.
- **8+ audio clip lanes** (one per RACK voice or sampler slot).
- **Place clips**: drag a recorded WAV (from LOCKER or a new REC of a single
  voice) onto a lane at a bar position; clip plays there in song time.
- **Recording to a lane**: arm a lane, REC captures that lane's audio output
  (routed per-voice), not just the master — multitrack recording.
- **Clip operations**: move, copy, trim (drag edges), delete; loop a clip
  (right-click → set length → repeats until end).
- **Playback head** tied to the engine clock; song position scrubs the
  engine's `bb.k`.
- Layering a step-sampler pattern under the timeline: patterns are loopable
  clips like everything else.

Design: columns = bars, rows = lanes; clip = named block with handles;
playhead = blood-red vertical rule. This is the largest single UI effort.

## R3. AUTOMATION LANES — "MOTION" (record knob moves over time) — PLANNED

- The engine already stores per-step parameter locks with interpolated
  motion (`bb.layer[].seq_lock` / `motion_mask`). Surface and extend them.
- **Draw automation**: select a voice knob (p0..p7, DRIVE, TONE, CRUSH,
  SPACE, LEVEL) → an automation lane appears on the timeline; click/drag to
  draw a curve; it becomes an interpolated lock lane.
- **Record automation**: toggle ARM, move a knob while the transport runs;
  the moves are sampled to the lock lane (the TUI already does this with `M`;
  bring the same capture to the GUI).
- **Lane types**: step (hold per 16th) and smooth (interpolate between
  points, the existing motion mode).

## R4. FX INSERTS + SENDS (deep mangling chains) — PLANNED

- **Per-channel insert slots**: each RACK voice (and each step-sampler slot)
  gets an FX chain strip in the MIXER, currently just DRIVE/TONE/CRUSH/SPACE.
  Add: distortion types, bitcrush, wavefolder, pitch-shifter, delay, reverb
  (convolution), filter (LP/HP/BP), and a **feedback/glitch** module.
- **Sends (A, B, C, D)**: route any channel × send dial to shared return
  buses (big reverb, big feedback delay, pitch-glitch) — classic noise-engine
  architecture.
- **Master FX**: an FX chain on the master bus (so REC and the looper capture
  it — SURVIVOR already captures post-everything).
- Integer-first DSP, realtime-safe, following the engine's no-alloc,
  no-lock rules.

## R5. MIXER DEPTH — PAN / SOLO / GROUPS / ROUTING — PLANNED

- **Pan** per channel (L/R, with per-layer crossfade math already cheap).
- **Solo** (opposite of mute).
- **Groups/buses**: route N channels to a bus with its own fader + FX.
- **Input/monitoring**: select an audio input device for live recording
  (mic/line) into a lane or sampler slot.

## R6. STEM EXPORT — PLANNED

- Export per-track WAV (one file per voice/slot/return), plus master, plus
  "song render" (full timeline, not just the loop).
- Formats: WAV now; MP3/FLAC later. Sizes from REC's existing writer.

## R7. BROWSER DRAG-DROP — PLANNED

- LOCKER becomes a real file browser with drag-drop: drag a sample onto a
  GRAIN MASS well, a step-sampler slot, or a timeline lane to load it there;
  drag a clip/pattern onto the timeline to place it.
- Folder tree with favorites + recent.

## R8. MIDI OUT + FULL MAPPING — PLANNED

- **MIDI out**: surface slots / transport / tempo to hardware; MIDI clock
  sync out.
- **Full CC matrix / learn**: right-click any knob → "learn" → move hardware
  CC → knob is mapped; saved in the session.
- **Note mapping**: map notes to trigger specific slots/layers, not just the
  focused voice.

## R9. UNDO / PROJECT VERSIONS — PARTIAL (engine) → PLANNED (GUI)

- The engine has program publish/reclaim; the TUI keeps an undo stack.
- GUI needs: Ctrl+Z undo of edits (expression, sequencer, mixer, timeline),
  autosave snapshots, "save as" named projects (currently a single autosaving
  `session.conf`).

---

## BUILD SEQUENCE (recommended)

1. **R1 Step Sampler** — unlocks the "record → sequence a rhythm" loop; the
   foundation R2 needs for clip content.
2. **R2 Arrangement timeline** — song composition + multitrack record.
3. **R3 Automation** — expressive parameter movement on top of the timeline.
4. **R4 FX inserts/sends** — the deep-mangling sound design core.
5. **R6 Stems export** — get the work out.
6. Remaining: R7 drag-drop, R8 MIDI out/mapping, R9 undo.

Each step keeps the 30 Hz UI↔engine sync contract and the engine's real-time
safety rules; the regression suite grows with each milestone.

