# AGENTS.md — MORGUE

An instrument for dark noise, ambience and terror: eight bytebeat voices, a
step sampler, four granular wells, a six-slot loop bank, an eight-slot return
bus and a bar-aligned song timeline. Everything you hear is a live-compiled
integer expression evaluated one sample at a time; the console just puts hands
on it.

Read this first, then `HANDOFF.md` for the engineering state and the full
invariant list.

## One front end, one engine

There used to be two front ends. The ncurses/ALSA terminal instrument
(`main.c`, `ui.c`, `audio.c`, `sink.c`, `Makefile`, `docs/TUI.md`) is deleted.
Its regression suite was the valuable part and survives as its own target.
Do not restore the `Makefile` on the theory that it was a working second build:
its `OBJS` list omitted `bb_platform.o` while `engine.c` calls `bb_now_us()`,
so `make` could not link for some time before it was removed.

- `engine.c` / `engine.h` — the device-independent audio core. Owns the `bb`
  master state, the render loop (`bb_engine_render`), publish/reclaim, the
  voices, the step sampler, the loop bank, the return bus, the song timeline,
  session I/O and the defaults. It knows nothing about JUCE and nothing about
  any sound card.
- `expr.c` — lexer, recursive-descent parser, bytecode emitter and VM. It
  depends on no other part of the project: a string in, a `Program` out, one
  `int32` sample per evaluation.
- `dsp.c` — the post-expression chain, integer only, called once per sample.
- `ret.c` — the four return effects, their shared pools, and the arithmetic
  that keeps a feedback patch bounded.
- `rack.c` — the source table (22 sources) and its renderer. Adding a source
  is adding one row to `SRC[]`.
- `gen.c` — the procedural patch generator, which exists because random
  bytebeat is almost always silence.
- `knob.c` — knob ladders and units: the values at which a parameter actually
  changes the sound.
- `bb_platform.c` — the platform seam. The clock, the paths and the rest of
  what this project asks an operating system for go through it, and it is
  where a new platform gets taught. It is not, however, the only file that
  knows which OS this is: `engine.c` carries four deliberate `_WIN32` blocks
  of its own, for the include it needs, for path joining, for the config
  directory search and for the flush-to-medium call. It argues for them in the
  comment at the head of the file (`engine.c:36`). Read that argument before
  moving any of them behind the seam.
- `bytebeat.h` — the memory the UI thread and the audio thread share.
  `bb_atomic.h` gives C++ a `std::atomic<>` view of the same bytes.
- `app/` — the JUCE console, and the only front end. `app/AudioEngine.cpp`
  owns JUCE's `AudioDeviceManager`, pushes every hardware buffer straight into
  `bb_engine_render`, and records WAVs off the engine's own sink ring. It
  mixes NOTHING on top of the engine, and that is load-bearing rather than
  incidental: it used to add the GRAIN MASS wells into the device buffers
  after the render returned, which put them downstream of `bb.sink` and so
  outside the recorder, the meter, the scope and every looper. Anything
  audible has to go through `bb_engine_render`. `app/Main.cpp` is the shell and the 30 Hz
  timer; `app/panels/` is one file per workspace, plus `Chrome.cpp` for the
  title bar, tabs, locker, scope, transport and status line, and
  `FieldManual.cpp` for the `?` overlay.
- `tests/engine_tests.c` — the regression suite, built as `morgue-tests`.
  Links no JUCE and no sound card, so the engine is provable on any machine
  with a C compiler and with `third_party/JUCE` completely empty.
- `examples.h` — compiled by nothing. It is kept because it is the only place
  the per-example bpm/beats/bars live.

## Build and test

The same four lines on all three platforms; substitute the preset.

```sh
git submodule update --init --recursive          # JUCE 8.0.15
cmake --preset windows-msvc-relwithdebinfo       # or linux-gcc- / linux-clang- / macos-clang-
cmake --build --preset windows-msvc-relwithdebinfo
ctest --preset windows-msvc-relwithdebinfo
```

Expect from the suite:

```text
2967 historical checks, 41 port checks, 106 return-bus checks, 120 loop-bank checks, 8 gate checks, 31 well checks
all 3273 checks passed (22 sources, session v7, reads v2+)
```

Those groups are counted separately on purpose: each one contains a check that
exists to notice a specific regression, and a moving total would let it hide.

The historical group moved from 2966 to 2967 when GRAIN MASS went into the
engine: the check pinning ARRANGE's refusal to capture lane 9 became a check
that lane 9 captures, and it was joined by one asserting the lane bound past it
is still enforced. That is the only time that number has moved, and it moved
because the behaviour it pinned was deliberately changed.

There is no `make`, no `make test`, no `./bytebeat` and no `bytebeat -T`. The
suite still accepts `-T` so muscle memory and any carried-over CI invocation
keep working, but the binary is `morgue-tests` and `ctest` is the documented
route.

