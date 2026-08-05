#!/usr/bin/env python3
"""
DEGRADE -- generation loss for MORGUE.

The visual counterpart of the analog audio loops: send a signal out into the
world, let a machine abuse it, bring it back, send it out again. The reference
processes are the ones that actually leave a mark:

    repeated photocopying at high exposures
    scanning a photo held up high above the scanner
    dragging a phone flash along with the scanner light

None of those are filters. Every one of them is a recursive, lossy, physical
pass -- f(f(f(x))) -- where the loss compounds because the machine reads its
own previous output. So this script does not "apply an effect". It runs a
chain of operators N times, each pass reading the file the last pass wrote,
and it writes EVERY generation to disk so the ladder can be looked at and a
generation chosen, rather than a number guessed.

    python tools/degrade.py ops
    python tools/degrade.py recipe --out PLATE.recipe
    python tools/degrade.py run --src scan.jpg --out ~/MORGUE/PLATES/TEST \
                                --passes 24 --seed 9F3C21AB
    python tools/degrade.py sheet --dir ~/MORGUE/PLATES/TEST --out sheet.jpg

Requires: Python 3.9+, ffmpeg and ffprobe on PATH. No third-party packages.
This is deliberately a standalone script, exactly as tools/exhume.py is: it
runs tonight without a build, it is how the ffmpeg chains get tested without a
GUI, and the PLATE panel in the app SHELLS OUT TO THIS FILE rather than
duplicating the command construction in C++. One implementation, one place to
fix a filter string, and a terminal to test it from.

--------------------------------------------------------------------------
THREE THINGS THIS SCRIPT IS BUILT AROUND
--------------------------------------------------------------------------

1. MORGUE ORCHESTRATES; FFMPEG PUSHES THE PIXELS. There is no raster code in
   here and none in the app. ffmpeg 8.x ships scale, eq, curves, noise, gblur,
   unsharp, rotate, pad, crop, convolution, erosion, hue and geq, plus mjpeg
   quantiser control, and that is the whole palette this needs. (ImageMagick is
   NOT a dependency and will not become one.)

2. DETERMINISM IS MANDATORY. "Run it again with 40 passes" has to be a
   one-field change that produces the same 40 plates, or the contact sheet is
   a lottery ticket rather than a ladder. So every random number comes from a
   stream keyed by (master_seed, pass_index, operator_index) through SHA-256 --
   NOT from a single sequential generator, because a single stream makes pass
   17 depend on how many operators pass 3 happened to have enabled. Adding an
   operator would silently rewrite the whole ladder below it. Keyed streams
   mean an operator's noise depends only on where it sits, so a recipe edit
   changes what you edited and nothing else. ffmpeg's own randomness is pinned
   too: the noise filter takes all_seed, and it is always supplied.

3. FILES ON DISK, NEVER PIPES. Every pass reads a file and writes a file.
   This is not just the recursion being literal -- juce::ChildProcess (which
   the app drives this script with) cannot write to a child's stdin at all, so
   any design that piped frames between processes could not be driven from the
   panel. Each generation being a real file on disk is also what makes the
   contact sheet, the ledger record and the round trip possible.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import random
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

VERSION = 1

# --------------------------------------------------------------------------
# Tools
# --------------------------------------------------------------------------


def ffmpeg_bin() -> str:
    exe = os.environ.get("MORGUE_FFMPEG") or shutil.which("ffmpeg")
    if not exe:
        sys.exit("ffmpeg not found. Install it or set MORGUE_FFMPEG.")
    return exe


def ffprobe_bin() -> str:
    exe = os.environ.get("MORGUE_FFPROBE") or shutil.which("ffprobe")
    if not exe:
        # ffprobe ships beside ffmpeg in every real distribution
        guess = Path(ffmpeg_bin()).with_name("ffprobe" + (".exe" if os.name == "nt" else ""))
        if guess.exists():
            return str(guess)
        sys.exit("ffprobe not found. Install it or set MORGUE_FFPROBE.")
    return exe


def run_tool(cmd: list[str]) -> str:
    """Run a tool, raise with its stderr attached. No shell, ever: real
    filenames carry spaces, brackets and non-ASCII, and a shell would need
    quoting rules that differ between cmd.exe and sh."""
    p = subprocess.run(cmd, capture_output=True, text=True, errors="replace")
    if p.returncode != 0:
        raise RuntimeError(
            "%s failed (%d)\n%s" % (Path(cmd[0]).name, p.returncode, p.stderr.strip()[-1200:])
        )
    return p.stdout


def probe_size(src: Path) -> tuple[int, int]:
    out = run_tool([
        ffprobe_bin(), "-v", "error", "-select_streams", "v:0",
        "-show_entries", "stream=width,height", "-of", "json", str(src),
    ])
    streams = json.loads(out).get("streams") or []
    if not streams:
        raise RuntimeError("no image stream in %s" % src)
    return int(streams[0]["width"]), int(streams[0]["height"])


def sha256_of(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# --------------------------------------------------------------------------
# Determinism. One stream per (master seed, pass index, operator index).
# See point 2 in the module docstring for why this is not one generator.
# --------------------------------------------------------------------------
def stream(master: int, pass_index: int, op_index: int) -> random.Random:
    key = "MORGUE-DEGRADE/%d/%08X/%d/%d" % (VERSION, master & 0xFFFFFFFF, pass_index, op_index)
    digest = hashlib.sha256(key.encode("utf-8")).digest()
    return random.Random(int.from_bytes(digest[:8], "big"))


def jittered(rnd: random.Random, value: float, jitter01: float, spread: float) -> float:
    """Perturb a value by +-spread*jitter, deterministically."""
    if jitter01 <= 0.0:
        return value
    return value * (1.0 + rnd.uniform(-spread, spread) * jitter01)


# --------------------------------------------------------------------------
# OPERATORS
#
# Amounts are 0..255, because that is the range every knob in MORGUE turns
# through; a recipe and a panel therefore speak the same numbers. Each builder
# receives the amount normalised to 0..1, its own PRNG stream, and a context,
# and returns a list of ffmpeg filter strings. `requant` returns none -- it is
# the WRITE, not a filter, and it is the operator the whole idea rests on.
# --------------------------------------------------------------------------


class Ctx:
    def __init__(self, w: int, h: int, pass_index: int, passes: int, jitter01: float):
        self.w = w
        self.h = h
        self.pass_index = pass_index
        self.passes = passes
        self.jitter = jitter01

    @property
    def progress(self) -> float:
        """0 at the first pass, 1 at the last. Several operators get worse as
        the stack deepens, which is what generation loss IS."""
        return 0.0 if self.passes <= 1 else self.pass_index / float(self.passes - 1)


def op_resample(a: float, rnd: random.Random, c: Ctx) -> list[str]:
    """Generation softening. A copier scans at one resolution and prints at
    another; every trip through that pair throws away high spatial frequency
    and cannot put it back. Down and back up, bilinear both ways -- the
    cheapest filter is deliberate here, because the cheap filter is what the
    machine has."""
    f = max(0.12, 1.0 - 0.62 * a)
    f = jittered(rnd, f, c.jitter, 0.08)
    w = max(8, int(round(c.w * f)))
    h = max(8, int(round(c.h * f)))
    return ["scale=%d:%d:flags=bilinear" % (w, h),
            "scale=%d:%d:flags=bilinear" % (c.w, c.h)]


def op_exposure(a: float, rnd: random.Random, c: Ctx) -> list[str]:
    """"High exposures". The copier lamp is turned up, the whites blow out and
    never come back, and each generation starts from the blown copy. This is
    the operator that makes the ladder irreversible rather than merely soft."""
    bright = jittered(rnd, 0.015 + 0.155 * a, c.jitter, 0.35)
    contrast = jittered(rnd, 1.0 + 1.05 * a, c.jitter, 0.10)
    return ["eq=contrast=%.4f:brightness=%.4f" % (contrast, bright)]


def op_crush(a: float, rnd: random.Random, c: Ctx) -> list[str]:
    """Tonal crush toward pure black and white. Toner is binary: a photocopier
    has no midtones, it has dots. Steepening the transfer curve every pass is
    how a photograph becomes a diagram."""
    lo = 0.25 * (1.0 - a)
    hi = 0.75 + 0.25 * a
    return ["curves=all='0/0 0.25/%.3f 0.75/%.3f 1/1'" % (lo, hi)]


def op_grain(a: float, rnd: random.Random, c: Ctx) -> list[str]:
    """Additive grain: sensor noise, platen dust, toner speckle. Seeded, so
    the same recipe grows the same dirt."""
    s = int(round(jittered(rnd, 2.0 + 46.0 * a, c.jitter, 0.25)))
    s = max(1, min(100, s))
    return ["noise=alls=%d:allf=u:all_seed=%d" % (s, rnd.randrange(1, 2 ** 31 - 1))]


def op_blur(a: float, rnd: random.Random, c: Ctx) -> list[str]:
    """Scanner MTF. A real scanner's response is a soft roll-off followed by
    an edge overshoot, because the firmware sharpens what its optics lost --
    so this is a blur AND an unsharp, not a blur alone. The overshoot is the
    part that reads as 'scanned' rather than 'blurred'."""
    sigma = jittered(rnd, 0.15 + 1.7 * a, c.jitter, 0.20)
    out = ["gblur=sigma=%.3f:steps=1" % sigma]
    if a > 0.35:
        out.append("unsharp=5:5:%.3f:5:5:0.0" % (0.4 + 1.1 * a))
    return out


CONV_SCALE = 1024      # see _bilinear_shift_matrix


def _bilinear_shift_matrix(dx: float, dy: float) -> tuple[str, float]:
    """A 3x3 convolution kernel that translates by a FRACTION of a pixel.
    Returns (matrix string, rdiv).

    A sub-pixel shift is exactly a bilinear resample, and a bilinear resample
    over a 2x2 neighbourhood embeds in a 3x3 kernel with the other five taps
    zero. ffmpeg's convolution filter indexes the neighbourhood in raster
    order with element 4 at the centre, so a shift right/down by (dx,dy) takes
    weight from the up-left neighbours (0,1,3,4) and a shift up/left from the
    down-right ones (4,5,7,8).

    THE COEFFICIENTS MUST BE INTEGERS. This cost an hour and is worth writing
    down: ffmpeg's convolution filter parses each matrix cell with an INTEGER
    conversion, so a perfectly reasonable kernel like "0 0 0 0.3 0.7 0 0 0 0"
    is read as all zeros -- and the filter does not warn, it does not error,
    it returns a frame whose every plane is zero. In yuvj that decodes to a
    flat mid-green field, so the symptom is "my ladder turns solid green at
    generation 1" with a successful exit status and an empty stderr. Verified
    against ffmpeg 8.1.2: an identity kernel of 1s works, the same kernel
    written as 1.0 does not.

    So the weights are carried in fixed point, scaled by CONV_SCALE, and the
    scale is undone with rdiv -- which IS parsed as a float. Weights sum to
    CONV_SCALE, so rdiv = 1/CONV_SCALE and the image does not change
    brightness. 1024 steps of sub-pixel precision is far finer than any
    registration error worth simulating.

    This is why the drift operator can be honest about "sub-pixel": scale and
    crop both quantise to the pixel grid, and a registration error that snaps
    to whole pixels does not look like a sheet of paper that was 0.3 mm out."""
    m = [0.0] * 9
    fx, fy = abs(dx), abs(dy)
    ix = 3 if dx > 0 else (5 if dx < 0 else 4)   # horizontal neighbour taken from
    iy = 1 if dy > 0 else (7 if dy < 0 else 4)   # vertical neighbour taken from
    # corner index shares the row of iy and the column of ix
    corner = {(3, 1): 0, (5, 1): 2, (3, 7): 6, (5, 7): 8,
              (4, 1): 1, (4, 7): 7, (3, 4): 3, (5, 4): 5, (4, 4): 4}[(ix, iy)]
    m[4] += (1.0 - fx) * (1.0 - fy)
    m[ix] += fx * (1.0 - fy)
    m[iy] += (1.0 - fx) * fy
    m[corner] += fx * fy

    q = [int(round(v * CONV_SCALE)) for v in m]
    # Rounding nine cells independently can lose or gain a step; give the
    # remainder to the centre tap so the kernel still sums to CONV_SCALE and
    # the pass is exactly brightness-neutral.
    q[4] += CONV_SCALE - sum(q)
    return " ".join(str(v) for v in q), 1.0 / CONV_SCALE


def op_drift(a: float, rnd: random.Random, c: Ctx) -> list[str]:
    """Registration drift. Paper is fed by rubber rollers and lands a little
    crooked and a little off-centre, every single time, and the error
    accumulates because the next copy is made of the crooked one. Sub-pixel
    rotation (the rotate filter interpolates), whole-pixel translation
    (pad + crop) and sub-pixel translation (a bilinear convolution kernel).

    Lid-off black is the fill: a photocopier with the lid up returns BLACK at
    the edges, because there is nothing up there to bounce the lamp back."""
    ang = jittered(rnd, rnd.uniform(-1.0, 1.0) * a * 0.45, c.jitter, 0.5)
    dx = rnd.uniform(-1.0, 1.0) * a * 2.4
    dy = rnd.uniform(-1.0, 1.0) * a * 2.4

    out: list[str] = []
    if abs(ang) > 0.0005:
        out.append("rotate=a=%.6f*PI/180:c=black:bilinear=1" % ang)

    ix, iy = int(dx), int(dy)                       # whole pixels
    fx, fy = dx - ix, dy - iy                       # the rest
    if ix or iy:
        pad = max(2, abs(ix) + 1, abs(iy) + 1)
        out.append("pad=w=%d:h=%d:x=%d:y=%d:color=black"
                   % (c.w + 2 * pad, c.h + 2 * pad, pad + ix, pad + iy))
        out.append("crop=w=%d:h=%d:x=%d:y=%d" % (c.w, c.h, pad, pad))
    if abs(fx) > 0.01 or abs(fy) > 0.01:
        k, rdiv = _bilinear_shift_matrix(fx, fy)
        # No quotes: the matrix contains spaces, which the filtergraph parser
        # is perfectly happy with, and single quotes here would be taken as
        # part of the first coefficient.
        out.append("convolution=%s:%s:%s:%s:%.10f:%.10f:%.10f:%.10f"
                   % (k, k, k, k, rdiv, rdiv, rdiv, rdiv))
    return out


def op_flash(a: float, rnd: random.Random, c: Ctx) -> list[str]:
    """"Dragging your phone flash along with the scanner light."

    A second light source moving across the platen while the head moves: one
    bright band, in a different place every pass, that the next generation
    then treats as if it were part of the picture. Additive Gaussian band via
    geq -- the band is in image coordinates and the chroma planes are passed
    through untouched, which produces exactly the colour fringe a real
    off-colour light source leaves against a calibrated scanner white.

    geq is slow. On a 1600px plate it costs about a second a pass, and it is
    the single most characteristic operator here, so it is worth it."""
    centre = rnd.uniform(0.02, 0.98)
    width = 0.025 + 0.12 * a
    amp = 40.0 + 190.0 * a
    vertical = rnd.random() < 0.5
    axis = "(X/W)" if vertical else "(Y/H)"
    band = "%.1f*exp(-pow((%s-%.4f)/%.4f,2))" % (amp, axis, centre, width)
    return ["geq=lum='clip(lum(X,Y)+%s,0,255)':cb='cb(X,Y)':cr='cr(X,Y)'" % band]


def op_skew(a: float, rnd: random.Random, c: Ctx) -> list[str]:
    """"Scanning a photo held up high above the scanner" -- and the object
    moving while the head is mid-pass. A scanner does not take a picture, it
    takes a thousand one-pixel pictures over eight seconds; anything that
    moves in those eight seconds is sheared, not blurred. Per-row horizontal
    displacement, which is the actual geometry of that failure."""
    s = jittered(rnd, rnd.uniform(-1.0, 1.0) * a * 0.055, c.jitter, 0.4)
    if abs(s) < 0.0004:
        return []
    d = "%.5f*W*(Y/H-0.5)" % s
    return ["geq=lum='lum(X+%s,Y)':cb='cb(X+%s,Y)':cr='cr(X+%s,Y)'" % (d, d, d)]


def op_toner(a: float, rnd: random.Random, c: Ctx) -> list[str]:
    """Toner bloom: dark marks spread into their neighbours, thin lines close
    up, small white gaps fill in.

    ffmpeg's `dilation` grows the BRIGHT areas and `erosion` grows the dark
    ones. Ink is dark, so the toner operator is erosion. Naming it after the
    physical process rather than the morphological one is deliberate: nobody
    standing at a photocopier is thinking about structuring elements."""
    t = int(round(a * 255))
    if t <= 0:
        return []
    return ["erosion=threshold0=%d:threshold1=0:threshold2=0:threshold3=0:coordinates=255" % t]


def op_mono(a: float, rnd: random.Random, c: Ctx) -> list[str]:
    """Desaturation, partial or total. A photocopier is monochrome; a scanner
    is not. Left off in the default recipe so the choice stays the operator's."""
    if a <= 0.001:
        return []
    return ["hue=s=%.4f" % max(0.0, 1.0 - a)]


