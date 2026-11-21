#!/usr/bin/env python3
"""Build meter_labels.json from the dance tags on dancebrainz.balfolk.social.

  make_meter_labels.py --music-root ~/Musik [--lookup-cache dbz.json] [-o meter_labels.json]

Reads the MusicBrainz recording id (MUSICBRAINZ_TRACKID) of every audio file
under the music root with ffprobe, looks the recordings up on dancebrainz
(POST /api/v1/recordings/lookup, 500 per request) and labels each track that
has exactly one dance tag:

- the dance has a known meter (DANCES below): that meter. A dancebrainz meter
  tag must agree (same number of beats per bar, or the dance cycle, e.g. 8/4
  on a laridé 8t); a track whose meter tag disagrees is left out and listed;
- otherwise a meter tag alone (e.g. 3/4 on a dance not in the table) labels it.

Writes labels only (MusicBrainz id and file path relative to the music root),
never audio. Standard library and ffprobe only. --lookup-cache reuses (and
updates) a JSON map of recording id -> lookup entry; --probe-cache does the
same for the file -> recording id map, so a rerun only probes new files.
"""

import argparse
import json
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

HERE = Path(__file__).resolve().parent
API = "https://dancebrainz.balfolk.social/api/v1/recordings/lookup"
UUID = re.compile(r"[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")
AUDIO = {".opus", ".ogg", ".mp3", ".flac", ".m4a", ".wav"}

# Dance tag -> (meter, type, extra). type: binary, ternary, six (6/4 bars) or
# odd (uneven bars of 5, 7, 8 as 3+3+2, 11 ...). cycle: dance steps per
# figure where it spans several bars. status: confirmed by someone who dances
# them, or assumed (commonly agreed, not checked on this library).
DANCES = {
    "Bourree 2t": ("2/4", "binary", {"status": "confirmed", "notes": "tags checked by ear where tools disagreed"}),
    "Bourree 3t": ("3/4", "ternary", {"status": "confirmed", "notes": "3/8 feel; tags checked by ear where tools disagreed"}),
    "Rondeau en couple": ("2/4", "binary", {"status": "confirmed"}),
    "Rondeau en chaine": ("2/4", "binary", {"status": "confirmed"}),
    "Rond de St. Vincent": ("2/4", "binary", {"status": "confirmed"}),
    "Pile menu": ("2/4", "binary", {"status": "confirmed"}),
    "an dro": ("4/4", "binary", {"cycle": 8, "status": "confirmed"}),
    "Laride 8t": ("4/4", "binary", {"cycle": 8, "status": "confirmed"}),
    "Kas a barh": ("4/4", "binary", {"cycle": 8, "status": "confirmed"}),
    "Tour": ("4/4", "binary", {"cycle": 8, "status": "confirmed"}),
    "Kost ar c'hoad": ("4/4", "binary", {"status": "confirmed"}),
    "Gavotte de l'Aven caresse": ("4/4", "binary", {"status": "confirmed"}),
    "ton simple": ("4/4", "binary", {"status": "confirmed"}),
    "ton double": ("4/4", "binary", {"status": "confirmed"}),
    "Ridée 6t": ("6/4", "six", {"grouping": [4, 2], "status": "confirmed"}),
    "hanter dro": ("6/4", "six", {"grouping": [4, 2], "status": "confirmed"}),
    "Scottish": ("2/4", "binary", {"status": "assumed", "notes": "phrase structure still to discuss"}),
    "Polka": ("2/4", "binary", {"status": "assumed"}),
    "Chapeloise": ("2/4", "binary", {"status": "assumed"}),
    "Valse": ("3/4", "ternary", {"status": "confirmed"}),
    "Mazurka": ("3/4", "ternary", {"status": "assumed"}),
    "Valse 5t": ("5/4", "odd", {"grouping": [3, 2], "status": "assumed"}),
    "Valse 8t": ("8/4", "odd", {"grouping": [3, 3, 2], "status": "assumed"}),
    "Valse 11t": ("11/4", "odd", {"grouping": [3, 3, 2, 3], "status": "assumed"}),
}
# Too complex for a single meter per track: never labelled, even with a meter tag.
EXCLUDED = {"Zwiefacher", "Branle", "Bourree", "Polska"}


def meter_type(beats):
    if beats in (2, 4):
        return "binary"
    if beats == 3:
        return "ternary"
    if beats == 6:
        return "six"
    return "odd"


def probe(path):
    p = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format_tags:stream_tags", "-of", "json",
                        str(path)], capture_output=True, text=True, errors="replace")
    try:
        j = json.loads(p.stdout)
    except ValueError:
        return None
    for tags in [j.get("format", {}).get("tags", {})] + [s.get("tags", {}) for s in j.get("streams", [])]:
        for k, v in tags.items():
            if k.upper() == "MUSICBRAINZ_TRACKID":
                # Some taggers write the id twice ("id;id"); several different ids: unusable.
                ids = set(UUID.findall(v.lower()))
                return ids.pop() if len(ids) == 1 else None
    return None


