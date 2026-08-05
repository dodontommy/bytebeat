# MORGUE — UI IMPLEMENTATION SPEC (JUCE 8.0.15 / macOS)

Companion to `MORGUE GUI.dc.html`. The mockup is the source of truth for
appearance; this file is the source of truth for numbers, states and class
structure. Every value below is logical px at 1.0 scale.

---

## 0. RULES OF THE LOOK

1. No gradients. No rounded corners (except knob faces, which are circles).
   No drop shadows. No default JUCE LookAndFeel drawing anywhere.
2. Every rule/border is exactly 1px. Never 2px except the active tab underline.
3. Monospace only. Caps + letterspacing for labels; stencil (condensed bold)
   only for masthead, panel titles and the three SURVIVOR buttons.
4. One accent: BLOOD. Oxide and amber are secondary/warn only. Nothing else
   is coloured, ever.
5. Voice of the UI: institutional, deadpan, lab paperwork. Serials on headers.
6. A `+` glyph at the left of every panel header band = registration mark.

---

## 1. COLOUR TOKENS

| Token | Hex | JUCE | Use |
|---|---|---|---|
| GROUND | `#0a0a0a` | `Colour(0xff0a0a0a)` | window ground |
| PANEL | `#0c0b0a` | `0xff0c0b0a` | panel body, lane field |
| PANEL_ALT | `#0e0d0c` | `0xff0e0d0c` | tab strip, status bar, toolbars |
| RAISED | `#151412` | `0xff151412` | header bands |
| TRANSPORT | `#121110` | `0xff121110` | title bar, transport bar |
| SOCKET | `#060606` | `0xff060606` | recessed wells, code editor, scope |
| CONTROL | `#171614` | `0xff171614` | knob face |
| PLATE | `#141312` / `#131211` | | idle button plate |
| PLATE_HOVER | `#1b1a17` | | hovered/action plate |
| HAIRLINE | `#232220` | `0xff232220` | all borders/rules |
| HAIRLINE_DIM | `#1c1b19` | | inner sub-rules |
| HAIRLINE_FAINT | `#141312` | | table row separators |
| EDGE | `#3a3833` | `0xff3a3833` | raised control border |
| INK | `#ded9ce` | `0xffded9ce` | primary text, knob cut |
| INK_BRIGHT | `#f0e6dc` | | text on blood |
| INK_DIM | `#8a8579` | | labels |
| INK_FAINT | `#55524b` | | metadata |
| INK_GHOST | `#3a3833` | | disabled |
| BLOOD | `#8b1e14` | `0xff8b1e14` | armed border, fader fill, meters |
| BLOOD_DEEP | `#2a0d09` | | armed plate background |
| BLOOD_HOT | `#c2301f` | | lamps, playhead, CLIP |
| OXIDE | `#8a5a2b` | `0xff8a5a2b` | sends, automation curve, PLANNED tags |
| OXIDE_DIM | `#6b4a2a` | | oxide borders |
| OXIDE_INK | `#c9a06a` | | text on oxide |
| AMBER | `#b8862b` | | tape/warn, meter above −6 dB, CPU warn |
| GREEN_FAINT | `#7c8a5a` | | one use only: "STREAM OK" / "COMPILED" |

Status badge colours: LIVE → border BLOOD / text BLOOD_HOT ·
PARTIAL & CANVAS → border OXIDE_DIM / text OXIDE ·
PLANNED → border `#2a2927` / text INK_FAINT.

---

## 2. TYPE

Fonts: **IBM Plex Mono** (400/500) everywhere; **IBM Plex Sans Condensed 700**
for stencil. Ship both as binary resources; no system-font fallback.

| Role | Size | Weight | Tracking | Case |
|---|---|---|---|---|
| Masthead | 30 | Cond 700 | .20em | caps |
| Panel title | 11 | Cond 700 | .24em | caps |
| SURVIVOR button | 22 | Cond 700 | .20em | caps |
| Tab | 10 | 400 | .18em | caps |
| Label | 9 | 500 | .16em | caps |
| Micro label | 8 | 400 | .10em | caps |
| Nano (cell nums, notes) | 6–7 | 400 | .06em | caps |
| Data readout | 15 | 400 | .04em | — |
| Big transport value | 15 | 400 | .04em | — |
| Code / expression | 12 | 400 | 0, line-height 1.5 | as typed |
| Body (manual) | 10 | 400 | 0, line-height 1.5 | sentence |

