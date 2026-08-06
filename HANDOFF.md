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

Expect: `2967 historical checks, 41 port checks, 106 return-bus checks, 120
loop-bank checks, 8 gate checks, 31 well checks / all 3273 checks passed`.

Windows needs VS 2022 **17.5+** (C11 atomics) and a **short build directory** —
JUCE's intermediate paths are long enough that a deep one exceeds `MAX_PATH`
and dies with an opaque `C1083`. `C:\Users\<you>\mb` works.

Windows also needs the **MSVC developer environment already in the shell**.
The Ninja generator invokes `cl.exe` directly and inherits `INCLUDE`/`LIB`
from the environment rather than baking them into `build.ninja`, so a build
started from a plain shell dies on `fatal error C1083: Cannot open include
file: 'algorithm'` — which reads like a broken JUCE checkout and is not.
Run `vcvars64.bat` first:

```sh
cmd /c '"C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" && cmake --build C:\Users\<you>\mb'
```

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
   unreachable. The suite calls `bb_engine_init()` many times per run, so
   clearing the pool is not affordable either; the measured count lives in one
   place, beside `g_sat_buf` in `engine.c`.

---

## What is done

| | |
|---|---|
| Cross-platform | Windows/MSVC, macOS/Clang, Linux/gcc **and** clang. CI matrix in `.github/workflows/build.yml`. |
| Loop bank | Six bar-synced loopers. Satellites record `LIVE` — never another looper. |
| Return bus | Eight ad-hoc slots (CHAMBER/DELAY/DRIVE/CHOIR), 12×8 send matrix, 8×8 link grid, per-slot limiter. Every link one sample old. |
| Arrangement | Own PLAY/STOP; `BB_REC_LIVE` records without printing the arrangement into the take. |
| EXHUME | archive.org search/audition/fetch, md5-verified, ffmpeg transcode, provenance + clearance. |
| PLATE | Watched INTAKE folder, seeded reproducible generation loss via ffmpeg. |
| Ledger | Real serials, append-only register, `derived_from` ancestry, credits export. |
| Autosave | Dirty-flagged, debounced, saves on focus loss. It previously only saved from a destructor. |
| UI | Contrast fixed by measurement; every dead control and fake meter removed. |
| One front end | The ncurses/ALSA terminal instrument is deleted. The JUCE GUI is the only front end; the engine is a static lib both it and the suite link. |
| RACK SEQ | The 16-step grid has a switch. Without it a layer whose `seq_on` was clear had a sixteen-cell editor that could never fire. |
| One sampler path | GRAIN MASS wells are engine voices (`bb.well[]`), summed beside the LICKS bus. `SamplerVoice` is deleted and nothing mixes on top of `bb_engine_render` any more. REC records a well, SURVIVOR loops it, ARRANGE lane 9 captures it, and it has a MASS column in the send matrix. |

---

## What is NOT done, roughly in the order I would do it

### 1. EXPORT / stem rendering (R6)
The sheet honestly says the engine cannot do it. It is the last big **missing**
feature rather than a broken one. Render each voice, slot, well and return in
isolation offline — `bb_engine_render_specimen_voice()` and the per-lane
capture path are the precedents. This is what gets a finished record out.

The sampler merge this used to depend on is **done** (2026-08-05): the GRAIN
MASS wells are engine voices now, so there is a source for the engine to render
a stem of. See "What is done" above.

### 1b. Condense the console, 10 tabs → 6
Decided 2026-08-05. Ten workspaces is more than the instrument has ideas, and
the overlaps are real code duplication rather than similar-looking screens:
three separate channel-strip implementations (MIXER faders, LICKS per-slot
level/mute/solo, GRAIN MASS per-well LEVEL) and two acquisition panels that are
not audio-path panels at all. The two SAMPLERS that used to head this list are
now one audio path, which is what makes the rest of it safe to do: a MIXER
strip for a well can finally read the same meter, in the same units, as the
strip beside it.

Target shape: **one SAMPLER** (wells and pattern grid as two views of the same
engine slots), **MIXER as the only place a level lives** — voices, sampler
slots, wells and returns on the same strip grammar — and **EXHUME + PLATE
folded into one INTAKE tab**. RACK, ARRANGE, SURVIVOR and EXPORT stay; HW/SYNC
becomes a settings sheet rather than a workspace.

The audio path has to move first. A MIXER strip for a well whose fader and
meter behave unlike every other strip on the same screen is exactly the kind of
control this console does not ship.

### 2. Cross-machine sync, Windows ↔ MacBook
Designed and critiqued in full; deliberately not built. **Build the small
version:** a `.morgue` project folder with **relative** asset refs, an importer
for the existing `~/MORGUE/session.conf`, and per-OS data directories — then
point Syncthing or an external SSD at it. Content-addressed storage, an S3
vtable and a merge UI are the right answer to a problem that does not exist
yet. Two real bugs to fix in passing: `aclip` stores absolute paths, and
GRAIN MASS well filenames and sampler slot names never touch disk at all.

