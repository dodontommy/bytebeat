# HANDOFF — MORGUE

Engineering state, for whoever builds next. Written 2026-08-05, on `main`,
with CI green on Windows, macOS and Linux.

---

## Build it

```sh
git submodule update --init --recursive          # JUCE 8.0.15, ~94 MB
cmake --preset windows-msvc-relwithdebinfo       # or linux-gcc- / linux-clang- / macos-clang-
cmake --build --preset windows-msvc-relwithdebinfo
ctest  --preset windows-msvc-relwithdebinfo
```

Expect: `2966 historical checks, 41 port checks, 106 return-bus checks, 120
loop-bank checks / all 3233 checks passed`.

Windows needs VS 2022 **17.5+** (C11 atomics) and a **short build directory** —
JUCE's intermediate paths are long enough that a deep one exceeds `MAX_PATH`
and dies with an opaque `C1083`. `C:\Users\<you>\mb` works.

`morgue-tests` does not link JUCE on purpose: the engine can be proven with the
submodule absent.

---

## Invariants. Break these and the instrument is subtly wrong

1. **`bb_engine_render()` never allocates, locks or blocks.** Everything
   published to it is either an atomic or one pointer swapped in, reclaimed
   two epochs later.
2. **`bytebeat.h` is read by C11 and C++17.** Spell atomics `BB_ATOMIC(T)`,
   never `_Atomic` or `atomic_int`, in anything a C++ TU can see. `_Atomic` in
   C++ is a Clang extension — that single mistake made the project build on
   exactly one compiler for its whole life. Declarations go inside the one
   `extern "C"` block; `#include`s go above it.
3. **Clips are summed AFTER the chamber.** `BB_REC_LIVE` depends on it: no clip
   feeds the reverb, so dropping the arrangement from a recording cannot strand
   a wet tail. Move the clip sum above the chamber and that guarantee dies.
4. **The record path forks BEFORE the master clamp.** `dsp_clip16` is not
   linear; subtracting after it is wrong exactly when the bus is hottest.
5. **Anything needing bar AND step reads `bb.pos`.** The separate `bb.bar` and
   `bb.seq_pos` are two stores; reading both tears once per bar, which is what
   made the playhead appear to jump backwards on every loop pass.
6. **Return slot 0 is the CHAMBER and loop slot 0 is SURVIVOR** — aliases, not
   copies. Both exist so a legacy session renders bit-identically. There are
   golden-hash checks pinning it; if a refactor moves the hash, fix the code.
7. **No negative left shifts.** Shift through the matching unsigned type. MSVC
   has no `-fwrapv` and the code deliberately no longer depends on it.
8. **Session keys are added without a format-version bump.** The loader skips
   unknown keys. An old binary that rejected a bumped version would still
   autosave on quit and write the song back out without them — that is the
   data-loss path a bump would open.
9. **The satellite loop pool is never memset.** Reads are `idx % len` and every
   index below `len` was written during that capture, so residue is
   unreachable. `bb_engine_init()` is called seven times by the suite.

---

## What is done

| | |
|---|---|
| Cross-platform | Windows/MSVC, macOS/Clang, Linux/gcc **and** clang. CI matrix in `.github/workflows/build.yml`. |
| Loop bank | Six bar-synced loopers. Satellites record `LIVE` — never another looper. |
| Return bus | Eight ad-hoc slots (CHAMBER/DELAY/DRIVE/CHOIR), 11×8 send matrix, 8×8 link grid, per-slot limiter. Every link one sample old. |
| Arrangement | Own PLAY/STOP; `BB_REC_LIVE` records without printing the arrangement into the take. |
| EXHUME | archive.org search/audition/fetch, md5-verified, ffmpeg transcode, provenance + clearance. |
| PLATE | Watched INTAKE folder, seeded reproducible generation loss via ffmpeg. |
| Ledger | Real serials, append-only register, `derived_from` ancestry, credits export. |
| Autosave | Dirty-flagged, debounced, saves on focus loss. It previously only saved from a destructor. |
| UI | Contrast fixed by measurement; every dead control and fake meter removed. |

---

## What is NOT done, roughly in the order I would do it