Minimum legible size in the app is 6px and it is used **only** for step-cell
index numbers and knob sub-notes. Nothing interactive is labelled below 7px.

---

## 3. WINDOW SKELETON (1440 × 900 default · min 1180 × 760)

Vertical stack, top to bottom:

| Band | Height | Notes |
|---|---|---|
| Title bar | 26 | 3× 9px circle outlines, masthead, session path, serial right |
| Tab strip | 30 | 8 tabs, each `padding 0 15`, active: bg `#191816` + 2px BLOOD bottom |
| Body | flex (764 at default) | left column + main stage |
| Transport | 60 | see §9 |
| Status | 20 | see §10 |

Body row:

| Zone | Width | Notes |
|---|---|---|
| Left column | 236 fixed | LOCKER (flex) over SCOPE (198 fixed) |
| Main stage | flex | 1px HAIRLINE divider on the left |

Every panel begins with a 22–24px header band: `background RAISED`,
`border-bottom HAIRLINE`, `padding 0 8–10`, contents
`+ · TITLE · subtitle … right: serial, status badge`.

Serials increment per panel: RACK `N.72-0418`, LICKS `0419`, MASS `0420`,
SURVIVOR `0421`, MIXER `0422`, HW/SYNC `0424`, EXPORT `0426`.

---

## 4. LEFT COLUMN

### LOCKER (236 × flex)
Header: `+ LOCKER … ~/MORGUE` (context hint changes per tab: "DRAG → LANE",
"DRAG → SLOT"). Rows are 26 tall: serial column 52px (8px, INK_GHOST; BLOOD_HOT
when selected), name (10px, ellipsised; OXIDE when `.morgue` patch), meta right
(8px INK_FAINT). Selected row bg `#191816`. Footer 20: count + a PLANNED note.

### SCOPE (236 × 198)
Header 22, plot area with 8px padding, centre-zero 1px HAIRLINE line, waveform
1px INK polyline from the engine ring buffer at 30 Hz, footer 18 with
`PRE-GAIN · PRE-MUTE · PK −1.8dB`.

---

## 5. RACK (tab 1) — main stage layout

| Row | Height |
|---|---|
| Header band | 24 |
| Voice strip | 46 |
| Middle (SOURCE \| editor/knobs \| sequencer) | flex |

**Voice strip**: label `VOICE` (34 wide) · 8 plates 30×26 (number over a 4px
lamp; selected = BLOOD_DEEP + BLOOD border + BLOOD_HOT lamp; sounding =
INK text + `#4a4842` lamp; silent = INK_FAINT) · divider · `ROLL` `MUTATE`
(26 tall, padding 0 12) · divider · `BODY` `SPACE` toggles with 5px lamps.

**SOURCE column**: 222 wide, 2-col grid of 17 cells, each 22 tall, `NN NAME`;
selected = BLOOD_DEEP/BLOOD. Footer: `AUDITION ON SELECT · NO-GLITCH SWAP`.

**Expression editor**: label row 20, then 122 tall SOCKET area, 12px mono,
line numbers in INK_FAINT, 8×15 BLOOD block cursor, footer line with byte
count / compile age / VM op budget. Cursor is never stolen by other controls.

**p0–p7**: label row 20, then 8 knobs of 44px, evenly flexed. Each knob:
face CONTROL, 1px ring EDGE (BLOOD when hovered/dragging/MIDI-bound), 1px cut
15 tall from top+4 rotating about the centre, inner 1px ring inset 9,
role label 8px beneath, value 10px (`000`-padded), `pN` in 7px INK_GHOST.
Role text comes from bytecode inference; `UNUSED` renders ring `#252420`,
cut `#3a3833`, label `#4a4842`, value INK_GHOST.