While working on the engine, skip JUCE entirely — build presets
`windows-msvc-relwithdebinfo-engine`, `linux-gcc-relwithdebinfo-engine` and
`macos-clang-relwithdebinfo-engine` build only `bbengine` and `morgue-tests`.

Requires CMake ≥ 3.22 (JUCE 8.0.15's own floor) and a C11 + C++17 toolchain.
Every preset uses the Ninja generator, including on Windows: the Visual Studio
generator drives MSBuild, MSBuild loads whatever is integrated user-wide on the
machine, and on the box this was ported on a broken vcpkg integration failed a
build that does not use vcpkg at all.

- **Windows** — Visual Studio 2022 **17.5 or newer**. C11 atomics need
  `/experimental:c11atomics`, which arrived in toolset 19.35; `CMakeLists.txt`
  checks the version and fails with that sentence rather than letting it become
  a `C1189` deep in a header. Configure from an *x64 Native Tools Command
  Prompt* — that is what puts `cl.exe`, the SDK and VS's own `ninja.exe` on
  PATH. Keep the build directory path SHORT: JUCE's intermediate paths are long
  enough that a deep one exceeds `MAX_PATH` and dies with an opaque `C1083`.
- **macOS** — Xcode command line tools. Nothing else.
- **Linux** — JUCE's system deps: `libasound2-dev libfreetype6-dev
  libfontconfig1-dev libx11-dev libxcomposite-dev libxcursor-dev libxext-dev
  libxinerama-dev libxrandr-dev libxrender-dev libglu1-mesa-dev
  mesa-common-dev`. No webkit; the app sets `JUCE_WEB_BROWSER=0`.

CI runs the engine and the suite on four toolchains (gcc, clang, Apple Clang,
MSVC) and builds the console on all three OSes: `.github/workflows/build.yml`.

## Architecture conventions

- **The engine is C11 with hard real-time rules.** `bb_engine_render` never
  allocates, never locks, never blocks. Scalar state crosses as atomics;
  anything larger — programs, samples, clips, songs — is published by a single
  atomic pointer swap and reclaimed only after two render epochs.
- **`bytebeat.h` is read by C11 and by C++17.** Spell atomics `BB_ATOMIC(T)`,
  never `_Atomic` or `atomic_int`, in anything a C++ translation unit can see.
  `_Atomic` in C++ is a Clang extension, and that single mistake made this
  project build on exactly one compiler for its whole life. Declarations go
  inside the one `extern "C"` block; `#include`s go above it.
- **No negative left shifts.** Shift through the matching unsigned type. MSVC
  has no `-fwrapv` and the code deliberately no longer depends on it.
- **Anything that needs bar AND step reads `bb.pos`.** The separate `bb.bar`
  and `bb.seq_pos` are two stores, and reading both tears once per bar, which
  is what made the playhead appear to jump backwards on every loop pass.
- **The 30 Hz sync contract.** `MainComponent`'s timer calls `sync()` on rack,
  mixer, survivor, transport, licks, arrange, exhume and plate at 30 Hz. It is
  not the only timer: GRAIN MASS, the scope and a few others start their own at
  30 Hz, and HW/SYNC deliberately runs at 15 Hz because it drives a telemetry
  line rather than a control surface. What the console guarantees is that a
  panel is pulled often enough to read as live, not that one timer does the
  pulling. `sync()` pulls FROM the
  engine, skipping any control the user is currently dragging
  (`isUserDragging` guards). The engine is the single source of truth; a panel
  holds no shadow copy of engine state, and UI edits write straight to the
  atomics.
- **No control that does not move an engine value.** A "planned" control drawn
  greyed out is the same lie in a lighter ink, so it does not ship. The
  painted-but-dead plates, matrices and meters that had accumulated were
  cleared out once and do not come back. If a feature does not exist, the
  panel says so in words (see `ExportSheet.h`).

### Adding an engine feature

1. Put the state in `bytebeat.h`, spelled `BB_ATOMIC(T)`.
2. Consume it in `engine.c`, inside the real-time rules above.
3. Expose the API in `engine.h`. That header is the seam; the console reaches
   the engine through nothing else.
4. Add checks to `tests/engine_tests.c`. It links no JUCE, so the feature is
   provable before any panel exists.
5. Only then wire a control in `app/panels/`.

## The console

Ten workspace tabs, in tab order (`StageTabs`, `app/panels/Chrome.cpp`):

| Workspace | What it does |
|---|---|
| **RACK** | The voice station. Voice focus (keys 1-8, shift+1-8 toggles a layer), the 22-source grid, 16 curated patches, ROLL/MUTATE, the live expression editor (RETURN compiles; a failed compile keeps the old program running), p0-p7 with roles inferred from the compiled bytecode, the five VOICE DESIGN macros, SCULPT with STEP BACK, the post chain, and the 16-step sequencer with its parameter-lock lane and its **SEQ** switch. |
| **ARRANGE** | The song timeline: 64 bars, 10 lanes (8 voices, LICKS, MASS), clips scheduled in absolute bars. Per-lane CAPTURE on every lane, MASS included since the wells moved into the engine. PLACE from the locker, move/re-lane/trim/loop, ruler click to seek, and its own PLAY/STOP independent of master RUN. `REC: OVERDUB` (`BB_REC_LIVE`) prints everything except the arrangement's own playback, so a take does not stack the backing on every pass. |
| **GRAIN LICKS** | The step sampler: 8 slots × 16 steps on the engine's clock, per-step pitch and velocity, choke groups, mute/solo. One 16-step pattern per slot; there is no pattern bank to switch between. |
| **GRAIN MASS** | Four sample wells: load anything, pitch it, reverse it, loop it. PLAY ALL starts every well together on the next bar. The wells are engine voices (`bb.well[]`), summed inside `bb_engine_render` beside the LICKS bus, so REC records them, SURVIVOR loops them, the meter and scope see them and ARRANGE's lane 9 captures them. Loads by double-click, by a drop from the desktop, or by a drag out of the LOCKER. |
| **SURVIVOR** | The loop bank: six bar-synced loopers. Slot 0 IS the master phrase looper, reached through the bank API by an alias. Slots 1-5 record `BB_LOOP_SRC_LIVE` — the bus at the input of the loop stage, which contains no looper's playback — so layers stack without recording each other. |
| **MIXER** | Faders, mutes and meters, plus the return bus: eight ad-hoc slots (CHAMBER, DELAY, DRIVE, CHOIR), the 12×8 send matrix (eight voices, LICKS, DRY, WET, MASS) and the 8×8 return→return link grid. One live send knob per strip into the focused return, with the full matrix drawn small in the routing dock. Any link that closes a cycle is drawn in blood and named in the footer, because the loop should be visible before it screams. |
| **HW/SYNC** | MIDI in, and only what is wired: note on/off trigger and re-pitch the focused voice, CC 1 rides p0. The engine reads no MIDI clock, so this panel shows none. |
| **EXHUME** | archive.org acquisition: search, audition and fetch into the locker, md5-verified, ffmpeg-transcoded, carrying licence and provenance. It is a faithful port of `tools/exhume.py`; read the script before "simplifying" anything here. |
| **PLATE** | The visual wing: a watched INTAKE folder, and generation loss as a seeded, reproducible operator chain. It shells out to `tools/degrade.py` rather than owning ffmpeg command construction twice. |
| **EXPORT** | A sheet over the dimmed console. The engine has no offline renderer, so the sheet says exactly that and offers one control that works. It is the last big missing feature, not a broken one. |

`?` or F1 opens the field manual over whatever is showing.

**SEQ arms the layer, and a struck source is silent without it.** `engine.c`
gates every trigger behind `if (sn->seq_on)`, so on a layer whose `seq_on` is
0 the sixteen cells latch, light and fire nothing. THUMP is
`bp(tr*vel*4096,p0,p1)`, where the trigger IS the entire input, so picking it
from the SOURCE grid used to produce exact digital silence rather than
something merely quiet. `test_seq_gate` in `tests/engine_tests.c` is the eight
checks that pin that distinction: an unarmed struck source must render a peak
of exactly 0 and an armed one must render above 0, while a free-running source
must sound either way, so the switch can never quietly become a master mute.
The retired terminal UI armed the layer silently as a side effect of choosing
the source, which is why nobody hit this until it was gone. RACK now carries
the switch, and throwing it over an empty grid seeds `E(4,16)` so there is
something to hear; an existing pattern is never overwritten. The measurement
behind all of this, and the fuller case for putting the switch on the panel,
sit in the comment beside the code in `app/panels/RackPanel.cpp`. The figure
is deliberately not repeated here: nothing pins it, so a number in a markdown
file goes quietly false the day the DSP moves.

## Screenshot (permission-free self-render)

The console renders its own window offscreen and quits, so no screen-recording
permission is involved anywhere.

```sh
# Windows
build/<preset>/MORGUE_artefacts/<config>/MORGUE.exe --screenshot
# Linux
build/<preset>/MORGUE_artefacts/<config>/MORGUE --screenshot
# macOS
build/<preset>/MORGUE_artefacts/<config>/MORGUE.app/Contents/MacOS/MORGUE --screenshot
```

`--screenshot` writes `morgue_render.png` into the current working directory.
`--screenshot=NAME` selects a view first and writes `morgue_NAME.png`. The
accepted names are the ones in `selectView` (`app/Main.cpp`):

```text
rack  arrange  licks  mass  survivor  mixer  hwsync  exhume  plate  export  manual
```

The shorter list in the comment above `initialise()` predates EXHUME and PLATE.
An unrecognised name is ignored rather than reported, and you get the default
view under that filename, so check the spelling before believing a render.
`docs/morgue_rack.png` and its siblings are named exactly as
`--screenshot=NAME` writes them.