### 1. Retire the TUI (small, and it is already half done)
`main.c`, `ui.c`, `audio.c`, `sink.c` and the `Makefile` are still in the tree.
The valuable part — the regression suite — was extracted to `tests/` already,
so this is deletion plus a docs pass. `docs/TUI.md` goes with it. Decide
whether `sink.c`'s raw-PCM-over-TCP is worth keeping as a portable feature
before deleting it; nothing else in there is.

### 2. EXPORT / stem rendering (R6)
The sheet honestly says the engine cannot do it. It is the last big **missing**
feature rather than a broken one. Render each voice, slot and return in
isolation offline — `bb_engine_render_specimen_voice()` and the per-lane
capture path are the precedents. This is what gets a finished record out.

### 3. Cross-machine sync, Windows ↔ MacBook
Designed and critiqued in full; deliberately not built. **Build the small
version:** a `.morgue` project folder with **relative** asset refs, an importer
for the existing `~/MORGUE/session.conf`, and per-OS data directories — then
point Syncthing or an external SSD at it. Content-addressed storage, an S3
vtable and a merge UI are the right answer to a problem that does not exist
yet. Two real bugs to fix in passing: `aclip` stores absolute paths, and
GRAIN MASS well filenames and sampler slot names never touch disk at all.

### 4. Desktop / window audio capture
Windows is tractable: WASAPI process loopback (Win10 20H1+) captures a specific
process. macOS needs ScreenCaptureKit (13+) or a virtual device like BlackHole.
Windows first.

### 5. Semantic sample search — PROVE IT BEFORE BUILDING IT
archive.org indexes what a recording is *about*, never what it sounds like.
"Dying transformer" will never match its metadata. The proposed answer is local
CLAP embeddings over the locker. **That is unproven on harsh material.** Spend a
weekend on a standalone Python script over the WAVs already in `~/MORGUE`, type
real queries at it, and see whether the ranking is any good. If it is not, most
of the acquisition roadmap loses its reason to exist. If it is, that script is
the feature for months.

### 6. The visual wing, beyond PLATE
Same discipline: before writing more code, do the physical version. Photocopy a
print forty times, scan a photo held above the platen, drag a phone flash
alongside the scanner head. Compare against `tools/degrade.py`'s output. One
non-obvious hardware fact that decides whether the practice works at all: buy a
**CCD** flatbed, not CIS — a CIS bar has millimetres of depth of field, so a
photo lifted off the platen just goes black.

### 7. Still `PLANNED` in DESIGN_SPEC.md
R3 automation lanes, R9 undo/project versions. Both real, neither blocking.

---

## Things that bit, so they do not bite again

- **The bug was never where I predicted.** Ten failing checks on the return bus
  looked like arena residue; the cause was the per-voice level ramp surviving
  `bb_engine_init()` — measured at 51200 vs 0 at the same point in two
  supposedly identical runs. Instrument before theorising.
- **`DARK` was inverted on three effects** (0 was darkest). Inside DELAY's
  feedback loop that turned a self-linked delay into a ~1 Hz oscillator:
  limiter pinned, meters lit, nothing audible.
- **`dsp_dc()` could not reach silence.** Its feedback used an arithmetic right
  shift, which rounds toward minus infinity, so a state that decayed to −1
  stayed −1 forever.
- **Four of the failures were test bugs**, each found by instrumenting, not by
  assuming: a reverb tail that legitimately outlives its input; an "empty
  routing graph" assertion contradicting the line above it; and a `sscanf` that
  writes its arguments before the `n == 0` guarding it is evaluated, so it
  compared the wrong slot.
- **Hex escapes are greedy.** `"\xe2\x86\x92ARR"` is the single escape `\x92A`,
  out of range. Split the literal.
- **Strict `-std=c11` defines `__STRICT_ANSI__`**, and glibc then hides
  `clock_gettime`, `fileno`, `fsync`, `getpid`, `rmdir`. `_GNU_SOURCE` on
  Linux, `_DARWIN_C_SOURCE` on macOS. Compiles fine on Windows either way,
  which is how it hides.
- **vcpkg is integrated user-wide into MSBuild on this machine and is broken**
  (`'pwsh.exe' is not recognized`). Use the Ninja generator.