**Post chain**: 420 wide block, 6 knobs of 40px — DRIVE TONE CRUSH TIME FBACK MIX.
Footer names the signal order and carries the `R4 INSERTS PLANNED` tag.

**Sequencer**: rest of the width. 16 cells in a flex row, `gap 2`, 38 tall,
6px centre dot, 6px index bottom-right. States: OFF `#0c0b0a`/`#232220` ·
HIT `#332f2a`/`#4a4640` + INK dot · ACCENT BLOOD/BLOOD_HOT + `#f0e6dc` dot.
Below: the 16-slot **lock lane**, 40 tall, OXIDE fill from the bottom by value.
Footer: which lock lane, which parameter, motion mode, `M = CAPTURE MOTION`.

---

## 6. ARRANGE (tab 2) — R2 + R3

Toolbar 30: SELECT / DRAW / TRIM / SLIP · divider · ARM LANE / LOOP CLIP /
CONSOLIDATE · right hint.
Ruler 22: 120px gutter, then 32 bar columns; bar number every 4th, heavier rule
every 4th. Playhead is a 1px BLOOD_HOT vertical rule spanning ruler + lanes.
Lanes: 42 tall each, 10 shown (8 voices + LICKS pattern + MASS well). Lane head
120 wide: 5px arm lamp, name, kind tag (`VOICE`/`STEP`/`SMPL`).
Clips: inset 2 vertically, 1px border, a filled 8px title bar across the top,
and a 14px striped waveform strip at the bottom. Clip kinds:
audio `#141312`/EDGE · recorded `#1d1210`/BLOOD · pattern `#171614`/OXIDE_DIM.
Automation lane at the bottom, 96 tall: 120 gutter with 255/128/000 scale,
OXIDE 1.5px curve, centre hairline, playhead continues through it, header row
carries voice + parameter + mode + `ARM CAPTURE`.

---

## 7. GRAIN LICKS (tab 3) — R1

Toolbar 28: PATTERN A/B/C/D · FILL EUCLID / RAND / CLEAR · hints.
Step header 22: 300 gutter, 16 numbered columns (current step column tinted
`#1b1a17`), 140 right gutter.
8 slot rows, equal flex (~76 each). Row head 300 wide: index 16, name + meta
stacked, `M`/`S` 18×18 plates, divider, three 26px knobs PIT / VEL / LVL.
Grid: 16 columns with 3px padding, cell fills the column — same tri-state as
the RACK sequencer. Right gutter 140: `CHOKE` label + group tag (`G1`…`G4` in
oxide, `—` when none) + a 6×26 vertical meter.
Empty slot: name `— EMPTY —` in INK_GHOST, meta `DOUBLE-CLICK OR DRAG TO LOAD`.

---

## 8. GRAIN MASS (tab 4)

2×2 grid of wells separated by 1px HAIRLINE. Each well:
header 22 (`WELL NN`, filename, duration/rate, 5px playing lamp) ·
waveform area (SOCKET, 8px margin, 1px HAIRLINE_DIM border, centre line,
1px BLOOD_HOT play position, 7px corner hint) ·
control row 56: PLAY 44×24 / STOP 44×24 / `R` 30×24 / `O` 30×24, divider,
three 32px knobs PITCH (live) + GRAIN + ERASE (drawn disabled: ring `#2a2927`,
cut `#4a4842`, label `#4a4842`, pointer at −135°), right column with rate and
`GRAIN/ERASE: PLANNED`.
Footer 26: mixing note, key reference `A / Z · R · O`, vision line in OXIDE.

---

## 9. SURVIVOR (tab 5)

Row 1 (120): three 150×74 buttons — ARM (armed: BLOOD_DEEP/BLOOD, lamp +
`ARMED · 1 BAR`), PLAY (idle plate + dead lamp + `LOOP IDLE`), CLEAR
(`WIPE BUFFER`) — divider — a 3-line data block BUFFER / SOURCE / CAPTURE
(countdown in BLOOD_HOT) — right: loop-out meter 140×8.
Row 2 (210): loop buffer waveform in a SOCKET box, 1/8 slice grid as HAIRLINE
verticals, BLOOD_HOT loop position.
Row 3 (flex): six 76px knobs — MIX / FB / OD / HALF / REV / SLICE. These are
the largest knobs in the app: 2px cut, 26 long, inner ring inset 16, 10px
label, 12px value (`NORM`, `OFF`, `1/8` are text values), 7px sub-note.
Transport footer note: `CUT DOES NOT WIPE THE LOOP`.