Well CONTROLS (level, pitch, loop, reverse, mute) now persist as `well` lines.
The sample PATH deliberately does not, and this is the place to add it: writing
one today would mean writing an absolute one, which is the very defect `aclip`
already has. One absolute path in a session file is a bug; two is a migration.

### 3. Desktop / window audio capture
Windows is tractable: WASAPI process loopback (Win10 20H1+) captures a specific
process. macOS needs ScreenCaptureKit (13+) or a virtual device like BlackHole.
Windows first.

### 4. Semantic sample search — PROVE IT BEFORE BUILDING IT
archive.org indexes what a recording is *about*, never what it sounds like.
"Dying transformer" will never match its metadata. The proposed answer is local
CLAP embeddings over the locker. **That is unproven on harsh material.** Spend a
weekend on a standalone Python script over the WAVs already in `~/MORGUE`, type
real queries at it, and see whether the ranking is any good. If it is not, most
of the acquisition roadmap loses its reason to exist. If it is, that script is
the feature for months.

### 5. The visual wing, beyond PLATE
Same discipline: before writing more code, do the physical version. Photocopy a
print forty times, scan a photo held above the platen, drag a phone flash
alongside the scanner head. Compare against `tools/degrade.py`'s output. One
non-obvious hardware fact that decides whether the practice works at all: buy a
**CCD** flatbed, not CIS — a CIS bar has millimetres of depth of field, so a
photo lifted off the platen just goes black.

### 6. Still `PLANNED` in DESIGN_SPEC.md
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
- **A struck source with `seq_on` clear is not quiet, it is exactly silent.**
  `thump` is `bp(tr*vel*4096,p0,p1)` — the trigger is its whole input, and the
  engine gates every trigger behind `if (sn->seq_on)`. Measured: peak 0 unarmed
  against 4331 armed, over four seconds. `feedback` and `rumble` survived but
  came out 21× and 2.3× down, and the rest of the rack was unaffected, which is
  exactly why nobody noticed. If a layer makes no sound, check `seq_on` before
  suspecting the DSP.
- **The Makefile had not linked for some time and nothing said so.** Its `OBJS`
  omitted `bb_platform.o` while `engine.c` called `bb_now_us()` ten times. A
  second build system that no one runs and CI does not cover rots in silence
  and then gets cited in a decision as though it worked.
- **A decision applied to one of the two things it applied to.** R1's notes
  put the step sampler's audio in the engine "because REC and SURVIVOR capture
  the engine's master bus; a JUCE-only mixer would sit outside the
  sink/looper", and then left GRAIN MASS as a JUCE-only mixer. The reasoning
  was right, written down, and half-applied, so the bug shipped with its own
  explanation attached and survived several milestones — audible on the
  speakers, absent from every recording. The suite could not have caught it:
  `morgue-tests` links no JUCE, so the wells were literally unreachable from
  the only thing that checks anything. **If a rule explains why a subsystem
  belongs somewhere, check every subsystem it names.**
- **Deleting a file orphans the comments that point at it.** The pitch table in
  `engine.c` said "see audio.c's comment" — and `audio.c` had never contained
  it. A cross-reference nothing checks is a claim nothing checks. Inline the
  explanation instead of pointing at it.

---

## Design notes rescued from `sink.c` before it was deleted

`sink.c` served finished samples over TCP so you could listen on a laptop while
the instrument ran on a headless box over ssh. It was POSIX-only, was never
compiled by CMake (only by the already-broken Makefile), and no current
workflow wants it, so it went. Three things in it were worth more than the
code, and would otherwise have to be rediscovered:

- **Each consumer gets its OWN read cursor into the sink ring.** A stalled
  network client must never be able to corrupt or delay the `.wav` you are
  recording, and one shared cursor is precisely how that happens. The JUCE
  `WavRecorder` already follows this rule with a private `ringRead`.
- **You can tell a browser from `nc` without asking.** A fresh client sits in a
  probe state; a browser sends `GET` immediately, a raw listener sends nothing.
  After ~15 service ticks of silence, treat it as raw PCM.
- **A stream has no length, so write `0xffffffff` into both WAV size fields.**
  Players read that as "keep going until the socket closes". Serving a bare
  `audio/wav` at `/` also does the wrong thing: browsers download it forever
  instead of playing it, so `/` must return a page with an `<audio>` element
  pointing at the stream.

If it is ever wanted again, write it against `juce::StreamingSocket` and the
existing `bb.sink` ring rather than reviving the C. That class already handles
`WSAStartup`, `TCP_NODELAY` and non-blocking sockets on all three platforms;
the old file's `MSG_NOSIGNAL`, `MSG_DONTWAIT`, `fcntl(O_NONBLOCK)`,
`getifaddrs` and `int`-typed file descriptors would each have needed replacing
before it compiled on Windows at all.
