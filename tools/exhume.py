#!/usr/bin/env python3
"""
EXHUME -- archive.org acquisition for MORGUE.

Searches the Internet Archive, downloads audio, transcodes it to the engine's
canonical format (44.1k mono 16-bit WAV), and writes a provenance sidecar next
to every specimen so a future release can be credited and cleared.

This is deliberately a standalone script and not part of the JUCE app. It needs
no build, it runs today, and it is the cheapest way to find out whether this
whole pipeline is worth putting behind a panel. Run it, fill the locker, then
decide.

    python tools/exhume.py search "civil defense" --collection prelinger
    python tools/exhume.py item 0171WhatToDoInAGasAttack
    python tools/exhume.py fetch 0171WhatToDoInAGasAttack --out ~/MORGUE/ACQ

Requires: Python 3.9+, ffmpeg on PATH. No third-party packages.

--------------------------------------------------------------------------
TWO ARCHIVE.ORG FACTS THIS SCRIPT IS BUILT AROUND, both verified the hard way:

1. The scrape API lies to unauthenticated clients. For `collection:prelinger`
   it reports total=13; advancedsearch.php reports numFound=10374. The cause is
   that guests cannot access scope=all, and the failure is an HTTP 200 with no
   warning at all. So: advancedsearch.php, always.

2. /download redirects round-robin across CDN nodes, and some of those nodes
   return 500 on exactly the Range requests that audition depends on -- while
   still answering plain HEAD with 200 and a correct Content-Length. HEAD is
   therefore not a usable health probe. We resolve the storage node ourselves
   out of /metadata and fail over across candidates.
--------------------------------------------------------------------------
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from dataclasses import dataclass, field, asdict
from pathlib import Path

# --------------------------------------------------------------------------
# Politeness. archive.org is a donation-funded nonprofit and this script can
# issue a lot of requests. Identify ourselves, cap concurrency at one, leave a
# gap between calls, and honour Retry-After. Getting throttled is the good
# outcome here; getting the client string blocked for everyone is the bad one.
# --------------------------------------------------------------------------
CONTACT = os.environ.get("MORGUE_CONTACT", "unset-contact")
USER_AGENT = f"MORGUE-EXHUME/0.1 (+noise instrument; {CONTACT})"
MIN_INTERVAL = 0.5          # seconds between requests, minimum
MAX_RETRIES = 4

_last_request = 0.0


def _throttle() -> None:
    global _last_request
    gap = time.monotonic() - _last_request
    if gap < MIN_INTERVAL:
        time.sleep(MIN_INTERVAL - gap)
    _last_request = time.monotonic()


def http_get(url: str, headers: dict | None = None, binary: bool = False):
    """GET with backoff. Honours Retry-After on 429/503."""
    hdrs = {"User-Agent": USER_AGENT}
    if headers:
        hdrs.update(headers)

    for attempt in range(MAX_RETRIES):
        _throttle()
        req = urllib.request.Request(url, headers=hdrs)
        try:
            with urllib.request.urlopen(req, timeout=60) as resp:
                data = resp.read()
                return data if binary else data.decode("utf-8", "replace")
        except urllib.error.HTTPError as e:
            if e.code in (429, 503):
                wait = float(e.headers.get("Retry-After") or (2 ** attempt))
                sys.stderr.write(f"  [{e.code}] backing off {wait:.0f}s\n")
                time.sleep(wait)
                continue
            if e.code >= 500 and attempt < MAX_RETRIES - 1:
                time.sleep(2 ** attempt)
                continue
            raise
        except (urllib.error.URLError, TimeoutError):
            if attempt == MAX_RETRIES - 1:
                raise
            time.sleep(2 ** attempt)
    raise RuntimeError(f"giving up on {url}")


# --------------------------------------------------------------------------
# Licence reality. archive.org licence metadata is uploader-supplied and the
# Archive itself says you should rarely trust the declarer. These rules encode
# what is actually known about the collections that matter for this project,
# so a specimen carries an honest clearance state rather than a false CC0.
# --------------------------------------------------------------------------
CLEARANCE_RULES = {
    # collection      : (clearance, note)
    "fedflix":        ("CLEARED", "US federal works are uncopyrightable in the US"),
    "librivoxaudio":  ("CLEARED", "LibriVox volunteers dedicate recordings to the public domain"),
    "netlabels":      ("REVIEW",  "CC-licensed netlabel release; check the specific licence"),
    "prelinger":      ("REVIEW",  "Collection convention is PD but many items carry no licenseurl"),
    "prelingerhomemovies": ("REVIEW", "Murkier status than the main Prelinger collection"),
    "georgeblood":    ("PERSONAL_ONLY", "Great 78 Project: research, teaching and private study ONLY"),
    "78rpm":          ("PERSONAL_ONLY", "Great 78 Project: research, teaching and private study ONLY"),
    "audio_religion": ("REVIEW",  "Many items are modern congregational recordings, not PD"),
    "shortwave-airchecks": ("REVIEW", "Off-air recordings of third-party broadcasts"),
    "dlarc":          ("REVIEW",  "Digital Library of Amateur Radio; per-item terms vary"),
}

# Collections that sound real and are not. Guessing these wastes an afternoon.
KNOWN_BAD = {
    "usgovernmentfilms", "nationalarchives", "ConetProject", "conet_project",
    "greatest78", "shortwave", "numbers_stations", "gratefuldead",
}

AUDIO_EXT = {".mp3", ".flac", ".wav", ".ogg", ".m4a", ".aiff", ".aif", ".opus", ".wma"}
VIDEO_EXT = {".mp4", ".mpeg", ".mpg", ".avi", ".mkv", ".mov", ".ogv", ".webm"}


def clearance_for(collections: list[str]) -> tuple[str, str]:
    """Most restrictive rule wins."""
    rank = {"PERSONAL_ONLY": 0, "REVIEW": 1, "CLEARED": 2, "UNREVIEWED": 3}
    best = ("UNREVIEWED", "No rule for this collection; verify manually")
    for c in collections:
        rule = CLEARANCE_RULES.get(c.lower())
        if rule and rank[rule[0]] < rank[best[0]]:
            best = rule
    return best


# --------------------------------------------------------------------------
# Search -- advancedsearch.php, never scrape. See the module docstring.
# --------------------------------------------------------------------------
FIELDS = ["identifier", "title", "creator", "date", "mediatype",
          "collection", "licenseurl", "subject", "downloads"]


def search(query: str, collection: str | None, mediatype: str | None,
           rows: int, page: int) -> dict:
    clauses = [f"({query})"] if query else []
    if collection:
        if collection in KNOWN_BAD:
            sys.stderr.write(
                f"!! '{collection}' is not a real archive.org collection.\n"
                f"   Verify with: curl https://archive.org/metadata/{collection}\n")
        clauses.append(f"collection:({collection})")
    if mediatype:
        clauses.append(f"mediatype:({mediatype})")

    params = [("q", " AND ".join(clauses) or "*:*"),
              ("rows", str(rows)), ("page", str(page)), ("output", "json")]
    params += [("fl[]", f) for f in FIELDS]
    url = "https://archive.org/advancedsearch.php?" + urllib.parse.urlencode(params)
    return json.loads(http_get(url))


def metadata(identifier: str) -> dict:
    return json.loads(http_get(f"https://archive.org/metadata/{identifier}"))


def as_list(v) -> list[str]:
    if v is None:
        return []
    return v if isinstance(v, list) else [v]


def resolve_nodes(meta: dict) -> list[str]:
    """Storage nodes, best first. Never follow /download redirects -- they
    round-robin onto CDN nodes that 500 on Range requests."""
    seen, out = set(), []
    for c in ([meta.get("server")] + as_list(meta.get("workable_servers"))
              + [meta.get("d1"), meta.get("d2")]):
        if c and c not in seen:
            seen.add(c)
            out.append(c)
    return out


def file_url(node: str, meta: dict, name: str) -> str:
    d = meta.get("dir", "")
    # Encode each path segment but not the separators: real archive.org
    # filenames contain spaces, brackets, parentheses and embedded subdirs.
    safe = "/".join(urllib.parse.quote(p) for p in name.split("/"))
    return f"https://{node}{d}/{safe}"


# --------------------------------------------------------------------------
# Fetch + transcode
# --------------------------------------------------------------------------
def ffmpeg_bin() -> str:
    exe = os.environ.get("MORGUE_FFMPEG") or shutil.which("ffmpeg")
    if not exe:
        sys.exit("ffmpeg not found. Install it or set MORGUE_FFMPEG.")
    return exe


def pick_files(meta: dict, want_video: bool) -> list[dict]:
    """Prefer original audio; fall back to derivatives, then video."""
    files = meta.get("files", [])
    exts = AUDIO_EXT | (VIDEO_EXT if want_video else set())
    cands = [f for f in files
             if Path(f.get("name", "")).suffix.lower() in exts]
    # Originals first, then largest -- derivatives are lossy re-encodes.
    cands.sort(key=lambda f: (f.get("source") != "original",
                              -int(f.get("size") or 0)))
    return cands


def download(meta: dict, entry: dict, dest: Path) -> Path:
    nodes = resolve_nodes(meta)
    if not nodes:
        raise RuntimeError("no storage nodes in metadata")
    dest.parent.mkdir(parents=True, exist_ok=True)
    last = None
    for node in nodes:
        url = file_url(node, meta, entry["name"])
        try:
            sys.stderr.write(f"  fetch {node} ...\n")
            data = http_get(url, binary=True)
            dest.write_bytes(data)
            # archive.org publishes md5 per file -- use it. This is the
            # integrity check, the resume anchor and the dedupe key.
            want = entry.get("md5")
            if want:
                got = hashlib.md5(data).hexdigest()
                if got != want:
                    raise RuntimeError(f"md5 mismatch: {got} != {want}")
            return dest
        except Exception as e:                       # try the next node
            last = e
            sys.stderr.write(f"  !! {node}: {e}\n")
    raise RuntimeError(f"all nodes failed: {last}")


def transcode(src: Path, dst: Path) -> None:
    """44.1k mono 16-bit -- what the engine renders at."""
    dst.parent.mkdir(parents=True, exist_ok=True)
    cmd = [ffmpeg_bin(), "-nostdin", "-v", "error", "-y", "-i", str(src),
           "-vn", "-ac", "1", "-ar", "44100", "-c:a", "pcm_s16le", str(dst)]
    subprocess.run(cmd, check=True)


def sha256_of(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def write_toetag(wav: Path, meta: dict, entry: dict, doc: dict) -> None:
    """Provenance sidecar. Travels with the file so a specimen copied out of
    the locker is still self-describing."""
    md = meta.get("metadata", {})
    collections = as_list(md.get("collection"))
    clearance, note = clearance_for(collections)
    tag = {
        "serial": wav.stem,
        "sha256": sha256_of(wav),
        "acquired_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "source": {
            "plugin": "archive.org",
            "identifier": md.get("identifier"),
            "details_url": f"https://archive.org/details/{md.get('identifier')}",
            "file": entry.get("name"),
            "file_md5": entry.get("md5"),
            "collections": collections,
        },
        "credit": {
            "title": md.get("title"),
            "creator": md.get("creator"),
            "date": md.get("date"),
        },
        "licence": {
            # declared_by matters: IA licence metadata is uploader-supplied and
            # the Archive explicitly does not vouch for it.
            "declared": md.get("licenseurl"),
            "declared_by": "uploader",
            "uploader": md.get("uploader"),
            "clearance": clearance,
            "note": note,
        },
        "derived_from": None,
    }
    wav.with_suffix(wav.suffix + ".toetag.json").write_text(
        json.dumps(tag, indent=2), encoding="utf-8")


# --------------------------------------------------------------------------
# Commands
# --------------------------------------------------------------------------
def cmd_search(a) -> None:
    res = search(a.query, a.collection, a.mediatype, a.rows, a.page)
    r = res.get("response", {})
    docs = r.get("docs", [])
    print(f"{r.get('numFound', 0)} items  (page {a.page}, showing {len(docs)})\n")
    for d in docs:
        cl, _ = clearance_for(as_list(d.get("collection")))
        mark = {"CLEARED": "+", "REVIEW": "?",
                "PERSONAL_ONLY": "X", "UNREVIEWED": " "}[cl]
        title = (d.get("title") or "")[:58]
        print(f" {mark} {d['identifier'][:38]:38} {title}")
        creator = d.get("creator")
        if creator:
            print(f"     {str(creator)[:70]}  {d.get('date','')[:10]}")
    print("\n  + clear   ? verify per item   X personal use only")


def cmd_item(a) -> None:
    meta = metadata(a.identifier)
    if not meta:
        sys.exit(f"'{a.identifier}' does not exist on archive.org")
    md = meta.get("metadata", {})
    cl, note = clearance_for(as_list(md.get("collection")))
    print(f"{md.get('title')}\n  by {md.get('creator')}  ({md.get('date','')[:10]})")
    print(f"  collections : {', '.join(as_list(md.get('collection')))}")
    print(f"  licence     : {md.get('licenseurl') or '(none declared)'}")
    print(f"  clearance   : {cl} -- {note}")
    print(f"  nodes       : {', '.join(resolve_nodes(meta)[:3])}\n")
    for f in pick_files(meta, want_video=True):
        size = int(f.get("size") or 0) / 1e6
        print(f"  {f.get('source','?'):8} {size:8.1f} MB  {f['name']}")


def cmd_fetch(a) -> None:
    meta = metadata(a.identifier)
    if not meta:
        sys.exit(f"'{a.identifier}' does not exist on archive.org")
    md = meta.get("metadata", {})
    cl, note = clearance_for(as_list(md.get("collection")))
    if cl == "PERSONAL_ONLY":
        sys.stderr.write(f"!! {note}\n!! Fine to study; do NOT put it in a release.\n")

    cands = pick_files(meta, want_video=a.video)
    if not cands:
        sys.exit("no audio (or video) files in this item")
    entry = cands[0] if not a.file else next(
        (f for f in cands if f["name"] == a.file), None)
    if entry is None:
        sys.exit(f"no such file: {a.file}")

    out = Path(os.path.expanduser(a.out)) / a.identifier
    raw = out / "source" / Path(entry["name"]).name
    if raw.exists() and not a.force:
        sys.stderr.write(f"  have {raw.name}, skipping download\n")
    else:
        download(meta, entry, raw)

    wav = out / (re.sub(r"[^A-Za-z0-9._-]", "_", Path(entry["name"]).stem) + ".wav")
    sys.stderr.write("  transcode -> 44.1k mono\n")
    transcode(raw, wav)
    write_toetag(wav, meta, entry, md)
    print(f"\n{wav}\n{wav}.toetag.json\nclearance: {cl}")


def main() -> None:
    p = argparse.ArgumentParser(prog="exhume", description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser("search", help="search archive.org")
    s.add_argument("query", nargs="?", default="")
    s.add_argument("--collection", "-c")
    s.add_argument("--mediatype", "-m", choices=["audio", "movies", "texts"])
    s.add_argument("--rows", type=int, default=25)
    s.add_argument("--page", type=int, default=1)
    s.set_defaults(func=cmd_search)

    i = sub.add_parser("item", help="inspect one item's files and licence")
    i.add_argument("identifier")
    i.set_defaults(func=cmd_item)

    f = sub.add_parser("fetch", help="download + transcode into the locker")
    f.add_argument("identifier")
    f.add_argument("--out", default="~/MORGUE/ACQ")
    f.add_argument("--file", help="specific file name from `item`")
    f.add_argument("--video", action="store_true",
                   help="allow pulling audio out of a video file")
    f.add_argument("--force", action="store_true", help="re-download")
    f.set_defaults(func=cmd_fetch)

    a = p.parse_args()
    if CONTACT == "unset-contact":
        sys.stderr.write(
            "note: set MORGUE_CONTACT to an email so archive.org can reach you\n"
            "      if this script ever misbehaves. It is the polite thing.\n\n")
    a.func(a)


if __name__ == "__main__":
    main()