---

## 10. MIXER (tab 6) — R4 / R5

12 strips: V01–V08, LICKS, RETURN A, RETURN B, MASTER (flex 1.5, bg `#100f0e`).
Strip stack, top to bottom:
1. Header 22 — 5px lamp + name.
2. INSERTS — 4 slots, 15 tall, 7px name; empty = `—` in INK_GHOST on `#101010`;
   loaded = `#1a1512`/OXIDE_DIM/OXIDE_INK; master inserts use blood tones.
3. SENDS — four 20px knobs A B C D; ring/cut go oxide above 40%.
4. PAN — 6px label + 8px trough with centre tick and a 3px OXIDE handle.
5. Fader area (flex) — 16px trough (`#080807`, HAIRLINE border), BLOOD fill
   from the bottom, 3px INK cap overhanging 3px each side; 8px meter column
   beside it, AMBER above −6 dB, BLOOD_HOT at the rails.
6. Value 10px, dB 7px, `M`/`S` plates 16 tall, route tag 14 tall
   (`G1` / `MASTER` / `OUT 1-2`).

Muted channel: fader fill `#2a2927`, value/name INK_FAINT, dB `−∞`,
`M` plate armed. Footer 26 documents returns, groups, and the R4/R5 interactions
(click an insert slot → picker; right-click a knob → learn).

---

## 11. EXPORT (tab 8) — R6

Modal sheet 720 wide centred over the dimmed console (console at 22% opacity
under a 72% GROUND scrim — no blur). Header 28 with serial.
Left pane: 12 track rows, 24 tall — 11px checkbox (checked = BLOOD_DEEP box,
BLOOD border, 5px BLOOD_HOT tick), name, size right.
Right pane 280: RANGE (LOOP / SONG / SEL), FORMAT (WAV 24 active; FLAC, MP3
disabled), TAIL field, DESTINATION path field, two info lines, then
CANCEL / RENDER (RENDER = BLOOD_DEEP/BLOOD) 30 tall at the bottom.

---

## 12. HW/SYNC (tab 7) — R8

Row 1 (104): left — INPUT DEVICE combo (26 tall SOCKET field with a `▾`) +
90×26 ENABLE toggle (armed), then a data line CH / LAST NOTE / LAST CC /
`STREAM OK`. Right 340 — CLOCK/OUT with `R8 PLANNED` badge, three disabled
plates CLK IN / CLK OUT / MIDI OUT, footnotes.
Row 2 (flex): CC matrix. Header 24: `SOURCE` gutter 150, 14 target columns
(p0–p7, DRIVE, TONE, CRUSH, SPACE, LEVEL, LOOP MIX), `VALUE` gutter 90.
10 rows, equal flex: source name + device alias; a 9×9 square per intersection
(mapped = BLOOD/BLOOD_HOT, else transparent/HAIRLINE_DIM); value gutter = 5px
bar + numeric. Last row is `— UNMAPPED / LEARN…` in INK_GHOST.

---

## 13. TRANSPORT (60) and STATUS (20)

Transport: RUN / CUT / REC each 64×34 with a 5px lamp under the word, then a
52×34 `?`; divider; four knob+readout pairs (34px knob, 8px label, 15px value)
BPM / BEATS / BARS / GAIN; right side is context — master meter on RACK, song
position on ARRANGE, EXPORT… button on MIXER, a deadpan note elsewhere.
Armed states: RUN armed whenever the engine runs; CUT engaged paints solid
BLOOD with INK_BRIGHT text; REC armed while writing.

Status: `BAR 003 · STEP 07/16 · CPU 41% · CLIP · 44100 Hz · 128 SMP` left,
`PRESS ? FOR A MAP OF THIS CONSOLE` right, all 9px .12em. CPU is INK_DIM
below 60%, AMBER 60–85%, BLOOD_HOT above. CLIP is INK_GHOST until it fires,
then BLOOD_HOT, holding 800 ms.