def op_requant(a: float, rnd: random.Random, c: Ctx) -> list[str]:
    """No filter. See quality_for_pass() -- this operator IS the write, and
    the write is where the generation is actually lost."""
    return []


def quality_for_pass(amount: int, ctx: Ctx, rnd: random.Random) -> int:
    """mjpeg -q:v, 2 (best) .. 31 (worst), DECLINING with pass index.

    Requantisation is the operator the whole idea rests on: every other filter
    here is a distortion the next pass could in principle partly undo, but a
    JPEG that has been quantised, dequantised and quantised again on a
    different 8x8 grid has lost coefficients that nothing downstream can
    reconstruct. Making it worse as the stack deepens is what a stack of
    photocopies does -- copy 40 is not 40 times worse than copy 1 by accident,
    it is worse because every copy started from a worse original."""
    a = amount / 255.0
    if a <= 0.0:
        return 2
    q = 2.0 + 10.0 * a + 19.0 * a * ctx.progress
    q = jittered(rnd, q, ctx.jitter, 0.06)
    return int(max(2, min(31, round(q))))


# name -> (builder, default amount, default enabled, one-line description)
OPS = {
    "skew":     (op_skew,     40,  False, "per-row shear: the object moved mid-pass"),
    "drift":    (op_drift,    70,  True,  "registration drift, sub-pixel rotate + translate"),
    "resample": (op_resample, 96,  True,  "generation softening: resample down and back up"),
    "blur":     (op_blur,     60,  True,  "scanner MTF: roll-off plus firmware overshoot"),
    "flash":    (op_flash,    0,   False, "a second light source dragged across the platen"),
    "exposure": (op_exposure, 120, True,  "high exposure: push contrast toward clipping"),
    "crush":    (op_crush,    70,  True,  "tonal crush toward pure black and white"),
    "toner":    (op_toner,    0,   False, "toner bloom: dark marks spread (erosion)"),
    "mono":     (op_mono,     0,   False, "desaturate, partially or fully"),
    "grain":    (op_grain,    46,  True,  "additive seeded grain: dust and speckle"),
    "requant":  (op_requant,  90,  True,  "mjpeg requantisation at declining quality"),
}

