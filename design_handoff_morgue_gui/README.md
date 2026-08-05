# Handoff: MORGUE — noise / experimental instrument GUI

## Overview
MORGUE is a macOS-native instrument for dark noise, experimentalism, ambience and
avant-garde music: an 8-layer bytebeat synth with a step sampler, granular/tape
wells, a master phrase looper, mixer, MIDI control and a song timeline. This
bundle is the complete visual and structural specification of its GUI —
all 8 stage tabs, the transport/status chrome, the export sheet, the field-manual
overlay, and a parts bin of every control state.

Target: **JUCE 8.0.15, macOS**, single window, 1440×900 default (min 1180×760).

## About the design files
`MORGUE GUI.dc.html` is a **design reference built in HTML** — a static,
pixel-accurate rendering of the intended interface, not production code and not
intended to be ported to a webview. Read it as a drawing: every colour, size,
border and label in it is deliberate. The implementation task is to recreate it
in JUCE using custom `paint()` drawing and the component tree given in
`MORGUE_UI_SPEC.md` §16.

`MORGUE_UI_SPEC.md` is the authoritative source for numbers: colour tokens,
type scale, band heights, per-panel geometry, control states, interaction
contract and the JUCE class map. Where the HTML and the spec disagree, the spec
wins for measurements and the HTML wins for appearance.

`DESIGN_SPEC.md` is the original product/feature brief (engine capabilities,
status legend, and the R1–R9 roadmap) for context on what each control drives.

## Fidelity
**High-fidelity.** Final colours, typography, spacing and control geometry.
Recreate pixel-for-pixel. There is no existing design system to defer to —
the tokens in the spec *are* the design system.

## Screens / views
Ten frames, in the HTML in this order (each carries a `data-screen-label`):

1. **RACK** (tab 1, LIVE) — the voice station. Voice strip, 17-source grid,
   bytebeat expression editor, p0–p7 knob bank with bytecode-inferred roles,
   6-knob post chain, 16-step sequencer + parameter-lock lane. Spec §5.
2. **ARRANGE** (tab 2, planned R2/R3) — bar ruler, 10 clip lanes, clip blocks,
   automation ("MOTION") lane, blood-red playhead. Spec §6.
3. **GRAIN LICKS** (tab 3, planned R1) — step sampler: 8 slots × 16 steps,
   per-slot pitch/vel/level, mute/solo, choke groups. Spec §7.
4. **GRAIN MASS** (tab 4, LIVE) — 2×2 sample wells with waveform, transport,
   pitch, and disabled GRAIN/ERASE (planned) knobs. Spec §8.
5. **SURVIVOR** (tab 5, LIVE) — master phrase looper: ARM/PLAY/CLEAR, loop
   buffer view with slice grid, six large loop knobs. Spec §9.
6. **MIXER** (tab 6, partial → R4/R5) — 12 strips (8 voices, LICKS, returns A/B,
   master): inserts, sends A–D, pan, trough fader + meter, M/S, routing. Spec §10.
7. **EXPORT** (tab 8, planned R6) — modal stem-render sheet over the dimmed
   console. Spec §11.
8. **HW/SYNC** (tab 7, LIVE) — MIDI device row plus a 10×14 CC mapping matrix.
   Spec §12.
9. **FIELD MANUAL** — full-window overlay: 12 zone cards, golden rules, key
   reference. Spec §14.
10. **COMPONENT SHEET** — not a screen. The parts bin: palette swatches with
    usage, all knob states, all switch-plate states, cell/fader/meter states,
    the type scale, and the JUCE component map. Build these primitives first.

Layout, sizes, colours and copy for each are enumerated per-panel in the spec.

## Interactions & behaviour
Spec §15 is the contract. The load-bearing parts:
- 30 Hz engine→UI sync; a control under an active drag is excluded from the pull
  so knob positions never fight the engine.
- Knobs: horizontal drag (1 unit / 2 px), ⌘-drag fine, double-click default,
  right-click MIDI learn, scroll ±1. Range 0–255, sweep −135°…+135°.
- Step cells: click cycles OFF → HIT → ACCENT; right-click clears; drag paints.
- Expression editor: RETURN compiles hot with no glitch; caret and focus survive
  both the compile and the 30 Hz sync. Compile failure prints one deadpan line
  and keeps the previous program running.
- Every interactive element has a tooltip: `NAME — what it does. Range/units.`
- Keys: `?` manual · RETURN compile · 1–8 focus voice · SPACE run · ESC cut ·
  R/O/A/Z sample · M arm motion capture · ⌘Z undo · ⌘R rec.
- No animation anywhere except meter/scope redraw and the 800 ms CLIP hold.
  Nothing fades, slides, or eases.

## State management
Owned by the engine, not the UI: transport (run/cut/rec, bpm, beats, bars, gain,
bar/step position), per-voice program + p0–p7 + post chain + 16-step pattern +
lock lanes, sampler slots, looper buffer/params, mixer levels/mutes/sends,
MIDI mapping, session file. The GUI holds only: active tab, focused voice,
selected locker row, editor caret/text buffer, drag-in-progress flags,
manual-overlay visibility. Persist mappings and all engine params in
`session.conf`; the GUI stores nothing of its own except window bounds.

## Design tokens
Full table in `MORGUE_UI_SPEC.md` §1–2. Summary: matte black ground
(#0a0a0a) → panel (#0c0b0a) → raised bands (#151412) → recessed sockets
(#060606); hairlines #232220, control edges #3a3833; ink #ded9ce / #8a8579 /
#55524b; the single accent is dried blood #8b1e14 with #c2301f for lamps and
playheads; oxide #8a5a2b for sends/automation/planned tags; amber #b8862b for
warn. Type is IBM Plex Mono (400/500) with IBM Plex Sans Condensed 700 for
stencil headers — ship both as binary resources. Spacing is a 1/2/4/6/8/10/12 px
scale. **Border radius is 0 everywhere** except circular knob faces; no
gradients, no shadows.

## Assets
None. No images, no icons, no SVG artwork — every mark in the design is a
rectangle, a circle, a 1px rule, a `+` registration glyph, or type. The two
webfonts are the only external dependency.

## Files
- `MORGUE GUI.dc.html` — the visual reference (open in a browser; scroll for
  all ten frames).
- `MORGUE_UI_SPEC.md` — measurements, states, interaction contract, JUCE tree.
- `DESIGN_SPEC.md` — original product brief and R1–R9 roadmap.

## Build order suggestion
`MorgueLookAndFeel` + the primitives from frame 10 (EngravedKnob, PlateButton,
StepGrid cell, TroughFader, MeterComponent, header band, StatusBadge) → window
skeleton (§3) → RACK → MIXER → SURVIVOR → GRAIN MASS → HW/SYNC → GRAIN LICKS →
ARRANGE + automation → EXPORT. The field manual can be built any time; it is
static content.