---

## 14. FIELD MANUAL OVERLAY

Full window, bg `#080807`. Header 64: masthead `FIELD MANUAL` 30px stencil,
serial/rev, `ESC / ? TO DISMISS` right. Body: left flex = 2-column grid of 12
zone cards (index, stencil name, status badge, one-line description in 10px
INK_DIM); right 400 = GOLDEN RULES (6 numbered rows) then KEY REFERENCE
(12 rows: 64px key cap plate + description). Footer 26 with the product line
and the regression-check count.

---

## 15. INTERACTION CONTRACT

- **Sync**: 30 Hz timer pulls engine state into all controls. A control being
  dragged is excluded from the pull until mouseUp — turns must never fight.
- **Knobs**: horizontal drag, 1 unit per 2 px; ⌘-drag = fine (1 unit per 8 px);
  double-click = default; right-click = MIDI learn (R8); scroll = ±1.
- **Cells**: left-click cycles OFF → HIT → ACCENT → OFF; right-click clears;
  click-drag paints the state you started with.
- **Expression editor**: RETURN compiles; focus and caret survive compile and
  30 Hz sync. Compile failure prints one deadpan line under the editor and
  keeps the previous program running.
- **Tooltips**: every interactive component has one. Format
  `NAME — what it does. Range/units.` No exclamation marks.
- **Keys**: `?` manual · `RETURN` compile · `1–8` focus voice · `SPACE` RUN ·
  `ESC` CUT (and dismiss manual) · `R`/`O`/`A`/`Z` sample controls ·
  `M` arm motion capture · `⌘Z` undo · `⌘R` REC.
- **Empty states** are lab paperwork, never friendly:
  `— EMPTY —` / `DOUBLE-CLICK OR DRAG TO LOAD` / `NO SPECIMENS IN ~/MORGUE`.

---

## 16. JUCE COMPONENT TREE

```
MorgueMainComponent                     // owns layout in resized()
├─ TitleBar                             // 26
├─ StageTabs                            // 30, custom (not TabbedComponent)
├─ Body
│  ├─ LeftColumn (236)
│  │  ├─ LockerListBox : ListBoxModel   // custom paintListBoxItem
│  │  └─ ScopeComponent : Timer(30)     // Path from engine ring buffer
│  └─ StageStack                        // one child per tab, only one visible
│     ├─ RackPanel
│     │  ├─ VoiceStrip (PlateButton ×8 + ROLL/MUTATE + BODY/SPACE)
│     │  ├─ SourceGrid  (PlateButton ×17)
│     │  ├─ ExpressionEditor : TextEditor  // custom paint, mono 12
│     │  ├─ KnobBank  (EngravedKnob ×8, 44px)
│     │  ├─ PostChain (EngravedKnob ×6, 40px)
│     │  └─ StepGrid + LockLane
│     ├─ ArrangePanel  (BarRuler, LaneViewport, ClipComponent, AutomationLane)
│     ├─ LicksPanel    (LickSlotRow ×8 → StepGrid + 3 knobs + choke tag)
│     ├─ GrainMassPanel(SampleWell ×4)
│     ├─ SurvivorPanel (3 stencil buttons, LoopWaveView, EngravedKnob ×6 76px)
│     ├─ MixerPanel    (ChannelStrip ×12)
│     ├─ HwSyncPanel   (device row, CcMatrixComponent)
│     └─ ExportSheet   (modal)
├─ TransportBar                         // 60
├─ StatusBar : Timer(30)                // 20
└─ FieldManualOverlay                   // modal, full window
```

Shared: `MorgueLookAndFeel` (only to kill JUCE defaults), `EngravedKnob : Slider`,
`PlateButton : Button`, `TroughFader : Slider`, `MeterComponent : Timer(30)`,
`SerialTag`, `StatusBadge`.

All painting is integer-aligned; use `g.fillRect` with whole pixels so hairlines
never soften. Do not use `Graphics::drawRoundedRectangle` anywhere.