# The order the default recipe writes them in: geometry, then optics, then
# tone, then dirt, then the write. Editing the order in a recipe file is
# supported and changes the result -- blurring grain is not grainy blur.
DEFAULT_ORDER = ["skew", "drift", "resample", "blur", "flash",
                 "exposure", "crush", "toner", "mono", "grain", "requant"]


# --------------------------------------------------------------------------
# RECIPE FILE
#
# Same `key value` grammar as session.conf and ACCESSION.ledger, for the same
# reason: this project already has a config dialect, and a second one would be
# a second parser and a second set of escaping bugs. One line per operator, in
# chain order, so a diff of a recipe reads as a change to the chain.
# --------------------------------------------------------------------------
HEADER = """\
# MORGUE -- degradation recipe (degrade.py v%d).
# Plain text, edit it if you like. One `key value` pair per line; `#` comments.
#
#   seed    <hex>     master seed. Every operator's randomness derives from
#                     (seed, pass index, operator index) -- change this and
#                     you get a different ladder from the same chain.
#   passes  <n>       how many times the chain is applied to its own output.
#   jitter  <0-255>   how much each pass is allowed to deviate from the last.
#                     0 = every pass identical; the loss still compounds.
#   op <name> <amount 0-255> <enabled 0|1>
#                     one line per operator, IN CHAIN ORDER. Reorder the lines
#                     to reorder the chain. `degrade.py ops` lists them.
""" % VERSION