def lookup(mbids):
    body = json.dumps({"mbids": mbids}).encode()
    for _ in range(8):
        req = urllib.request.Request(API, data=body, headers={"Content-Type": "application/json",
                                                              "User-Agent": "folksentia-eval/1"})
        try:
            with urllib.request.urlopen(req, timeout=120) as r:
                return json.load(r)["recordings"]
        except urllib.error.HTTPError as e:
            if e.code in (429, 502, 503):
                time.sleep(int(e.headers.get("Retry-After") or 3) + 1)
                continue
            raise
    raise SystemExit("dancebrainz lookup keeps failing")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--music-root", required=True, type=Path)
    ap.add_argument("--lookup-cache", type=Path)
    ap.add_argument("--probe-cache", type=Path)
    ap.add_argument("--refresh", action="store_true", help="look up again even when cached")
    ap.add_argument("-o", "--output", type=Path, default=HERE / "meter_labels.json")
    ap.add_argument("-j", type=int, default=8)
    a = ap.parse_args()

    files = sorted(p for p in a.music_root.rglob("*") if p.suffix.lower() in AUDIO and p.is_file())
    def encodable(p):
        try:
            p.as_posix().encode("utf-8")
            return True
        except UnicodeEncodeError:  # file name not in UTF-8: cannot be a JSON key
            print(f"skipped, file name not UTF-8: {p.as_posix().encode('utf-8', 'replace').decode()}",
                  file=sys.stderr)
            return False
    files = [p for p in files if encodable(p)]
    rel = [p.relative_to(a.music_root).as_posix() for p in files]
    probed = json.loads(a.probe_cache.read_text()) if a.probe_cache and a.probe_cache.is_file() else {}
    todo = [(r, p) for r, p in zip(rel, files) if r not in probed]
    with ThreadPoolExecutor(a.j) as pool:
        for r, m in zip([r for r, _ in todo], pool.map(lambda rp: probe(rp[1]), todo)):
            probed[r] = m
    if a.probe_cache:
        a.probe_cache.write_text(json.dumps(probed, indent=0, ensure_ascii=False))
    mbid = {r: probed[r] for r in rel if probed.get(r)}
    print(f"{len(files)} files, {len(mbid)} with a recording id", file=sys.stderr)

    cache = json.loads(a.lookup_cache.read_text()) if a.lookup_cache and a.lookup_cache.is_file() else {}
    want = sorted(set(mbid.values()) if a.refresh else set(mbid.values()) - set(cache))
    for i in range(0, len(want), 500):
        for e in lookup(want[i:i + 500]):
            cache[e["mbid"]] = e
    if a.lookup_cache:
        a.lookup_cache.write_text(json.dumps(cache, indent=1, ensure_ascii=False))

    tunes, conflicts = [], []
    for r, m in sorted(mbid.items()):
        tags = (cache.get(m) or {}).get("tags") or []
        dances = sorted({t["name"] for t in tags if t.get("type") == "dance"})
        meters = sorted({t["name"] for t in tags if t.get("type") == "meter"})
        if len(dances) != 1 or dances[0] in EXCLUDED or len(meters) > 1:
            continue
        dance, tag = dances[0], (meters[0] if meters else None)
        if dance in DANCES:
            meter, kind, extra = DANCES[dance]
            if tag:
                beats = int(tag.split("/")[0])
                if beats not in (int(meter.split("/")[0]), extra.get("cycle")):
                    conflicts.append((r, dance, tag))
                    continue
            source = "dance + meter tag" if tag else "dance"
        elif tag:
            meter, kind, extra, source = tag, meter_type(int(tag.split("/")[0])), {"status": "meter tag"}, "meter tag"
        else:
            continue
        tune = {"file": r, "mbid": m, "dance": dance, "meter": meter, "type": kind, "source": source}
        tune.update({k: v for k, v in extra.items() if k != "notes"})
        tunes.append(tune)

    out = {
        "about": "Meter labels for folk dance tracks, from the dance and meter tags on "
                 "dancebrainz.balfolk.social (generated by make_meter_labels.py). Labels only, no audio: point "
                 "score_meter.py at a local copy. file is relative to the music root; mbid is the MusicBrainz "
                 "recording id. meter: the bar as it should be written; type: binary, ternary, six (6/4) or "
                 "odd (uneven bars such as 5 = 3+2, 8 = 3+3+2, 11 = 3+3+2+3); cycle: dance steps per figure where "
                 "it spans several bars; grouping: beats of an uneven bar. status: confirmed (by someone who "
                 "dances them), assumed (commonly agreed, not checked here) or meter tag (only a dancebrainz meter "
                 "tag). Tags are not perfect: list_disagreements in score_meter.py shows tracks to check by ear.",
        "dances": {d: {"meter": m, "type": k, **x} for d, (m, k, x) in DANCES.items()},
        "tunes": tunes,
    }
    a.output.write_text(json.dumps(out, indent=1, ensure_ascii=False) + "\n")
    print(f"{len(tunes)} labelled tracks -> {a.output}", file=sys.stderr)
    for r, d, t in conflicts:
        print(f"left out, meter tag {t} disagrees with {d}: {r}", file=sys.stderr)


if __name__ == "__main__":
    main()