class Recipe:
    def __init__(self):
        self.seed = 0
        self.passes = 12
        self.jitter = 24
        self.ops: list[tuple[str, int, bool]] = []

    @staticmethod
    def default(seed: int | None = None, passes: int | None = None) -> "Recipe":
        r = Recipe()
        r.seed = random.SystemRandom().randrange(1, 2 ** 32) if seed is None else seed
        if passes is not None:
            r.passes = passes
        r.ops = [(n, OPS[n][1], OPS[n][2]) for n in DEFAULT_ORDER]
        return r

    @staticmethod
    def load(path: Path) -> "Recipe":
        r = Recipe()
        for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            key = parts[0]
            try:
                if key == "seed" and len(parts) >= 2:
                    # ALWAYS base 16. The writer emits "%08X", and a seed that
                    # happened to be all decimal digits would otherwise read
                    # back as a different number than it was written as --
                    # which would break reproducibility for exactly one seed in
                    # sixteen and be almost impossible to notice.
                    r.seed = int(parts[1], 16)
                elif key == "passes" and len(parts) >= 2:
                    r.passes = max(1, min(512, int(parts[1])))
                elif key == "jitter" and len(parts) >= 2:
                    r.jitter = max(0, min(255, int(parts[1])))
                elif key == "op" and len(parts) >= 3:
                    name = parts[1]
                    if name not in OPS:
                        # Forward compatibility: a recipe from a newer build
                        # keeps its unknown operators on the page rather than
                        # being silently rewritten without them.
                        sys.stderr.write("  ?? unknown operator '%s', ignored\n" % name)
                        continue
                    amount = max(0, min(255, int(parts[2])))
                    on = (len(parts) < 4) or parts[3] not in ("0", "off", "false")
                    r.ops.append((name, amount, on))
            except ValueError:
                sys.stderr.write("  ?? unparsable recipe line: %s\n" % line)
        if not r.ops:
            r.ops = [(n, OPS[n][1], OPS[n][2]) for n in DEFAULT_ORDER]
        return r

    def text(self) -> str:
        out = [HEADER, "version %d" % VERSION,
               "seed %08X" % (self.seed & 0xFFFFFFFF),
               "passes %d" % self.passes,
               "jitter %d" % self.jitter, ""]
        for name, amount, on in self.ops:
            out.append("op %-9s %3d %d" % (name, amount, 1 if on else 0))
        return "\n".join(out) + "\n"

    def save(self, path: Path) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(self.text(), encoding="utf-8")

    def digest(self) -> str:
        """A short stable fingerprint of the chain, seed included. Two runs
        with the same digest produced the same ladder; that is the whole
        claim, and it belongs in the ledger record."""
        body = "|".join("%s:%d:%d" % (n, a, 1 if o else 0) for n, a, o in self.ops)
        key = "%08X/%d/%d/%s" % (self.seed & 0xFFFFFFFF, self.passes, self.jitter, body)
        return hashlib.sha256(key.encode("utf-8")).hexdigest()[:16].upper()


# --------------------------------------------------------------------------
# THE LADDER
# --------------------------------------------------------------------------
def build_chain(recipe: Recipe, ctx: Ctx) -> tuple[list[str], int]:
    """Filters for one pass, plus the mjpeg quality that pass writes at."""
    filters: list[str] = []
    quality = 2
    for op_index, (name, amount, on) in enumerate(recipe.ops):
        if not on:
            continue
        if amount <= 0 and name != "requant":
            continue        # requant at 0 still writes, at the best quality
        rnd = stream(recipe.seed, ctx.pass_index, op_index)
        if name == "requant":
            quality = quality_for_pass(amount, ctx, rnd)
            continue
        builder = OPS[name][0]
        filters.extend(builder(amount / 255.0, rnd, ctx))
    return filters, quality


def render_pass(src: Path, dst: Path, thumb: Path, filters: list[str],
                quality: int, thumb_w: int) -> None:
    """One ffmpeg invocation: the full generation and its contact-sheet thumb.

    Both outputs come out of one decode via `split`, because the alternative
    is decoding and re-encoding every generation twice, and because a thumb
    made from a SECOND render would not be guaranteed to be a picture of the
    generation it sits under."""
    chain = ",".join(filters) if filters else "null"
    graph = "[0:v]%s,split=2[full][tsrc];[tsrc]scale=%d:-1:flags=bilinear[thumb]" % (
        chain, thumb_w)
    dst.parent.mkdir(parents=True, exist_ok=True)
    cmd = [ffmpeg_bin(), "-nostdin", "-v", "error", "-y", "-i", str(src),
           "-filter_complex", graph,
           "-map", "[full]", "-q:v", str(quality), "-frames:v", "1", str(dst),
           "-map", "[thumb]", "-q:v", "4", "-frames:v", "1", str(thumb)]
    run_tool(cmd)


def cmd_run(a) -> None:
    src = Path(os.path.expanduser(a.src)).resolve()
    if not src.is_file():
        sys.exit("no such image: %s" % src)
    out = Path(os.path.expanduser(a.out))
    out.mkdir(parents=True, exist_ok=True)

    recipe = Recipe.load(Path(os.path.expanduser(a.recipe))) if a.recipe else Recipe.default()
    if a.passes is not None:
        recipe.passes = max(1, min(512, a.passes))
    if a.seed is not None:
        recipe.seed = int(a.seed, 16)
    if a.jitter is not None:
        recipe.jitter = max(0, min(255, a.jitter))

    # The canvas is fixed for the whole ladder: every generation is the same
    # size, so the contact sheet is comparable and so `resample` measures a
    # real loss rather than a change of resolution. A 6000px scan is reduced
    # once, here, because forty passes over 36 megapixels is an hour.
    w, h = probe_size(src)
    if a.max_width and w > a.max_width:
        h = max(2, int(round(h * a.max_width / float(w))))
        w = a.max_width
    w -= w % 2
    h -= h % 2

    recipe.save(out / "PLATE.recipe")
    print("CANVAS %d %d" % (w, h), flush=True)
    print("DIGEST %s" % recipe.digest(), flush=True)
    print("SEED %08X" % (recipe.seed & 0xFFFFFFFF), flush=True)

    # gen-000 is the plate AS INGESTED: resized onto the canvas at the best
    # quality mjpeg offers, and nothing else. It is generation zero of the
    # ladder, not a degraded frame, and the sheet needs it to compare against.
    gen0 = out / "gen-000.jpg"
    render_pass(src, gen0, out / "thumb-000.jpg",
                ["scale=%d:%d:flags=bicubic" % (w, h)], 2, a.thumb)
    print("PASS 0 %d gen-000.jpg" % recipe.passes, flush=True)

    manifest = [
        "# MORGUE -- plate manifest (degrade.py v%d). Written by the run, read by the panel." % VERSION,
        "version %d" % VERSION,
        "utc %s" % time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "source %s" % src,
        "source_sha256 %s" % sha256_of(src),
        "canvas %dx%d" % (w, h),
        "seed %08X" % (recipe.seed & 0xFFFFFFFF),
        "passes %d" % recipe.passes,
        "jitter %d" % recipe.jitter,
        "digest %s" % recipe.digest(),
        "gen 000 gen-000.jpg %s" % sha256_of(gen0),
    ]

    prev = gen0
    for p in range(1, recipe.passes + 1):
        ctx = Ctx(w, h, p - 1, recipe.passes, recipe.jitter / 255.0)
        filters, quality = build_chain(recipe, ctx)
        dst = out / ("gen-%03d.jpg" % p)
        render_pass(prev, dst, out / ("thumb-%03d.jpg" % p), filters, quality, a.thumb)
        manifest.append("gen %03d gen-%03d.jpg %s" % (p, p, sha256_of(dst)))
        # One line per pass, flushed: this is the progress channel the PLATE
        # panel reads off the child's stdout. It cannot write to our stdin
        # (juce::ChildProcess has no such call), so the protocol is one-way
        # and line-oriented on purpose.
        print("PASS %d %d gen-%03d.jpg" % (p, recipe.passes, p), flush=True)
        prev = dst

    (out / "PLATE.manifest").write_text("\n".join(manifest) + "\n", encoding="utf-8")
    print("DONE %d" % recipe.passes, flush=True)
    print("OUT %s" % out, flush=True)


def cmd_sheet(a) -> None:
    d = Path(os.path.expanduser(a.dir))
    gens = sorted(d.glob("gen-*.jpg"))
    if not gens:
        sys.exit("no generations in %s" % d)
    cols = a.cols
    rows = int(math.ceil(len(gens) / float(cols)))
    out = Path(os.path.expanduser(a.out))
    out.parent.mkdir(parents=True, exist_ok=True)
    # image2 sequence input, tiled in one pass. `-start_number 0` because the
    # ladder starts at generation zero.
    run_tool([ffmpeg_bin(), "-nostdin", "-v", "error", "-y",
              "-start_number", "0", "-i", str(d / "gen-%03d.jpg"),
              "-frames:v", "1",
              "-vf", "scale=%d:-1:flags=bilinear,tile=%dx%d:padding=2:color=black"
                     % (a.cell, cols, rows),
              "-q:v", "3", str(out)])
    print(out)


def cmd_recipe(a) -> None:
    r = Recipe.default(seed=int(a.seed, 16) if a.seed else None, passes=a.passes)
    if a.out:
        p = Path(os.path.expanduser(a.out))
        r.save(p)
        print(p)
    else:
        sys.stdout.write(r.text())


def cmd_ops(a) -> None:
    print("operator   dflt on  effect")
    for name in DEFAULT_ORDER:
        builder, amount, on, desc = OPS[name]
        print(" %-9s %4d  %s  %s" % (name, amount, "*" if on else " ", desc))
    print("\n  amounts are 0-255, the range every knob in MORGUE turns through")
    print("  '*' marks the operators the default recipe enables")


def cmd_probe(a) -> None:
    src = Path(os.path.expanduser(a.src))
    w, h = probe_size(src)
    print("%dx%d  %s" % (w, h, src))


def main() -> None:
    p = argparse.ArgumentParser(prog="degrade", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    o = sub.add_parser("ops", help="list the operators")
    o.set_defaults(func=cmd_ops)

    rc = sub.add_parser("recipe", help="write a default recipe file")
    rc.add_argument("--out", help="path to write (default: stdout)")
    rc.add_argument("--seed", help="master seed, hex")
    rc.add_argument("--passes", type=int)
    rc.set_defaults(func=cmd_recipe)

    r = sub.add_parser("run", help="run the ladder; writes every generation")
    r.add_argument("--src", required=True, help="source image")
    r.add_argument("--out", required=True, help="output directory")
    r.add_argument("--recipe", help="recipe file (default: the built-in chain)")
    r.add_argument("--passes", type=int)
    r.add_argument("--seed", help="master seed, hex; overrides the recipe")
    r.add_argument("--jitter", type=int)
    r.add_argument("--thumb", type=int, default=260, help="contact-sheet thumb width")
    r.add_argument("--max-width", type=int, default=1600,
                   help="reduce the canvas to this width first (0 = never)")
    r.set_defaults(func=cmd_run)

    s = sub.add_parser("sheet", help="tile a run's generations into one image")
    s.add_argument("--dir", required=True)
    s.add_argument("--out", required=True)
    s.add_argument("--cols", type=int, default=8)
    s.add_argument("--cell", type=int, default=240)
    s.set_defaults(func=cmd_sheet)

    pr = sub.add_parser("probe", help="print an image's dimensions")
    pr.add_argument("src")
    pr.set_defaults(func=cmd_probe)

    a = p.parse_args()
    try:
        a.func(a)
    except RuntimeError as e:
        sys.exit(str(e))


if __name__ == "__main__":
    main()
